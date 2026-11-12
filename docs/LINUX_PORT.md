# Linux port: status

Roadmap item 8, SteamOS on the Steam Deck first. The hazards below were found by the code review
before the port (November 2026). Each is now **fixed**, **decided** (a reasoned choice, and what
it means) or **open**. Line numbers drift, so search for the named function.

## Verification

- Windows: debug and release build, all 212 test cases pass (the Linux-only cases in
  `tests/test_posix.cpp` are compiled out there).
- Linux (November 2026): built and tested on Ubuntu 24.04 (GCC 13, Qt 6.4) in a fresh WSL
  distribution. Debug and release build with no errors, the engine, the command line and the
  frontend; all 208 test cases pass in both when run as a normal user (as root, the root restore
  guard rightly refuses the tests' restores into `/tmp`). Build it with:

  ```
  sudo apt install g++ cmake ninja-build pkg-config libzstd-dev libssl-dev libcurl4-openssl-dev \
      nlohmann-json3-dev doctest-dev qt6-base-dev libqt6svg6-dev
  BUILD_DIR=~/vaultik-build ./build.sh debug test
  ```

  The command line end to end as a normal user, with `VAULTIK_CONFIG_DIR` in a scratch folder:
  init, two backups (the second stores only the change), snapshots, restore (contents identical
  with `diff -r`, modes 750 and 600 and a relative symlink come back), `verify --read-data`,
  forget with prune, then verify and restore again: all correct. The frontend starts (offscreen)
  and runs until it is ended.

  With FUSE (fuse3 3.14), as a normal user, debug and release: a backed-up folder mounted with
  `vaultik mount` matches the source (`diff -r`, and type, mode, size, modification time, owner and
  link target of every item), writing, creating and deleting are refused, and the mount ends and
  its folder goes on `fusermount3 -u`, Ctrl+C, SIGTERM and `cancel` with `--json` (the frontend's
  way, in the default `browse` folder); a killed mount's folder is removed by the next mount.
  `vaultik drives` lists WSL's virtual disks as unknown with the reason, as a user and as root.

  Fixed by this first build: the tests' `random_data` helper clashed with glibc's
  `struct random_data`; `own_admin_store` had no POSIX definition; `virt.cpp` included `acl.hpp`
  only on Windows; the theme used Qt 6.5 and 6.6 calls (colour scheme, accent colour), which now
  fall back on Qt 6.4 (dark when the palette's window is darker than its text; no live switch);
  the folders a backup puts above its sources (never read, no metadata) were restored with mode
  000; one test compared Windows paths and one compared nanosecond times exactly.

  Still not exercised on Linux: a restore as root (its refusal is), sealed secrets across two
  computers, the CPU cap's duty cycle under load, systemd timers, the GUI's tray, drive watcher
  and continuous backups on a desktop, the browser sign-in, cloud storage, the AppImage and the
  Flatpak, and a Steam Deck.

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
- **Fixed (limited)** - restoring as root (`restore.cpp`, `root_guard_`): Linux cannot hold a
  folder in place as Windows does (`File::hold_directory` and `final_path` are not supported
  there) and the restore writes, chowns and chmods by path, so a user who can rename entries in a
  folder on the way could swap it for a link to `/etc`. A restore running as root therefore goes
  only through folders root alone can change (`os::others_can_change`: root's, not writable by
  group or others, not a link): the target and every folder above it, or the restore stops with
  a message; an existing folder inside the restore that fails the test is skipped with a warning.
  Restoring as root straight into another account's folder (`/home/alice`) is refused: restore
  into a new folder under `/root` and move the files, or restore as that account.
- **Open** - a restore through directory descriptors (`openat`/`mkdirat` with `O_NOFOLLOW`, or
  `openat2` with `RESOLVE_BENEATH`, and `fchownat`/`fchmodat`/`utimensat` relative to them), which
  would let root restore into other accounts' folders safely.

## Processes and signals

- **Fixed** - `run_command` / `run_shell` use `posix_spawn` (nothing allocated in a forked child
  of a threaded process); a program that cannot start is an `OsError`, not exit code 127. Standard
  input is `/dev/null`; output goes to the caller or `/dev/null`, never into the `--json` stream;
  the timeout and `cancelled` stop the whole process group (TERM, then KILL after 2 s); the clean
  environment (a fixed PATH plus HOME, USER, LOGNAME, LANG and TZ) and extra variables work for
  both, the working folder for `run_shell`. A failed `waitpid` is an error, never exit code 0.
- **Fixed** - with `--json`, the watcher for `cancel` reads standard input from the descriptor,
  not `std::getline`: a thread blocked in `getline` holds glibc's stdin lock and `exit` waits for
  it, so every `--json` run whose caller kept the pipe open (the frontend) hung after its work.
- **Fixed** - `process_alive` answers false for an id past the `pid_t` range (it would have
  tested a process group).
- **Fixed** - `console_posix.cpp`: SIGINT, SIGTERM and SIGHUP set a flag that `stop_requested`
  reads, so Ctrl+C or `systemctl --user stop` ends a backup cleanly and releases its lock; a second
  signal ends the program at once. Echo comes back on every way out of the password prompt (also
  from the signal handler). SIGPIPE is ignored. The service unit sets `KillMode=mixed` and
  `TimeoutStopSec=120`.
- **Fixed** - `resources_posix.cpp`: the memory cap is `RLIMIT_DATA` plus 128 MiB for thread
  stacks (not `RLIMIT_AS`, which counts reserved address space), malloc arenas are limited to 2
  while capped, and the cap is lifted when set back to 0.
- **Fixed** - the CPU cap is a duty cycle: process CPU time (all threads) is kept at or below
  the allowed share of the wall time by sleeping at the throttle points, for any share below the
  whole machine (it used to do nothing from one core up); the window restarts after 10 quiet
  seconds so waiting on the network does not save up credit.
- **Decided** - nice values: a run applies its priority once, at start; working hours change only
  the read and write rates (`set_rate_limits`), never the priority, so "an unprivileged process
  cannot raise its priority again" never comes up. "High" stays normal without CAP_SYS_NICE.

## Scheduling

- **Fixed** - unit Descriptions are one line with `%` escaped and no trailing backslash (it
  would continue the line); `unit_quote` doubles `$` and `%`; without HOME the home folder comes
  from the password database (`os::user_folder`).
- **Fixed** - a Manual job gets no timer, also when it carries idle, lock or update triggers from
  Windows (an empty `OnCalendar=` made systemd refuse the timer).
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
  ::1), waited on with `poll` (`select` cannot take descriptors past 1023); the browser opens with
  `xdg-open` in the background (web addresses only).
- **Fixed** - `protect_secret` seals secrets (AES-256-GCM) with a random key in this account's own
  settings folder (`secrets/local.key` under `VAULTIK_CONFIG_DIR`, `$XDG_CONFIG_HOME/vaultik` or
  `~/.config/vaultik`, 0600 in a 0700 folder), and the sealed files are 0600 in a 0700 `secrets`
  folder tightened on every read and write (`os::make_private_dirs`). A portable copy keeps its
  settings on the backup drive, but the key stays on the computer, so the drive does not carry
  the repository password in readable form; on another computer the password is asked for again.
  Limitation: whatever can read the account's files (root, a program running as the user, a copy
  of the whole home folder) can still read the passwords, like an SSH key without a passphrase.
  libsecret needs a running, unlocked keyring, which scheduled runs before sign-in and SteamOS'
  Game Mode do not have; possible as an option.
