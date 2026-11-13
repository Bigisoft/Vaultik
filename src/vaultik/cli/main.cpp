// Vaultik command-line interface.

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <iostream>
#include <thread>
#include <cstdio>
#include <cstdlib>  // std::system (image_cmds.inc) on every platform
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <condition_variable>
#include <mutex>
#include <new>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include <future>

#include <nlohmann/json.hpp>

#include "vaultik/cli/console.hpp"
#include "vaultik/crypto/hash.hpp"
#include "vaultik/jobs/job.hpp"
#include "vaultik/jobs/scheduler.hpp"
#include "vaultik/jobs/settings_transfer.hpp"
#include "vaultik/model/tree.hpp"
#include "vaultik/image/disk.hpp"
#include "vaultik/image/disk_tools.hpp"
#include "vaultik/image/recovery_media.hpp"
#include "vaultik/ops/apps.hpp"
#include "vaultik/ops/backup.hpp"
#include "vaultik/ops/prune.hpp"
#include "vaultik/ops/repair.hpp"
#include "vaultik/ops/restore.hpp"
#include "vaultik/ops/restore_test.hpp"
#include "vaultik/ops/rot_check.hpp"
#include "vaultik/ops/thaw.hpp"
#include "vaultik/ops/verify.hpp"
#include "vaultik/repo/keys.hpp"
#include "vaultik/repo/backends.hpp"
#include "vaultik/repo/s3_backend.hpp"
#include "vaultik/ops/copy.hpp"
#include "vaultik/ops/diff.hpp"
#include "vaultik/ops/history.hpp"
#include "vaultik/ops/mount.hpp"
#include "vaultik/repo/lock.hpp"
#include "vaultik/crypto/crypto.hpp"
#include "vaultik/repo/repository.hpp"
#include "vaultik/util/resources.hpp"
#include "vaultik/version.hpp"
#include "vaultik/platform/os.hpp"
#include "vaultik/platform/vss.hpp"
#include "vaultik/jobs/admin_jobs.hpp"
#include "vaultik/jobs/alerts.hpp"
#include "vaultik/jobs/computers.hpp"
#include "vaultik/jobs/drive_history.hpp"
#include "vaultik/jobs/report.hpp"
#include "vaultik/jobs/growth.hpp"
#include "vaultik/jobs/recovery_kit.hpp"
#include "vaultik/jobs/update_check.hpp"
#include "vaultik/ops/change_journal.hpp"
#include "vaultik/ops/move_here.hpp"
#include "vaultik/ops/databases.hpp"
#include "vaultik/ops/docker.hpp"
#include "vaultik/ops/virt.hpp"
#include "vaultik/util/hex.hpp"
#include "vaultik/platform/acl.hpp"
#include "vaultik/platform/drive_health.hpp"

#ifndef _WIN32
#include "vaultik/platform/mounts_posix.hpp"
#endif

#ifdef _WIN32
#include <windows.h>

#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif
#include "vaultik/util/win.hpp"
#else
#include <poll.h>
#include <unistd.h>
#endif

namespace vaultik::cli {

namespace {

constexpr char kVersion[] = VAULTIK_VERSION;

// Exit codes.
constexpr int kExitOk = 0;
constexpr int kExitError = 1;
constexpr int kExitAuth = 2;
constexpr int kExitPartial = 3;  // finished, but some files could not be read or written
constexpr int kExitCancelled = 4;
constexpr int kExitLocked = 5;  // another run holds a conflicting repository lock
constexpr int kExitInUse = 6;   // disk tools: a volume is in use by another program (--force-dismount forces it)

// In pieces (the ")" R"(" joins below): Visual Studio 2022 refuses a single string literal over
// 16 KB (error C2026); adjacent literals are joined into one string.
constexpr char kUsage[] = R"(Vaultik - deduplicating, compressed, encrypted backups

Usage: vaultik <command> -r <repository> [options]

Commands:
  init                         Create a new repository (asks for a password)
  backup <path>...             Back up folders or files
      --exclude <pattern>      Skip names matching * ? wildcards (repeatable); a
                               pattern containing a path separator matches the
                               full path
      --tag <tag>              Label the snapshot (repeatable)
      --no-parent              Re-read every file instead of reusing unchanged ones
      --parent <snapshot>      Compare against this snapshot instead of the latest
      --skip-unchanged         Save no snapshot when nothing changed since it
      --check-every-file       Ask every file for its size and times instead of
                               trusting the folder listing (slower; catches files
                               changed through another name, a hard link)
      --threads <n>            Compress and encrypt on n worker threads (default: up
                               to 4; 0 at background priority or with --max-memory)
      --read-threads <n>       Read, chunk and hash n files at once (default: up to
                               4; 1 at background priority or with --max-memory;
                               also accepted by run-job)
      --uploads <n>            Upload n packs in the background (default 2 for cloud
                               storage, 1 for folders; also accepted by run-job)
      --vss <mode>             Windows: read from a Volume Shadow Copy, so open and
                               locked files are included and everything is from one
                               moment. auto (default): when running as administrator,
                               otherwise read live with a warning; on: required;
                               off: never. run-job follows the job's setting
      --vss-skip-sql           Do not pause SQL Server for the shadow copy; its
                               database files are then copied as they would be
                               after a power cut (SQL Server recovers them)
      --apps                   Keep the list of installed programs with the
                               snapshot (saved jobs do this unless turned off)
      --wsl <distro>           Windows: also back up this WSL distribution
                               (repeatable), exported with wsl --export into the
                               file WSL/<distro>.tar; its ext4.vhdx is then left
                               out of the file backup
      --wsl-running <what>     A chosen distribution that is running: skip (the
                               default; it is left out with a warning), export
                               (as it runs) or stop (wsl --terminate it first)
      --hyperv <machine>       Windows, as administrator: also back up this
                               Hyper-V virtual machine (repeatable), exported with
                               Export-VM into the folder Hyper-V/<machine>
      --hyperv-staging <dir>   Where machines are exported to first, in a new
                               folder "Vaultik Hyper-V export" made there
                               (default: ProgramData\Vaultik\hyperv-export;
                               needs room for the largest one; emptied afterwards)
      --docker-volume <name>   Also back up this Docker volume (repeatable),
                               exported with tar in a throwaway container into
                               the file Docker/<name>.tar
      --docker-image <image>   The image that container runs (default: busybox,
                               alpine, debian or ubuntu, or Windows' nanoserver,
                               when one is on this computer; never downloaded)
      --database <spec>        Dump this database first with its own tool and back
                               the dump up as Databases/<name> (repeatable):
                               kind://[user@][host][:port]/database[?container=
                               <c>&name=<n>], kind sqlserver, mysql or postgres;
                               the password from VAULTIK_DB_PASSWORD
  wsl list                     The WSL distributions of this account, whether they
                               run, and their disk files
  wsl import <snapshot> <distro>
                               Install the distribution exported in that snapshot
                               (WSL/<distro>.tar) as a NEW distribution
      --name <name>            Its name (required); an existing distribution of
                               that name is never replaced unless --replace
      --to <folder>            Where to install it (required)
      --replace                Unregister an existing distribution of that name
                               first (deletes its disk)
  hyperv list                  Hyper-V virtual machines (needs administrator rights)
  hyperv restore <snapshot> <machine>
                               Restore a backed-up machine and import it as a new
                               machine (as administrator): its files go to
                               <folder>\Exported, the machine to <folder>\Machine
      --to <folder>            A new or empty folder (required)
      --name <name>            The new machine's name (default: the old one, which
                               must then not exist any more)
  hyperv import <folder>       Import a restored Hyper-V/<machine> folder as a new
                               machine (Import-VM -Copy -GenerateNewId)
      --to <folder>            A new or empty folder for its files (required)
      --name <name>            As for hyperv restore
      --report <file>          Also write the result to this file
  docker list                  The Docker volumes and the running containers
                               using them
  docker restore <snapshot> <volume>
                               Import the volume exported in that snapshot
                               (Docker/<volume>.tar) into a NEW volume
      --name <name>            Its name (required; must not exist)
      --image <image>          As --docker-image for backup
  db dump <spec>               Dump a database now, as a backup would (spec as for
                               backup --database; password from
                               VAULTIK_DB_PASSWORD), to try its settings
      --to <folder>            Where the dump goes (required)
  snapshots                    List snapshots
  computers                    Every computer backing up into the repository:
                               how its jobs last ran (each run of a saved job
                               leaves a status record there), its versions and
                               whether it is ok, has warnings, failed or is
                               stale (more than 3 days past its schedule)
      --forget <computer>      Remove a retired computer's status note (by its
                               name or id; never this computer's own)
      --ask <computer>         Ask that computer to back up now: it runs its
                               jobs on this repository once, when one of them
                               runs or Vaultik there next looks (every 15
                               minutes, every hour for cloud storage)
      --ask-job <job id>       Only this job of it (repeatable)
      --poll                   Look for requests to this computer in the
                               repositories of its saved jobs and list the
                               jobs asked for (used by the frontend; --cloud
                               includes cloud storage)
  ls <snapshot> [path]         List a folder inside a snapshot (-R for recursive)
  mount <snapshot> [folder]    Show a version as a read-only folder (Windows:
                               Projected File System, Linux: FUSE 3) until Enter
                               or Ctrl+C is pressed (files are read from the
                               backup when opened)
      --target <folder>        A new or empty folder to use (default: in the
                               settings folder, under browse)
  restore <snapshot>           Restore files
      --target <folder>        Where to restore (required)
      --path <path>            Only this file or folder, e.g. C/Users/me/Documents
      --overwrite              Replace existing files in the target
      --permissions            Windows: give items their stored owner and access
                               list (needs administrator rights for other owners)
      --no-links               Windows: skip symlinks and junctions
  verify                       Check repository structure and snapshots
      --read-data              Also read and decrypt every stored chunk
      --read-share <percent>   With --read-data, only this share of the packs,
                               a different one each time, e.g. 10
  stats                        Repository size and deduplication summary
  forget [<snapshot>...]       Remove the given snapshots, or old snapshots by a
                               keep policy; each rule keeps the newest snapshot of
                               each of the last N periods (local time), applied
                               per job, or per computer and paths for snapshots
                               made without one. Pinned snapshots always stay
                               (--force removes a pinned one given by ID)
      --keep-last <n>          The n newest snapshots
      --keep-within <days>     Every snapshot of the last n days, e.g. 30d
      --keep-hourly <n>        One per hour   --keep-daily <n>    One per day
      --keep-weekly <n>        One per week   --keep-monthly <n>  One per month
      --keep-yearly <n>        One per year
      --tag <tag>              Only snapshots with this tag (repeatable), e.g.
                               job:documents
      --prune                  Run prune afterwards
      --dry-run                Only show what would be removed
  pin <snapshot>               Keep a snapshot whatever the keep policy says
  unpin <snapshot>             Let the keep policy decide again
  note <snapshot> [text]       Describe a snapshot ("before Windows update");
                               no text removes the note
  history <path>               The versions of a file or folder, e.g.
                               C/Users/me/report.docx, where it changed (--all:
                               every snapshot that has it)
  estimate <path>...           How much a first backup reads and roughly how long it
                               takes (--exclude as for backup; --job: the job's
                               sources and exclusions); needs no repository
  diff <older> [<newer>]       Files and folders added (+), removed (-) and changed
                               (M) between two snapshots; with one, since the
                               snapshot before it
  find <pattern>               Files and folders by name in every snapshot; *
                               and ? wildcards, any case; without them, names
                               containing the text
  prune                        Delete data no snapshot uses any more
      --dry-run                Only show what would be deleted
  repair                       Rebuild the index from the stored data, checking
                               every chunk, and save damaged versions again
                               without what was lost (use --job <job> or -r)
      --quick                  Trust the packs' own headers; read no chunks
      --dry-run                Only show what would be done
      --accept-missing-packs   Go on when over a tenth of the indexed packs are
                               gone (otherwise it stops: wrong destination?)
  move-here [<snapshot>]       Move to this PC: put an old computer's Documents,
                               Desktop, pictures, browser profiles, email and game
                               saves from the backup into this account's folders
      --account <name>         The old account, when the backup holds several
      --only <list>            Only these, comma-separated: documents, desktop,
                               pictures, music, videos, downloads, browsers, email,
                               games (or single items such as chrome)
      --dry-run                Only show what goes where
      --overwrite              Replace files that exist here already
  apps [<snapshot>]            The programs that were installed when the snapshot
                               was made (default: latest), with their winget
                               package where one is known
      --missing                Only those not installed on this PC
  apps install <id>...         Install programs of that list again with winget,
                               one after the other, as you (winget asks for
                               administrator approval when an installer needs it)
      --snapshot <snapshot>    The snapshot whose list the ids come from (default:
                               latest)
  repair-data <percent>|off    Keep Reed-Solomon repair data next to the stored
                               data (1-50% of its size; jobs to drives use 5%),
                               so data damaged by bad sectors can be rebuilt by
                               repair; adds it to the data stored already
)" R"(  unlock                       Remove locks left by runs that ended abnormally
  key passwd                   Set a new password (asks for the current one, or
                               use --recovery-key); saved jobs using this
                               repository get the new password too
  key recovery                 Make a new recovery key, which can set a new
                               password when the old one is forgotten (an older
                               recovery key stops working)
      --recovery-key           Unlock with the recovery key instead of the password
                               (asked for, or set VAULTIK_RECOVERY_KEY)
  recovery-kit                 Write a recovery kit to print: one page with the
                               recovery key (also as a QR code), where the backup
                               is, this computer's name, the date and the steps to
                               restore on a new computer; never a password or a
                               cloud storage key (asks for the recovery key, or
                               set VAULTIK_RECOVERY_KEY)
      --out <file>             kit.pdf or kit.html (a PDF from here has Western
                               European letters only; the page has all of them)
      --new                    Make a new recovery key for the kit first (asks for
                               the password, or use --recovery-key; an older
                               recovery key stops working)
      --all                    Remove every lock (only when no other run is active)
  cloud-rollback               S3 with Object Lock: bring every repository file
                               back as it was at a time, from the bucket's old
                               versions (after files were deleted or overwritten)
      --to <time>              Local time, e.g. "2026-11-09 20:30"
      --dry-run                Only list what would be brought back
  cloud-thaw [<snapshot>]      S3 archive storage (GLACIER, DEEP_ARCHIVE): ask for
                               a readable copy of the data a restore of this
                               version needs (default: latest), so it can be
                               restored once the thaw is done (hours); run it
                               again to see how far it is
      --path <path>            Only what restoring this file or folder needs
                               (repeatable)
      --days <n>               Keep the readable copy this many days (default 7)
      --tier <tier>            Standard (default), Bulk (cheaper, slower) or
                               Expedited (GLACIER only, faster, dearer)
      --status                 Only report, ask for nothing
  drives                       The health of this computer's drives, as the
                               system reports it (details need administrator
                               rights on Windows, root on Linux)
      --smart                  Also list each drive's S.M.A.R.T. attributes
      --history                Also list each drive's stored health readings
  jobs                         List saved backup jobs (created in vaultik-gui)
  run-job <job>                Run a saved job by ID or name, with its own
                               sources, destination, password and resource
                               settings; the run is added to the job's history.
                               Then runs the job's health check when one is due
      --no-check               Skip the health check this time
      --on-connect             Run only if the job backs up when its drive is
                               connected, the drive is there, and it is not
                               already backing up or just finished (used by
                               the tray when a drive is plugged in)
      --on-event               Run only if the last backup is older than the
                               job's time between triggered backups (used when
                               the computer is idle or locked)
      --on-sign-out            The same, when the user signs out or shuts down
                               (used by the tray)
      --on-change              Run only if it is not already backing up (used
                               by the tray for continuous backups)
      --on-request             Run because another computer asked for it
                               (vaultik computers --ask; started by the tray)
  copy <job>                   Copy the job's new versions to its second copy now
                               (run-job does this after each backup), creating it
                               there on first use with the job's password
  check-job <job>              Read the job's share of its stored data back and
                               check every version now (added to its history);
                               also re-read a few unchanged files on this PC and
                               compare them with the newest version, to catch
                               silent damage (unless the job turned it off)
      --full                   Read all of the stored data
  alert-test                   Send a test message to the alert channels set up
                               in Settings (ntfy, Telegram, webhook)
  report                       Monthly report of every saved job: backups and their
                               results, data added, backup size and its growth,
                               health checks, test restores, drive warnings and
                               what needs attention (plain text)
      --month <YYYY-MM>        The month (default: the month before this one)
      --html <file>            Write it as a web page instead
      --json                   Print it as JSON
  grew                         Which folders added the most new data to the
                               backup, with ready-made exclusions that fit
                               (-r <repository>, or --job <job> for its versions)
      --since <time>           From when: 30d (the default), 2w, 12h, or a date
                               (2026-10-01)
      --top <n>                How many folders at most (default 15)
      --json                   Print it as JSON
  update-check                 Ask GitHub whether a newer Vaultik was released
                               (only asks; nothing is downloaded)
  test-restore <job>           Restore a few files of the newest version into a
                               temporary folder, check them (and compare them with
                               the unchanged files on this PC), then remove them
      --files <n>              How many files, at most (default 20)
      --max-size <size>        Their total size, at most (default 500M)
  schedule <job>               Register the job's schedule with the system
                               scheduler (Task Scheduler or a systemd user timer)
  unschedule <job>             Remove it again (the job itself is kept)
  pause-job <job>              Pause the job: it keeps its settings and schedule,
                               but nothing starts it on its own (schedule, events,
                               drive, changes, requests) until it is resumed;
                               run-job still runs it
  resume-job <job>             Resume a paused job (registers its schedule again)
  settings export [<file>]     Write the settings and every saved job to one file
                               (default Vaultik-settings-<computer>-<date>
                               .vaultik-settings), to set up another computer the
                               same way; without passwords and cloud keys
      --with-secrets           With them, and the whole file encrypted with a
                               password (asked for, or VAULTIK_SETTINGS_PASSWORD)
  settings import <file>       Take the jobs (and alert settings) from such a file;
                               they keep their schedule but start paused
      --jobs <a,b>             Only these jobs (their ids in the file)
      --replace                A job whose id is taken here replaces that job
                               (otherwise it is imported under a new id)
      --alerts                 Also the alert settings (where alerts are sent)
      --keep-commands          Keep the jobs' commands before and after the
                               backup and their administrator rights (left out
                               otherwise: only for a file you made yourself)
  admin-job install|remove     Used by schedule and the frontend for jobs with
                               administrator rights (asks for approval)

Disk images (Windows, administrator rights except image disks and --vhdx):
  image disks                  The disks of this computer, with their numbers
  image backup <disk>...       Back up whole disks (a number from image disks,
                               "system" for the disk Windows runs from, or an id):
                               every partition and the partition table, NTFS
                               volumes from a shadow copy and only their used
                               space (--vss off: read them while in use)
  image info <snapshot>        The disks and partitions in a disk image, and the
                               smallest disk it can be restored onto
  image restore <snapshot>     Restore a disk image
      --vhdx <file>            ... to a new virtual disk file (no administrator
                               rights needed); open it in File Explorer or use it
                               in Hyper-V
      --size <size>            Its size (default: the size of the disk imaged).
                               A disk smaller than the imaged one works when its
                               NTFS partitions can shrink to fit (see image info);
                               so does --to-disk onto a smaller disk
      --new-ids                New disk and partition ids, so it can be attached
                               next to the original disk
      --to-disk <number>       ... onto a disk, replacing everything on it. Never
                               the disk Windows runs from, nor the one holding the
                               backup; asks you to type ERASE DISK <number>
      --confirm <text>         That text, given beforehand
      --expect-size <bytes>, --expect-serial <s>, --expect-id <id>
                               The disk you chose from a listing: stops when the
                               disk at that number is another one by now
      --boot                 Then make the disk start Windows (bcdboot)
      --disk-index <n>         Which disk of the image (default 1)
  image mount <snapshot>       Open a disk image's drives read-only in File
                               Explorer until Enter is pressed (it is copied to a
                               temporary virtual disk file first; --target <folder>)
  recovery-media               Make a recovery drive: Windows PE (needs the free
                               Windows ADK with its Windows PE add-on) with
                               Vaultik, to restore a disk image onto a computer
                               that does not start
      --iso <file>             ... as an ISO file, or
      --usb <drive>            ... on a USB stick (erased; asks you to type
                               ERASE <drive>, or give --confirm)
  recovery                     The restore menu the recovery drive starts with

Disk tools (administrator rights on Windows, root on Linux, except disk list and wipe-free):
  disk list                    The disks, with what keeps one from being erased
  disk init <disk>             Set up a blank or unwanted disk for use: a new
                               partition table, one partition over the whole disk,
                               formatted, with a drive letter. Never the disk the
                               system starts or runs from or Vaultik runs from, nor
                               one with a page file, a dynamic or offline disk;
                               asks you to type ERASE DISK <number> (Linux:
                               ERASE DISK sdb)
      --gpt, --mbr             The partition table (default GPT)
      --fs <type>              ntfs (Windows default), exfat, or ext4 (Linux default)
      --label <name>           The volume's name
      --letter <X>             Windows: its drive letter (default: a free one)
      --mount <folder>         Linux: mount it there
      --size <size>            Partitions of these sizes instead (repeatable; "rest"
                               as the last one takes what is left)
      --confirm <text>         The confirmation, given beforehand
      --force-dismount         When another program has files open on the disk
                               (Linux: a partition is mounted), take it away from
                               that program first (as format /X does; Linux:
                               umount) instead of stopping; it loses access to
                               them and unsaved changes there are lost. The checks
                               and the confirmation stay the same
  disk wipe <disk>             Erase a whole disk so its data cannot be read back,
                               then leave it without a partition table (the same
                               guards and confirmation as disk init)
      --volume <X:>            ... only this volume instead (Linux: /dev/sdb1);
                               asks you to type ERASE X:
      --expect-size <bytes>, --expect-serial <s>, --expect-id <id>
                               The disk or volume you chose from a listing: stops
                               when it is another one by now
      --method <m>             zeros (one pass, default), random (one pass), or
                               dod3 (random, random, zeros, as DoD 5220.22-M)
      --verify                 Read the last pass back and compare it
      --force-dismount         As for disk init: force a disk or volume in use closed
  disk wipe-free <folder>      Overwrite the free space of the drive holding the
                               folder (or X:), so deleted files cannot be brought
                               back; files on it are not touched
      --method <m>             As for disk wipe

<snapshot> is an ID prefix (at least 4 characters) or "latest".

Resource use (any command):
  --priority <level>           background, low, normal (default) or high. Sets the
                               CPU, disk and RAM priorities below in one go
  --cpu <level>                CPU priority: idle, low, normal, high
  --io <level>                 Disk and network priority: background, low, normal
  --ram <level>                Memory priority: low (Windows reclaims Vaultik's
                               memory first), normal
  --cpu-limit <percent>        Hard cap on total CPU use, 1-100
  --max-memory <size>          Hard cap on memory, e.g. 256M (at least 128M)
  --read-limit <rate>          Max read speed per second, e.g. 50M
  --write-limit <rate>         Max write speed per second, e.g. 20M (also caps
                               uploads to a network share)
Priorities only matter when the PC is busy; limits apply all the time.

Options:
  -r, --repo <folder>          Repository location (or set VAULTIK_REPO)
  --job <job>                  Use a saved job's repository and stored password
  --password-file <file>       Read the password from a file (or set VAULTIK_PASSWORD)
  --json                       Machine-readable output: one JSON event per line;
                               a line "cancel" on stdin stops a backup or restore,
                               "pause" and "resume" pause and continue it
  --lock-wait <time>           When another run holds the repository (prune holds
                               it alone), wait this long for it, e.g. 30s, 10m
                               (default: do not wait; run-job: 10m)
  -h, --help                   Show this help
  --version                    Show the version

Exit codes: 0 ok, 1 error, 2 wrong password or damaged data, 3 finished with skipped files,
4 cancelled, 5 repository in use by another run.
)";

struct Args {
    std::string command;
    std::vector<std::string> positional;
    std::map<std::string, std::vector<std::string>> values;
    std::set<std::string> flags;

    bool has(const char* flag) const { return flags.contains(flag); }

    std::optional<std::string> value(const char* key) const {
        auto it = values.find(key);
        if (it == values.end() || it->second.empty()) return std::nullopt;
        return it->second.back();
    }

    std::vector<std::string> all(const char* key) const {
        auto it = values.find(key);
        return it == values.end() ? std::vector<std::string>{} : it->second;
    }
};

const std::set<std::string> kValueOptions = {
    "--repo",     "--tag", "--exclude", "--target",    "--path",       "--password-file",
    "--parent",   "--priority", "--cpu", "--io",       "--ram",        "--cpu-limit",
    "--max-memory", "--read-limit", "--write-limit", "--job", "--lock-wait", "--vss", "--keep-last",
    "--keep-hourly", "--keep-daily", "--keep-weekly", "--keep-monthly", "--keep-yearly",
    "--user", "--from", "--account", "--only", "--keep-within", "--read-share", "--to", "--files", "--max-size", "--threads", "--uploads",
    "--read-threads", "--days", "--tier", "--month", "--html", "--snapshot", "--forget", "--ask", "--ask-job", "--wsl", "--wsl-running", "--hyperv", "--hyperv-staging", "--docker-volume", "--docker-image", "--database", "--image", "--name", "--report", "--vhdx", "--to-disk", "--confirm", "--expect-size", "--expect-serial", "--expect-id", "--disk-index", "--iso", "--usb", "--size", "--out", "--since", "--top", "--fs", "--label", "--letter", "--mount", "--method", "--volume", "--jobs"};
const std::set<std::string> kFlagOptions = {"--json",    "--no-parent", "--overwrite", "--read-data", "--recursive",
                                              "--help",    "--version",   "--dry-run",   "--prune",     "--all",
                                              "--recovery-key", "--vss-skip-sql", "--admin", "--force",
                                              "--full", "--no-check", "--on-connect", "--on-event", "--on-sign-out", "--on-change", "--smart", "--history", "--quick", "--permissions", "--no-links", "--scheduled", "--skip-unchanged", "--check-every-file", "--status", "--deliver", "--apps", "--missing", "--on-request", "--poll", "--cloud", "--replace", "--new-ids", "--boot", "--pause", "--accept-missing-packs", "--new", "--gpt", "--mbr", "--verify", "--with-secrets", "--alerts", "--keep-commands", "--force-dismount"};

Args parse_args(const std::vector<std::string>& argv) {
    Args a;
    const std::size_t argc = argv.size();
    for (std::size_t i = 1; i < argc; ++i) {
        std::string s = argv[i];
        if (s == "-r") s = "--repo";
        if (s == "-R") s = "--recursive";
        if (s == "-h" || s == "/?") s = "--help";
        if (s.starts_with("--")) {
            const std::size_t eq = s.find('=');
            const std::string key = s.substr(0, eq);
            if (kValueOptions.contains(key)) {
                std::string v;
                if (eq != std::string::npos) {
                    v = s.substr(eq + 1);
                } else {
                    if (i + 1 >= argc) throw Error("missing value for " + key);
                    v = argv[++i];
                }
                a.values[key].push_back(v);
                continue;
            }
            if (kFlagOptions.contains(key) && eq == std::string::npos) {
                a.flags.insert(key);
                continue;
            }
            throw Error("unknown option " + s + " (see vaultik --help)");
        }
        if (a.command.empty()) {
            a.command = s;
        } else {
            a.positional.push_back(s);
        }
    }
    return a;
}

// ---- machine-readable output (--json) -------------------------------------------------------

bool g_json = false;
std::atomic<bool> g_cancel{false};
std::atomic<bool> g_stdin_closed{false};  // the program that started this one went away (--json)
std::atomic<bool> g_paused{false};        // "pause" on stdin (--json) until "resume"

