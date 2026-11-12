#pragma once

// TrueType fonts for PDF output. Reads a font (a .ttf file, or one font of a .ttc collection) whose
// glyph outlines are in a 'glyf' table, maps characters to its glyphs and gives their widths, and
// writes a subset for embedding (a PDF's FontFile2): the glyphs used keep their numbers, every
// other glyph is left empty, and only the tables a PDF reader needs are kept. Fonts with CFF
// outlines (most .otf files) are not read.

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "vaultik/util/bytes.hpp"

namespace vaultik::ttf {

class Font {
public:
    // nullopt when `data` is not a TrueType font with glyf outlines (or font `index` of a
    // collection), or is damaged.
    static std::optional<Font> parse(Bytes data, unsigned index = 0);
    // Fonts in the data: the count of a .ttc collection, 1 for a single font, 0 for anything else.
    static unsigned count(ByteSpan data);

    // The glyph for a character; 0 (the font's "missing" glyph) when it has none.
    std::uint16_t glyph(char32_t c) const;
    // A glyph's advance width in 1/1000 of the font size.
    int advance(std::uint16_t glyph) const;
    int glyph_count() const { return glyphs_; }
    // In 1/1000 of the font size, as a PDF's font descriptor gives them.
    int ascent() const { return scale(ascent_); }
    int descent() const { return scale(descent_); }
    int bbox(int i) const { return scale(bbox_[i]); }
    // The PostScript name, letters, digits and '-' only; "Font" when it has none.
    const std::string& name() const { return name_; }

    // The font with only `glyphs` (and the glyphs their composites are made of, and glyph 0).
    Bytes subset(const std::set<std::uint16_t>& glyphs) const;

private:
    struct Table {
        std::uint32_t offset = 0, length = 0;
    };
    const Table* table(const char* tag) const;
    std::uint32_t glyph_offset(std::uint32_t glyph) const;  // into data_, from loca
    int scale(int units) const { return static_cast<int>(static_cast<long long>(units) * 1000 / em_); }

    Bytes data_;
    std::vector<std::pair<std::string, Table>> tables_;
    int em_ = 1000, glyphs_ = 0, metrics_ = 0, ascent_ = 0, descent_ = 0;
    int bbox_[4] = {0, 0, 0, 0};
    bool long_loca_ = false;
    std::uint32_t cmap_ = 0;  // offset of the cmap subtable used (format 4 or 12)
    int cmap_format_ = 0;
    std::string name_ = "Font";
};

// Font files of this system that cover letters beyond Western European ones, in the order to try:
// broad fonts (Segoe UI, Arial, DejaVu Sans, Noto Sans) first, then fonts for East Asian and other
// scripts. Only files that exist.
std::vector<std::string> system_font_files();

}  // namespace vaultik::ttf
