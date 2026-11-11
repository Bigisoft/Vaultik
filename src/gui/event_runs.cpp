#include "event_runs.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>
#include <QProcess>
#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <atomic>
#include <functional>
#include <thread>
#include <vector>

#include "common.h"
#include "runner.h"
#include "vaultik/jobs/admin_jobs.hpp"
#include "vaultik/ops/backup.hpp"
#include "vaultik/platform/os.hpp"
#include "vaultik/repo/cloud.hpp"

#ifdef Q_OS_WIN
#include <windows.h>
#endif

using namespace vaultik;

// Watches one folder and everything below it, calling `on_change` (on the watching thread) when a
// file or folder in it that counts was added, removed, renamed or written. Windows only; elsewhere
// it never calls.
class FolderWatch {
public:
    FolderWatch(std::string root, std::vector<std::string> ignored, std::vector<std::string> excludes,
                std::function<void()> on_change)
        : root_(std::move(root)), ignored_(std::move(ignored)), excludes_(std::move(excludes)),
          on_change_(std::move(on_change)) {
        for (std::string& i : ignored_) i = os::fold_case(i);
#ifdef Q_OS_WIN
        stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (stop_ != nullptr) thread_ = std::thread([this] { watch(); });
#endif
    }

    ~FolderWatch() {
#ifdef Q_OS_WIN
        if (stop_ != nullptr) {
            SetEvent(stop_);
            if (thread_.joinable()) thread_.join();
            CloseHandle(stop_);
        }
#endif
    }

    FolderWatch(const FolderWatch&) = delete;
    FolderWatch& operator=(const FolderWatch&) = delete;

private:
    // Whether a change at `path` counts: not in a folder the backup leaves out, nor in the
    // backup's own folder or Vaultik's settings (which every run writes to).
    bool counts(const std::string& path) const {
        const std::string folded = os::fold_case(path);
        for (const std::string& i : ignored_) {
            if (folded.size() >= i.size() && folded.compare(0, i.size(), i) == 0 &&
                (folded.size() == i.size() || folded[i.size()] == '\\' || i.ends_with('\\')))
                return false;
        }
        return !path_excluded(excludes_, path);
    }

#ifdef Q_OS_WIN
    void watch() {
        HANDLE dir = CreateFileW(reinterpret_cast<const wchar_t*>(QString::fromStdString(root_).utf16()), FILE_LIST_DIRECTORY,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                 FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        if (dir == INVALID_HANDLE_VALUE) return;  // not there now: nothing to watch
        HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (done == nullptr) {
            CloseHandle(dir);
            return;
        }
        std::vector<DWORD> storage(16 * 1024);  // DWORD-aligned, as the records must be
        auto* buffer = reinterpret_cast<std::uint8_t*>(storage.data());
        const DWORD size = static_cast<DWORD>(storage.size() * sizeof(DWORD));
        const DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE |
                             FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION;
        std::string base = root_;
        if (!base.ends_with('\\')) base += '\\';
        for (;;) {
            OVERLAPPED ov{};
            ov.hEvent = done;
            ResetEvent(done);
            if (!ReadDirectoryChangesW(dir, buffer, size, TRUE, filter, nullptr, &ov, nullptr)) break;
            const HANDLE wait[] = {done, stop_};
            const DWORD which = WaitForMultipleObjects(2, wait, FALSE, INFINITE);
            if (which != WAIT_OBJECT_0) {
                CancelIoEx(dir, &ov);
                DWORD ignored = 0;
                GetOverlappedResult(dir, &ov, &ignored, TRUE);
                break;
            }
            DWORD got = 0;
            if (!GetOverlappedResult(dir, &ov, &got, FALSE)) break;
            bool relevant = got == 0;  // too many changes to list: count them
            for (std::size_t offset = 0; !relevant && got != 0 && offset + sizeof(FILE_NOTIFY_INFORMATION) <= got;) {
                const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buffer + offset);
                const QString name = QString::fromWCharArray(info->FileName, static_cast<qsizetype>(info->FileNameLength / sizeof(wchar_t)));
                if (counts(base + name.toStdString())) relevant = true;
                if (info->NextEntryOffset == 0) break;
                offset += info->NextEntryOffset;
            }
            if (relevant) on_change_();
        }
        CloseHandle(done);
        CloseHandle(dir);
    }

    HANDLE stop_ = nullptr;
    std::thread thread_;
#endif

    std::string root_;
    std::vector<std::string> ignored_;
    std::vector<std::string> excludes_;
    std::function<void()> on_change_;
};

EventRuns::EventRuns(QWidget* window, QObject* parent) : QObject(parent), window_(window) {
    poll_ = new QTimer(this);
    poll_->setInterval(3000);
    connect(poll_, &QTimer::timeout, this, &EventRuns::poll);
    QCoreApplication::instance()->installNativeEventFilter(this);
}

EventRuns::~EventRuns() {
    if (QCoreApplication::instance() != nullptr) QCoreApplication::instance()->removeNativeEventFilter(this);
    watched_.clear();  // stops the watching threads before the members they call go
}

