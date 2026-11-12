#include "vaultik/ops/docker.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>

#include "vaultik/crypto/crypto.hpp"
#include "vaultik/platform/os.hpp"
#include "vaultik/util/error.hpp"
#include "vaultik/util/hex.hpp"

#ifndef _WIN32
#include <unistd.h>
#endif

namespace vaultik {

namespace {

namespace fs = std::filesystem;

fs::path fs_path(const std::string& utf8) {
    return fs::path(std::u8string(utf8.begin(), utf8.end()));
}

std::string utf8_of(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::string trim(std::string_view s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = s.find_last_not_of(" \t\r\n");
    return std::string(s.substr(first, last - first + 1));
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}

std::vector<std::string> split(std::string_view text, char sep) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t at = text.find(sep, start);
        out.emplace_back(text.substr(start, at == std::string_view::npos ? std::string_view::npos : at - start));
        if (at == std::string_view::npos) break;
        start = at + 1;
    }
    return out;
}

std::vector<std::string> lines_of(std::string_view text) {
    std::vector<std::string> out;
    for (std::string& line : split(text, '\n')) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!trim(line).empty()) out.push_back(std::move(line));
    }
    return out;
}

std::string first_line(std::string_view text) {
    for (const std::string& line : lines_of(text)) return trim(line);
    return {};
}

// "a10" after "a9": numbers in names compare by value, so "PostgreSQL\17" is found before "\9".
bool natural_less(const std::string& a, const std::string& b) {
    std::size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        const bool da = a[i] >= '0' && a[i] <= '9';
        const bool db = b[j] >= '0' && b[j] <= '9';
        if (da && db) {
            std::size_t ei = i, ej = j;
            while (ei < a.size() && a[ei] >= '0' && a[ei] <= '9') ++ei;
            while (ej < b.size() && b[ej] >= '0' && b[ej] <= '9') ++ej;
            std::string_view na(a.data() + i, ei - i), nb(b.data() + j, ej - j);
            while (na.size() > 1 && na.front() == '0') na.remove_prefix(1);
            while (nb.size() > 1 && nb.front() == '0') nb.remove_prefix(1);
            if (na.size() != nb.size()) return na.size() < nb.size();
            if (na != nb) return na < nb;
            i = ei;
            j = ej;
            continue;
        }
        const char ca = static_cast<char>(std::tolower(static_cast<unsigned char>(a[i])));
        const char cb = static_cast<char>(std::tolower(static_cast<unsigned char>(b[j])));
        if (ca != cb) return ca < cb;
        ++i;
        ++j;
    }
    return a.size() - i < b.size() - j;
}

// The folders a pattern names: each component ending with '*' matches the entries of its parent
// starting with the rest, the highest (natural order) first.
std::vector<std::string> expand_dirs(const std::string& pattern) {
    std::vector<std::string> current;
    const fs::path p = fs_path(pattern);
    fs::path root = p.root_path();
    current.push_back(utf8_of(root));
    for (const fs::path& part : p.relative_path()) {
        const std::string name = utf8_of(part);
        std::vector<std::string> next;
        for (const std::string& dir : current) {
            if (!name.empty() && name.back() == '*') {
                const std::string prefix = lower(name.substr(0, name.size() - 1));
                std::vector<std::string> found;
                std::error_code ec;
                for (fs::directory_iterator it(fs_path(dir), ec), end; !ec && it != end; it.increment(ec)) {
                    const std::string entry = utf8_of(it->path().filename());
                    std::error_code dec;
                    if (lower(entry).starts_with(prefix) && it->is_directory(dec)) found.push_back(entry);
                }
                std::sort(found.begin(), found.end(), [](const std::string& a, const std::string& b) {
                    return natural_less(b, a);
                });
                for (const std::string& f : found) next.push_back(utf8_of(fs_path(dir) / fs_path(f)));
            } else {
                next.push_back(utf8_of(fs_path(dir) / part));
            }
        }
        current = std::move(next);
    }
    return current;
}

bool is_program(const std::string& path) {
    std::error_code ec;
    if (!fs::is_regular_file(fs_path(path), ec)) return false;
#ifndef _WIN32
    if (::access(path.c_str(), X_OK) != 0) return false;
#endif
    return true;
}

