#include "vaultik/repo/sftp_backend.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <libssh2.h>
#include <libssh2_sftp.h>

#include <algorithm>
#include <functional>

#include "vaultik/crypto/crypto.hpp"
#include "vaultik/util/base64.hpp"
#include "vaultik/util/hex.hpp"

namespace vaultik {

namespace {

// libssh2 and Winsock are set up once per process.
void init_once() {
    static const bool done = [] {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) throw Error("cannot start Windows sockets");
        if (libssh2_init(0) != 0) throw Error("cannot start libssh2");
        return true;
    }();
    (void)done;
}

// A failure that a new connection may cure (the link broke, the server went away for a moment).
class LinkError : public Error {
public:
    using Error::Error;
};

std::string openssh_fingerprint(const unsigned char* sha256) {
    std::string b64 = base64_encode(ByteSpan(sha256, 32));
    while (!b64.empty() && b64.back() == '=') b64.pop_back();
    return "SHA256:" + b64;
}

}  // namespace

// parse_sftp_location is in sftp_location.cpp, built on every platform (the GUI checks the
// location typed in with it).

struct SftpBackend::Connection {
    SOCKET sock = INVALID_SOCKET;
    LIBSSH2_SESSION* session = nullptr;
    LIBSSH2_SFTP* sftp = nullptr;

    ~Connection() {
        if (sftp != nullptr) libssh2_sftp_shutdown(sftp);
        if (session != nullptr) {
            libssh2_session_disconnect(session, "Vaultik done");
            libssh2_session_free(session);
        }
        if (sock != INVALID_SOCKET) closesocket(sock);
    }

    std::string last_error() const {
        char* msg = nullptr;
        int len = 0;
        if (session == nullptr) return "no session";
        libssh2_session_last_error(session, &msg, &len, 0);
        return msg != nullptr ? std::string(msg, static_cast<std::size_t>(len)) : std::string("unknown error");
    }

    unsigned long sftp_error() const { return sftp != nullptr ? libssh2_sftp_last_error(sftp) : 0; }

    // Throws LinkError for a broken connection, Error otherwise.
    [[noreturn]] void fail(const std::string& what, int rc) const {
        const std::string text = what + ": " + last_error();
        if (rc == LIBSSH2_ERROR_SOCKET_SEND || rc == LIBSSH2_ERROR_SOCKET_RECV || rc == LIBSSH2_ERROR_SOCKET_DISCONNECT ||
            rc == LIBSSH2_ERROR_TIMEOUT || rc == LIBSSH2_ERROR_SOCKET_TIMEOUT)
            throw LinkError(text);
        throw Error(text);
    }
};

SftpBackend::SftpBackend(const std::string& location, CloudCredentials credentials)
    : location_(location), where_(parse_sftp_location(location)) {
    init_once();
    key_file_ = credentials.key_id;
    secret_ = credentials.secret;
    host_key_ = credentials.region;
    secure_wipe(credentials.secret.data(), credentials.secret.size());
}

SftpBackend::~SftpBackend() {
    disconnect();
    secure_wipe(secret_.data(), secret_.size());
}

std::string SftpBackend::location() const {
    return location_;
}

void SftpBackend::disconnect() {
    conn_.reset();
}