// Polled between files and chunks: waits while paused, then tells whether to stop.
bool stop_requested() {
    if (interrupted()) g_cancel = true;
    while (g_paused.load() && !g_cancel.load() && !g_stdin_closed.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    return g_cancel.load();
}

// One JSON object per line on stdout; the frontend parses these.
void emit(const nlohmann::json& event) {
    // Whole lines only: the stdin watcher reports pauses from its own thread.
    static std::mutex mu;
    const std::string line = event.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
    std::lock_guard<std::mutex> lock(mu);
    out(line);
}

void warning(const std::string& msg) {
    if (g_json) {
        emit({{"event", "warning"}, {"message", msg}});
    } else {
        err("warning: " + msg + "\n");
    }
}

// One line of standard input, without its end; false at the end of the input. POSIX: read from
// the descriptor itself, since a thread blocked in std::getline holds the C library's lock on
// stdin, and exit() then waits for that lock forever (a --json run whose caller keeps the pipe
// open would never end).
bool read_input_line(std::string& line) {
#ifdef _WIN32
    return static_cast<bool>(std::getline(std::cin, line));
#else
    static std::string pending;
    for (;;) {
        const std::size_t end = pending.find('\n');
        if (end != std::string::npos) {
            line = pending.substr(0, end);
            pending.erase(0, end + 1);
            return true;
        }
        char buf[512];
        const ssize_t n = ::read(STDIN_FILENO, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            if (pending.empty()) return false;
            line.swap(pending);
            pending.clear();
            return true;
        }
        pending.append(buf, static_cast<std::size_t>(n));
    }
#endif
}

// With --json, a line "cancel" on stdin stops the running backup or restore cleanly.
void start_cancel_watcher() {
    std::thread([] {
        std::string line;
        while (read_input_line(line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (line == "cancel") {
                g_cancel = true;
                return;
            }
            if (line == "pause" || line == "resume") {
                g_paused = line == "pause";
                emit({{"event", g_paused ? "paused" : "resumed"}});
            }
        }
        g_stdin_closed = true;
    }).detach();
}

// Resource settings from the command line on top of `base` (a job's saved settings, or the
// defaults). --priority replaces only the three priorities, not the limits.
ResourcePolicy resource_policy(const Args& a, ResourcePolicy p = {}) {
    if (auto v = a.value("--priority")) {
        const ResourcePolicy preset = priority_preset(*v);
        p.cpu = preset.cpu;
        p.io = preset.io;
        p.memory = preset.memory;
    }
    if (auto v = a.value("--cpu")) p.cpu = parse_cpu_priority(*v);
    if (auto v = a.value("--io")) p.io = parse_io_priority(*v);
    if (auto v = a.value("--ram")) p.memory = parse_memory_priority(*v);
    if (auto v = a.value("--cpu-limit")) p.cpu_limit_percent = parse_percent(*v);
    if (auto v = a.value("--max-memory")) p.max_memory = parse_size(*v);
    if (auto v = a.value("--read-limit")) p.read_rate = parse_size(*v);
    if (auto v = a.value("--write-limit")) p.write_rate = parse_size(*v);
    return p;
}

std::string describe(const ResourcePolicy& p) {
    std::string s = std::string("CPU ") + to_string(p.cpu) + ", disk " + to_string(p.io) + ", RAM " +
                    to_string(p.memory);
    if (p.cpu_limit_percent) s += ", CPU cap " + std::to_string(p.cpu_limit_percent) + "%";
    if (p.max_memory) s += ", memory cap " + format_bytes(p.max_memory);
    if (p.read_rate) s += ", read " + format_bytes(p.read_rate) + "/s";
    if (p.write_rate) s += ", write " + format_bytes(p.write_rate) + "/s";
    return s;
}

// The saved job named by --job, if any.
std::optional<jobs::Job> job_arg(const Args& a) {
    const auto name = a.value("--job");
    if (!name) return std::nullopt;
    auto job = jobs::JobStore().find(*name);
    if (!job) throw Error("no saved job called " + *name + " (see vaultik jobs)");
    return job;
}

std::string repo_path(const Args& a) {
    if (auto v = a.value("--repo")) return *v;
    if (auto job = job_arg(a)) return job->repository;
    std::string env = os::get_env("VAULTIK_REPO");
    if (!env.empty()) return env;
    throw Error("no repository given: use -r <folder> or set VAULTIK_REPO");
}

// Reads a small file (at most 64 KiB) such as a password file.
std::string read_small_file(const std::string& path, const char* what) {
    os::ErrorCode e = 0;
    os::File f = os::File::open_read(os::absolute_path(path), e);
    if (!f.valid()) throw Error(std::string("cannot read ") + what + " " + path + ": " + os::error_message(e));
    std::string data;
    std::uint8_t buf[4096];
    for (;;) {
        const std::size_t n = f.read(buf, sizeof buf);
        if (n == 0) break;
        data.append(reinterpret_cast<const char*>(buf), n);
        if (data.size() > (64u << 10)) throw Error(std::string(what) + " is too large: " + path);
    }
    secure_wipe(buf, sizeof buf);
    return data;
}

std::string get_password(const Args& a, bool confirm) {
    if (auto file = a.value("--password-file")) {
        std::string pw = read_small_file(*file, "password file");
        while (!pw.empty() && (pw.back() == '\n' || pw.back() == '\r')) pw.pop_back();
        if (pw.empty()) throw Error("password file is empty");
        return pw;
    }
    if (auto job = job_arg(a)) {
        std::string stored = jobs::JobStore().password(job->id);
        if (!stored.empty()) return stored;
    }
    const std::string env = os::get_env("VAULTIK_PASSWORD");
    if (!env.empty()) return env;

    std::string pw = read_password("Repository password: ");
    if (confirm) {
        if (pw.size() < 8) throw Error("use a password of at least 8 characters");
        std::string again = read_password("Repeat password: ");
        const bool same = again == pw;
        secure_wipe(again.data(), again.size());
        if (!same) throw Error("passwords do not match");
    }
    if (pw.empty()) throw Error("empty password");
    return pw;
}

// Cloud storage keys: a saved job's (with --job), otherwise none, so the environment is used.
CloudCredentials cloud_credentials(const Args& a) {
    if (auto job = job_arg(a)) return jobs::JobStore().cloud_credentials(*job);
    return {};
}

std::unique_ptr<Backend> backend_for(const Args& a, bool create = false) {
    const CloudCredentials cloud = cloud_credentials(a);
    return open_backend(repo_path(a), &cloud, create);
}

std::unique_ptr<Repository> open_repo(const Args& a, bool load_index = true) {
    auto backend = backend_for(a);
    std::string pw = get_password(a, false);
    status("Opening repository...");
    std::unique_ptr<Repository> repo;
    try {
        repo = Repository::open(std::move(backend), pw, load_index);
    } catch (...) {
        secure_wipe(pw.data(), pw.size());
        clear_status();
        throw;
    }
    secure_wipe(pw.data(), pw.size());
    clear_status();
    return repo;
}

// --lock-wait: "90", "30s", "10m" or "2h".
std::chrono::seconds lock_wait(const Args& a, std::chrono::seconds fallback = std::chrono::seconds(0)) {
    const auto v = a.value("--lock-wait");
    if (!v) return fallback;
    std::string s = *v;
    long long unit = 1;
    if (!s.empty()) {
        const char u = static_cast<char>(std::tolower(static_cast<unsigned char>(s.back())));
        if (u == 's' || u == 'm' || u == 'h') {
            unit = u == 's' ? 1 : u == 'm' ? 60 : 3600;
            s.pop_back();
        }
    }
    try {
        std::size_t used = 0;
        const long long n = std::stoll(s, &used);
        if (used == s.size() && n >= 0 && n <= 7 * 24 * 3600) return std::chrono::seconds(n * unit);
    } catch (const std::exception&) {
    }
    throw Error("invalid --lock-wait value " + *v + " (use for example 30s or 10m)");
}

RetentionPolicy retention_arg(const Args& a) {
    RetentionPolicy p;
    auto count = [&](const char* key, int& out) {
        const auto v = a.value(key);
        if (!v) return;
        try {
            std::size_t used = 0;
            const int n = std::stoi(*v, &used);
            if (used == v->size() && n >= 0) {
                out = n;
                return;
            }
        } catch (const std::exception&) {
        }
        throw Error(std::string("invalid value for ") + key + ": " + *v);
    };
    count("--keep-last", p.last);
    count("--keep-hourly", p.hourly);
    count("--keep-daily", p.daily);
    count("--keep-weekly", p.weekly);
    count("--keep-monthly", p.monthly);
    count("--keep-yearly", p.yearly);
    if (auto v = a.value("--keep-within")) {
        // "30" or "30d"
        std::string days = *v;
        if (!days.empty() && (days.back() == 'd' || days.back() == 'D')) days.pop_back();
        int n = -1;
        try {
            std::size_t used = 0;
            n = std::stoi(days, &used);
            if (used != days.size()) n = -1;
        } catch (const std::exception&) {
            n = -1;
        }
        if (n < 0) throw Error("invalid value for --keep-within: " + *v + " (a number of days, e.g. 30d)");
        p.within_days = n;
    }
    return p;
}

constexpr char kLockLost[] =
    "the repository lock was lost (this computer was probably asleep or stalled for a long time and "
    "another run took over); stopped without saving";

PruneResult prune_and_report(Repository& repo, RepoLock& lock, bool dry_run) {
    PruneOptions o;
    o.dry_run = dry_run;
    o.before_delete = [&lock] {
        if (!lock.held()) throw LockedError(kLockLost);
    };
    o.on_progress = [](const std::string& msg) {
        if (!g_json) status("Prune: " + msg + "...");
    };
    o.on_warning = [](const std::string& msg) { warning(msg); };
    PruneResult r = run_prune(repo, o);
    if (!g_json) clear_status();
    return r;
}

std::uint64_t space_freed(const PruneResult& r) {
    return r.bytes_deleted > r.bytes_written ? r.bytes_deleted - r.bytes_written : 0;
}

struct SnapshotRef {
    Id id{};
    Snapshot snapshot;
};

std::vector<SnapshotRef> load_all_snapshots(Repository& repo) {
    std::vector<SnapshotRef> all;
    for (const Id& id : repo.list_snapshot_ids()) {
        try {
            all.push_back({id, repo.load_snapshot(id)});
        } catch (const Error& e) {
            err("warning: cannot read snapshot " + id_hex(id).substr(0, 8) + ": " + e.what() + "\n");
        }
    }
    std::sort(all.begin(), all.end(), [](const SnapshotRef& x, const SnapshotRef& y) {
        return x.snapshot.time < y.snapshot.time;
    });
    return all;
}

// The versions of a saved job: those tagged with it, else (made before jobs tagged their versions)
// those of this computer.
std::function<bool(const Snapshot&)> versions_of_job(const std::vector<SnapshotRef>& all, const jobs::Job& job) {
    const std::string tag = "job:" + job.id;
    const auto tagged = [tag](const Snapshot& s) { return std::find(s.tags.begin(), s.tags.end(), tag) != s.tags.end(); };
    const bool any = std::any_of(all.begin(), all.end(), [&](const SnapshotRef& s) { return tagged(s.snapshot); });
    if (any) return tagged;
    return [host = os::hostname()](const Snapshot& s) { return s.hostname == host; };
}

SnapshotRef resolve_snapshot(Repository& repo, const std::string& ref_w) {
    std::string ref = ref_w;
    for (char& c : ref) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ref == "latest") {
        auto all = load_all_snapshots(repo);
        if (all.empty()) throw Error("the repository has no snapshots yet");
        return all.back();
    }
    if (ref.size() < 4) throw Error("snapshot ID prefix must be at least 4 characters");
    std::vector<Id> matches;
    for (const Id& id : repo.list_snapshot_ids())
        if (id_hex(id).starts_with(ref)) matches.push_back(id);
    if (matches.empty()) throw Error("no snapshot matches " + ref);
    if (matches.size() > 1) throw Error("snapshot ID " + ref + " is ambiguous; give more characters");
    return {matches[0], repo.load_snapshot(matches[0])};
}

std::string short_id(const Id& id) {
    return id_hex(id).substr(0, 8);
}

std::string pad(std::string s, std::size_t width) {
    // Width in code points is close enough for a terminal table.
    std::size_t cps = 0;
    for (const unsigned char c : s) cps += (c & 0xC0) != 0x80;
    if (cps < width) s.append(width - cps, ' ');
    return s;
}

std::string rpad(const std::string& s, std::size_t width) {
    return s.size() >= width ? s : std::string(width - s.size(), ' ') + s;
}

double seconds_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

// ---------------------------------------------------------------------------------------------

int cmd_init(const Args& a) {
    const auto backend_ptr = backend_for(a, /*create=*/true);
    Backend& backend = *backend_ptr;
    std::string pw = get_password(a, true);
    status("Creating repository...");
    Repository::init(backend, pw);
    const std::string recovery = create_recovery_key(backend, {pw});
    secure_wipe(pw.data(), pw.size());
    clear_status();
    out("Created repository at " + backend.location() + "\n");
    out("Recovery key: " + recovery + "\n");
    out("Keep the password safe: without it these backups cannot be restored. Write the\n"
        "recovery key down and keep it apart from this computer: it can set a new password\n"
        "if the password is forgotten.\n");
    return kExitOk;
}

enum class VssMode { Off, Auto, On };

// --vss auto|on|off. Auto (the Windows default) uses a shadow copy when it can be made.
VssMode vss_arg(const Args& a, VssMode fallback) {
    const auto v = a.value("--vss");
    if (!v) return fallback;
    if (*v == "auto") return VssMode::Auto;
    if (*v == "on") return VssMode::On;
    if (*v == "off") return VssMode::Off;
    throw Error("--vss takes auto, on or off");
}

#ifdef _WIN32
constexpr VssMode kDefaultVss = VssMode::Auto;
#else
constexpr VssMode kDefaultVss = VssMode::Off;
#endif

// Makes shadow copies of the source volumes and points the backup's reads at them. The returned
// object must outlive the backup. Auto falls back to reading live with a warning.
std::unique_ptr<os::ShadowCopy> prepare_shadow_copy(VssMode mode, bool pause_sql_server, BackupOptions& o) {
    if (mode == VssMode::Off) return nullptr;
    std::vector<std::string> full;
    for (const std::string& p : o.paths) full.push_back(os::absolute_path(p));
    if (!g_json) status("Creating a shadow copy...");
    std::unique_ptr<os::ShadowCopy> sc;
    try {
        sc = os::ShadowCopy::create(full, pause_sql_server);
    } catch (const Error& e) {
        if (!g_json) clear_status();
        if (mode == VssMode::On) throw;
        warning(std::string("reading files live, without a shadow copy (open or locked files may be skipped): ") +
                e.what());
        return nullptr;
    }
    if (!g_json) clear_status();
    for (const std::string& s : sc->skipped()) warning("no shadow copy for " + s + "; read live");
    for (const std::string& s : sc->warnings()) warning(s);
    if (sc->volumes().empty()) return nullptr;
    if (!g_json) {
        std::string list;
        for (const std::string& v : sc->volumes()) list += (list.empty() ? "" : ", ") + v;
        err("Reading from a shadow copy of " + list + "\n");
    }
    const os::ShadowCopy* raw = sc.get();
    o.read_path = [raw](const std::string& p) { return raw->map(p); };
    return sc;
}

// Runs a backup with console or JSON reporting. Fills `record` with the outcome (also on failure,
// before rethrowing) and returns the exit code. `lock` is the shared lock held for it.
// A backup that saw signs of ransomware: the version before it is pinned with a note, so no keep
// rule can remove the last good copy, and the run says so loudly. (The caller holds a lock.)
void raise_alarm(Repository& repo, const BackupResult& r, jobs::RunRecord& record) {
    const ChangeAlarm& a = r.alarm;
    std::string what = std::to_string(a.suspicious()) + " file(s)";
    if (a.parent_files > 0) what += " of " + std::to_string(a.parent_files);
    record.alert = what + " look encrypted or were renamed with an added extension since the previous version";
    if (!a.examples.empty()) record.alert += ", e.g. " + a.examples.front();
    std::string kept;
    if (r.snapshot.parent) {
        try {
            Snapshot before = repo.load_snapshot(*r.snapshot.parent);
            before.pinned = true;
            if (before.note.empty())
                before.note = "Kept automatically: the next backup (" + os::format_time_local(r.snapshot.time) +
                              ") saw many files that look encrypted";
            repo.update_snapshot(*r.snapshot.parent, before);
            kept = short_id(*r.snapshot.parent);
        } catch (const Error& e) {
            warning(std::string("could not pin the previous version: ") + e.what());
        }
    }
    record.alert += kept.empty() ? "." : ". The previous version " + kept + " is pinned so it stays.";
    if (g_json) {
        nlohmann::json examples = nlohmann::json::array();
        for (const std::string& p : a.examples) examples.push_back(p);
        emit({{"event", "alert"},
              {"message", record.alert},
              {"became_random", a.became_random},
              {"renamed", a.renamed},
              {"parent_files", a.parent_files},
              {"pinned", kept},
              {"examples", examples}});
    } else {
        clear_status();
        err("WARNING: " + record.alert + "\n");
        for (const std::string& p : a.examples) err("  " + p + "\n");
        err("If you did not change these files yourself, your computer may have ransomware. Do not trust the new "
            "version; restore from the pinned one.\n");
    }
}

// The installed programs, listed on another thread while the backup runs (winget can take up to a
// minute); the backup asks for the list once its files are stored. A run that ends early stops
// the listing.
std::function<std::optional<std::string>()> app_list_in_background() {
    struct Task {
        std::atomic<bool> stop{false};
        std::future<AppScan> scan;
        ~Task() {
            stop = true;
            if (scan.valid()) scan.wait();
        }
    };
    auto task = std::make_shared<Task>();
    std::atomic<bool>* stop = &task->stop;
    task->scan = std::async(std::launch::async,
                            [stop] { return scan_installed_apps(true, [stop] { return stop->load() || g_cancel.load(); }); });
    return [task]() -> std::optional<std::string> { return apps_to_json(task->scan.get().apps); };
}

// The message of a run that found nothing changed (and so saved no version).
constexpr const char* kUnchanged = "nothing changed since the last version, so no new one was saved";

int backup_and_report(Repository& repo, BackupOptions o, const std::string& repo_location, jobs::RunRecord& record,
                      RepoLock& lock, VssMode vss, bool pause_sql_server, ChangeJournal* journal = nullptr) {
    // Never back up the repository into itself.
    if (!is_cloud_location(repo_location)) {
        const std::string repo_full = os::absolute_path(repo_location);
        o.excludes.push_back(repo_full);
        o.excludes.push_back(os::join(repo_full, "*"));
    }

    const auto start = std::chrono::steady_clock::now();
    record.started = os::now();
    o.on_progress = [&](const BackupProgress& p) {
        if (g_json) {
            emit({{"event", "progress"},
                  {"files", p.files},
                  {"dirs", p.dirs},
                  {"bytes", p.bytes},
                  {"new_bytes", p.new_bytes},
                  {"current", p.current}});
            return;
        }
        const double secs = std::max(seconds_since(start), 0.001);
        status(std::to_string(p.files) + " files, " + format_bytes(p.bytes) + " (" +
               format_bytes(static_cast<std::uint64_t>(static_cast<double>(p.bytes) / secs)) + "/s), new " +
               format_bytes(p.new_bytes) + "  " + p.current);
    };
    o.on_warning = [](const std::string& msg) { warning(msg); };
    o.should_cancel = [&lock] { return stop_requested() || lock.lost(); };
    o.before_save = [&lock] {
        if (!lock.held()) throw LockedError(kLockLost);
    };

    BackupResult r;
    std::unique_ptr<os::ShadowCopy> shadow;
    try {
        shadow = prepare_shadow_copy(vss, pause_sql_server, o);
        r = run_backup(repo, o);
    } catch (const Cancelled&) {
        record.finished = os::now();
        if (lock.lost()) {
            record.status = jobs::RunStatus::Failed;
            record.message = kLockLost;
            throw LockedError(kLockLost);
        }
        record.status = jobs::RunStatus::Cancelled;
        record.message = "cancelled";
        throw;
    } catch (const std::exception& e) {
        record.finished = os::now();
        record.status = jobs::RunStatus::Failed;
        record.message = e.what();
        throw;
    }
    const SnapshotStats& s = r.snapshot.stats;
    // Where the journal stands for the next run: only after a backup that read everything it
    // tried to (a file skipped now must be tried again, not taken as unchanged).
    if (journal != nullptr && s.errors == 0 && r.missing_sources.empty()) {
        if (!r.unchanged) journal->save(r.snapshot_id);
        else if (r.snapshot.parent) journal->save(*r.snapshot.parent);
    }
    if (r.dirs_reused > 0 && !g_json)
        out("Change journal: " + std::to_string(r.dirs_reused) + " unchanged folder(s) were not read again\n");
    else if (journal != nullptr && !journal->why_not().empty() && !g_json)
        out("Change journal: every folder was read (" + journal->why_not() + ")\n");
    record.finished = os::now();
    record.status = s.errors > 0 ? jobs::RunStatus::Partial : jobs::RunStatus::Success;
    record.snapshot = r.unchanged ? std::string() : id_hex(r.snapshot_id);
    record.files = s.files;
    record.bytes = s.bytes;
    record.new_bytes = s.new_bytes;
    record.stored_bytes = s.stored_bytes;
    record.errors = s.errors;
    record.message = s.errors > 0 ? std::to_string(s.errors) + " item(s) could not be read" : "";
    if (r.unchanged)
        record.message = std::string(kUnchanged) + (record.message.empty() ? "" : "; " + record.message);
    if (r.alarm.raised()) raise_alarm(repo, r, record);
    // A source that is gone (a detached disk, a locked volume, an unreachable share) or a sharp
    // drop in files makes a near-empty version; removing old versions by count or by day would
    // then throw away the good ones.
    if (!r.missing_sources.empty()) {
        record.hold = std::to_string(r.missing_sources.size()) + " backup path(s) could not be read at all (" +
                      r.missing_sources.front() + ")";
    } else if (r.alarm.parent_files >= 20 && s.files * 2 < r.alarm.parent_files) {
        record.hold = "only " + std::to_string(s.files) + " files were found, " + std::to_string(r.alarm.parent_files) +
                      " the time before";
    }
    if (!record.hold.empty()) warning(record.hold + "; old versions are kept this time");

    if (g_json) {
        emit({{"event", "done"},
              {"status", jobs::to_string(record.status)},
              {"snapshot", record.snapshot},
              {"parent", r.snapshot.parent ? id_hex(*r.snapshot.parent) : std::string()},
              {"files", s.files},
              {"dirs", s.dirs},
              {"bytes", s.bytes},
              {"new_bytes", s.new_bytes},
              {"stored_bytes", s.stored_bytes},
              {"skipped", s.skipped},
              {"errors", s.errors},
              {"alert", record.alert},
              {"unchanged", r.unchanged},
              {"seconds", seconds_since(start)}});
        return s.errors > 0 ? kExitPartial : kExitOk;
    }
    clear_status();
    if (r.unchanged)
        out("Nothing changed since snapshot " + short_id(*r.snapshot.parent) + "; no new snapshot was saved\n");
    else
        out("Snapshot " + short_id(r.snapshot_id) + " saved\n");
    out("  files:    " + std::to_string(s.files) + " in " + std::to_string(s.dirs) + " folders, " +
        format_bytes(s.bytes) + "\n");
    out("  new data: " + format_bytes(s.new_bytes) + ", stored " + format_bytes(s.stored_bytes) +
        " after compression and encryption\n");
    if (s.skipped > 0) out("  links/placeholders kept without content: " + std::to_string(s.skipped) + "\n");
    out("  time:     " + format_duration(seconds_since(start)) +
        (r.snapshot.parent ? " (compared with " + short_id(*r.snapshot.parent) + ")" : std::string(" (full)")) +
        "\n");
    if (s.errors > 0) {
        out("  " + std::to_string(s.errors) + " file(s) or folder(s) could not be read; see warnings above\n");
        return kExitPartial;
    }
    return kExitOk;
}

// How many worker threads compress and encrypt, and how many packs upload in the background:
// --threads / --uploads, or by default up to 4 threads (one core is left for the backup's own
// thread; none at background CPU priority or under a memory cap, where the work stays on one
// thread) and 2 uploads in flight for cloud storage, 1 for a folder (each holds a 32 MiB pack
// in memory; none at background priority for a folder, or under a memory cap).
// Returns how many files a backup reads, chunks and hashes at once (--read-threads): by default
// 4 (fewer on a PC with fewer cores), matching the seal threads, so neither side waits on the
// other on a fast disk; one at background priority or under a memory cap, like the sealing
// (each reader holds up to about 16 MiB: its read buffer and the chunks of two files).
unsigned apply_parallelism(Repository& repo, const Args& a, const std::string& location,
                           const ResourcePolicy& policy) {
    const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
    unsigned threads = std::min(4u, cores > 1 ? cores - 1 : 1u);
    unsigned readers = std::min(4u, cores);
    if (policy.cpu == CpuPriority::Idle || policy.max_memory != 0) {
        threads = 0;
        readers = 1;
    }
    // A folder too gets one pack written in the background while the next is built: writing
    // packs, not reading, is what a first backup to a disk waits on (about twice as fast).
    unsigned uploads = policy.max_memory != 0 ? 0 : is_cloud_location(location) ? 2
                                                 : policy.cpu == CpuPriority::Idle ? 0 : 1;
    if (auto v = a.value("--threads")) threads = static_cast<unsigned>(std::clamp(std::stoi(*v), 0, 16));
    if (auto v = a.value("--uploads")) uploads = static_cast<unsigned>(std::clamp(std::stoi(*v), 0, 8));
    if (auto v = a.value("--read-threads")) readers = static_cast<unsigned>(std::clamp(std::stoi(*v), 1, 16));
    repo.set_parallelism(threads, uploads);
    return readers;
}

// The virtual machine part of a backup (see ops/virt.hpp), set up before the files are read: WSL
// distributions become streamed exports, Hyper-V machines are exported into a staging folder that
// the backup then reads. Each one that cannot be backed up becomes an item that fails, so the run
// warns about it and old versions (which still have it) are kept. The staging folder of this run is
// removed when this goes away.
struct VirtualMachines {
    std::string staging_run;
    VirtualMachines() = default;
    VirtualMachines(const VirtualMachines&) = delete;
    VirtualMachines& operator=(const VirtualMachines&) = delete;
    ~VirtualMachines() {
        if (staging_run.empty()) return;
        std::error_code ec;
        std::filesystem::remove_all(std::filesystem::path(std::u8string(staging_run.begin(), staging_run.end())), ec);
        if (ec) warning("could not remove the exported virtual machines in " + staging_run + ": " + ec.message());
    }
};

// The Hyper-V export folder inside a folder the job names: that folder stays the user's (it is
// never made administrator-only or moved aside, and may be a drive's root); the exports go into
// this administrator-only folder made in it.
std::string hyperv_export_root(const std::string& staging) {
    return staging.empty() ? default_hyperv_staging() : os::join(staging, "Vaultik Hyper-V export");
}

// The folder of one run's exports: "run-<process id>-<random>", so a later run can tell that the
// run which made it has ended.
std::string hyperv_run_folder(const std::string& root) {
    std::uint8_t rnd[6];
    random_bytes(rnd);
    return os::join(root, "run-" + std::to_string(os::process_id()) + "-" + to_hex(rnd));
}

// Removes what runs that ended without cleaning up left in the export folder (full copies of
// virtual machines or database dumps, which only fill the drive). `what` names them.
void remove_leftover_exports(const std::string& root, const std::string& what = "exported virtual machines") {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir(std::u8string(root.begin(), root.end()));
    const auto now = fs::file_time_type::clock::now();
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::u8string n = it->path().filename().u8string();
        const std::string name(n.begin(), n.end());
        std::error_code tec;
        const auto when = fs::last_write_time(it->path(), tec);
        if (tec) continue;
        const auto age = std::chrono::duration_cast<std::chrono::hours>(now - when);
        if (!it->is_directory(tec) || it->is_symlink(tec) ||
            !hyperv_run_left_over(name, age, os::process_id(), os::process_alive))
            continue;
        std::error_code rec;
        fs::remove_all(it->path(), rec);
        if (rec) warning("could not remove the " + what + " left in " + os::join(root, name) + ": " + rec.message());
        else if (!g_json) err("Removed " + what + " left by an earlier run: " + os::join(root, name) + "\n");
    }
}

ExtraSource failed_extra(std::vector<std::string> place, const std::string& why) {
    ExtraSource x;
    x.place = std::move(place);
    x.open = [why]() -> std::unique_ptr<StreamSource> { throw Error(why); };
    return x;
}

std::unique_ptr<VirtualMachines> add_virtual_machines(BackupOptions& o, const std::vector<std::string>& distros,
                                                      WslWhenRunning when, const std::vector<std::string>& machines,
                                                      const std::string& staging) {
    auto vm = std::make_unique<VirtualMachines>();
    if (!distros.empty()) {
        const std::vector<WslDistro> installed = wsl_distros();
        std::vector<std::string> running;
        const bool any_installed = std::any_of(installed.begin(), installed.end(), [&](const WslDistro& d) {
            return std::any_of(distros.begin(), distros.end(),
                               [&](const std::string& n) { return os::fold_case(n) == os::fold_case(d.name); });
        });
        if (any_installed) {
            try {
                running = wsl_running();
            } catch (const std::exception& e) {
                // Not knowing counts as running: never export one as it runs unless the job allows it.
                warning(std::string("could not tell which WSL distributions are running: ") + e.what());
                running = distros;
            }
        }
        const WslPlan plan = plan_wsl(distros, installed, running, when);
        std::vector<std::string> disks;
        for (const std::string& want : distros)
            for (const WslDistro& d : installed)
                if (os::fold_case(d.name) == os::fold_case(want) && !d.disk.empty()) disks.push_back(d.disk);
        for (const std::string& ex : disk_excludes(disks)) o.excludes.push_back(ex);
        // One that is not installed any more is only mentioned; a running one is an item that could
        // not be read this time.
        for (const std::string& name : plan.not_installed)
            warning("the WSL distribution " + name + " is not installed (any more), so it was left out");
        for (const auto& [name, why] : plan.running_skipped) o.extras.push_back(failed_extra(wsl_place(name), why));
        for (const std::string& name : plan.export_names) {
            if (std::find(plan.stop_first.begin(), plan.stop_first.end(), name) != plan.stop_first.end()) {
                if (!g_json) status("Stopping the WSL distribution " + name + "...");
                try {
                    wsl_terminate(name);
                } catch (const std::exception& e) {
                    o.extras.push_back(failed_extra(wsl_place(name), e.what()));
                    continue;
                }
                if (!g_json) clear_status();
                if (!g_json) err("Stopped the WSL distribution " + name + " for its backup\n");
            }
            o.extras.push_back(wsl_export_source(name, [] { return stop_requested(); }));
        }
    }
    if (!machines.empty()) {
        if (!os::process_elevated()) {
            for (const std::string& m : machines)
                o.extras.push_back(failed_extra(hyperv_place(m), "backing up a Hyper-V virtual machine needs administrator rights"));
            return vm;
        }
        const HyperVList list = hyperv_machines();
        if (!list.available) {
            for (const std::string& m : machines) o.extras.push_back(failed_extra(hyperv_place(m), list.problem));
            return vm;
        }
        const std::string root = hyperv_export_root(staging);
        if (!staging.empty()) os::make_dirs(staging);
        // Only administrators can read the exported disks (they hold all of the machine's data).
        os::make_admin_dir(root);
        remove_leftover_exports(root);
        vm->staging_run = hyperv_run_folder(root);
        o.excludes.push_back(root);  // a file backup of the drive does not read the exports twice
        o.excludes.push_back(os::join(root, "*"));
        std::size_t index = 0;
        for (const std::string& name : machines) {
            const auto it = std::find_if(list.machines.begin(), list.machines.end(),
                                         [&](const HyperVm& m) { return m.name == name; });
            if (it == list.machines.end()) {
                o.extras.push_back(failed_extra(hyperv_place(name), "there is no Hyper-V virtual machine called " + name));
                continue;
            }
            for (const std::string& ex : disk_excludes(it->disks)) o.excludes.push_back(ex);
            if (stop_requested()) throw Cancelled();
            if (!g_json) status("Exporting the virtual machine " + name + " (this can take a while)...");
            try {
                const std::string folder = hyperv_export(name, os::join(vm->staging_run, std::to_string(++index)),
                                                         [] { return stop_requested(); });
                ExtraSource x;
                x.place = hyperv_place(name);
                x.folder = folder;
                o.extras.push_back(std::move(x));
            } catch (const std::exception& e) {
                o.extras.push_back(failed_extra(hyperv_place(name), std::string("Export-VM failed: ") + e.what()));
            }
            if (!g_json) clear_status();
        }
    }
    return vm;
}

