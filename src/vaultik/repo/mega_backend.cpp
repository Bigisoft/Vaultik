#include "vaultik/repo/mega_backend.hpp"

#include <algorithm>
#include <cstring>
#include <thread>

#include "vaultik/crypto/crypto.hpp"
#include "vaultik/platform/http.hpp"
#include "vaultik/repo/oauth.hpp"
#include "vaultik/util/base64.hpp"
#include "vaultik/util/resources.hpp"

namespace vaultik {

namespace {

using nlohmann::json;

constexpr const char* kApi = "https://g.api.mega.co.nz";
constexpr std::size_t kUploadPiece = 8u << 20;

// ---- big integers (only for the RSA step of signing in) ----------------------------------------

using Limbs = std::vector<std::uint32_t>;  // little-endian

Limbs from_be(ByteSpan b) {
    std::size_t start = 0;
    while (start < b.size() && b[start] == 0) ++start;
    const std::size_t n = b.size() - start;
    Limbs out((n + 3) / 4, 0);
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t bit = (n - 1 - i) * 8;
        out[bit / 32] |= static_cast<std::uint32_t>(b[start + i]) << (bit % 32);
    }
    return out;
}

Bytes to_be(const Limbs& a) {
    Bytes out;
    bool started = false;
    for (std::size_t i = a.size(); i-- > 0;) {
        for (int s = 24; s >= 0; s -= 8) {
            const auto byte = static_cast<std::uint8_t>(a[i] >> s);
            if (byte == 0 && !started) continue;
            started = true;
            out.push_back(byte);
        }
    }
    if (out.empty()) out.push_back(0);
    return out;
}

// a >= b, both of n limbs.
bool geq(const Limbs& a, const Limbs& b) {
    for (std::size_t i = a.size(); i-- > 0;) {
        if (a[i] != b[i]) return a[i] > b[i];
    }
    return true;
}

// a -= b modulo 2^(32n).
void sub(Limbs& a, const Limbs& b) {
    std::uint64_t borrow = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const std::uint64_t d = static_cast<std::uint64_t>(a[i]) - b[i] - borrow;
        a[i] = static_cast<std::uint32_t>(d);
        borrow = (d >> 63) & 1;
    }
}

// x = 2x mod n (x < n).
void double_mod(Limbs& x, const Limbs& n) {
    std::uint32_t carry = 0;
    for (std::uint32_t& limb : x) {
        const std::uint32_t next = limb >> 31;
        limb = (limb << 1) | carry;
        carry = next;
    }
    if (carry != 0 || geq(x, n)) sub(x, n);
}

struct Montgomery {
    Limbs n;
    std::uint32_t n0inv = 0;  // -n^-1 mod 2^32

    explicit Montgomery(Limbs mod) : n(std::move(mod)) {
        std::uint32_t inv = 1;
        for (int i = 0; i < 5; ++i) inv *= 2 - n[0] * inv;
        n0inv = 0u - inv;
    }

    Limbs mul(const Limbs& a, const Limbs& b) const {
        const std::size_t k = n.size();
        std::vector<std::uint32_t> t(k + 2, 0);
        for (std::size_t i = 0; i < k; ++i) {
            std::uint64_t c = 0;
            for (std::size_t j = 0; j < k; ++j) {
                const std::uint64_t s = static_cast<std::uint64_t>(t[j]) + static_cast<std::uint64_t>(a[j]) * b[i] + c;
                t[j] = static_cast<std::uint32_t>(s);
                c = s >> 32;
            }
            std::uint64_t s = static_cast<std::uint64_t>(t[k]) + c;
            t[k] = static_cast<std::uint32_t>(s);
            t[k + 1] = static_cast<std::uint32_t>(s >> 32);

            const std::uint32_t m = t[0] * n0inv;
            s = static_cast<std::uint64_t>(t[0]) + static_cast<std::uint64_t>(m) * n[0];
            c = s >> 32;
            for (std::size_t j = 1; j < k; ++j) {
                s = static_cast<std::uint64_t>(t[j]) + static_cast<std::uint64_t>(m) * n[j] + c;
                t[j - 1] = static_cast<std::uint32_t>(s);
                c = s >> 32;
            }
            s = static_cast<std::uint64_t>(t[k]) + c;
            t[k - 1] = static_cast<std::uint32_t>(s);
            t[k] = t[k + 1] + static_cast<std::uint32_t>(s >> 32);
        }
        Limbs r(t.begin(), t.begin() + static_cast<std::ptrdiff_t>(k));
        if (t[k] != 0 || geq(r, n)) sub(r, n);
        return r;
    }
};

// ---- small helpers ----------------------------------------------------------------------------

std::uint32_t word_at(ByteSpan b, std::size_t i) {
    return (static_cast<std::uint32_t>(b[4 * i]) << 24) | (static_cast<std::uint32_t>(b[4 * i + 1]) << 16) |
           (static_cast<std::uint32_t>(b[4 * i + 2]) << 8) | b[4 * i + 3];
}

