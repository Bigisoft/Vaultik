#pragma once

// Shows a snapshot as a read-only folder that File Explorer, a Linux file manager and other
// programs can browse (Windows: the Projected File System, built into Windows; Linux: FUSE 3).
// Nothing is copied up front: folder listings come from the snapshot's trees and a file's content
// is read from the repository when it is opened. Deleting or renaming is refused. On Windows
// anything else written there stays in the folder and never reaches the backup; on Linux the
// folder is mounted read-only. Where neither is available, available() is false.

#include <memory>
#include <string>

#include "vaultik/model/snapshot.hpp"
#include "vaultik/repo/repository.hpp"

namespace vaultik {

class SnapshotMount {
public:
    // Whether the file system is there: Windows, the Projected File System (an optional Windows
    // feature); Linux, FUSE 3 (built in, /dev/fuse usable and fusermount3 installed).
    static bool available();
    // How to turn it on or install it, for messages.
    static const char* how_to_enable();

    // Shows `snapshot` in `folder`, which must not exist yet or be an empty folder; it is created,
    // and removed again by the destructor. `repo` must outlive the mount and is used from the file
    // system's threads (serialised here; do not use it elsewhere meanwhile). Throws Error when the
    // file system is unavailable or refuses the folder.
    SnapshotMount(Repository& repo, const Snapshot& snapshot, std::string folder);
    ~SnapshotMount();
    SnapshotMount(const SnapshotMount&) = delete;
    SnapshotMount& operator=(const SnapshotMount&) = delete;

    const std::string& folder() const;
    // False once the folder stopped showing the version from outside (Linux: unmounted with
    // fusermount3 -u or umount). Always true on Windows.
    bool active() const;

    // Removes a folder left behind by a mount that ended without cleaning up (its process was
    // killed); on Linux it is unmounted first. Returns false when it could not be removed.
    static bool remove_leftover(const std::string& folder);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace vaultik