void EventRuns::set_jobs(const std::vector<jobs::Job>& list) {
    std::map<std::string, Watched> keep;
    for (const jobs::Job& job : list) {
        if (!job.continuous || job.run_as_admin) continue;
        auto it = watched_.find(job.id);
        if (it != watched_.end() && it->second.job.sources == job.sources && it->second.job.excludes == job.excludes &&
            it->second.job.repository == job.repository) {
            it->second.job = job;
            keep.emplace(job.id, std::move(it->second));
            watched_.erase(it);
            continue;
        }
        Watched w;
        w.job = job;
        w.timer = new QTimer(this);
        w.timer->setSingleShot(true);
        const std::string id = job.id;
        connect(w.timer, &QTimer::timeout, this, [this, id] {
            const auto it = watched_.find(id);
            if (it == watched_.end()) return;
            if (running(id)) {
                it->second.changed_while_running = true;
                return;
            }
            start(it->second.job, QStringLiteral("--on-change"), false);
        });
        std::vector<std::string> ignored = {jobs::config_dir()};
        if (!is_cloud_location(job.repository)) ignored.push_back(os::absolute_path(job.repository));
        QPointer<EventRuns> self(this);
        for (const std::string& source : job.sources) {
            w.watches.push_back(std::make_unique<FolderWatch>(os::absolute_path(source), ignored, job.excludes, [self, id] {
                QMetaObject::invokeMethod(
                    self.data(), [self, id] { if (self) self->changed(id); }, Qt::QueuedConnection);
            }));
        }
        keep.emplace(job.id, std::move(w));
    }
    for (auto& [id, w] : watched_) delete w.timer;
    watched_ = std::move(keep);
}

void EventRuns::changed(const std::string& job_id) {
    const auto it = watched_.find(job_id);
    if (it == watched_.end()) return;
    Watched& w = it->second;
    if (running(job_id)) {
        w.changed_while_running = true;  // another run once this one ends
        return;
    }
    if (!w.timer->isActive()) w.timer->start(std::chrono::minutes(std::clamp(w.job.continuous_minutes, 1, 1440)));
}

bool EventRuns::running(const std::string& job_id) const {
    return std::any_of(runs_.begin(), runs_.end(), [&](const Run& r) { return r.job == job_id; });
}

bool EventRuns::start(const jobs::Job& job, const QString& flag, bool sign_out) {
    QString program = Runner::engine_path();
#ifdef Q_OS_WIN
    program = QFileInfo(program).dir().filePath(QStringLiteral("vaultik-task.exe"));
#endif
    qint64 pid = 0;
    if (!QProcess::startDetached(program, {QStringLiteral("run-job"), qs(job.id), flag}, QString(), &pid)) return false;
    runs_.push_back({job.id, static_cast<std::uint32_t>(pid), sign_out});
    if (!poll_->isActive()) poll_->start();
    return true;
}

void EventRuns::poll() {
    for (auto it = runs_.begin(); it != runs_.end();) {
        if (os::process_alive(it->pid)) {
            ++it;
            continue;
        }
        const std::string job = it->job;
        it = runs_.erase(it);
        emit run_finished(qs(job));
        const auto w = watched_.find(job);
        if (w != watched_.end() && w->second.changed_while_running) {
            w->second.changed_while_running = false;
            changed(job);
        }
    }
#ifdef Q_OS_WIN
    if (blocking_ && std::none_of(runs_.begin(), runs_.end(), [](const Run& r) { return r.sign_out; })) {
        blocking_ = false;
        ShutdownBlockReasonDestroy(reinterpret_cast<HWND>(window_->winId()));
        // Still signing out or shutting down (the user did not cancel it): this program closes, so
        // Windows goes on.
        if (GetSystemMetrics(SM_SHUTTINGDOWN) != 0) QCoreApplication::quit();
    }
#endif
    if (runs_.empty()) poll_->stop();
}

bool EventRuns::session_ending(std::intptr_t flags) {
#ifdef Q_OS_WIN
    // Not when Windows must end now, or when an installer only asks this program to close.
    if ((flags & ENDSESSION_CRITICAL) != 0 || (flags & ENDSESSION_CLOSEAPP) != 0) return false;
    if (blocking_) return true;
    const jobs::JobStore store;
    bool started = false;
    for (const jobs::Job& job : store.list()) {
        if (!job.schedule.on_sign_out || job.run_as_admin || running(job.id)) continue;
        if (!jobs::event_backup_due(store, job)) continue;
        started = start(job, QStringLiteral("--on-sign-out"), true) || started;
    }
    if (!started) return false;
    blocking_ = true;
    const QString reason = tr("Vaultik is backing up before Windows signs out. This takes a few minutes at most.");
    ShutdownBlockReasonCreate(reinterpret_cast<HWND>(window_->winId()), reinterpret_cast<LPCWSTR>(reason.utf16()));
    return true;
#else
    (void)flags;
    return false;
#endif
}

bool EventRuns::nativeEventFilter(const QByteArray& type, void* message, qintptr* result) {
#ifdef Q_OS_WIN
    if (type != "windows_generic_MSG") return false;
    const MSG* msg = static_cast<const MSG*>(message);
    if (msg->message != WM_QUERYENDSESSION || window_ == nullptr || msg->hwnd != reinterpret_cast<HWND>(window_->winId()))
        return false;
    if (!session_ending(static_cast<std::intptr_t>(msg->lParam))) return false;
    if (result != nullptr) *result = FALSE;  // not yet: Windows shows the reason meanwhile
    return true;
#else
    (void)type;
    (void)message;
    (void)result;
    return false;
#endif
}
