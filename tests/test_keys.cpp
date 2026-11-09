#include <doctest/doctest.h>

#include <cctype>
#include <filesystem>

#include "vaultik/crypto/crypto.hpp"
#include "vaultik/repo/keys.hpp"
#include "vaultik/repo/local_backend.hpp"
#include "vaultik/repo/repository.hpp"
#include "vaultik/util/hex.hpp"

using namespace vaultik;
namespace fs = std::filesystem;

namespace {

struct TempRepo {
    fs::path path;
    TempRepo() {
        std::uint8_t rnd[6];
        random_bytes(rnd);
        path = fs::temp_directory_path() / ("vaultik-keys-" + to_hex(rnd));
        LocalBackend backend(location(), true);
        InitOptions options;
        options.kdf_iterations = 1000;
        Repository::init(backend, "first password", options);
    }
    ~TempRepo() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    std::string location() const {
        const std::u8string s = path.u8string();
        return std::string(s.begin(), s.end());
    }
    bool opens_with(const char* password) const {
        try {
            Repository::open(std::make_unique<LocalBackend>(location()), password);
            return true;
        } catch (const AuthError&) {
            return false;
        }
    }
};

}  // namespace

TEST_CASE("recovery key text round trip and normalization") {
    Bytes key(20);
    for (std::size_t i = 0; i < key.size(); ++i) key[i] = static_cast<std::uint8_t>(i * 13 + 7);
    const std::string text = format_recovery_key(key);
    CHECK(text.size() == 39);  // 8 groups of 4 with 7 dashes
    CHECK(text[4] == '-');
    CHECK(parse_recovery_key(text) == key);

    std::string sloppy;
    for (const char c : text)
        if (c != '-') sloppy.push_back(static_cast<char>(c == '0' ? 'o' : c == '1' ? 'l' : std::tolower(c)));
    CHECK(parse_recovery_key(sloppy) == key);

    CHECK_THROWS_AS(parse_recovery_key("ABCD-EFGH"), Error);
    CHECK_THROWS_AS(parse_recovery_key(text + "U"), Error);
}

TEST_CASE("change the password with the old one") {
    TempRepo repo;
    LocalBackend backend(repo.location());
    CHECK_THROWS_AS(change_password(backend, {"wrong password"}, "second password"), AuthError);
    CHECK_THROWS_AS(change_password(backend, {"first password"}, "short"), Error);
    change_password(backend, {"first password"}, "second password");
    CHECK(repo.opens_with("second password"));
    CHECK_FALSE(repo.opens_with("first password"));
}

TEST_CASE("a recovery key resets a forgotten password") {
    TempRepo repo;
    LocalBackend backend(repo.location());
    CHECK_FALSE(has_recovery_key(backend));
    CHECK_THROWS_AS(create_recovery_key(backend, {"wrong password"}), AuthError);

    const std::string old_key = create_recovery_key(backend, {"first password"});
    const std::string key = create_recovery_key(backend, {"first password"});
    CHECK(has_recovery_key(backend));
    CHECK(key != old_key);

    // The newest key works, the replaced one does not.
    CHECK_THROWS_AS(change_password(backend, {old_key, true}, "reset password"), AuthError);
    change_password(backend, {key, true}, "reset password");
    CHECK(repo.opens_with("reset password"));
    CHECK_FALSE(repo.opens_with("first password"));

    // The recovery key survives the password change and still matches the master key.
    const MasterKey a = unlock_master_key(backend, {key, true});
    const MasterKey b = unlock_master_key(backend, {"reset password"});
    CHECK(a.enc == b.enc);
    CHECK(a.id == b.id);
}

TEST_CASE("a recovery key file from another repository is refused") {
    TempRepo one;
    TempRepo two;
    LocalBackend b1(one.location());
    LocalBackend b2(two.location());
    const std::string key = create_recovery_key(b1, {"first password"});
    b2.put("recovery", b1.get("recovery"));
    CHECK_THROWS_AS(change_password(b2, {key, true}, "stolen password"), AuthError);
    CHECK(two.opens_with("first password"));
}