void put_word(std::uint8_t* out, std::uint32_t w) {
    out[0] = static_cast<std::uint8_t>(w >> 24);
    out[1] = static_cast<std::uint8_t>(w >> 16);
    out[2] = static_cast<std::uint8_t>(w >> 8);
    out[3] = static_cast<std::uint8_t>(w);
}

Bytes padded(ByteSpan b, std::size_t multiple) {
    Bytes out(b.begin(), b.end());
    while (out.size() % multiple != 0) out.push_back(0);
    return out;
}

ByteSpan span_of(std::string_view s) {
    return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

std::string request_id() {
    std::uint8_t rnd[8];
    random_bytes(rnd);
    return base64url_encode(ByteSpan(rnd, sizeof rnd)).substr(0, 10);
}

std::string error_name(int code) {
    switch (code) {
        case -2: return "bad arguments";
        case -3: return "try again";
        case -4: return "rate limited";
        case -5: return "failed";
        case -6: return "too many";
        case -9: return "not found";
        case -11: return "access denied";
        case -13: return "incomplete";
        case -14: return "wrong key";
        case -15: return "session ended";
        case -16: return "account blocked";
        case -17: return "over quota";
        case -18: return "temporarily unavailable";
        case -26: return "two-factor code needed";
        default: return "error " + std::to_string(code);
    }
}

// One API command; returns its answer (an object, or a negative number for errors the caller
// handles such as -9). Retries "try again" and rate limits.
json mega_request(http::Client& client, std::uint64_t& seq, const std::string& sid, const json& command,
                  const std::string& what) {
    const std::string body = "[" + command.dump() + "]";
    int wait_s = 1;
    for (int attempt = 0;; ++attempt) {
        const std::string url = std::string(kApi) + "/cs?id=" + std::to_string(seq++) + (sid.empty() ? "" : "&sid=" + sid);
        const http::Response r = cloud::send_with_retry(
            client,
            [&] {
                http::Request req;
                req.method = "POST";
                req.url = url;
                req.headers = {{"Content-Type", "application/json"}};
                req.body = span_of(body);
                return req;
            },
            what);
        if (r.status == 402)
            throw Error(what + ": MEGA asked for a proof of work this client cannot do yet; try again later");
        if (r.status != 200) throw Error(what + ": MEGA answered HTTP " + std::to_string(r.status));
        json j;
        try {
            j = json::parse(r.body.begin(), r.body.end());
        } catch (const json::exception&) {
            throw Error(what + ": unexpected answer from MEGA");
        }
        json result = j.is_array() && !j.empty() ? j[0] : j;
        int code = 0;
        if (result.is_number_integer()) code = result.get<int>();
        else if (result.is_object() && result.contains("err") && result["err"].is_number_integer())
            code = result["err"].get<int>();
        if (code == -3 || code == -4 || code == -18) {
            if (attempt >= 8) throw Error(what + ": MEGA is busy (" + error_name(code) + "); try again later");
            std::this_thread::sleep_for(std::chrono::seconds(wait_s));
            wait_s = std::min(wait_s * 2, 30);
            continue;
        }
        if (code == -26) throw MegaCodeNeeded("MEGA needs the two-factor authentication code");
        if (code == -15 || code == -16)
            throw Error(what + ": " + error_name(code) + " - sign in to MEGA again in Vaultik");
        if (code == -17) throw Error(what + ": the MEGA account is over its storage quota");
        if (code < 0 && code != -9) throw Error(what + ": MEGA error " + std::to_string(code) + " (" + error_name(code) + ")");
        return result;
    }
}

bool is_error(const json& j, int code) {
    return j.is_number_integer() && j.get<int>() == code;
}

}  // namespace

// ---- protocol pieces --------------------------------------------------------------------------