- **Fixed** - `disk_posix.cpp`: a failed `fsync` of a disk image file is an error.
- **Fixed** - `crypto_openssl.cpp`: PBKDF2-SHA512 refuses iteration counts above `INT_MAX`.
- **Decided** - `vss.cpp`, `acl.cpp`, administrator jobs: Windows only. On Linux open files are
  read live; for other users' files run the engine as root from a system timer. The GUI hides "Run
  with administrator rights", "Start Vaultik as administrator" and Windows Hello.
- **Fixed** - `mount_posix.cpp`: a version is shown as a folder through FUSE 3 (libfuse3's
  high-level API, served by a loop of our own that polls the session's descriptor and a wake pipe,
  so the destructor can stop it at any time). Mounted `ro,auto_unmount`, without `allow_other`
  (only the mounting account sees it), and without `default_permissions` (the stored modes are
  shown, but the owner can read everything, as in the version). Sizes, modification and change
  times, modes, owners and symbolic links come from the version; items from Windows get the
  account's owner and 0755/0644 (0444 when read-only). Devices, pipes and Windows links are left
  out. File content is read chunk by chunk as the kernel asks (the last 8 chunks are kept), and
  the kernel caches listings, attributes and content for the whole mount (a version never
  changes). Writing gives `EROFS`. `vaultik mount` ends on Enter, Ctrl+C, SIGTERM, `cancel` from
  the frontend, a lost lock, or `fusermount3 -u` from outside (`SnapshotMount::active`). A killed process
  should be unmounted by `fusermount3` (`auto_unmount`), but fuse3 3.14 (Ubuntu 24.04) does not:
  its helper, running as root, is refused access to a mount without `allow_other` and gives up.
  So `remove_leftover` unmounts lazily whatever is still mounted (`fusermount3 -u -z`) before
  removing a leftover folder in `browse` (never deleting through a mount), which was tested.
  Standard input is polled in the wait loop rather than read by a thread: a thread blocked in
  `getline` holds the stdin lock, and glibc's `exit` then waits for it forever.
  Built only when pkg-config finds `fuse3` (`VAULTIK_FUSE`, on by default); otherwise
  `how_to_enable` says which package to install. A Flatpak cannot mount (no `fusermount3` in the
  sandbox); the AppImage and native builds can.