// Runs docker and returns its exit code; its output (both streams) goes into `output`.
int run_docker(const std::string& docker, std::vector<std::string> args, std::string& output,
               std::chrono::seconds timeout = std::chrono::minutes(2)) {
    args.insert(args.begin(), docker);
    os::CommandOptions o;
    o.output = &output;
    o.timeout = timeout;
    return os::run_command(args, o);
}

bool valid_docker_name(std::string_view name) {
    if (name.size() < 2 || name.size() > 255) return false;
    const auto alnum = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    };
    if (!alnum(name[0])) return false;
    return std::all_of(name.begin() + 1, name.end(),
                       [&](char c) { return alnum(c) || c == '_' || c == '.' || c == '-'; });
}

// Removes a helper container that may still run (the docker client was stopped). Best effort.
void remove_container(const std::string& docker, const std::string& name) {
    try {
        std::string ignored;
        run_docker(docker, {"rm", "-f", name}, ignored, std::chrono::minutes(1));
    } catch (const std::exception&) {
    }
}

// The export stream, which also removes the helper container when the export did not end well
// (docker run --rm removes it when it ends by itself; a stopped client leaves it running).
class VolumeExport : public StreamSource {
public:
    VolumeExport(std::unique_ptr<StreamSource> inner, std::string docker, std::string container)
        : inner_(std::move(inner)), docker_(std::move(docker)), container_(std::move(container)) {}
    ~VolumeExport() override {
        if (!finished_) {
            inner_.reset();  // stops the client first
            remove_container(docker_, container_);
        }
    }
    std::size_t read(std::uint8_t* dst, std::size_t max) override { return inner_->read(dst, max); }
    void finish() override {
        inner_->finish();
        finished_ = true;
    }

private:
    std::unique_ptr<StreamSource> inner_;
    std::string docker_;
    std::string container_;
    bool finished_ = false;
};

}  // namespace

std::string find_program(const std::string& name, const std::vector<std::string>& extra_dirs) {
#ifdef _WIN32
    const std::string file = lower(name).ends_with(".exe") ? name : name + ".exe";
    constexpr char kSep = ';';
#else
    const std::string& file = name;
    constexpr char kSep = ':';
#endif
    if (name.empty() || name.find_first_of("/\\") != std::string::npos) return {};
    for (std::string dir : split(os::get_env("PATH"), kSep)) {
        dir = trim(dir);
        if (dir.size() >= 2 && dir.front() == '"' && dir.back() == '"') dir = dir.substr(1, dir.size() - 2);
        // Only absolute folders: a relative one would be looked up from wherever this runs.
        if (dir.empty() || !fs_path(dir).is_absolute()) continue;
        const std::string candidate = utf8_of(fs_path(dir) / fs_path(file));
        if (is_program(candidate)) return candidate;
    }
    for (const std::string& pattern : extra_dirs)
        for (const std::string& dir : expand_dirs(pattern)) {
            const std::string candidate = utf8_of(fs_path(dir) / fs_path(file));
            if (is_program(candidate)) return candidate;
        }
    return {};
}

std::string docker_program() {
    std::vector<std::string> extra;
#ifdef _WIN32
    const std::string pf = os::get_env("ProgramFiles");
    if (!pf.empty()) extra.push_back(pf + "\\Docker\\Docker\\resources\\bin");
#else
    extra = {"/usr/bin", "/usr/local/bin", "/snap/bin"};
#endif
    return find_program("docker", extra);
}

bool valid_volume_name(std::string_view name) {
    return valid_docker_name(name);
}

bool valid_container_name(std::string_view name) {
    return valid_docker_name(name);
}

std::vector<std::string> volume_place(const std::string& volume) {
    return {kDockerFolder, volume + ".tar"};
}

std::string volume_snapshot_path(const std::string& volume) {
    return std::string(kDockerFolder) + "/" + volume + ".tar";
}

