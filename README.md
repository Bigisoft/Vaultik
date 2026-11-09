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
- **Verifiable.** `verify --read-data` decrypts and checks every stored chunk, and jobs run
  health checks on their own (see below).
- **Portable repositories.** A repository written on Windows restores on Linux and the other way
  round (same format, same crypto).
- **Local or cloud.** Back up to a disk, a USB drive, a network share, S3-compatible cloud
  storage (Cloudflare R2, Backblaze B2, Amazon S3, IDrive e2, MinIO, ...), or a cloud drive
  (pCloud, OneDrive, Google Drive, MEGA), each through its own API with no extra software. The
  cloud only ever sees encrypted chunks.

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

- **General:** a name, the destination (a folder on a USB drive, other disk or network share, or
  **Cloud storage...**) and the repository password. A new destination gets a new encrypted
  repository; an existing one is opened with its password. Cloud storage asks for the provider,
  then for S3-compatible storage the endpoint, an existing bucket, an optional folder in it and an
  access key, or for a cloud drive a folder and a sign-in; **Test connection** checks them. The
  secret key or sign-in token is stored like the password (see "Cloud drives" below).
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
- **Old backups:** a preset: **Smart** (the default: one version per day for 7 days, per week
  for 4 weeks and per month for 12 months), the newest N versions, every version of the last N
  days, custom rules, or keep everything; plus an optional size limit, which removes the oldest
  versions (never the newest, never pinned ones) when the backup grows past it. The dialog says
  in plain words what the rules keep. Every version is a complete backup, so removing one never
  breaks the others; after each run the versions no rule keeps go and the space only they used
  is freed.

The window follows the Windows light or dark setting and accent colour. The sidebar lists the
jobs with their last result; the job page shows live progress and four tabs:

- **Versions:** a timeline grouped by day, week and month, each version with what changed since
  the one before (new, changed and removed files), its size and the space it added. **Pin** a
  version to keep it whatever the rules say, add a **note** ("before reinstalling Windows"),
  **delete** one, or **restore** from it. Versions the next cleanup will remove are marked.
- **Find a file:** search every version by name (`budget`, `*.xlsx`); pick a result to see only
  the versions where it changed, each compared with the file on this PC now, and restore any
  of them.
- **Activity:** every run, also the scheduled ones, with its result.
- **Log:** warnings of the runs started here.

**Restore** opens a version browser: pick a file or folder (or nothing, for everything) and
restore it to another folder or to its original location; right-click an item for all its
versions. A **preview** beside the browser (and **Preview** in Find a file) shows a file of any
version without restoring it, decrypted into memory only:

- Pictures (JPEG, PNG, GIF, BMP, TIFF, ICO, and HEIC or WebP where Windows has the codecs), fitted
  to the window or at full size, turned the way the camera recorded.
- Text and code with line numbers and syntax colours (C/C++, C#, Java/Kotlin, JavaScript and
  TypeScript, Go, Rust, Python, PowerShell, shell and batch files, SQL, Lua, XML/HTML, JSON, YAML,
  INI/TOML, CSS, Markdown), line wrapping on request, and **Ctrl+F** to find text (all matches
  highlighted, with next/previous and match case). Files over 8 MB show their beginning.

Small files of a local backup open as soon as they are selected; large files and cloud backups
wait for **Show preview**, since they have to be downloaded first.

**Settings** (in the sidebar) has, like System Informer: start Vaultik when you sign in, optionally
hidden in the notification area; show its icon there; turn the icon red when a backup needs
attention; close and/or minimize to the notification area; and a single click on the icon to show
or hide the window. From the notification area it watches every job, including scheduled runs, and shows a Windows notification when a
backup fails or skips files, or when a job has not backed up for too long (about twice its
interval: two days for a daily job; a set number of days for jobs without a schedule). The icon
turns red while something needs attention; its menu opens Vaultik, starts a job's backup, opens
Settings or quits. Closing the window keeps Vaultik in the notification area, and starting it
again brings the running one to the front. With Windows' Do Not Disturb on, the notifications
go quietly to the notification centre.

**Restore from a backup location** in the toolbar opens any Vaultik backup, in a folder or in
cloud storage, with its password, without a saved job: the way to get files back on another
computer. Copy `vaultik-gui.exe`, `vaultik.exe` and `vaultik-task.exe` there (they need nothing
else), connect the backup drive (or have the cloud storage keys at hand), pick the location and
enter the password.

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
vaultik check-job documents --full
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

It can also be S3-compatible cloud storage, written `s3:https://<endpoint>/<bucket>[/<folder>]`:

```
set AWS_ACCESS_KEY_ID=...
set AWS_SECRET_ACCESS_KEY=...
vaultik init   -r s3:https://<account id>.r2.cloudflarestorage.com/backups/my-pc
vaultik backup -r s3:https://s3.eu-central-003.backblazeb2.com/my-bucket/vaultik C:\Users\me\Documents
```

The bucket must exist. The signing region is worked out from the endpoint (`auto` for R2,
`us-east-1` when it cannot be); set `AWS_REGION` to override it. With `--job`, the job's stored
keys are used. Each repository file is one object; requests are retried with backoff on network
errors, throttling and server errors, and `--write-limit` / `--read-limit` cap uploads and
downloads. Browsing a cloud snapshot in the frontend downloads the folders it shows, so it is
slower than browsing a local one.

### Cloud drives

Locations `pcloud:<folder>`, `onedrive:<folder>`, `gdrive:<folder>` and `mega:<folder>` back up
to a folder in that drive. Sign in once in the frontend's **Cloud storage** dialog; the job then
keeps a sign-in token (never the password), sealed like the job password, and scheduled runs use
it. On the command line use `--job`, or put a token in `VAULTIK_CLOUD_SECRET`.

pCloud, OneDrive and Google Drive only let registered apps sign in, so each needs a free, one-time
registration of your own "Vaultik" app; its client ID (and secret) go into the dialog and are
remembered:

- **pCloud:** at [docs.pcloud.com](https://docs.pcloud.com), "My applications", create an app and
  add the redirect URI `http://localhost:53682/`. Enter its client ID and client secret. The
  folder is from the top of your pCloud; the token does not expire.
- **OneDrive:** in [portal.azure.com](https://portal.azure.com), **App registrations**, **New
  registration**: accounts "in any organizational directory and personal Microsoft accounts",
  platform **Mobile and desktop applications** with the redirect URI `http://localhost`, then
  **Authentication**, "Allow public client flows" on. Enter its Application (client) ID. Vaultik
  asks only for its own app folder (`OneDrive\Apps\<app name>`), and the folder is inside it.
  Files deleted by `prune` go to the OneDrive recycle bin, which counts against the quota until it
  is emptied.
- **Google Drive:** in [console.cloud.google.com](https://console.cloud.google.com), a project
  with the **Google Drive API** enabled, the **OAuth consent screen** set to External and
  **published** (in "Testing" the sign-in lapses after 7 days), and an **OAuth client ID** of type
  **Desktop app**. Enter its client ID and client secret. Vaultik asks only for the files it
  creates (`drive.file`), so it makes its own folders; one of the same name made by hand is not
  used. The folder is from the top of My Drive.
- **MEGA:** no registration: sign in with the email and password (and the two-factor code if the
  account has one). Vaultik stores the MEGA session and the account key, not the password;
  "close all other sessions" in MEGA's settings signs it out. MEGA encrypts files with its own
  scheme, which Vaultik does itself (on top of its own encryption). Free accounts have a download
  transfer limit, which large restores can reach.

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

### External drives

A job saving to a folder on a drive records the drive's volume ID (`"volume"` in the job
file). When Windows gives the drive another letter, the job follows it, so a backup drive that
comes back as `G:` instead of `F:` still works. With "Also back up when the destination drive is
connected" (Schedule tab), the running frontend or tray icon starts the job as soon as the drive
is plugged in (`run-job <job> --on-connect`, which does nothing when the job just saved a
version or is already running), then says when the drive can be unplugged.

### Locked cloud copies (S3 Object Lock)

For S3-compatible storage, the cloud settings of a job have "Lock uploads for N days" (on the
command line: `VAULTIK_S3_LOCK_DAYS`, and `VAULTIK_S3_LOCK_MODE=compliance`). Every repository
file except the short-lived lock files is then uploaded with Object Lock until that date: a
delete only hides it behind a delete marker, and an overwrite only adds a newer version. In
governance mode (the default) that stops Vaultik's keys only if they lack
`s3:BypassGovernanceRetention`, `s3:PutObjectRetention` and the right to change the bucket's lock
settings, so give Vaultik a key limited to reading, listing, writing and deleting. Compliance
mode cannot be lifted early by anyone, the account owner included, so the data is also billed
until its date. The bucket must have Object Lock turned on (most providers
only offer that when the bucket is created; Vaultik checks before saving). Data removed by
cleanup stays billed until its lock ends, so a lock a little longer than the oldest kept version
is enough.

After an attack, `vaultik cloud-rollback --job <job> --to "2026-11-09 20:30" --dry-run` lists
the files that were deleted or overwritten since that time, and without `--dry-run` brings them
back: later delete markers and versions are removed, or the old version is copied over a later
one that is itself locked. Files added later stay; old lock files never come back. Then run
`vaultik verify`.

### Ransomware warning

While backing up, Vaultik compares each changed file with its previous version. A document,
database or text file whose content went from compressible to random-looking is what encryption
by ransomware looks like (photos and videos look random already and never count), and so is a
file replaced by its name plus an extension (`report.docx` -> `report.docx.locked`). When at
least 20 files and 5% of the previous version's files look like that, the backup still saves
the new version but warns (`"alert"` in the history and the `done` event), pins the version
before it with a note, and the job skips removing old versions that run. The frontend shows the
warning and the tray icon turns red.

### Health checks

After `run-job` saves a version, it runs the job's health check when one is due (`--no-check`
skips it). The weekly check reads a share of the stored packs back (10% by default, a different
share each week, always at least one pack), checks their hashes and decrypts every chunk in
them; the monthly check reads everything. Both also check that every version's folders and
files point at data that exists. A check is its own entry in the job's history (`"kind":
"check"` or `"full_check"`), so a damaged drive turns the tray icon red and shows a
notification. `check-job <job> [--full]` runs one now; `verify --read-data --read-share 10` does
the same on any repository. New cloud jobs skip the monthly full check, since it downloads the
whole backup.

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
3. Cloud storage: S3-compatible storage (Cloudflare R2, Backblaze B2, Amazon S3, IDrive e2,
   MinIO) and cloud drives (pCloud, OneDrive, Google Drive, MEGA) (done, Windows)
4. Friendlier versions (done for the timeline, pins, notes, file history, search, cleanup
   presets, and a start page per job with the last result, next run, size, warnings and a
   calendar of backup days); still to come:
   - Browse a version in File Explorer as a folder or drive letter (Windows Projected File
     System, built into Windows, no third-party driver).
5. Knowing backups work, and protecting them (done):
   - Notifications and a tray icon (done).
   - Health checks (done): after a backup, a check reads a sample of the stored data weekly and
     all of it monthly, so a failing drive is noticed before a restore needs it.
   - Back up when the drive is plugged in (done): a job whose destination is an external drive
     runs when that drive connects (recognised by its volume ID, not its letter), then says it
     can be unplugged.
   - Ransomware warning (done): when a backup sees many files that look encrypted or were renamed
     with an added extension, it warns, pins the version before it and skips cleanup, so the
     last clean version is never removed automatically.
   - Locked cloud copies (done): S3 Object Lock (Backblaze B2, Wasabi, Amazon S3) keeps uploaded
     data undeletable for a set time, even with the keys, and `cloud-rollback` brings it back.
6. Portable version and installer, so Vaultik is easy to use on another computer:
   - Portable: the programs in one folder; a `portable` marker file next to them keeps jobs and
     settings in that folder instead of the user profile.
   - A copy on the backup drive: backups to a disk or USB drive keep `<drive>\Vaultik\` up to date
     with the portable programs and a short how-to-restore note, so another computer can restore
     with nothing installed.
   - Windows installer: Program Files, a Start menu entry, upgrades in place, and an uninstaller
     that also removes the scheduled tasks.
   - Linux: the same as an AppImage (portable: runs from the backup drive and on SteamOS) and a
     Flatpak, put on the backup drive next to the Windows programs once the Linux port (8) works.
7. More convenience and security:
   - A second copy (3-2-1): copy a backup to another destination after each run (USB drive to
     cloud or back), so one job keeps two independent copies.
   - A size and time estimate before the first backup.
   - Compare two versions: files added, changed and removed, with a side-by-side diff for text.
   - "Restore previous version" in File Explorer's right-click menu, opening the file's history.
   - Ready-made exclusions: temporary files, browser caches, `node_modules`, Windows clutter and
     the recycle bin, each one tick.
   - Pause and resume a long backup, and a slower speed during working hours.
   - Bulgarian translation of the frontend.
   - Unlock with Windows Hello or a security key instead of typing the backup password.
8. Linux port, SteamOS on the Steam Deck first. It starts with an in-depth review of the whole
   codebase once items 4 to 7 are done, so the Windows version is sound before porting. Then: build and test the existing POSIX platform layer,
   systemd user timers and the frontend, and give the cloud code its Linux parts (an HTTP client
   and the browser sign-in are Windows-only so far). Packaged as in 6.
9. Volume image backup, restore to a volume or `.vhdx`
10. Bare-metal restore from a recovery USB (WinPE on Windows)
