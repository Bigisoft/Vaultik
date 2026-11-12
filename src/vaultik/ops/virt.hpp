#pragma once

// Virtual machines (Windows): WSL distributions and Hyper-V machines, backed up through their own
// export tools instead of by copying their disk files, which are open while they run (so copied
// half-written, or not readable at all).
//
//   WSL: "wsl --export <distro> -" is streamed straight into the repository as the file
//        WSL/<distro>.tar (no copy on disk first; deduplication then finds what is unchanged in
//        the next version's export). Restoring imports such a file as a new distribution with
//        "wsl --import <new name> <folder> -", fed from the repository. WSL belongs to the user's
//        own account, so this is for jobs without administrator rights.
//   Hyper-V: Export-VM (which takes a consistent copy of a running machine through a checkpoint)
//        writes the machine's configuration and disks to a staging folder; that folder is backed
//        up as Hyper-V/<machine>/ and removed afterwards. Restoring puts the files in a folder and
//        imports them as a new machine (Import-VM -Copy -GenerateNewId). Hyper-V needs
//        administrator rights, so this is for jobs that run with them.

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "vaultik/model/snapshot.hpp"
#include "vaultik/model/tree.hpp"
#include "vaultik/ops/backup.hpp"
#include "vaultik/repo/repository.hpp"

