#include "vaultik/jobs/recovery_kit.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

#include "vaultik/platform/os.hpp"
#include "vaultik/repo/cloud.hpp"
#include "vaultik/util/error.hpp"
#include "vaultik/util/ttf.hpp"
#include "vaultik/version.hpp"

namespace vaultik::jobs {

namespace {

// Marks a fixed phrase for translation (tools/extract_strings.py collects these and say()'s).
constexpr const char* phrase(const char* text) {
    return text;
}

// Fills %1 .. %9 in a phrase after it was translated, in one pass.
std::string fill(const std::string& text, const std::vector<std::string>& args) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 1 < text.size() && text[i + 1] >= '1' && text[i + 1] <= '9') {
            const std::size_t n = static_cast<std::size_t>(text[i + 1] - '1');
            if (n < args.size()) {
                out += args[n];
                ++i;
                continue;
            }
        }
        out += text[i];
    }
    return out;
}

struct Say {
    const ReportWords& words;
    std::string operator()(const std::string& text, const std::vector<std::string>& args = {}) const {
        return fill(words ? words(text) : text, args);
    }
};

// The page's texts, in the order they appear.
struct Texts {
    std::string title, warning, key_heading, key_note, details, location, kind, computer, made, steps_heading;
    std::vector<std::string> steps;
    std::string footer;
};

Texts texts_of(const RecoveryKit& kit, const Say& say) {
    Texts t;
    t.title = say("Vaultik recovery kit");
    t.warning = say("Keep this page somewhere safe, away from this computer. With the recovery key, anyone who can "
                    "reach the backup can open it.");
    t.key_heading = say("Recovery key");
    t.key_note = say("It sets a new password when the password is forgotten. Making a new recovery key makes this "
                     "one stop working: then print a new kit.");
    t.details = say("The backup");
    t.location = say("Where it is");
    t.kind = say(kit.kind, {kit.kind_arg});
    t.computer = say("Computer");
    t.made = say("Kit made on");
    t.steps_heading = say("To restore on a new computer");
    t.steps = {
        say("Install Vaultik (github.com/Bigisoft/Vaultik), or use its portable copy: after each backup to an "
            "external drive, Vaultik keeps one in the Vaultik folder on that drive."),
        say("Connect the backup drive, or make sure the network share can be reached. For cloud storage, have its "
            "sign-in or access key at hand: it is not on this page."),
        say("Start Vaultik and choose Restore from an existing backup (or Restore from another backup), then "
            "pick the place shown above."),
        say("Enter the backup's password. If it is forgotten, open Password and recovery key, choose the recovery "
            "key, type it (or scan the QR code) and set a new password."),
        say("Pick a version and what to restore. Move to this PC in the restore window brings back a whole "
            "account's files at once."),
    };
    t.footer = say("Made with Vaultik %1", {kit.version});
    return t;
}

std::string date_text(std::uint64_t time) {
    return os::format_time_local(time).substr(0, 16);
}

// ---- PDF -----------------------------------------------------------------------------------

// Widths of Helvetica's characters 32..126 in 1/1000 of the font size (from its metrics).
constexpr short kHelvetica[95] = {
    278, 278, 355, 556, 556, 889, 667, 191, 333, 333, 389, 584, 278, 333, 278, 278, 556, 556, 556, 556,
    556, 556, 556, 556, 556, 556, 278, 278, 584, 584, 584, 556, 1015, 667, 667, 722, 722, 667, 611, 778,
    722, 278, 500, 667, 556, 833, 722, 778, 667, 778, 722, 667, 611, 722, 667, 944, 667, 667, 611, 278,
    278, 278, 469, 556, 333, 556, 556, 500, 556, 556, 278, 556, 556, 222, 222, 500, 222, 833, 556, 556,
    556, 556, 333, 500, 278, 556, 500, 722, 500, 500, 500, 334, 260, 334, 584};

// UTF-8 to characters; a broken sequence becomes '?'.
std::u32string decode(const std::string& utf8) {
    std::u32string out;
    for (std::size_t i = 0; i < utf8.size();) {
        const auto c = static_cast<unsigned char>(utf8[i]);
        char32_t cp = 0;
        std::size_t len = 1;
        if (c < 0x80) cp = c;
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
        else { out += U'?'; ++i; continue; }
        if (i + len > utf8.size()) { out += U'?'; break; }
        bool ok = true;
        for (std::size_t k = 1; k < len; ++k) {
            const auto d = static_cast<unsigned char>(utf8[i + k]);
            if ((d & 0xC0) != 0x80) ok = false;
            cp = (cp << 6) | (d & 0x3F);
        }
        i += ok ? len : 1;
        out += ok && cp <= 0x10FFFF ? cp : U'?';
    }
    return out;
}