namespace mega {

Bytes modexp(ByteSpan base, ByteSpan exp, ByteSpan mod) {
    Limbs n = from_be(mod);
    if (n.empty() || (n[0] & 1) == 0) throw Error("RSA modulus must be odd");
    const std::size_t k = n.size();
    // base mod n, bit by bit.
    Limbs x(k, 0);
    for (const std::uint8_t byte : base) {
        for (int bit = 7; bit >= 0; --bit) {
            double_mod(x, n);
            if ((byte >> bit) & 1) {
                // x + 1 <= n
                std::uint64_t carry = 1;
                for (std::size_t i = 0; i < k && carry; ++i) {
                    const std::uint64_t s = static_cast<std::uint64_t>(x[i]) + carry;
                    x[i] = static_cast<std::uint32_t>(s);
                    carry = s >> 32;
                }
                if (geq(x, n)) sub(x, n);
            }
        }
    }
    // R mod n and R^2 mod n, R = 2^(32k).
    Limbs r(k, 0);
    r[0] = 1;
    if (k == 1 && n[0] == 1) return Bytes{0};
    for (std::size_t i = 0; i < 32 * k; ++i) double_mod(r, n);
    Limbs r2 = r;
    for (std::size_t i = 0; i < 32 * k; ++i) double_mod(r2, n);

    const Montgomery m(n);
    const Limbs xm = m.mul(x, r2);
    Limbs acc = r;  // 1 in Montgomery form
    for (const std::uint8_t byte : exp) {
        for (int bit = 7; bit >= 0; --bit) {
            acc = m.mul(acc, acc);
            if ((byte >> bit) & 1) acc = m.mul(acc, xm);
        }
    }
    Limbs one(k, 0);
    one[0] = 1;
    return to_be(m.mul(acc, one));
}

Bytes read_mpi(ByteSpan data, std::size_t& pos) {
    if (pos + 2 > data.size()) throw Error("MEGA key data is too short");
    const std::size_t bits = (static_cast<std::size_t>(data[pos]) << 8) | data[pos + 1];
    const std::size_t len = (bits + 7) / 8;
    pos += 2;
    if (pos + len > data.size()) throw Error("MEGA key data is too short");
    Bytes out(data.begin() + static_cast<std::ptrdiff_t>(pos), data.begin() + static_cast<std::ptrdiff_t>(pos + len));
    pos += len;
    return out;
}

Bytes prepare_key(std::string_view password) {
    const Bytes pw = padded(span_of(password), 4);
    const std::size_t words = pw.size() / 4;
    std::vector<std::unique_ptr<Aes128>> ciphers;
    for (std::size_t j = 0; j * 4 < words; ++j) {
        std::uint8_t key[16] = {};
        for (std::size_t w = 0; w < 4 && (4 * j + w) < words; ++w) std::memcpy(key + 4 * w, pw.data() + 4 * (4 * j + w), 4);
        ciphers.push_back(std::make_unique<Aes128>(ByteSpan(key, 16)));
    }
    Bytes pkey(16);
    put_word(pkey.data(), 0x93C467E3);
    put_word(pkey.data() + 4, 0x7DB0C7A4);
    put_word(pkey.data() + 8, 0xD1BE3F81);
    put_word(pkey.data() + 12, 0x0152CB56);
    for (int round = 0; round < 65536; ++round)
        for (const auto& c : ciphers) c->ecb_encrypt(pkey.data(), 16);
    return pkey;
}

std::string stringhash(std::string_view email, ByteSpan password_key) {
    const Bytes e = padded(span_of(email), 4);
    std::uint32_t h[4] = {0, 0, 0, 0};
    for (std::size_t i = 0; i < e.size() / 4; ++i) h[i & 3] ^= word_at(e, i);
    std::uint8_t hb[16];
    for (int i = 0; i < 4; ++i) put_word(hb + 4 * i, h[i]);
    const Aes128 aes(password_key);
    for (int i = 0; i < 16384; ++i) aes.ecb_encrypt(hb, 16);
    std::uint8_t out[8];
    std::memcpy(out, hb, 4);
    std::memcpy(out + 4, hb + 8, 4);
    return base64url_encode(ByteSpan(out, 8));
}

std::string encrypt_attributes(const json& attributes, ByteSpan key) {
    const std::string text = "MEGA" + attributes.dump();
    Bytes buf = padded(span_of(text), 16);
    const std::uint8_t iv[16] = {};
    Aes128(key).cbc_encrypt(iv, buf.data(), buf.size());
    return base64url_encode(buf);
}

json decrypt_attributes(std::string_view b64, ByteSpan key) {
    Bytes buf;
    try {
        buf = base64_decode(b64);
    } catch (const Error&) {
        return json::object();
    }
    if (buf.empty() || buf.size() % 16 != 0) return json::object();
    const std::uint8_t iv[16] = {};
    Aes128(key).cbc_decrypt(iv, buf.data(), buf.size());
    if (buf.size() < 4 || std::memcmp(buf.data(), "MEGA", 4) != 0) return json::object();
    const std::string text(buf.begin() + 4, buf.end());
    const std::size_t a = text.find('{');
    const std::size_t b = text.rfind('}');
    if (a == std::string::npos || b == std::string::npos || b < a) return json::object();
    try {
        return json::parse(text.substr(a, b - a + 1));
    } catch (const json::exception&) {
        return json::object();
    }
}

std::vector<std::pair<std::uint64_t, std::size_t>> chunks(std::uint64_t size) {
    std::vector<std::pair<std::uint64_t, std::size_t>> out;
    if (size == 0) {
        out.emplace_back(0, 0);
        return out;
    }
    std::uint64_t offset = 0;
    for (std::size_t i = 1; offset < size; ++i) {
        const std::size_t want = i <= 8 ? i * 131072 : 1048576;
        const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(want, size - offset));
        out.emplace_back(offset, n);
        offset += n;
    }
    return out;
}

void ctr_crypt(ByteSpan key, ByteSpan nonce, std::uint64_t offset, std::uint8_t* data, std::size_t size) {
    const Aes128 aes(key);
    std::uint64_t block = offset / 16;
    std::size_t skip = static_cast<std::size_t>(offset % 16);
    std::size_t done = 0;
    Bytes stream(64 * 1024);
    while (done < size) {
        const std::size_t want = std::min(stream.size(), ((size - done + skip + 15) / 16) * 16);
        for (std::size_t i = 0; i < want; i += 16) {
            std::memcpy(stream.data() + i, nonce.data(), 8);
            const std::uint64_t b = block + i / 16;
            for (int s = 0; s < 8; ++s) stream[i + 8 + s] = static_cast<std::uint8_t>(b >> (56 - 8 * s));
        }
        aes.ecb_encrypt(stream.data(), want);
        const std::size_t n = std::min(want - skip, size - done);
        for (std::size_t i = 0; i < n; ++i) data[done + i] ^= stream[skip + i];
        done += n;
        block += want / 16;
        skip = 0;
    }
}

