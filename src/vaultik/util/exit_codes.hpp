#pragma once

// The exit codes of the vaultik command line, in one place for the command line itself, the app
// (which reads them from the engine it starts) and the tests. Each value is part of what scripts
// and the app rely on: never change one, only add new ones.

namespace vaultik {

constexpr int kExitOk = 0;
constexpr int kExitError = 1;
constexpr int kExitAuth = 2;       // wrong password or damaged data
constexpr int kExitPartial = 3;    // finished, but some files could not be read or written
constexpr int kExitCancelled = 4;
constexpr int kExitLocked = 5;     // another run holds a conflicting repository lock
constexpr int kExitInUse = 6;      // disk tools: a volume is in use by another program (--force-dismount forces it)
constexpr int kExitNoChannel = 7;  // POSIX: --progress-pipe was given but cannot be used; nothing was done
// run-job: the backup drive is not connected, so the run waits for it; nothing was done. copy:
// the same for the drive of the job's second copy.
constexpr int kExitWaiting = 8;
constexpr int kExitSetupFailed = 9;  // disk wipe --then-init: erased completely, but setting the disk up failed

}  // namespace vaultik
