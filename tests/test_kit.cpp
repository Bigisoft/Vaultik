#include <doctest/doctest.h>

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include "vaultik/jobs/recovery_kit.hpp"
#include "vaultik/util/error.hpp"
#include "vaultik/platform/os.hpp"
#include "vaultik/util/qr.hpp"
#include "vaultik/util/ttf.hpp"

using namespace vaultik;

namespace {

// The modules of a code as hex, row by row, padded to whole bytes (as the reference vectors are).
std::string module_hex(const qr::Code& c) {
    std::string out;
    int bits = 0, value = 0;
    const auto flush = [&] {
        static const char* kDigits = "0123456789abcdef";
        out += kDigits[(value >> 4) & 15];
        out += kDigits[value & 15];
        bits = value = 0;
    };
    for (int y = 0; y < c.size; ++y)
        for (int x = 0; x < c.size; ++x) {
            value = value << 1 | (c.dark(x, y) ? 1 : 0);
            if (++bits == 8) flush();
        }
    if (bits > 0) {
        value <<= 8 - bits;
        flush();
    }
    return out;
}

// ---- an independent reader, to prove that what is encoded reads back -------------------------

std::uint8_t gf_mul(std::uint8_t x, std::uint8_t y) {
    int z = 0;
    for (int i = 7; i >= 0; --i) {
        z = (z << 1) ^ ((z >> 7) * 0x11D);
        z ^= ((y >> i) & 1) * x;
    }
    return static_cast<std::uint8_t>(z);
}

// Version 1 to 10 only: the centres of the alignment patterns.
std::vector<int> alignment(int version) {
    static const std::vector<std::vector<int>> kTable = {
        {}, {}, {6, 18}, {6, 22}, {6, 26}, {6, 30}, {6, 34}, {6, 22, 38}, {6, 24, 42}, {6, 26, 46}, {6, 28, 50}};
    return kTable[static_cast<std::size_t>(version)];
}

bool reserved(int version, int size, int x, int y) {
    if ((x < 9 && y < 9) || (x >= size - 8 && y < 9) || (x < 9 && y >= size - 8)) return true;  // finders, format
    if (x == 6 || y == 6) return true;                                                          // timing
    if (version >= 7 && ((x >= size - 11 && x < size - 8 && y < 6) || (y >= size - 11 && y < size - 8 && x < 6)))
        return true;  // version information
    const std::vector<int> pos = alignment(version);
    for (std::size_t i = 0; i < pos.size(); ++i)
        for (std::size_t j = 0; j < pos.size(); ++j) {
            if ((i == 0 && j == 0) || (i == 0 && j == pos.size() - 1) || (i == pos.size() - 1 && j == 0)) continue;
            if (std::abs(x - pos[i]) <= 2 && std::abs(y - pos[j]) <= 2) return true;
        }
    return false;
}

struct Decoded {
    int ecc_bits = -1;
    int mask = -1;
    std::string text;
    bool codes_valid = false;
};

// Reads a version 1-10 code: its format, its codewords (checking each block's Reed-Solomon
// remainder) and the byte mode text. Blocks and their sizes come from the standard's table for
// the levels used here.
Decoded read_code(const qr::Code& c, int blocks, int ecc_per_block) {
    Decoded d;
    const int size = c.size;
    int format = 0;
    // The copy around the top left finder, bit 14 first.
    const std::vector<std::pair<int, int>> cells = {{0, 8}, {1, 8}, {2, 8}, {3, 8}, {4, 8}, {5, 8}, {7, 8}, {8, 8},
                                                    {8, 7}, {8, 5}, {8, 4}, {8, 3}, {8, 2}, {8, 1}, {8, 0}};
    for (const auto& [x, y] : cells) format = format << 1 | (c.dark(x, y) ? 1 : 0);
    format ^= 0x5412;
    d.ecc_bits = format >> 13;
    d.mask = (format >> 10) & 7;

    std::vector<int> bits;
    for (int right = size - 1; right >= 1; right -= 2) {
        if (right == 6) right = 5;
        for (int vert = 0; vert < size; ++vert)
            for (int j = 0; j < 2; ++j) {
                const int x = right - j;
                const bool upward = ((right + 1) & 2) == 0;
                const int y = upward ? size - 1 - vert : vert;
                if (reserved(c.version, size, x, y)) continue;
                bool m = c.dark(x, y);
                bool inv = false;
                switch (d.mask) {
                    case 0: inv = (x + y) % 2 == 0; break;
                    case 1: inv = y % 2 == 0; break;
                    case 2: inv = x % 3 == 0; break;
                    case 3: inv = (x + y) % 3 == 0; break;
                    case 4: inv = (x / 3 + y / 2) % 2 == 0; break;
                    case 5: inv = x * y % 2 + x * y % 3 == 0; break;
                    case 6: inv = (x * y % 2 + x * y % 3) % 2 == 0; break;
                    default: inv = ((x + y) % 2 + x * y % 3) % 2 == 0; break;
                }
                bits.push_back(m != inv ? 1 : 0);
            }
    }
    std::vector<std::uint8_t> words(bits.size() / 8, 0);
    for (std::size_t i = 0; i < words.size() * 8; ++i) words[i / 8] = static_cast<std::uint8_t>(words[i / 8] << 1 | bits[i]);

    // De-interleave: short blocks first, each data part then each ecc part column by column.
    const int total = static_cast<int>(words.size());
    const int short_len = total / blocks;
    const int short_blocks = blocks - total % blocks;
    std::vector<std::vector<std::uint8_t>> parts(static_cast<std::size_t>(blocks));
    int k = 0;
    const int max_data = short_len - ecc_per_block + 1;
    for (int i = 0; i < max_data; ++i)
        for (int b = 0; b < blocks; ++b) {
            const int data_len = short_len - ecc_per_block + (b < short_blocks ? 0 : 1);
            if (i < data_len) parts[static_cast<std::size_t>(b)].push_back(words[static_cast<std::size_t>(k++)]);
        }
    for (int i = 0; i < ecc_per_block; ++i)
        for (int b = 0; b < blocks; ++b) parts[static_cast<std::size_t>(b)].push_back(words[static_cast<std::size_t>(k++)]);

    // Every block is a code word: its polynomial is zero at alpha^0 .. alpha^(ecc-1).
    d.codes_valid = true;
    std::vector<std::uint8_t> data;
    for (int b = 0; b < blocks; ++b) {
        const auto& part = parts[static_cast<std::size_t>(b)];
        std::uint8_t alpha = 1;
        for (int i = 0; i < ecc_per_block; ++i) {
            std::uint8_t sum = 0;
            for (const std::uint8_t w : part) sum = static_cast<std::uint8_t>(gf_mul(sum, alpha) ^ w);
            if (sum != 0) d.codes_valid = false;
            alpha = gf_mul(alpha, 2);
        }
        data.insert(data.end(), part.begin(), part.end() - ecc_per_block);
    }

    // Byte mode: 0100, the count (8 bits up to version 9), the bytes.
    std::size_t bit = 0;
    const auto take = [&](int n) {
        int v = 0;
        for (int i = 0; i < n; ++i, ++bit) v = v << 1 | ((data[bit / 8] >> (7 - bit % 8)) & 1);
        return v;
    };
    if (take(4) != 4) return d;
    const int count = take(c.version <= 9 ? 8 : 16);
    for (int i = 0; i < count; ++i) d.text += static_cast<char>(take(8));
    return d;
}

std::string bytes_text(const Bytes& b) {
    return std::string(b.begin(), b.end());
}

}  // namespace