// Docker volumes: each chosen one is exported through Docker as the backup reads it. One that
// cannot be (Docker is not running, no such volume) becomes an item that fails, like a machine.
void add_docker_volumes(BackupOptions& o, const std::vector<std::string>& volumes, const std::string& image) {
    if (volumes.empty()) return;
    const DockerInfo info = docker_info();
    if (!info.available) {
        for (const std::string& v : volumes)
            o.extras.push_back(failed_extra(volume_place(v), info.problem + ", so the Docker volume " + v +
                                                                 " could not be backed up"));
        return;
    }
    std::vector<DockerVolume> all;
    std::string helper;
    try {
        all = docker_volumes(info);
        helper = pick_helper_image(docker_images(info), info.os_type, image);
    } catch (const std::exception& e) {
        for (const std::string& v : volumes) o.extras.push_back(failed_extra(volume_place(v), e.what()));
        return;
    }
    for (const std::string& name : volumes) {
        const auto it = std::find_if(all.begin(), all.end(), [&](const DockerVolume& v) { return v.name == name; });
        if (it == all.end()) {
            o.extras.push_back(failed_extra(volume_place(name), "there is no Docker volume called " + name));
            continue;
        }
        const std::string in_use = volume_in_use_warning(*it);
        if (!in_use.empty()) warning(in_use);
        o.extras.push_back(docker_volume_source(info, name, helper, [] { return stop_requested(); }));
    }
}

// The database dumps of one run, in a private folder of the account's own, removed when this
// goes away (after the backup read them).
struct DatabaseDumps {
    std::string run;
    DatabaseDumps() = default;
    DatabaseDumps(const DatabaseDumps&) = delete;
    DatabaseDumps& operator=(const DatabaseDumps&) = delete;
    ~DatabaseDumps() {
        if (run.empty()) return;
        std::error_code ec;
        std::filesystem::remove_all(std::filesystem::path(std::u8string(run.begin(), run.end())), ec);
        if (ec) warning("could not remove the database dumps in " + run + ": " + ec.message());
    }
};

// Dumps each database now, before the backup, and adds the dumps to it. A dump that fails is an
// item that fails (the run warns and keeps the old versions), like a machine that cannot be
// exported; the other dumps and the files are still backed up.
std::unique_ptr<DatabaseDumps> add_database_dumps(BackupOptions& o, const std::vector<DatabaseDump>& dbs,
                                                  const std::function<std::string(const DatabaseDump&)>& password_of) {
    auto dumps = std::make_unique<DatabaseDumps>();
    if (dbs.empty()) return dumps;
    // The account's own folder, not the shared temporary folder: there another account could make
    // the folder first (and read the dumps, or keep every backup from making it).
    const std::string root = os::make_work_dir("databases");
    remove_leftover_exports(root, "database dumps");
    dumps->run = hyperv_run_folder(root);
    os::make_private_dirs(dumps->run);
    o.excludes.push_back(root);  // a file backup of the temporary folder does not read them twice
    o.excludes.push_back(os::join(root, "*"));
    std::size_t index = 0;
    for (const DatabaseDump& db : dbs) {
        if (stop_requested()) throw Cancelled();
        if (!g_json) status("Dumping the database " + db.database + "...");
        std::string password = password_of(db);
        try {
            const std::string file = run_dump(db, password, os::join(dumps->run, std::to_string(++index)),
                                              [] { return stop_requested(); });
            o.extras.push_back(file_stream_source(database_place(db), file));
            if (!g_json) {
                clear_status();
                err("Dumped " + describe(db) + "\n");
            }
        } catch (const std::exception& e) {
            secure_wipe(password.data(), password.size());
            if (stop_requested()) throw Cancelled();
            if (!g_json) clear_status();
            o.extras.push_back(failed_extra(database_place(db), "the dump of " + describe(db) + " failed: " + e.what()));
            continue;
        }
        secure_wipe(password.data(), password.size());
    }
    return dumps;
}

// The databases given on the command line (backup --database), with VAULTIK_DB_PASSWORD.
std::vector<DatabaseDump> database_args(const Args& a) {
    std::vector<DatabaseDump> dbs;
    for (const std::string& spec : a.all("--database")) dbs.push_back(parse_database_spec(spec));
    check_databases(dbs);
    return dbs;
}

int cmd_backup(const Args& a) {
    const std::vector<std::string> distros = a.all("--wsl");
    const std::vector<std::string> machines = a.all("--hyperv");
    const std::vector<std::string> volumes = a.all("--docker-volume");
    const std::vector<DatabaseDump> databases = database_args(a);
    if (a.positional.empty() && distros.empty() && machines.empty() && volumes.empty() && databases.empty())
        throw Error("usage: vaultik backup -r <repo> <path>...");
    for (const std::string& v : volumes)
        if (!valid_volume_name(v)) throw Error("invalid Docker volume name \"" + v + "\"");
    if (const auto image = a.value("--docker-image"); image && !image->empty() && !valid_image_reference(*image))
        throw Error("invalid Docker image name \"" + *image + "\"");
    const WslWhenRunning when = parse_wsl_when_running(a.value("--wsl-running").value_or("skip"));
    for (const std::string& d : distros)
        if (!valid_distro_name(d)) throw Error("invalid WSL distribution name \"" + d + "\"");
    for (const std::string& m : machines)
        if (!valid_vm_name(m)) throw Error("invalid virtual machine name \"" + m + "\"");
    auto repo = open_repo(a);
    const unsigned readers = apply_parallelism(*repo, a, repo_path(a), resource_policy(a));
    RepoLock lock(*repo, RepoLock::Mode::Shared, "backup", lock_wait(a));
    repo->reload_index();  // a prune may have finished before the lock was granted

    BackupOptions o;
    o.paths = a.positional;
    o.read_threads = readers;
    o.excludes = a.all("--exclude");
    o.tags = a.all("--tag");
    o.use_parent = !a.has("--no-parent");
    o.skip_unchanged = a.has("--skip-unchanged");
    o.check_every_file = a.has("--check-every-file");
    if (auto p = a.value("--parent")) o.parent = resolve_snapshot(*repo, *p).id;
    if (a.has("--apps")) o.app_list = app_list_in_background();
    const auto machines_guard = add_virtual_machines(o, distros, when, machines, a.value("--hyperv-staging").value_or(""));
    const auto dumps_guard = add_database_dumps(o, databases, [](const DatabaseDump&) {
        return os::get_env("VAULTIK_DB_PASSWORD");
    });
    add_docker_volumes(o, volumes, a.value("--docker-image").value_or(""));
    jobs::RunRecord record;
    return backup_and_report(*repo, std::move(o), repo_path(a), record, lock, vss_arg(a, kDefaultVss),
                             !a.has("--vss-skip-sql"));
}

int cmd_jobs(const Args& /*a*/) {
    const jobs::JobStore store;
    const auto all = store.list();
    if (g_json) {
        nlohmann::json list = nlohmann::json::array();
        for (const auto& j : all) list.push_back(nlohmann::json::parse(jobs::to_json(j)));
        emit({{"event", "jobs"}, {"jobs", list}});
        return kExitOk;
    }
    if (all.empty()) {
        out("No saved jobs. Create one in vaultik-gui.\n");
        return kExitOk;
    }
    out(pad("ID", 24) + pad("Schedule", 26) + pad("Last run", 30) + "Repository\n");
    for (const auto& j : all) {
        const auto h = jobs::combined_history(store, j.id, 1);
        const std::string last =
            h.empty() ? "never" : os::format_time_local(h.back().started) + " " + jobs::to_string(h.back().status);
        out(pad(j.id, 24) + pad(jobs::describe(j.schedule), 26) + pad(last, 30) + j.repository + "\n");
    }
    return kExitOk;
}

int cmd_schedule(const Args& a, bool install) {
    if (a.positional.empty()) throw Error(std::string("usage: vaultik ") + (install ? "schedule" : "unschedule") + " <job>");
    const auto job = jobs::JobStore().find(a.positional[0]);
    if (!job) throw Error("no saved job called " + a.positional[0] + " (see vaultik jobs)");
    if (!install || !jobs::has_system_schedule(job->schedule)) {
        jobs::remove_schedule(job->id);
        out("Job \"" + job->name + "\" has no system schedule now.\n");
        return kExitOk;
    }
    jobs::apply_schedule(*job, os::parent_of(os::executable_path()));
    out("Job \"" + job->name + "\" scheduled: " + jobs::describe(job->schedule) + "\n");
    return kExitOk;
}

// Pauses or resumes a job: it keeps its settings and schedule, but while paused nothing starts it
// on its own. Saved like an edit of the job, then its system schedule is applied (removed while
// paused; a job with administrator rights asks for approval as when it is saved). When that
// fails the job is saved back as it was (see jobs::set_paused).
int cmd_pause_job(const Args& a, bool pause) {
    if (a.positional.empty()) throw Error(std::string("usage: vaultik ") + (pause ? "pause-job" : "resume-job") + " <job>");
    const jobs::JobStore store;
    auto job = store.find(a.positional[0]);
    if (!job) throw Error("no saved job called " + a.positional[0] + " (see vaultik jobs)");
    if (job->schedule.paused == pause) {
        out("Job \"" + job->name + "\" is " + (pause ? "already paused" : "not paused") + ".\n");
        return kExitOk;
    }
    const std::string program_dir = os::parent_of(os::executable_path());
    jobs::set_paused(store, *job, pause, [&](const jobs::Job& j) { jobs::apply_schedule(j, program_dir); });
    out("Job \"" + job->name + "\" " + (pause ? "paused" : "resumed") + ": " + jobs::describe(job->schedule) + "\n");
    return kExitOk;
}

// The password of a settings file: --password-file, VAULTIK_SETTINGS_PASSWORD, or asked for
// (twice when it is made).
std::string settings_password(const Args& a, bool confirm) {
    if (auto file = a.value("--password-file")) {
        std::string pw = read_small_file(*file, "password file");
        while (!pw.empty() && (pw.back() == '\n' || pw.back() == '\r')) pw.pop_back();
        if (pw.empty()) throw Error("password file is empty");
        return pw;
    }
    const std::string env = os::get_env("VAULTIK_SETTINGS_PASSWORD");
    if (!env.empty()) return env;
    std::string pw = read_password("Password for the settings file: ",
                                   "no console to ask for the password; set VAULTIK_SETTINGS_PASSWORD");
    if (confirm) {
        if (pw.size() < 8) throw Error("use a password of at least 8 characters");
        std::string again = read_password("Repeat the password: ");
        const bool same = again == pw;
        secure_wipe(again.data(), again.size());
        if (!same) throw Error("passwords do not match");
    }
    if (pw.empty()) throw Error("empty password");
    return pw;
}

// Settings and plans to another computer: one file with every saved job and the alert settings
// (the frontend adds its own settings when it exports), secrets only with --with-secrets and
// then encrypted with a password.
int cmd_settings(const Args& a) {
    const std::string sub = a.positional.empty() ? std::string() : a.positional[0];
    const jobs::JobStore store;
    if (sub == "export") {
        const bool secrets = a.has("--with-secrets");
        std::string path = a.positional.size() >= 2 ? a.positional[1]
                                                    : jobs::default_settings_file_name(os::hostname(), os::now());
        path = os::absolute_path(path);
        if (os::exists(path) && !a.has("--force")) throw Error(path + " exists already (--force replaces it)");
        const jobs::SettingsBundle bundle = jobs::collect_settings(store, secrets);
        std::string pw = secrets ? settings_password(a, true) : std::string();
        Bytes data;
        try {
            data = jobs::write_settings_file(bundle, pw);
        } catch (...) {
            secure_wipe(pw.data(), pw.size());
            throw;
        }
        secure_wipe(pw.data(), pw.size());
        os::write_file_atomic(path, data, 0600);
        const std::string msg = "Exported " + std::to_string(bundle.jobs.size()) + " job(s) to " + path +
                                (secrets ? " (with passwords and keys, encrypted)" : " (without passwords and keys)");
        if (g_json) emit({{"event", "done"}, {"status", "success"}, {"file", path}, {"message", msg}});
        else out(msg + "\n");
        return kExitOk;
    }
    if (sub == "import") {
        if (a.positional.size() < 2) throw Error("usage: vaultik settings import <file> [--jobs a,b] [--replace] [--alerts] [--keep-commands]");
        const Bytes data = os::read_file(os::absolute_path(a.positional[1]), 64u << 20);
        std::string pw = jobs::settings_file_encrypted(data) ? settings_password(a, false) : std::string();
        jobs::SettingsBundle bundle;
        try {
            bundle = jobs::read_settings_file(data, pw);
        } catch (...) {
            secure_wipe(pw.data(), pw.size());
            throw;
        }
        secure_wipe(pw.data(), pw.size());
        jobs::ImportChoice choice;
        choice.replace = a.has("--replace");
        choice.alerts = a.has("--alerts");
        choice.keep_commands = a.has("--keep-commands");
        if (auto list = a.value("--jobs")) {
            std::size_t at = 0;
            while (at <= list->size()) {
                const std::size_t comma = list->find(',', at);
                std::string id = list->substr(at, comma == std::string::npos ? std::string::npos : comma - at);
                while (!id.empty() && id.back() == ' ') id.pop_back();
                while (!id.empty() && id.front() == ' ') id.erase(0, 1);
                if (!id.empty()) choice.jobs.push_back(id);
                if (comma == std::string::npos) break;
                at = comma + 1;
            }
        }
        if (!g_json) {
            out("The file was made on " + (bundle.computer.empty() ? std::string("another computer") : bundle.computer) +
                (bundle.created ? " on " + os::format_time_local(bundle.created) : std::string()) + " and has these jobs:\n");
            for (const jobs::Job& j : bundle.jobs) {
                out("  " + pad(j.id, 24) + pad(j.name, 28) + j.repository + "\n");
                if (!j.before_command.empty()) out("      runs before the backup: " + j.before_command + "\n");
                if (!j.after_command.empty()) out("      runs after the backup: " + j.after_command + "\n");
                if (j.run_as_admin) out("      runs with administrator rights\n");
            }
        }
        const jobs::ImportResult r = jobs::import_settings(store, bundle, choice);
        // Paused, so this removes the tasks of a job that was replaced (and sets up nothing new).
        bool schedule_failed = false;
        for (const jobs::ImportedJob& i : r.jobs) {
            if (const auto job = store.find(i.id)) {
                try {
                    jobs::apply_schedule(*job, os::parent_of(os::executable_path()));
                } catch (const Error& e) {
                    if (i.replaced) schedule_failed = true;
                    warning("the schedule of " + i.name + " could not be changed, so it may still run on its own (" +
                            e.what() + "); pause it with vaultik pause-job " + i.id);
                }
            }
        }
        if (g_json) {
            nlohmann::json list = nlohmann::json::array();
            for (const jobs::ImportedJob& i : r.jobs)
                list.push_back({{"from", i.from_id}, {"id", i.id}, {"name", i.name}, {"replaced", i.replaced},
                                {"renamed", i.renamed}, {"secrets", i.secrets}, {"stripped", i.stripped}});
            emit({{"event", "done"}, {"status", "success"}, {"jobs", list}, {"alerts", r.alerts},
                  {"schedule_failed", schedule_failed},
                  {"message", "imported " + std::to_string(r.jobs.size()) + " job(s)"}});
            return kExitOk;
        }
        for (const jobs::ImportedJob& i : r.jobs) {
            std::string how = i.replaced ? "replaced the job of that id" : i.renamed ? "imported as " + i.id + " (the id was taken)" : "imported";
            if (!i.secrets) how += "; its password is asked for when it first runs";
            out("  " + i.name + ": " + how + "\n");
            for (const std::string& left : i.stripped) out("      left out: " + left + "\n");
        }
        if (std::any_of(r.jobs.begin(), r.jobs.end(), [](const jobs::ImportedJob& i) { return !i.stripped.empty(); }))
            out("Commands and administrator rights were left out (they would run on this computer); import again\n"
                "with --keep-commands for a file you made yourself, or set them in the job.\n");
        out("Imported " + std::to_string(r.jobs.size()) + " job(s), all paused: check their folders on this computer, then\n"
            "resume them (vaultik resume-job <job>, or Resume schedule in Vaultik).\n");
        if (r.alerts) out("The alert settings were imported too.\n");
        else if (bundle.alerts) out("The alert settings in the file were not imported (--alerts imports them).\n");
        if (!bundle.app.empty()) out("The frontend's own settings in the file are imported when the file is imported in Vaultik.\n");
        return kExitOk;
    }
    throw Error("usage: vaultik settings export [<file>] [--with-secrets] | import <file> [--jobs a,b] [--replace] [--alerts] "
                "[--keep-commands]");
}

// After a job's backup: removes its snapshots beyond its retention, then the data nothing uses any
// more. Never fails the run; a problem becomes a warning and a note for the history.
std::string clean_up(Repository& repo, const jobs::Job& job, bool* pruned = nullptr) {
    try {
        // Exclusive, without waiting: if another run is busy, cleaning up can wait for next time.
        RepoLock lock(repo, RepoLock::Mode::Exclusive, "cleanup (job " + job.id + ")");
        if (!g_json) status("Removing old snapshots...");
        std::size_t removed = 0;
        if (!job.retention.no_rules()) {
            ForgetOptions fo;
            fo.policy = job.retention;
            fo.tags = {"job:" + job.id};
            removed += run_forget(repo, fo).removed;
        }
        std::string size_note;
        if (job.retention.max_bytes > 0) {
            // The oldest unpinned versions beyond the size limit (never the newest).
            if (!g_json) status("Checking the size limit...");
            const SizePlan plan =
                plan_size_limit(repo, load_snapshot_records(repo), "job:" + job.id, job.retention.max_bytes);
            removed += remove_snapshots(repo, plan.remove);
            if (!plan.fits) {
                size_note = "the backup is " + format_bytes(plan.size_after) + ", over its " +
                            format_bytes(job.retention.max_bytes) + " limit even with only the newest and pinned versions";
                warning(size_note);
            }
        }
        if (!g_json) clear_status();
        if (removed == 0) return size_note;
        if (pruned != nullptr) *pruned = true;
        const PruneResult pr = prune_and_report(repo, lock, false);
        std::string note = "removed " + std::to_string(removed) + " old snapshot(s), freed " +
                           format_bytes(space_freed(pr));
        if (!size_note.empty()) note += "; " + size_note;
        if (g_json) {
            emit({{"event", "cleanup"}, {"removed", removed}, {"freed", space_freed(pr)}, {"message", note}});
        } else {
            out("Cleanup: " + note + " (keeping " + describe(job.retention) + ")\n");
        }
        return note;
    } catch (const LockedError&) {
        const std::string note = "old snapshots were not removed this time: another run is using the repository";
        warning(note);
        return note;
    } catch (const std::exception& e) {
        if (!g_json) clear_status();
        const std::string note = std::string("removing old snapshots failed: ") + e.what();
        warning(note);
        return note;
    }
}

// "1.10.2" -> {1, 10, 2}; missing or bad parts are 0.
std::array<int, 3> version_parts(const std::string& v) {
    std::array<int, 3> out{};
    const char* p = v.c_str();
    for (int& part : out) {
        char* end = nullptr;
        part = static_cast<int>(std::strtol(p, &end, 10));
        if (end == p || *end != '.') break;
        p = end + 1;
    }
    return out;
}

