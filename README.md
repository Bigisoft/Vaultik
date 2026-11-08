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
- **What to back up:** folders and files, and name patterns to skip (`*.tmp`, `node_modules`).
- **Schedule:** manually, every few hours, daily, or on selected weekdays. Schedules are kept by
  Task Scheduler (Windows, task folder `\Vaultik`) or a systemd user timer (Linux), so Vaultik does
  not need to stay open; a run missed while the computer was off starts at the next chance.
- **Resources:** a priority preset and optional hard limits (see below).

The job page shows live progress, the run history (also of scheduled runs), and the snapshots.
**Restore** opens a snapshot browser: pick a file or folder (or nothing, for everything) and
restore it to another folder or to its original location.

The password is stored for scheduled runs, readable only by your user account: encrypted with
DPAPI on Windows, in a file with mode 0600 on Linux. Job files live in `%LOCALAPPDATA%\Vaultik`
or `~/.config/vaultik` (override with `VAULTIK_CONFIG_DIR`).

**Keep the password safe elsewhere too.** Without it the backups cannot be decrypted; there is no
recovery.

## Command line

```
vaultik init      -r E:\Backups\vaultik
vaultik backup    -r E:\Backups\vaultik C:\Users\me\Documents D:\Projects --exclude node_modules --exclude *.tmp
vaultik snapshots -r E:\Backups\vaultik
vaultik ls        -r E:\Backups\vaultik latest C/Users/me/Documents
vaultik restore   -r E:\Backups\vaultik latest --target D:\Restore --path C/Users/me/Documents/report.docx
vaultik verify    -r E:\Backups\vaultik --read-data
vaultik stats     -r E:\Backups\vaultik
vaultik jobs
vaultik run-job   documents
vaultik restore   --job documents latest --target D:\Restore
```

The repository can be any folder: an internal disk, a USB drive or a network share
(`\\nas\backups\vaultik`, or a mounted share on Linux). Set `VAULTIK_REPO` and `VAULTIK_PASSWORD`
(or use `--password-file`) for unattended runs, or use `--job` to take the repository and stored
password from a saved job. Snapshot paths start with the drive letter (`C/...`) or
`UNC/server/share/...` on Windows, and with the top-level folder (`home/...`) on Linux.

`--json` turns the output into one JSON event per line (`progress`, `warning`, `done`); with it,
a line `cancel` on stdin stops a backup or restore cleanly. The frontend uses this.

Exit codes: `0` ok, `1` error, `2` wrong password or damaged data, `3` finished but some files
were skipped (see warnings), `4` cancelled.

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

- Files are read live, without a VSS snapshot: files locked by another program are skipped with a
  warning, and a file being written during the backup may be captured mid-change.
- Windows: file content, attributes, creation and modification times are kept. ACLs, alternate
  data streams and symlink/junction targets are not (links are recorded without content).
  Linux: content, permissions, owner (restored when running as root), modification time and
  symlinks are kept; extended attributes and ACLs are not. Kernel file systems (`/proc`, `/sys`,
  `/dev`, tmpfs) are skipped.
- NTFS names that are not valid UTF-16 (unpaired surrogates, very rare) cannot be backed up.
- No `forget`/`prune` yet, so the repository only grows.
- No repository lock: do not run two backups into the same repository at the same time.
- Single-threaded; about 140 MB/s on a fresh backup, which is above USB HDD speeds anyway.

## Roadmap

1. Repository format, file backup/restore/verify, CLI (done)
2. Saved jobs, scheduling, resource controls, Qt frontend on Windows (done); next: VSS snapshots,
   retention (`forget`/`prune`), repository lock
3. Linux port: build and test the existing POSIX platform layer, systemd timers and the frontend
4. S3-compatible cloud backend (Cloudflare R2, Backblaze B2, MinIO)
5. Volume image backup, restore to a volume or `.vhdx`
6. Bare-metal restore from a recovery USB (WinPE on Windows)
