#include "elevated_task.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

#include <chrono>

#include "common.h"
#include "runner.h"
#include "vaultik/platform/os.hpp"
#include "vaultik/platform/progress_channel.hpp"

using namespace vaultik;

namespace {

constexpr int kExitCancelled = 4;
constexpr int kExitInUse = 6;  // the engine's exit code when a drive is in use
#ifndef _WIN32
constexpr int kExitNoChannel = 7;  // it could not use the progress channel and did nothing
#endif

// Windows: how long the engine may take to connect after it started (after the approval
// prompt) before the window says the progress cannot be shown here; an engine that connects
// later is still taken. POSIX: no limit (pkexec returns before its password prompt was
// answered, which may take any time).
constexpr int kConnectMs = 20000;

std::atomic<int> g_running{0};

double now_seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

QLabel* text_label() {
    auto* l = new QLabel;
    l->setWordWrap(true);
    l->setTextFormat(Qt::PlainText);
    return l;
}


}  // namespace

bool elevated_tasks_possible() {
#ifdef _WIN32
    return true;
#else
    return !os::pkexec_path().empty();
#endif
}

int elevated_tasks_running() {
    return g_running.load();
}

ElevatedTaskDialog::ElevatedTaskDialog(const ElevatedTask& task, const QStringList& args, QWidget* parent)
    : QDialog(parent), task_(task), args_(args) {
    setWindowTitle(task.title);
    setMinimumWidth(520);
    auto* layout = new QVBoxLayout(this);
    heading_ = text_label();
    QFont bold = heading_->font();
    bold.setBold(true);
    heading_->setFont(bold);
    heading_->setText(task.working);
    layout->addWidget(heading_);
    phase_ = text_label();
    layout->addWidget(phase_);
    bar_ = new QProgressBar;
    bar_->setRange(0, 0);  // busy until it tells how far it is
    bar_->setTextVisible(false);
    layout->addWidget(bar_);
    details_ = text_label();
    layout->addWidget(details_);
    note_ = text_label();
    note_->setForegroundRole(QPalette::PlaceholderText);
    note_->hide();
    layout->addWidget(note_);
    layout->addStretch();
    auto* buttons = new QDialogButtonBox;
    button_ = buttons->addButton(tr("Cancel"), QDialogButtonBox::RejectRole);
    connect(button_, &QPushButton::clicked, this, [this] {
        if (running_ && !may_close_) reject();
        else accept();
    });
    layout->addWidget(buttons);
    phase_->setText(tr("Waiting for the approval to run with administrator rights..."));
}

ElevatedTaskDialog::~ElevatedTaskDialog() {
    // Vaultik is closing while the task runs: it goes on without the window (the channel closes
    // with it, which does not stop the task).
    abort_ = true;
    if (worker_.joinable()) worker_.join();
    if (running_) --g_running;
}

void ElevatedTaskDialog::start() {
    running_ = true;
    ++g_running;
    started_ = now_seconds();
    std::vector<std::string> argv{u8(QDir::toNativeSeparators(Runner::engine_path()))};
    for (const QString& a : args_) argv.push_back(u8(a));
    try {
        server_ = std::make_unique<os::ProgressServer>();
        argv.insert(argv.end(), {"--progress-pipe", server_->name(), "--progress-user", server_->user()});
    } catch (const std::exception& e) {
        // Without a channel: as before, in a window of its own that stays open at the end.
        server_.reset();
        result_.in_window = true;
#ifdef _WIN32
        argv.push_back("--pause");
#endif
        note_->setText(tr("The progress cannot be shown here (%1).").arg(QString::fromUtf8(e.what())));
        note_->show();
    }
    worker_ = std::thread([this, argv] { work(argv); });
}

void ElevatedTaskDialog::post_line(const std::string& line) {
    QMetaObject::invokeMethod(this, [this, l = QString::fromStdString(line)] { on_line(l); }, Qt::QueuedConnection);
}