SftpBackend::Connection& SftpBackend::connection() {
    if (conn_) return *conn_;
    auto c = std::make_unique<Connection>();

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* found = nullptr;
    if (getaddrinfo(where_.host.c_str(), std::to_string(where_.port).c_str(), &hints, &found) != 0 || found == nullptr)
        throw LinkError(location_ + ": cannot find the server " + where_.host);
    for (addrinfo* a = found; a != nullptr && c->sock == INVALID_SOCKET; a = a->ai_next) {
        SOCKET s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        DWORD timeout = 60000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof timeout);
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof timeout);
        if (::connect(s, a->ai_addr, static_cast<int>(a->ai_addrlen)) == 0) c->sock = s;
        else closesocket(s);
    }
    freeaddrinfo(found);
    if (c->sock == INVALID_SOCKET)
        throw LinkError(location_ + ": cannot connect to " + where_.host + " port " + std::to_string(where_.port));

    c->session = libssh2_session_init();
    if (c->session == nullptr) throw Error("cannot start an SSH session");
    libssh2_session_set_blocking(c->session, 1);
    libssh2_session_set_timeout(c->session, 120000);
    if (int rc = libssh2_session_handshake(c->session, c->sock); rc != 0) c->fail(location_ + ": SSH handshake", rc);

    // The server's identity, before any password or key is sent.
    const char* hash = libssh2_hostkey_hash(c->session, LIBSSH2_HOSTKEY_HASH_SHA256);
    if (hash == nullptr) throw Error(location_ + ": the server's key cannot be checked");
    const std::string fingerprint = openssh_fingerprint(reinterpret_cast<const unsigned char*>(hash));
    if (host_key_.empty())
        throw SftpHostKeyError(location_ + ": the server's key " + fingerprint + " is not trusted yet", fingerprint,
                               false);
    if (fingerprint != host_key_)
        throw SftpHostKeyError(location_ + ": the server's key changed (it shows " + fingerprint + ", trusted was " +
                                   host_key_ + "); this can be another computer pretending to be the server",
                               fingerprint, true);

    int rc = 0;
    if (!key_file_.empty()) {
        rc = libssh2_userauth_publickey_fromfile_ex(c->session, where_.user.c_str(),
                                                    static_cast<unsigned>(where_.user.size()), nullptr,
                                                    key_file_.c_str(), secret_.c_str());
    } else {
        rc = libssh2_userauth_password_ex(c->session, where_.user.c_str(), static_cast<unsigned>(where_.user.size()),
                                          secret_.c_str(), static_cast<unsigned>(secret_.size()), nullptr);
    }
    if (rc != 0) {
        if (rc == LIBSSH2_ERROR_AUTHENTICATION_FAILED || rc == LIBSSH2_ERROR_PUBLICKEY_UNVERIFIED)
            throw Error(location_ + ": the server refused the " + (key_file_.empty() ? "password" : "key"));
        c->fail(location_ + ": signing in", rc);
    }
    c->sftp = libssh2_sftp_init(c->session);
    if (c->sftp == nullptr) c->fail(location_ + ": starting SFTP", libssh2_session_last_errno(c->session));
    conn_ = std::move(c);
    return *conn_;
}

template <class Fn>
auto SftpBackend::with_retry(const char* what, const std::string& name, Fn&& fn) {
    std::lock_guard lock(mu_);
    for (int attempt = 0;; ++attempt) {
        try {
            return fn(connection());
        } catch (const LinkError&) {
            disconnect();
            if (attempt >= 2) throw;
            Sleep(2000u << attempt);
        }
        (void)what;
        (void)name;
    }
}

std::string SftpBackend::path_of(const std::string& name) const {
    return where_.folder == "/" ? "/" + name : where_.folder + "/" + name;
}

void SftpBackend::make_dirs(Connection& c, const std::string& name) {
    // Every folder from the top down to the file's own (they may be missing: a new repository).
    std::vector<std::string> dirs;
    for (std::string p = os_parent(path_of(name)); p.size() > 1; p = os_parent(p)) dirs.push_back(p);
    std::reverse(dirs.begin(), dirs.end());
    for (const std::string& d : dirs) {
        if (dirs_.contains(d)) continue;
        LIBSSH2_SFTP_ATTRIBUTES attrs{};
        if (libssh2_sftp_stat_ex(c.sftp, d.c_str(), static_cast<unsigned>(d.size()), LIBSSH2_SFTP_STAT, &attrs) != 0) {
            const int rc = libssh2_sftp_mkdir_ex(c.sftp, d.c_str(), static_cast<unsigned>(d.size()), 0700);
            if (rc != 0 && libssh2_sftp_stat_ex(c.sftp, d.c_str(), static_cast<unsigned>(d.size()), LIBSSH2_SFTP_STAT,
                                                &attrs) != 0)
                c.fail(location_ + ": creating the folder " + d, rc);
        }
        dirs_.insert(d);
    }
}

std::string SftpBackend::os_parent(const std::string& path) {
    const std::size_t slash = path.rfind('/');
    if (slash == std::string::npos || slash == 0) return "/";
    return path.substr(0, slash);
}

bool SftpBackend::exists(const std::string& name) {
    return with_retry("checking", name, [&](Connection& c) {
        // A replace cut short on a server without overwriting renames leaves only "<name>.old".
        for (const std::string& p : {path_of(name), path_of(name) + ".old"}) {
            LIBSSH2_SFTP_ATTRIBUTES attrs{};
            const int rc = libssh2_sftp_stat_ex(c.sftp, p.c_str(), static_cast<unsigned>(p.size()), LIBSSH2_SFTP_STAT, &attrs);
            if (rc == 0) return !LIBSSH2_SFTP_S_ISDIR(attrs.permissions);
            if (c.sftp_error() != LIBSSH2_FX_NO_SUCH_FILE && c.sftp_error() != LIBSSH2_FX_NO_SUCH_PATH)
                c.fail(location_ + ": checking " + name, rc);
        }
        return false;
    });
}

