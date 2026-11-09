#include "vaultik/repo/local_backend.hpp"

#include <algorithm>
#include <limits>

#include "vaultik/crypto/crypto.hpp"
#include "vaultik/platform/acl.hpp"
#include "vaultik/platform/os.hpp"
#include "vaultik/util/hex.hpp"

namespace vaultik {

namespace {

constexpr std::size_t kMaxCachedHandles = 16;
constexpr char kTmpMarker[] = ".tmp-";

}  // namespace

struct LocalBackend::OpenFile {
    os::File file;
};

LocalBackend::LocalBackend(const std::string& root, bool create) : root_(os::absolute_path(root)) {
    if (create) ensure_parent_dirs(os::join(root_, "x"));
    if (!os::is_directory(root_)) throw Error("repository folder does not exist: " + root_);
    // A job with administrator rights writes into a folder the user can change: a folder there
    // swapped for a link to a system folder would let it create and delete files there.
    elevated_ = os::process_elevated();
}

void LocalBackend::check_no_links(const std::string& name) const {
    if (!elevated_) return;
    constexpr std::uint32_t kReparsePoint = 0x400;  // FILE_ATTRIBUTE_REPARSE_POINT
    std::string path = root_;
    std::string rest = name;
    for (;;) {
        os::FileInfo info;
        os::ErrorCode err = 0;
        if (os::stat(path, info, err) && ((info.attributes & kReparsePoint) != 0 || info.type == os::EntryType::Symlink))
            throw Error("refusing to write through a link in the backup folder: " + path);
        const std::size_t slash = rest.find('/');
        if (slash == std::string::npos) return;  // the folders on the way are checked, not the file
        path = os::join(path, rest.substr(0, slash));
        rest = rest.substr(slash + 1);
    }
}

LocalBackend::~LocalBackend() = default;

std::string LocalBackend::location() const {
    return root_;
}

std::string LocalBackend::path_of(const std::string& name) const {
    std::string rel = name;
    if constexpr (os::kSeparator != '/') std::replace(rel.begin(), rel.end(), '/', os::kSeparator);
    return os::join(root_, rel);
}

void LocalBackend::ensure_parent_dirs(const std::string& file_path) {
    // Walk up to the first existing directory, then create the missing ones top-down.
    const std::string dir = os::parent_of(file_path);
    if (dir.empty()) return;  // never above a drive, share or "/"; those cannot be created
    if (os::exists(dir)) {
        if (!os::is_directory(dir)) throw Error("not a folder: " + dir);
        return;
    }
    ensure_parent_dirs(dir);
    const os::ErrorCode err = os::make_dir(dir);
    if (err != 0 && !os::is_exists(err)) os::throw_error("cannot create folder " + dir, err);
}

bool LocalBackend::exists(const std::string& name) {
    // Only "not found" means missing: an unreachable share or a refused access must not look
    // like an empty repository (a second copy would then be created over the old one).
    os::FileInfo info;
    os::ErrorCode err = 0;
    if (!os::stat(path_of(name), info, err)) {
        if (os::is_not_found(err)) return false;
        os::throw_error("cannot check repository file " + name, err);
    }
    return info.type != os::EntryType::Dir;
}

void LocalBackend::put(const std::string& name, ByteSpan data) {
    const std::string final_path = path_of(name);
    check_no_links(name);
    ensure_parent_dirs(final_path);
    check_no_links(name);  // also the folders just created

    std::uint8_t rnd[8];
    random_bytes(rnd);
    const std::string tmp_path = final_path + kTmpMarker + to_hex(rnd);

    {
        os::ErrorCode err = 0;
        os::File f = os::File::create(tmp_path, false, err, 0600);
        if (!f.valid()) os::throw_error("cannot create " + tmp_path, err);
        try {
            f.write_all(data);
            f.sync();
        } catch (...) {
            f.close();
            os::remove_file(tmp_path);
            throw;
        }
    }

    {
        std::lock_guard lock(mu_);
        drop_cached(name);
    }
    try {
        os::rename_replace(tmp_path, final_path);
    } catch (...) {
        os::remove_file(tmp_path);
        throw;
    }
}

LocalBackend::OpenFile& LocalBackend::open_cached(const std::string& name) {
    for (auto it = cache_.begin(); it != cache_.end(); ++it) {
        if (it->first == name) {
            cache_.splice(cache_.begin(), cache_, it);
            return *cache_.front().second;
        }
    }
    auto file = std::make_unique<OpenFile>();
    os::ErrorCode err = 0;
    file->file = os::File::open_random(path_of(name), err);
    if (!file->file.valid()) os::throw_error("cannot open repository file " + name, err);
    cache_.emplace_front(name, std::move(file));
    if (cache_.size() > kMaxCachedHandles) cache_.pop_back();
    return *cache_.front().second;
}

void LocalBackend::drop_cached(const std::string& name) {
    cache_.remove_if([&](const auto& entry) { return entry.first == name; });
}

Bytes LocalBackend::get(const std::string& name) {
    const std::uint64_t n = size(name);
    if (n > std::numeric_limits<std::size_t>::max()) throw Error("repository file too large: " + name);
    return get_range(name, 0, static_cast<std::size_t>(n));
}

Bytes LocalBackend::get_range(const std::string& name, std::uint64_t offset, std::size_t length) {
    Bytes out(length);
    if (length == 0) return out;
    // Only packs (written once, never replaced) keep an open handle. Snapshots, index, lock and
    // key files are replaced in place, and an open handle on them would make another process's
    // replace fail on file systems without POSIX renames (FAT, exFAT).
    if (!name.starts_with("data/")) {
        os::ErrorCode err = 0;
        os::File file = os::File::open_random(path_of(name), err);
        if (!file.valid()) os::throw_error("cannot open repository file " + name, err);
        try {
            file.read_at(offset, out.data(), length);
        } catch (const os::OsError&) {
            throw;
        } catch (const Error& e) {
            throw Error(std::string("repository file ") + name + ": " + e.what());
        }
        return out;
    }
    std::lock_guard lock(mu_);
    OpenFile& f = open_cached(name);
    try {
        f.file.read_at(offset, out.data(), length);
    } catch (const os::OsError&) {
        throw;
    } catch (const Error& e) {
        throw Error(std::string("repository file ") + name + ": " + e.what());
    }
    return out;
}

std::uint64_t LocalBackend::size(const std::string& name) {
    os::FileInfo info;
    os::ErrorCode err = 0;
    if (!os::stat(path_of(name), info, err)) os::throw_error("cannot stat repository file " + name, err);
    return info.size;
}

void LocalBackend::list_into(const std::string& dir_path, const std::string& prefix, std::vector<std::string>& out) {
    os::DirReader reader(dir_path);
    if (!reader.ok()) {
        if (os::is_not_found(reader.error())) return;
        os::throw_error("cannot list " + dir_path, reader.error());
    }
    std::string name;
    os::FileInfo info;
    while (reader.next(name, info)) {
        if (name.find(kTmpMarker) != std::string::npos) continue;
        if (info.type == os::EntryType::Dir) {
            list_into(os::join(dir_path, name), prefix + name + "/", out);
        } else if (info.type == os::EntryType::File) {
            out.push_back(prefix + name);
        }
    }
    if (!reader.ok()) os::throw_error("cannot list " + dir_path, reader.error());
}

std::vector<std::string> LocalBackend::list(const std::string& dir) {
    std::vector<std::string> out;
    list_into(path_of(dir), dir.empty() ? std::string() : dir + "/", out);
    std::sort(out.begin(), out.end());
    return out;
}

void LocalBackend::remove(const std::string& name) {
    {
        std::lock_guard lock(mu_);
        drop_cached(name);
    }
    check_no_links(name);
    const os::ErrorCode err = os::remove_file(path_of(name));
    if (err != 0 && !os::is_not_found(err)) os::throw_error("cannot delete repository file " + name, err);
}

}  // namespace vaultik