TEST_CASE("the QR encoder matches reference codes") {
    // Made with segno 1.6.6 (an independent encoder) with the same version, level and mask, its
    // padding corrected: it adds a zero byte where the data already ends on a byte boundary, which
    // ISO/IEC 18004 (7.4.10) does not, and which byte mode with a full terminator always does.
    const std::string key = "ABCD-EFGH-JKMN-PQRS-TVWX-YZ01-2345-6789";
    qr::Code c = qr::encode_text(key, qr::Ecc::Q, 1, 3);
    CHECK(c.version == 4);
    CHECK(module_hex(c) == "fe360c3fc170e7d06eb34e2bb74ae425dba4ce62ec12172107faaaaafe002fa100765c2e03601da40192cfc86bf79b1b6f8f255190bb250672363bfa6271688d4cdeb3daca60821f526cc49aa32110992b4a107e47341ec82c6f800b3fe65ef9b11591bbcb9efd005803c4ff83252a105272317ba03c3ffdd46e9a4eebf0b48905a155e9fe54785600");

    c = qr::encode_text("Hello, world!", qr::Ecc::M, 1, 0);
    CHECK(c.version == 1);
    CHECK(module_hex(c) == "fe6bfc16506e96bb7435dbaaaec13507faafe00700aa1096e484ca837f40422e6688004ddbf832f04620babd15d1edaea62b04612feb1180");

    Bytes data(100);
    for (std::size_t i = 0; i < data.size(); ++i) data[i] = static_cast<std::uint8_t>(i);
    c = qr::encode(data, qr::Ecc::L, 7, 5);
    CHECK(c.version == 7);
    CHECK(module_hex(c) == "fe2be0678bfc10a855be906e825ea554bb752d65e1b5dbac84f9d3aec113d46d0107faaaaaaaafe0094d15a200c7172f9678c5a0db1d19515bd16429a669f87b03e416def63e9da6ebea6ca0b96edbfb87625f914337d160b6168fb859503f3e25e184944937ceb712cfcf1284b00a16edfbd2fc81fe1c4dec52f4746b18eb29ead3154f15a9151fe9efd9efe6254fb8182918a770882561639a61b55fdc2cd2a37e76225ca0b968331a1e5be91ea21480d5681613c84df20a3d22b5e5950d42e8930c2710f320d90a1ac9bdbffa59fa805ca4468473fa9aab296a50542f187f15ba1f3f967fbdd04f4c5cf36e9b40972afb05db39e4e3cfeeca537e0700");

    data.resize(300);
    for (std::size_t i = 0; i < data.size(); ++i) data[i] = static_cast<std::uint8_t>((i * 7 + 3) % 256);
    c = qr::encode(data, qr::Ecc::H, 1, 6);
    CHECK(c.version == 18);
    CHECK(module_hex(c) == "fe4af89e817847b2eeefbfc1160c715f251e245a26906ea363f128e8ff28f0090bb752618be31056a0eeeed5dba68f87fc9d43fe90bd12ec11241246d4d5513fb5c907faaaaaaaaaaaaaaaaaaafe002416f110b2e44e62c4001b6c3e5fda610fff94e88672a516abab97f44f75555bac8e036df8228154615f7ee1bf2be35f1f3be0b5c959286161bb7258cf8744455a326e32a89002057d6c18ea738c365c7ff0e6bfe4615d8213fc24ced0c2f4102133e4ebd89fc31c36f05d0ac1b14aabc011614769ce9f2b8a288730b71ab6acd5ab23f00a2241c26149f98d3bf1a36ee24e32e594152d258343940008600b179d1e800022486c3e428f8c2d244cbf5cf861e22d398bf5d7516ea53d47177fd329aaeb5eeeef7fd885d611399c850a7a17430689dbc28c53e5770adb29f58b273b20c70bec97456c65fc507ffc25a33e3e527f83c49ce247240fb19c85ec532b5a46eaa332baf45c96a55137a631186c74622e0b191fde725fe01117e8a39ff8180c8cc9121afe52cff78b9ebaf5ecdec308119f3960dab9b459d25318e8c370c8fa36a07f6f7b6832532ab78bc0381b761691fde0f6c80a893d899b55873514671b61b83442b5e1f013d0e7cd7d1a78c77de4e2cc2c5d85e0e820b011f778e5844009562ab3114ff3c0b14a09e3e13bbe707efaa8ad7f3fcb003caec2b5955453e0345f17df65f4bc5c5a328d688a2765faabb480294fd3d18246343a2fca9b8b968a196852f0bea186cd5dea0eeff8f859363298536fd6a4c7db256a8bcbf85a56146b45f5d1215bb0ed5863913e8874296b3839ae371326abe727d48574e20ed070d3ff1f30e5cd833f0077feee5b2faf04d7ee31cbbc91f0c084618de71cdac1e9aaa57e4ab9e2dbad6c4d2ef45d528f19ddedc447e92797f60c33ffd6a9fe29287aaa7db1627f0c50d7a5f8c1dde46e498a2a227e02e8bef6cc25cd820ef4077b8b6688677455f4d1f0cd6b20bfa607844fb11edfc0c88261fa9b32e554b84f937e003974bedc17110a71c683adb236e86fe7712e18901f960d2f1e1ac90a44814b156e73286da7c7dd11e0d61e41c67326b250656188aaa1f57d0c888aed8a61a07393df2b0a3d6b6c07b6ff18d28533a99663fc45a7f99988bcd2f94c3e7b6c8239f0bd346cceea681cf56d33f5a62bf4a969a9e7c2bc0f4657566c64366a844f72a64935837c0b7e4a12fe98e9924b0fea48be73bd5717c889018e059bdab5206c33897d41e785936e33efe32157edb7cafb8072bac4526251133331c5ffbf019aa95f46ace9d7aa904780091f80d1c6e09c11dbadbf1bfeced4be67777fddd5f2f8ea576c2ce85ea2a2e8d907b04b5934f396ab8704a06420bd2b6b257464cdfe1c499fe686929e38e27e00");
}

