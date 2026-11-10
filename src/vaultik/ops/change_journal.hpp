#pragma once

// Backups that skip folders in which nothing changed, from the NTFS change journal (Windows, with
// administrator rights, such as a job's scheduled runs as the system account).
//
// After a backup that read everything without errors, the journal's position on each source's
// volume is kept with the version it saved (<job store>\journal\<job id>.json). The next backup
// reads the journal from there: a folder that is not the folder of any record (nothing in it was
// added, removed, renamed or changed) and has no record itself is taken from that version.
// Once a week, and whenever the journal cannot answer (it wrapped, was recreated, another
// version is the parent, the job's folders or exclusions changed), every folder is read.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "vaultik/crypto/hash.hpp"
#include "vaultik/ops/backup.hpp"

namespace vaultik {

class ChangeJournal {
public:
    // Reads the saved position and the journals now. Never throws: without administrator rights
    // or a journal it simply changes nothing.
    ChangeJournal(std::string store_dir, std::string job_id, std::vector<std::string> sources,
                  const std::vector<std::string>& excludes);
    ~ChangeJournal();

    // Lets the backup skip unchanged folders, when the journal can answer.
    void apply(BackupOptions& options) const;
    // Whether apply lets the backup skip folders.
    bool usable() const;
    // After a backup without errors that saved `snapshot` (or found nothing changed since it):
    // the position to read from next time. Never throws.
    void save(const Id& snapshot) noexcept;

private:
    struct State;
    std::unique_ptr<State> s_;
};

}  // namespace vaultik
