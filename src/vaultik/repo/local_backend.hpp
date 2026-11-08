#pragma once

#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "vaultik/repo/backend.hpp"

namespace vaultik {

// Repository on a local path: an internal disk, a USB drive or a network share (an SMB path such
// as \\server\share\... on Windows, a mounted share on Linux).
class LocalBackend final : public Backend {
public:
    // `root` (UTF-8) may be relative; it is resolved to an absolute path. With `create`, the root
    // directory is created if missing.
    explicit LocalBackend(const std::string& root, bool create = false);
    ~LocalBackend() override;

    std::string location() const override;
    bool exists(const std::string& name) override;
    void put(const std::string& name, ByteSpan data) override;
    Bytes get(const std::string& name) override;
    Bytes get_range(const std::string& name, std::uint64_t offset, std::size_t length) override;
    std::uint64_t size(const std::string& name) override;
    std::vector<std::string> list(const std::string& dir) override;
    void remove(const std::string& name) override;

private:
    struct OpenFile;

    std::string path_of(const std::string& name) const;
    void ensure_parent_dirs(const std::string& file_path);
    OpenFile& open_cached(const std::string& name);
    void drop_cached(const std::string& name);
    void list_into(const std::string& dir_path, const std::string& prefix, std::vector<std::string>& out);

    std::string root_;  // absolute
    std::mutex mu_;
    std::list<std::pair<std::string, std::unique_ptr<OpenFile>>> cache_;  // most recent first
};

}  // namespace vaultik