// A character in the PDF's WinAnsi encoding (of its built-in fonts), or -1 when it has none.
int ansi_of(char32_t c) {
    if (c < 0x20) return ' ';
    if (c < 0x7F || (c >= 0xA0 && c <= 0xFF)) return static_cast<int>(c);
    switch (c) {
    case 0x2018: case 0x2019: return '\'';
    case 0x201C: case 0x201D: return '"';
    case 0x2013: case 0x2014: return '-';
    case 0x20AC: return 0x80;  // euro sign
    case 0x2026: return 0x85;  // ellipsis
    case 0x2022: return 0x95;  // bullet
    default: return -1;
    }
}

std::string win_ansi(const std::string& utf8) {
    std::string out;
    for (const char32_t c : decode(utf8)) {
        const int a = ansi_of(c);
        out += static_cast<char>(a < 0 ? '?' : a);
    }
    return out;
}

// Fonts of this system for the characters the built-in fonts do not have, embedded as subsets
// (as /F4, /F5 ...).
struct ExtraFonts {
    std::vector<ttf::Font> fonts;
    std::map<char32_t, std::pair<std::size_t, std::uint16_t>> glyphs;  // character: font, glyph
    std::vector<std::map<std::uint16_t, char32_t>> used;               // per font: glyph, its character
};

ExtraFonts extra_fonts(const std::u32string& text) {
    std::set<char32_t> missing;
    for (const char32_t c : text)
        if (ansi_of(c) < 0) missing.insert(c);
    ExtraFonts x;
    for (const std::string& path : missing.empty() ? std::vector<std::string>{} : ttf::system_font_files()) {
        if (missing.empty() || x.fonts.size() == 4) break;
        Bytes data;
        try {
            data = os::read_file(path, 48u << 20);
        } catch (const std::exception&) {
            continue;
        }
        std::optional<ttf::Font> font = ttf::Font::parse(std::move(data));
        if (!font) continue;
        std::vector<char32_t> covered;
        for (const char32_t c : missing)
            if (font->glyph(c)) covered.push_back(c);
        if (covered.empty()) continue;
        for (const char32_t c : covered) {
            x.glyphs[c] = {x.fonts.size(), font->glyph(c)};
            missing.erase(c);
        }
        x.fonts.push_back(std::move(*font));
        x.used.emplace_back();
    }
    return x;
}

std::string pdf_string(const std::string& ansi) {
    std::string out = "(";
    for (const char c : ansi) {
        if (c == '(' || c == ')' || c == '\\') out += '\\';
        out += c;
    }
    return out + ")";
}

std::string num(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.2f", v);
    return buf;
}

std::string hex4(unsigned v) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "%04X", v & 0xFFFF);
    return buf;
}

class PdfPage {
public:
    explicit PdfPage(ExtraFonts& extra) : x_(extra) {}

    // font: 1 Helvetica, 2 Helvetica-Bold, 3 Courier-Bold. Characters these do not have are drawn
    // with an extra font (/F4 ...), or as '?'.
    void text(double x, double y, int font, double size, const std::string& utf8) { text(x, y, font, size, decode(utf8)); }
    // A paragraph from (x, y) down; returns the y below it.
    double paragraph(double x, double y, double width, int font, double size, const std::string& utf8) {
        for (const std::u32string& line : wrap(decode(utf8), font, size, width)) {
            text(x, y, font, size, line);
            y -= size * 1.35;
        }
        return y;
    }
    void color(double r, double g, double b) { s_ += num(r) + " " + num(g) + " " + num(b) + " rg\n"; }
    void rect(double x, double y, double w, double h) { s_ += num(x) + " " + num(y) + " " + num(w) + " " + num(h) + " re\n"; }
    void fill() { s_ += "f\n"; }
    void line(double x1, double y1, double x2, double y2, double width) {
        s_ += num(width) + " w " + num(x1) + " " + num(y1) + " m " + num(x2) + " " + num(y2) + " l S\n";
    }
    const std::string& content() const { return s_; }

private:
    const std::pair<std::size_t, std::uint16_t>* extra_glyph(char32_t c) const {
        if (ansi_of(c) >= 0) return nullptr;
        const auto it = x_.glyphs.find(c);
        return it == x_.glyphs.end() ? nullptr : &it->second;
    }

