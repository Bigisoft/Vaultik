# Linux port: status

Roadmap item 8, SteamOS on the Steam Deck first. The hazards below were found by the code review
before the port (November 2026). Each is now **fixed**, **decided** (a reasoned choice, and what
it means) or **open**. Line numbers drift, so search for the named function.

## Verification

- Windows: debug and release build, all 157 test cases pass (the Linux-only cases in
  `tests/test_posix.cpp` are compiled out there).
- Linux: **not yet compiled or run.** The WSL Ubuntu distribution on the development PC stopped
  answering (every `wsl -d Ubuntu ...` hung, also `--exec /bin/echo`) for the whole session, so
  the code written for Linux has not met a compiler. Until it has, expect compile errors in
  code that was never built on Linux before (the whole GUI and parts of the engine). To verify:

  ```
  sudo apt install g++ cmake ninja-build pkg-config libzstd-dev libssl-dev libcurl4-openssl-dev \
      nlohmann-json3-dev doctest-dev qt6-base-dev libqt6svg6-dev
  BUILD_DIR=~/vaultik-build ./build.sh debug test
  ```

  then the command line end to end against a temporary repository (init, backup, snapshots,
  restore, verify, forget/prune) with `VAULTIK_CONFIG_DIR` set to a scratch folder.

## Metadata and restore

- **Fixed** - `os_posix.cpp` `to_ticks` / `from_ticks`: times before 1970 and exactly at the epoch
  round-trip (signed arithmetic, floor division); 0 stays "unset", which a real time cannot be (it
  would be 1601-01-01). Nanoseconds are kept to 100 ns, the format's resolution, and a restored
  file reads back exactly the stored time, so exact comparisons hold.
- **Fixed** - `set_metadata` changes the mode through an `O_PATH | O_NOFOLLOW` descriptor and its
  `/proc/self/fd` entry, so 0200 and 0300 are restored and a path swapped for a link is still not
  followed (without `/proc` the mode is left alone).
- **Fixed** - mode 0: with Windows attributes (never 0 on Windows: a plain file is NORMAL or
  ARCHIVE) the node comes from Windows and the file gets the umask default; mode 0 with attributes
  0 is a real Linux mode and is restored (`restore.cpp` `has_posix_mode`, `set_metadata`).