// After a backup to an external drive: keeps <drive>\Vaultik\ with the portable programs and a
// note on restoring, so another computer can restore from the drive with nothing installed. Only
// replaces an older copy. Never fails the run.
void refresh_drive_copy(const jobs::Job& job) {
#ifdef _WIN32
    if (is_cloud_location(job.repository)) return;
    try {
        // Never with administrator rights: anyone can prepare an external drive, e.g. with
        // "Vaultik" as a link into Windows' own folders, and an elevated run would write there.
        if (os::process_elevated()) return;
        const os::VolumeLocation loc = os::volume_of(job.repository);
        if (loc.volume.empty() || loc.system || !loc.external) return;
        const std::string target = os::join(loc.root, "Vaultik");
        // Only a real folder and real files there (or none yet): links lead somewhere else. Any
        // reparse point counts as a link, also one whose kind cannot be read (fail closed). What
        // is left is a race with someone changing the drive during this user's own backup, who
        // could then only write where this user can anyway.
        constexpr std::uint32_t kReparsePoint = 0x400;  // FILE_ATTRIBUTE_REPARSE_POINT
        const auto plain = [](const std::string& path, os::EntryType want) {
            os::FileInfo info;
            os::ErrorCode err = 0;
            if (!os::stat(path, info, err)) return os::is_not_found(err);
            return info.type == want && (info.attributes & kReparsePoint) == 0;
        };
        const char* const names[] = {"vaultik.exe", "vaultik-gui.exe", "vaultik-task.exe", "portable",
                                     "How to restore.txt", "version"};
        bool safe = plain(target, os::EntryType::Dir);
        for (const char* name : names) safe = safe && plain(os::join(target, name), os::EntryType::File);
        if (!safe) {
            warning(target + " on the backup drive is not a plain folder of Vaultik's files, so it was left alone");
            return;
        }
        const std::string from = os::parent_of(os::executable_path());
        if (os::fold_case(from) == os::fold_case(os::absolute_path(target))) return;  // running from that copy
        const std::string stamp = os::join(target, "version");
        std::string there;
        try {
            const Bytes b = os::read_file(stamp, 64);
            there.assign(b.begin(), b.end());
        } catch (const Error&) {
        }
        if (!there.empty() && version_parts(there) >= version_parts(VAULTIK_VERSION)) return;

        os::make_dirs(target);
        const auto fs_path = [](const std::string& s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); };
        for (const char* exe : {"vaultik.exe", "vaultik-gui.exe", "vaultik-task.exe"}) {
            const std::string src = os::join(from, exe);
            if (!os::exists(src)) continue;
            // Deleted first, then created: overwriting would write through a hard link someone
            // put there in place of the old file, into whatever file it is linked to.
            const std::string dest = os::join(target, exe);
            const os::ErrorCode gone = os::remove_file(dest);
            if (gone != 0 && !os::is_not_found(gone)) os::throw_error("cannot replace " + dest, gone);
            std::filesystem::copy_file(fs_path(src), fs_path(dest), std::filesystem::copy_options::none);
        }
        const auto write = [&](const char* name, const std::string& text) {
            os::write_file_atomic(os::join(target, name),
                                  ByteSpan(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
        };
        write("portable",
              "This file makes this copy of Vaultik portable: its settings are kept in the Settings folder next to "
              "it, and nothing is written to the computer it runs on.\r\n");
        const std::string where = loc.relative.empty() ? std::string("the top of this drive") : loc.relative;
        write("How to restore.txt",
              "Restoring files from this drive on any Windows PC\r\n\r\n"
              "1. Start vaultik-gui.exe in this folder (nothing needs to be installed).\r\n"
              "2. Choose \"Restore from another backup...\" and select the folder " + where + " on this drive.\r\n"
              "3. Enter the backup's password (or use its recovery key), pick a version and restore.\r\n\r\n"
              "Vaultik keeps this folder up to date after each backup to this drive.\r\n");
        write("version", VAULTIK_VERSION);
        if (!g_json) out("Updated the Vaultik copy on the backup drive (" + target + ").\n");
    } catch (const std::exception& e) {
        warning(std::string("could not update the Vaultik copy on the backup drive: ") + e.what());
    }
#else
    // Linux: only an AppImage can be copied as one file. Next to the Windows programs, in the
    // same <drive>/Vaultik folder, with its own note.
    const std::string appimage = os::get_env("APPIMAGE");
    if (appimage.empty() || is_cloud_location(job.repository) || ::geteuid() == 0) return;
    try {
        const os::VolumeLocation loc = os::volume_of(job.repository);
        if (loc.volume.empty() || loc.system || !loc.external) return;
        const std::string target = os::join(loc.root, "Vaultik");
        // Only a real folder and real files there (or none yet): a link leads somewhere else.
        const auto plain = [](const std::string& path, os::EntryType want) {
            os::FileInfo info;
            os::ErrorCode err = 0;
            if (!os::stat(path, info, err)) return os::is_not_found(err);
            return info.type == want;
        };
        constexpr const char* kImage = "Vaultik-x86_64.AppImage";
        constexpr const char* kNote = "How to restore on Linux.txt";
        constexpr const char* kStamp = "version-linux";
        bool safe = plain(target, os::EntryType::Dir);
        for (const char* name : {kImage, kNote, kStamp, "portable"}) safe = safe && plain(os::join(target, name), os::EntryType::File);
        if (!safe) {
            warning(target + " on the backup drive is not a plain folder of Vaultik's files, so it was left alone");
            return;
        }
        if (os::absolute_path(appimage) == os::join(target, kImage)) return;  // running from that copy
        std::string there;
        try {
            const Bytes b = os::read_file(os::join(target, kStamp), 64);
            there.assign(b.begin(), b.end());
        } catch (const Error&) {
        }
        if (!there.empty() && version_parts(there) >= version_parts(VAULTIK_VERSION)) return;

        os::make_dirs(target);
        const auto fs_path = [](const std::string& s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); };
        // Deleted first, then created: overwriting would write through a hard link.
        const std::string dest = os::join(target, kImage);
        const os::ErrorCode gone = os::remove_file(dest);
        if (gone != 0 && !os::is_not_found(gone)) os::throw_error("cannot replace " + dest, gone);
        std::filesystem::copy_file(fs_path(appimage), fs_path(dest), std::filesystem::copy_options::none);
        std::filesystem::permissions(fs_path(dest), std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec |
                                                       std::filesystem::perms::others_exec,
                                     std::filesystem::perm_options::add);
        const auto write = [&](const char* name, const std::string& text) {
            os::write_file_atomic(os::join(target, name),
                                  ByteSpan(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
        };
        if (!os::exists(os::join(target, "portable")))
            write("portable",
                  "This file makes this copy of Vaultik portable: its settings are kept in the Settings folder "
                  "(Settings-Linux for the Linux program) next to it. The Linux program keeps only the key that "
                  "unlocks saved passwords on the computer it runs on (~/.config/vaultik/secrets), so this drive "
                  "alone does not give them away.\n");
        const std::string where = loc.relative.empty() ? std::string("the top of this drive") : loc.relative;
        write(kNote,
              "Restoring files from this drive on Linux (also SteamOS on a Steam Deck)\n\n"
              "1. Start Vaultik-x86_64.AppImage in this folder (nothing needs to be installed; on SteamOS switch to\n"
              "   Desktop Mode first). If it does not start, allow it to run as a program in its properties, or\n"
              "   run: chmod +x Vaultik-x86_64.AppImage\n"
              "2. Choose \"Restore from another backup...\" and select the folder " + where + " on this drive.\n"
              "3. Enter the backup's password (or use its recovery key), pick a version and restore.\n\n"
              "The drive's file system must allow programs to run from it (exFAT and ext4 drives mounted by the\n"
              "desktop do; a drive mounted with \"noexec\" does not: copy the AppImage to your home folder then).\n"
              "Vaultik keeps this file up to date after each backup to this drive.\n");
        write(kStamp, VAULTIK_VERSION);
        if (!g_json) out("Updated the Vaultik copy on the backup drive (" + target + ").\n");
    } catch (const std::exception& e) {
        warning(std::string("could not update the Vaultik copy on the backup drive: ") + e.what());
    }
#endif
}

// "uninstall-cleanup": run by the installer when Vaultik is removed; deletes every Vaultik task
// from Task Scheduler (all accounts' jobs; it runs with administrator rights). Jobs, history and
// backups stay.
int cmd_uninstall_cleanup() {
#ifdef _WIN32
    std::string list;
    os::run_command({"schtasks.exe", "/Query", "/FO", "CSV", "/NH"}, &list);
    std::set<std::string> tasks;
    std::size_t pos = 0;
    while ((pos = list.find("\"\\Vaultik\\", pos)) != std::string::npos) {
        const std::size_t end = list.find('"', pos + 1);
        if (end == std::string::npos) break;
        tasks.insert(list.substr(pos + 1, end - pos - 1));
        pos = end + 1;
    }
    for (const std::string& t : tasks) {
        std::string ignored;
        if (os::run_command({"schtasks.exe", "/Delete", "/TN", t, "/F"}, &ignored) == 0) out("removed " + t + "\n");
    }
#endif
    return kExitOk;
}

// The approved step behind saving or removing a job with administrator rights (see
// admin_jobs.hpp): "admin-job install|remove <id> --user <sid> [--from <config dir>]". Started by
// the frontend or `schedule` through a Windows approval prompt, without a visible console, so a
// failure is also written where the caller can read it.
int cmd_admin_job(const Args& a) {
    // gui-on|gui-off: the task behind "Start Vaultik as administrator" (no job id).
    const bool gui = !a.positional.empty() && (a.positional[0] == "gui-on" || a.positional[0] == "gui-off");
    if (a.positional.size() != (gui ? 1u : 2u) || !a.value("--user"))
        throw Error("usage: vaultik admin-job install|remove <job id> --user <account SID> [--from <folder>]\n"
                    "       vaultik admin-job gui-on|gui-off --user <account SID>");
    const std::string& verb = a.positional[0];
    const std::string id = gui ? std::string() : a.positional[1];
    const std::string sid = *a.value("--user");
    std::string error_file;
    try {
        error_file = jobs::admin_error_file(sid);
        // Only in a store that is already protected: elsewhere the file could be a planted link.
        std::string ignored;
        if (!os::admin_only(os::parent_of(error_file), false, &ignored)) error_file.clear();
        if (!error_file.empty()) os::remove_file(error_file);
    } catch (const Error&) {
        error_file.clear();
    }
    try {
        if (verb == "install") {
            const auto from = a.value("--from");
            if (!from) throw Error("admin-job install needs --from <the account's Vaultik settings folder>");
            jobs::install_admin_job(jobs::JobStore(*from), id, sid);
        } else if (verb == "remove") {
            jobs::remove_admin_job(id, sid);
        } else if (verb == "gui-on") {
            jobs::install_elevated_gui(sid);
        } else if (verb == "gui-off") {
            jobs::remove_elevated_gui(sid);
        } else {
            throw Error("admin-job takes install, remove, gui-on or gui-off");
        }
    } catch (const std::exception& e) {
        if (error_file.empty()) {
            try {
                // The store may have been created (and so protected) during the failed step.
                std::string ignored;
                const std::string f = jobs::admin_error_file(sid);
                if (os::admin_only(os::parent_of(f), false, &ignored)) error_file = f;
            } catch (const Error&) {
            }
        }
        if (!error_file.empty()) {
            try {
                const std::string text = e.what();
                os::write_file_atomic(error_file, ByteSpan(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
            } catch (const Error&) {
            }
        }
        throw;
    }
    return kExitOk;
}

// Reads a job's stored data back (all of it for a full check, else the job's weekly share) and
// checks every version's structure, under a shared lock so backups of other jobs can go on.
// Fills `record` (kind, times, status, packs read as files, bytes read, problems as errors).
// Returns kExitOk, or kExitAuth when damage was found; throws on cancel or when the check could
// not run, after filling the record.
// The silent damage part of a health check (ops/rot_check.hpp): a few files of the job's newest
// version made on this computer are read back from this computer and compared with it. Fills the
// record's rot fields; returns a sentence for its message (empty when nothing was compared).
std::string rot_check(Repository& repo, const jobs::Job& job, jobs::RunRecord& record, const std::function<bool()>& cancel) {
    if (!job.health.rot_check || !job.image_disks.empty()) return {};
    const std::vector<SnapshotRef> all = load_all_snapshots(repo);
    const auto mine = versions_of_job(all, job);
    const std::string host = os::hostname();
    const SnapshotRef* newest = nullptr;
    for (const SnapshotRef& s : all)  // oldest first
        if (!s.snapshot.image && s.snapshot.hostname == host && mine(s.snapshot)) newest = &s;
    if (newest == nullptr) return {};
    RotCheckOptions o;
    o.seed = record.started;
    o.should_cancel = cancel;
    o.on_progress = [](const std::string& msg) {
        if (g_json) emit({{"event", "check_progress"}, {"current", "files on this PC: " + msg}});
        else status("Comparing with the backup: " + msg);
    };
    RotCheckResult r;
    try {
        r = check_unchanged_files(repo, newest->snapshot, o);
    } catch (const Cancelled&) {
        throw;
    } catch (const std::exception& e) {
        warning(std::string("could not compare files on this PC with the backup: ") + e.what());
        return {};
    }
    if (r.damaged.empty())
        return r.checked == 0 ? std::string()
                              : "compared " + std::to_string(r.checked) +
                                    " unchanged file(s) on this PC with the backup: no silent changes";
    const std::string when = os::format_time_local(newest->snapshot.time).substr(0, 16);
    record.rot_files = r.damaged.size();
    record.rot_version = newest->snapshot.time;
    record.rot.clear();
    for (std::size_t i = 0; i < r.damaged.size(); ++i) {
        warning(r.damaged[i].path + ": its content differs from the backup of " + when +
                " although its size and times are the same");
        if (i < 5) record.rot += (i ? "; " : "") + r.damaged[i].path;
    }
    if (r.damaged.size() > 5) record.rot += "; ...";
    return std::to_string(r.damaged.size()) + " file(s) on this PC changed without being written to (silent damage, "
           "a failing drive?): " + record.rot + "; the version of " + when + " holds the earlier copy";
}

int health_check(Repository& repo, const jobs::Job& job, jobs::RunKind kind, jobs::RunRecord& record) {
    record.kind = kind;
    record.started = os::now();
    const bool full = kind == jobs::RunKind::FullCheck;
    std::vector<std::string> problems;
    VerifyResult r;
    std::string rot;
    try {
        RepoLock lock(repo, RepoLock::Mode::Shared, "check (job " + job.id + ")");
        repo.reload_index();  // a prune may have finished before the lock was granted
        VerifyOptions o;
        o.read_data = true;
        o.read_share = full ? 1.0 : job.health.sample_percent / 100.0;
        o.add_parity = repo.parity_percent();  // data from before repair data gets it over time
        o.sample_seed = record.started;  // a different share each time
        o.should_cancel = [&lock] { return stop_requested() || lock.lost(); };
        o.on_progress = [](const std::string& msg) {
            if (g_json) emit({{"event", "check_progress"}, {"current", msg}});
            else status("Checking " + msg);
        };
        o.on_error = [&problems](const std::string& msg) {
            if (problems.size() < 3) problems.push_back(msg);
            warning(msg);
        };
        if (!g_json)
            out(std::string(full ? "Full health check: reading all stored data back...\n"
                                 : "Health check: reading " + std::to_string(job.health.sample_percent) +
                                       "% of the stored data back...\n"));
        r = run_verify(repo, o);
        rot = rot_check(repo, job, record, o.should_cancel);
    } catch (const Cancelled&) {
        if (!g_json) clear_status();
        record.finished = os::now();
        record.status = jobs::RunStatus::Cancelled;
        record.message = "cancelled";
        throw;
    } catch (const std::exception& e) {
        if (!g_json) clear_status();
        record.finished = os::now();
        record.status = jobs::RunStatus::Failed;
        record.message = std::string("the check could not run: ") + e.what();
        throw;
    }
    record.finished = os::now();
    record.files = r.packs_read;
    record.bytes = r.bytes_read;
    record.errors = r.errors;
    record.status = r.errors > 0 ? jobs::RunStatus::Failed : jobs::RunStatus::Success;
    if (r.errors > 0) {
        record.message = std::to_string(r.errors) + " problem(s) found: ";
        for (std::size_t i = 0; i < problems.size(); ++i) record.message += (i ? "; " : "") + problems[i];
    } else {
        record.message = "read " + format_bytes(r.bytes_read) + " in " + std::to_string(r.packs_read) + " of " +
                         std::to_string(r.packs) + " pack(s), checked " + std::to_string(r.snapshots) +
                         " version(s); no problems";
    }
    if (r.packs_repairable > 0)
        record.message += "; " + std::to_string(r.packs_repairable) +
                          " damaged pack(s) can be rebuilt from their repair data: run a repair";
    if (r.parity_added > 0) record.message += "; repair data added to " + std::to_string(r.parity_added) + " pack(s)";
    if (r.packs_archived > 0)
        record.message += "; " + std::to_string(r.packs_archived) +
                          " pack(s) in archive storage were only checked to be there (reading them needs a thaw)";
    if (!rot.empty()) record.message += "; " + rot;
    if (g_json) {
        emit({{"event", "check"},
              {"kind", jobs::to_string(kind)},
              {"status", jobs::to_string(record.status)},
              {"packs", r.packs},
              {"packs_read", r.packs_read},
              {"bytes_read", r.bytes_read},
              {"snapshots", r.snapshots},
              {"errors", r.errors},
              {"rot_files", record.rot_files},
              {"message", record.message}});
    } else {
        clear_status();
        out(std::string(r.errors > 0 ? "Health check FAILED: " : "Health check: ") + record.message + "\n");
        if (record.rot_files > 0)
            out("WARNING: files on this PC changed although nothing wrote to them, which a failing drive does. "
                "The backup holds the earlier copy: restore them from the version of " +
                os::format_time_local(record.rot_version).substr(0, 16) + " (vaultik restore --job " + job.id +
                " <version> --path <file> --target <folder>), then check the drive.\n");
    }
    return r.errors > 0 ? kExitAuth : kExitOk;
}

// A test restore of the job's newest version (see ops/restore_test.hpp), recorded in `record`.
// The temporary folder is made in the job store's folder: the user's own, or with administrator
// rights the admin-only store, so a privileged restore never writes where the user could plant a
// link. Returns kExitOk, or kExitError when a file did not come back right.
int restore_check(const Args& a, const jobs::JobStore& store, Repository& repo, const jobs::Job& job,
                  jobs::RunRecord& record) {
    record.kind = jobs::RunKind::RestoreTest;
    record.started = os::now();
    std::string work = store.dir();
    if (os::process_elevated() && !a.has("--admin")) work = jobs::own_admin_store();
    RestoreTestResult r;
    try {
        RepoLock lock(repo, RepoLock::Mode::Shared, "test restore (job " + job.id + ")");
        repo.reload_index();
        // The newest version of this job.
        const std::vector<SnapshotRef> all = load_all_snapshots(repo);
        const SnapshotRef* newest = nullptr;  // the job's own (tagged), else the newest of any
        const SnapshotRef* newest_tagged = nullptr;
        for (const SnapshotRef& s : all) {  // oldest first
            newest = &s;
            const auto& tags = s.snapshot.tags;
            if (std::find(tags.begin(), tags.end(), "job:" + job.id) != tags.end()) newest_tagged = &s;
        }
        if (newest_tagged != nullptr) newest = newest_tagged;
        if (newest == nullptr) throw Error("the backup has no versions yet");
        if (newest->snapshot.image) throw Error("a test restore restores files; disk images are checked by the health check");
        RestoreTestOptions o;
        o.work_dir = work;
        o.seed = record.started;
        if (auto v = a.value("--files")) o.max_files = static_cast<std::size_t>(std::max(1, std::stoi(*v)));
        if (auto v = a.value("--max-size")) o.max_bytes = parse_size(*v);
        o.should_cancel = [&lock] { return stop_requested() || lock.lost(); };
        o.on_progress = [](const std::string& msg) {
            if (g_json) emit({{"event", "test_progress"}, {"current", msg}});
            else status("Test restore: " + msg);
        };
        if (!g_json) out("Test restore: restoring a few files of the version of " +
                         os::format_time_local(newest->snapshot.time) + "...\n");
        r = test_restore(repo, newest->snapshot, o);
        record.snapshot = id_hex(newest->id);
    } catch (const Cancelled&) {
        if (!g_json) clear_status();
        record.finished = os::now();
        record.status = jobs::RunStatus::Cancelled;
        record.message = "cancelled";
        throw;
    } catch (const std::exception& e) {
        if (!g_json) clear_status();
        record.finished = os::now();
        record.status = jobs::RunStatus::Failed;
        record.message = std::string("the test restore could not run: ") + e.what();
        throw;
    }
    record.finished = os::now();
    record.files = r.files;
    record.bytes = r.bytes;
    record.errors = r.problems.size();
    if (!r.problems.empty()) {
        record.status = jobs::RunStatus::Failed;
        record.message = std::to_string(r.problems.size()) + " file(s) did not come back right: ";
        for (std::size_t i = 0; i < r.problems.size() && i < 3; ++i) record.message += (i ? "; " : "") + r.problems[i];
        for (const std::string& p : r.problems) warning(p);
    } else if (r.files == 0) {
        record.status = jobs::RunStatus::Success;
        record.message = "the newest version has no files to test";
    } else {
        record.status = jobs::RunStatus::Success;
        record.message = "restored " + std::to_string(r.files) + " file(s), " + format_bytes(r.bytes) +
                         ", all intact; " + std::to_string(r.compared) + " also compared with the file on this PC";
    }
    if (g_json) {
        emit({{"event", "restore_test"}, {"status", jobs::to_string(record.status)}, {"files", r.files},
              {"bytes", r.bytes}, {"compared", r.compared}, {"problems", r.problems}, {"message", record.message}});
    } else {
        clear_status();
        out(std::string(r.problems.empty() ? "Test restore: " : "Test restore FAILED: ") + record.message + "\n");
    }
    return r.problems.empty() ? kExitOk : kExitError;
}

// The job's store: with --admin (the scheduled task of a job with administrator rights) only the
// admin-only copy of the job is trusted, never the user's own settings.
jobs::JobStore job_store(const Args& a) {
    if (!a.has("--admin")) return jobs::JobStore();
    // Run as the system account for the account that approved the job (its admin store). A task
    // from before that runs as the account itself, with its full token, and has no --user.
    if (const auto sid = a.value("--user")) jobs::act_for_account(*sid);
    return jobs::open_admin_store();
}

std::unique_ptr<Repository> open_job_repo(const Args& a, const jobs::JobStore& store, const jobs::Job& job) {
    std::string pw = store.password(job.id);
    if (pw.empty()) pw = get_password(a, false);
    status("Opening repository...");
    std::unique_ptr<Repository> repo;
    try {
        const CloudCredentials cloud = store.cloud_credentials(job);
        repo = Repository::open(open_backend(job.repository, &cloud), pw);
    } catch (...) {
        secure_wipe(pw.data(), pw.size());
        clear_status();
        throw;
    }
    secure_wipe(pw.data(), pw.size());
    clear_status();
    return repo;
}

// Leaves this computer's status record in the job's repository (see jobs/computers.hpp), so other
// computers backing up there can see how this one is doing. Best effort: a record that cannot be
// written is a warning, never a failed run.
void publish_job_status(Repository& repo, const jobs::JobStore& store, const jobs::Job& job) {
    std::vector<jobs::RunRecord> history;
    try {
        history = jobs::combined_history(store, job.id, 500);
    } catch (const std::exception&) {
    }
    const std::string failed = jobs::publish_status(repo, job, history, kVersion);
    if (!failed.empty()) warning("could not leave this computer's status in the backup: " + failed);
}

// Whether another run is backing up this job right now (it holds a live lock naming the job).
bool backup_running(Repository& repo, const jobs::Job& job) {
    const std::string mine = "backup (job " + job.id + ")";
    const std::uint64_t now = os::now();
    for (const LockEntry& e : list_locks(repo))
        if (e.info && e.info->command == mine && !is_stale(*e.info, now)) return true;
    return false;
}

// Where `mount` puts versions by default: <settings>\browse\<process id>\<name>, so folders left by
// a mount whose process was killed can be told apart and removed.
std::string browse_dir() {
    return os::join(jobs::config_dir(), "browse");
}

void remove_leftover_mounts() {
    const std::string base = browse_dir();
    os::DirReader reader(base);
    std::string name;
    os::FileInfo info;
    std::vector<std::string> stale;
    while (reader.next(name, info)) {
        if (info.type != os::EntryType::Dir) continue;
        const unsigned long pid = std::strtoul(name.c_str(), nullptr, 10);
        if (pid != 0 && pid != os::process_id() && !os::process_alive(static_cast<std::uint32_t>(pid)))
            stale.push_back(os::join(base, name));
    }
    for (const std::string& dir : stale) {
        os::DirReader inner(dir);
        while (inner.next(name, info))
            if (info.type == os::EntryType::Dir) SnapshotMount::remove_leftover(os::join(dir, name));
        std::error_code ec;
        std::filesystem::remove_all(std::filesystem::path(std::u8string(dir.begin(), dir.end())), ec);
    }
}

// "mount <snapshot> [--target <folder>]": shows a version as a read-only folder (File Explorer
// can browse it) until Enter is pressed, or with --json until "cancel" or the caller goes away.
int cmd_mount(const Args& a) {
    if (a.positional.empty() || a.positional.size() > 2 || (a.positional.size() == 2 && a.value("--target")))
        throw Error("usage: vaultik mount -r <repo> <snapshot> [<folder> | --target <folder>]");
    if (!SnapshotMount::available()) throw Error(SnapshotMount::how_to_enable());
    auto repo = open_repo(a);
    // Shared, like a restore: a prune cannot delete data the folder still shows.
    RepoLock lock(*repo, RepoLock::Mode::Shared, "browse", lock_wait(a));
    repo->reload_index();
    const SnapshotRef ref = resolve_snapshot(*repo, a.positional[0]);

    remove_leftover_mounts();
    std::string folder;
    std::string own_dir;
    if (const auto target = a.value("--target")) {
        folder = *target;
    } else if (a.positional.size() == 2) {
        folder = a.positional[1];
    } else {
        std::string label = job_arg(a) ? job_arg(a)->name : std::string("Backup");
        label += " " + os::format_time_local(ref.snapshot.time);
        for (char& c : label)
            if (c == ':' || c == '/' || c == '\\' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
                c = '.';
        own_dir = os::join(browse_dir(), std::to_string(os::process_id()));
        folder = os::join(own_dir, label);
    }

    {
        SnapshotMount mount(*repo, ref.snapshot, folder);
        if (g_json) {
            emit({{"event", "mounted"}, {"folder", mount.folder()}, {"snapshot", id_hex(ref.id)}});
        } else {
            out("Version " + short_id(ref.id) + " from " + os::format_time_local(ref.snapshot.time) + " is open at\n  " +
                mount.folder() + "\nIt is read-only. Press Enter (or Ctrl+C) to close it.\n");
#ifdef _WIN32
            std::thread([] {
                std::string line;
                std::getline(std::cin, line);
                g_cancel = true;
            }).detach();
#endif
        }
        // Also ends on Ctrl+C or SIGTERM (stop_requested), and when the folder was unmounted from
        // outside (Linux: fusermount3 -u).
        while (!stop_requested() && !g_stdin_closed && !lock.lost() && mount.active()) {
#ifndef _WIN32
            // Enter (or the end of the input) is looked for here: a thread left blocked reading
            // standard input would hold its lock, and the C library's exit waits for that lock.
            if (!g_json) {
                struct pollfd p = {STDIN_FILENO, POLLIN, 0};
                if (::poll(&p, 1, 0) > 0 && (p.revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
                    char buf[256];
                    const ssize_t n = ::read(STDIN_FILENO, buf, sizeof buf);
                    if (n <= 0 || std::memchr(buf, '\n', static_cast<std::size_t>(n)) != nullptr) break;
                }
            }
#endif
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }  // the folder goes here
    if (!own_dir.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(std::filesystem::path(std::u8string(own_dir.begin(), own_dir.end())), ec);
    }
    if (g_json) emit({{"event", "done"}, {"status", "success"}, {"message", "closed"}});
    else out("Closed.\n");
    return kExitOk;
}

// "2026-11-09 20:30" (or with seconds, or only a date) in local time, as seconds since 1970.
std::int64_t parse_local_time(const std::string& text) {
    int v[6] = {0, 0, 0, 0, 0, 0};
    const char* p = text.c_str();
    const char seps[] = {'-', '-', ' ', ':', ':'};
    int n = 0;
    for (; n < 6; ++n) {
        char* end = nullptr;
        const long x = std::strtol(p, &end, 10);
        if (end == p) break;
        v[n] = static_cast<int>(x);
        p = end;
        if (*p == '\0') {
            ++n;
            break;
        }
        if (n >= 5 || (*p != seps[n] && !(n == 2 && *p == 'T'))) throw Error("give the time as YYYY-MM-DD HH:MM");
        ++p;
    }
    if (n != 3 && n != 5 && n != 6) throw Error("give the time as YYYY-MM-DD HH:MM");
    std::tm tm{};
    tm.tm_year = v[0] - 1900;
    tm.tm_mon = v[1] - 1;
    tm.tm_mday = v[2];
    tm.tm_hour = v[3];
    tm.tm_min = v[4];
    tm.tm_sec = v[5];
    tm.tm_isdst = -1;
    const std::time_t t = std::mktime(&tm);
    if (t == static_cast<std::time_t>(-1) || v[1] < 1 || v[1] > 12 || v[2] < 1 || v[2] > 31)
        throw Error("not a valid time: " + text);
    return static_cast<std::int64_t>(t);
}

// "cloud-rollback --to <time> [--dry-run]": brings an S3 repository's files back as they were at
// that time, from the bucket's old versions (after ransomware deleted or overwrote them).
int cmd_cloud_rollback(const Args& a) {
    const auto to = a.value("--to");
    if (!to) throw Error("usage: vaultik cloud-rollback -r s3:... --to \"YYYY-MM-DD HH:MM\" [--dry-run]");
    const std::int64_t when = parse_local_time(*to);
    auto backend = backend_for(a);
    auto* s3 = dynamic_cast<S3Backend*>(backend.get());
    if (s3 == nullptr) throw Error("cloud-rollback works on S3-compatible storage with versioning (Object Lock)");
    const bool dry = a.has("--dry-run");
    status("Reading the bucket's old versions...");
    const auto r = s3->roll_back(when, dry, [dry](const std::string& name) {
        if (g_json) emit({{"event", "file"}, {"name", name}});
        else out(std::string(dry ? "would restore " : "restoring ") + name + "\n");
    });
    clear_status();
    if (g_json) {
        emit({{"event", "done"},
              {"status", r.failed.empty() ? "success" : "partial"},
              {"checked", r.files_checked},
              {"restored", r.restored},
              {"unchanged", r.unchanged},
              {"newer", r.newer},
              {"failed", r.failed}});
    } else {
        for (const std::string& f : r.failed) err("error: " + f + "\n");
        out(std::to_string(r.restored) + " file(s) " + (dry ? "would be" : "were") + " brought back, " +
            std::to_string(r.unchanged) + " unchanged, " + std::to_string(r.newer) + " added later (left alone)" +
            (r.failed.empty() ? std::string() : ", " + std::to_string(r.failed.size()) + " failed") + ".\n");
        if (!dry && r.restored > 0) out("Run \"vaultik verify\" next, then restore from the versions you need.\n");
    }
    return r.failed.empty() ? kExitOk : kExitPartial;
}

// "cloud-thaw [<snapshot>] [--path <p>]... [--days n] [--tier t] [--status]": asks S3 archive
// storage for a readable copy of every pack a restore of that version (or of those paths) reads,
// and reports how many were asked for now, are being thawed, and can be read already.
int cmd_cloud_thaw(const Args& a) {
    if (a.positional.size() > 1)
        throw Error("usage: vaultik cloud-thaw -r s3:... [<snapshot>|latest] [--path <path>] [--days <n>] "
                    "[--tier Standard|Bulk|Expedited] [--status]");
    int days = 7;
    if (const auto v = a.value("--days")) {
        char* end = nullptr;
        const long n = std::strtol(v->c_str(), &end, 10);
        if (end == v->c_str() || *end != '\0' || n < 1 || n > 365) throw Error("--days takes a number from 1 to 365");
        days = static_cast<int>(n);
    }
    const std::string tier = a.value("--tier").value_or("Standard");
    (void)s3::restore_request(days, tier);  // checks both before anything is read
    const bool status_only = a.has("--status");
    auto repo = open_repo(a);
    auto* s3 = dynamic_cast<S3Backend*>(&repo->backend());
    if (s3 == nullptr) throw Error("cloud-thaw works on S3-compatible storage (archive storage classes)");
    RepoLock lock(*repo, RepoLock::Mode::Shared, "cloud-thaw", lock_wait(a));
    repo->reload_index();
    const SnapshotRef ref = resolve_snapshot(*repo, a.positional.empty() ? std::string("latest") : a.positional[0]);
    status("Finding the data this restore needs...");
    const std::vector<Id> packs = packs_for_restore(*repo, ref.snapshot, a.all("--path"));
    std::uint64_t requested = 0, in_progress = 0, available = 0;
    std::size_t done = 0;
    for (const Id& pack : packs) {
        if (stop_requested()) throw Cancelled();
        const std::string name = Repository::pack_name(pack);
        ++done;
        if (g_json) emit({{"event", "progress"}, {"files", done}, {"bytes", 0}, {"current", name}});
        else status("Thawing " + std::to_string(done) + " of " + std::to_string(packs.size()) + "...");
        if (status_only) {
            const S3Backend::ArchiveState st = s3->archive_state(name);
            if (!st.archived || st.thawed) ++available;
            else if (st.thawing) ++in_progress;
            else ++requested;  // here: not asked for yet
            continue;
        }
        switch (s3->thaw(name, days, tier)) {
            case S3Backend::ThawOutcome::Requested: ++requested; break;
            case S3Backend::ThawOutcome::InProgress: ++in_progress; break;
            case S3Backend::ThawOutcome::Available: ++available; break;
        }
    }
    clear_status();
    const bool ready = requested == 0 && in_progress == 0;
    if (g_json) {
        emit({{"event", "done"},
              {"status", "success"},
              {"snapshot", id_hex(ref.id)},
              {"packs", packs.size()},
              {status_only ? "not_requested" : "requested", requested},
              {"in_progress", in_progress},
              {"available", available},
              {"ready", ready}});
        return kExitOk;
    }
    out("Version " + short_id(ref.id) + " needs " + std::to_string(packs.size()) + " pack(s): " +
        std::to_string(requested) + (status_only ? " not asked for yet, " : " thaw(s) asked for now, ") +
        std::to_string(in_progress) + " being thawed, " + std::to_string(available) + " readable already.\n");
    if (ready) {
        out("Everything this restore needs can be read now: restore it while the thawed copies last.\n");
    } else if (status_only && in_progress == 0) {
        out("Run \"vaultik cloud-thaw\" without --status to ask for a thaw.\n");
    } else {
        out("A thaw takes from minutes (Expedited) to 12 hours (Standard) or more (Bulk; up to 48 hours for "
            "DEEP_ARCHIVE). Run \"vaultik cloud-thaw --status\" for the same version to see how far it is, then "
            "restore.\n");
    }
    return kExitOk;
}

// While a job runs: switches to the working-hours speed limits when they start, and back when they
// end (checked every 30 seconds). Does nothing for a job without working hours.
class SpeedSchedule {
public:
    SpeedSchedule(const ResourcePolicy& normal, const jobs::WorkHours& hours) {
        if (!hours.enabled) return;
        apply(normal, hours);
        thread_ = std::thread([this, normal, hours] {
            std::unique_lock<std::mutex> lock(mu_);
            while (!cv_.wait_for(lock, std::chrono::seconds(30), [this] { return stop_; })) apply(normal, hours);
        });
    }
    ~SpeedSchedule() {
        if (!thread_.joinable()) return;
        {
            std::lock_guard<std::mutex> lock(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        thread_.join();
    }
    SpeedSchedule(const SpeedSchedule&) = delete;
    SpeedSchedule& operator=(const SpeedSchedule&) = delete;

private:
    void apply(const ResourcePolicy& normal, const jobs::WorkHours& hours) {
        const auto [read, write] = jobs::rate_limits_now(normal, hours);
        if (read == read_ && write == write_) return;
        read_ = read;
        write_ = write;
        set_rate_limits(read, write);
        if (g_json) emit({{"event", "speed"}, {"read_limit", read}, {"write_limit", write}});
    }

    std::uint64_t read_ = ~0ull, write_ = ~0ull;
    std::mutex mu_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::thread thread_;
};

std::string drive_problem(const os::DriveHealth& d);  // below, with the drives command

// After a job's backup: copies its new versions to the job's second destination (creating the
// repository there on first use, with the job's password), then removes copies beyond the job's
// retention there. Fills `record` (kind Copy) whatever happens; returns kExitOk or kExitError.
int second_copy(const Args& a, const jobs::JobStore& store, const jobs::Job& job, Repository& source,
                jobs::RunRecord& record, bool keep_old = false) {
    record.kind = jobs::RunKind::Copy;
    record.started = os::now();
    const std::string& where = job.second_copy.repository;
    try {
        if (!jobs::copy_drive_connected(job)) throw Error("the drive of the second copy is not connected (" + where + ")");
        std::string pw = store.password(job.id);
        if (pw.empty()) pw = get_password(a, false);
        std::unique_ptr<Repository> target;
        try {
            const CloudCredentials cloud = store.copy_credentials(job);
            auto backend = open_backend(where, &cloud, true);
            if (!backend->exists("config")) {
                if (!g_json) out("Creating the second copy at " + where + "\n");
                Repository::init(*backend, pw);
            }
            target = Repository::open(std::move(backend), pw);
            // Remember the drive once the copy is there, so a different drive that gets this
            // letter later is never taken for it.
            if (!is_cloud_location(where) && job.second_copy.volume.empty()) {
                try {
                    jobs::Job updated = job;
                    jobs::record_copy_volume(updated);
                    if (!updated.second_copy.volume.empty()) store.save(updated);
                } catch (const Error& e) {
                    warning(std::string("could not record the second copy's drive: ") + e.what());
                }
            }
        } catch (...) {
            secure_wipe(pw.data(), pw.size());
            throw;
        }
        secure_wipe(pw.data(), pw.size());
        if (!g_json) out("Second copy: copying new versions to " + where + "\n");

        CopyResult r;
        {
            RepoLock from_lock(source, RepoLock::Mode::Shared, "copy (job " + job.id + ")", lock_wait(a, std::chrono::minutes(10)));
            RepoLock to_lock(*target, RepoLock::Mode::Shared, "copy (job " + job.id + ")", lock_wait(a, std::chrono::minutes(10)));
            source.reload_index();
            target->reload_index();
            CopyOptions co;
            co.tags = {"job:" + job.id};
            co.map_file = os::join(os::join(store.dir(), "copies"), job.id + ".map");
            co.should_cancel = [&] { return stop_requested() || from_lock.lost() || to_lock.lost(); };
            co.on_progress = [](const std::string& msg) {
                if (g_json) emit({{"event", "copy_progress"}, {"current", msg}});
                else status("Copying " + msg);
            };
            r = copy_snapshots(source, *target, co);
        }
        if (!g_json) clear_status();
        // The copy is what a user falls back on: never thin it out after a warning on the source.
        std::string note;
        if (keep_old) note = "old versions in the copy were not removed because of the warning";
        else if (!job.retention.empty()) note = clean_up(*target, job);
        record.finished = os::now();
        record.status = jobs::RunStatus::Success;
        record.files = r.copied;
        record.bytes = r.bytes;
        record.message = r.copied == 0 ? "the second copy was up to date"
                                       : "copied " + std::to_string(r.copied) + " version(s), " + format_bytes(r.bytes) + " new";
        if (!note.empty()) record.message += "; " + note;
        if (g_json) {
            emit({{"event", "copy"}, {"status", "success"}, {"copied", r.copied}, {"bytes", r.bytes},
                  {"message", record.message}});
        } else {
            out("Second copy: " + record.message + "\n");
        }
        return kExitOk;
    } catch (const Cancelled&) {
        if (!g_json) clear_status();
        record.finished = os::now();
        record.status = jobs::RunStatus::Cancelled;
        record.message = "cancelled; the versions copied so far stay";
    } catch (const std::exception& e) {
        if (!g_json) clear_status();
        record.finished = os::now();
        record.status = jobs::RunStatus::Failed;
        record.message = std::string("the second copy failed: ") + e.what();
    }
    if (g_json) emit({{"event", "copy"}, {"status", jobs::to_string(record.status)}, {"message", record.message}});
    else warning(record.message);
    return kExitError;
}

// "copy --job <job>": makes the job's second copy now (also run by run-job after each backup).
int cmd_copy(const Args& a) {
    if (a.positional.size() != 1) throw Error("usage: vaultik copy <job id or name>");
    const jobs::JobStore store = job_store(a);
    const auto job = store.find(a.positional[0]);
    if (!job) throw Error("no saved job called " + a.positional[0] + " (see vaultik jobs)");
    if (job->second_copy.repository.empty()) throw Error("the job \"" + job->name + "\" has no second copy set up");
    apply_resource_policy(resource_policy(a, job->resources));
    const auto repo = open_job_repo(a, store, *job);
    jobs::RunRecord record;
    const int code = second_copy(a, store, *job, *repo, record);
    store.append_history(job->id, record);
    publish_job_status(*repo, store, *job);
    if (g_json) emit({{"event", "done"}, {"status", jobs::to_string(record.status)}, {"message", record.message}});
    return code;
}

// "check-job <job> [--full]": a health check of the job's backup now, recorded in its history.
int cmd_check_job(const Args& a) {
    if (a.positional.empty()) throw Error("usage: vaultik check-job <job id or name> [--full]");
    const jobs::JobStore store = job_store(a);
    const auto job = store.find(a.positional[0]);
    if (!job) throw Error("no saved job called " + a.positional[0] + " (see vaultik jobs)");
    apply_resource_policy(resource_policy(a, job->resources));
    jobs::RunRecord record;
    record.kind = a.has("--full") ? jobs::RunKind::FullCheck : jobs::RunKind::Check;
    record.started = os::now();
    int code = kExitError;
    std::unique_ptr<Repository> repo;
    try {
        repo = open_job_repo(a, store, *job);
        code = health_check(*repo, *job, record.kind, record);
    } catch (const LockedError&) {
        throw;  // nothing was checked; not a result for the history
    } catch (const std::exception& e) {
        if (record.finished == 0) {  // failed before the check started
            record.finished = os::now();
            record.status = jobs::RunStatus::Failed;
            record.message = std::string("the check could not run: ") + e.what();
        }
        try {
            store.append_history(job->id, record);
        } catch (const Error&) {
        }
        if (repo) publish_job_status(*repo, store, *job);
        if (record.status != jobs::RunStatus::Cancelled) throw;
        if (g_json) emit({{"event", "done"}, {"status", "cancelled"}, {"message", "health check cancelled"}});
        else err("health check cancelled\n");
        return kExitCancelled;
    }
    store.append_history(job->id, record);
    publish_job_status(*repo, store, *job);
    if (g_json) emit({{"event", "done"}, {"status", jobs::to_string(record.status)}, {"message", record.message}});
    return code;
}

// "test-restore <job> [--files n] [--max-size size]": a test restore of the job's newest version
// now, recorded in its history.
int cmd_test_restore(const Args& a) {
    if (a.positional.empty()) throw Error("usage: vaultik test-restore <job id or name> [--files <n>] [--max-size <size>]");
    const jobs::JobStore store = job_store(a);
    const auto job = store.find(a.positional[0]);
    if (!job) throw Error("no saved job called " + a.positional[0] + " (see vaultik jobs)");
    apply_resource_policy(resource_policy(a, job->resources));
    jobs::RunRecord record;
    record.kind = jobs::RunKind::RestoreTest;
    record.started = os::now();
    int code = kExitError;
    std::unique_ptr<Repository> repo;
    try {
        repo = open_job_repo(a, store, *job);
        code = restore_check(a, store, *repo, *job, record);
    } catch (const LockedError&) {
        throw;  // nothing was tested; not a result for the history
    } catch (const std::exception& e) {
        if (record.finished == 0) {  // failed before the test started
            record.finished = os::now();
            record.status = jobs::RunStatus::Failed;
            record.message = std::string("the test restore could not run: ") + e.what();
        }
        try {
            store.append_history(job->id, record);
        } catch (const Error&) {
        }
        if (repo) publish_job_status(*repo, store, *job);
        if (record.status != jobs::RunStatus::Cancelled) throw;
        if (g_json) emit({{"event", "done"}, {"status", "cancelled"}, {"message", "test restore cancelled"}});
        else err("test restore cancelled\n");
        return kExitCancelled;
    }
    store.append_history(job->id, record);
    publish_job_status(*repo, store, *job);
    if (g_json) emit({{"event", "done"}, {"status", jobs::to_string(record.status)}, {"message", record.message}});
    return code;
}

// Sends the alert a run calls for (see jobs/alerts.hpp), if the user set up alerts and wants this
// kind. A failure to send is a warning, never a reason for the run to fail.
void alert_for(const jobs::Job& job, const jobs::RunRecord& r, const std::string& settings_dir) {
    // Alerts leave the computer: they say what happened in fixed words, with counts, and never
    // carry a run's own message, file names or paths (those stay in Vaultik's history).
    const jobs::AlertSettings s = jobs::load_alerts(settings_dir);
    if (!s.any()) return;
    jobs::Alert a;
    a.job = job.name;
    const std::string where = " Open Vaultik on " + os::hostname() + " for the details.";
    const bool check = jobs::is_check(r.kind) || r.kind == jobs::RunKind::RestoreTest;
    if (check && s.on_damage && r.rot_files > 0 && !(r.status == jobs::RunStatus::Failed && r.errors > 0)) {
        // Files on this computer that changed by themselves: the backup is fine, the drive is not.
        a.level = jobs::Alert::Level::Critical;
        a.event = "rot";
        a.title = "Health check of " + job.name + " found files that changed by themselves";
        a.message = std::to_string(r.rot_files) +
                    " file(s) on the computer changed although nothing wrote to them, which a failing drive does. "
                    "The backup holds the earlier copy." + where;
    } else if (check) {
        // Damage found (not a check that could not run: the backup before it said so).
        if (!s.on_damage || r.status != jobs::RunStatus::Failed || r.errors == 0) return;
        a.level = jobs::Alert::Level::Critical;
        a.event = "damage";
        a.title = (r.kind == jobs::RunKind::RestoreTest ? "Test restore of " : "Health check of ") + job.name +
                  " found damage";
        a.message = "Some backed-up data could not be read back or restored (" + std::to_string(r.errors) +
                    " problem(s))." + where;
    } else if (!r.alert.empty()) {
        a.level = jobs::Alert::Level::Critical;
        a.event = "ransomware";
        a.title = job.name + ": files may be encrypted by ransomware";
        a.message = "Many files changed the way ransomware changes them. Disconnect the backup drive." + where;
    } else if (r.status == jobs::RunStatus::Failed) {
        if (!s.on_failure) return;
        a.level = jobs::Alert::Level::Critical;
        a.event = "failed";
        a.title = "Backup of " + job.name + " failed";
        a.message = "No version was saved." + where;
    } else if (!r.drive.empty() && s.on_damage) {
        a.level = jobs::Alert::Level::Warning;
        a.event = "drive";
        a.title = "The backup drive of " + job.name + " reports a problem";
        a.message = "Copy the backup to another drive soon, before it fails." + where;
    } else if (r.status == jobs::RunStatus::Partial) {
        if (!s.on_failure) return;
        a.level = jobs::Alert::Level::Warning;
        a.event = "partial";
        a.title = "Backup of " + job.name + " skipped " + std::to_string(r.errors) + " item(s)";
        a.message = "A version was saved without them." + where;
    } else if (r.status == jobs::RunStatus::Success && r.kind == jobs::RunKind::Backup && s.on_success) {
        a.level = jobs::Alert::Level::Info;
        a.event = "success";
        a.title = "Backup of " + job.name + " completed";
        a.message = std::to_string(r.files) + " files, " + format_bytes(r.new_bytes) + " new";
    } else {
        return;
    }
    a.title += " (" + os::hostname() + ")";
    for (const std::string& f : jobs::send_alert(s, a, settings_dir)) warning("could not send the alert: " + f);
}

// When the backup drive newly fills up within jobs::kFillWarnDays (the forecast after this run warns
// and the one before it did not): a warning, and an alert when alerts are set up. Only on the
// change, so a drive that stays nearly full is not announced after every run (the start page, the
// tray and the monthly report keep saying it).
void space_alert_for(const jobs::JobStore& store, const jobs::Job& job, const jobs::FillForecast& before,
                     const std::string& settings_dir) {
    jobs::FillForecast now;
    try {
        now = jobs::forecast_for(jobs::combined_history(store, job.id, 500), os::now());
    } catch (const std::exception&) {
        return;
    }
    if (!jobs::fill_warning(now) || jobs::fill_warning(before)) return;
    warning("the backup drive " + jobs::describe(now) +
            "; keep fewer versions (the job's Old backups settings) or use a larger drive");
    const jobs::AlertSettings s = jobs::load_alerts(settings_dir);
    if (!s.any() || !s.on_damage) return;
    jobs::Alert a;
    a.level = jobs::Alert::Level::Warning;
    a.event = "space";
    a.job = job.name;
    a.title = "The backup drive of " + job.name + " fills up " + jobs::fill_time_text(now.days) + " (" +
              os::hostname() + ")";
    a.message = "At the current rate the backup needs more space soon. Keep fewer versions or move it to a larger "
                "drive. Open Vaultik on " + os::hostname() + " for the details.";
    for (const std::string& f : jobs::send_alert(s, a, settings_dir)) warning("could not send the alert: " + f);
}

// "report [--month YYYY-MM] [--html file] [--json]": the monthly report (see jobs/report.hpp).
// "report --deliver" sends last month's summary to the alert channels when it is due (the
// frontend runs it, since scheduled runs may not happen every month).
int cmd_report(const Args& a) {
    const jobs::JobStore store;
    if (a.has("--deliver")) {
        const std::vector<std::string> failed = jobs::deliver_monthly_summary(store);
        for (const std::string& f : failed) err(f + "\n");
        if (g_json) emit({{"event", "done"}, {"status", failed.empty() ? "success" : "failed"}, {"failed", failed}});
        return failed.empty() ? kExitOk : kExitError;
    }
    const std::string month = a.value("--month").value_or(jobs::previous_month(jobs::month_of(os::now())));
    if (!jobs::valid_month(month)) throw Error("invalid month \"" + month + "\" (use YYYY-MM, e.g. 2026-10)");
    const jobs::MonthlyReport report = jobs::report_for(store, month);
    if (auto file = a.value("--html")) {
        const std::string html = jobs::render_html(report);
        os::write_file_atomic(os::absolute_path(*file), ByteSpan(reinterpret_cast<const std::uint8_t*>(html.data()), html.size()));
        if (g_json) emit({{"event", "done"}, {"status", "success"}, {"file", *file}});
        else out("The report for " + month + " was written to " + *file + "\n");
        return kExitOk;
    }
    if (g_json) {
        out(jobs::to_json(report) + "\n");
        return kExitOk;
    }
    out(jobs::render_text(report));
    return kExitOk;
}

// "update-check": whether a newer Vaultik has been released (nothing is downloaded).
int cmd_update_check(const Args&) {
    const std::optional<jobs::Release> r = jobs::newer_release(VAULTIK_VERSION);
    if (g_json) {
        emit({{"event", "done"}, {"status", "success"}, {"update", r.has_value()},
              {"version", r ? r->version : std::string()}, {"url", r ? r->url : std::string()}});
    } else if (r) {
        out("Vaultik " + r->version + " is available (this is " + VAULTIK_VERSION + "): " + r->url + "\n");
    } else {
        out(std::string("Vaultik ") + VAULTIK_VERSION + " is the newest version.\n");
    }
    return kExitOk;
}

// "alert-test": sends a test message to every alert channel that is set up.
int cmd_alert_test(const Args&) {
    const jobs::AlertSettings s = jobs::load_alerts();
    if (!s.any()) throw Error("no alerts are set up (Settings in Vaultik)");
    jobs::Alert a;
    a.level = jobs::Alert::Level::Info;
    a.event = "test";
    a.job = "test";
    a.title = "Vaultik on " + os::hostname() + ": a test alert";
    a.message = "Alerts reach you here when a backup fails or finds damage.";
    const std::vector<std::string> failed = jobs::send_alert(s, a);
    for (const std::string& f : failed) err(f + "\n");
    if (g_json) emit({{"event", "done"}, {"status", failed.empty() ? "success" : "failed"}, {"failed", failed}});
    else if (failed.empty()) out("The test alert was sent.\n");
    return failed.empty() ? kExitOk : kExitError;
}

// Runs a job's "before" or "after" command line (see Job::before_command). Returns its exit code;
// its output goes to the log (the start of it), each line as a warning when it failed.
int run_job_command(const jobs::Job& job, const std::string& which, const std::string& command,
                    const std::string& result, bool admin, const std::string& store_dir) {
    os::ShellOptions o;
    std::string output;
    o.output = &output;
    // Stopping the backup stops the "before" command; the "after" one (often a cleanup, such as
    // restarting a service) still runs to the end, within its time limit.
    if (which == "before") o.cancelled = [] { return stop_requested(); };
    const std::vector<std::pair<std::string, std::string>> vars = {
        {"VAULTIK_JOB", job.id}, {"VAULTIK_JOB_NAME", job.name}, {"VAULTIK_RESULT", result}};
    if (admin) {
        // With administrator rights: only an approved, checked command, with a fixed environment
        // and the admin-only store as its folder, so nothing the user can change decides what runs.
        try {
            jobs::check_admin_command(command, "the command " + which + " the backup");
        } catch (const Error& e) {
            warning(e.what());
            return -1;
        }
        o.clean_environment = true;
        o.extra_environment = vars;
        o.directory = store_dir;
    } else {
        for (const auto& [name, value] : vars) os::set_env(name, value);
    }
    if (!g_json) out("Running the command " + which + " the backup\n");
    int code = 0;
    try {
        code = os::run_shell(command, o);
    } catch (const std::exception& e) {
        warning("the command " + which + " the backup did not run to the end: " + std::string(e.what()));
        return -1;
    }
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) output.pop_back();
    if (code != 0) {
        warning("the command " + which + " the backup failed (exit code " + std::to_string(code) + ")" +
                (output.empty() ? std::string() : ": " + output.substr(0, 500)));
    } else if (!output.empty() && !g_json) {
        out(output.substr(0, 2000) + "\n");
    }
    return code;
}

#include "vaultik/cli/image_cmds.inc"
#include "vaultik/cli/disk_cmds.inc"

int cmd_run_job(const Args& a) {
    if (a.positional.empty()) throw Error("usage: vaultik run-job <job id or name>");
    const jobs::JobStore store = job_store(a);
    const auto job = store.find(a.positional[0]);
    if (!job) throw Error("no saved job called " + a.positional[0] + " (see vaultik jobs)");

    // The job's own resource settings, unless overridden on the command line.
    const bool on_connect = a.has("--on-connect");
    // With administrator rights, a job from the user's own settings (which any program running as
    // the user can change) neither starts on its own nor runs its commands with them.
    if (!a.has("--admin") && os::process_elevated()) {
        const bool automatic = on_connect || a.has("--on-event") || a.has("--on-sign-out") || a.has("--on-change") ||
                               a.has("--on-request");
        const std::string refusal = jobs::elevated_run_refusal(*job, automatic);
        if (!refusal.empty()) throw Error(refusal);
    }
    // A paused job is not started on its own: only a plain run-job (Back up now) runs it.
    if (job->schedule.paused && (on_connect || a.has("--scheduled") || a.has("--on-event") || a.has("--on-sign-out") ||
                                 a.has("--on-change") || a.has("--on-request"))) {
        if (!g_json) err("The job \"" + job->name + "\" is paused: not started (vaultik resume-job resumes it).\n");
        return kExitOk;
    }
    if (on_connect && !jobs::connect_backup_due(store, *job)) return kExitOk;
    // Started by an event rather than a time: not again soon after a backup.
    std::string trigger = a.has("--on-event") ? "event" : a.has("--on-sign-out") ? "sign-out"
                          : a.has("--on-change")                                 ? "change"
                          : a.has("--on-request")                                ? "request"
                                                                                 : std::string();
    if (trigger == "event" && job->schedule.on_update) {
        // The events task also starts on Windows Update installing something: tell whether that
        // was it, and skip routine updates (Defender definitions, Store apps) nobody backs up for.
        trigger = jobs::events_trigger(job->schedule, jobs::recent_update_installs(15));
        if (trigger.empty()) return kExitOk;
    }
    if ((trigger == "event" || trigger == "update" || trigger == "sign-out") && !jobs::event_backup_due(store, *job))
        return kExitOk;
    if (trigger == "change" && !jobs::drive_connected(*job)) return kExitOk;
    // While a backup started by an update runs, the tray holds up the restart that follows it.
    struct UpdateMark {
        std::string dir, id;
        bool on = false;
        ~UpdateMark() {
            if (on) jobs::clear_update_run(dir, id);
        }
    } update_mark{store.dir(), job->id, trigger == "update"};
    if (update_mark.on) jobs::mark_update_run(store.dir(), job->id);

    const ResourcePolicy policy = resource_policy(a, job->resources);
    apply_resource_policy(policy);
    if (!policy.is_default() && !g_json) err("Resources: " + describe(policy) + "\n");
    const SpeedSchedule speed(policy, job->work_hours);

    jobs::RunRecord record;
    record.on_connect = on_connect;
    record.trigger = trigger;
    record.started = os::now();
    int code = kExitError;
    std::unique_ptr<Repository> repo;
    // Once the repository is open, this computer's status there is updated last, however the run
    // ends (after the backup, the second copy, the health check and the test restore).
    struct StatusOnExit {
        std::unique_ptr<Repository>& repo;
        const jobs::JobStore& store;
        const jobs::Job& job;
        ~StatusOnExit() {
            try {
                if (repo) publish_job_status(*repo, store, job);
            } catch (...) {
            }
            // The job ran: a request from another computer to run it is answered, however it was
            // started (see jobs::BackupRequest).
            if (repo) jobs::finish_request(*repo, job.id);
        }
    } status_on_exit{repo, store, *job};
    // The "after" command runs once the "before" one did (or there is none), whatever happens.
    bool after_due = false;
    const auto run_after = [&](const std::string& result) {
        if (!after_due || job->after_command.empty()) return;
        after_due = false;
        run_job_command(*job, "after", job->after_command, result, a.has("--admin"), store.dir());
    };
    // A scheduled run of a cloud job waits for an unmetered connection, if the job says so. It is
    // recorded (as cancelled), so the Activity tab shows why no version was saved.
    if (a.has("--scheduled") && job->schedule.skip_metered && is_cloud_location(job->repository) &&
        os::metered_connection()) {
        record.finished = os::now();
        record.status = jobs::RunStatus::Cancelled;
        record.message = "skipped: the internet connection is metered";
        store.append_history(job->id, record);
        if (g_json) emit({{"event", "done"}, {"status", "cancelled"}, {"message", record.message}});
        else out("Job \"" + job->name + "\": " + record.message + "\n");
        return kExitOk;
    }
    try {
        if (!jobs::drive_connected(*job))
            throw Error("the backup drive is not connected (" + job->repository + ")");
        repo = open_job_repo(a, store, *job);
        const unsigned readers = apply_parallelism(*repo, a, job->repository, policy);
        if ((on_connect || !trigger.empty()) && backup_running(*repo, *job)) {
            if (!g_json) out("Job \"" + job->name + "\" is already backing up.\n");
            return kExitOk;
        }
        if (!g_json) out("Job \"" + job->name + "\": backing up to " + job->repository + "\n");
        if (!job->before_command.empty()) {
            const int before = run_job_command(*job, "before", job->before_command, "", a.has("--admin"), store.dir());
            if (before != 0)
                throw Error("the command before the backup failed (exit code " + std::to_string(before) +
                            "), so nothing was backed up");
        }
        after_due = true;

        BackupOptions o;
        o.paths = job->sources;
        o.read_threads = readers;
        o.excludes = job->excludes;
        o.tags = job->tags;
        o.tags.push_back("job:" + job->id);
        o.skip_unchanged = job->skip_unchanged;
        o.check_every_file = job->check_every_file;
        if (job->record_apps && job->image_disks.empty()) {
            // Within a day of the last backup (continuous or triggered runs, hourly schedules) the
            // last version's list is kept; winget lists the programs again after that.
            const auto history = jobs::combined_history(store, job->id, 50);
            const jobs::RunRecord* last = jobs::last_backup(history);
            constexpr std::uint64_t kDay = 24ull * 3600 * 10'000'000;
            if (last != nullptr && last->status != jobs::RunStatus::Failed && last->finished + kDay > os::now())
                o.reuse_app_list = true;
            else
                o.app_list = app_list_in_background();
        }
        // With administrator rights: folders the change journal saw nothing happen in are not
        // read again (see ops/change_journal.hpp).
        ChangeJournal journal(store.dir(), job->id, job->sources, job->excludes);
        journal.apply(o);
        // Virtual machines: WSL distributions are exported as the backup reads them, Hyper-V
        // machines into a staging folder now (removed when the run ends).
        std::unique_ptr<VirtualMachines> machines_guard;
        if (!job->wsl_distros.empty() || !job->hyperv_vms.empty())
            machines_guard = add_virtual_machines(o, job->wsl_distros, parse_wsl_when_running(job->wsl_when_running),
                                                  job->hyperv_vms, job->hyperv_staging);
        // Databases are dumped now into a private folder (removed when the run ends); Docker
        // volumes are exported as the backup reads them.
        const auto dumps_guard = add_database_dumps(o, job->databases, [&](const DatabaseDump& db) {
            return store.database_secret(job->id, db.id);
        });
        add_docker_volumes(o, job->docker_volumes, job->docker_image);
        // The job's repair data setting becomes the destination's (every writer then follows it).
        if (repo->parity_percent() != job->repair_data) {
            try {
                repo->set_parity_percent(job->repair_data);
            } catch (const std::exception& e) {
                warning(std::string("could not change the repair data setting of the destination: ") + e.what());
            }
        }
        {
            // A scheduled run waits a while for a prune of the same repository to finish.
            RepoLock lock(*repo, RepoLock::Mode::Shared, "backup (job " + job->id + ")",
                          lock_wait(a, std::chrono::minutes(10)));
            repo->reload_index();  // a prune may have finished before the lock was granted
            if (!job->image_disks.empty())
                code = image_backup_and_report(*repo, job_disks(*job), o.tags, job->repository,
                                               vss_arg(a, job->vss ? VssMode::Auto : VssMode::Off) != VssMode::Off, record,
                                               lock);
            else
                code = backup_and_report(*repo, std::move(o), job->repository, record, lock,
                                         vss_arg(a, job->vss ? kDefaultVss : VssMode::Off),
                                         !(job->vss_skip_sql || a.has("--vss-skip-sql")), &journal);
        }
        refresh_drive_copy(*job);
        // The destination drive's own health, as the system reports it: a warning here is the
        // time to copy the backups elsewhere, before the drive fails.
        if (!is_cloud_location(job->repository)) {
            const std::vector<os::DriveHealth> drives = os::drive_health(job->repository);
            jobs::record_drives(drives);
            for (const os::DriveHealth& d : drives) {
                const std::string problem = drive_problem(d);
                if (problem.empty()) continue;
                record.drive = problem;
                warning("the backup drive reports a problem: " + problem);
            }
        }
        if (!record.alert.empty() || !record.hold.empty()) {
            // Never thin out the versions from before a possible attack or a missing source.
            if (!job->retention.empty())
                record.message += std::string(record.message.empty() ? "" : "; ") +
                                  "old versions were not removed because of the warning";
        } else if (!job->retention.empty()) {
            bool pruned = false;
            const std::string note = clean_up(*repo, *job, &pruned);
            if (!note.empty()) record.message += (record.message.empty() ? "" : "; ") + note;
            if (pruned) {
                // The cleanup rewrote the index; read it again for the size below.
                try {
                    repo->reload_index();
                } catch (const std::exception&) {
                }
            }
        }
        // The stored size of the whole destination now, for the monthly report.
        if (record.status != jobs::RunStatus::Failed) {
            std::uint64_t stored = 0;
            repo->index().for_each([&stored](const Id&, const BlobLocation& loc) { stored += loc.length; });
            record.backup_size = stored;
            if (!is_cloud_location(job->repository))
                if (const auto space = os::disk_space(job->repository)) record.free_space = space->free;
        }
    } catch (const std::exception& e) {
        if (record.finished == 0) {  // failed before the backup started
            record.finished = os::now();
            record.status = jobs::RunStatus::Failed;
            record.message = e.what();
        }
        try {
            store.append_history(job->id, record);
        } catch (const Error&) {
        }
        run_after(jobs::to_string(record.status));
        alert_for(*job, record, store.dir());
        throw;
    }
    // The drive's forecast before this run, to tell when it newly says the drive fills up soon.
    jobs::FillForecast fill_before;
    try {
        fill_before = jobs::forecast_for(jobs::combined_history(store, job->id, 500), os::now());
    } catch (const std::exception&) {
    }
    store.append_history(job->id, record);
    run_after(jobs::to_string(record.status));
    alert_for(*job, record, store.dir());
    space_alert_for(store, *job, fill_before, store.dir());
    // Last month's summary to the alert channels, once, when asked for. Only from the user's own
    // settings: a run with administrator rights uses the admin store's copy of the alerts, and
    // both would send it.
    if (store.dir() == jobs::config_dir())
        for (const std::string& f : jobs::deliver_monthly_summary(store)) warning("could not send the monthly summary: " + f);

    // The second copy, after a backup that saved a version. Its result is its own history entry.
    const bool copy_waits = a.has("--scheduled") && job->schedule.skip_metered &&
                            is_cloud_location(job->second_copy.repository) && os::metered_connection();
    if (copy_waits && !g_json) out("Second copy: skipped, the internet connection is metered\n");
    if (!job->second_copy.repository.empty() && !g_cancel.load() && record.status != jobs::RunStatus::Failed &&
        !copy_waits) {
        jobs::RunRecord copy;
        second_copy(a, store, *job, *repo, copy, !record.alert.empty() || !record.hold.empty());
        try {
            store.append_history(job->id, copy);
        } catch (const Error& e) {
            warning(std::string("could not record the second copy: ") + e.what());
        }
    }

    // Then the health check, when one is due. It never changes the backup's result: a problem
    // it finds is its own entry in the history (and a notification from the tray).
    if (a.has("--no-check") || g_cancel.load()) return code;
    const auto due = jobs::due_check(job->health, jobs::combined_history(store, job->id, 500), os::now());
    if (due) {
        jobs::RunRecord check;
        try {
            health_check(*repo, *job, *due, check);
        } catch (const LockedError&) {
            // Another run is using the repository: try again after the next backup.
            if (!g_json) clear_status();
            warning("the health check was skipped this time: another run is using the repository");
            return code;
        } catch (const std::exception&) {
            // Cancelled or could not run: the record says which.
        }
        try {
            store.append_history(job->id, check);
        } catch (const Error& e) {
            warning(std::string("could not record the health check: ") + e.what());
        }
        alert_for(*job, check, store.dir());
    }

    // And the monthly test restore, when it is on and due. Like the check, its own entry. (Disk
    // images are checked by the health check alone: a test restore restores files.)
    if (g_cancel.load() || !job->image_disks.empty() || !jobs::restore_test_due(job->health, jobs::combined_history(store, job->id, 500), os::now()))
        return code;
    jobs::RunRecord test;
    try {
        restore_check(a, store, *repo, *job, test);
    } catch (const LockedError&) {
        if (!g_json) clear_status();
        warning("the test restore was skipped this time: another run is using the repository");
        return code;
    } catch (const std::exception&) {
        // Cancelled or could not run: the record says which.
    }
    try {
        store.append_history(job->id, test);
    } catch (const Error& e) {
        warning(std::string("could not record the test restore: ") + e.what());
    }
    alert_for(*job, test, store.dir());
    return code;
}

int cmd_forget(const Args& a) {
    ForgetOptions fo;
    fo.policy = retention_arg(a);
    fo.tags = a.all("--tag");
    fo.dry_run = a.has("--dry-run");
    fo.force = a.has("--force");
    if (a.positional.empty() && fo.policy.no_rules())
        throw Error("give snapshot IDs or at least one --keep-... rule; forget never removes every snapshot");
    if (!a.positional.empty() && !fo.policy.no_rules())
        throw Error("give either snapshot IDs or --keep-... rules, not both");
    auto repo = open_repo(a);
    RepoLock lock(*repo, RepoLock::Mode::Exclusive, "forget", lock_wait(a));
    for (const std::string& ref : a.positional) fo.ids.push_back(resolve_snapshot(*repo, ref).id);
    const ForgetResult fr = run_forget(*repo, fo);

    if (g_json) {
        nlohmann::json list = nlohmann::json::array();
        for (const ForgetDecision& d : fr.decisions)
            list.push_back({{"id", id_hex(d.entry.id)}, {"time", d.entry.snapshot.time}, {"keep", d.keep}, {"reason", d.reason}});
        emit({{"event", "forget"}, {"dry_run", fo.dry_run}, {"removed", fr.removed}, {"snapshots", list}});
    } else {
        for (const ForgetDecision& d : fr.decisions) {
            out(std::string(d.keep ? "keep    " : (fo.dry_run ? "remove? " : "removed ")) + short_id(d.entry.id) + "  " +
                os::format_time_local(d.entry.snapshot.time) + "  " + d.reason + "\n");
        }
        out(std::to_string(fr.removed) + " of " + std::to_string(fr.decisions.size()) + " snapshot(s) " +
            (fo.dry_run ? "would be removed" : "removed") +
            (fo.ids.empty() ? " (keeping " + describe(fo.policy) + ")" : std::string()) + "\n");
    }
    if (a.has("--prune") && fr.removed > 0 && !fo.dry_run) {
        const PruneResult pr = prune_and_report(*repo, lock, false);
        if (g_json) {
            emit({{"event", "prune"}, {"freed", space_freed(pr)}});
        } else {
            out("Prune: freed " + format_bytes(space_freed(pr)) + "\n");
        }
    }
    return kExitOk;
}

int cmd_prune(const Args& a) {
    const bool dry = a.has("--dry-run");
    auto repo = open_repo(a);
    RepoLock lock(*repo, RepoLock::Mode::Exclusive, "prune", lock_wait(a));
    const auto start = std::chrono::steady_clock::now();
    const PruneResult r = prune_and_report(*repo, lock, dry);
    if (g_json) {
        emit({{"event", "done"},
              {"status", "success"},
              {"dry_run", dry},
              {"packs", r.packs},
              {"packs_deleted", r.packs_deleted},
              {"packs_repacked", r.packs_repacked},
              {"bytes_before", r.bytes_before},
              {"freed", space_freed(r)}});
        return kExitOk;
    }
    out(std::to_string(r.blobs_used) + " chunk(s) in use, " + std::to_string(r.blobs_unused) + " unused, in " +
        std::to_string(r.packs) + " pack(s) (" + format_bytes(r.bytes_before) + ")\n");
    if (dry) {
        out("Would delete " + std::to_string(r.packs_deleted) + " pack(s) and rewrite " +
            std::to_string(r.packs_repacked) + ", freeing about " + format_bytes(r.bytes_deleted) + "\n");
        return kExitOk;
    }
    out("Deleted " + std::to_string(r.packs_deleted) + " pack(s), rewrote " + std::to_string(r.packs_repacked) +
        " into " + std::to_string(r.packs_written) + "; freed " + format_bytes(space_freed(r)) + " in " +
        format_duration(seconds_since(start)) + "\n");
    return kExitOk;
}

// "repair [--quick] [--dry-run]": see ops/repair.hpp. Opens the repository without its index, so
// a damaged index file cannot stop it.
int cmd_repair(const Args& a) {
    RepairOptions o;
    o.quick = a.has("--quick");
    o.dry_run = a.has("--dry-run");
    o.accept_missing_packs = a.has("--accept-missing-packs");
    auto repo = open_repo(a, false);
    RepoLock lock(*repo, RepoLock::Mode::Exclusive, "repair", lock_wait(a));
    o.should_cancel = [&lock] { return stop_requested() || lock.lost(); };
    o.on_progress = [](const std::string& msg) {
        if (g_json) emit({{"event", "repair_progress"}, {"current", msg}});
        else status("Repair: " + msg);
    };
    o.on_warning = [](const std::string& msg) { warning(msg); };
    const auto start = std::chrono::steady_clock::now();
    if (!g_json) out(o.quick ? "Repair: rebuilding the index from the packs' headers...\n"
                             : "Repair: reading and checking all stored data...\n");
    const RepairResult r = run_repair(*repo, o);
    if (!g_json) clear_status();
    std::string summary = std::to_string(r.blobs) + " intact chunk(s) in " + std::to_string(r.packs) + " pack(s)";
    if (r.packs_rebuilt) summary += ", " + std::to_string(r.packs_rebuilt) + " damaged pack(s) rebuilt from repair data";
    if (r.packs_unreadable) summary += ", " + std::to_string(r.packs_unreadable) + " pack(s) unreadable";
    if (r.packs_missing) summary += ", " + std::to_string(r.packs_missing) + " pack(s) missing";
    if (r.blobs_damaged) summary += ", " + std::to_string(r.blobs_damaged) + " damaged chunk(s)";
    summary += "; " + std::to_string(r.snapshots) + " version(s)";
    if (r.snapshots_repaired)
        summary += ", " + std::to_string(r.snapshots_repaired) + " saved again without " +
                   std::to_string(r.files_lost) + " lost file(s) and " + std::to_string(r.folders_lost) + " folder(s)";
    if (r.snapshots_removed) summary += ", " + std::to_string(r.snapshots_removed) + " removed";
    if (!r.changed()) summary += "; nothing was damaged";
    if (g_json) {
        emit({{"event", "done"}, {"status", "success"}, {"dry_run", o.dry_run}, {"packs", r.packs},
              {"packs_unreadable", r.packs_unreadable}, {"packs_rebuilt", r.packs_rebuilt},
              {"packs_missing", r.packs_missing}, {"blobs", r.blobs},
              {"blobs_damaged", r.blobs_damaged},
              {"snapshots", r.snapshots}, {"snapshots_repaired", r.snapshots_repaired},
              {"snapshots_removed", r.snapshots_removed}, {"files_lost", r.files_lost},
              {"folders_lost", r.folders_lost}, {"message", summary}});
        return kExitOk;
    }
    out((o.dry_run ? "Repair (dry run, nothing changed): " : "Repair done in " + format_duration(seconds_since(start)) + ": ") +
        summary + "\n");
    if (r.changed() && !o.dry_run)
        out("Back up again soon: files still on this computer are stored again by the next backup.\n");
    return kExitOk;
}

int cmd_unlock(const Args& a) {
    auto repo = open_repo(a);
    const std::size_t removed = remove_locks(*repo, a.has("--all"));
    out("Removed " + std::to_string(removed) + " lock(s)\n");
    for (const LockEntry& e : list_locks(*repo))
        out("still held: " + (e.info ? describe(*e.info) : e.name + " (unreadable)") + "\n");
    return kExitOk;
}

// What unlocks the repository for `key`: --recovery-key, or the password from the usual places.
Credential key_credential(const Args& a) {
    // Never taken from the command line, where other programs and the shell history can see it.
    if (a.has("--recovery-key")) {
        std::string key = os::get_env("VAULTIK_RECOVERY_KEY");
        if (key.empty()) key = read_password("Recovery key: ", "no console to ask for the recovery key; set VAULTIK_RECOVERY_KEY");
        return {std::move(key), true};
    }
    return {get_password(a, false), false};
}

bool same_location(const std::string& a, const std::string& b) {
    if (is_cloud_location(a) || is_cloud_location(b)) {
        std::string x = a;
        std::string y = b;
        while (!x.empty() && x.back() == '/') x.pop_back();
        while (!y.empty() && y.back() == '/') y.pop_back();
        return x == y;
    }
    std::string x = os::absolute_path(a);
    std::string y = os::absolute_path(b);
#ifdef _WIN32
    for (auto* s : {&x, &y})
        for (char& c : *s)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
#endif
    while (x.size() > 1 && (x.back() == '\\' || x.back() == '/')) x.pop_back();
    while (y.size() > 1 && (y.back() == '\\' || y.back() == '/')) y.pop_back();
    return x == y;
}

int cmd_key(const Args& a) {
    const std::string sub = a.positional.empty() ? std::string() : a.positional[0];
    if (sub != "passwd" && sub != "recovery") throw Error("usage: vaultik key passwd|recovery -r <repository>");
    if (a.positional.size() > 1)
        throw Error("unexpected \"" + a.positional[1] +
                    "\": --recovery-key asks for the key (or reads VAULTIK_RECOVERY_KEY); keep it off the command line");
    const std::string location = repo_path(a);
    const auto backend_ptr = backend_for(a);
    Backend& backend = *backend_ptr;
    Credential credential = key_credential(a);

    if (sub == "recovery") {
        status("Making a recovery key...");
        const std::string key = create_recovery_key(backend, credential);
        secure_wipe(credential.secret.data(), credential.secret.size());
        out("Recovery key: " + key + "\n");
        out("Write it down and keep it apart from this computer. Any older recovery key no longer works.\n");
        return kExitOk;
    }

    // Check the current password or recovery key before asking for the new password.
    { const MasterKey check = unlock_master_key(backend, credential); }
    std::string pw = os::get_env("VAULTIK_NEW_PASSWORD");
    if (pw.empty()) {
        pw = read_password("New password: ");
        std::string again = read_password("Repeat new password: ");
        const bool same = again == pw;
        secure_wipe(again.data(), again.size());
        if (!same) throw Error("passwords do not match");
    }
    status("Setting the new password...");
    change_password(backend, credential, pw);
    secure_wipe(credential.secret.data(), credential.secret.size());
    out("The password of " + backend.location() + " is changed.\n");

    const jobs::JobStore store;
    for (const jobs::Job& job : store.list()) {
        if (!same_location(job.repository, location)) continue;
        try {
            store.set_password(job.id, pw);
            out("Saved job \"" + job.name + "\" uses the new password.\n");
            // Its scheduled runs read the administrator copy, which needs approval to update.
            if (job.run_as_admin && jobs::has_system_schedule(job.schedule)) {
                jobs::apply_schedule(job, os::parent_of(os::executable_path()));
            }
        } catch (const Error& e) {
            warning("cannot update the password of job \"" + job.name + "\": " + e.what());
        }
    }
    secure_wipe(pw.data(), pw.size());
    return kExitOk;
}

// pin / unpin <snapshot>: a pinned snapshot is never removed by retention or a size limit.
int cmd_pin(const Args& a, bool pin) {
    if (a.positional.size() != 1) throw Error(std::string("usage: vaultik ") + (pin ? "pin" : "unpin") + " -r <repo> <snapshot>");
    auto repo = open_repo(a);
    // Shared: a forget (exclusive) cannot remove the snapshot while it is rewritten.
    RepoLock lock(*repo, RepoLock::Mode::Shared, pin ? "pin" : "unpin", lock_wait(a));
    SnapshotRef ref = resolve_snapshot(*repo, a.positional[0]);
    ref.snapshot.pinned = pin;
    repo->update_snapshot(ref.id, ref.snapshot);
    out("Snapshot " + short_id(ref.id) + (pin ? " is pinned: it stays until it is unpinned.\n" : " is no longer pinned.\n"));
    return kExitOk;
}

// note <snapshot> [text...]: describes a snapshot; no text removes the note.
int cmd_note(const Args& a) {
    if (a.positional.empty()) throw Error("usage: vaultik note -r <repo> <snapshot> [text]");
    std::string text;
    for (std::size_t i = 1; i < a.positional.size(); ++i) text += (i > 1 ? " " : "") + a.positional[i];
    for (char& c : text)
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    if (text.size() > 500) throw Error("a note can be at most 500 characters");
    auto repo = open_repo(a);
    RepoLock lock(*repo, RepoLock::Mode::Shared, "note", lock_wait(a));
    SnapshotRef ref = resolve_snapshot(*repo, a.positional[0]);
    ref.snapshot.note = text;
    repo->update_snapshot(ref.id, ref.snapshot);
    out(text.empty() ? "Note removed from snapshot " + short_id(ref.id) + ".\n"
                     : "Snapshot " + short_id(ref.id) + ": \"" + text + "\"\n");
    return kExitOk;
}

// history <path>: the versions of one file or folder (only those where it changed, unless --all).
// How the drive got worse in the last 90 days, from its stored readings ("2 more reallocated
// sectors since 2026-10-02 (1 -> 3)"); empty when it did not.
std::vector<std::string> drive_changes(const os::DriveHealth& d) {
    std::vector<std::string> out;
    const jobs::DriveReading now = jobs::reading_of(d, os::now());
    for (const jobs::DriveChange& c : jobs::drive_trend(jobs::drive_history(now.key), now))
        out.push_back(jobs::describe(c));
    return out;
}

// A drive's problem in words ("WD My Passport 2627: warning, Predictive failure"); empty when it
// reports none.
std::string drive_problem(const os::DriveHealth& d) {
    const bool bad = d.status == os::DriveHealth::Status::Warning || d.status == os::DriveHealth::Status::Unhealthy;
    const bool uncorrected = d.read_errors_uncorrected.value_or(0) > 0;
    const std::vector<std::string> worse = drive_changes(d);
    if (!bad && d.condition.empty() && !uncorrected && !d.worn_out() && worse.empty()) return {};
    std::string s = (d.model.empty() ? "disk " + std::to_string(d.disk) : d.model) + ": " + os::to_string(d.status);
    if (!d.condition.empty()) s += ", " + d.condition;
    if (uncorrected) s += ", " + std::to_string(*d.read_errors_uncorrected) + " unrecoverable read errors";
    if (d.worn_out()) s += ", " + std::to_string(*d.health_percent()) + "% of its rated life left";
    for (const std::string& w : worse) s += ", " + w;
    return s;
}

// "drives [--smart]": the health of this computer's drives, as the system reports it, and with
// --smart each ATA drive's attribute table.
int cmd_drives(const Args& a) {
    const bool smart = a.has("--smart");
    const bool history = a.has("--history");
    const std::vector<os::DriveHealth> drives = os::all_drives();
    jobs::record_drives(drives);
    // Which drive letters are on each disk.
    std::map<int, std::string> letters;
#ifdef _WIN32
    for (char c = 'A'; c <= 'Z'; ++c) {
        const std::string root = std::string(1, c) + ":\\";
        if (!os::exists(root)) continue;
        for (const int n : os::disk_numbers(root)) letters[n] += (letters[n].empty() ? "" : " ") + root.substr(0, 2);
    }
#else
    // The mount points of the file systems on each disk (not bind mounts or kernel file systems).
    for (const os::mounts::Mount& m : os::mounts::read_mounts()) {
        if (m.root != "/" || !m.source.starts_with("/dev/")) continue;
        for (const int n : os::disk_numbers(m.point)) {
            std::string& list = letters[n];
            if ((" " + list + " ").find(" " + m.point + " ") == std::string::npos) list += (list.empty() ? "" : " ") + m.point;
        }
    }
#endif
    if (g_json) {
        nlohmann::json list = nlohmann::json::array();
        for (const os::DriveHealth& d : drives) {
            nlohmann::json j = {{"disk", d.disk}, {"model", d.model}, {"bus", d.bus}, {"media", d.media},
                                {"size", d.size}, {"status", os::to_string(d.status)}, {"condition", d.condition},
                                {"volumes", letters[d.disk]}};
            if (d.temperature) j["temperature"] = *d.temperature;
            if (d.wear) j["wear"] = *d.wear;
            if (const auto p = d.health_percent()) j["health_percent"] = *p;
            if (d.power_on_hours) j["power_on_hours"] = *d.power_on_hours;
            if (d.read_errors_uncorrected) j["read_errors_uncorrected"] = *d.read_errors_uncorrected;
            if (!d.smart_issues.empty()) j["issues"] = d.smart_issues;
            if (!d.serial.empty()) j["serial"] = d.serial;
            if (!d.device.empty()) j["device"] = d.device;
            if (!d.unknown_reason.empty()) j["unknown_reason"] = d.unknown_reason;
            if (const auto worse = drive_changes(d); !worse.empty()) j["worse"] = worse;
            if (history) {
                nlohmann::json readings = nlohmann::json::array();
                for (const jobs::DriveReading& r : jobs::drive_history(jobs::drive_key(d))) {
                    nlohmann::json e = {{"time", os::format_time_local(r.time)}};
                    if (r.health) e["health_percent"] = *r.health;
                    if (r.reallocated) e["reallocated"] = *r.reallocated;
                    if (r.pending) e["pending"] = *r.pending;
                    if (r.uncorrectable) e["uncorrectable"] = *r.uncorrectable;
                    if (r.read_errors) e["read_errors"] = *r.read_errors;
                    readings.push_back(e);
                }
                j["history"] = readings;
            }
            if (smart && !d.smart.empty()) {
                nlohmann::json attrs = nlohmann::json::array();
                for (const os::SmartAttribute& s : d.smart)
                    attrs.push_back({{"id", s.id}, {"name", os::smart_attribute_name(s.id)}, {"value", s.value},
                                     {"worst", s.worst}, {"threshold", s.threshold}, {"raw", s.raw},
                                     {"prefail", s.prefail}});
                j["smart"] = attrs;
            }
            list.push_back(j);
        }
        emit({{"event", "drives"}, {"drives", list}});
        return kExitOk;
    }
    if (drives.empty()) {
        out("The system reported no drives (or cannot tell).\n");
        return kExitOk;
    }
    bool missing = false;
    for (const os::DriveHealth& d : drives) {
        std::string line = (d.device.empty() ? "Disk " + std::to_string(d.disk) : d.device) + "  " + d.model + "  (" + d.bus +
                           (d.media.empty() ? "" : " " + d.media) + ", " + format_bytes(d.size) + ")";
        if (!letters[d.disk].empty()) line += "  " + letters[d.disk];
        line += "\n  " + std::string(os::to_string(d.status));
        if (const auto p = d.health_percent()) line += ", health " + std::to_string(*p) + "%";
        if (!d.condition.empty()) line += ", " + d.condition;
        if (d.temperature) line += ", " + std::to_string(*d.temperature) + " C";
        if (d.power_on_hours) line += ", " + std::to_string(*d.power_on_hours) + " hours on";
        if (d.read_errors_uncorrected.value_or(0) > 0 && d.smart.empty())  // (else among the issues)
            line += ", " + std::to_string(*d.read_errors_uncorrected) + " unrecoverable read errors";
        for (const std::string& issue : d.smart_issues)
            if (issue != d.condition) line += ", " + issue;
        if (!d.unknown_reason.empty()) line += " (" + d.unknown_reason + ")";
        missing = missing || !d.health_percent() || !d.power_on_hours;
        out(line + "\n");
        for (const std::string& w : drive_changes(d)) out("  Getting worse: " + w + "\n");
        if (history) {
            const std::vector<jobs::DriveReading> readings = jobs::drive_history(jobs::drive_key(d));
            if (readings.empty()) out("    (no stored readings)\n");
            for (const jobs::DriveReading& r : readings) {
                std::string row = "    " + os::format_time_local(r.time);
                if (r.health) row += "  health " + std::to_string(*r.health) + "%";
                if (r.reallocated) row += "  reallocated " + std::to_string(*r.reallocated);
                if (r.pending) row += "  pending " + std::to_string(*r.pending);
                if (r.uncorrectable) row += "  uncorrectable " + std::to_string(*r.uncorrectable);
                if (r.read_errors) row += "  read errors " + std::to_string(*r.read_errors);
                out(row + "\n");
            }
        }
        if (smart && !d.smart.empty()) {
            out("    ID  Attribute                       Value Worst Thresh  Raw\n");
            for (const os::SmartAttribute& s : d.smart) {
                const char* name = os::smart_attribute_name(s.id);
                char row[160];
                std::snprintf(row, sizeof row, "   %3d  %-30s  %4d  %4d  %4d   %llu%s\n", s.id, *name ? name : "?",
                              s.value, s.worst, s.threshold, static_cast<unsigned long long>(s.raw),
                              s.prefail ? "  (pre-fail)" : "");
                out(row);
            }
        } else if (smart && d.bus != "NVMe") {
#ifdef _WIN32
            out(os::process_elevated() ? "    (the drive or its enclosure did not hand out its S.M.A.R.T. attributes)\n"
                                       : "    (S.M.A.R.T. attributes need administrator rights)\n");
#else
            if (d.unknown_reason.empty()) out("    (no S.M.A.R.T. attribute table)\n");
#endif
        }
    }
#ifdef _WIN32
    if (missing && !os::process_elevated())
        out("(Run as administrator for the health percentage of hard disks and SATA SSDs, power-on hours\n"
            " and error counts.)\n");
#else
    (void)missing;  // the reason is on each drive's line
#endif
    return kExitOk;
}

// "estimate <path>...": how much a first backup of these paths reads, and roughly how long it takes.
int cmd_estimate(const Args& a) {
    std::vector<std::string> paths = a.positional;
    std::vector<std::string> excludes = a.all("--exclude");
    if (const auto job = job_arg(a)) {
        if (paths.empty()) paths = job->sources;
        excludes.insert(excludes.end(), job->excludes.begin(), job->excludes.end());
    }
    if (paths.empty()) throw Error("usage: vaultik estimate <path>... [--exclude <pattern>] (or --job <job>)");
    const SourceEstimate e = estimate_sources(paths, excludes, [] { return g_cancel.load(); },
                                              [](const SourceEstimate& so_far) {
                                                  if (!g_json)
                                                      status(std::to_string(so_far.files) + " files, " +
                                                             format_bytes(so_far.bytes) + "...");
                                              });
    clear_status();
    // Typical speeds: an external hard disk, and two internet uploads (Mbit/s / 8 = MB/s).
    const auto secs = [&](double mb_per_s) { return static_cast<double>(e.bytes) / (mb_per_s * 1e6); };
    if (g_json) {
        emit({{"event", "done"}, {"status", "success"}, {"files", e.files}, {"dirs", e.dirs}, {"bytes", e.bytes},
              {"unreadable", e.unreadable}, {"seconds_drive", secs(100)}, {"seconds_upload_20", secs(2.5)},
              {"seconds_upload_100", secs(12.5)}});
        return kExitOk;
    }
    out(std::to_string(e.files) + " files in " + std::to_string(e.dirs) + " folders, " + format_bytes(e.bytes) + "\n");
    if (e.unreadable > 0) out(std::to_string(e.unreadable) + " folder(s) could not be read\n");
    out("First backup, roughly: " + format_duration(secs(100)) + " to an external drive (100 MB/s), " +
        format_duration(secs(12.5)) + " uploading at 100 Mbit/s, " + format_duration(secs(2.5)) +
        " at 20 Mbit/s.\nIt stores less after compression and deduplication; later backups only read what "
        "changed.\n");
    return kExitOk;
}

// "diff <older> [<newer>]": what changed between two snapshots (one: since its parent).
int cmd_diff(const Args& a) {
    if (a.positional.empty() || a.positional.size() > 2)
        throw Error("usage: vaultik diff -r <repo> <older snapshot> [<newer snapshot>]");
    auto repo = open_repo(a);
    SnapshotRef older, newer;
    if (a.positional.size() == 2) {
        older = resolve_snapshot(*repo, a.positional[0]);
        newer = resolve_snapshot(*repo, a.positional[1]);
    } else {
        newer = resolve_snapshot(*repo, a.positional[0]);
        if (!newer.snapshot.parent) throw Error("snapshot " + short_id(newer.id) + " has no earlier one to compare with");
        older = resolve_snapshot(*repo, id_hex(*newer.snapshot.parent));
    }
    status("Comparing...");
    const DiffResult d = diff_snapshots(*repo, older.snapshot, newer.snapshot, 100000, [] { return g_cancel.load(); });
    clear_status();
    const auto mark = [](Change::Kind k) { return k == Change::Kind::Added ? "+" : k == Change::Kind::Removed ? "-" : "M"; };
    for (const Change& c : d.changes) {
        const bool dir = c.node_kind == NodeKind::Dir;
        if (g_json) {
            emit({{"event", "change"},
                  {"kind", c.kind == Change::Kind::Added ? "added" : c.kind == Change::Kind::Removed ? "removed" : "changed"},
                  {"path", c.path},
                  {"dir", dir},
                  {"old_size", c.old_size},
                  {"new_size", c.new_size}});
        } else {
            std::string line = std::string(mark(c.kind)) + " " + c.path + (dir ? "/" : "");
            if (c.kind == Change::Kind::Changed) line += "  (" + format_bytes(c.old_size) + " -> " + format_bytes(c.new_size) + ")";
            out(line + "\n");
        }
    }
    if (g_json) {
        emit({{"event", "done"}, {"status", "success"}, {"added", d.added}, {"removed", d.removed},
              {"changed", d.changed}, {"truncated", d.truncated}});
    } else {
        out(short_id(older.id) + " -> " + short_id(newer.id) + ": " + std::to_string(d.added) + " added, " +
            std::to_string(d.removed) + " removed, " + std::to_string(d.changed) + " changed" +
            (d.truncated ? " (list cut short)" : "") + "\n");
    }
    return kExitOk;
}

int cmd_history(const Args& a) {
    if (a.positional.size() != 1) throw Error("usage: vaultik history -r <repo> <path>   e.g. C/Users/me/report.docx");
    std::string path = a.positional[0];
    // A backslash separates folders on Windows only; on Linux it can be part of a name.
    if (os::kSeparator == '\\') std::replace(path.begin(), path.end(), '\\', '/');
    auto repo = open_repo(a);
    const std::vector<SnapshotRecord> snaps = load_snapshot_records(*repo);
    const std::vector<ItemVersion> versions = item_history(*repo, snaps, path);
    const bool all = a.has("--all");
    if (g_json) {
        nlohmann::json list = nlohmann::json::array();
        for (const ItemVersion& v : versions) {
            if (!all && !v.changed) continue;
            const SnapshotRecord& s = snaps[v.snapshot];
            list.push_back({{"snapshot", id_hex(s.id)},
                            {"time", s.snapshot.time},
                            {"size", v.node.size},
                            {"modified", v.node.mtime},
                            {"changed", v.changed}});
        }
        emit({{"event", "history"}, {"path", path}, {"versions", list}});
        return kExitOk;
    }
    if (versions.empty()) {
        out("No snapshot has " + path + "\n");
        return kExitOk;
    }
    out(pad("Snapshot", 10) + pad("Backed up", 21) + pad("Modified", 21) + rpad("Size", 12) + "\n");
    std::size_t shown = 0;
    for (const ItemVersion& v : versions) {
        if (!all && !v.changed) continue;
        const SnapshotRecord& s = snaps[v.snapshot];
        out(pad(short_id(s.id), 10) + pad(os::format_time_local(s.snapshot.time), 21) +
            pad(os::format_time_local(v.node.mtime), 21) + rpad(format_bytes(v.node.size), 12) +
            (v.changed ? "" : "  (same)") + "\n");
        ++shown;
    }
    out(std::to_string(shown) + " version(s)" + (all ? "" : " where it changed") + ", in " +
        std::to_string(versions.size()) + " of " + std::to_string(snaps.size()) + " snapshot(s)\n");
    return kExitOk;
}

// find <pattern>: files and folders by name in every snapshot.
int cmd_find(const Args& a) {
    if (a.positional.size() != 1) throw Error("usage: vaultik find -r <repo> <name or pattern>   e.g. \"budget*.xlsx\"");
    auto repo = open_repo(a);
    const std::vector<SnapshotRecord> snaps = load_snapshot_records(*repo);
    bool truncated = false;
    const std::vector<FoundItem> items = find_items(*repo, snaps, a.positional[0], 500, &truncated);
    if (g_json) {
        nlohmann::json list = nlohmann::json::array();
        for (const FoundItem& f : items) {
            list.push_back({{"path", f.path},
                            {"folder", f.kind == NodeKind::Dir},
                            {"size", f.size},
                            {"snapshots", f.snapshots.size()},
                            {"newest", snaps[f.snapshots.front()].snapshot.time}});
        }
        emit({{"event", "find"}, {"items", list}, {"truncated", truncated}});
        return kExitOk;
    }
    for (const FoundItem& f : items) {
        const std::string newest = os::format_time_local(snaps[f.snapshots.front()].snapshot.time);
        out((f.kind == NodeKind::Dir ? std::string("d ") : std::string("- ")) + rpad(format_bytes(f.size), 11) + "  " +
            pad(newest, 20) + pad(std::to_string(f.snapshots.size()) + " snap", 9) + f.path + "\n");
    }
    out(std::to_string(items.size()) + " match(es)" + (truncated ? " (more exist; narrow the pattern)" : "") + "\n");
    return kExitOk;
}

// "computers --poll [--cloud]": the jobs other computers asked this one to run now, from the
// requests in the repositories of the saved jobs (those with a stored password, without
// administrator rights; their drives connected). Cloud storage only with --cloud: the frontend
// asks for it less often, since every look costs a few requests there. Prints
// {"event": "requests", "jobs": [ids]} (or one id per line); never fails on one repository.
int poll_requests(const Args& a) {
    const jobs::JobStore store = job_store(a);
    std::map<std::string, std::vector<jobs::Job>> by_repo;
    // Every job a repository has here, those with administrator rights too: a request for one of
    // them is answered by its own run, not removed as unknown.
    std::map<std::string, std::vector<std::string>> known_by_repo;
    for (const jobs::Job& job : store.list()) {
        known_by_repo[job.repository].push_back(job.id);
        if (job.run_as_admin || (is_cloud_location(job.repository) && !a.has("--cloud"))) continue;
        if (!jobs::drive_connected(job)) continue;
        by_repo[job.repository].push_back(job);
    }
    std::vector<std::string> asked;
    std::vector<std::string> failed;
    for (const auto& [location, list] : by_repo) {
        std::string pw = store.password(list.front().id);
        if (pw.empty()) continue;
        try {
            const CloudCredentials cloud = store.cloud_credentials(list.front());
            auto repo = Repository::open(open_backend(location, &cloud), pw, false);
            secure_wipe(pw.data(), pw.size());
            std::vector<std::string> local;
            for (const jobs::Job& j : list) local.push_back(j.id);
            for (const std::string& id : jobs::take_request(*repo, known_by_repo[location], local, os::now(), store.dir()))
                asked.push_back(id);
        } catch (const std::exception& e) {
            secure_wipe(pw.data(), pw.size());
            failed.push_back(location + ": " + e.what());
        }
    }
    if (g_json) {
        emit({{"event", "requests"}, {"jobs", asked}, {"failed", failed}});
    } else {
        for (const std::string& id : asked) out(id + "\n");
        for (const std::string& f : failed) err("could not look for requests in " + f + "\n");
    }
    return kExitOk;
}

// "computers": every computer backing up into the repository, from their status records and the
// snapshots' host names. --forget removes a retired computer's record, --ask asks one to back up.
int cmd_computers(const Args& a) {
    if (a.has("--poll")) return poll_requests(a);
    if (const auto what = a.value("--forget")) {
        auto repo = open_repo(a, false);
        const std::string host = jobs::forget_computer(*repo, *what);
        if (g_json) emit({{"event", "done"}, {"status", "success"}, {"forgot", host}});
        else out("Removed the status note of " + host + " (its versions stay).\n");
        return kExitOk;
    }
    if (const auto what = a.value("--ask")) {
        auto repo = open_repo(a, false);
        const std::vector<jobs::StatusFile> statuses = jobs::list_statuses(*repo);
        const std::string name = jobs::resolve_computer(statuses, *what);
        const auto it = std::find_if(statuses.begin(), statuses.end(), [&](const jobs::StatusFile& f) { return f.name == name; });
        jobs::ask_backup(*repo, *it, a.all("--ask-job"), os::hostname(), os::now());
        const std::string host = it->status ? it->status->host : std::string("?");
        if (g_json) emit({{"event", "done"}, {"status", "success"}, {"asked", host}});
        else out("Asked " + host + " to back up now. It does when one of its jobs here runs, or when Vaultik there next looks.\n");
        return kExitOk;
    }
    auto repo = open_repo(a);
    const std::uint64_t now = os::now();
    const std::vector<jobs::Computer> list = jobs::list_computers(*repo, now);
    const auto time_or_null = [](std::uint64_t t) { return t == 0 ? nlohmann::json() : nlohmann::json(t); };
    if (g_json) {
        nlohmann::json computers = nlohmann::json::array();
        for (const jobs::Computer& c : list) {
            nlohmann::json j = {{"host", c.host},
                                {"this_computer", c.this_computer},
                                {"health", jobs::to_string(c.health)},
                                {"versions", c.versions},
                                {"latest", time_or_null(c.latest)},
                                {"latest_bytes", c.latest_bytes},
                                {"added_bytes", c.added_bytes},
                                {"record", c.status_name.empty() ? nlohmann::json() : nlohmann::json(c.status_name)},
                                {"record_behind", c.record_behind}};
            if (c.request)
                j["request"] = {{"from", c.request->from}, {"created", c.request->created},
                                {"jobs", c.request->jobs}, {"state", c.request->state}, {"started", c.request->started}};
            if (c.status) {
                j["version"] = c.status->version;
                j["updated"] = c.status->updated;
                nlohmann::json js = nlohmann::json::array();
                for (const jobs::JobStatus& s : c.status->jobs) {
                    nlohmann::json o = nlohmann::json::parse(jobs::to_json(s));
                    o["health"] = jobs::to_string(jobs::job_health(s, now));
                    js.push_back(o);
                }
                j["jobs"] = js;
            } else if (!c.status_name.empty()) {
                j["error"] = "the status record cannot be read";
            }
            computers.push_back(j);
        }
        emit({{"event", "computers"}, {"computers", computers}});
        return kExitOk;
    }
    if (list.empty()) {
        out("No computers have backed up here yet.\n");
        return kExitOk;
    }
    out(pad("Computer", 20) + pad("State", 9) + pad("Newest version", 21) + rpad("Versions", 9) + rpad("Size", 12) +
        "\n");
    for (const jobs::Computer& c : list) {
        std::string host = c.host + (c.this_computer ? " (this)" : "");
        std::string line = pad(host, 20) + pad(jobs::to_string(c.health), 9) +
                           pad(c.latest ? os::format_time_local(c.latest) : "-", 21) +
                           rpad(std::to_string(c.versions), 9) + rpad(c.latest ? format_bytes(c.latest_bytes) : "-", 12);
        if (!c.status) {
            line += c.status_name.empty() ? "  (no status record)" : "  (status record unreadable)";
        } else if (c.record_behind) {
            line += "  (status record older than its newest version)";
        }
        out(line + "\n");
        if (c.request)
            out("    asked to back up by " + c.request->from + " " + os::format_time_local(c.request->created) +
                (c.request->state == "running" ? ", taken up " + os::format_time_local(c.request->started) : ", waiting") +
                "\n");
        if (!c.status) continue;
        for (const jobs::JobStatus& s : c.status->jobs) {
            std::string row = "    " + s.name + ": " + jobs::to_string(jobs::job_health(s, now));
            if (s.last_run) row += ", last " + s.last_kind + " " + os::format_time_local(s.last_run) + " " + s.last_result;
            if (s.last_success) row += ", last saved " + os::format_time_local(s.last_success);
            row += s.next_run ? ", next " + os::format_time_local(s.next_run) : ", " + s.schedule;
            out(row + "\n");
            for (const std::string& w : s.warnings) out("      ! " + w + "\n");
        }
    }
    out(std::to_string(list.size()) + " computer(s)\n");
    return kExitOk;
}

int cmd_snapshots(const Args& a) {
    auto repo = open_repo(a);
    const auto all = load_all_snapshots(*repo);
    if (all.empty()) {
        out("No snapshots.\n");
        return kExitOk;
    }
    out(pad("ID", 10) + pad("Time", 21) + pad("Host", 16) + rpad("Size", 12) + rpad("Added", 12) + "  Paths\n");
    for (const auto& [id, s] : all) {
        std::string paths;
        for (const auto& p : s.paths) paths += (paths.empty() ? "" : ", ") + p;
        std::string line = pad(short_id(id), 10) + pad(os::format_time_local(s.time), 21) + pad(s.hostname, 16) +
                           rpad(format_bytes(s.stats.bytes), 12) + rpad(format_bytes(s.stats.stored_bytes), 12) +
                           "  " + paths;
        if (!s.tags.empty()) {
            line += "  [";
            for (std::size_t i = 0; i < s.tags.size(); ++i) line += (i ? "," : "") + s.tags[i];
            line += "]";
        }
        if (s.pinned) line += "  pinned";
        if (!s.note.empty()) line += "  \"" + s.note + "\"";
        out(line + "\n");
    }
    out(std::to_string(all.size()) + " snapshot(s)\n");
    return kExitOk;
}

void list_tree(Repository& repo, const Id& tree_id, const std::string& prefix, bool recursive) {
    const Tree tree = parse_tree(repo.load_blob(BlobType::Tree, tree_id));
    for (const Node& n : tree.nodes) {
        const char kind = n.kind == NodeKind::Dir       ? 'd'
                          : n.kind == NodeKind::Symlink ? 'l'
                          : n.kind == NodeKind::Reparse ? 'r'
                                                        : '-';
        const std::string path = prefix.empty() ? n.name : prefix + "/" + n.name;
        const std::string suffix = n.kind == NodeKind::Dir       ? "/"
                                   : n.kind == NodeKind::Symlink ? " -> " + n.link_target
                                                                 : "";
        out(std::string(1, kind) + " " + rpad(n.kind == NodeKind::File ? format_bytes(n.size) : "", 11) + "  " +
            pad(os::format_time_local(n.mtime), 20) + path + suffix + "\n");
        if (recursive && n.kind == NodeKind::Dir) list_tree(repo, n.subtree, path, true);
    }
}

int cmd_ls(const Args& a) {
    if (a.positional.empty()) throw Error("usage: vaultik ls -r <repo> <snapshot> [path]");
    auto repo = open_repo(a);
    RepoLock lock(*repo, RepoLock::Mode::Shared, "ls", lock_wait(a));
    repo->reload_index();  // a prune may have finished before the lock was granted
    const SnapshotRef ref = resolve_snapshot(*repo, a.positional[0]);
    const std::string path = a.positional.size() > 1 ? a.positional[1] : std::string();
    Node node;
    if (!resolve_snapshot_path(*repo, ref.snapshot, path, node)) throw Error("path not found in snapshot: " + path);
    if (node.kind == NodeKind::Dir) {
        std::string prefix = path;
        if (os::kSeparator == '\\') std::replace(prefix.begin(), prefix.end(), '\\', '/');
        while (!prefix.empty() && prefix.back() == '/') prefix.pop_back();
        list_tree(*repo, node.subtree, prefix, a.has("--recursive"));
    } else {
        out("- " + rpad(format_bytes(node.size), 11) + "  " + pad(os::format_time_local(node.mtime), 20) + path +
            "\n");
    }
    return kExitOk;
}

// "wsl list" and "wsl import <snapshot> <distro> --name <new> --to <folder> [--replace]".
int cmd_wsl(const Args& a) {
    const std::string sub = a.positional.empty() ? std::string() : a.positional[0];
    if (sub == "list") {
        const std::vector<WslDistro> distros = wsl_distros();
        std::vector<std::string> running;
        std::string unknown;
        try {
            running = wsl_running();
        } catch (const std::exception& e) {
            unknown = e.what();
        }
        const auto is_running = [&](const std::string& n) {
            return std::any_of(running.begin(), running.end(),
                               [&](const std::string& r) { return os::fold_case(r) == os::fold_case(n); });
        };
        if (g_json) {
            for (const WslDistro& d : distros)
                emit({{"event", "distro"},
                      {"name", d.name},
                      {"version", d.version},
                      {"folder", d.folder},
                      {"disk", d.disk},
                      {"running", unknown.empty() ? nlohmann::json(is_running(d.name)) : nlohmann::json(nullptr)}});
            emit({{"event", "done"}, {"status", "success"}, {"message", unknown}});
            return kExitOk;
        }
        if (distros.empty()) out("No WSL distributions are installed for this account.\n");
        for (const WslDistro& d : distros)
            out(pad(d.name, 24) + pad("WSL " + std::to_string(d.version), 7) +
                pad(unknown.empty() ? (is_running(d.name) ? "running" : "stopped") : "?", 9) +
                (d.disk.empty() ? d.folder : d.disk) + "\n");
        if (!unknown.empty()) warning("could not tell which distributions are running: " + unknown);
        return kExitOk;
    }
    if (sub != "import" || a.positional.size() < 3)
        throw Error("usage: vaultik wsl list | wsl import <snapshot> <distro> --name <new name> --to <folder> [--replace]");
    const auto name = a.value("--name");
    const auto to = a.value("--to");
    if (!name || !to) throw Error("wsl import needs --name <new name> and --to <folder>");
    auto repo = open_repo(a);
    RepoLock lock(*repo, RepoLock::Mode::Shared, "wsl import", lock_wait(a));
    repo->reload_index();
    const SnapshotRef ref = resolve_snapshot(*repo, a.positional[1]);
    // The distribution's name, or its path in the version ("WSL/Ubuntu.tar").
    const std::string from =
        distro_of_path(a.positional[2]).empty() ? a.positional[2] : distro_of_path(a.positional[2]);
    Node node;
    if (!resolve_snapshot_path(*repo, ref.snapshot, wsl_snapshot_path(from), node) || node.kind != NodeKind::File)
        throw Error("snapshot " + short_id(ref.id) + " holds no WSL distribution called " + from);
    const auto start = std::chrono::steady_clock::now();
    if (!g_json) err("Importing " + wsl_snapshot_path(from) + " as the WSL distribution " + *name + " in " + *to + "\n");
    wsl_import(
        *repo, node, *name, *to, a.has("--replace"),
        [&](const VirtProgress& p) {
            if (g_json) {
                emit({{"event", "progress"}, {"files", 0}, {"bytes", p.bytes}, {"total", p.total}, {"current", *name}});
                return;
            }
            status(format_bytes(p.bytes) + " of " + format_bytes(p.total));
        },
        [&lock] { return stop_requested() || lock.lost(); });
    if (g_json) {
        emit({{"event", "done"},
              {"status", "success"},
              {"files", 1},
              {"bytes", node.size},
              {"name", *name},
              {"seconds", seconds_since(start)}});
        return kExitOk;
    }
    clear_status();
    out("Imported the WSL distribution " + *name + " (" + format_bytes(node.size) + ") in " +
        format_duration(seconds_since(start)) + "; start it with: wsl -d " + *name + "\n");
    return kExitOk;
}

// "docker list" and "docker restore <snapshot> <volume> --name <new> [--image <image>]".
int cmd_docker(const Args& a) {
    const std::string sub = a.positional.empty() ? std::string() : a.positional[0];
    if (sub == "list") {
        const DockerInfo info = docker_info();
        std::vector<DockerVolume> volumes;
        std::string problem = info.problem;
        if (info.available) {
            try {
                volumes = docker_volumes(info);
            } catch (const std::exception& e) {
                problem = e.what();
            }
        }
        if (g_json) {
            for (const DockerVolume& v : volumes)
                emit({{"event", "volume"}, {"name", v.name}, {"driver", v.driver}, {"running", v.running}});
            emit({{"event", "done"}, {"status", problem.empty() ? "success" : "failed"}, {"message", problem}});
            return problem.empty() ? kExitOk : kExitError;
        }
        if (!problem.empty()) throw Error(problem);
        if (volumes.empty()) out("There are no Docker volumes (" + info.os_type + " containers).\n");
        for (const DockerVolume& v : volumes) {
            std::string users;
            for (const std::string& c : v.running) users += (users.empty() ? "" : ", ") + c;
            out(pad(v.name, 40) + pad(v.driver, 8) + (users.empty() ? "" : "used by " + users) + "\n");
        }
        return kExitOk;
    }
    if (sub != "restore" || a.positional.size() < 3)
        throw Error("usage: vaultik docker list | docker restore <snapshot> <volume> --name <new volume> [--image <image>]");
    const auto name = a.value("--name");
    if (!name) throw Error("docker restore needs --name <new volume> (an existing volume is never written to)");
    if (!valid_volume_name(*name)) throw Error("invalid Docker volume name \"" + *name + "\"");
    if (const auto image = a.value("--image"); image && !image->empty() && !valid_image_reference(*image))
        throw Error("invalid Docker image name \"" + *image + "\"");
    auto repo = open_repo(a);
    RepoLock lock(*repo, RepoLock::Mode::Shared, "docker restore", lock_wait(a));
    repo->reload_index();
    const SnapshotRef ref = resolve_snapshot(*repo, a.positional[1]);
    // The volume's name, or its path in the version ("Docker/data.tar").
    const std::string from =
        volume_of_path(a.positional[2]).empty() ? a.positional[2] : volume_of_path(a.positional[2]);
    Node node;
    if (!resolve_snapshot_path(*repo, ref.snapshot, volume_snapshot_path(from), node) || node.kind != NodeKind::File)
        throw Error("snapshot " + short_id(ref.id) + " holds no Docker volume called " + from);
    const auto start = std::chrono::steady_clock::now();
    if (!g_json) err("Importing " + volume_snapshot_path(from) + " into the new Docker volume " + *name + "\n");
    docker_import(
        *repo, node, *name, a.value("--image").value_or(""),
        [&](const VirtProgress& p) {
            if (g_json) {
                emit({{"event", "progress"}, {"files", 0}, {"bytes", p.bytes}, {"total", p.total}, {"current", *name}});
                return;
            }
            status(format_bytes(p.bytes) + " of " + format_bytes(p.total));
        },
        [&lock] { return stop_requested() || lock.lost(); });
    if (g_json) {
        emit({{"event", "done"},
              {"status", "success"},
              {"files", 1},
              {"bytes", node.size},
              {"name", *name},
              {"seconds", seconds_since(start)}});
        return kExitOk;
    }
    clear_status();
    out("Imported the Docker volume " + *name + " (" + format_bytes(node.size) + ") in " +
        format_duration(seconds_since(start)) + "\n");
    return kExitOk;
}

// "db dump <spec> --to <folder>": one dump now, as a backup makes it.
int cmd_db(const Args& a) {
    const std::string sub = a.positional.empty() ? std::string() : a.positional[0];
    if (sub != "dump" || a.positional.size() < 2) throw Error("usage: vaultik db dump <spec> --to <folder>");
    const auto to = a.value("--to");
    if (!to) throw Error("db dump needs --to <folder>");
    const DatabaseDump db = parse_database_spec(a.positional[1]);
    std::string password = os::get_env("VAULTIK_DB_PASSWORD");
    if (!g_json) status("Dumping " + describe(db) + "...");
    std::string file;
    try {
        file = run_dump(db, password, os::absolute_path(*to), [] { return stop_requested(); });
    } catch (...) {
        secure_wipe(password.data(), password.size());
        if (!g_json) clear_status();
        throw;
    }
    secure_wipe(password.data(), password.size());
    if (g_json) {
        emit({{"event", "done"}, {"status", "success"}, {"file", file}});
        return kExitOk;
    }
    clear_status();
    out("Dumped " + describe(db) + " into " + file + "\n");
    return kExitOk;
}

void write_text(const std::string& path, const std::string& text) {
    os::write_file_atomic(path, ByteSpan(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

// "hyperv list", "hyperv import <folder> --to <folder> [--name <n>] [--report <file>]" and
// "hyperv restore <snapshot> <machine> --to <folder> [--name <n>]".
int cmd_hyperv(const Args& a) {
    const std::string sub = a.positional.empty() ? std::string() : a.positional[0];
    if (sub == "list") {
        const HyperVList list = hyperv_machines();
        if (g_json) {
            for (const HyperVm& m : list.machines)
                emit({{"event", "machine"}, {"name", m.name}, {"state", m.state}, {"disks", m.disks}});
            emit({{"event", "done"}, {"status", list.available ? "success" : "failed"}, {"message", list.problem}});
            return list.available ? kExitOk : kExitError;
        }
        if (!list.available) throw Error(list.problem);
        if (list.machines.empty()) out("There are no Hyper-V virtual machines on this computer.\n");
        for (const HyperVm& m : list.machines) {
            out(pad(m.name, 32) + m.state + "\n");
            for (const std::string& d : m.disks) out("    " + d + "\n");
        }
        return kExitOk;
    }
    const auto to = a.value("--to");
    const std::string name = a.value("--name").value_or("");
    if (sub == "import" && a.positional.size() >= 2) {
        // The frontend runs this with administrator rights and reads the result from --report.
        const auto report = a.value("--report");
        try {
            if (!to) throw Error("hyperv import needs --to <folder> for the machine's files");
            if (!os::process_elevated()) throw Error("importing a Hyper-V virtual machine needs administrator rights");
            const std::string made = hyperv_import(a.positional[1], name, *to);
            if (report) write_text(*report, "ok\n" + made);
            if (g_json) emit({{"event", "done"}, {"status", "success"}, {"name", made}});
            else out("Imported the virtual machine " + made + " into " + *to + "\n");
            return kExitOk;
        } catch (const std::exception& e) {
            if (report) {
                try {
                    write_text(*report, std::string("error\n") + e.what());
                } catch (const std::exception&) {
                }
            }
            throw;
        }
    }
    if (sub != "restore" || a.positional.size() < 3)
        throw Error("usage: vaultik hyperv list | hyperv restore <snapshot> <machine> --to <folder> [--name <name>] | "
                    "hyperv import <folder> --to <folder> [--name <name>]");
    if (!to) throw Error("hyperv restore needs --to <folder>");
    if (!os::process_elevated()) throw Error("importing a Hyper-V virtual machine needs administrator rights");
    const std::string target = os::absolute_path(*to);
    const std::string exported = os::join(target, "Exported");
    const std::string machine_dir = os::join(target, "Machine");
    if (os::exists(exported) || os::exists(machine_dir))
        throw Error(target + " has an Exported or Machine folder already; choose a new or empty folder");
    auto repo = open_repo(a);
    RepoLock lock(*repo, RepoLock::Mode::Shared, "hyperv restore", lock_wait(a));
    repo->reload_index();
    const SnapshotRef ref = resolve_snapshot(*repo, a.positional[1]);
    const std::string machine =
        machine_of_path(a.positional[2]).empty() ? a.positional[2] : machine_of_path(a.positional[2]);
    const std::string path = hyperv_snapshot_path(machine);
    Node node;
    if (!resolve_snapshot_path(*repo, ref.snapshot, path, node) || node.kind != NodeKind::Dir)
        throw Error("snapshot " + short_id(ref.id) + " holds no virtual machine called " + machine);
    RestoreOptions o;
    o.target = exported;
    o.subpath = path;
    o.on_warning = [](const std::string& msg) { warning(msg); };
    o.should_cancel = [] { return stop_requested(); };
    o.on_progress = [](const RestoreProgress& p) {
        if (g_json) emit({{"event", "progress"}, {"files", p.files}, {"bytes", p.bytes}, {"current", p.current}});
        else status(std::to_string(p.files) + " files, " + format_bytes(p.bytes) + "  " + p.current);
    };
    const RestoreResult r = run_restore(*repo, ref.snapshot, o);
    if (!g_json) clear_status();
    if (r.errors > 0)
        throw Error(std::to_string(r.errors) + " file(s) of the machine could not be restored into " + exported);
    if (!g_json) status("Importing the virtual machine...");
    const std::string made = hyperv_import(os::join(exported, vm_file_name(machine)), name, machine_dir);
    if (!g_json) clear_status();
    // Import-VM copied everything into the machine's folder: the restored export is not needed.
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::path(std::u8string(exported.begin(), exported.end())), ec);
    if (g_json) {
        emit({{"event", "done"}, {"status", "success"}, {"files", r.files}, {"bytes", r.bytes}, {"name", made}});
        return kExitOk;
    }
    out("Imported the virtual machine " + made + " into " + machine_dir + "\n");
    return kExitOk;
}

int cmd_restore(const Args& a) {
    if (a.positional.empty()) throw Error("usage: vaultik restore -r <repo> <snapshot> --target <folder>");
    const auto target = a.value("--target");
    if (!target) throw Error("restore needs --target <folder>");
    auto repo = open_repo(a);
    RepoLock lock(*repo, RepoLock::Mode::Shared, "restore", lock_wait(a));
    repo->reload_index();  // a prune may have finished before the lock was granted
    const SnapshotRef ref = resolve_snapshot(*repo, a.positional[0]);

    RestoreOptions o;
    o.target = *target;
    if (auto p = a.value("--path")) o.subpath = *p;
    o.overwrite = a.has("--overwrite");
    o.permissions = a.has("--permissions");
    o.links = !a.has("--no-links");
    const auto start = std::chrono::steady_clock::now();
    o.on_progress = [&](const RestoreProgress& p) {
        if (g_json) {
            emit({{"event", "progress"}, {"files", p.files}, {"bytes", p.bytes}, {"current", p.current}});
            return;
        }
        const double secs = std::max(seconds_since(start), 0.001);
        status(std::to_string(p.files) + " files, " + format_bytes(p.bytes) + " (" +
               format_bytes(static_cast<std::uint64_t>(static_cast<double>(p.bytes) / secs)) + "/s)  " + p.current);
    };
    o.on_warning = [](const std::string& msg) { warning(msg); };
    o.should_cancel = [] { return stop_requested(); };

    const RestoreResult r = run_restore(*repo, ref.snapshot, o);
    if (g_json) {
        emit({{"event", "done"},
              {"status", r.errors > 0 ? "partial" : "success"},
              {"files", r.files},
              {"dirs", r.dirs},
              {"bytes", r.bytes},
              {"skipped", r.skipped},
              {"errors", r.errors},
              {"seconds", seconds_since(start)}});
        return r.errors > 0 ? kExitPartial : kExitOk;
    }
    clear_status();
    out("Restored " + std::to_string(r.files) + " files in " + std::to_string(r.dirs) + " folders, " +
        format_bytes(r.bytes) + " in " + format_duration(seconds_since(start)) + " from snapshot " +
        short_id(ref.id) + "\n");
    if (r.skipped > 0) out("  skipped: " + std::to_string(r.skipped) + " (existing files, links or placeholders)\n");
    if (r.errors > 0) {
        out("  " + std::to_string(r.errors) + " item(s) failed; see warnings above\n");
        return kExitPartial;
    }
    return kExitOk;
}

// "move-here [<snapshot>]": the folders of an old computer's account in the backup (Documents,
// Desktop, browser profiles, email, game saves), each restored into the same kind of folder of
// this account. Files that exist here already are left alone unless --overwrite.
int cmd_move_here(const Args& a) {
    auto repo = open_repo(a);
    RepoLock lock(*repo, RepoLock::Mode::Shared, "move to this PC", lock_wait(a));
    repo->reload_index();
    const SnapshotRef ref = resolve_snapshot(*repo, a.positional.empty() ? std::string("latest") : a.positional[0]);
    std::vector<MoveItem> plan = plan_move(*repo, ref.snapshot);
    const std::vector<std::string> accounts = plan_accounts(plan);
    if (plan.empty()) throw Error("this version holds none of an account's own folders (Documents, Desktop, ...)");
    std::string account;
    if (const auto chosen = a.value("--account")) {
        account = *chosen;
        if (std::find(accounts.begin(), accounts.end(), account) == accounts.end())
            throw Error("this version holds no folders of an account called " + account);
    } else if (accounts.size() == 1) {
        account = accounts[0];
    } else {
        std::string names;
        for (const std::string& n : accounts) names += (names.empty() ? "" : ", ") + n;
        throw Error("this version holds the folders of several accounts (" + names + "); choose one with --account");
    }
    std::vector<std::string> only;
    if (const auto o = a.value("--only")) {
        std::size_t start = 0;
        while (start <= o->size()) {
            std::size_t end = o->find(',', start);
            if (end == std::string::npos) end = o->size();
            if (end > start) only.push_back(o->substr(start, end - start));
            start = end + 1;
        }
    }
    std::erase_if(plan, [&](const MoveItem& m) {
        return m.account != account ||
               (!only.empty() && std::find(only.begin(), only.end(), m.id) == only.end() &&
                std::find(only.begin(), only.end(), m.group) == only.end());
    });
    if (plan.empty()) throw Error("nothing chosen is in this version");

    if (a.has("--dry-run")) {
        for (const MoveItem& m : plan) {
            if (g_json) emit({{"event", "move_item"}, {"id", m.id}, {"group", m.group}, {"from", m.from}, {"to", m.to}});
            else out("  " + m.id + ": " + m.from + " -> " + m.to + "\n");
        }
        if (g_json) emit({{"event", "done"}, {"status", "success"}, {"dry_run", true}});
        return kExitOk;
    }
    const auto start = std::chrono::steady_clock::now();
    RestoreResult total;
    for (const MoveItem& m : plan) {
        if (!g_json) out("Moving " + m.id + " to " + m.to + "\n");
        RestoreOptions o;
        o.target = m.to;
        o.subpath = m.from;
        o.contents_only = true;
        o.overwrite = a.has("--overwrite");
        // Another account's permissions would lock this one out of its own files.
        o.permissions = false;
        o.on_progress = [&](const RestoreProgress& p) {
            if (g_json) {
                emit({{"event", "progress"}, {"files", total.files + p.files}, {"bytes", total.bytes + p.bytes},
                      {"current", p.current}});
                return;
            }
            status(std::to_string(total.files + p.files) + " files, " + format_bytes(total.bytes + p.bytes) + "  " +
                   p.current);
        };
        o.on_warning = [](const std::string& msg) { warning(msg); };
        o.should_cancel = [] { return stop_requested(); };
        const RestoreResult r = run_restore(*repo, ref.snapshot, o);
        total.files += r.files;
        total.dirs += r.dirs;
        total.bytes += r.bytes;
        total.skipped += r.skipped;
        total.errors += r.errors;
    }
    if (g_json) {
        emit({{"event", "done"}, {"status", total.errors > 0 ? "partial" : "success"}, {"files", total.files},
              {"dirs", total.dirs}, {"bytes", total.bytes}, {"skipped", total.skipped}, {"errors", total.errors},
              {"seconds", seconds_since(start)}});
        return total.errors > 0 ? kExitPartial : kExitOk;
    }
    clear_status();
    out("Moved " + std::to_string(total.files) + " files, " + format_bytes(total.bytes) + " in " +
        format_duration(seconds_since(start)) + "\n");
    if (total.skipped > 0) out("  left alone: " + std::to_string(total.skipped) + " (already here, or links)\n");
    if (total.errors > 0) {
        out("  " + std::to_string(total.errors) + " item(s) failed; see warnings above\n");
        return kExitPartial;
    }
    return kExitOk;
}

nlohmann::json app_json(const InstalledApp& app) {
    return {{"name", app.name},      {"version", app.version}, {"publisher", app.publisher},
            {"id", app.winget_id},   {"source", app.source},   {"installable", !app.winget_id.empty()}};
}

// The program list a snapshot recorded.
std::vector<InstalledApp> snapshot_apps(Repository& repo, const SnapshotRef& ref) {
    std::optional<std::vector<InstalledApp>> apps = load_app_list(repo, ref.snapshot);
    if (!apps)
        throw Error("snapshot " + short_id(ref.id) +
                    " has no list of programs (made by an older Vaultik, or with the list turned off)");
    return std::move(*apps);
}

// The last line of winget's output that says something (it draws progress bars with \r).
std::string last_message(const std::string& output) {
    std::string last;
    std::size_t at = 0;
    while (at < output.size()) {
        std::size_t end = output.find_first_of("\r\n", at);
        if (end == std::string::npos) end = output.size();
        const std::string line = output.substr(at, end - at);
        if (std::any_of(line.begin(), line.end(), [](char c) { return std::isalpha(static_cast<unsigned char>(c)); }))
            last = line;
        at = end + 1;
    }
    while (!last.empty() && last.front() == ' ') last.erase(last.begin());
    return last;
}

// "apps install <id>...": installs programs of a snapshot's list with winget, one by one, as the
// user running this (never with administrator rights of Vaultik's own). Only package ids from the
// list are taken.
int cmd_apps_install(const Args& a) {
    const std::vector<std::string> ids(a.positional.begin() + 1, a.positional.end());
    if (ids.empty()) throw Error("usage: vaultik apps install <id>... (-r <repository> or --job <job>)");
    for (const std::string& id : ids)
        if (!valid_winget_id(id)) throw Error("not a winget package id: " + id);
    std::vector<InstalledApp> listed;
    {
        auto repo = open_repo(a);
        RepoLock lock(*repo, RepoLock::Mode::Shared, "apps", lock_wait(a));
        repo->reload_index();
        listed = snapshot_apps(*repo, resolve_snapshot(*repo, a.value("--snapshot").value_or("latest")));
    }
    std::vector<const InstalledApp*> chosen;
    for (const std::string& id : ids) {
        const auto it = std::find_if(listed.begin(), listed.end(), [&](const InstalledApp& app) {
            const auto lower = [](std::string v) {
                for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                return v;
            };
            return !app.winget_id.empty() && lower(app.winget_id) == lower(id);
        });
        if (it == listed.end()) throw Error(id + " is not a winget package of that snapshot's program list");
        chosen.push_back(&*it);
    }
    if (winget_program().empty())
        throw Error(os::process_elevated()
                        ? "programs are not installed from a program running with administrator rights: start "
                          "Vaultik without them (winget asks for approval where a program needs it)"
                        : "winget is not available for this account (install App Installer from the Microsoft Store)");

    const auto start = std::chrono::steady_clock::now();
    std::size_t ok = 0;
    std::size_t failed = 0;
    for (std::size_t i = 0; i < chosen.size(); ++i) {
        const InstalledApp& app = *chosen[i];
        if (stop_requested()) throw Cancelled();
        if (g_json) emit({{"event", "app_install"}, {"id", app.winget_id}, {"name", app.name}, {"index", i},
                          {"count", chosen.size()}});
        else out("Installing " + app.name + " (" + app.winget_id + ")...\n");
        std::string output;
        int code = 0;
        std::string message;
        try {
            // Not stopped halfway: a cancel takes effect before the next one.
            code = winget_install(app.winget_id, app.source, &output);
            message = last_message(output);
        } catch (const std::exception& e) {
            code = -1;
            message = e.what();
        }
        const bool success = code == 0;
        (success ? ok : failed) += 1;
        if (g_json) {
            emit({{"event", "app_installed"}, {"id", app.winget_id}, {"name", app.name}, {"ok", success},
                  {"code", code}, {"message", message}});
        } else {
            char hex[16];
            std::snprintf(hex, sizeof hex, "0x%08X", static_cast<unsigned>(code));
            out(std::string("  ") + (success ? "installed" : "failed (winget exit code " + std::string(hex) + ")") +
                (message.empty() ? "" : ": " + message) + "\n");
        }
    }
    if (g_json) {
        emit({{"event", "done"}, {"status", failed > 0 ? "partial" : "success"}, {"installed", ok},
              {"failed", failed}, {"seconds", seconds_since(start)}});
    } else {
        out(std::to_string(ok) + " installed, " + std::to_string(failed) + " failed\n");
    }
    return failed > 0 ? kExitPartial : kExitOk;
}

// "apps [<snapshot>]": the programs a snapshot recorded (--missing: those not installed here).
int cmd_apps(const Args& a) {
    if (!a.positional.empty() && a.positional[0] == "install") return cmd_apps_install(a);
    if (a.positional.size() > 1) throw Error("usage: vaultik apps [<snapshot>] [--missing] (-r <repository> or --job <job>)");
    std::vector<InstalledApp> apps;
    {
        auto repo = open_repo(a);
        RepoLock lock(*repo, RepoLock::Mode::Shared, "apps", lock_wait(a));
        repo->reload_index();
        apps = snapshot_apps(*repo, resolve_snapshot(*repo, a.positional.empty() ? std::string("latest") : a.positional[0]));
    }
    bool winget_here = false;
    if (a.has("--missing")) {
        if (!g_json) err("Looking at the programs on this PC...\n");
        const AppScan here = scan_installed_apps(true, [] { return stop_requested(); });
        if (stop_requested()) throw Cancelled();
        winget_here = here.winget;
        apps = missing_apps(apps, here.apps, here.winget);
    }
    std::stable_sort(apps.begin(), apps.end(), [](const InstalledApp& x, const InstalledApp& y) {
        return !x.winget_id.empty() && y.winget_id.empty();  // the ones winget can install first
    });
    if (g_json) {
        for (const InstalledApp& app : apps) {
            nlohmann::json ev = app_json(app);
            ev["event"] = "app";
            emit(ev);
        }
        emit({{"event", "done"}, {"status", "success"}, {"count", apps.size()}, {"winget", winget_here}});
        return kExitOk;
    }
    std::size_t installable = 0;
    for (const InstalledApp& app : apps) {
        std::string line = pad(app.name, 44) + " " + pad(app.version, 16) + " " + pad(app.publisher, 24);
        if (!app.winget_id.empty()) {
            line += " winget: " + app.winget_id;
            ++installable;
        }
        while (!line.empty() && line.back() == ' ') line.pop_back();
        out(line + "\n");
    }
    out(std::to_string(apps.size()) + " program(s)" + (a.has("--missing") ? " not on this PC" : "") + ", " +
        std::to_string(installable) + " installable with winget (vaultik apps install <id>...)\n");
    return kExitOk;
}

// "repair-data <percent>|off": repair data for every pack from now on, and (unless off) for the
// packs stored already, each after checking that it is intact.
int cmd_repair_data(const Args& a) {
    if (a.positional.size() != 1) throw Error("usage: vaultik repair-data <percent (1-50)>|off (-r <repository> or --job <job>)");
    const std::string& v = a.positional[0];
    unsigned percent = 0;
    if (v != "off" && v != "0") {
        char* end = nullptr;
        const long p = std::strtol(v.c_str(), &end, 10);
        if (end == v.c_str() || (*end != '\0' && std::string_view(end) != "%") || p < 1 || p > 50)
            throw Error("repair-data takes a percentage from 1 to 50, or off");
        percent = static_cast<unsigned>(p);
    }
    auto repo = open_repo(a);
    RepoLock lock(*repo, RepoLock::Mode::Shared, "repair data", lock_wait(a));
    repo->reload_index();
    repo->set_parity_percent(percent);
    if (percent == 0) {
        out("Repair data is off for new data; what exists stays and is still used by a repair.\n");
        return kExitOk;
    }
    std::uint64_t added = 0;
    std::uint64_t damaged = 0;
    const std::vector<Id>& packs = repo->index().packs();
    for (std::size_t i = 0; i < packs.size(); ++i) {
        if (stop_requested() || lock.lost()) throw Cancelled();
        status("Repair data: pack " + std::to_string(i + 1) + "/" + std::to_string(packs.size()));
        if (repo->backend().exists(Repository::parity_name(packs[i]))) continue;
        const Bytes pack = repo->backend().get(Repository::pack_name(packs[i]));
        if (blake3(pack) != packs[i]) {
            ++damaged;
            warning("pack " + id_hex(packs[i]).substr(0, 8) + " is damaged, so no repair data was made for it");
            continue;
        }
        repo->write_parity(packs[i], pack, percent);
        ++added;
    }
    clear_status();
    out("Repair data is " + std::to_string(percent) + "%; added to " + std::to_string(added) + " of " +
        std::to_string(packs.size()) + " pack(s)" +
        (damaged ? ", " + std::to_string(damaged) + " damaged pack(s) skipped (run a repair)" : std::string()) + ".\n");
    return damaged ? kExitError : kExitOk;
}

int cmd_verify(const Args& a) {
    auto repo = open_repo(a);
    RepoLock lock(*repo, RepoLock::Mode::Shared, "verify", lock_wait(a));
    repo->reload_index();  // a prune may have finished before the lock was granted
    VerifyOptions o;
    o.read_data = a.has("--read-data");
    if (const auto share = a.value("--read-share")) {
        if (!o.read_data) throw Error("--read-share needs --read-data");
        char* end = nullptr;
        const double pct = std::strtod(share->c_str(), &end);
        if (end == share->c_str() || (*end != '\0' && std::string_view(end) != "%") || !(pct > 0 && pct <= 100))
            throw Error("--read-share takes a percentage from 1 to 100");
        o.read_share = pct / 100.0;
        o.sample_seed = os::now();
    }
    o.should_cancel = [] { return stop_requested(); };
    o.on_progress = [](const std::string& msg) { status("Checking " + msg); };
    o.on_error = [](const std::string& msg) { err("error: " + msg + "\n"); };
    const auto start = std::chrono::steady_clock::now();
    const VerifyResult r = run_verify(*repo, o);
    clear_status();
    if (r.packs_archived > 0)
        out(std::to_string(r.packs_archived) + " pack(s) are in archive storage: checked to be there, but not read "
            "(that needs a thaw, see cloud-thaw); their folder listings and versions were checked.\n");
    out("Checked " + std::to_string(r.packs) + " packs, " + std::to_string(r.blobs) + " blobs, " +
        std::to_string(r.snapshots) + " snapshots, " + std::to_string(r.trees) + " folders" +
        (o.read_data ? ", read " + format_bytes(r.bytes_read) + " (" + std::to_string(r.packs_read) + " packs)"
                     : std::string()) +
        " in " +
        format_duration(seconds_since(start)) + "\n");
    if (r.errors > 0) {
        out(std::to_string(r.errors) + " problem(s) found\n");
        return kExitAuth;
    }
    out(o.read_data ? (o.read_share < 1.0 ? "No problems found in the data read.\n" : "No problems found; all data is readable.\n")
                    : "No problems found. Run with --read-data to also check every stored chunk.\n");
    return kExitOk;
}

// "recovery-kit -r <repo> --out <file> [--new]": the printable recovery kit (jobs/recovery_kit.hpp)
// for the backup's recovery key, which is asked for (it is not stored anywhere), or for a new one.
int cmd_recovery_kit(const Args& a) {
    const auto target = a.value("--out");
    if (!target || !a.positional.empty())
        throw Error("usage: vaultik recovery-kit -r <repository> --out <kit.pdf|kit.html> [--new]");
    const std::string location = repo_path(a);
    const auto backend = backend_for(a);
    std::string key;
    struct Wipe {
        std::string& s;
        ~Wipe() { secure_wipe(s.data(), s.size()); }
    } wipe_key{key};
    if (a.has("--new")) {
        Credential credential = key_credential(a);
        status("Making a recovery key...");
        try {
            key = create_recovery_key(*backend, credential);
        } catch (...) {
            secure_wipe(credential.secret.data(), credential.secret.size());
            throw;
        }
        secure_wipe(credential.secret.data(), credential.secret.size());
        clear_status();
        out("A new recovery key was made; any older one no longer works.\n");
    } else {
        std::string typed = os::get_env("VAULTIK_RECOVERY_KEY");
        if (typed.empty()) typed = read_password("Recovery key: ", "no console to ask for the recovery key; set VAULTIK_RECOVERY_KEY");
        Bytes raw;
        try {
            raw = parse_recovery_key(typed);
        } catch (...) {
            secure_wipe(typed.data(), typed.size());
            throw;
        }
        secure_wipe(typed.data(), typed.size());
        key = format_recovery_key(raw);
        secure_wipe(raw.data(), raw.size());
        // Only a key that opens this backup goes on paper.
        status("Checking the recovery key...");
        { const MasterKey check = unlock_master_key(*backend, {key, true}); }
        clear_status();
    }
    const jobs::RecoveryKit kit = jobs::make_recovery_kit(key, location);
    const std::string path = os::absolute_path(*target);
    try {
        jobs::write_recovery_kit(kit, path);
    } catch (const std::exception&) {
        // A new key exists only here now: it must not be lost with the file.
        if (a.has("--new")) out("Recovery key: " + key + "\nWrite it down: the kit could not be written.\n");
        throw;
    }
    out("Wrote the recovery kit to " + path + ".\n");
    out("It holds the recovery key: print it, keep the paper apart from this computer, and delete the file.\n");
    return kExitOk;
}

// "30d", "2w", "12h" back from now, or a local date ("2026-10-01") or time.
std::uint64_t parse_since(const std::string& v, std::uint64_t now) {
    constexpr std::uint64_t kHour = 36'000'000'000ull;
    if (!v.empty() && (v.back() == 'd' || v.back() == 'w' || v.back() == 'h')) {
        char* end = nullptr;
        const double n = std::strtod(v.c_str(), &end);
        if (end == v.c_str() + v.size() - 1 && n > 0 && n < 100000) {
            const double unit = v.back() == 'h' ? 1 : v.back() == 'd' ? 24 : 24 * 7;
            const auto back = static_cast<std::uint64_t>(n * unit * static_cast<double>(kHour));
            return back < now ? now - back : 0;
        }
        throw Error("--since takes 30d, 2w, 12h or a date (YYYY-MM-DD)");
    }
    return os::from_unix(parse_local_time(v), 0);
}

// "grew [-r <repo> | --job <job>] [--since 30d] [--top n] [--json]": which folders added the most new
// data over a period (jobs/growth.hpp).
int cmd_grew(const Args& a) {
    if (!a.positional.empty()) throw Error("usage: vaultik grew -r <repository> | --job <job> [--since 30d] [--top <n>] [--json]");
    const std::optional<jobs::Job> job = job_arg(a);
    auto repo = open_repo(a);
    RepoLock lock(*repo, RepoLock::Mode::Shared, "grew", lock_wait(a));
    repo->reload_index();
    const std::uint64_t now = os::now();
    jobs::GrowthOptions o;
    o.since = parse_since(a.value("--since").value_or("30d"), now);
    if (auto v = a.value("--top")) {
        const int n = std::atoi(v->c_str());
        if (n < 1 || n > 1000) throw Error("--top takes a number from 1 to 1000");
        o.top = static_cast<std::size_t>(n);
    }
    const std::vector<SnapshotRef> all = load_all_snapshots(*repo);
    if (job) {
        o.select = versions_of_job(all, *job);
        o.excludes = job->excludes;
    }
    o.should_cancel = [&lock] { return stop_requested() || lock.lost(); };
    o.on_progress = [](const std::string& msg) {
        if (!g_json) status("Reading the versions: " + msg);
    };
    std::vector<std::pair<Id, Snapshot>> snaps;
    snaps.reserve(all.size());
    for (const SnapshotRef& s : all) snaps.emplace_back(s.id, s.snapshot);
    const jobs::GrowthReport r = jobs::what_grew(*repo, snaps, o);
    clear_status();
    if (g_json) {
        out(jobs::to_json(r) + "\n");
        return kExitOk;
    }
    out("New data stored since " + os::format_time_local(r.since).substr(0, 16) + " (" + std::to_string(r.versions) +
        " version(s)): " + format_bytes(r.stored) + " in " + std::to_string(r.files) + " file(s)\n");
    if (r.folders.empty()) {
        out("Nothing new was stored in that time.\n");
        return kExitOk;
    }
    out("\nFolders that added the most:\n");
    for (const jobs::FolderGrowth& f : r.folders) {
        std::string size = format_bytes(f.stored);
        if (size.size() < 11) size.insert(0, 11 - size.size(), ' ');
        out("  " + size + "  " + f.path + (f.rest ? " (other files)" : "") + "  (" + std::to_string(f.files) +
            " file(s))\n");
        if (f.suggestion) {
            std::string ex;
            for (const std::string& p : f.suggestion->patterns) ex += " --exclude \"" + p + "\"";
            out("               " +
                (f.suggestion->preset.empty() ? std::string("could be left out:")
                                              : "the ready-made exclusion \"" + f.suggestion->preset + "\" leaves it out:") +
                ex + "\n");
        }
    }
    if (!r.presets.empty()) {
        out(std::string("\nReady-made exclusions") + (job ? " the job does not use" : "") +
            ", and what they would have left out:\n");
        for (const jobs::PresetGrowth& p : r.presets)
            out("  " + p.id + ": " + format_bytes(p.stored) + " in " + std::to_string(p.files) + " file(s)\n");
    }
    return kExitOk;
}

int cmd_stats(const Args& a) {
    auto repo = open_repo(a);
    const Index& index = repo->index();
    std::uint64_t unique_raw = 0;
    std::uint64_t stored = 0;
    std::uint64_t data_blobs = 0;
    index.for_each([&](const Id&, const BlobLocation& loc) {
        if (loc.type == BlobType::Data) {
            unique_raw += loc.raw_len;
            ++data_blobs;
        }
        stored += loc.length;
    });
    const auto snaps = load_all_snapshots(*repo);
    std::uint64_t logical = 0;
    for (const auto& s : snaps) logical += s.snapshot.stats.bytes;

    out("Repository:      " + repo->backend().location() + "\n");
    out("Snapshots:       " + std::to_string(snaps.size()) + "\n");
    out("Packs:           " + std::to_string(index.packs().size()) + "\n");
    out("Chunks:          " + std::to_string(data_blobs) + "\n");
    out("Data in snapshots (sum of all): " + format_bytes(logical) + "\n");
    out("Unique data:     " + format_bytes(unique_raw) + "\n");
    out("Stored size:     " + format_bytes(stored) + "\n");
    if (stored > 0 && logical > 0) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.1fx", static_cast<double>(logical) / static_cast<double>(stored));
        out("Space saving:    " + std::string(buf) + " (deduplication and compression)\n");
    }
    return kExitOk;
}

int run(const std::vector<std::string>& argv) {
    const Args a = parse_args(argv);
    if (a.has("--version") || a.command == "version") {
        out(std::string("vaultik ") + kVersion + "\n");
        return kExitOk;
    }
    if (a.command.empty() || a.has("--help") || a.command == "help") {
        out(kUsage);
        return a.command.empty() && !a.has("--help") ? kExitError : kExitOk;
    }
    g_json = a.has("--json");
    if (g_json) start_cancel_watcher();

    if (a.command == "jobs") return cmd_jobs(a);
    if (a.command == "drives") return cmd_drives(a);
    if (a.command == "run-job") return cmd_run_job(a);  // applies the job's own resource settings
    if (a.command == "check-job") return cmd_check_job(a);
    if (a.command == "test-restore") return cmd_test_restore(a);
    if (a.command == "alert-test") return cmd_alert_test(a);
    if (a.command == "report") return cmd_report(a);
    if (a.command == "update-check") return cmd_update_check(a);
    if (a.command == "copy") return cmd_copy(a);
    if (a.command == "cloud-rollback") return cmd_cloud_rollback(a);
    if (a.command == "cloud-thaw") return cmd_cloud_thaw(a);
    if (a.command == "mount") return cmd_mount(a);
    if (a.command == "admin-job") return cmd_admin_job(a);
    if (a.command == "uninstall-cleanup") return cmd_uninstall_cleanup();
    if (a.command == "schedule") return cmd_schedule(a, true);
    if (a.command == "unschedule") return cmd_schedule(a, false);
    if (a.command == "pause-job") return cmd_pause_job(a, true);
    if (a.command == "settings") return cmd_settings(a);
    if (a.command == "resume-job") return cmd_pause_job(a, false);
    if (a.command == "recovery") return cmd_recovery(a);
    if (a.command == "recovery-media") return cmd_recovery_media(a);

    const ResourcePolicy policy = resource_policy(a);
    apply_resource_policy(policy);
    if (!policy.is_default() && !g_json) err("Resources: " + describe(policy) + "\n");

    if (a.command == "init") return cmd_init(a);
    if (a.command == "backup") return cmd_backup(a);
    if (a.command == "snapshots") return cmd_snapshots(a);
    if (a.command == "computers") return cmd_computers(a);
    if (a.command == "ls") return cmd_ls(a);
    if (a.command == "restore") return cmd_restore(a);
    if (a.command == "wsl") return cmd_wsl(a);
    if (a.command == "hyperv") return cmd_hyperv(a);
    if (a.command == "docker") return cmd_docker(a);
    if (a.command == "db") return cmd_db(a);
    if (a.command == "verify") return cmd_verify(a);
    if (a.command == "stats") return cmd_stats(a);
    if (a.command == "forget") return cmd_forget(a);
    if (a.command == "prune") return cmd_prune(a);
    if (a.command == "repair") return cmd_repair(a);
    if (a.command == "repair-data") return cmd_repair_data(a);
    if (a.command == "move-here") return cmd_move_here(a);
    if (a.command == "apps") return cmd_apps(a);
    if (a.command == "unlock") return cmd_unlock(a);
    if (a.command == "key") return cmd_key(a);
    if (a.command == "recovery-kit") return cmd_recovery_kit(a);
    if (a.command == "grew") return cmd_grew(a);
    if (a.command == "pin") return cmd_pin(a, true);
    if (a.command == "unpin") return cmd_pin(a, false);
    if (a.command == "note") return cmd_note(a);
    if (a.command == "history") return cmd_history(a);
    if (a.command == "diff") return cmd_diff(a);
    if (a.command == "estimate") return cmd_estimate(a);
    if (a.command == "find") return cmd_find(a);
    if (a.command == "image") return cmd_image(a);
    if (a.command == "disk") return cmd_disk(a);
    throw Error("unknown command " + a.command + " (see vaultik --help)");
}

}  // namespace

}  // namespace vaultik::cli

namespace {

// Reports a fatal error on stderr and, with --json, as a final "done" event on stdout.
int fail(const char* status, const std::string& message, int code) {
    using namespace vaultik::cli;
    if (g_json) emit({{"event", "done"}, {"status", status}, {"message", message}});
    err((std::string(status) == "cancelled" ? "" : "error: ") + message + "\n");
    return code;
}

int run_guarded(const std::vector<std::string>& args) {
    using namespace vaultik::cli;
    try {
        return run(args);
    } catch (const vaultik::Cancelled&) {
        return fail("cancelled", "cancelled; no snapshot was saved", kExitCancelled);
    } catch (const vaultik::AuthError& e) {
        return fail("failed", e.what(), kExitAuth);
    } catch (const vaultik::LockedError& e) {
        return fail("failed", e.what(), kExitLocked);
    } catch (const vaultik::image::VolumesInUse& e) {
        // A frontend asks whether to force it closed (and runs the job again with
        // --force-dismount); at a console, the option is named.
        if (g_json) {
            emit({{"event", "in_use"}, {"volumes", e.volumes}, {"message", e.what()}});
            emit({{"event", "done"}, {"status", "in_use"}, {"volumes", e.volumes}, {"message", e.what()}});
        }
        err(std::string("error: ") + e.what() + "\n");
        if (!g_json)
            err("Give --force-dismount to force it closed: programs with files open there lose access to them, and "
                "unsaved changes to those files are lost.\n");
        return kExitInUse;
    } catch (const vaultik::ArchivedError& e) {
        // The frontend offers a thaw on "archived".
        if (g_json) emit({{"event", "done"}, {"status", "failed"}, {"archived", true}, {"message", e.what()}});
        err(std::string("error: ") + e.what() + "\n");
        return kExitError;
    } catch (const vaultik::Error& e) {
        return fail("failed", e.what(), kExitError);
    } catch (const std::bad_alloc&) {
        return fail("failed", "out of memory (raise or remove --max-memory)", kExitError);
    } catch (const std::exception& e) {
        return fail("failed", std::string("internal error: ") + e.what(), kExitError);
    }
}

int run_main(const std::vector<std::string>& args) {
    using namespace vaultik::cli;
    init_console();
    vaultik::jobs::log_runs_to_system(true);  // every recorded run also goes to the Event Log
    const int code = run_guarded(args);
    // A window of its own (a restore started with administrator rights from the frontend) stays
    // open until the result is read.
    // Not when a disk tool found a volume in use: the frontend that started it asks right away
    // whether to force it closed.
    if (code != kExitInUse && std::find(args.begin(), args.end(), "--pause") != args.end()) {
        err("\nPress Enter to close this window.");
        std::string line;
        std::getline(std::cin, line);
    }
    return code;
}

}  // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
#if defined(_MSC_VER) && defined(_DEBUG)
    // With VAULTIK_LEAK_CHECK set, a debug build reports every heap block still allocated at exit.
    wchar_t* leak_check = nullptr;
    std::size_t leak_check_len = 0;
    if (_wdupenv_s(&leak_check, &leak_check_len, L"VAULTIK_LEAK_CHECK") == 0 && leak_check != nullptr) {
        std::free(leak_check);
        _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
        _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
    }
#endif
    // Libraries loaded later (delay-loaded ones such as the virtual disk library) only from the
    // system folder, never from the program's folder or the current one.
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    // Started by an elevated Vaultik for work a program running as the user could have chosen (a
    // job from the user's own settings): before reading anything, run again with an ordinary
    // user's rights, the same command line, and pass its exit code on. Never with the full rights.
    {
        wchar_t flag[4] = {};
        if (GetEnvironmentVariableW(L"VAULTIK_USER_RIGHTS", flag, 4) == 1 && flag[0] == L'1' &&
            vaultik::os::process_elevated()) {
            SetEnvironmentVariableW(L"VAULTIK_USER_RIGHTS", L"0");  // the run itself must not try again
            try {
                std::wstring self(MAX_PATH, L'\0');
                for (;;) {
                    const DWORD n = GetModuleFileNameW(nullptr, self.data(), static_cast<DWORD>(self.size()));
                    if (n == 0) throw vaultik::Error("cannot find this program's own file");
                    if (n < self.size()) {
                        self.resize(n);
                        break;
                    }
                    self.resize(self.size() * 2);
                }
                return vaultik::os::run_with_user_rights(vaultik::narrow(self), vaultik::narrow(GetCommandLineW()));
            } catch (const std::exception& e) {
                std::fprintf(stderr, "%s\n", e.what());
                return 1;
            }
        }
    }
    // Arguments arrive as UTF-16; everything inside Vaultik is UTF-8.
    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) args.push_back(vaultik::narrow(argv[i]));
    return run_main(args);
}
#else
int main(int argc, char** argv) {
    return run_main(std::vector<std::string>(argv, argv + argc));
}
#endif