Bytes meta_mac(ByteSpan key, ByteSpan nonce, ByteSpan plaintext) {
    const Aes128 aes(key);
    std::uint8_t iv[16];
    std::memcpy(iv, nonce.data(), 8);
    std::memcpy(iv + 8, nonce.data(), 8);
    Bytes macs;
    Bytes buf;
    for (const auto& [off, n] : chunks(plaintext.size())) {
        if (n == 0) {
            macs.insert(macs.end(), 16, 0);
            continue;
        }
        buf.assign(plaintext.begin() + static_cast<std::ptrdiff_t>(off),
                   plaintext.begin() + static_cast<std::ptrdiff_t>(off + n));
        while (buf.size() % 16 != 0) buf.push_back(0);
        aes.cbc_encrypt(iv, buf.data(), buf.size());
        macs.insert(macs.end(), buf.end() - 16, buf.end());
    }
    const std::uint8_t zero[16] = {};
    aes.cbc_encrypt(zero, macs.data(), macs.size());
    const std::uint8_t* t = macs.data() + macs.size() - 16;
    Bytes out(8);
    for (int i = 0; i < 4; ++i) {
        out[i] = t[i] ^ t[4 + i];
        out[4 + i] = t[8 + i] ^ t[12 + i];
    }
    return out;
}

Bytes pack_file_key(ByteSpan key, ByteSpan nonce, ByteSpan mac) {
    Bytes out(32);
    for (int i = 0; i < 8; ++i) {
        out[i] = key[i] ^ nonce[i];
        out[8 + i] = key[8 + i] ^ mac[i];
    }
    std::memcpy(out.data() + 16, nonce.data(), 8);
    std::memcpy(out.data() + 24, mac.data(), 8);
    return out;
}

Bytes file_aes_key(ByteSpan nk) {
    Bytes out(16);
    for (int i = 0; i < 16; ++i) out[i] = nk[i] ^ nk[16 + i];
    return out;
}

Bytes file_nonce(ByteSpan nk) {
    return Bytes(nk.begin() + 16, nk.begin() + 24);
}

}  // namespace mega

// ---- the backend ------------------------------------------------------------------------------

MegaBackend::MegaBackend(const std::string& location, CloudCredentials credentials)
    : location_(location), root_folder_(cloud_folder(location)), client_(std::make_unique<http::Client>()) {
    const json secret = cloud::parse_secret(credentials.secret, "MEGA");
    sid_ = secret.value("sid", std::string());
    user_ = secret.value("user", std::string());
    master_ = base64_decode(secret.value("master_key", std::string()));
    if (sid_.empty() || user_.empty() || master_.size() != 16)
        throw Error("no MEGA sign-in stored: sign in to MEGA in Vaultik first");
    if (sid_.find_first_of("&?#/ ") != std::string::npos) throw Error("bad MEGA session in the stored sign-in");
    std::uint8_t rnd[4];
    random_bytes(rnd);
    seq_ = (static_cast<std::uint64_t>(rnd[0]) << 24) | (rnd[1] << 16) | (rnd[2] << 8) | rnd[3];
}

MegaBackend::~MegaBackend() {
    secure_wipe(master_.data(), master_.size());
}

std::string MegaBackend::location() const {
    return location_;
}

json MegaBackend::api(const json& command) {
    std::lock_guard lock(mu_);
    return mega_request(*client_, seq_, sid_, command, location_);
}

bool MegaBackend::add_node(const json& n) {
    Node node;
    node.handle = n.value("h", std::string());
    node.parent = n.value("p", std::string());
    node.type = n.value("t", -1);
    node.size = n.value("s", std::uint64_t{0});
    node.ts = n.value("ts", std::int64_t{0});
    if (node.handle.empty()) return false;
    if (node.type >= 2 && node.type <= 4) {
        if (node.type == 2) cloud_root_ = node.handle;
    } else if (node.type == 0 || node.type == 1) {
        // "k" holds owner:key pairs separated by '/'; only our own key opens the node.
        const std::string keys = n.value("k", std::string());
        Bytes key;
        std::size_t pos = 0;
        while (pos < keys.size()) {
            std::size_t end = keys.find('/', pos);
            if (end == std::string::npos) end = keys.size();
            const std::string pair = keys.substr(pos, end - pos);
            pos = end + 1;
            const std::size_t colon = pair.find(':');
            if (colon == std::string::npos || pair.substr(0, colon) != user_) continue;
            try {
                key = base64_decode(pair.substr(colon + 1));
            } catch (const Error&) {
                continue;
            }
            break;
        }
        const std::size_t want = node.type == 0 ? 32 : 16;
        if (key.size() != want) return false;  // shared from someone else, or damaged
        Aes128(master_).ecb_decrypt(key.data(), key.size());
        node.key = key;
        const Bytes attr_key = node.type == 0 ? mega::file_aes_key(key) : key;
        node.name = mega::decrypt_attributes(n.value("a", std::string()), attr_key).value("n", std::string());
        if (node.name.empty()) return false;
    } else {
        return false;
    }
    if (auto old = nodes_.find(node.handle); old != nodes_.end()) children_[old->second.parent].erase(node.handle);
    children_[node.parent].insert(node.handle);
    nodes_[node.handle] = std::move(node);
    return true;
}

