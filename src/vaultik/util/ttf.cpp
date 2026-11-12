#include "vaultik/util/ttf.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <utility>

#ifndef _WIN32
#include <filesystem>
#include <system_error>
#endif

#include "vaultik/platform/os.hpp"

namespace vaultik::ttf {

namespace {

// Thrown by the readers below on data that runs short; parse() turns it into nullopt.
struct Damaged {};

std::uint32_t be(ByteSpan d, std::size_t at, int n) {
    if (at > d.size() || static_cast<std::size_t>(n) > d.size() - at) throw Damaged{};
    std::uint32_t v = 0;
    for (int i = 0; i < n; ++i) v = (v << 8) | d[at + static_cast<std::size_t>(i)];
    return v;
}
std::uint16_t u16(ByteSpan d, std::size_t at) {
    return static_cast<std::uint16_t>(be(d, at, 2));
}
std::int16_t s16(ByteSpan d, std::size_t at) {
    return static_cast<std::int16_t>(be(d, at, 2));
}
std::uint32_t u32(ByteSpan d, std::size_t at) {
    return be(d, at, 4);
}

void put16(Bytes& b, std::uint32_t v) {
    b.push_back(static_cast<std::uint8_t>(v >> 8));
    b.push_back(static_cast<std::uint8_t>(v));
}
void put32(Bytes& b, std::uint32_t v) {
    put16(b, v >> 16);
    put16(b, v & 0xFFFF);
}
void set32(Bytes& b, std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (24 - 8 * i));
}

std::uint32_t checksum(ByteSpan d) {
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < d.size(); i += 4) {
        std::uint32_t v = 0;
        for (std::size_t k = 0; k < 4; ++k) v = (v << 8) | (i + k < d.size() ? d[i + k] : 0);
        sum += v;
    }
    return sum;
}

// The offset of font `index`'s table directory, or nullopt.
std::optional<std::uint32_t> font_offset(ByteSpan d, unsigned index) {
    if (d.size() < 12) return std::nullopt;
    const std::uint32_t tag = u32(d, 0);
    if (tag == 0x74746366) {  // 'ttcf'
        const std::uint32_t n = u32(d, 8);
        if (index >= n) return std::nullopt;
        return u32(d, 12 + 4 * static_cast<std::size_t>(index));
    }
    if (index != 0) return std::nullopt;
    if (tag == 0x00010000 || tag == 0x74727565) return 0u;  // TrueType, 'true'
    return std::nullopt;  // 'OTTO' (CFF outlines) and anything else
}

// The glyphs that composite glyph data names as its parts.
std::vector<std::uint16_t> components(ByteSpan g) {
    std::vector<std::uint16_t> out;
    if (g.size() < 10 || s16(g, 0) >= 0) return out;
    std::size_t at = 10;
    for (;;) {
        const std::uint16_t flags = u16(g, at);
        out.push_back(u16(g, at + 2));
        at += 4 + ((flags & 0x0001) ? 4 : 2);
        if (flags & 0x0008) at += 2;
        else if (flags & 0x0040) at += 4;
        else if (flags & 0x0080) at += 8;
        if (!(flags & 0x0020)) break;
    }
    return out;
}

}  // namespace

unsigned Font::count(ByteSpan data) {
    try {
        if (data.size() >= 12 && u32(data, 0) == 0x74746366) return u32(data, 8);
        return font_offset(data, 0) ? 1 : 0;
    } catch (const Damaged&) {
        return 0;
    }
}

const Font::Table* Font::table(const char* tag) const {
    for (const auto& [t, table] : tables_)
        if (t == tag) return &table;
    return nullptr;
}

