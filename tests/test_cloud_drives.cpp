#include <doctest/doctest.h>

#include <string>

#include "vaultik/crypto/crypto.hpp"
#include "vaultik/repo/cloud.hpp"
#include "vaultik/repo/mega_backend.hpp"
#include "vaultik/repo/oauth.hpp"
#include "vaultik/repo/webdav_backend.hpp"
#include "vaultik/util/base64.hpp"
#include "vaultik/util/hex.hpp"

using namespace vaultik;

namespace {

#include "mega_modexp_vectors.inc"

ByteSpan span_of(std::string_view s) {
    return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

Bytes hex(const std::string& h) {
    std::string even = h.size() % 2 ? "0" + h : h;
    Bytes out;
    for (std::size_t i = 0; i < even.size(); i += 2)
        out.push_back(static_cast<std::uint8_t>(std::stoi(even.substr(i, 2), nullptr, 16)));
    return out;
}

Bytes pattern(std::size_t size) {
    Bytes b(size);
    for (std::size_t i = 0; i < size; ++i) b[i] = static_cast<std::uint8_t>((i * 7 + (i >> 9)) & 0xFF);
    return b;
}

}  // namespace

TEST_CASE("base64 and base64url") {
    CHECK(base64_encode(span_of("foobar")) == "Zm9vYmFy");
    CHECK(base64_encode(span_of("fooba")) == "Zm9vYmE=");
    CHECK(base64_encode(span_of("f")) == "Zg==");
    const Bytes odd = {0xFB, 0xFF, 0xBF};
    CHECK(base64_encode(odd) == "+/+/");
    CHECK(base64url_encode(odd) == "-_-_");
    CHECK(base64_decode("-_-_") == odd);
    CHECK(base64_decode("+/+/") == odd);
    const Bytes fooba = {'f', 'o', 'o', 'b', 'a'};
    CHECK(base64_decode("Zm9vYmE=") == fooba);
    CHECK(base64_decode("Zm9vYmE") == fooba);
    CHECK_THROWS_AS(base64_decode("Zm9v!"), Error);
}

TEST_CASE("cloud drive locations") {
    CHECK(cloud_kind("pcloud:Backups/pc") == CloudKind::PCloud);
    CHECK(cloud_kind("OneDrive:x") == CloudKind::OneDrive);
    CHECK(cloud_kind("gdrive:x") == CloudKind::GoogleDrive);
    CHECK(cloud_kind("mega:x") == CloudKind::Mega);
    CHECK(cloud_kind("s3:https://h/b") == CloudKind::S3);
    CHECK(cloud_kind("C:\\Backups") == CloudKind::None);
    CHECK(cloud_kind("\\\\nas\\share") == CloudKind::None);
    CHECK(cloud_folder("pcloud:/Backups//my-pc/") == "Backups/my-pc");
    CHECK(cloud_folder("mega:Backups\\pc") == "Backups/pc");
    CHECK_THROWS_AS(cloud_folder("gdrive:"), Error);
    CHECK_THROWS_AS(cloud_folder("gdrive:/"), Error);
    CHECK_THROWS_AS(cloud_folder("onedrive:a/../b"), Error);

    const auto q = cloud::parse_query("/?code=a%2Fb&state=x+y&empty");
    CHECK(q.at("code") == "a/b");
    CHECK(q.at("state") == "x y");
    CHECK(q.contains("empty"));
    CHECK(cloud::form_encode({{"a b", "c&d"}, {"e", "/"}}) == "a%20b=c%26d&e=%2F");
}

TEST_CASE("PKCE and JWT claims") {
    const cloud::Pkce p = cloud::make_pkce();
    CHECK(p.verifier.size() == 43);
    const Key32 h = sha256(span_of(p.verifier));
    CHECK(p.challenge == base64url_encode(ByteSpan(h.data(), h.size())));
    // header.payload.signature with payload {"email":"a@b.c"}
    const std::string jwt = "eyJhbGciOiJub25lIn0." + base64url_encode(span_of(R"({"email":"a@b.c"})")) + ".sig";
    CHECK(cloud::jwt_claims(jwt).value("email", std::string()) == "a@b.c");
    CHECK(cloud::jwt_claims("garbage").empty());
}

TEST_CASE("MEGA RSA modexp matches Python's pow") {
    // 1024-bit modulus, exponent and base from an independent Python pow() run.
    CHECK(to_hex(mega::modexp(hex(kModC), hex(kModD), hex(kModN))) == kModR);
    // Small cases: 4^13 mod 497 = 445.
    CHECK(to_hex(mega::modexp(Bytes{4}, Bytes{13}, Bytes{0x01, 0xF1})) == "01bd");

    const Bytes mpi = {0x00, 0x09, 0x01, 0xFF, 0x00, 0x08, 0x7F};
    std::size_t pos = 0;
    CHECK(mega::read_mpi(mpi, pos) == Bytes{0x01, 0xFF});
    CHECK(mega::read_mpi(mpi, pos) == Bytes{0x7F});
    CHECK_THROWS_AS(mega::read_mpi(mpi, pos), Error);
}

TEST_CASE("MEGA password keys") {
    const Bytes pkey = mega::prepare_key("correct horse battery staple");
    CHECK(to_hex(pkey) == "57d978ef82e99ea50ab00045e8614d7b");
    CHECK(mega::stringhash("someone@example.com", pkey) == "_k5UtaPwK-A");
    Bytes salt(32);
    for (int i = 0; i < 32; ++i) salt[i] = static_cast<std::uint8_t>(i);
    CHECK(to_hex(pbkdf2_sha512("correct horse battery staple", salt, 100000, 32)) ==
          "88aa99bab648e0a15a6dcfd127cb7d9f17d1a3fa0caa6ca9cc97e2a09600bbc0");
}

TEST_CASE("MEGA file encryption") {
    const Bytes key = hex("000102030405060708090a0b0c0d0e0f");
    const Bytes nonce = hex("a1a2a3a4a5a6a7a8");

    CHECK(mega::encrypt_attributes({{"n", "data"}}, key) == "V2kIwqVhlUxe_TCjJoWbgA");
    CHECK(mega::decrypt_attributes("V2kIwqVhlUxe_TCjJoWbgA", key).value("n", std::string()) == "data");
    CHECK(mega::decrypt_attributes("V2kIwqVhlUxe_TCjJoWbgA", hex("ff0102030405060708090a0b0c0d0e0f")).empty());

    const auto ch = mega::chunks(2621440 + 37);
    REQUIRE(ch.size() == 6);  // 128K * (1+2+3+4+5) = 1.875 MiB, then the last 0.625 MiB + 37 bytes
    CHECK(ch[0] == std::make_pair(std::uint64_t{0}, std::size_t{131072}));
    CHECK(ch[1].first == 131072);
    CHECK(mega::chunks(0).size() == 1);

    const Bytes plain = pattern(2621440 + 37);
    CHECK(to_hex(mega::meta_mac(key, nonce, plain)) == "2f5a3c4c5823334e");

    Bytes slice(plain.begin() + 1000003, plain.begin() + 1000003 + 50);
    CHECK(to_hex(slice) ==
          "767d848b9299a0a7aeb5bcc3cad1d8dfe6edf4fb020910171e252c333a41484f565d646b727980878e959ca3aab1b8bfc6cd");
    mega::ctr_crypt(key, nonce, 1000003, slice.data(), slice.size());
    CHECK(to_hex(slice) ==
          "9d61e80537b16585ad2b05d63df60f142f97183fdba42e13f2d86ea803fce9aaacff79d49c93b04295b4230e3114eb81af8d");

    // Whole-file CTR, then any range decrypts on its own.
    Bytes whole = plain;
    mega::ctr_crypt(key, nonce, 0, whole.data(), whole.size());
    Bytes part(whole.begin() + 1000003, whole.begin() + 1000003 + 50);
    CHECK(to_hex(part) ==
          "9d61e80537b16585ad2b05d63df60f142f97183fdba42e13f2d86ea803fce9aaacff79d49c93b04295b4230e3114eb81af8d");
    mega::ctr_crypt(key, nonce, 0, whole.data(), whole.size());
    CHECK(whole == plain);

    const Bytes mac = mega::meta_mac(key, nonce, plain);
    const Bytes nk = mega::pack_file_key(key, nonce, mac);
    CHECK(nk.size() == 32);
    CHECK(mega::file_aes_key(nk) == key);
    CHECK(mega::file_nonce(nk) == nonce);
}

TEST_CASE("WebDAV folder listings are read whatever the namespace prefix") {
    const std::string xml =
        "<?xml version=\"1.0\"?><d:multistatus xmlns:d=\"DAV:\">"
        "<d:response><d:href>/dav/backups/pc/</d:href><d:propstat><d:prop><d:resourcetype><d:collection/>"
        "</d:resourcetype></d:prop></d:propstat></d:response>"
        "<d:response><d:href>/dav/backups/pc/data/</d:href><d:propstat><d:prop><d:resourcetype><d:collection />"
        "</d:resourcetype></d:prop></d:propstat></d:response>"
        "<d:response><d:href>https://cloud.example.com/dav/backups/pc/config</d:href><d:propstat><d:prop>"
        "<d:resourcetype/></d:prop></d:propstat></d:response>"
        "<D:response xmlns:D=\"DAV:\"><D:href>/dav/backups/pc/my%20file&amp;x</D:href></D:response>"
        "</d:multistatus>";
    const std::vector<std::string> paths = parse_propfind(xml);
    REQUIRE(paths.size() == 4);
    CHECK(paths[0] == "/dav/backups/pc/");
    CHECK(paths[1] == "/dav/backups/pc/data/");
    CHECK(paths[2] == "/dav/backups/pc/config");
    CHECK(paths[3] == "/dav/backups/pc/my file&x");
    CHECK_THROWS(WebDavBackend("webdav:http://cloud.example.com/x", {}));  // passwords only over https
    CHECK_NOTHROW(WebDavBackend("webdav:http://192.168.1.10/x", {}));      // or on the own network
    CHECK_NOTHROW(WebDavBackend("webdav:http://172.20.0.5:8080/x", {}));
    CHECK_THROWS(WebDavBackend("webdav:http://10.example.com/x", {}));     // a name, not an address
    CHECK_THROWS(WebDavBackend("webdav:http://192.168.1.10.example.com/x", {}));
    CHECK_THROWS(WebDavBackend("webdav:http://172.32.0.1/x", {}));
}
