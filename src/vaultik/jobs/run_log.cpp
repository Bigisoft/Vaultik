#include "vaultik/jobs/run_log.hpp"

#include <algorithm>
#include <vector>

#include "vaultik/platform/os.hpp"

namespace vaultik::jobs {

namespace {

std::string logs_dir(const std::string& dir, const std::string& job_id) {
    return os::join(os::join(dir, "logs"), job_id);
}

void write(const std::string& path, const std::string& text) {
    os::append_file(path, ByteSpan(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()), 0600);
}

}  // namespace

void prune_run_logs(const std::string& dir, const std::string& job_id, std::size_t keep) {
    const std::string folder = logs_dir(dir, job_id);
    std::vector<std::string> names;
    os::DirReader reader(folder);
    if (!reader.ok()) return;
    std::string name;
    os::FileInfo info;
    while (reader.next(name, info))
        if (info.type == os::EntryType::File && name.ends_with(".log")) names.push_back(name);
    if (names.size() <= keep) return;
    std::sort(names.begin(), names.end());  // named by start time, with leading zeros: oldest first
    for (std::size_t i = 0; i + keep < names.size(); ++i) (void)os::remove_file(os::join(folder, names[i]));
}

RunLog::RunLog(const std::string& dir, const std::string& job_id, std::uint64_t started, std::size_t cap) : cap_(cap) {
    (void)os::make_dir(os::join(dir, "logs"));
    const std::string folder = logs_dir(dir, job_id);
    (void)os::make_dir(folder);
    std::string stamp = std::to_string(started);
    stamp.insert(0, stamp.size() < 20 ? 20 - stamp.size() : 0, '0');
    path_ = os::join(folder, stamp + ".log");
    try {
        prune_run_logs(dir, job_id, kRunLogsKept > 0 ? kRunLogsKept - 1 : 0);  // room for this one
    } catch (const std::exception&) {
    }
}

RunLog::~RunLog() {
    flush();
}

void RunLog::line(const std::string& text) {
    std::lock_guard<std::mutex> lock(mu_);
    if (full_) return;
    std::string l = os::format_time_local(os::now()) + "  " + text + "\n";
    if (written_ + buffer_.size() + l.size() > cap_) {
        l = "... the log was cut short here (at its size limit of " + std::to_string(cap_ >> 10) + " KiB)\n";
        full_ = true;
    }
    buffer_ += l;
    if (buffer_.size() >= (64u << 10) || full_) {
        try {
            write(path_, buffer_);
            written_ += buffer_.size();
        } catch (const std::exception&) {
        }
        buffer_.clear();
    }
}

void RunLog::flush() {
    std::lock_guard<std::mutex> lock(mu_);
    if (buffer_.empty()) return;
    try {
        write(path_, buffer_);
        written_ += buffer_.size();
    } catch (const std::exception&) {
    }
    buffer_.clear();
}

}  // namespace vaultik::jobs