std::optional<Font> Font::parse(Bytes data, unsigned index) {
    try {
        Font f;
        f.data_ = std::move(data);
        const ByteSpan d = f.data_;
        const auto dir = font_offset(d, index);
        if (!dir) return std::nullopt;
        if (u32(d, *dir) != 0x00010000 && u32(d, *dir) != 0x74727565) return std::nullopt;
        const std::uint16_t n = u16(d, *dir + 4);
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t rec = *dir + 12 + 16 * i;
            Table t{u32(d, rec + 8), u32(d, rec + 12)};
            if (t.offset > d.size() || t.length > d.size() - t.offset) return std::nullopt;
            f.tables_.emplace_back(std::string(reinterpret_cast<const char*>(&d[rec]), 4), t);
        }
        const Table* head = f.table("head");
        const Table* hhea = f.table("hhea");
        const Table* maxp = f.table("maxp");
        const Table* cmap = f.table("cmap");
        if (!head || !hhea || !maxp || !f.table("hmtx") || !f.table("loca") || !f.table("glyf"))
            return std::nullopt;
        f.em_ = u16(d, head->offset + 18);
        if (f.em_ < 16) return std::nullopt;
        for (int i = 0; i < 4; ++i) f.bbox_[i] = s16(d, head->offset + 36 + 2 * static_cast<std::size_t>(i));
        f.long_loca_ = s16(d, head->offset + 50) == 1;
        f.ascent_ = s16(d, hhea->offset + 4);
        f.descent_ = s16(d, hhea->offset + 6);
        f.metrics_ = u16(d, hhea->offset + 34);
        f.glyphs_ = u16(d, maxp->offset + 4);
        if (f.metrics_ < 1 || f.glyphs_ < 1) return std::nullopt;
        if (f.table("hmtx")->length < 4u * static_cast<unsigned>(f.metrics_)) return std::nullopt;
        if (f.table("loca")->length < (f.long_loca_ ? 4u : 2u) * (static_cast<unsigned>(f.glyphs_) + 1)) return std::nullopt;

        // The best character map: full Unicode (format 12), else the Basic Multilingual Plane (4).
        // Without one (as in a subset) no character has a glyph.
        int best = 0;
        const std::uint16_t maps = cmap ? u16(d, cmap->offset + 2) : 0;
        for (std::size_t i = 0; i < maps; ++i) {
            const std::size_t rec = cmap->offset + 4 + 8 * i;
            const std::uint16_t platform = u16(d, rec), encoding = u16(d, rec + 2);
            const std::uint32_t at = cmap->offset + u32(d, rec + 4);
            const bool unicode = platform == 0 || (platform == 3 && (encoding == 1 || encoding == 10));
            if (!unicode) continue;
            const std::uint16_t format = u16(d, at);
            const int rank = format == 12 ? 2 : format == 4 ? 1 : 0;
            if (rank > best) {
                best = rank;
                f.cmap_ = at;
                f.cmap_format_ = format;
            }
        }

        // The PostScript name (name 6), for the PDF's font name.
        if (const Table* name = f.table("name")) {
            const std::uint16_t count = u16(d, name->offset + 2), strings = u16(d, name->offset + 4);
            for (std::size_t i = 0; i < count; ++i) {
                const std::size_t rec = name->offset + 6 + 12 * i;
                if (u16(d, rec + 6) != 6) continue;
                const std::uint16_t platform = u16(d, rec), len = u16(d, rec + 8);
                const std::size_t at = name->offset + strings + u16(d, rec + 10);
                std::string s;
                const std::size_t step = platform == 1 ? 1 : 2;
                for (std::size_t k = step - 1; k < len; k += step) {
                    const char c = static_cast<char>(be(d, at + k, 1));
                    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') s += c;
                }
                if (!s.empty()) {
                    f.name_ = s.substr(0, 60);
                    break;
                }
            }
        }
        return f;
    } catch (const Damaged&) {
        return std::nullopt;
    }
}

std::uint16_t Font::glyph(char32_t c) const {
    const ByteSpan d = data_;
    if (!cmap_format_) return 0;
    try {
        if (cmap_format_ == 12) {
            const std::uint32_t groups = u32(d, cmap_ + 12);
            std::uint32_t lo = 0, hi = groups;
            while (lo < hi) {  // groups are sorted by their first character
                const std::uint32_t mid = (lo + hi) / 2;
                const std::size_t g = cmap_ + 16 + 12 * static_cast<std::size_t>(mid);
                const std::uint32_t start = u32(d, g), end = u32(d, g + 4);
                if (c < start) hi = mid;
                else if (c > end) lo = mid + 1;
                else {
                    const std::uint32_t id = u32(d, g + 8) + (c - start);
                    return id < static_cast<std::uint32_t>(glyphs_) ? static_cast<std::uint16_t>(id) : 0;
                }
            }
            return 0;
        }
        if (c > 0xFFFF) return 0;
        const std::size_t segs = u16(d, cmap_ + 6) / 2;
        const std::size_t ends = cmap_ + 14, starts = ends + 2 * segs + 2, deltas = starts + 2 * segs,
                          ranges = deltas + 2 * segs;
        for (std::size_t i = 0; i < segs; ++i) {
            if (u16(d, ends + 2 * i) < c) continue;
            const std::uint16_t start = u16(d, starts + 2 * i);
            if (start > c) return 0;
            const std::uint16_t delta = u16(d, deltas + 2 * i), range = u16(d, ranges + 2 * i);
            std::uint32_t id;
            if (range == 0) {
                id = (c + delta) & 0xFFFF;
            } else {
                id = u16(d, ranges + 2 * i + range + 2 * (c - start));
                if (id) id = (id + delta) & 0xFFFF;
            }
            return id < static_cast<std::uint32_t>(glyphs_) ? static_cast<std::uint16_t>(id) : 0;
        }
    } catch (const Damaged&) {
    }
    return 0;
}