namespace vaultik {

// The top folders of a version that hold them (never a drive letter or "UNC").
inline constexpr char kWslFolder[] = "WSL";
inline constexpr char kHyperVFolder[] = "Hyper-V";

// Text printed by wsl.exe: UTF-16 (its default when its output is redirected) or UTF-8 (with
// WSL_UTF8=1), as UTF-8 without carriage returns or a byte order mark.
std::string console_text(std::string_view raw);

// A program's standard output as a backup's stream: `argv` runs (with `env` on top of this
// process's environment) when the stream is opened, and what it prints is the file. `cancelled` is
// polled while it runs; when it says so, or nothing arrives for `stall`, the program is stopped and
// the item skipped. finish() throws Error when the program failed or printed less than `min_size`
// bytes; `what` names it in those messages ("wsl --export").
std::unique_ptr<StreamSource> program_stream(const std::vector<std::string>& argv,
                                             const std::vector<std::pair<std::string, std::string>>& env,
                                             const std::string& what, std::function<bool()> cancelled,
                                             std::chrono::seconds stall, std::uint64_t min_size);

// A file on disk as a backup's stream, stored at `place` (a database dump, see ops/databases.hpp).
ExtraSource file_stream_source(std::vector<std::string> place, const std::string& path);

// ---- WSL ------------------------------------------------------------------------------------

struct WslDistro {
    std::string name;
    std::string folder;  // where it is installed (its BasePath)
    std::string disk;    // its ext4.vhdx (WSL 2); empty for WSL 1, which keeps plain files
    int version = 2;
};

// The current account's distributions, from its registry (works while WSL itself is busy); empty
// when WSL is not installed, and on POSIX.
std::vector<WslDistro> wsl_distros();
// The names of the distributions running now ("wsl --list --running --quiet"). Throws Error when
// wsl.exe cannot tell (it did not answer within a minute, or is missing).
std::vector<std::string> wsl_running();
// The distribution names in a list printed by wsl.exe: one per line, trimmed; lines that are not
// a valid name (messages such as "There are no running distributions.") are left out.
std::vector<std::string> parse_distro_names(std::string_view text);

// Names WSL accepts for a distribution: letters, digits, '.', '_' and '-', at most 64.
bool valid_distro_name(std::string_view name);

// Where a distribution's export is in a version: {"WSL", "<name>.tar"} and "WSL/<name>.tar".
std::vector<std::string> wsl_place(const std::string& distro);
std::string wsl_snapshot_path(const std::string& distro);
// The distribution a version path "WSL/<name>.tar" holds (either separator); empty for any other.
std::string distro_of_path(std::string_view snapshot_path);

// What a backup does with a chosen distribution that is running: leave it out (with a warning),
// export it while it runs (files being written at that moment may be caught half-way), or stop it
// first ("wsl --terminate"; it starts again when next used).
enum class WslWhenRunning { Skip, Export, Stop };
const char* to_string(WslWhenRunning w);
// "skip", "export", "stop"; throws Error for anything else.
WslWhenRunning parse_wsl_when_running(std::string_view text);

struct WslPlan {
    std::vector<std::string> export_names;  // in the chosen order, as installed (its spelling)
    std::vector<std::string> stop_first;    // of those, the running ones to stop before exporting
    std::vector<std::string> not_installed;  // chosen ones that are not installed (any more)
    // Running ones left out (the installed spelling), with a warning saying why.
    std::vector<std::pair<std::string, std::string>> running_skipped;
};
// Which chosen distributions a backup exports, given those installed and those running (names
// compare ignoring case, as WSL does).
WslPlan plan_wsl(const std::vector<std::string>& chosen, const std::vector<WslDistro>& installed,
                 const std::vector<std::string>& running, WslWhenRunning when);

// Stops a running distribution ("wsl --terminate <name>"). Throws Error.
void wsl_terminate(const std::string& name);

// The export of a distribution as a backup's extra source. `cancelled` is polled while the export
// runs; when it says so, or nothing arrives for `stall` (WSL hangs now and then), the export is
// stopped and the item skipped.
ExtraSource wsl_export_source(const std::string& name, std::function<bool()> cancelled,
                              std::chrono::seconds stall = std::chrono::minutes(15));

// Patterns for BackupOptions::excludes that leave out these files (their exact paths, also when
// given in the \\?\ form), so a distribution or machine backed up through its export is not also
// copied as a raw disk file.
std::vector<std::string> disk_excludes(const std::vector<std::string>& files);

struct VirtProgress {
    std::uint64_t bytes = 0;
    std::uint64_t total = 0;
};

// Imports the export `file` (a file node of a version, see wsl_place) as a new distribution
// `name` installed in `folder` (created when missing; it must hold no ext4.vhdx yet), streaming it
// from the repository into "wsl --import <name> <folder> -". A distribution of that name must not
// exist, unless `replace`, which unregisters it first (deleting its disk; the caller asked and the
// user confirmed), but only after every piece of the export was read and checked, so a damaged or
// incomplete backup never costs the existing distribution (progress then counts both passes).
// Throws Error (also when cancelled).
void wsl_import(Repository& repo, const Node& file, const std::string& name, const std::string& folder, bool replace,
                const std::function<void(const VirtProgress&)>& progress = {},
                const std::function<bool()>& cancelled = {});

// ---- Hyper-V --------------------------------------------------------------------------------

struct HyperVm {
    std::string name;
    std::string state;               // "Running", "Off", "Saved", ...
    std::vector<std::string> disks;  // the paths of its virtual disks
};

struct HyperVList {
    bool available = false;  // Hyper-V is installed and could be asked
    std::string problem;     // why not, in plain words (not installed, needs administrator rights)
    std::vector<HyperVm> machines;
};
// The machines on this computer, through PowerShell's Get-VM (needs administrator rights, or
// membership of Hyper-V Administrators).
HyperVList hyperv_machines();
// The machines in Get-VM's JSON as hyperv_machines asks for it (an object or an array of them).
std::vector<HyperVm> parse_hyperv_list(std::string_view json);

// Names a backup or import accepts for a machine: not empty, at most 100 characters, no control
// characters and none of * ? [ ] (Get-VM would take them as wildcards and match other machines).
bool valid_vm_name(std::string_view name);
// A machine's name as one file name: characters Windows does not allow in names become '_', and a
// name changed that way gets "-<8 hex digits>" from the real name, so two machines never share a
// folder ("a:b" and "a_b"). A name that needs no change stays as it is (so does the result).
std::string vm_file_name(std::string_view name);
std::vector<std::string> hyperv_place(const std::string& machine);
std::string hyperv_snapshot_path(const std::string& machine);
// The folder name of a version path "Hyper-V/<folder>" (the machine's name, as vm_file_name made
// it; the real name is inside the export), used to name an import; empty for any other path.
std::string machine_of_path(std::string_view snapshot_path);

// Where machines are exported before the backup reads them, by default:
// <ProgramData>\Vaultik\hyperv-export (only administrators can read it: the disks hold the
// machine's data).
std::string default_hyperv_staging();

// Whether a run folder in the export folder was left by a run that ended without removing it
// (killed, or the computer lost power): its process is gone, or it is older than a week (the
// process id may have been reused). One from before the process id was in the name
// ("run-<random>"), once it is a day old.
// `own_pid` is this process, `alive` tells whether a process runs.
bool hyperv_run_left_over(const std::string& name, std::chrono::hours age, std::uint32_t own_pid,
                          const std::function<bool(std::uint32_t)>& alive);

// Exports machine `name` (running or not) with Export-VM into `folder`, a new or empty folder,
// and returns the folder holding the export (`folder`\<machine>). Throws Error.
std::string hyperv_export(const std::string& name, const std::string& folder,
                          const std::function<bool()>& cancelled = {});

// The configuration file (.vmcx) of an exported machine below `folder` ("Virtual Machines\*.vmcx");
// throws Error when there is none or more than one.
std::string find_vmcx(const std::string& folder);

// Imports the exported machine in `folder` (a restored Hyper-V/<machine> folder) as a new machine:
// its files are copied into `destination` (a new or empty folder) with a new id, and it is named
// `new_name` (empty: the name it was exported with, which must then not be taken). Never touches
// an existing machine: a machine called `new_name` must not exist. Returns the new machine's name.
// Needs administrator rights. Throws Error.
std::string hyperv_import(const std::string& folder, const std::string& new_name, const std::string& destination);

}  // namespace vaultik