void SftpBackend::put(const std::string& name, ByteSpan data) {
    with_retry("uploading", name, [&](Connection& c) {
        make_dirs(c, name);
        // Written to a temporary name and renamed into place, so nobody reads half a file.
        std::uint8_t rnd[8];
        random_bytes(rnd);
        const std::string final_path = path_of(name);
        const std::string tmp = final_path + ".tmp-" + to_hex(ByteSpan(rnd, sizeof rnd));
        LIBSSH2_SFTP_HANDLE* h =
            libssh2_sftp_open_ex(c.sftp, tmp.c_str(), static_cast<unsigned>(tmp.size()),
                                 LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_EXCL, 0600, LIBSSH2_SFTP_OPENFILE);
        if (h == nullptr) c.fail(location_ + ": uploading " + name, libssh2_session_last_errno(c.session));
        std::size_t done = 0;
        while (done < data.size()) {
            const ssize_t n = libssh2_sftp_write(h, reinterpret_cast<const char*>(data.data() + done),
                                                  std::min<std::size_t>(data.size() - done, 1u << 20));
            if (n < 0) {
                libssh2_sftp_close(h);
                libssh2_sftp_unlink_ex(c.sftp, tmp.c_str(), static_cast<unsigned>(tmp.size()));
                c.fail(location_ + ": uploading " + name, static_cast<int>(n));
            }
            done += static_cast<std::size_t>(n);
        }
        libssh2_sftp_fsync(h);
        libssh2_sftp_close(h);
        // Replace an older file (snapshots and keys are replaced in place); servers without the
        // POSIX rename extension cannot overwrite, so the old one is removed first there.
        int rc = libssh2_sftp_rename_ex(c.sftp, tmp.c_str(), static_cast<unsigned>(tmp.size()), final_path.c_str(),
                                        static_cast<unsigned>(final_path.size()),
                                        LIBSSH2_SFTP_RENAME_OVERWRITE | LIBSSH2_SFTP_RENAME_ATOMIC |
                                            LIBSSH2_SFTP_RENAME_NATIVE);
        if (rc != 0) {
            // Without overwriting renames: move the old file aside as "<name>.old", put the new one
            // in place, and only then remove the old one; if that fails, the old one goes back.
            // Readers fall back to "<name>.old" while the name itself is missing, so a replace cut
            // short in between loses nothing, and the next one finishes it.
            const std::string old = final_path + ".old";
            LIBSSH2_SFTP_ATTRIBUTES attrs{};
            const bool final_there = libssh2_sftp_stat_ex(c.sftp, final_path.c_str(),
                                                          static_cast<unsigned>(final_path.size()), LIBSSH2_SFTP_STAT,
                                                          &attrs) == 0;
            // Left from an earlier replace: with the name in place it is stale, without it it is the
            // only copy and goes back first.
            if (final_there) {
                libssh2_sftp_unlink_ex(c.sftp, old.c_str(), static_cast<unsigned>(old.size()));
            } else {
                libssh2_sftp_rename_ex(c.sftp, old.c_str(), static_cast<unsigned>(old.size()), final_path.c_str(),
                                       static_cast<unsigned>(final_path.size()), LIBSSH2_SFTP_RENAME_NATIVE);
            }
            const bool had_old = libssh2_sftp_rename_ex(c.sftp, final_path.c_str(), static_cast<unsigned>(final_path.size()),
                                                        old.c_str(), static_cast<unsigned>(old.size()),
                                                        LIBSSH2_SFTP_RENAME_NATIVE) == 0;
            rc = libssh2_sftp_rename_ex(c.sftp, tmp.c_str(), static_cast<unsigned>(tmp.size()), final_path.c_str(),
                                        static_cast<unsigned>(final_path.size()), LIBSSH2_SFTP_RENAME_NATIVE);
            if (rc == 0) {
                if (had_old) libssh2_sftp_unlink_ex(c.sftp, old.c_str(), static_cast<unsigned>(old.size()));
            } else if (had_old) {
                libssh2_sftp_rename_ex(c.sftp, old.c_str(), static_cast<unsigned>(old.size()), final_path.c_str(),
                                       static_cast<unsigned>(final_path.size()), LIBSSH2_SFTP_RENAME_NATIVE);
            }
        }
        if (rc != 0) {
            libssh2_sftp_unlink_ex(c.sftp, tmp.c_str(), static_cast<unsigned>(tmp.size()));
            c.fail(location_ + ": uploading " + name, rc);
        }
        return 0;
    });
}