// On a thread of its own: starts the engine (the approval prompt waits here), reads its
// events and passes them on, sends "cancel" when asked, and waits for it to end.
void ElevatedTaskDialog::work(std::vector<std::string> argv) {
    const auto finish = [this](int code, const QString& error, bool started) {
        QMetaObject::invokeMethod(this, [this, code, error, started] { on_finished(code, error, started); }, Qt::QueuedConnection);
    };
    std::unique_ptr<os::ElevatedProcess> process;
    try {
        process = os::start_command_elevated(argv, server_ == nullptr);
    } catch (const std::exception& e) {
        finish(-1, QString::fromUtf8(e.what()), false);
        return;
    }
    try {
        const std::optional<int> code = follow(*process);
        if (code) finish(*code, {}, true);
    } catch (const std::exception& e) {
        // How it ended cannot be read: never taken for success.
        finish(-1, QString::fromUtf8(e.what()), true);
    }
}

// Follows the engine until it ended (its exit code), or the window went away (nullopt).
std::optional<int> ElevatedTaskDialog::follow(os::ElevatedProcess& process) {
    std::optional<int> code;
    if (server_ != nullptr) {
        // Only the engine started here may connect (none, when its process id is not known).
        // A client whose process id cannot be read is taken only once the engine ended: it was
        // the engine, which closed its end at once.
        server_->expect_client(process.pid(), [&process] {
            try {
                return process.wait(1000).has_value();
            } catch (const std::exception&) {
                return false;  // seen again by the wait below
            }
        });
#ifdef _WIN32
        QMetaObject::invokeMethod(this, [this] { phase_->setText(tr("Starting...")); }, Qt::QueuedConnection);
        const int slow_ms = kConnectMs;
#else
        const int slow_ms = -1;  // the password prompt may be answered at any time
#endif
        const os::EngineWait w = os::wait_for_engine(
            *server_, [&] { return process.wait(0); }, [this] { return abort_.load(); }, slow_ms,
            [this] { QMetaObject::invokeMethod(this, [this] { on_in_window(); }, Qt::QueuedConnection); });
        code = w.code;
        if (w.connected) {
            QMetaObject::invokeMethod(this, [this] { on_connected(); }, Qt::QueuedConnection);
            bool cancel_tried = false;
            std::string line;
            while (!abort_) {
                if (cancel_ && !cancel_tried) {
                    cancel_tried = true;
                    if (!server_->send_line("cancel"))
                        QMetaObject::invokeMethod(this, [this] { on_channel_lost(); }, Qt::QueuedConnection);
                }
                const os::ProgressServer::Read got = server_->read_line(line, 200);
                if (got == os::ProgressServer::Read::Line) {
                    post_line(line);
                } else if (got == os::ProgressServer::Read::Closed) {
                    // Its end closed: it ends (then it is gone within moments), or the channel
                    // broke while it goes on, and a Cancel can no longer reach it.
                    if (!code) code = process.wait(3000);
                    if (!code) QMetaObject::invokeMethod(this, [this] { on_channel_lost(); }, Qt::QueuedConnection);
                    break;
                } else if (!code && (code = process.wait(0))) {
                    // It ended: what it sent last is read, then no more is waited for.
                    while (server_->read_line(line, 300) == os::ProgressServer::Read::Line) post_line(line);
                    break;
                }
            }
        }
    }
    // `process` does not outlive this call.
    if (server_ != nullptr) server_->expect_client(process.pid());
    while (!abort_ && !code) code = process.wait(200);
    return code;
}

void ElevatedTaskDialog::on_connected() {
    if (!running_) return;
    connected_ = true;
    phase_->setText(QString());
    if (result_.in_window) {
        // It connected late, after the window said its progress could not be shown here.
        result_.in_window = false;
        note_->clear();
        note_->hide();
    }
    button_->setEnabled(!task_.cancel_question.isEmpty());
}

