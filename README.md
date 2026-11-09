# Vaultik

Lightweight, deduplicating, compressed and encrypted backups for Windows, written in C++20, with a
Qt 6 frontend. Linux support is planned: the platform code is written but not yet built or tested.

- **Small and idle-free.** The engine is one static ~1 MB executable: no service, no kernel driver.
  Scheduled backups are started by the system scheduler, run, and exit. Peak RAM stays under
  ~100 MB however much data there is (measured: 77 MB for a 6.8 GB folder, 9 MB for an unchanged
  re-run). The frontend only uses memory while it is open.
- **Deduplicated.** Files are split into content-defined chunks (FastCDC, ~1 MiB average). A chunk
  already stored, from any file in any earlier snapshot, is never stored again. Every snapshot looks
  like a full backup but only adds what changed.
- **Compressed and encrypted.** Each chunk is zstd-compressed, then sealed with AES-256-GCM. Chunk
  ids are keyed BLAKE3 hashes, so the storage location learns nothing about the content.
- **Fast incremental runs.** Files whose size and timestamps match the previous snapshot are not
  even read.
- **Polite.** Priorities and hard limits for CPU, disk, memory and bandwidth, per job.
- **Verifiable.** `verify --read-data` decrypts and checks every stored chunk.
- **Portable repositories.** A repository written on Windows restores on Linux and the other way
  round (same format, same crypto).

The on-disk format is specified in [docs/FORMAT.md](docs/FORMAT.md).

## Programs

| Program | What it is |
|---|---|
| `vaultik-gui` | The frontend: backup jobs, schedules, progress, history, snapshot browser, restore |
| `vaultik` | The engine and command line. The frontend and the scheduler run it for the actual work |
| `vaultik-task` | Windows only: windowless launcher that Task Scheduler starts, so no console flashes up |

## Build

**Windows:** Visual Studio 2022 or later with the C++ workload (MSVC, CMake and the bundled vcpkg).

```
build.cmd release test
```

