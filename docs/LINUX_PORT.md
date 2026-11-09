# Linux port: known hazards

Found by the code review before the port (November 2026). Each needs a decision or a fix when the
port starts; line numbers drift, so search for the named function.

## Metadata and restore

- `os_posix.cpp` `from_ticks`: times before 1970 become 0, and `set_metadata` then stamps
  1970-01-01; a real 1970 epoch time reads as "unset"; nanoseconds are cut to 100 ns
  (`tests/test_end_to_end.cpp` compares exact `last_write_time` and will be flaky).
- `os_posix.cpp` `set_metadata` opens the file read-only to `fchmod` it, so modes without owner
  read (0200, 0300) are never restored; mode 0 from a Linux snapshot is taken for "made on
  Windows" (`restore.cpp` then creates 0666 minus the umask).
- `restore.cpp`, `cli/main.cpp`: `\` is treated as a path separator, so Linux names containing
  a backslash cannot be addressed; `tree.cpp` `is_safe_name` on Windows lacks reserved device
  names and trailing dot or space checks for snapshots made on Linux.
- `jobs/job.cpp`: `json.dump()` throws on names that are not UTF-8 (legal on Linux); only the
  CLI's `emit` uses the replace handler.

## Processes and signals

- `os_posix.cpp` `run_command` allocates in the forked child of a multithreaded process (the
  lock refresh and stdin threads exist); an exec failure gives exit 127 instead of an error.
- `console_posix.cpp`: echo is turned off for the password prompt and only restored on a normal
  return; there is no SIGINT/SIGTERM handling, so `systemctl --user stop` kills a run without
  releasing its lock.
- `util/resources_posix.cpp`: `RLIMIT_AS` as the memory cap (the 128M minimum will hit
  `bad_alloc` with thread stacks and arenas, and it is never lifted when set back to 0); lowering
  nice cannot be undone unprivileged and the failure is swallowed, so switching back after
  working hours does not work.

## Scheduling

- `jobs/scheduler.cpp`: the hourly `OnCalendar` `00/N` restarts at midnight (an uneven gap when N
  does not divide 24, unlike Task Scheduler); the raw job name goes into the unit's Description
  (newline and `%` injection), `unit_quote` leaves `$` unescaped, HOME unset gives "/.config";
  `ExecStart` from `executable_path()` is a temporary mount path under an AppImage.

## Case, paths and exclusions

- `fold_case` is the identity on POSIX: `ops/history.cpp` find becomes case-sensitive
  (`tests/test_versions.cpp` will fail); `restore.cpp`'s `toupper` fallback maps `/home/a` to
  `/home/A`.
- `jobs/job.cpp` exclusion presets are Windows names and paths; `backup.cpp` backslash patterns
  from jobs written on Windows never match on Linux.
- `backup.cpp` `estimate_sources` skips no pseudo file systems; `is_pseudo_filesystem` lists
  tmpfs (a nested tmpfs is silently skipped) but not FUSE, NFS, SMB or overlay.

## Platform parts still missing

- `volume_of` / `volume_root` are stubs: no drive tracking, so a backup to an unmounted USB path
  writes into the empty mount point on the root file system, and on-connect never fires.
- `platform/http.cpp`, `loopback.cpp`, `vss.cpp`, `mount_posix.cpp`, `acl.cpp`: cloud storage,
  browser sign-in, shadow copies, browsing in a file manager and administrator jobs throw.
- `protect_secret` is the identity; the mode of an existing secrets file is never checked, and
  the `secrets` folder is created 0755.
- GUI: no single-instance guard or `--history` hand-over (`main.cpp`; its non-Windows
  `history_argument` also runs before the application exists), autostart is a no-op, the drive
  watcher never fires, and "Open in Explorer" is shown but always fails.

## Build

- `build.sh` has CRLF line endings in a Windows working copy (`core.autocrlf=true`); give it
  `eol=lf` in `.gitattributes`. The POSIX branch of CMakeLists links no Threads (fine on glibc
  2.34 and later).