int Font::advance(std::uint16_t glyph) const {
    const int i = std::min<int>(glyph, metrics_ - 1);
    return scale(u16(data_, table("hmtx")->offset + 4 * static_cast<std::size_t>(i)));
}

std::uint32_t Font::glyph_offset(std::uint32_t glyph) const {
    const Table* loca = table("loca");
    return long_loca_ ? u32(data_, loca->offset + 4 * static_cast<std::size_t>(glyph))
                      : 2u * u16(data_, loca->offset + 2 * static_cast<std::size_t>(glyph));
}

Bytes Font::subset(const std::set<std::uint16_t>& wanted) const {
    const ByteSpan d = data_;
    const Table* glyf = table("glyf");
    const auto glyph_data = [&](std::uint32_t g) -> ByteSpan {
        try {
            const std::uint32_t from = glyph_offset(g), to = glyph_offset(g + 1);
            if (to <= from || to > glyf->length) return {};
            return d.subspan(glyf->offset + from, to - from);
        } catch (const Damaged&) {
            return {};
        }
    };

    // Glyph 0 and the wanted glyphs, with every part of the composite ones.
    std::set<std::uint16_t> keep = {0};
    std::vector<std::uint16_t> todo(wanted.begin(), wanted.end());
    while (!todo.empty()) {
        const std::uint16_t g = todo.back();
        todo.pop_back();
        if (g >= glyphs_ || !keep.insert(g).second) continue;
        try {
            for (const std::uint16_t part : components(glyph_data(g))) todo.push_back(part);
        } catch (const Damaged&) {
        }
    }

    // Glyphs after the last one kept are left out altogether (their numbers are never used).
    const int count = *keep.rbegin() + 1;
    Bytes new_glyf;
    std::vector<std::uint32_t> offsets;
    for (int g = 0; g < count; ++g) {
        offsets.push_back(static_cast<std::uint32_t>(new_glyf.size()));
        if (!keep.count(static_cast<std::uint16_t>(g))) continue;
        const ByteSpan bytes = glyph_data(static_cast<std::uint32_t>(g));
        new_glyf.insert(new_glyf.end(), bytes.begin(), bytes.end());
        while (new_glyf.size() % 4) new_glyf.push_back(0);
    }
    offsets.push_back(static_cast<std::uint32_t>(new_glyf.size()));
    // Short offsets (half the offset, two bytes each) while they fit: a font with tens of thousands
    // of glyphs then needs half the room.
    const bool short_loca = new_glyf.size() <= 0x1FFFE;
    Bytes new_loca;
    for (const std::uint32_t o : offsets) short_loca ? put16(new_loca, o / 2) : put32(new_loca, o);

    // The tables a PDF reader uses for a TrueType font, in tag order (as the directory must be).
    std::vector<std::pair<std::string, Bytes>> out;
    for (const char* tag : {"cvt ", "fpgm", "glyf", "head", "hhea", "hmtx", "loca", "maxp", "prep"}) {
        if (std::strcmp(tag, "glyf") == 0) {
            out.emplace_back(tag, std::move(new_glyf));
        } else if (std::strcmp(tag, "loca") == 0) {
            out.emplace_back(tag, std::move(new_loca));
        } else if (std::strcmp(tag, "hmtx") == 0) {
            // Full metrics up to the last glyph kept (or as many as there are), then side bearings.
            const std::uint32_t at = table("hmtx")->offset;
            const int full = std::min(metrics_, count);
            Bytes hmtx(data_.begin() + at, data_.begin() + at + 4 * static_cast<std::size_t>(full));
            for (int g = full; g < count; ++g) {
                try {
                    put16(hmtx, u16(d, at + 4 * static_cast<std::size_t>(metrics_) + 2 * static_cast<std::size_t>(g - metrics_)));
                } catch (const Damaged&) {
                    put16(hmtx, 0);  // a short table: no side bearing
                }
            }
            out.emplace_back(tag, std::move(hmtx));
        } else if (const Table* t = table(tag)) {
            const ByteSpan bytes = d.subspan(t->offset, t->length);
            out.emplace_back(tag, Bytes(bytes.begin(), bytes.end()));
        }
    }
    for (auto& [tag, bytes] : out) {
        if (tag == "maxp" && bytes.size() >= 6) {  // numGlyphs
            bytes[4] = static_cast<std::uint8_t>(count >> 8);
            bytes[5] = static_cast<std::uint8_t>(count);
        } else if (tag == "hhea" && bytes.size() >= 36) {  // numberOfHMetrics
            const int full = std::min(metrics_, count);
            bytes[34] = static_cast<std::uint8_t>(full >> 8);
            bytes[35] = static_cast<std::uint8_t>(full);
        }
    }
    for (auto& [tag, bytes] : out) {
        if (tag != "head") continue;
        if (bytes.size() < 54) return {};
        set32(bytes, 8, 0);   // checkSumAdjustment, set below
        bytes[50] = 0;        // indexToLocFormat
        bytes[51] = short_loca ? 0 : 1;
    }

    Bytes font;
    const std::uint16_t n = static_cast<std::uint16_t>(out.size());
    std::uint16_t power = 1, log2 = 0;
    while (power * 2 <= n) {
        power = static_cast<std::uint16_t>(power * 2);
        ++log2;
    }
    put32(font, 0x00010000);
    put16(font, n);
    put16(font, power * 16u);
    put16(font, log2);
    put16(font, n * 16u - power * 16u);
    std::size_t at = 12 + 16 * static_cast<std::size_t>(n), head_at = 0;
    for (const auto& [tag, bytes] : out) {
        font.insert(font.end(), tag.begin(), tag.end());
        put32(font, checksum(bytes));
        put32(font, static_cast<std::uint32_t>(at));
        put32(font, static_cast<std::uint32_t>(bytes.size()));
        if (tag == "head") head_at = at;
        at += (bytes.size() + 3) / 4 * 4;
    }
    for (const auto& [tag, bytes] : out) {
        font.insert(font.end(), bytes.begin(), bytes.end());
        while (font.size() % 4) font.push_back(0);
    }
    if (head_at) set32(font, head_at + 8, 0xB1B0AFBAu - checksum(font));
    return font;
}

