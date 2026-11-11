#include "vaultik/repo/config.hpp"

#include <cstring>

namespace vaultik {

namespace {

constexpr std::uint8_t kMagic[8] = {'V', 'A', 'U', 'L', 'T', 'I', 'K', '\0'};
constexpr std::size_t kWrappedSize = AesGcm::kOverhead + 64;
constexpr std::uint32_t kMinIterations = 1000;

}  // namespace

Bytes serialize_config_header(const RepoConfig& c) {
    ByteWriter w;
    w.raw(kMagic);
    w.u32(c.version);
    w.raw(c.repo_id);
    w.u32(c.kdf);
    w.u32(c.kdf_iterations);
    w.raw(c.salt);
    w.u32(c.chunker.min_size);
    w.u32(c.chunker.avg_size);
    w.u32(c.chunker.max_size);
    w.u32(static_cast<std::uint32_t>(c.zstd_level));
    return w.take();
}

Bytes serialize_config(const RepoConfig& c) {
    ByteWriter w;
    w.raw(serialize_config_header(c));
    w.u32(static_cast<std::uint32_t>(c.wrapped_key.size()));
    w.raw(c.wrapped_key);
    return w.take();
}

RepoConfig parse_config(ByteSpan data) {
    ByteReader r(data);
    ByteSpan magic = r.raw(sizeof kMagic);
    if (std::memcmp(magic.data(), kMagic, sizeof kMagic) != 0) throw Error("not a Vaultik repository (bad config)");
    RepoConfig c;
    c.version = r.u32();
    if (c.version != kFormatVersion) throw Error("unsupported repository format version");
    r.raw_into(c.repo_id);
    c.kdf = r.u32();
    if (c.kdf != kKdfPbkdf2Sha256) throw Error("unsupported key derivation function");
    c.kdf_iterations = r.u32();
    if (c.kdf_iterations < kMinIterations) throw Error("repository key derivation is too weak");
    if (c.kdf_iterations > kMaxKdfIterations) throw Error("malformed repository config (key derivation too slow)");
    r.raw_into(c.salt);
    c.chunker.min_size = r.u32();
    c.chunker.avg_size = r.u32();
    c.chunker.max_size = r.u32();
    c.chunker.validate();
    c.zstd_level = static_cast<int>(r.u32());
    const std::uint32_t wrapped_len = r.u32();
    if (wrapped_len != kWrappedSize) throw Error("malformed repository config");
    ByteSpan wrapped = r.raw(wrapped_len);
    c.wrapped_key.assign(wrapped.begin(), wrapped.end());
    r.expect_done();
    return c;
}

void wrap_master_key(RepoConfig& config, const MasterKey& key, std::string_view password) {
    Key32 kek = pbkdf2_sha256(password, config.salt, config.kdf_iterations);
    AesGcm aead(kek);
    secure_wipe(kek.data(), kek.size());

    std::uint8_t plain[64];
    std::memcpy(plain, key.enc.data(), 32);
    std::memcpy(plain + 32, key.id.data(), 32);
    const Bytes aad = serialize_config_header(config);
    config.wrapped_key = aead.seal(plain, aad);
    secure_wipe(plain, sizeof plain);
}

MasterKey unwrap_master_key(const RepoConfig& config, std::string_view password) {
    Key32 kek = pbkdf2_sha256(password, config.salt, config.kdf_iterations);
    AesGcm aead(kek);
    secure_wipe(kek.data(), kek.size());

    const Bytes aad = serialize_config_header(config);
    Bytes plain;
    try {
        plain = aead.open(config.wrapped_key, aad);
    } catch (const AuthError&) {
        throw AuthError("wrong password (or the repository config was modified)");
    }
    if (plain.size() != 64) throw Error("malformed repository key");
    MasterKey key;
    std::memcpy(key.enc.data(), plain.data(), 32);
    std::memcpy(key.id.data(), plain.data() + 32, 32);
    secure_wipe(plain.data(), plain.size());
    return key;
}

}  // namespace vaultik