    double width(const std::u32string& s, int font, double size) const {
        double w = 0;
        for (const char32_t c : s) {
            if (const auto* g = extra_glyph(c)) {
                w += x_.fonts[g->first].advance(g->second);
                continue;
            }
            const int a = ansi_of(c) < 0 ? '?' : ansi_of(c);
            const double base = font == 3 ? 600 : a >= 32 && a <= 126 ? kHelvetica[a - 32] : 556;
            w += base * (font == 2 ? 1.08 : 1.0);  // bold runs a little wider
        }
        return w * size / 1000.0;
    }

    // Lines of at most `width` points; a word longer than a line is cut.
    std::vector<std::u32string> wrap(const std::u32string& s, int font, double size, double width_) const {
        std::vector<std::u32string> lines;
        std::u32string line;
        std::size_t at = 0;
        while (at < s.size()) {
            std::size_t end = s.find(U' ', at);
            if (end == std::u32string::npos) end = s.size();
            std::u32string word = s.substr(at, end - at);
            at = end + 1;
            const std::u32string candidate = line.empty() ? word : line + U" " + word;
            if (width(candidate, font, size) <= width_) {
                line = candidate;
                continue;
            }
            if (!line.empty()) lines.push_back(line);
            line.clear();
            while (width(word, font, size) > width_ && word.size() > 1) {
                std::size_t n = word.size();
                while (n > 1 && width(word.substr(0, n), font, size) > width_) --n;
                lines.push_back(word.substr(0, n));
                word = word.substr(n);
            }
            line = word;
        }
        if (!line.empty()) lines.push_back(line);
        return lines;
    }

    void text(double x, double y, int font, double size, const std::u32string& s) {
        s_ += "BT " + num(x) + " " + num(y) + " Td";
        int current = 0;
        std::string run;
        const auto flush = [&] {
            if (run.empty()) return;
            s_ += " /F" + std::to_string(current) + " " + num(size) + " Tf " +
                  (current > 3 ? "<" + run + ">" : pdf_string(run)) + " Tj";
            run.clear();
        };
        for (const char32_t c : s) {
            const auto* g = extra_glyph(c);
            const int f = g ? 4 + static_cast<int>(g->first) : font;
            if (f != current) {
                flush();
                current = f;
            }
            if (g) {
                run += hex4(g->second);  // Identity-H: the glyph number, two bytes
                x_.used[g->first].emplace(g->second, c);
            } else {
                const int a = ansi_of(c);
                run += static_cast<char>(a < 0 ? '?' : a);
            }
        }
        flush();
        s_ += " ET\n";
    }

    ExtraFonts& x_;
    std::string s_;
};

// The PDF objects of extra font `k` as numbers `first` .. `first` + 4: the font, its CID font, its
// descriptor, the embedded subset and the map back to characters (for copying text out).
std::vector<std::string> font_objects(const ExtraFonts& x, std::size_t k, std::size_t first) {
    const ttf::Font& f = x.fonts[k];
    const std::map<std::uint16_t, char32_t>& used = x.used[k];
    const std::string name = std::string("/VLTK") + static_cast<char>('A' + k) + "A+" + f.name();
    const auto ref = [&](std::size_t i) { return std::to_string(first + i) + " 0 R"; };
    std::string widths = "[";
    for (const auto& [glyph, c] : used) widths += " " + std::to_string(glyph) + " [" + std::to_string(f.advance(glyph)) + "]";
    widths += " ]";
    std::set<std::uint16_t> glyphs;
    for (const auto& [glyph, c] : used) glyphs.insert(glyph);
    const Bytes subset = f.subset(glyphs);

    std::string cmap =
        "/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n"
        "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
        "/CMapName /Adobe-Identity-UCS def\n/CMapType 2 def\n"
        "1 begincodespacerange\n<0000> <FFFF>\nendcodespacerange\n";
    std::vector<std::pair<std::uint16_t, char32_t>> pairs(used.begin(), used.end());
    for (std::size_t i = 0; i < pairs.size(); i += 100) {
        const std::size_t n = std::min<std::size_t>(100, pairs.size() - i);
        cmap += std::to_string(n) + " beginbfchar\n";
        for (std::size_t j = i; j < i + n; ++j) {
            const char32_t c = pairs[j].second;
            std::string utf16 = c > 0xFFFF ? hex4(0xD800 + ((c - 0x10000) >> 10)) + hex4(0xDC00 + ((c - 0x10000) & 0x3FF))
                                           : hex4(c);
            cmap += "<" + hex4(pairs[j].first) + "> <" + utf16 + ">\n";
        }
        cmap += "endbfchar\n";
    }
    cmap += "endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend";

    return {
        "<< /Type /Font /Subtype /Type0 /BaseFont " + name + " /Encoding /Identity-H /DescendantFonts [" + ref(1) +
            "] /ToUnicode " + ref(4) + " >>",
        "<< /Type /Font /Subtype /CIDFontType2 /BaseFont " + name +
            " /CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >> /FontDescriptor " + ref(2) +
            " /CIDToGIDMap /Identity /DW 1000 /W " + widths + " >>",
        "<< /Type /FontDescriptor /FontName " + name + " /Flags 32 /FontBBox [" + std::to_string(f.bbox(0)) + " " +
            std::to_string(f.bbox(1)) + " " + std::to_string(f.bbox(2)) + " " + std::to_string(f.bbox(3)) +
            "] /ItalicAngle 0 /Ascent " + std::to_string(f.ascent()) + " /Descent " + std::to_string(f.descent()) +
            " /CapHeight " + std::to_string(f.ascent()) + " /StemV 80 /FontFile2 " + ref(3) + " >>",
        "<< /Length " + std::to_string(subset.size()) + " /Length1 " + std::to_string(subset.size()) + " >>\nstream\n" +
            std::string(subset.begin(), subset.end()) + "\nendstream",
        "<< /Length " + std::to_string(cmap.size()) + " >>\nstream\n" + cmap + "\nendstream",
    };
}