void MegaBackend::drop_node(const std::string& handle) {
    auto it = nodes_.find(handle);
    if (it == nodes_.end()) return;
    if (auto kids = children_.find(handle); kids != children_.end()) {
        const std::set<std::string> copy = kids->second;
        for (const std::string& k : copy) drop_node(k);
        children_.erase(handle);
    }
    children_[it->second.parent].erase(handle);
    links_.erase(handle);
    nodes_.erase(it);
    folder_cache_.clear();
}

void MegaBackend::load_tree() {
    std::lock_guard lock(mu_);
    const json r = api({{"a", "f"}, {"c", 1}});
    if (!r.is_object()) throw Error(location_ + ": cannot read the MEGA file list");
    nodes_.clear();
    children_.clear();
    folder_cache_.clear();
    links_.clear();
    cloud_root_.clear();
    for (const json& n : r.value("f", json::array())) add_node(n);
    if (cloud_root_.empty()) throw Error(location_ + ": the MEGA account has no Cloud Drive");
    sn_ = r.value("sn", std::string());
    loaded_ = true;
}

void MegaBackend::catch_up() {
    std::lock_guard lock(mu_);
    if (!loaded_) {
        load_tree();
        return;
    }
    if (sn_.empty()) return;
    // Changes since the tree was read (from this or another computer), until the feed says
    // "nothing more" by offering a wait address.
    for (int round = 0; round < 50; ++round) {
        const std::string url = std::string(kApi) + "/sc?sn=" + sn_ + "&sid=" + sid_;
        const http::Response r = cloud::send_with_retry(
            *client_,
            [&] {
                http::Request req;
                req.method = "POST";
                req.url = url;
                return req;
            },
            location_ + ": reading MEGA changes");
        json j;
        try {
            j = json::parse(r.body.begin(), r.body.end());
        } catch (const json::exception&) {
            load_tree();
            return;
        }
        if (!j.is_object()) {  // an error such as an expired position: read everything again
            load_tree();
            return;
        }
        for (const json& e : j.value("a", json::array())) {
            const std::string a = e.value("a", std::string());
            if (a == "t" && e.contains("t")) {
                for (const json& n : e["t"].value("f", json::array())) add_node(n);
                folder_cache_.clear();
            } else if (a == "d") {
                drop_node(e.value("n", std::string()));
            } else if (a == "u") {
                auto it = nodes_.find(e.value("n", std::string()));
                if (it != nodes_.end() && !it->second.key.empty() && e.contains("at")) {
                    const Bytes attr_key = it->second.type == 0 ? mega::file_aes_key(it->second.key) : it->second.key;
                    const std::string name =
                        mega::decrypt_attributes(e.value("at", std::string()), attr_key).value("n", std::string());
                    if (!name.empty()) it->second.name = name;
                    folder_cache_.clear();
                }
            }
        }
        sn_ = j.value("sn", sn_);
        if (j.contains("w")) return;
    }
}

std::vector<std::string> MegaBackend::children_named(const std::string& parent, const std::string& name,
                                                     bool folders) const {
    std::vector<std::string> out;
    auto it = children_.find(parent);
    if (it == children_.end()) return out;
    for (const std::string& h : it->second) {
        const Node& n = nodes_.at(h);
        if (n.name == name && n.type == (folders ? 1 : 0)) out.push_back(h);
    }
    std::sort(out.begin(), out.end(), [&](const std::string& a, const std::string& b) {
        return nodes_.at(a).ts > nodes_.at(b).ts;
    });
    return out;
}

std::string MegaBackend::make_folder(const std::string& parent, const std::string& name) {
    std::uint8_t key[16];
    random_bytes(key);
    Bytes wrapped(key, key + 16);
    Aes128(master_).ecb_encrypt(wrapped.data(), wrapped.size());
    const json r = api({{"a", "p"},
                        {"t", parent},
                        {"n", json::array({{{"h", "xxxxxxxx"},
                                            {"t", 1},
                                            {"a", mega::encrypt_attributes({{"n", name}}, ByteSpan(key, 16))},
                                            {"k", base64url_encode(wrapped)}}})},
                        {"i", request_id()}});
    secure_wipe(key, sizeof key);
    if (!r.is_object() || !r.contains("f") || r["f"].empty() || !add_node(r["f"][0]))
        throw Error(location_ + ": creating folder " + name + " in MEGA failed");
    return r["f"][0].value("h", std::string());
}