std::string volume_of_path(std::string_view snapshot_path) {
    std::vector<std::string> parts;
    std::string cur;
    for (const char c : snapshot_path) {
        if (c == '/' || c == '\\') {
            if (!cur.empty()) parts.push_back(std::move(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) parts.push_back(std::move(cur));
    if (parts.size() != 2 || parts[0] != kDockerFolder || !parts[1].ends_with(".tar")) return {};
    const std::string name = parts[1].substr(0, parts[1].size() - 4);
    return valid_volume_name(name) ? name : std::string();
}

std::string docker_problem(std::string_view output, int exit_code) {
    const std::string text = lower(output);
    if (text.find("cannot connect to the docker daemon") != std::string::npos ||
        text.find("error during connect") != std::string::npos ||
        text.find("is the docker daemon running") != std::string::npos ||
        text.find("docker desktop is not running") != std::string::npos ||
        (text.find("pipe") != std::string::npos && text.find("the system cannot find the file") != std::string::npos))
        return "Docker is not running (start Docker Desktop, or the Docker service)";
    if (text.find("permission denied") != std::string::npos && text.find("docker") != std::string::npos)
        return "this account may not use Docker (on Linux, it must be in the docker group)";
    const std::string line = first_line(output);
    return line.empty() ? "docker failed (exit code " + std::to_string(exit_code) + ")" : line;
}

DockerInfo docker_info() {
    DockerInfo info;
    info.program = docker_program();
    if (info.program.empty()) {
        info.problem = "Docker is not installed on this computer (the docker program was not found)";
        return info;
    }
    std::string output;
    int code = 0;
    try {
        code = run_docker(info.program, {"info", "--format", "{{.OSType}}"}, output, std::chrono::seconds(60));
    } catch (const std::exception& e) {
        info.problem = std::string("could not ask Docker: ") + e.what();
        return info;
    }
    const std::string os_type = lower(first_line(output));
    if (code != 0 || (os_type != "linux" && os_type != "windows")) {
        info.problem = docker_problem(output, code);
        return info;
    }
    info.os_type = os_type;
    info.available = true;
    return info;
}

std::vector<DockerVolume> parse_volume_list(std::string_view text) {
    std::vector<DockerVolume> out;
    for (const std::string& line : lines_of(text)) {
        const std::vector<std::string> cols = split(line, '\t');
        DockerVolume v;
        v.name = trim(cols[0]);
        if (cols.size() > 1) v.driver = trim(cols[1]);
        if (valid_volume_name(v.name)) out.push_back(std::move(v));
    }
    std::sort(out.begin(), out.end(), [](const DockerVolume& a, const DockerVolume& b) { return a.name < b.name; });
    return out;
}

std::map<std::string, std::vector<std::string>> parse_container_mounts(std::string_view text) {
    std::map<std::string, std::vector<std::string>> out;
    for (const std::string& line : lines_of(text)) {
        const std::size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        const std::string container = trim(line.substr(0, tab));
        if (container.empty()) continue;
        for (const std::string& mount : split(line.substr(tab + 1), ',')) {
            const std::string m = trim(mount);
            // A bind-mounted folder is a path, never a valid volume name.
            if (!valid_volume_name(m)) continue;
            std::vector<std::string>& users = out[m];
            if (std::find(users.begin(), users.end(), container) == users.end()) users.push_back(container);
        }
    }
    return out;
}

std::vector<std::string> parse_image_list(std::string_view text) {
    std::vector<std::string> out;
    for (const std::string& line : lines_of(text)) {
        const std::string image = trim(line);
        if (image.empty() || image.find("<none>") != std::string::npos) continue;
        out.push_back(image);
    }
    return out;
}

std::vector<DockerVolume> docker_volumes(const DockerInfo& info) {
    if (!info.available) throw Error(info.problem);
    std::string output;
    int code = run_docker(info.program, {"volume", "ls", "--format", "{{.Name}}\t{{.Driver}}"}, output);
    if (code != 0) throw Error("could not list the Docker volumes: " + docker_problem(output, code));
    std::vector<DockerVolume> volumes = parse_volume_list(output);
    output.clear();
    code = run_docker(info.program, {"ps", "--no-trunc", "--format", "{{.Names}}\t{{.Mounts}}"}, output);
    if (code != 0) throw Error("could not list the running Docker containers: " + docker_problem(output, code));
    const auto users = parse_container_mounts(output);
    for (DockerVolume& v : volumes) {
        const auto it = users.find(v.name);
        if (it != users.end()) v.running = it->second;
    }
    return volumes;
}

std::vector<std::string> docker_images(const DockerInfo& info) {
    if (!info.available) throw Error(info.problem);
    std::string output;
    const int code = run_docker(info.program, {"image", "ls", "--format", "{{.Repository}}:{{.Tag}}"}, output);
    if (code != 0) throw Error("could not list the Docker images: " + docker_problem(output, code));
    return parse_image_list(output);
}

std::string pick_helper_image(const std::vector<std::string>& images, const std::string& os_type,
                              const std::string& wanted) {
    if (!wanted.empty()) return wanted;
    const bool windows = os_type == "windows";
    const std::vector<std::string> known =
        windows ? std::vector<std::string>{"mcr.microsoft.com/windows/nanoserver", "mcr.microsoft.com/windows/servercore",
                                           "mcr.microsoft.com/windows/server"}
                : std::vector<std::string>{"busybox", "alpine", "debian", "ubuntu"};
    for (const std::string& want : known)
        for (const std::string& image : images) {
            // "repository:tag"; a registry with a port has a ':' before the last '/'.
            const std::size_t slash = image.rfind('/');
            const std::size_t colon = image.rfind(':');
            std::string repo = colon != std::string::npos && (slash == std::string::npos || colon > slash)
                                   ? image.substr(0, colon)
                                   : image;
            for (const char* prefix : {"docker.io/library/", "docker.io/", "library/"})
                if (repo.starts_with(prefix)) repo = repo.substr(std::string_view(prefix).size());
            if (lower(repo) == want) return image;
        }
    if (windows)
        throw Error("there is no image on this computer to run tar in for Docker's Windows containers: download one "
                    "once with \"docker pull mcr.microsoft.com/windows/nanoserver:ltsc2022\" (the version that "
                    "matches your Windows), or name an image in the job; Vaultik never downloads images itself");
    throw Error("there is no image on this computer to run tar in: download one once with \"docker pull busybox\", or "
                "name an image in the job; Vaultik never downloads images itself");
}

std::string volume_mount_point(const std::string& os_type) {
    return os_type == "windows" ? "C:\\vaultik-volume" : "/vaultik-volume";
}

namespace {

std::vector<std::string> helper_run(const std::string& docker, const std::string& volume, bool read_only,
                                    const std::string& image, const std::string& os_type,
                                    const std::string& container, bool input) {
    const std::string mount = volume_mount_point(os_type);
    std::vector<std::string> argv = {docker, "run", "--rm"};
    if (input) argv.push_back("-i");
    // Only an image already here, no network, and no log of the stream in Docker's own files.
    argv.insert(argv.end(), {"--pull", "never", "--network", "none", "--log-driver", "none", "--name", container,
                             "-v", volume + ":" + mount + (read_only ? ":ro" : ""), "--entrypoint", "tar"});
    // Linux: as root, so every file can be read and owners are kept when unpacking.
    if (os_type != "windows") argv.insert(argv.end(), {"--user", "0:0"});
    argv.push_back(image);
    return argv;
}

}  // namespace

std::vector<std::string> volume_export_command(const std::string& docker, const std::string& volume,
                                               const std::string& image, const std::string& os_type,
                                               const std::string& container) {
    std::vector<std::string> argv = helper_run(docker, volume, true, image, os_type, container, false);
    argv.insert(argv.end(), {"-c", "-f", "-", "-C", volume_mount_point(os_type), "."});
    return argv;
}

std::vector<std::string> volume_import_command(const std::string& docker, const std::string& volume,
                                               const std::string& image, const std::string& os_type,
                                               const std::string& container) {
    std::vector<std::string> argv = helper_run(docker, volume, false, image, os_type, container, true);
    argv.insert(argv.end(), {"-x", "-f", "-", "-C", volume_mount_point(os_type)});
    return argv;
}

std::string helper_container_name(const std::string& what) {
    std::uint8_t rnd[4];
    random_bytes(rnd);
    return "vaultik-" + what + "-" + to_hex(rnd);
}

std::string volume_in_use_warning(const DockerVolume& volume) {
    if (volume.running.empty()) return {};
    std::string names;
    for (const std::string& c : volume.running) names += (names.empty() ? "" : ", ") + c;
    return "the Docker volume " + volume.name + " is used by the running container" +
           (volume.running.size() == 1 ? " " : "s ") + names +
           "; it is backed up as it is at this moment, so a database in it may be saved half-written (add the "
           "database to the job's database dumps for a consistent copy)";
}

ExtraSource docker_volume_source(const DockerInfo& info, const std::string& volume, const std::string& image,
                                 std::function<bool()> cancelled, std::chrono::seconds stall) {
    ExtraSource x;
    x.place = volume_place(volume);
    x.open = [info, volume, image, cancelled = std::move(cancelled), stall]() -> std::unique_ptr<StreamSource> {
        if (!valid_volume_name(volume)) throw Error("invalid Docker volume name \"" + volume + "\"");
        const std::string container = helper_container_name("export");
        auto inner = program_stream(volume_export_command(info.program, volume, image, info.os_type, container), {},
                                    "the export of the Docker volume " + volume, cancelled, stall, 1024);
        return std::make_unique<VolumeExport>(std::move(inner), info.program, container);
    };
    return x;
}

void docker_import(Repository& repo, const Node& file, const std::string& new_name, const std::string& image,
                   const std::function<void(const VirtProgress&)>& progress, const std::function<bool()>& cancelled) {
    if (!valid_volume_name(new_name))
        throw Error("\"" + new_name +
                    "\" cannot be a Docker volume name (a letter or digit, then letters, digits, '_', '.' and '-')");
    if (file.kind != NodeKind::File) throw Error("that is not an exported Docker volume (a .tar file)");
    const DockerInfo info = docker_info();
    if (!info.available) throw Error(info.problem);
    const std::string& docker = info.program;

    // Never into an existing volume.
    std::string output;
    int code = run_docker(docker, {"volume", "inspect", "--format", "{{.Name}}", new_name}, output);
    if (code == 0)
        throw Error("a Docker volume called " + new_name +
                    " exists already; choose another name (an existing volume is never written to)");
    if (lower(output).find("no such volume") == std::string::npos)
        throw Error("could not ask Docker about the volume " + new_name + ": " + docker_problem(output, code));

    const std::string helper = pick_helper_image(docker_images(info), info.os_type, image);

    // A backup missing a piece fails before anything is created.
    for (const Id& id : file.content)
        if (!repo.index().contains(id))
            throw Error("the backup of this volume is incomplete (a piece of it is missing); nothing was imported");

    // Created with a mark of this import: "volume create" of a name that appeared meanwhile would
    // quietly return the existing volume, which the mark then tells apart.
    std::uint8_t rnd[8];
    random_bytes(rnd);
    const std::string mark = to_hex(rnd);
    output.clear();
    code = run_docker(docker, {"volume", "create", "--label", "vaultik.import=" + mark, new_name}, output);
    if (code != 0) throw Error("could not create the Docker volume " + new_name + ": " + docker_problem(output, code));
    output.clear();
    code = run_docker(docker, {"volume", "inspect", "--format", "{{index .Labels \"vaultik.import\"}}", new_name}, output);
    if (code != 0 || trim(output) != mark)
        throw Error("a Docker volume called " + new_name +
                    " appeared while importing; it was left as it is (choose another name)");

    const std::string container = helper_container_name("import");
    const auto drop_volume = [&] {
        remove_container(docker, container);
        try {
            std::string ignored;
            run_docker(docker, {"volume", "rm", new_name}, ignored);
        } catch (const std::exception&) {
        }
    };
    VirtProgress p;
    p.total = file.size;
    try {
        os::PipedProcess tar(volume_import_command(docker, new_name, helper, info.os_type, container),
                             os::PipedProcess::Direction::ToProgram);
        try {
            for (const Id& id : file.content) {
                if (cancelled && cancelled()) throw Error("cancelled");
                const Bytes chunk = repo.load_blob(BlobType::Data, id);
                tar.write(chunk);
                p.bytes += chunk.size();
                if (progress) progress(p);
            }
        } catch (const std::exception& e) {
            tar.stop();
            const std::string why = first_line(tar.messages());
            throw Error(std::string(e.what()) + (why.empty() ? "" : " (docker: " + why + ")"));
        }
        const int done = tar.wait();
        if (done != 0) {
            const std::string why = first_line(tar.messages());
            throw Error("unpacking into the Docker volume failed (exit code " + std::to_string(done) + ")" +
                        (why.empty() ? "" : ": " + why));
        }
    } catch (const std::exception& e) {
        // Only the volume this import created goes, never another.
        drop_volume();
        throw Error(std::string(e.what()) + "; the new volume " + new_name + " was removed again");
    }
}

}  // namespace vaultik