Bytes pdf_file(const std::string& content, const std::string& title, const ExtraFonts& extra) {
    // 1-7 as below, 8 the document's information, then five objects per extra font that is used.
    std::string fonts = "/F1 5 0 R /F2 6 0 R /F3 7 0 R";
    std::vector<std::string> extra_objects;
    for (std::size_t k = 0; k < extra.fonts.size(); ++k) {
        if (extra.used[k].empty()) continue;
        const std::size_t first = 9 + extra_objects.size();
        fonts += " /F" + std::to_string(4 + k) + " " + std::to_string(first) + " 0 R";
        for (std::string& o : font_objects(extra, k, first)) extra_objects.push_back(std::move(o));
    }
    std::vector<std::string> objects = {
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Resources << /Font << " + fonts +
            " >> >> /Contents 4 0 R >>",
        "<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content + "\nendstream",
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>",
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold /Encoding /WinAnsiEncoding >>",
        "<< /Type /Font /Subtype /Type1 /BaseFont /Courier-Bold /Encoding /WinAnsiEncoding >>",
        "<< /Title " + pdf_string(win_ansi(title)) + " /Producer (Vaultik) >>",
    };
    for (std::string& o : extra_objects) objects.push_back(std::move(o));
    std::string out = "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
    std::vector<std::size_t> offsets;
    for (std::size_t i = 0; i < objects.size(); ++i) {
        offsets.push_back(out.size());
        out += std::to_string(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }
    const std::size_t xref = out.size();
    out += "xref\n0 " + std::to_string(objects.size() + 1) + "\n0000000000 65535 f \n";
    for (const std::size_t o : offsets) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%010zu 00000 n \n", o);
        out += buf;
    }
    out += "trailer\n<< /Size " + std::to_string(objects.size() + 1) + " /Root 1 0 R /Info 8 0 R >>\nstartxref\n" +
           std::to_string(xref) + "\n%%EOF\n";
    return Bytes(out.begin(), out.end());
}

}  // namespace

std::string public_location(const std::string& location) {
    std::string s = location;
    std::size_t start = s.find("://");
    if (start != std::string::npos) {
        start += 3;
    } else if (is_cloud_location(s)) {
        start = s.find(':') + 1;
    } else {
        return s;  // a folder
    }
    std::size_t end = s.find('/', start);
    if (end == std::string::npos) end = s.size();
    const std::size_t at = s.rfind('@', end);
    if (at != std::string::npos && at >= start) {
        const std::size_t colon = s.find(':', start);
        if (colon != std::string::npos && colon < at) s.erase(colon, at - colon);
    }
    const std::size_t query = s.find_first_of("?#", start);
    if (query != std::string::npos) s.erase(query);
    return s;
}