Bytes SftpBackend::get_range(const std::string& name, std::uint64_t offset, std::size_t length) {
    if (length == 0) return {};
    return with_retry("downloading", name, [&](Connection& c) {
        LIBSSH2_SFTP_HANDLE* h = nullptr;
        for (const std::string& p : {path_of(name), path_of(name) + ".old"}) {  // (see exists)
            h = libssh2_sftp_open_ex(c.sftp, p.c_str(), static_cast<unsigned>(p.size()), LIBSSH2_FXF_READ, 0,
                                     LIBSSH2_SFTP_OPENFILE);
            if (h != nullptr) break;
            if (c.sftp_error() != LIBSSH2_FX_NO_SUCH_FILE)
                c.fail(location_ + ": downloading " + name, libssh2_session_last_errno(c.session));
        }
        if (h == nullptr) throw Error(location_ + ": repository file " + name + " is missing");
        libssh2_sftp_seek64(h, offset);
        Bytes out(length);
        std::size_t done = 0;
        while (done < length) {
            const ssize_t n = libssh2_sftp_read(h, reinterpret_cast<char*>(out.data() + done), length - done);
            if (n < 0) {
                libssh2_sftp_close(h);
                c.fail(location_ + ": downloading " + name, static_cast<int>(n));
            }
            if (n == 0) break;
            done += static_cast<std::size_t>(n);
        }
        libssh2_sftp_close(h);
        if (done != length) throw Error(location_ + ": repository file " + name + " is shorter than expected");
        return out;
    });
}

Bytes SftpBackend::get(const std::string& name) {
    return get_range(name, 0, static_cast<std::size_t>(size(name)));
}

std::uint64_t SftpBackend::size(const std::string& name) {
    return with_retry("checking", name, [&](Connection& c) -> std::uint64_t {
        for (const std::string& p : {path_of(name), path_of(name) + ".old"}) {  // (see exists)
            LIBSSH2_SFTP_ATTRIBUTES attrs{};
            const int rc = libssh2_sftp_stat_ex(c.sftp, p.c_str(), static_cast<unsigned>(p.size()), LIBSSH2_SFTP_STAT, &attrs);
            if (rc == 0) return attrs.filesize;
            if (c.sftp_error() != LIBSSH2_FX_NO_SUCH_FILE) c.fail(location_ + ": checking " + name, rc);
        }
        throw Error(location_ + ": repository file " + name + " is missing");
    });
}

std::vector<std::string> SftpBackend::list(const std::string& dir) {
    return with_retry("listing", dir, [&](Connection& c) {
        std::vector<std::string> out;
        const std::function<void(const std::string&)> walk = [&](const std::string& rel) {
            const std::string p = rel.empty() ? where_.folder : path_of(rel);
            LIBSSH2_SFTP_HANDLE* h = libssh2_sftp_open_ex(c.sftp, p.c_str(), static_cast<unsigned>(p.size()), 0, 0,
                                                          LIBSSH2_SFTP_OPENDIR);
            if (h == nullptr) {
                const unsigned long e = c.sftp_error();
                if (e == LIBSSH2_FX_NO_SUCH_FILE || e == LIBSSH2_FX_NO_SUCH_PATH) return;
                c.fail(location_ + ": listing " + (rel.empty() ? std::string("files") : rel),
                       libssh2_session_last_errno(c.session));
            }
            char name[1024];
            LIBSSH2_SFTP_ATTRIBUTES attrs{};
            std::vector<std::string> folders;
            for (;;) {
                const int n = libssh2_sftp_readdir_ex(h, name, sizeof name, nullptr, 0, &attrs);
                if (n <= 0) break;
                const std::string entry(name, static_cast<std::size_t>(n));
                if (entry == "." || entry == "..") continue;
                const std::string full = rel.empty() ? entry : rel + "/" + entry;
                if (LIBSSH2_SFTP_S_ISDIR(attrs.permissions)) folders.push_back(full);
                else if (entry.ends_with(".old")) out.push_back(full.substr(0, full.size() - 4));  // (see exists)
                else if (entry.find(".tmp-") == std::string::npos && entry.find(".old-") == std::string::npos)
                    out.push_back(full);
            }
            libssh2_sftp_close(h);
            for (const std::string& f : folders) walk(f);
        };
        walk(dir);
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    });
}

void SftpBackend::remove(const std::string& name) {
    with_retry("deleting", name, [&](Connection& c) {
        // The old copy first: were it left, readers would fall back to it once the name is gone.
        for (const std::string& p : {path_of(name) + ".old", path_of(name)}) {  // (see exists)
            const int rc = libssh2_sftp_unlink_ex(c.sftp, p.c_str(), static_cast<unsigned>(p.size()));
            if (rc != 0 && c.sftp_error() != LIBSSH2_FX_NO_SUCH_FILE) c.fail(location_ + ": deleting " + name, rc);
        }
        return 0;
    });
}

}  // namespace vaultik