std::string MegaBackend::folder_handle(const std::string& dir, bool create) {
    std::lock_guard lock(mu_);
    if (!loaded_) load_tree();
    if (auto it = folder_cache_.find(dir); it != folder_cache_.end() && nodes_.contains(it->second)) return it->second;
    const std::string path = dir.empty() ? root_folder_ : root_folder_ + "/" + dir;
    std::string current = cloud_root_;
    std::size_t pos = 0;
    while (pos <= path.size()) {
        std::size_t end = path.find('/', pos);
        if (end == std::string::npos) end = path.size();
        const std::string part = path.substr(pos, end - pos);
        pos = end + 1;
        // With several folders of this name (two runs created it at once), every run uses the
        // one with the smallest handle, so they all end up writing to the same folder.
        const auto first_of = [](const std::vector<std::string>& handles) {
            return *std::min_element(handles.begin(), handles.end());
        };
        auto found = children_named(current, part, true);
        if (found.empty() && create) {
            catch_up();  // another run may have made it since the tree was loaded
            found = children_named(current, part, true);
        }
        if (!found.empty()) {
            current = first_of(found);
        } else if (create) {
            const std::string parent = current;
            current = make_folder(parent, part);
            // Another run may have created one too meanwhile: settle on the same one as it will.
            catch_up();
            if (const auto now = children_named(parent, part, true); !now.empty()) current = first_of(now);
        } else {
            return {};
        }
    }
    folder_cache_[dir] = current;
    return current;
}

std::optional<MegaBackend::Node> MegaBackend::find_file(const std::string& name) {
    std::lock_guard lock(mu_);
    const std::size_t slash = name.rfind('/');
    const std::string dir = slash == std::string::npos ? std::string() : name.substr(0, slash);
    const std::string leaf = name.substr(slash == std::string::npos ? 0 : slash + 1);
    for (int attempt = 0; attempt < 2; ++attempt) {
        const std::string parent = folder_handle(dir, false);
        if (!parent.empty()) {
            const auto found = children_named(parent, leaf, false);
            if (!found.empty()) return nodes_.at(found.front());
            // Files a run put into a duplicate of the folder (made before runs settled on one).
            if (!dir.empty()) {
                const std::size_t up = dir.rfind('/');
                const std::string grand = folder_handle(up == std::string::npos ? std::string() : dir.substr(0, up), false);
                const std::string name_of_dir = dir.substr(up == std::string::npos ? 0 : up + 1);
                if (!grand.empty()) {
                    for (const std::string& other : children_named(grand, name_of_dir, true)) {
                        if (other == parent) continue;
                        const auto in_other = children_named(other, leaf, false);
                        if (!in_other.empty()) return nodes_.at(in_other.front());
                    }
                }
            }
        }
        if (attempt == 0) catch_up();  // maybe another computer just added it
    }
    return std::nullopt;
}

bool MegaBackend::exists(const std::string& name) {
    return find_file(name).has_value();
}

bool MegaBackend::exists_now(const std::string& name) {
    // The tree in memory first takes in what others changed since (a removal included).
    catch_up();
    return find_file(name).has_value();
}

std::uint64_t MegaBackend::size(const std::string& name) {
    const auto n = find_file(name);
    if (!n) throw Error(location_ + ": repository file " + name + " is missing");
    return n->size;
}

void MegaBackend::put(const std::string& name, ByteSpan data) {
    const std::size_t slash = name.rfind('/');
    const std::string dir = slash == std::string::npos ? std::string() : name.substr(0, slash);
    const std::string leaf = name.substr(slash == std::string::npos ? 0 : slash + 1);
    const std::string parent = folder_handle(dir, true);
    std::vector<std::string> older;
    {
        std::lock_guard lock(mu_);
        older = children_named(parent, leaf, false);
    }

    std::uint8_t key[16];
    std::uint8_t nonce[8];
    random_bytes(key);
    random_bytes(nonce);
    const Bytes mac = mega::meta_mac(ByteSpan(key, 16), ByteSpan(nonce, 8), data);
    Bytes enc(data.begin(), data.end());
    mega::ctr_crypt(ByteSpan(key, 16), ByteSpan(nonce, 8), 0, enc.data(), enc.size());

    const json u = api({{"a", "u"}, {"s", data.size()}, {"ssl", 2}});
    const std::string upload_url = u.is_object() ? u.value("p", std::string()) : std::string();
    if (upload_url.rfind("https://", 0) != 0) throw Error(location_ + ": MEGA gave no upload address");

    std::string completion;
    for (std::size_t off = 0; off < enc.size() || (off == 0 && enc.empty()); off += kUploadPiece) {
        const std::size_t n = std::min(kUploadPiece, enc.size() - off);
        const http::Response r = cloud::send_with_retry(
            *client_,
            [&] {
                http::Request req;
                req.method = "POST";
                req.url = upload_url + "/" + std::to_string(off);
                req.body = ByteSpan(enc.data() + off, n);
                return req;
            },
            location_ + ": uploading " + name);
        if (r.status != 200) throw Error(location_ + ": uploading " + name + " failed: HTTP " + std::to_string(r.status));
        const std::string text = cloud::body_text(r);
        if (!text.empty() && text[0] == '-')
            throw Error(location_ + ": uploading " + name + " failed: MEGA error " + text);
        if (!text.empty()) completion = text;
        if (enc.empty()) break;
    }
    if (completion.empty()) throw Error(location_ + ": uploading " + name + " did not complete");

    Bytes node_key = mega::pack_file_key(ByteSpan(key, 16), ByteSpan(nonce, 8), mac);
    const std::string attr = mega::encrypt_attributes({{"n", leaf}}, ByteSpan(key, 16));
    secure_wipe(key, sizeof key);
    Aes128(master_).ecb_encrypt(node_key.data(), node_key.size());
    const json r = api({{"a", "p"},
                        {"t", parent},
                        {"n", json::array({{{"h", completion}, {"t", 0}, {"a", attr}, {"k", base64url_encode(node_key)}}})},
                        {"i", request_id()}});
    {
        std::lock_guard lock(mu_);
        if (!r.is_object() || !r.contains("f") || r["f"].empty() || !add_node(r["f"][0]))
            throw Error(location_ + ": saving " + name + " in MEGA failed");
    }
    // The new file is in place: remove the copies it replaces.
    for (const std::string& h : older) {
        const json d = api({{"a", "d"}, {"n", h}, {"i", request_id()}});
        (void)d;
        std::lock_guard lock(mu_);
        drop_node(h);
    }
    throttle_write(data.size());
}

