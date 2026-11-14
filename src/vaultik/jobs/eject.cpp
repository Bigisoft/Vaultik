#include "vaultik/jobs/eject.hpp"

#include <chrono>

#include <nlohmann/json.hpp>

#include "vaultik/jobs/drive_lock.hpp"
#include "vaultik/platform/os.hpp"
#include "vaultik/repo/cloud.hpp"

namespace vaultik::jobs {

namespace {

std::string settings_path(const std::string& dir) {
    return os::join(dir, "drives-eject.json");
}

nlohmann::json load_settings(const std::string& dir) {
    try {
        const Bytes data = os::read_file(settings_path(dir), 1u << 20);
        nlohmann::json j = nlohmann::json::parse(std::string(data.begin(), data.end()));
        if (j.is_object()) return j;
    } catch (const std::exception&) {
    }
    return nlohmann::json::object();
}

}  // namespace

std::string backup_drive_root(const Job& job) {
    if (is_cloud_location(job.repository) || job.repository.starts_with("\\\\") || job.repository.starts_with("//"))
        return {};
    if (!job.repository_volume.empty()) return os::volume_root(job.repository_volume);
    try {
        return os::volume_of(os::absolute_path(job.repository)).root;
    } catch (const std::exception&) {
        return {};
    }
}

std::string eject_backup_drive(const Job& job, const std::string& settings_dir) {
    try {
        const std::string key = drive_lock_key(job);
        if (key.empty()) return "the backup of this job is not on a drive";
        const DriveLock lock(key, os::join(settings_dir, "locks"), std::chrono::milliseconds(0));
        const std::string root = backup_drive_root(job);
        if (root.empty()) return "the backup of this job is not on a drive that is connected";
        const os::VolumeLocation where = os::volume_of(root);
        if (where.system || !where.external) return "only an external backup drive can be ejected";
        return os::eject_drive(root);
    } catch (const DriveBusy&) {
        return "a backup, health check, cleanup or restore is using this drive; it can be ejected once it is done";
    } catch (const std::exception& e) {
        return e.what();
    }
}

bool eject_when_done(const std::string& dir, const std::string& volume) {
    if (volume.empty()) return false;
    const nlohmann::json j = load_settings(dir);
    const auto it = j.find(volume);
    return it != j.end() && it->is_boolean() && it->get<bool>();
}

void set_eject_when_done(const std::string& dir, const std::string& volume, bool on) {
    if (volume.empty()) throw Error("only a drive Vaultik knows can be ejected when done");
    nlohmann::json j = load_settings(dir);
    if (on) j[volume] = true;
    else j.erase(volume);
    const std::string text = j.dump(2) + "\n";
    os::write_file_atomic(settings_path(dir), ByteSpan(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

}  // namespace vaultik::jobs