std::vector<std::string> system_font_files() {
    std::vector<std::string> out;
#ifdef _WIN32
    std::string dir = os::get_env("WINDIR");
    if (dir.empty()) dir = "C:\\Windows";
    dir += "\\Fonts\\";
    // Latin, Greek and Cyrillic, then Arabic and Hebrew, Indian scripts, Korean, Chinese, Japanese.
    for (const char* name : {"segoeui.ttf", "arial.ttf", "tahoma.ttf", "Nirmala.ttc", "malgun.ttf", "msyh.ttc",
                             "YuGothR.ttc", "msgothic.ttc", "simsun.ttc", "seguisym.ttf"}) {
        const std::string path = dir + name;
        if (os::exists(path)) out.push_back(path);
    }
#else
    namespace fs = std::filesystem;
    std::vector<std::string> found;
    std::vector<std::string> dirs = {"/usr/share/fonts", "/usr/local/share/fonts"};
    const std::string home = os::get_env("HOME");
    if (!home.empty()) {
        dirs.push_back(home + "/.local/share/fonts");
        dirs.push_back(home + "/.fonts");
    }
    for (const std::string& dir : dirs) {
        std::error_code ec;
        fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
        for (; !ec && it != end && found.size() < 2000; it.increment(ec)) {
            const std::string ext = it->path().extension().string();
            if (ext == ".ttf" || ext == ".ttc" || ext == ".TTF" || ext == ".TTC") found.push_back(it->path().string());
        }
    }
    // Broad sans-serif fonts first, then the rest in name order.
    const auto rank = [](const std::string& path) {
        const std::string file = path.substr(path.find_last_of('/') + 1);
        for (int i = 0; const char* name : {"DejaVuSans.ttf", "NotoSans-Regular.ttf", "LiberationSans-Regular.ttf",
                                             "FreeSans.ttf", "Ubuntu-R.ttf", "NotoSansArabic-Regular.ttf",
                                             "NotoSansHebrew-Regular.ttf", "NotoSansDevanagari-Regular.ttf",
                                             "wqy-zenhei.ttc", "wqy-microhei.ttc", "DroidSansFallbackFull.ttf",
                                             "DroidSansFallback.ttf", "NanumGothic.ttf"}) {
            if (file == name) return i;
            ++i;
        }
        return 100;
    };
    std::stable_sort(found.begin(), found.end(), [&](const std::string& a, const std::string& b) {
        const int ra = rank(a), rb = rank(b);
        return ra != rb ? ra < rb : a < b;
    });
    out = std::move(found);
#endif
    return out;
}

}  // namespace vaultik::ttf
