#pragma once

// Volume Shadow Copy (Windows): a frozen, consistent view of a volume, so a backup can read files
// that are open or locked by other programs, and sees all files as they were at one moment.

#include <memory>
#include <string>
#include <vector>

namespace vaultik::os {

class ShadowCopy {
public:
    // Shadow copies of every local volume holding one of `paths` (network paths are skipped).
    // Needs administrator rights. Throws Error with a readable reason when it cannot be done;
    // on POSIX it always throws (not supported).
    // With `pause_sql_server` false the SQL Server writer is left out: SQL Server keeps working
    // during the snapshot instead of pausing its writes, and its database files are captured as
    // they would be after a power cut (SQL Server recovers them when they are attached).
    static std::unique_ptr<ShadowCopy> create(const std::vector<std::string>& paths, bool pause_sql_server = true);

    // Deletes the shadow copies.
    ~ShadowCopy();
    ShadowCopy(const ShadowCopy&) = delete;
    ShadowCopy& operator=(const ShadowCopy&) = delete;

    // Where to read `path` (absolute) from: inside the shadow copy of its volume, or the path
    // itself when its volume has none.
    std::string map(const std::string& path) const;

    // Volumes covered ("C:\"), and ones that could not be (with the reason).
    const std::vector<std::string>& volumes() const { return volumes_; }
    const std::vector<std::string>& skipped() const { return skipped_; }

private:
    ShadowCopy() = default;

    struct Impl;
    std::unique_ptr<Impl> impl_;
    // Volume mount point (upper case, trailing backslash) -> shadow device path.
    std::vector<std::pair<std::string, std::string>> map_;
    std::vector<std::string> volumes_;
    std::vector<std::string> skipped_;
};

}  // namespace vaultik::os