void ElevatedTaskDialog::on_in_window() {
    if (!running_ || connected_) return;
    result_.in_window = true;
    phase_->setText(QString());
#ifdef _WIN32
    note_->setText(tr("The progress cannot be shown here: it shows in a window of its own, where Ctrl+C stops it."));
#else
    note_->setText(tr("The progress cannot be shown here. This window says when it has finished."));
#endif
    note_->show();
    button_->setEnabled(false);
}

void ElevatedTaskDialog::on_channel_lost() {
    if (!running_ || lost_) return;
    lost_ = true;
    connected_ = false;
    may_close_ = true;
    if (cancelling_) phase_->clear();
    note_->setText(cancelling_ ? tr("Vaultik can no longer reach the task, so it cannot be stopped from here: it "
                                    "finishes on its own. You can close this window; the task goes on.")
                               : tr("Vaultik can no longer reach the task: its progress cannot be shown and it cannot "
                                    "be stopped from here; it finishes on its own. You can close this window; the "
                                    "task goes on."));
    note_->show();
    button_->setText(tr("Close"));
    button_->setEnabled(true);
}

void ElevatedTaskDialog::on_line(const QString& line) {
    const TaskEvent e = parse_task_event(u8(line));
    switch (e.type) {
        case TaskEvent::Type::Progress: show_progress(e); break;
        case TaskEvent::Type::Step: phase_->setText(qs(e.text)); break;
        case TaskEvent::Type::Warning:
            note_->setText(note_->text().isEmpty() ? qs(e.text) : note_->text() + "\n" + qs(e.text));
            note_->show();
            break;
        case TaskEvent::Type::InUse:
            for (const std::string& v : e.volumes) result_.in_use << qs(v);
            break;
        case TaskEvent::Type::Done:
            result_.status = qs(e.status);
            result_.message = qs(e.text);
            if (e.status == "in_use" && result_.in_use.isEmpty())
                for (const std::string& v : e.volumes) result_.in_use << qs(v);
            break;
        case TaskEvent::Type::Unknown: break;
    }
}

void ElevatedTaskDialog::show_progress(const TaskEvent& e) {
    if (cancelling_) return;  // "Stopping..." stays
    if (e.verifying) phase_->setText(tr("Checking what was written"));
    else if (e.passes > 1) phase_->setText(tr("Pass %1 of %2").arg(e.pass).arg(e.passes));
    else if (e.passes == 1) phase_->setText(tr("Writing"));
    else phase_->setText(qs(e.text));
    if (!e.has_total) {
        bar_->setRange(0, 0);
        details_->clear();
        return;
    }
    if (e.total != last_total_ || e.pass != last_pass_ || e.verifying != last_verifying_) rate_ = RateEstimator();
    last_total_ = e.total;
    last_pass_ = e.pass;
    last_verifying_ = e.verifying;
    rate_.add(e.done, e.total, now_seconds() - started_);
    const int permille = static_cast<int>(static_cast<double>(e.done) * 1000.0 / static_cast<double>(e.total));
    bar_->setRange(0, 1000);
    bar_->setValue(permille);
    const double speed = e.speed > 0 ? e.speed : rate_.speed();
    const double left = e.seconds_left >= 0 ? e.seconds_left : rate_.seconds_left();
    QStringList parts{tr("%1%").arg(permille / 10), tr("%1 of %2").arg(format_bytes(e.done), format_bytes(e.total))};
    if (speed > 0) parts << tr("%1/s").arg(format_bytes(static_cast<std::uint64_t>(speed)));
    const QString time = time_left_text(left);
    if (!time.isEmpty()) parts << time;
    details_->setText(parts.join(QStringLiteral(", ")));
}