TEST_CASE("a QR code reads back to what was encoded") {
    const std::string key = "Q7RW-2KMX-9TCF-HB4N-ZP8D-6YVJ-3SAE-WG5L";
    // Level Q: version 4 has 2 blocks of 26 error correction codewords each.
    for (int mask = -1; mask < 8; ++mask) {
        const qr::Code c = qr::encode_text(key, qr::Ecc::Q, 1, mask);
        REQUIRE(c.version == 4);
        REQUIRE(c.size == 33);
        const Decoded d = read_code(c, 2, 26);
        CHECK(d.ecc_bits == 3);  // Q
        CHECK(d.mask == c.mask);
        CHECK(d.codes_valid);
        CHECK(d.text == key);
    }
    // Level M, version 1 (one block of 10).
    const qr::Code small = qr::encode_text("vaultik", qr::Ecc::M);
    REQUIRE(small.version == 1);
    const Decoded d = read_code(small, 1, 10);
    CHECK(d.ecc_bits == 0);
    CHECK(d.codes_valid);
    CHECK(d.text == "vaultik");

    CHECK(qr::byte_capacity(4, qr::Ecc::Q) == 46);
    CHECK(qr::byte_capacity(40, qr::Ecc::L) == 2953);
    CHECK_THROWS_AS(qr::encode(Bytes(3000, 1), qr::Ecc::L), Error);
}