- **Fixed** - `drive_health_posix.cpp`: the drives are `/sys/block` (without loop, RAM, zram,
  device-mapper, RAID, optical and network devices and a memory card's boot areas); a folder's
  drives come from `/proc/self/mountinfo` (shared with `volume_of` through `mounts_posix.hpp`;
  btrfs through its source device) down through partitions, `slaves/` (LVM, LUKS, RAID) and a
  loop device's backing file to the whole disks. Model, serial (sysfs, else udev's
  `ID_SERIAL_SHORT`), size, bus (NVMe, USB, SD, Virtio, SATA, SCSI) and HDD/SSD (`rotational`)
  come from sysfs. The health comes from the drive: an NVMe drive's SMART / Health log (Get Log
  Page 02h, `NVME_IOCTL_ADMIN_CMD` on the controller `/dev/nvmeN`), an ATA drive's attribute
  table and thresholds (SMART READ DATA / READ THRESHOLDS through `SG_IO` and ATA PASS-THROUGH
  (16)), else `smartctl -j -a`. Devices are opened read-only and only reading commands are sent.
  The scoring is the Windows one: `apply_smart` for attribute tables, and `apply_nvme_log`
  (now shared, also used by Windows) for NVMe logs; smartctl's JSON is turned into the same
  inputs (`apply_smartctl_json`). The kernel lets only root send these commands (CAP_SYS_ADMIN
  for NVMe, CAP_SYS_RAWIO for ATA pass-through), so for a normal user the health is "unknown"
  with `unknown_reason` saying it needs root (the frontend shows it in the tooltip, `vaultik
  drives` on the drive's line); a normal user's refused request does not go on to smartctl,
  which would be refused too.
  The temperature comes from hwmon (nvme, drivetemp) for anyone. Memory cards and virtual disks
  (Hyper-V/WSL, QEMU, VMware, VirtualBox, virtio) are not asked and say so. `disk` is the whole
  disk's device number and `device` its `/dev` name; `vaultik drives` lists each disk's mount
  points. Possible later: udisks2 over D-Bus, which reads S.M.A.R.T. as root for everyone.
- **Decided** - `usn_journal_posix.cpp`: no change journal; every backup scans the folders.
- **Fixed** - app list (`apps_posix.cpp`): the Flatpak applications (`flatpak list`), which is how
  programs are installed on SteamOS; installing them again is left to the user (no winget).
- **Decided** - metered connections read as "not metered" (only NetworkManager knows, and not
  every system has it).

## GUI

- **Fixed** - one instance per user and settings folder through a user-only `QLocalServer` whose
  socket has an absolute path in `$XDG_RUNTIME_DIR` (when that is a 0700 folder of the account),
  else in a 0700 `~/.cache/vaultik` (a bare name would put it in `/tmp`, where another account
  could take the name first); without either there is no single-instance check. A second start
  hands `--history <path>` or "show" to the first. `history_argument` reads argv (it ran before
  the application object existed).
- **Fixed** - "Start Vaultik when I sign in": an XDG autostart entry
  (`~/.config/autostart/vaultik.desktop`) that starts `$APPIMAGE` or the Flatpak when run from
  those.
- **Fixed** - drive watcher: `/proc/self/mountinfo` signals every mount (POLLPRI, through a
  `QSocketNotifier`); new mount points go to the on-connect backups as drive letters do on Windows.
- **Fixed** - continuous backups: inotify, one watch per folder, new folders added as they
  appear; past `fs.inotify.max_user_watches` the rest is not watched.
- **Fixed** - "Open as folder" (Windows: "Open in Explorer") mounts the version (see FUSE above)
  and opens it with `QDesktopServices` (xdg-open), as other "open folder" actions do; when FUSE
  is missing the engine's message says what to install. Drive health that cannot be read shows
  the engine's reason instead of the Windows advice to run as administrator.

## Build

- **Fixed** - `.gitattributes` gives `*.sh` `eol=lf`; Linux links `Threads::Threads` and
  `CURL::libcurl`.
- **Fixed** - `parse_sftp_location` is in `repo/sftp_location.cpp`, built everywhere (the cloud
  dialog checks SFTP locations with it; the SFTP backend itself is Windows only so far).
  `cli/main.cpp` includes `<cstdlib>` everywhere (`std::system` in `image_cmds.inc`). The engine is
  installed without Qt too.
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
  `Settings-Linux` (the Windows copy uses `Settings`); the key that unlocks their saved passwords
  stays in `~/.config/vaultik/secrets` on each computer (see `protect_secret` above), so on another
  computer the password is asked for again. On the Deck: switch to Desktop Mode, open
  the drive in Dolphin and start the AppImage (a drive mounted `noexec` cannot run it: copy it to
  the home folder then). SteamOS updates reset the read-only system but not `/home`, where the
  AppImage, `~/.config/vaultik` and the systemd user timers live.