std::pair<std::string, std::string> location_kind(const std::string& location) {
    switch (cloud_kind(location)) {
        case CloudKind::S3: return {phrase("S3-compatible cloud storage"), {}};
        case CloudKind::Sftp: return {phrase("a folder on an SFTP server"), {}};
        case CloudKind::WebDav: return {phrase("a folder on a WebDAV server"), {}};
        case CloudKind::PCloud:
        case CloudKind::OneDrive:
        case CloudKind::GoogleDrive:
        case CloudKind::Mega: return {phrase("a folder in the cloud drive %1"), cloud_kind_name(cloud_kind(location))};
        case CloudKind::None: break;
    }
    if (location.starts_with("\\\\") || location.starts_with("//")) return {phrase("a folder on a network share"), {}};
    try {
        const os::VolumeLocation v = os::volume_of(os::absolute_path(location));
        if (v.external) return {phrase("a folder on an external drive (%1 when it was made)"), v.root};
        if (v.volume.empty() && !v.root.empty()) return {phrase("a folder on a network share"), {}};
    } catch (const std::exception&) {
    }
    return {phrase("a folder on a drive of the computer"), {}};
}

RecoveryKit make_recovery_kit(const std::string& key, const std::string& location) {
    RecoveryKit kit;
    kit.key = key;
    kit.location = public_location(location);
    std::tie(kit.kind, kit.kind_arg) = location_kind(location);
    kit.computer = os::hostname();
    kit.made = os::now();
    kit.version = VAULTIK_VERSION;
    return kit;
}

qr::Code kit_qr(const RecoveryKit& kit) {
    return qr::encode_text(kit.key, qr::Ecc::Q);
}

std::string qr_svg(const qr::Code& code, int module) {
    const int quiet = 4;
    const int side = (code.size + 2 * quiet) * module;
    std::string s = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" + std::to_string(side) + "\" height=\"" +
                    std::to_string(side) + "\" viewBox=\"0 0 " + std::to_string(code.size + 2 * quiet) + " " +
                    std::to_string(code.size + 2 * quiet) +
                    "\" shape-rendering=\"crispEdges\"><rect width=\"100%\" height=\"100%\" fill=\"#fff\"/><path fill=\"#000\" d=\"";
    for (int y = 0; y < code.size; ++y)
        for (int x = 0; x < code.size; ++x)
            if (code.dark(x, y)) s += "M" + std::to_string(x + quiet) + " " + std::to_string(y + quiet) + "h1v1h-1z";
    return s + "\"/></svg>";
}

std::string render_kit_html(const RecoveryKit& kit, const ReportWords& words, const std::string& qr_html) {
    const Say say{words};
    const Texts t = texts_of(kit, say);
    const auto e = [](const std::string& x) { return html_escape(x); };
    std::string s =
        "<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\"><title>" + e(t.title) + "</title>"
        "<style>body{font-family:'Segoe UI',Arial,sans-serif;margin:24px;color:#1b1b1b;background:#fff;max-width:760px}"
        "h1{font-size:18pt;margin:0 0 8px}h2{font-size:12pt;margin:18px 0 6px}"
        "p.warn{font-weight:bold;color:#a4262c;margin:0 0 12px}"
        "p.key{font-family:Consolas,'Courier New',monospace;font-size:16pt;font-weight:bold;margin:6px 0}"
        "p.note{color:#5f5f5f;margin:0}td{padding:2px 16px 2px 0;vertical-align:top}td.label{color:#5f5f5f}"
        "ol li{margin:0 0 6px}p.foot{color:#8a8a8a;font-size:8pt;margin-top:20px}"
        "@media print{body{margin:0}}</style></head><body>\n";
    s += "<h1>" + e(t.title) + "</h1>\n<p class=\"warn\">" + e(t.warning) + "</p>\n";
    s += "<table width=\"100%\"><tr><td>\n<h2>" + e(t.key_heading) + "</h2>\n<p class=\"key\">" + e(kit.key) +
         "</p>\n<p class=\"note\">" + e(t.key_note) + "</p>\n</td><td align=\"right\">" +
         (qr_html.empty() ? qr_svg(kit_qr(kit), 4) : qr_html) + "</td></tr></table>\n";
    s += "<h2>" + e(t.details) + "</h2>\n<table>\n";
    s += "<tr><td class=\"label\">" + e(t.location) + "</td><td>" + e(kit.location) + "<br>" + e(t.kind) + "</td></tr>\n";
    s += "<tr><td class=\"label\">" + e(t.computer) + "</td><td>" + e(kit.computer) + "</td></tr>\n";
    s += "<tr><td class=\"label\">" + e(t.made) + "</td><td>" + e(date_text(kit.made)) + "</td></tr>\n</table>\n";
    s += "<h2>" + e(t.steps_heading) + "</h2>\n<ol>\n";
    for (const std::string& step : t.steps) s += "<li>" + e(step) + "</li>\n";
    s += "</ol>\n<p class=\"foot\">" + e(t.footer) + "</p>\n</body></html>\n";
    return s;
}