TEST_CASE("a location for the recovery kit leaves out secrets") {
    CHECK(jobs::public_location("E:\\Backups\\vaultik") == "E:\\Backups\\vaultik");
    CHECK(jobs::public_location("s3:https://s3.example.com/bucket/pc") == "s3:https://s3.example.com/bucket/pc");
    CHECK(jobs::public_location("webdav:https://me:secret@dav.example.com/backup") ==
          "webdav:https://me@dav.example.com/backup");
    CHECK(jobs::public_location("sftp:me:pw@nas:22/backups") == "sftp:me@nas:22/backups");
    CHECK(jobs::public_location("s3:https://s3.example.com/bucket?X-Amz-Signature=abc") == "s3:https://s3.example.com/bucket");
    CHECK(jobs::location_kind("s3:https://s3.example.com/bucket").first == "S3-compatible cloud storage");
    CHECK(jobs::location_kind("onedrive:Backups/pc").second == "OneDrive");
    CHECK(jobs::location_kind("\\\\nas\\share\\vaultik").first == "a folder on a network share");
}

TEST_CASE("the recovery kit holds the key, the place and the steps, in a page and a PDF") {
    jobs::RecoveryKit kit;
    kit.key = "Q7RW-2KMX-9TCF-HB4N-ZP8D-6YVJ-3SAE-WG5L";
    kit.location = "E:\\Backups\\<mine> & co";
    kit.kind = "a folder on an external drive (%1 when it was made)";
    kit.kind_arg = "E:\\";
    kit.computer = "DESKTOP-1";
    kit.made = 133'000'000'000'000'000ull;
    kit.version = "9.9.9";

    const std::string html = jobs::render_kit_html(kit);
    CHECK(html.find(kit.key) != std::string::npos);
    CHECK(html.find("E:\\Backups\\&lt;mine&gt; &amp; co") != std::string::npos);  // escaped
    CHECK(html.find("<mine>") == std::string::npos);
    CHECK(html.find("a folder on an external drive (E:\\ when it was made)") != std::string::npos);
    CHECK(html.find("DESKTOP-1") != std::string::npos);
    CHECK(html.find("<svg") != std::string::npos);
    CHECK(html.find("<script") == std::string::npos);
    CHECK(html.find("Restore from an existing backup") != std::string::npos);

    // Translated phrases, and the frontend's own image in place of the SVG.
    const std::string bg = jobs::render_kit_html(
        kit, [](const std::string& p) { return p == "Recovery key" ? std::string("KEY-WORD") : p; }, "<img src=\"qr\">");
    CHECK(bg.find("KEY-WORD") != std::string::npos);
    CHECK(bg.find("<img src=\"qr\">") != std::string::npos);
    CHECK(bg.find("<svg") == std::string::npos);

    const std::string pdf = bytes_text(jobs::render_kit_pdf(kit));
    CHECK(pdf.starts_with("%PDF-1.4"));
    CHECK(pdf.find("%%EOF") != std::string::npos);
    CHECK(pdf.find("(Q7RW-2KMX-9TCF-HB4N-)") != std::string::npos);
    CHECK(pdf.find("(ZP8D-6YVJ-3SAE-WG5L)") != std::string::npos);
    CHECK(pdf.find("E:\\\\Backups") != std::string::npos);  // a backslash is escaped in a PDF string
    // The cross-reference table points at each object.
    const std::size_t xref = pdf.find("\nxref\n");
    REQUIRE(xref != std::string::npos);
    const std::size_t start = pdf.find("startxref\n");
    CHECK(std::stoul(pdf.substr(start + 10)) == xref + 1);
    const std::size_t first = pdf.find("0000000000 65535 f \n", xref);
    REQUIRE(first != std::string::npos);
    const std::size_t obj1 = std::stoul(pdf.substr(first + 20, 10));
    CHECK(pdf.compare(obj1, 7, "1 0 obj") == 0);
}