std::string MegaBackend::download_url(const std::string& handle, bool refresh) {
    {
        std::lock_guard lock(mu_);
        auto it = links_.find(handle);
        if (!refresh && it != links_.end() && std::chrono::steady_clock::now() < it->second.until) return it->second.url;
    }
    const json r = api({{"a", "g"}, {"g", 1}, {"n", handle}, {"ssl", 2}});
    if (is_error(r, -9)) return {};
    if (!r.is_object()) throw Error(location_ + ": MEGA refused the download");
    if (r.contains("e") && r["e"].is_number_integer() && r["e"].get<int>() != 0)
        throw Error(location_ + ": MEGA refused the download (" + error_name(r["e"].get<int>()) + ")");
    const std::string url = r.value("g", std::string());
    if (url.rfind("https://", 0) != 0) throw Error(location_ + ": MEGA gave no download address");
    std::lock_guard lock(mu_);
    if (links_.size() > 256) links_.clear();
    links_[handle] = {url, std::chrono::steady_clock::now() + std::chrono::minutes(5)};
    return url;
}

Bytes MegaBackend::download(const std::string& name, std::uint64_t offset, std::size_t length, bool whole) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        const auto node = find_file(name);
        if (!node) throw Error(location_ + ": repository file " + name + " is missing");
        if (whole) {
            offset = 0;
            length = static_cast<std::size_t>(node->size);
        }
        if (length == 0) return {};
        if (offset + length > node->size) throw Error(location_ + ": repository file " + name + " is shorter than expected");
        const std::string url = download_url(node->handle, attempt > 0);
        if (url.empty()) {
            std::lock_guard lock(mu_);
            drop_node(node->handle);
            continue;
        }
        const std::string ranged = url + "/" + std::to_string(offset) + "-" + std::to_string(offset + length - 1);
        http::Response r = cloud::send_with_retry(
            *client_,
            [&] {
                http::Request req;
                req.method = "GET";
                req.url = ranged;
                return req;
            },
            location_ + ": downloading " + name);
        if (r.status == 509)
            throw Error(location_ + ": MEGA's download transfer limit is reached; try again later");
        if (r.status == 403 || r.status == 404 || r.status == 410) continue;  // the address expired
        if (r.status != 200 || r.body.size() != length)
            throw Error(location_ + ": downloading " + name + " failed: HTTP " + std::to_string(r.status));
        mega::ctr_crypt(mega::file_aes_key(node->key), mega::file_nonce(node->key), offset, r.body.data(), r.body.size());
        throttle_read(length);
        return std::move(r.body);
    }
    throw Error(location_ + ": downloading " + name + " failed");
}

Bytes MegaBackend::get(const std::string& name) {
    return download(name, 0, 0, true);
}

Bytes MegaBackend::get_range(const std::string& name, std::uint64_t offset, std::size_t length) {
    if (length == 0) return {};
    return download(name, offset, length, false);
}

std::vector<std::string> MegaBackend::list(const std::string& dir) {
    std::lock_guard lock(mu_);
    catch_up();
    std::vector<std::string> out;
    const std::string start = folder_handle(dir, false);
    if (start.empty()) return out;
    std::set<std::string> seen;
    const std::function<void(const std::string&, const std::string&)> walk = [&](const std::string& handle,
                                                                                  const std::string& prefix) {
        auto it = children_.find(handle);
        if (it == children_.end()) return;
        for (const std::string& h : it->second) {
            const Node& n = nodes_.at(h);
            if (n.type == 1) walk(h, prefix + n.name + "/");
            else if (n.type == 0 && seen.insert(prefix + n.name).second) out.push_back(prefix + n.name);
        }
    };
    walk(start, dir.empty() ? std::string() : dir + "/");
    std::sort(out.begin(), out.end());
    return out;
}

