#include "vaultik/repo/lock.hpp"

#include <algorithm>
#include <cstring>
#include <random>

#include "vaultik/platform/os.hpp"
#include "vaultik/util/hex.hpp"

namespace vaultik {

namespace {

constexpr char kLockDir[] = "locks";
constexpr char kLockAadPrefix[] = "VKLOCK01";
constexpr std::uint32_t kLockVersion = 1;

// "locks/<64 hex>" -> AAD, or nothing when the name does not have that shape.
std::optional<Bytes> lock_aad(const std::string& name) {
    const std::string prefix = std::string(kLockDir) + "/";
    if (!name.starts_with(prefix) || name.size() != prefix.size() + 64) return std::nullopt;
    Id id;
    try {
        id = id_from_hex(std::string_view(name).substr(prefix.size()));
    } catch (const Error&) {
        return std::nullopt;
    }
    Bytes aad(kLockAadPrefix, kLockAadPrefix + std::strlen(kLockAadPrefix));
    aad.insert(aad.end(), id.begin(), id.end());
    return aad;
}

// Same envelope as every sealed object: compression byte (always none) and length first.
Bytes seal_lock(const AesGcm& aead, const Bytes& aad, const LockInfo& info) {
    const Bytes plain = serialize_lock(info);
    ByteWriter w;
    w.u8(0);
    w.varint(plain.size());
    w.raw(plain);
    return aead.seal(w.take(), aad);
}

LockInfo open_lock(const AesGcm& aead, const Bytes& aad, ByteSpan sealed) {
    const Bytes payload = aead.open(sealed, aad);
    ByteReader r(payload);
    if (r.u8() != 0) throw Error("malformed lock file");
    const std::uint64_t n = r.varint();
    if (n != r.remaining()) throw Error("malformed lock file");
    return parse_lock(r.raw(r.remaining()));
}

// Why `exclusive` cannot be taken now, or empty. Removes stale locks it comes across.
std::string find_conflict(Repository& repo, bool exclusive, const std::string& own) {
    const std::uint64_t now = os::now();
    for (const LockEntry& e : list_locks(repo)) {
        if (e.name == own) continue;
        if (!e.info) {
            return "an unreadable lock file " + e.name +
                   " (if no other Vaultik run uses this repository, remove it with vaultik unlock --all)";
        }
        if (is_stale(*e.info, now)) {
            try {
                repo.backend().remove(e.name);
            } catch (const Error&) {
            }
            continue;
        }
        if (exclusive || e.info->exclusive) return describe(*e.info);
    }
    return {};
}

}  // namespace

Bytes serialize_lock(const LockInfo& info) {
    ByteWriter w;
    w.u32(kLockVersion);
    w.u8(info.exclusive ? 1 : 0);
    w.u64(info.time);
    w.u32(info.pid);
    w.str(info.hostname);
    w.str(info.username);
    w.str(info.command);
    return w.take();
}

LockInfo parse_lock(ByteSpan data) {
    ByteReader r(data);
    if (r.u32() != kLockVersion) throw Error("unsupported lock file version");
    LockInfo info;
    info.exclusive = r.u8() != 0;
    info.time = r.u64();
    info.pid = r.u32();
    info.hostname = r.str();
    info.username = r.str();
    info.command = r.str();
    r.expect_done();
    return info;
}

bool is_stale(const LockInfo& info, std::uint64_t now) {
    if (now > info.time && now - info.time > kLockStaleTicks) return true;
    return info.hostname == os::hostname() && !os::process_alive(info.pid);
}

std::string describe(const LockInfo& info) {
    return std::string(info.exclusive ? "exclusive" : "shared") + " lock by " + info.command + " (" + info.username +
           "@" + info.hostname + ", process " + std::to_string(info.pid) + ", refreshed " +
           os::format_time_local(info.time) + ")";
}

std::vector<LockEntry> list_locks(Repository& repo) {
    std::vector<LockEntry> out;
    for (const std::string& name : repo.backend().list(kLockDir)) {
        LockEntry e;
        e.name = name;
        const std::optional<Bytes> aad = lock_aad(name);
        if (aad) {
            try {
                e.info = open_lock(repo.lock_aead(), *aad, repo.backend().get(name));
            } catch (const Error&) {
                if (!repo.backend().exists(name)) continue;  // released while we looked
            }
        }
        out.push_back(std::move(e));
    }
    return out;
}

void write_lock(Repository& repo, const std::string& name, const LockInfo& info) {
    const std::optional<Bytes> aad = lock_aad(name);
    if (!aad) throw Error("not a lock file name: " + name);
    repo.backend().put(name, seal_lock(repo.lock_aead(), *aad, info));
}

std::size_t remove_locks(Repository& repo, bool all) {
    const std::uint64_t now = os::now();
    std::size_t removed = 0;
    for (const LockEntry& e : list_locks(repo)) {
        if (!all && (!e.info || !is_stale(*e.info, now))) continue;
        repo.backend().remove(e.name);
        ++removed;
    }
    return removed;
}

RepoLock::RepoLock(Repository& repo, Mode mode, std::string command, std::chrono::seconds wait) : repo_(repo) {
    info_.exclusive = mode == Mode::Exclusive;
    info_.pid = os::process_id();
    info_.hostname = os::hostname();
    info_.username = os::username();
    info_.command = std::move(command);
    name_ = std::string(kLockDir) + "/" + id_hex(random_id());

    const auto deadline = std::chrono::steady_clock::now() + wait;
    std::minstd_rand jitter(info_.pid);
    for (;;) {
        // Write first, then look: of two runs starting together, at least one sees the other.
        std::string conflict = find_conflict(repo_, info_.exclusive, name_);
        if (conflict.empty()) {
            info_.time = os::now();
            write_lock(repo_, name_, info_);
            conflict = find_conflict(repo_, info_.exclusive, name_);
            if (conflict.empty()) break;
            repo_.backend().remove(name_);
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            throw LockedError("the repository is in use by another Vaultik run: " + conflict +
                              ". If that run is gone, remove the lock with vaultik unlock --all");
        }
        const auto pause = std::chrono::milliseconds(2000 + jitter() % 3000);
        std::this_thread::sleep_for(std::min<std::chrono::steady_clock::duration>(pause, deadline - now));
    }
    thread_ = std::thread([this] { refresh_loop(); });
}

RepoLock::~RepoLock() {
    try {
        release();
    } catch (...) {
    }
}

void RepoLock::release() {
    if (!thread_.joinable()) return;
    {
        std::lock_guard lock(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    thread_.join();
    try {
        repo_.backend().remove(name_);
    } catch (const Error&) {
        // Left behind, it goes stale and is ignored.
    }
}

bool RepoLock::held() {
    if (lost_) return false;
    // A definite answer only: saving a snapshot on a guess could reference data a prune that
    // took over the repository has deleted. A few tries ride out a network hiccup.
    for (int attempt = 0; attempt < 3; ++attempt) {
        try {
            if (!repo_.backend().exists(name_)) lost_ = true;
            return !lost_;
        } catch (const Error&) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }
    return false;  // unknown: not held, as far as saving is concerned (lost_ stays as it was)
}

void RepoLock::refresh_loop() {
    std::unique_lock lock(mu_);
    std::uint64_t last_write = info_.time;
    while (!cv_.wait_for(lock, std::chrono::minutes(1), [this] { return stop_; })) {
        // Never write back a lock that someone removed: that run may be deleting data now.
        if (!held()) return;
        const std::uint64_t now = os::now();
        if (now - last_write < kLockRefreshTicks) continue;
        info_.time = now;
        try {
            write_lock(repo_, name_, info_);
            last_write = now;
        } catch (const Error&) {
            // Try again next minute; but well before others may judge the lock stale (and take
            // the repository over), give up on it, so this run stops instead of carrying on
            // against a repository that is no longer reserved for it.
            if (now - last_write > kLockStaleTicks * 2 / 3) {
                lost_ = true;
                return;
            }
        }
    }
}

}  // namespace vaultik
