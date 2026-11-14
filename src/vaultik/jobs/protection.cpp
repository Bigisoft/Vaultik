#include "vaultik/jobs/protection.hpp"

#include <algorithm>
#include <set>

#include "vaultik/crypto/hash.hpp"
#include "vaultik/jobs/admin_jobs.hpp"
#include "vaultik/jobs/drive_lock.hpp"
#include "vaultik/platform/os.hpp"
#include "vaultik/repo/cloud.hpp"
#include "vaultik/util/hex.hpp"

namespace vaultik::jobs {

namespace {

constexpr std::uint64_t kDay = 864'000'000'000ull;

// One name for a destination however it was typed (separators, case on Windows).
std::string kit_name(const std::string& location) {
    std::string s = location;
    if (!is_cloud_location(s)) {
        try {
            s = os::absolute_path(s);
        } catch (const std::exception&) {
        }
        std::replace(s.begin(), s.end(), '\\', '/');
        while (s.size() > 1 && s.back() == '/') s.pop_back();
#ifdef _WIN32
        s = os::fold_case(s);
#endif
    }
    const Id id = blake3(ByteSpan(reinterpret_cast<const std::uint8_t*>(s.data()), s.size()));
    return to_hex(ByteSpan(id.data(), 16));
}

std::string kit_path(const std::string& dir, const std::string& location) {
    return os::join(os::join(dir, "kits"), kit_name(location));
}

}  // namespace

void note_recovery_kit(const std::string& dir, const std::string& location) {
    (void)os::make_dir(os::join(dir, "kits"));
    const std::string when = std::to_string(os::now());
    os::write_file_atomic(kit_path(dir, location), ByteSpan(reinterpret_cast<const std::uint8_t*>(when.data()), when.size()), 0600);
}

bool recovery_kit_noted(const std::string& dir, const std::string& location) {
    return os::exists(kit_path(dir, location));
}

ProtectionFacts protection_facts(const JobStore& store, const Job& job) {
    ProtectionFacts f;
    try {
        // A job with administrator rights keeps its password in the admin-only store.
        f.password_saved = !store.password(job.id).empty() || (job.run_as_admin && has_admin_copy(job.id));
    } catch (const std::exception&) {
    }
    try {
        f.kit_saved = recovery_kit_noted(store.dir(), job.repository);
    } catch (const std::exception&) {
    }
    return f;
}

std::vector<ProtectionGap> protection_gaps(const Job& job, const std::vector<Job>& all,
                                           const std::vector<RunRecord>& history, const ProtectionFacts& facts,
                                           std::uint64_t now) {
    std::vector<ProtectionGap> gaps;
    const bool scheduled = has_system_schedule_unpaused(job.schedule) || job.backup_on_connect || job.continuous;
    if (!facts.password_saved && scheduled)
        gaps.push_back({"password", "The password is not saved on this PC, so scheduled backups of this job cannot run.",
                        "edit"});
    if (job.schedule.paused)
        gaps.push_back({"paused", "The schedule is paused: nothing backs this job up on its own.", "resume"});
    if (job.second_copy.repository.empty())
        gaps.push_back({"second-copy",
                        "There is no second copy: if the backup drive fails or is lost, every version is gone.", "edit"});
    if (!facts.kit_saved)
        gaps.push_back({"kit",
                        "The recovery kit was never saved or printed: without the password nobody can restore "
                        "these backups.",
                        "kit"});

    // Health checks.
    const RunRecord* check = last_check(history);
    std::uint64_t last_done = 0;
    for (const RunRecord& r : history)
        if (check_completed(r) || (r.kind == RunKind::FullCheck && r.status == RunStatus::Partial))
            last_done = std::max(last_done, r.finished);
    const bool backed_up = last_backup(history) != nullptr;
    if (!job.health.weekly && !job.health.monthly) {
        gaps.push_back({"checks-off", "Health checks are off: damage on the backup drive would go unnoticed.", "edit"});
    } else if (check != nullptr && (found_damage(*check) || check_not_finished(*check))) {
        gaps.push_back({"check-failed",
                        found_damage(*check) ? "The last health check found damage in the backup."
                                             : "The last health check could not finish.",
                        "check"});
    } else if (backed_up) {
        const std::uint64_t period = (job.health.weekly ? 7 : 30) * kDay;
        if (last_done == 0 || last_done + 2 * period <= now)
            gaps.push_back({"check-overdue",
                            last_done == 0 ? "The backup was never checked." : "The last health check is overdue.",
                            "check"});
    }
    if (job.image_disks.empty() && backed_up) {
        const bool tested = std::any_of(history.begin(), history.end(), [](const RunRecord& r) {
            return r.kind == RunKind::RestoreTest && r.status == RunStatus::Success;
        });
        if (!tested)
            gaps.push_back({"test-restore", "A test restore was never run: nothing has shown yet that files come back.",
                            "test"});
    }

    // Files only in the cloud (OneDrive) that the newest backup could not save.
    if (const RunRecord* b = last_backup(history); b != nullptr && b->cloud_only > 0 && !job.download_cloud_files)
        gaps.push_back({"cloud-only",
                        std::to_string(b->cloud_only) +
                            (b->cloud_only == 1 ? " file is" : " files are") +
                            " only in the cloud and not in the backup (the job can download them for the backup).",
                        "edit"});

    // Drives: the backup on the very drive it protects, or every backup on one drive.
    if (!is_cloud_location(job.repository)) {
        const std::string dest = drive_lock_key(job);
        for (const std::string& src : job.sources) {
            if (!dest.empty() && drive_lock_key(src) == dest) {
                gaps.push_back({"same-drive",
                                "The backup is on the same drive as files it backs up: if that drive fails, both are "
                                "gone.",
                                "edit"});
                break;
            }
        }
        if (all.size() >= 2) {
            std::set<std::string> drives;
            bool any_copy = false;
            for (const Job& j : all) {
                drives.insert(is_cloud_location(j.repository) ? "cloud:" + j.repository : drive_lock_key(j));
                any_copy = any_copy || !j.second_copy.repository.empty();
            }
            if (drives.size() == 1 && !any_copy)
                gaps.push_back({"one-drive",
                                "All " + std::to_string(all.size()) +
                                    " backup jobs save to the same drive and none has a second copy: one lost drive "
                                    "loses them all.",
                                "edit"});
        }
    }
    return gaps;
}

}  // namespace vaultik::jobs
