#pragma once

#include <QDialog>
#include <QString>
#include <QStringList>

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "vaultik/util/task_events.hpp"

namespace vaultik::os {
class ElevatedProcess;
class ProgressServer;
}

class QLabel;
class QProgressBar;
class QPushButton;

// What a task that needs administrator rights shows while it runs.
struct ElevatedTask {
    QString title;            // the status window's title
    QString working;          // what it does, e.g. "Erasing disk 3..."
    QString cancel_question;  // asked before stopping it; empty: it cannot be stopped
    QString done;             // shown when it succeeded
};

struct ElevatedResult {
    int code = -1;            // the engine's exit code; -1 when it did not start
    QString error;            // why it did not start
    QString status;           // of the engine's "done" event (success, failed, cancelled, in_use)
    QString message;          // the engine's own message with it
    QStringList in_use;       // the drives in use it named
    bool in_window = false;   // no progress channel: it ran in a window of its own
    bool ended = false;       // it ended (or did not start), and `code` tells how
    bool left_running = false;  // the window was closed (or Vaultik was) while it ran: it goes on
};

// Runs the command-line engine with `args` with administrator rights and shows its progress in
// a status window until it ended: Windows after the approval (UAC) prompt, without a console
// window of its own; Linux through pkexec. The engine sends its events over a private progress
// channel (--progress-pipe), and Cancel there asks it to stop. When the channel cannot be used,
// the engine shows its progress in a console window of its own as before (Windows), and the
// status window says so. When the task ends because a drive is in use, the window closes at
// once (the caller asks whether to force it closed).
ElevatedResult run_elevated_task(QWidget* parent, const ElevatedTask& task, const QStringList& args);

// Linux: whether run_elevated_task can get root rights (pkexec is installed); when not, the
// caller shows the command to run with sudo instead. Windows: always true.
bool elevated_tasks_possible();

// How many of these tasks are running (for the question when Vaultik is closed).
int elevated_tasks_running();

class ElevatedTaskDialog : public QDialog {
    Q_OBJECT

public:
    ElevatedTaskDialog(const ElevatedTask& task, const QStringList& args, QWidget* parent = nullptr);
    ~ElevatedTaskDialog() override;

    // Starts the engine; then exec().
    void start();
    ElevatedResult result_of() const { return result_; }

protected:
    void reject() override;

private:
    void on_line(const QString& line);
    void on_in_window();
    void on_connected();
    void on_channel_lost();
    // `error`: why it did not start, or (`started`) why how it ended cannot be told.
    void on_finished(int code, const QString& error, bool started);
    void show_progress(const vaultik::TaskEvent& e);
    void work(std::vector<std::string> argv);
    std::optional<int> follow(vaultik::os::ElevatedProcess& process);
    void post_line(const std::string& line);

    ElevatedTask task_;
    QStringList args_;
    ElevatedResult result_;
    QLabel* heading_ = nullptr;
    QLabel* phase_ = nullptr;
    QProgressBar* bar_ = nullptr;
    QLabel* details_ = nullptr;
    QLabel* note_ = nullptr;
    QPushButton* button_ = nullptr;
    bool running_ = false;
    bool connected_ = false;
    bool cancelling_ = false;
    bool lost_ = false;       // the channel broke while it runs: it can no longer be stopped from here
    bool may_close_ = false;  // the window may be closed while it runs (it goes on)
    vaultik::RateEstimator rate_;
    std::uint64_t last_total_ = 0;
    int last_pass_ = -1;
    bool last_verifying_ = false;

    std::unique_ptr<vaultik::os::ProgressServer> server_;
    double started_ = 0;
    std::thread worker_;
    std::atomic<bool> abort_{false};
    std::atomic<bool> cancel_{false};
};