namespace {

// The first font of this system that has `c`, or nullopt.
std::optional<ttf::Font> system_font_with(char32_t c) {
    for (const std::string& path : ttf::system_font_files()) {
        try {
            if (auto f = ttf::Font::parse(os::read_file(path, 48u << 20)); f && f->glyph(c)) return f;
        } catch (const std::exception&) {
        }
    }
    return std::nullopt;
}

}  // namespace

TEST_CASE("a TrueType subset keeps the glyphs used and reads back as the same font") {
    CHECK_FALSE(ttf::Font::parse(Bytes{1, 2, 3}));
    CHECK(ttf::Font::count(Bytes(20, 0)) == 0);
    const auto font = system_font_with(U'\u0416');  // Cyrillic Zhe
    if (!font) {
        MESSAGE("no TrueType font with Cyrillic letters on this system; skipped");
        return;
    }
    const std::uint16_t zhe = font->glyph(U'\u0416'), a = font->glyph(U'A');
    CHECK(zhe != 0);
    CHECK(a != 0);
    CHECK(font->glyph(U'\U0010FFFD') == 0);
    CHECK(font->advance(zhe) > 0);

    const Bytes sub = font->subset({zhe});
    const auto back = ttf::Font::parse(sub);
    REQUIRE(back);
    CHECK(back->glyph_count() == zhe + 1);  // glyph numbers stay; the ones after are left out
    CHECK(back->advance(zhe) == font->advance(zhe));
    CHECK(back->ascent() == font->ascent());
    // No character map in the subset (a PDF maps glyph numbers itself), so nothing is looked up.
    CHECK(back->glyph(U'A') == 0);
    CHECK(sub.size() < 400u * 1024);
    // The whole subset sums to the magic number, as a font's head table promises.
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < sub.size(); i += 4)
        sum += (std::uint32_t{sub[i]} << 24) | (std::uint32_t{sub[i + 1]} << 16) | (std::uint32_t{sub[i + 2]} << 8) | sub[i + 3];
    CHECK(sum == 0xB1B0AFBAu);
}

TEST_CASE("the recovery kit PDF draws letters beyond Western European ones with a system font") {
    jobs::RecoveryKit kit;
    kit.key = "Q7RW-2KMX-9TCF-HB4N-ZP8D-6YVJ-3SAE-WG5L";
    kit.location = "D:\\\u0410\u0440\u0445\u0438\u0432 (caf\u00e9)";  // "Arkhiv" in Cyrillic
    kit.kind = "a folder on an external drive (%1 when it was made)";
    kit.kind_arg = "D:\\";
    kit.computer = "\u6771\u4eac-PC";  // "Tokyo" in Chinese characters
    kit.version = "9.9.9";
    const std::string pdf = bytes_text(jobs::render_kit_pdf(kit));
    CHECK(pdf.find("caf\xe9") != std::string::npos);  // Latin-1 letters stay in the built-in font

    // Every cross-reference entry points at its object.
    const std::size_t xref = pdf.find("\nxref\n");
    REQUIRE(xref != std::string::npos);
    const std::size_t count = std::stoul(pdf.substr(xref + 8));
    const std::size_t table = pdf.find("0000000000 65535 f \n", xref) + 20;
    for (std::size_t i = 1; i < count; ++i) {
        const std::size_t at = std::stoul(pdf.substr(table + 20 * (i - 1), 10));
        CHECK(pdf.compare(at, std::to_string(i).size() + 6, std::to_string(i) + " 0 obj") == 0);
    }

    if (!system_font_with(U'\u0410')) {
        MESSAGE("no TrueType font with Cyrillic letters on this system; the PDF shows '?'");
        CHECK(pdf.find("D:\\\\????? (caf") != std::string::npos);
        return;
    }
    CHECK(pdf.find("/FontFile2") != std::string::npos);
    CHECK(pdf.find("/Encoding /Identity-H") != std::string::npos);
    CHECK(pdf.find("<0410>") != std::string::npos);  // the copy-out map names the letter
    if (system_font_with(U'\u6771')) CHECK(pdf.find("<6771>") != std::string::npos);
}
