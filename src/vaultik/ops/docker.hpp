#pragma once

// Docker volumes, backed up through Docker itself instead of by copying the files Docker keeps
// them in (inside Docker Desktop's virtual disk on Windows, below /var/lib/docker on Linux, where
// only root can read them).
//
//   Backup: a throwaway container that mounts the volume read-only runs "tar -c" over it, and its
//        output is streamed straight into the repository as the file Docker/<volume>.tar (no copy
//        on disk first). The container runs from an image that is already on this computer (never
//        pulled), without network, and is removed afterwards. Containers using the volume keep
//        running; a database in one may then be caught mid-write (see ops/databases.hpp).
//   Restore: a NEW volume is created and the .tar is fed into "tar -x" in a throwaway container
//        that mounts it. An existing volume is never written to.
//
// Linux containers and Windows containers (Docker Desktop in Windows containers mode) both work:
// the volume is mounted at /vaultik-volume or C:\vaultik-volume.

#include <chrono>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "vaultik/model/tree.hpp"
#include "vaultik/ops/backup.hpp"
#include "vaultik/ops/virt.hpp"
#include "vaultik/repo/repository.hpp"

namespace vaultik {

// The top folder of a version that holds the volumes.
inline constexpr char kDockerFolder[] = "Docker";

// The full path of a program: looked up on PATH (only absolute folders of it, never the current
// one), then, on Windows, in the folders its installer uses when it does not add itself to PATH
// (`extra_dirs`; a trailing '*' in a folder name matches any ending, the highest one first, e.g.
// "C:\Program Files\PostgreSQL\*\bin"). Windows adds ".exe" to `name`. Empty when not found.
std::string find_program(const std::string& name, const std::vector<std::string>& extra_dirs = {});

// The Docker command line program (docker.exe on Windows, also from Docker Desktop's own folder);
// empty when Docker is not installed.
std::string docker_program();

// Names Docker accepts for a named volume, and for a container: a letter or digit, then letters,
// digits, '_', '.' and '-'; at least 2 and at most 255 characters.
bool valid_volume_name(std::string_view name);
bool valid_container_name(std::string_view name);
// An image reference as Docker writes them ("busybox", "busybox:1.36", "ghcr.io:443/a/b@sha256:..."):
// a letter or digit first (so never an option such as "--privileged"), then letters, digits and
// '_', '.', '-', '/', ':', '@'; at most 512 characters.
bool valid_image_reference(std::string_view image);

// Where a volume's export is in a version: {"Docker", "<volume>.tar"} and "Docker/<volume>.tar".
std::vector<std::string> volume_place(const std::string& volume);
std::string volume_snapshot_path(const std::string& volume);
// The volume a version path "Docker/<volume>.tar" holds (either separator); empty for any other.
std::string volume_of_path(std::string_view snapshot_path);

struct DockerVolume {
    std::string name;
    std::string driver;
    std::vector<std::string> running;  // the running containers that use it
};

struct DockerInfo {
    bool available = false;  // Docker is installed and its engine answers
    std::string problem;     // why not, in plain words (not installed, not running)
    std::string os_type;     // the engine's containers: "linux" or "windows"
    std::string program;     // docker(.exe)
};
// Asks the Docker engine whether it runs (docker info). Takes a few seconds when it does not.
DockerInfo docker_info();

// The engine's volumes with the running containers using them. Throws Error.
std::vector<DockerVolume> docker_volumes(const DockerInfo& info);

// The images on this computer ("repository:tag"). Throws Error.
std::vector<std::string> docker_images(const DockerInfo& info);

// Parsing what docker prints for the formats docker_volumes asks for:
//   "docker volume ls --format {{.Name}}\t{{.Driver}}": one volume per line.
std::vector<DockerVolume> parse_volume_list(std::string_view text);
//   "docker ps --no-trunc --format {{.Names}}\t{{.Mounts}}": per volume name, the containers
//   whose mounts name it (a mount is a volume's name or a bind-mounted folder's path).
std::map<std::string, std::vector<std::string>> parse_container_mounts(std::string_view text);
//   "docker image ls --format {{.Repository}}:{{.Tag}}": the images, without "<none>" ones.
std::vector<std::string> parse_image_list(std::string_view text);

// A plain explanation of a failed docker command from its output: "Docker is not running" when
// the engine could not be reached, otherwise the first line of what it printed.
std::string docker_problem(std::string_view output, int exit_code);

// The image the export and import containers run, from the images on this computer: one known to
// have tar (busybox, alpine, debian, ubuntu for Linux containers; Windows' nanoserver or
// servercore for Windows containers), in that order. `wanted` (from the job) is used as it is
// when not empty. Throws Error saying which image to pull once when there is none.
std::string pick_helper_image(const std::vector<std::string>& images, const std::string& os_type,
                              const std::string& wanted = {});

// Where the helper container mounts the volume.
std::string volume_mount_point(const std::string& os_type);

// The command lines (program first) that export a volume as a tar stream on standard output, and
// that unpack a tar stream from standard input into a volume. `container` names the throwaway
// container (so it can be removed when the run is stopped).
std::vector<std::string> volume_export_command(const std::string& docker, const std::string& volume,
                                               const std::string& image, const std::string& os_type,
                                               const std::string& container);
std::vector<std::string> volume_import_command(const std::string& docker, const std::string& volume,
                                               const std::string& image, const std::string& os_type,
                                               const std::string& container);

// A new name for a throwaway container: "vaultik-<what>-<random>".
std::string helper_container_name(const std::string& what);

// The warning for a chosen volume that running containers use (empty when none do).
std::string volume_in_use_warning(const DockerVolume& volume);

// The export of a volume as a backup's extra source (see program_stream for `cancelled` and
// `stall`).
ExtraSource docker_volume_source(const DockerInfo& info, const std::string& volume, const std::string& image,
                                 std::function<bool()> cancelled,
                                 std::chrono::seconds stall = std::chrono::minutes(15));

// Imports the export `file` (a file node of a version, see volume_place) into a NEW volume
// `new_name`, which must not exist (an existing volume is never touched), streaming it from the
// repository into tar in a throwaway container. When the import fails, the volume it created is
// removed again. `image` as for pick_helper_image. Throws Error (also when cancelled).
void docker_import(Repository& repo, const Node& file, const std::string& new_name, const std::string& image = {},
                   const std::function<void(const VirtProgress&)>& progress = {},
                   const std::function<bool()>& cancelled = {});

}  // namespace vaultik
