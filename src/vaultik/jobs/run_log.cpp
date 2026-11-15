#include "vaultik/jobs/run_log.hpp"

#include <algorithm>
#include <filesystem>
#include <system_error>
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

bool is_run_log_of(const std::string& dir, const std::string& job_id, const std::string& path) {
    namespace fs = std::filesystem;
    if (dir.empty() || job_id.empty() || path.empty()) return false;
    if (job_id.find_first_of("/\\:") != std::string::npos || job_id == "." || job_id == "..") return false;
    const auto native = [](const std::string& utf8) { return fs::path(std::u8string(utf8.begin(), utf8.end())); };
    std::error_code ec;
    const fs::path file = fs::canonical(native(path), ec);
    if (ec || !fs::is_regular_file(file, ec) || ec) return false;
    const fs::path folder = fs::canonical(native(logs_dir(dir, job_id)), ec);
    if (ec) return false;
    if (file.extension() != ".log") return false;
#ifdef _WIN32
    // The file system ignores case; canonical paths may still be spelled differently.
    const auto folded = [](const fs::path& p) {
        const std::u8string s = p.u8string();
        return os::fold_case(std::string(s.begin(), s.end()));
    };
    return folded(file.parent_path()) == folded(folder);
#else
    return file.parent_path() == folder;
#endif
}

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
