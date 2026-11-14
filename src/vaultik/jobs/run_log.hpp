#pragma once

// A log file per run of a job: every warning of the run (each file that was skipped, each
// problem a check found, what a command printed when it failed), in full, where the history keeps
// only counts and the first few. In <settings dir>/logs/<job id>/, one file per run, at most
// kRunLogCap bytes each; only the newest kRunLogsKept of a job are kept. The run's history records
// name it (RunRecord::log), and the Activity tab opens it.

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

namespace vaultik::jobs {

constexpr std::size_t kRunLogCap = 4u << 20;  // 4 MiB per run
constexpr std::size_t kRunLogsKept = 30;      // per job

class RunLog {
public:
    // Starts the log of a run of `job_id` that started at `started` (FILETIME ticks), and removes
    // the job's oldest logs beyond kRunLogsKept. `cap` is for tests.
    RunLog(const std::string& settings_dir, const std::string& job_id, std::uint64_t started, std::size_t cap = kRunLogCap);
    ~RunLog();  // writes what is still buffered
    RunLog(const RunLog&) = delete;
    RunLog& operator=(const RunLog&) = delete;

    // Adds a line (with the time). Thread-safe; never throws. Once the cap is reached, one last
    // line says the log was cut short, and the rest is dropped.
    void line(const std::string& text);
    void flush();

    const std::string& path() const { return path_; }

private:
    std::mutex mu_;
    std::string path_;
    std::string buffer_;
    std::size_t written_ = 0;
    std::size_t cap_;
    bool full_ = false;
};

// Removes the oldest logs of a job beyond `keep`.
void prune_run_logs(const std::string& settings_dir, const std::string& job_id, std::size_t keep = kRunLogsKept);

}  // namespace vaultik::jobs
