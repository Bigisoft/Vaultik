#include "vaultik/repo/keys.hpp"

#include <cstring>

#include "vaultik/crypto/crypto.hpp"

namespace vaultik {

namespace {

constexpr char kConfigName[] = "config";
constexpr char kRecoveryName[] = "recovery";
constexpr std::uint8_t kRecoveryMagic[8] = {'V', 'K', 'R', 'E', 'C', 'O', 'V', '\0'};
constexpr std::uint32_t kRecoveryVersion = 1;
// The recovery key is random (160 bits), so the derivation only has to be sound, not slow.
constexpr std::uint32_t kRecoveryIterations = 100000;
constexpr std::size_t kRecoveryKeyBytes = 20;
constexpr std::size_t kWrappedSize = AesGcm::kOverhead + 64;
// Crockford's base32: no I, L, O or U, so the key is easy to read back and type.
constexpr char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

struct RecoveryFile {
    std::uint32_t iterations = kRecoveryIterations;
    std::array<std::uint8_t, 32> salt{};
    Bytes wrapped_key;
};

// AAD of the recovery seal: the file header plus the repository id from config, so a recovery
// file copied from another repository does not open this one.
Bytes recovery_header(const RepoConfig& config, const RecoveryFile& f) {
    ByteWriter w;
    w.raw(kRecoveryMagic);
    w.u32(kRecoveryVersion);
    w.raw(config.repo_id);
    w.u32(f.iterations);
    w.raw(f.salt);
    return w.take();
}

RecoveryFile parse_recovery(ByteSpan data, const RepoConfig& config) {
    ByteReader r(data);
    ByteSpan magic = r.raw(sizeof kRecoveryMagic);
    if (std::memcmp(magic.data(), kRecoveryMagic, sizeof kRecoveryMagic) != 0)
        throw Error("malformed recovery key file");
    if (r.u32() != kRecoveryVersion) throw Error("unsupported recovery key file version");
    std::array<std::uint8_t, 16> repo_id{};
    r.raw_into(repo_id);
    if (repo_id != config.repo_id) throw AuthError("the recovery key file belongs to another repository");
    RecoveryFile f;
    f.iterations = r.u32();
    if (f.iterations < 1000) throw Error("recovery key derivation is too weak");
    r.raw_into(f.salt);
    const std::uint32_t len = r.u32();
    if (len != kWrappedSize) throw Error("malformed recovery key file");
    ByteSpan wrapped = r.raw(len);
    f.wrapped_key.assign(wrapped.begin(), wrapped.end());
    r.expect_done();
    return f;
}

Key32 recovery_kek(ByteSpan recovery_key, const RecoveryFile& f) {
    const std::string_view text(reinterpret_cast<const char*>(recovery_key.data()), recovery_key.size());
    return pbkdf2_sha256(text, f.salt, f.iterations);
}

RepoConfig read_config(Backend& backend) {
    if (!backend.exists(kConfigName)) throw Error("no Vaultik repository at " + backend.location());
    return parse_config(backend.get(kConfigName));
}

MasterKey unlock(Backend& backend, const RepoConfig& config, const Credential& credential) {
    if (!credential.recovery_key) return unwrap_master_key(config, credential.secret);
    Bytes key = parse_recovery_key(credential.secret);
    if (!backend.exists(kRecoveryName)) throw Error("this repository has no recovery key");
    const RecoveryFile f = parse_recovery(backend.get(kRecoveryName), config);
    Key32 kek = recovery_kek(key, f);
    secure_wipe(key.data(), key.size());
    AesGcm aead(kek);
    secure_wipe(kek.data(), kek.size());
    Bytes plain;
    try {
        plain = aead.open(f.wrapped_key, recovery_header(config, f));
    } catch (const AuthError&) {
        throw AuthError("wrong recovery key (or the recovery key file was modified)");
    }
    if (plain.size() != 64) throw Error("malformed repository key");
    MasterKey mk;
    std::memcpy(mk.enc.data(), plain.data(), 32);
    std::memcpy(mk.id.data(), plain.data() + 32, 32);
    secure_wipe(plain.data(), plain.size());
    return mk;
}

void wipe(MasterKey& key) {
    secure_wipe(key.enc.data(), key.enc.size());
    secure_wipe(key.id.data(), key.id.size());
}

}  // namespace

MasterKey unlock_master_key(Backend& backend, const Credential& credential) {
    return unlock(backend, read_config(backend), credential);
}

bool has_recovery_key(Backend& backend) {
    return backend.exists(kRecoveryName);
}

std::string create_recovery_key(Backend& backend, const Credential& credential) {
    const RepoConfig config = read_config(backend);
    MasterKey mk = unlock(backend, config, credential);

    Bytes key(kRecoveryKeyBytes);
    random_bytes(key);
    RecoveryFile f;
    random_bytes(f.salt);
    Key32 kek = recovery_kek(key, f);
    AesGcm aead(kek);
    secure_wipe(kek.data(), kek.size());
    std::uint8_t plain[64];
    std::memcpy(plain, mk.enc.data(), 32);
    std::memcpy(plain + 32, mk.id.data(), 32);
    wipe(mk);
    f.wrapped_key = aead.seal(plain, recovery_header(config, f));
    secure_wipe(plain, sizeof plain);

    ByteWriter w;
    w.raw(recovery_header(config, f));
    w.u32(static_cast<std::uint32_t>(f.wrapped_key.size()));
    w.raw(f.wrapped_key);
    backend.put(kRecoveryName, w.take());

    std::string text = format_recovery_key(key);
    secure_wipe(key.data(), key.size());
    return text;
}

void change_password(Backend& backend, const Credential& credential, std::string_view new_password) {
    if (new_password.size() < kMinPasswordLength)
        throw Error("use a password of at least " + std::to_string(kMinPasswordLength) + " characters");
    RepoConfig config = read_config(backend);
    MasterKey mk = unlock(backend, config, credential);
    random_bytes(config.salt);
    wrap_master_key(config, mk, new_password);
    wipe(mk);
    backend.put(kConfigName, serialize_config(config));
}

std::string format_recovery_key(ByteSpan key) {
    std::string out;
    std::uint32_t buffer = 0;
    int bits = 0;
    int written = 0;
    for (const std::uint8_t b : key) {
        buffer = (buffer << 8) | b;
        bits += 8;
        while (bits >= 5) {
            if (written > 0 && written % 4 == 0) out.push_back('-');
            out.push_back(kAlphabet[(buffer >> (bits - 5)) & 31u]);
            bits -= 5;
            ++written;
        }
    }
    if (bits > 0) {
        if (written > 0 && written % 4 == 0) out.push_back('-');
        out.push_back(kAlphabet[(buffer << (5 - bits)) & 31u]);
    }
    return out;
}

Bytes parse_recovery_key(std::string_view text) {
    Bytes out;
    std::uint32_t buffer = 0;
    int bits = 0;
    std::size_t chars = 0;
    for (char c : text) {
        if (c == '-' || c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        if (c == 'O') c = '0';
        if (c == 'I' || c == 'L') c = '1';
        const char* pos = std::strchr(kAlphabet, c);
        if (c == '\0' || pos == nullptr) throw Error("the recovery key holds a character it cannot contain");
        buffer = (buffer << 5) | static_cast<std::uint32_t>(pos - kAlphabet);
        bits += 5;
        ++chars;
        if (bits >= 8) {
            out.push_back(static_cast<std::uint8_t>(buffer >> (bits - 8)));
            bits -= 8;
        }
    }
    buffer = 0;
    if (chars != 32 || out.size() != kRecoveryKeyBytes) {
        secure_wipe(out.data(), out.size());
        throw Error("a recovery key has 32 characters (8 groups of 4)");
    }
    return out;
}

}  // namespace vaultik
