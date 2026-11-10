#pragma once

// Repository on an SSH server over SFTP ("sftp:user@host[:port]/folder"): a NAS, a Linux server,
// a rented storage box. Windows only for now (libssh2 with the system's crypto).
//
// Sign-in: the credentials' secret is the password, or, when key_id names a private key file
// (OpenSSH or PEM format), that key's passphrase (may be empty). The server must prove it is the
// one first trusted: the credentials' `region` holds its host key fingerprint, "SHA256:<base64>"
// as OpenSSH shows it. Without one, or when the server shows another key, the connection is
// refused with SftpHostKeyError carrying the key the server showed, so the user can compare and
// trust it.

#include <memory>
#include <mutex>
#include <set>
#include <string>

#include "vaultik/repo/backend.hpp"
#include "vaultik/repo/cloud.hpp"

namespace vaultik {

class SftpHostKeyError : public Error {
public:
    SftpHostKeyError(const std::string& message, std::string fingerprint, bool changed)
        : Error(message), fingerprint_(std::move(fingerprint)), changed_(changed) {}
    const std::string& fingerprint() const { return fingerprint_; }
    bool changed() const { return changed_; }  // a key was trusted before, and this is another one

private:
    std::string fingerprint_;
    bool changed_;
};

struct SftpLocation {
    std::string user;
    std::string host;
    int port = 22;
    std::string folder;  // absolute, "/volume1/backups/pc"
};
// Throws Error for anything that is not "sftp:user@host[:port]/folder".
SftpLocation parse_sftp_location(const std::string& location);

class SftpBackend final : public Backend {
public:
    SftpBackend(const std::string& location, CloudCredentials credentials);
    ~SftpBackend() override;

    std::string location() const override;
    bool exists(const std::string& name) override;
    void put(const std::string& name, ByteSpan data) override;
    Bytes get(const std::string& name) override;
    Bytes get_range(const std::string& name, std::uint64_t offset, std::size_t length) override;
    std::uint64_t size(const std::string& name) override;
    std::vector<std::string> list(const std::string& dir) override;
    void remove(const std::string& name) override;

private:
    struct Connection;
    Connection& connection();  // connects (again) when needed; call with mu_ held
    void disconnect();
    template <class Fn>
    auto with_retry(const char* what, const std::string& name, Fn&& fn);  // one reconnect on a broken link
    std::string path_of(const std::string& name) const;
    static std::string os_parent(const std::string& path);  // "/a/b/c" -> "/a/b"
    void make_dirs(Connection& c, const std::string& name);

    std::string location_;
    SftpLocation where_;
    std::string key_file_;
    std::string secret_;
    std::string host_key_;
    std::mutex mu_;
    std::unique_ptr<Connection> conn_;
    std::set<std::string> dirs_;  // known to exist
};

}  // namespace vaultik
