// Portable helpers of the OS layer, built on File and the per-platform primitives.

#include <limits>

#include "vaultik/crypto/crypto.hpp"
#include "vaultik/platform/os.hpp"
#include "vaultik/util/hex.hpp"

namespace vaultik::os {

void make_dirs(const std::string& path) {
    if (path.empty() || is_directory(path)) return;
    const std::string parent = parent_of(path);
    if (!parent.empty()) make_dirs(parent);
    const ErrorCode err = make_dir(path);
    if (err != 0 && !(is_exists(err) && is_directory(path))) throw_error("cannot create folder " + path, err);
}

Bytes read_file(const std::string& path, std::size_t max_size) {
    ErrorCode err = 0;
    File f = File::open_read(path, err);
    if (!f.valid()) throw_error("cannot read " + path, err);
    Bytes data;
    std::uint8_t buf[64 * 1024];
    for (;;) {
        const std::size_t n = f.read(buf, sizeof buf);
        if (n == 0) break;
        if (data.size() + n > max_size) throw Error("file is too large: " + path);
        data.insert(data.end(), buf, buf + n);
    }
    return data;
}

void write_file_atomic(const std::string& path, ByteSpan data, std::uint32_t mode) {
    const std::string parent = parent_of(path);
    if (!parent.empty()) make_dirs(parent);
    std::uint8_t rnd[6];
    random_bytes(rnd);
    const std::string tmp = path + ".tmp-" + to_hex(rnd);
    {
        ErrorCode err = 0;
        File f = File::create(tmp, false, err, mode);
        if (!f.valid()) throw_error("cannot create " + tmp, err);
        try {
            f.write_all(data);
            f.sync();
        } catch (...) {
            f.close();
            remove_file(tmp);
            throw;
        }
    }
    try {
        rename_replace(tmp, path);
    } catch (...) {
        remove_file(tmp);
        throw;
    }
}

void append_file(const std::string& path, ByteSpan data, std::uint32_t mode) {
    const std::string parent = parent_of(path);
    if (!parent.empty()) make_dirs(parent);
    ErrorCode err = 0;
    File f = File::open_append(path, err, mode);
    if (!f.valid()) throw_error("cannot open " + path, err);
    f.write_all(data);
}

}  // namespace vaultik::os