- **Fixed** - `restore.cpp` `split_snapshot_path` and the `history` and `ls` commands: `\`
  separates folders only on Windows; on Linux it is a name character.
- **Fixed** - `tree.cpp` `is_safe_name` on Windows refuses a trailing dot or space and the
  reserved device names (`CON`, `nul.txt`, `COM1`, `LPT9.log`, ...), which Linux allows and
  Windows programs cannot open or delete. They are skipped with a warning like other unsafe names.
- **Fixed** - history records, computer status records and monthly reports replace bytes that
  are not UTF-8 (Linux names need not be); saving a job with such a path gives a clear error
  instead of storing a changed path.

## Processes and signals

- **Fixed** - `run_command` / `run_shell` use `posix_spawn` (nothing allocated in a forked child
  of a threaded process); a program that cannot start is an `OsError`, not exit code 127. Standard
  input is `/dev/null`; output goes to the caller or `/dev/null`, never into the `--json` stream;
  the timeout and `cancelled` stop the whole process group (TERM, then KILL after 2 s); the clean
  environment, extra variables and working folder work.
- **Fixed** - `console_posix.cpp`: SIGINT, SIGTERM and SIGHUP set a flag that `stop_requested`
  reads, so Ctrl+C or `systemctl --user stop` ends a backup cleanly and releases its lock; a second
  signal ends the program at once. Echo comes back on every way out of the password prompt (also
  from the signal handler). SIGPIPE is ignored. The service unit sets `KillMode=mixed` and
  `TimeoutStopSec=120`.
- **Fixed** - `resources_posix.cpp`: the memory cap is `RLIMIT_DATA` plus 128 MiB for thread
  stacks (not `RLIMIT_AS`, which counts reserved address space), malloc arenas are limited to 2
  while capped, and the cap is lifted when set back to 0.
- **Decided** - nice values: a run applies its priority once, at start; working hours change only
  the read and write rates (`set_rate_limits`), never the priority, so "an unprivileged process
  cannot raise its priority again" never comes up. "High" stays normal without CAP_SYS_NICE.

## Scheduling

- **Fixed** - unit Descriptions are one line with `%` escaped; `unit_quote` doubles `$` and `%`;
  without HOME the home folder comes from the password database.
- **Fixed** - ExecStart under an AppImage is `"$APPIMAGE" --cli run-job ...` (the program folder
  is a temporary mount); `packaging/linux/AppRun` hands `--cli` to the engine. In a Flatpak it is
  `flatpak run --command=vaultik io.github.bigisoft.Vaultik ...`, the units go to the real
  `~/.config/systemd/user` and systemctl runs on the host through `flatpak-spawn --host`.
- **Decided** - hourly `OnCalendar=*-*-* 00/N:MM`: the same as Task Scheduler for every N that
  divides 24 (1, 2, 3, 4, 6, 8, 12); for other N the last gap before midnight is shorter. systemd
  has no "every N hours from a start time" that also catches up missed runs (`Persistent=` works
  with calendar timers only).
- **Decided** - idle, lock and sign-out triggers are Task Scheduler and Windows session events;
  the job dialog hides them on Linux (desktops differ too much to rely on one signal).

## Case, paths and exclusions

- **Fixed** - searching (`history.cpp` `name_matches`) ignores case on Linux too (ASCII, Latin-1,
  Latin Extended-A, Greek, Cyrillic); `fold_case` stays the identity for paths, which are case
  sensitive there.
- **Fixed** - `resolve_snapshot_path` takes a lower-case drive letter only at the top level, so
  `/home/a` is never taken for `/home/A`.
- **Decided** - exclusion presets stay as they are: a job "uses" a preset when it has all of its
  patterns, so adding Linux patterns would untick the preset on every saved Windows job. The
  Windows paths never match on Linux; `*/.cache`, the `dev` set and `*.tmp` do. Jobs belong to one
  computer, so backslash patterns written on Windows do not reach Linux.
- **Decided** - `estimate_sources` and `is_pseudo_filesystem`: kernel file systems (proc, sysfs,
  tmpfs, cgroup, ...) are skipped; FUSE, NFS, SMB and overlay are not, because a user who picked a
  folder on them wants it backed up. A tmpfs below a source is skipped (memory, gone after a
  reboot).

## Platform parts

- **Fixed** - `volume_of` / `volume_root`: volumes are named `uuid:<file system UUID>` from
  `/dev/disk/by-uuid` and found in `/proc/self/mountinfo` (btrfs through its source device, bind
  mounts through the whole mount). `external` means USB, FireWire, a memory card (the Deck's
  microSD slot) or a removable disk; `system` means the same disk as `/`. A job saved while its
  drive is mounted runs only when that drive is mounted again (wherever), so a backup never goes
  into the empty mount point on the root file system.
- **Fixed** - `platform/http.cpp`: libcurl (system certificates, `https_proxy`), TLS 1.2 or newer,
  no redirects, no headers the caller did not ask for (signed requests), uploads read straight from
  memory, a reused connection cache per thread.
- **Fixed** - `loopback.cpp`: the browser sign-in's local server on POSIX sockets (127.0.0.1 and
  ::1); the browser opens with `xdg-open` in the background (web addresses only).
- **Decided** - `protect_secret` stays a 0600 file, now in a 0700 `secrets` folder that is
  tightened on every read and write (`os::make_private_dirs`). Limitation: whatever can read the
  account's files (root, a program running as the user, a copy of the home folder) can read the
  passwords, like an SSH key without a passphrase. libsecret needs a running, unlocked keyring,
  which scheduled runs before sign-in and SteamOS' Game Mode do not have; possible as an option.
- **Decided** - `vss.cpp`, `acl.cpp`, administrator jobs: Windows only. On Linux open files are
  read live; for other users' files run the engine as root from a system timer. The GUI hides "Run
  with administrator rights", "Start Vaultik as administrator" and Windows Hello.
- **Open** - `mount_posix.cpp`: showing a version as a folder needs FUSE; "Open in Explorer" is
  hidden on Linux until then (restore and Find a file cover it).
- **Open** - `drive_health_posix.cpp`: SMART needs root (smartctl) or udisks2 over D-Bus; drive
  health reads "unknown".
- **Decided** - `usn_journal_posix.cpp`: no change journal; every backup scans the folders.
- **Fixed** - app list (`apps_posix.cpp`): the Flatpak applications (`flatpak list`), which is how
  programs are installed on SteamOS; installing them again is left to the user (no winget).
- **Decided** - metered connections read as "not metered" (only NetworkManager knows, and not
  every system has it).

## GUI

- **Fixed** - one instance per user and settings folder through a user-only `QLocalServer`; a
  second start hands `--history <path>` or "show" to the first. `history_argument` reads argv
  (it ran before the application object existed).
- **Fixed** - "Start Vaultik when I sign in": an XDG autostart entry
  (`~/.config/autostart/vaultik.desktop`) that starts `$APPIMAGE` or the Flatpak when run from
  those.
- **Fixed** - drive watcher: `/proc/self/mountinfo` signals every mount (POLLPRI, through a
  `QSocketNotifier`); new mount points go to the on-connect backups as drive letters do on Windows.
- **Fixed** - continuous backups: inotify, one watch per folder, new folders added as they
  appear; past `fs.inotify.max_user_watches` the rest is not watched.
- **Fixed** - "Open in Explorer" is hidden (see FUSE above); other "open folder" actions use
  `QDesktopServices` (xdg-open).

## Build

- **Fixed** - `.gitattributes` gives `*.sh` `eol=lf`; Linux links `Threads::Threads` and
  `CURL::libcurl`.
- **Fixed** - CMake uses the distribution's packages without vcpkg (zstd and BLAKE3 also through
  pkg-config; otherwise BLAKE3 1.5.4 is downloaded at a fixed SHA-256). `build.sh` builds with
  them, `build.sh vcpkg` and the `linux-vcpkg-*` presets with vcpkg. `-DVAULTIK_BUILD_TESTS=OFF`
  builds without doctest.

## Packaging (roadmap 6)

- **Written, not yet run** - `tools/appimage.sh` builds `dist/Vaultik-x86_64.AppImage` with
  linuxdeploy and its Qt plugin. The AppImage needs a C library at least as new as the build
  system's: build it on the oldest distribution it should run on (`ldd --version` on the Deck).
- **Written, not yet built** - `packaging/flatpak/io.github.bigisoft.Vaultik.yml` (KDE runtime 6.8).
- **Running from the backup drive on SteamOS** - after a backup to an external drive, a run from
  the AppImage copies it to `<drive>/Vaultik/Vaultik-x86_64.AppImage` with "How to restore on
  Linux.txt", next to the Windows programs. The `portable` file there keeps the Linux settings in
  `Settings-Linux` (the Windows copy uses `Settings`). On the Deck: switch to Desktop Mode, open
  the drive in Dolphin and start the AppImage (a drive mounted `noexec` cannot run it: copy it to
  the home folder then). SteamOS updates reset the read-only system but not `/home`, where the
  AppImage, `~/.config/vaultik` and the systemd user timers live.
