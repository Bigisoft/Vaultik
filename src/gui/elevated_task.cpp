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

// How long the engine has to connect after it started (after the approval prompt).
constexpr auto kConnectTime = std::chrono::seconds(20);

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

// "33 h 06 min left", "12 min left", "less than a minute left"; empty when not known.
QString time_left_text(double seconds) {
    const TimeLeft t = split_time_left(seconds);
    if (!t.known) return {};
    if (t.under_a_minute) return QObject::tr("less than a minute left");
    if (t.hours == 0) return QObject::tr("%1 min left").arg(t.minutes);
    return QObject::tr("%1 h %2 min left").arg(t.hours).arg(t.minutes, 2, 10, QChar('0'));
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
        if (running_) reject();
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
    std::unique_ptr<os::ElevatedProcess> process;
    try {
        process = os::start_command_elevated(argv, server_ == nullptr);
    } catch (const std::exception& e) {
        const QString error = QString::fromUtf8(e.what());
        QMetaObject::invokeMethod(this, [this, error] { on_finished(-1, error); }, Qt::QueuedConnection);
        return;
    }
    std::optional<int> code;
    if (server_ != nullptr) {
        QMetaObject::invokeMethod(this, [this] { phase_->setText(tr("Starting...")); }, Qt::QueuedConnection);
        const auto deadline = std::chrono::steady_clock::now() + kConnectTime;
        bool connected = false;
        while (!abort_ && !(connected = server_->wait_connected(200))) {
            if ((code = process->wait(0))) break;
            if (std::chrono::steady_clock::now() > deadline) break;
        }
        if (!connected && !abort_) connected = server_->wait_connected(0);
        if (connected) {
            QMetaObject::invokeMethod(this, [this] { on_connected(); }, Qt::QueuedConnection);
            bool cancel_sent = false;
            std::string line;
            while (!abort_) {
                if (cancel_ && !cancel_sent) cancel_sent = server_->send_line("cancel");
                const os::ProgressServer::Read got = server_->read_line(line, 200);
                if (got == os::ProgressServer::Read::Line) {
                    post_line(line);
                } else if (got == os::ProgressServer::Read::Closed) {
                    break;
                } else if (!code && (code = process->wait(0))) {
                    // It ended: what it sent last is read, then no more is waited for.
                    while (server_->read_line(line, 300) == os::ProgressServer::Read::Line) post_line(line);
                    break;
                }
            }
        } else if (!code && !abort_) {
            QMetaObject::invokeMethod(this, [this] { on_in_window(); }, Qt::QueuedConnection);
        }
    }
    while (!abort_ && !code) code = process->wait(200);
    if (code) {
        const int c = *code;
        QMetaObject::invokeMethod(this, [this, c] { on_finished(c, {}); }, Qt::QueuedConnection);
    }
}

void ElevatedTaskDialog::on_connected() {
    connected_ = true;
    phase_->setText(QString());
    if (task_.cancel_question.isEmpty()) button_->setEnabled(false);
}

void ElevatedTaskDialog::on_in_window() {
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

void ElevatedTaskDialog::on_finished(int code, const QString& error) {
    if (!running_) return;
    running_ = false;
    --g_running;
    result_.code = code;
    result_.error = error;
#ifndef _WIN32
    // pkexec: 126 when the password was not given, 127 when it is not allowed.
    if (error.isEmpty() && !connected_ && (code == 126 || code == 127)) {
        result_.code = -1;
        result_.error = tr("root rights were not given (pkexec ended with code %1)").arg(code);
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
    if (!running_) {
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
    // exec() also ends when Vaultik quits (after asking): the task goes on without it.
    if (r.code == -1 && r.error.isEmpty()) r.left_running = true;
    return r;
}