void ElevatedTaskDialog::on_finished(int code, const QString& error, bool started) {
    if (!running_) return;
    running_ = false;
    --g_running;
    result_.ended = true;
    result_.code = code;
    if (started && !error.isEmpty()) {
        // It ran, but how it ended cannot be told: a failure, never success.
        result_.status.clear();
        result_.message = tr("Vaultik cannot tell how it ended (%1).").arg(error);
    } else {
        result_.error = error;
    }
#ifndef _WIN32
    // pkexec: 126 when the password was not given, 127 when it is not allowed.
    if (error.isEmpty() && !connected_ && (code == 126 || code == 127)) {
        result_.code = -1;
        result_.error = tr("root rights were not given (pkexec ended with code %1)").arg(code);
    }
    if (error.isEmpty() && !connected_ && code == kExitNoChannel) {
        result_.code = -1;
        result_.error = tr("the task with root rights could not reach this window, so it stopped before changing "
                           "anything");
    }
#endif
    if (result_.code == kExitInUse || result_.status == QLatin1String("in_use")) {
        // The caller asks whether to force the drives closed.
        accept();
        return;
    }
    button_->setText(tr("Close"));
    button_->setEnabled(true);
    button_->setDefault(true);
    phase_->clear();
    if (lost_) {
        // "You can close this window; the task goes on" no longer holds: it ended.
        note_->clear();
        note_->hide();
    }
    if (!result_.error.isEmpty()) {
        bar_->setRange(0, 1);
        bar_->setValue(0);
        heading_->setText(tr("It did not start."));
        details_->setText(result_.error);
        return;
    }
    bar_->setRange(0, 1000);
    if (result_.code == 0) {
        bar_->setValue(1000);
        heading_->setText(task_.done);
        details_->setText(result_.message);
        return;
    }
    bar_->setValue(0);
    if (result_.code == kExitCancelled || result_.status == QLatin1String("cancelled")) {
        heading_->setText(tr("Stopped before the end."));
        details_->setText(result_.message);
        return;
    }
    heading_->setText(tr("It did not finish."));
    if (!result_.message.isEmpty()) details_->setText(result_.message);
    else if (result_.in_window) details_->setText(tr("Its window showed why."));
    else details_->setText(tr("The engine ended with code %1.").arg(result_.code));
}

void ElevatedTaskDialog::reject() {
    if (!running_ || may_close_) {
        // Closed while it runs (it can no longer be reached): it goes on.
        QDialog::reject();
        return;
    }
    if (cancelling_) return;
    if (result_.in_window || task_.cancel_question.isEmpty() || !connected_) {
#ifdef _WIN32
        const bool own_window = result_.in_window;
#else
        const bool own_window = false;
#endif
        QMessageBox::information(this, windowTitle(),
                                 own_window ? tr("Stop it in its own window (Ctrl+C there).")
                                            : tr("This cannot be stopped from here; it ends by itself."));
        return;
    }
    QMessageBox box(QMessageBox::Question, windowTitle(), task_.cancel_question, QMessageBox::NoButton, this);
    box.setTextFormat(Qt::PlainText);
    QPushButton* stop = box.addButton(tr("Stop"), QMessageBox::DestructiveRole);
    QPushButton* go_on = box.addButton(tr("Go on"), QMessageBox::RejectRole);
    box.setDefaultButton(go_on);
    box.exec();
    if (box.clickedButton() != stop || !running_) return;
    if (lost_) return;  // it said so meanwhile
    cancelling_ = true;
    cancel_ = true;
    button_->setEnabled(false);
    phase_->setText(tr("Stopping..."));
}

ElevatedResult run_elevated_task(QWidget* parent, const ElevatedTask& task, const QStringList& args) {
    ElevatedTaskDialog dialog(task, args, parent);
    dialog.start();
    dialog.exec();
    ElevatedResult r = dialog.result_of();
    // exec() also ends when Vaultik quits (after asking), or when the window was closed after
    // the task could no longer be reached: the task goes on without it. Not for an engine that
    // ended, whatever its exit code.
    if (!r.ended) r.left_running = true;
    return r;
}