void MegaBackend::remove(const std::string& name) {
    const std::size_t slash = name.rfind('/');
    const std::string dir = slash == std::string::npos ? std::string() : name.substr(0, slash);
    const std::string leaf = name.substr(slash == std::string::npos ? 0 : slash + 1);
    std::vector<std::string> handles;
    {
        std::lock_guard lock(mu_);
        const std::string parent = folder_handle(dir, false);
        if (parent.empty()) return;
        handles = children_named(parent, leaf, false);
    }
    // A real delete, not the rubbish bin, so pruned data stops counting against the quota.
    for (const std::string& h : handles) {
        api({{"a", "d"}, {"n", h}, {"i", request_id()}});
        std::lock_guard lock(mu_);
        drop_node(h);
    }
}

cloud::SignIn MegaBackend::sign_in(const std::string& email_in, const std::string& password, const std::string& code) {
    const std::string email = lower(email_in);
    if (email.empty() || password.empty()) throw Error("enter the MEGA email and password");
    http::Client client;
    std::uint8_t rnd[4];
    random_bytes(rnd);
    std::uint64_t seq = (static_cast<std::uint64_t>(rnd[0]) << 24) | (rnd[1] << 16) | (rnd[2] << 8) | rnd[3];

    const json pre = mega_request(client, seq, {}, {{"a", "us0"}, {"user", email}}, "signing in to MEGA");
    if (!pre.is_object()) throw Error("signing in to MEGA: no such account");
    const int version = pre.value("v", 0);
    Bytes password_key;
    std::string user_hash;
    if (version == 1) {
        password_key = mega::prepare_key(password);
        user_hash = mega::stringhash(email, password_key);
    } else if (version == 2) {
        const Bytes salt = base64_decode(pre.value("s", std::string()));
        Bytes derived = pbkdf2_sha512(password, salt, 100000, 32);
        password_key.assign(derived.begin(), derived.begin() + 16);
        user_hash = base64url_encode(ByteSpan(derived.data() + 16, 16));
        secure_wipe(derived.data(), derived.size());
    } else {
        throw Error("signing in to MEGA: unsupported account version " + std::to_string(version));
    }

    json login = {{"a", "us"}, {"user", email}, {"uh", user_hash}};
    if (!code.empty()) login["mfa"] = code;
    const json r = mega_request(client, seq, {}, login, "signing in to MEGA");
    if (!r.is_object()) throw Error("signing in to MEGA: wrong email or password");

    Bytes master = base64_decode(r.value("k", std::string()));
    if (master.size() != 16) throw Error("signing in to MEGA: unexpected account key");
    Aes128(password_key).ecb_decrypt(master.data(), master.size());
    secure_wipe(password_key.data(), password_key.size());

    std::string sid;
    if (r.contains("csid")) {
        Bytes privk = base64_decode(r.value("privk", std::string()));
        if (privk.empty() || privk.size() % 16 != 0) throw Error("signing in to MEGA: unexpected private key");
        Aes128(master).ecb_decrypt(privk.data(), privk.size());
        std::size_t pos = 0;
        const Bytes p = mega::read_mpi(privk, pos);
        const Bytes q = mega::read_mpi(privk, pos);
        const Bytes d = mega::read_mpi(privk, pos);
        // n = p * q
        const Limbs lp = from_be(p);
        const Limbs lq = from_be(q);
        Limbs n(lp.size() + lq.size(), 0);
        for (std::size_t i = 0; i < lp.size(); ++i) {
            std::uint64_t c = 0;
            for (std::size_t j = 0; j < lq.size(); ++j) {
                const std::uint64_t s = static_cast<std::uint64_t>(n[i + j]) + static_cast<std::uint64_t>(lp[i]) * lq[j] + c;
                n[i + j] = static_cast<std::uint32_t>(s);
                c = s >> 32;
            }
            n[i + lq.size()] = static_cast<std::uint32_t>(c);
        }
        const Bytes csid = base64_decode(r.value("csid", std::string()));
        std::size_t cpos = 0;
        const Bytes c = mega::read_mpi(csid, cpos);
        const Bytes m = mega::modexp(c, d, to_be(n));
        secure_wipe(privk.data(), privk.size());
        if (m.size() < 43) throw Error("signing in to MEGA: unexpected session data");
        sid = base64url_encode(ByteSpan(m.data(), 43));
    } else if (r.contains("tsid")) {
        sid = r.value("tsid", std::string());
    } else {
        throw Error("signing in to MEGA: no session came back");
    }
    std::string user = r.value("u", std::string());
    if (user.empty()) {
        // Older answers leave out the handle; ask for it.
        const json ug = mega_request(client, seq, sid, {{"a", "ug"}}, "reading the MEGA account");
        if (ug.is_object()) user = ug.value("u", std::string());
    }
    if (user.empty()) throw Error("signing in to MEGA: the account handle is unknown");

    cloud::SignIn out;
    out.account = email;
    out.secret = json{{"sid", sid}, {"user", user}, {"master_key", base64url_encode(master)}}.dump();
    secure_wipe(master.data(), master.size());
    return out;
}

}  // namespace vaultik