Bytes render_kit_pdf(const RecoveryKit& kit) {
    const Say say{ReportWords{}};
    const Texts t = texts_of(kit, say);
    constexpr double kLeft = 56, kWidth = 595 - 2 * kLeft;
    std::string all = kit.key + kit.location + kit.computer + date_text(kit.made) + t.title + t.warning + t.key_heading +
                      t.key_note + t.details + t.location + t.kind + t.computer + t.made + t.steps_heading + t.footer;
    for (const std::string& step : t.steps) all += step;
    ExtraFonts extra = extra_fonts(decode(all));
    PdfPage p(extra);
    double y = 842 - 70;
    p.text(kLeft, y, 2, 22, t.title);
    y -= 26;
    p.color(0.64, 0.15, 0.17);
    y = p.paragraph(kLeft, y, kWidth, 2, 10.5, t.warning);
    p.color(0, 0, 0);

    // The key on the left, its QR code on the right.
    constexpr double kQr = 150;
    const double top = y - 10;
    const qr::Code code = kit_qr(kit);
    const double cell = kQr / (code.size + 8);
    const double qx = kLeft + kWidth - kQr, qy = top - kQr;
    for (int row = 0; row < code.size; ++row)
        for (int col = 0; col < code.size; ++col)
            if (code.dark(col, row)) p.rect(qx + (col + 4) * cell, qy + kQr - (row + 5) * cell, cell, cell);
    p.fill();
    y = top - 16;
    p.text(kLeft, y, 2, 13, t.key_heading);
    y -= 26;
    // Four groups per line.
    const std::string& key = kit.key;
    std::vector<std::string> key_lines;
    std::size_t groups = 0, from = 0;
    for (std::size_t i = 0; i <= key.size(); ++i) {
        if (i == key.size() || key[i] == '-') {
            if (++groups % 4 == 0 || i == key.size()) {
                key_lines.push_back(key.substr(from, i + (i < key.size() ? 1 : 0) - from));
                from = i + 1;
            }
        }
    }
    for (const std::string& line : key_lines) {
        p.text(kLeft, y, 3, 18, line);
        y -= 24;
    }
    y = p.paragraph(kLeft, y - 4, kWidth - kQr - 20, 1, 9.5, t.key_note);
    y = std::min(y, qy) - 18;

    p.text(kLeft, y, 2, 13, t.details);
    y -= 20;
    const auto row = [&](const std::string& label, const std::string& value) {
        p.text(kLeft, y, 2, 10.5, label);
        y = p.paragraph(kLeft + 110, y, kWidth - 110, 1, 10.5, value) - 3;
    };
    row(t.location, kit.location);
    y = p.paragraph(kLeft + 110, y + 3, kWidth - 110, 1, 10.5, t.kind) - 3;
    row(t.computer, kit.computer);
    row(t.made, date_text(kit.made));

    y -= 10;
    p.text(kLeft, y, 2, 13, t.steps_heading);
    y -= 20;
    for (std::size_t i = 0; i < t.steps.size(); ++i) {
        p.text(kLeft, y, 2, 10.5, std::to_string(i + 1) + ".");
        y = p.paragraph(kLeft + 18, y, kWidth - 18, 1, 10.5, t.steps[i]) - 5;
    }
    p.line(kLeft, 52, kLeft + kWidth, 52, 0.5);
    p.text(kLeft, 40, 1, 8, t.footer + " - " + t.computer + ": " + kit.computer);
    return pdf_file(p.content(), t.title, extra);
}

void write_recovery_kit(const RecoveryKit& kit, const std::string& path) {
    const bool pdf = path.size() >= 4 && [&] {
        std::string ext = path.substr(path.size() - 4);
        for (char& c : ext)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        return ext == ".pdf";
    }();
    Bytes data;
    if (pdf) {
        data = render_kit_pdf(kit);
    } else {
        const std::string html = render_kit_html(kit);
        data.assign(html.begin(), html.end());
    }
    os::write_file_atomic(path, data, 0600);
}

}  // namespace vaultik::jobs