The programs end up in `build\release\`. Dependencies (zstd, BLAKE3, nlohmann-json, Qt 6 for the
frontend, doctest for tests) come from vcpkg and are linked statically. The first build compiles
a minimal Qt and takes a while; later builds reuse it.

**Linux (planned, not yet built or tested):** a C++20 compiler (GCC 12+ or Clang 16+), CMake,
Ninja, vcpkg, OpenSSL and Qt 6 development packages. On Debian or Ubuntu:

```
sudo apt install build-essential cmake ninja-build pkg-config libssl-dev qt6-base-dev
export VCPKG_ROOT=~/vcpkg   # a vcpkg checkout
./build.sh release test
```

The frontend uses the system Qt on Linux. Configure with `-DVAULTIK_BUILD_GUI=OFF` for a
command-line-only build (servers). Until the Linux port is verified, the Linux notes in this
README describe the intended behavior.

## Using the frontend

Start `vaultik-gui`, then **New backup job**:

- **General:** a name, the destination folder (USB drive, other disk, network share) and the
  repository password. A new destination gets a new encrypted repository; an existing one is
  opened with its password.
- **What to back up:** folders and files, name patterns to skip (`*.tmp`, `node_modules`), and
  whether to use a Volume Shadow Copy for open and locked files (on by default; it needs
  administrator rights, see "Current limits"). For the shadow copy, Windows briefly pauses the
  writes of programs that take part in it, such as SQL Server. "Do not pause SQL Server" leaves
  SQL Server running; its database files are then saved as they would be after a power cut,
  which SQL Server recovers from when the files are attached again.
- **Schedule:** manually, every few hours, daily, or on selected weekdays. Schedules are kept by
  Task Scheduler (Windows, task folder `\Vaultik`) or a systemd user timer (Linux), so Vaultik does
  not need to stay open; a run missed while the computer was off starts at the next chance.
  On Windows a job can also run with administrator rights, so scheduled runs use a shadow copy
  for open and locked files. Windows then asks for administrator approval each time the job (or
  its schedule) is saved or removed. Without the option, the task runs with normal rights and
  needs no approval. Such a job only works when Vaultik is installed in a folder only
  administrators can change, such as `C:\Program Files\Vaultik`: the approved step checks this,
  then copies the job and its password into `C:\ProgramData\Vaultik\admin\<your account>`, which
  your account can read but only administrators can change. Scheduled runs read only that copy,
  and refuse to run if its permissions were loosened, so nothing your normal account (or a program
  running as you) can change decides what an administrator run does. Editing the job in Vaultik
  and saving it updates the copy; so does changing the password in Vaultik.
- **Resources:** a priority preset and optional hard limits (see below).
- **Old backups:** how many backups to keep, for example one per day for 7 days, one per week
  for 4 weeks and one per month for 12 months (the default for new jobs). After each run the
  job's backups that no rule keeps are removed and the space only they used is freed. Turned off,
  every backup is kept.

The job page shows live progress, the run history (also of scheduled runs), and the snapshots.
**Restore** opens a snapshot browser: pick a file or folder (or nothing, for everything) and
restore it to another folder or to its original location.

**Restore from a backup folder** in the toolbar opens any Vaultik backup with its password,
without a saved job: the way to get files back on another computer. Copy `vaultik-gui.exe`,
`vaultik.exe` and `vaultik-task.exe` there (they need nothing else), connect the backup drive,
pick the backup folder and enter the password.

The password is stored for scheduled runs, readable only by your user account: encrypted with
DPAPI on Windows, in a file with mode 0600 on Linux. Job files live in `%LOCALAPPDATA%\Vaultik`
or `~/.config/vaultik` (override with `VAULTIK_CONFIG_DIR`).

**Keep the password safe elsewhere too.** Without it the backups cannot be decrypted, and nobody
can reset it. What helps is the **recovery key**: a new backup shows one once (8 groups of 4
characters, with buttons to copy it or save it to a file). Keep it apart from the computer. If
the password is forgotten, **Password and recovery key** in the toolbar sets a new password with
the recovery key; the same dialog changes the password with the current one, or makes a new
recovery key (which replaces the old one, and gives older backups one). Saved jobs that use the
backup get the new password. A changed password does not re-encrypt the backup: a copy of the
backup folder made before the change still opens with the old password.

## Command line

```
vaultik init      -r E:\Backups\vaultik
vaultik backup    -r E:\Backups\vaultik C:\Users\me\Documents D:\Projects --exclude node_modules --exclude *.tmp
vaultik snapshots -r E:\Backups\vaultik
vaultik ls        -r E:\Backups\vaultik latest C/Users/me/Documents
vaultik restore   -r E:\Backups\vaultik latest --target D:\Restore --path C/Users/me/Documents/report.docx
vaultik verify    -r E:\Backups\vaultik --read-data
vaultik stats     -r E:\Backups\vaultik
vaultik forget    -r E:\Backups\vaultik --keep-daily 7 --keep-weekly 4 --keep-monthly 12 --prune
vaultik prune     -r E:\Backups\vaultik
vaultik jobs
vaultik run-job   documents
vaultik restore   --job documents latest --target D:\Restore
vaultik key passwd   -r E:\Backups\vaultik
vaultik key passwd   -r E:\Backups\vaultik --recovery-key
vaultik key recovery -r E:\Backups\vaultik
```

The repository can be any folder: an internal disk, a USB drive or a network share
(`\\nas\backups\vaultik`, or a mounted share on Linux). Set `VAULTIK_REPO` and `VAULTIK_PASSWORD`
(or use `--password-file`) for unattended runs, or use `--job` to take the repository and stored
password from a saved job. Snapshot paths start with the drive letter (`C/...`) or
`UNC/server/share/...` on Windows, and with the top-level folder (`home/...`) on Linux.

`init` prints the new repository's recovery key. `key passwd` sets a new password after
checking the current one (or, with `--recovery-key`, the recovery key from a prompt or
`VAULTIK_RECOVERY_KEY`; the new password comes from a prompt or `VAULTIK_NEW_PASSWORD`), and `key recovery` makes a new recovery key.

On Windows, `backup` reads from a Volume Shadow Copy when it runs as administrator
(`--vss auto`, the default); `--vss on` makes that required and `--vss off` always reads live.
`--vss-skip-sql` keeps SQL Server from being paused for the shadow copy.

`--json` turns the output into one JSON event per line (`progress`, `warning`, `done`); with it,
a line `cancel` on stdin stops a backup or restore cleanly. The frontend uses this.

Exit codes: `0` ok, `1` error, `2` wrong password or damaged data, `3` finished but some files
were skipped (see warnings), `4` cancelled, `5` repository in use by another run.

### Old snapshots and space

`forget` removes snapshots by keep rules (`--keep-last`, `--keep-hourly`, `--keep-daily`,
`--keep-weekly`, `--keep-monthly`, `--keep-yearly`). Each rule keeps the newest snapshot of each
of the last N periods that have one; a snapshot kept by any rule stays. Rules apply per job, or
per computer and set of paths for snapshots made without a job; `--tag job:<id>` limits forget
to one job. `--dry-run` shows the decision without removing anything. Removing snapshots frees
no space by itself: `prune` (or `forget --prune`) then deletes the data no remaining snapshot
uses, rewriting packs that are mostly unused.

### Running side by side

Backups, restores and checks of the same repository can run at the same time, also from several
computers. `forget` and `prune` need the repository to themselves: they wait for nothing by
default and stop with exit code 5 when another run is active (`--lock-wait 10m` waits instead;
scheduled jobs wait up to 10 minutes). A run that crashes leaves a lock behind that expires after
30 minutes, or at once on the same computer; `vaultik unlock` removes expired locks and
`vaultik unlock --all` every lock.

## Resource use

Every command, and every saved job, can control how much of the computer it may use, so a backup
can run while you work without slowing anything down.

```
vaultik backup -r E:\Backups\vaultik C:\Users\me --priority low
vaultik backup -r \\nas\backups\vaultik D:\Projects --priority background --write-limit 20M
vaultik restore -r E:\Backups\vaultik latest --target D:\Restore --priority high
```

| Option | Values | Windows | Linux |
|---|---|---|---|
| `--priority` | `background`, `low`, `normal` (default), `high` | Preset for the three priorities below | same |
| `--cpu` | `idle`, `low`, `normal`, `high` | Process priority class | nice 19 / 10 / 0 / -5 (`high` needs root) |
| `--io` | `background`, `low`, `normal` | I/O priority hint on every file | ioprio idle / best-effort 7 / default |
| `--ram` | `low`, `normal` | Memory priority: Windows reclaims Vaultik's pages first | no equivalent, ignored |
| `--cpu-limit` | `1`-`100` | Hard cap, percent of total CPU (Job Object) | same, enforced by pausing |
| `--max-memory` | size, at least `128M` | Hard cap on committed memory (Job Object) | address-space limit |
| `--read-limit` | rate, e.g. `50M` | Max read speed per second | same |
| `--write-limit` | rate, e.g. `20M` | Max write speed per second, also to network shares | same |

Presets: `background` = CPU idle, I/O background, RAM low; `low` = CPU low, I/O low, RAM low;
`high` = CPU high. Individual options override the preset (`--priority low --cpu normal`).
`run-job` uses the job's saved settings; options given on its command line override them.

Priorities only take effect when something else competes for the resource, so on an idle PC a
`low` backup runs at full speed. Limits apply all the time. Sizes and rates use binary units
(`512K`, `64M`, `1.5G`). Restores from the frontend run at normal priority without limits.

## Current limits

- Open and locked files (Outlook data files, databases, files of running programs) are only
  backed up when Vaultik runs with administrator rights: it then reads from a Volume Shadow Copy,
  a frozen view of each source volume made at the start of the backup. Without those rights, and
  on Linux, files are read live: locked files are skipped with a warning, and a file being
  written during the backup may be captured mid-change. Scheduled jobs get those rights only
  with the job's "Run with administrator rights" option, which needs Vaultik installed in a
  folder only administrators can change. The backup destination of such a job is still a folder
  you can change; keep it on a drive used for backups only.
- Windows: file content, attributes, creation and modification times are kept. ACLs, alternate
  data streams and symlink/junction targets are not (links are recorded without content).
  Linux: content, permissions, owner (restored when running as root), modification time and
  symlinks are kept; extended attributes and ACLs are not. Kernel file systems (`/proc`, `/sys`,
  `/dev`, tmpfs) are skipped.
- NTFS names that are not valid UTF-16 (unpaired surrogates, very rare) cannot be backed up.
- Restores and backups check for links planted in the folders they work on, but a parent folder
  swapped for a link during the run by someone who can write there is not detected. Run elevated
  restores into folders only you can write to.
- Single-threaded; about 140 MB/s on a fresh backup, which is above USB HDD speeds anyway.

## Roadmap

1. Repository format, file backup/restore/verify, CLI (done)
2. Saved jobs, scheduling, resource controls, Qt frontend on Windows, retention
   (`forget`/`prune`), repository locks, Volume Shadow Copy (done)
3. Linux port: build and test the existing POSIX platform layer, systemd timers and the frontend
4. S3-compatible cloud backend (Cloudflare R2, Backblaze B2, MinIO)
5. Volume image backup, restore to a volume or `.vhdx`
6. Bare-metal restore from a recovery USB (WinPE on Windows)
