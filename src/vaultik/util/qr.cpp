#include "vaultik/util/qr.hpp"

#include <algorithm>
#include <cstdlib>
#include <limits>

#include "vaultik/util/error.hpp"

namespace vaultik::qr {

namespace {

// Per level (L, M, Q, H) and version (index 0 unused): the error correction codewords of each
// block, and the number of blocks (ISO/IEC 18004, table 9).
constexpr std::int8_t kEccPerBlock[4][41] = {
    {-1, 7,  10, 15, 20, 26, 18, 20, 24, 30, 18, 20, 24, 26, 30, 22, 24, 28, 30, 28, 28,
     28, 28, 30, 30, 26, 28, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30},
    {-1, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26, 30, 22, 22, 24, 24, 28, 28, 26, 26, 26,
     26, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28},
    {-1, 13, 22, 18, 26, 18, 24, 18, 22, 20, 24, 28, 26, 24, 20, 30, 24, 28, 28, 26, 30,
     28, 30, 30, 30, 30, 28, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30},
    {-1, 17, 28, 22, 16, 22, 28, 26, 26, 24, 28, 24, 28, 22, 24, 24, 30, 28, 28, 26, 28,
     30, 24, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30},
};
constexpr std::int8_t kBlocks[4][41] = {
    {-1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 4,  4,  4,  4,  4,  6,  6,  6,  6,  7,  8,
     8,  9, 9, 10, 12, 12, 12, 13, 14, 15, 16, 17, 18, 19, 19, 20, 21, 22, 24, 25},
    {-1, 1,  1,  1,  2,  2,  4,  4,  4,  5,  5,  5,  8,  9,  9,  10, 10, 11, 13, 14, 16,
     17, 17, 18, 20, 21, 23, 25, 26, 28, 29, 31, 33, 35, 37, 38, 40, 43, 45, 47, 49},
    {-1, 1,  1,  2,  2,  4,  4,  6,  6,  8,  8,  8,  10, 12, 16, 12, 17, 16, 18, 21, 20,
     23, 23, 25, 27, 29, 34, 34, 35, 38, 40, 43, 45, 48, 51, 53, 56, 59, 62, 65, 68},
    {-1, 1,  1,  2,  4,  4,  4,  5,  6,  8,  8,  11, 11, 16, 16, 18, 16, 19, 21, 25, 25,
     25, 34, 30, 32, 35, 37, 40, 42, 45, 48, 51, 54, 57, 60, 63, 66, 70, 74, 77, 81},
};

int level_index(Ecc e) {
    return static_cast<int>(e);
}

// The two format bits of a level (they are not in the order of the enum).
int format_bits(Ecc e) {
    switch (e) {
        case Ecc::L: return 1;
        case Ecc::M: return 0;
        case Ecc::Q: return 3;
        case Ecc::H: return 2;
    }
    return 0;
}

// Modules left for data and error correction once the function patterns are drawn.
int raw_data_modules(int version) {
    int result = (16 * version + 128) * version + 64;
    if (version >= 2) {
        const int align = version / 7 + 2;
        result -= (25 * align - 10) * align - 55;
        if (version >= 7) result -= 36;
    }
    return result;
}

int data_codewords(int version, Ecc e) {
    const int l = level_index(e);
    return raw_data_modules(version) / 8 - kEccPerBlock[l][version] * kBlocks[l][version];
}

int count_bits(int version) {
    return version <= 9 ? 8 : 16;  // byte mode
}

// GF(2^8) with the polynomial x^8 + x^4 + x^3 + x^2 + 1.
std::uint8_t gf_multiply(std::uint8_t x, std::uint8_t y) {
    int z = 0;
    for (int i = 7; i >= 0; --i) {
        z = (z << 1) ^ ((z >> 7) * 0x11D);
        z ^= ((y >> i) & 1) * x;
    }
    return static_cast<std::uint8_t>(z);
}

std::vector<std::uint8_t> rs_divisor(int degree) {
    std::vector<std::uint8_t> result(static_cast<std::size_t>(degree), 0);
    result.back() = 1;
    std::uint8_t root = 1;
    for (int i = 0; i < degree; ++i) {
        for (std::size_t j = 0; j < result.size(); ++j) {
            result[j] = gf_multiply(result[j], root);
            if (j + 1 < result.size()) result[j] ^= result[j + 1];
        }
        root = gf_multiply(root, 0x02);
    }
    return result;
}

std::vector<std::uint8_t> rs_remainder(const std::vector<std::uint8_t>& data, const std::vector<std::uint8_t>& divisor) {
    std::vector<std::uint8_t> result(divisor.size(), 0);
    for (const std::uint8_t b : data) {
        const std::uint8_t factor = b ^ result[0];
        result.erase(result.begin());
        result.push_back(0);
        for (std::size_t i = 0; i < result.size(); ++i) result[i] ^= gf_multiply(divisor[i], factor);
    }
    return result;
}

class Builder {
public:
    Builder(int version, Ecc ecc)
        : version_(version), size_(17 + 4 * version), ecc_(ecc),
          modules_(static_cast<std::size_t>(size_) * size_, 0), function_(modules_.size(), 0) {}

    Code build(const std::vector<std::uint8_t>& data_codewords, int mask) {
        draw_function_patterns();
        draw_codewords(add_ecc_and_interleave(data_codewords));
        if (mask < 0) {
            long best = std::numeric_limits<long>::max();
            for (int m = 0; m < 8; ++m) {
                apply_mask(m);
                draw_format_bits(m);
                const long p = penalty();
                if (p < best) {
                    best = p;
                    mask = m;
                }
                apply_mask(m);  // XOR again: undone
            }
        }
        apply_mask(mask);
        draw_format_bits(mask);
        Code c;
        c.version = version_;
        c.size = size_;
        c.mask = mask;
        c.ecc = ecc_;
        c.modules = std::move(modules_);
        return c;
    }

private:
    bool get(int x, int y) const { return modules_[static_cast<std::size_t>(y) * size_ + x] != 0; }
    void set(int x, int y, bool dark) { modules_[static_cast<std::size_t>(y) * size_ + x] = dark ? 1 : 0; }
    void set_function(int x, int y, bool dark) {
        set(x, y, dark);
        function_[static_cast<std::size_t>(y) * size_ + x] = 1;
    }
    bool is_function(int x, int y) const { return function_[static_cast<std::size_t>(y) * size_ + x] != 0; }

    std::vector<int> alignment_positions() const {
        if (version_ == 1) return {};
        const int count = version_ / 7 + 2;
        const int step = version_ == 32 ? 26 : (version_ * 4 + count * 2 + 1) / (count * 2 - 2) * 2;
        std::vector<int> result{6};
        for (int i = 0, pos = size_ - 7; i < count - 1; ++i, pos -= step) result.insert(result.begin() + 1, pos);
        return result;
    }

    void draw_finder(int x, int y) {
        for (int dy = -4; dy <= 4; ++dy)
            for (int dx = -4; dx <= 4; ++dx) {
                const int dist = std::max(std::abs(dx), std::abs(dy));
                const int xx = x + dx, yy = y + dy;
                if (xx >= 0 && xx < size_ && yy >= 0 && yy < size_) set_function(xx, yy, dist != 2 && dist != 4);
            }
    }

    void draw_alignment(int x, int y) {
        for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -2; dx <= 2; ++dx) set_function(x + dx, y + dy, std::max(std::abs(dx), std::abs(dy)) != 1);
    }

    void draw_function_patterns() {
        for (int i = 0; i < size_; ++i) {
            set_function(6, i, i % 2 == 0);
            set_function(i, 6, i % 2 == 0);
        }
        draw_finder(3, 3);
        draw_finder(size_ - 4, 3);
        draw_finder(3, size_ - 4);
        const std::vector<int> pos = alignment_positions();
        const std::size_t n = pos.size();
        for (std::size_t i = 0; i < n; ++i)
            for (std::size_t j = 0; j < n; ++j) {
                if ((i == 0 && j == 0) || (i == 0 && j == n - 1) || (i == n - 1 && j == 0)) continue;  // the finders
                draw_alignment(pos[i], pos[j]);
            }
        draw_format_bits(0);  // reserves the area; drawn again with the real mask
        draw_version();
    }

    void draw_format_bits(int mask) {
        const int data = format_bits(ecc_) << 3 | mask;
        int rem = data;
        for (int i = 0; i < 10; ++i) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
        const int bits = (data << 10 | rem) ^ 0x5412;
        const auto bit = [bits](int i) { return ((bits >> i) & 1) != 0; };
        // Around the top left finder.
        for (int i = 0; i <= 5; ++i) set_function(8, i, bit(i));
        set_function(8, 7, bit(6));
        set_function(8, 8, bit(7));
        set_function(7, 8, bit(8));
        for (int i = 9; i < 15; ++i) set_function(14 - i, 8, bit(i));
        // The copy beside the other two finders, and the module that is always dark.
        for (int i = 0; i < 8; ++i) set_function(size_ - 1 - i, 8, bit(i));
        for (int i = 8; i < 15; ++i) set_function(8, size_ - 15 + i, bit(i));
        set_function(8, size_ - 8, true);
    }

    void draw_version() {
        if (version_ < 7) return;
        int rem = version_;
        for (int i = 0; i < 12; ++i) rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
        const long bits = static_cast<long>(version_) << 12 | rem;
        for (int i = 0; i < 18; ++i) {
            const bool b = ((bits >> i) & 1) != 0;
            const int a = size_ - 11 + i % 3, c = i / 3;
            set_function(a, c, b);
            set_function(c, a, b);
        }
    }

    std::vector<std::uint8_t> add_ecc_and_interleave(const std::vector<std::uint8_t>& data) const {
        const int l = level_index(ecc_);
        const int blocks = kBlocks[l][version_];
        const int ecc_len = kEccPerBlock[l][version_];
        const int raw = raw_data_modules(version_) / 8;
        const int short_blocks = blocks - raw % blocks;
        const int short_len = raw / blocks;
        const std::vector<std::uint8_t> divisor = rs_divisor(ecc_len);
        std::vector<std::vector<std::uint8_t>> parts;
        std::size_t k = 0;
        for (int i = 0; i < blocks; ++i) {
            const int len = short_len - ecc_len + (i < short_blocks ? 0 : 1);
            std::vector<std::uint8_t> dat(data.begin() + static_cast<std::ptrdiff_t>(k),
                                          data.begin() + static_cast<std::ptrdiff_t>(k + static_cast<std::size_t>(len)));
            k += static_cast<std::size_t>(len);
            const std::vector<std::uint8_t> ecc = rs_remainder(dat, divisor);
            if (i < short_blocks) dat.push_back(0);  // a placeholder, skipped below
            dat.insert(dat.end(), ecc.begin(), ecc.end());
            parts.push_back(std::move(dat));
        }
        std::vector<std::uint8_t> result;
        for (std::size_t i = 0; i < parts[0].size(); ++i)
            for (std::size_t j = 0; j < parts.size(); ++j)
                if (i != static_cast<std::size_t>(short_len - ecc_len) || j >= static_cast<std::size_t>(short_blocks))
                    result.push_back(parts[j][i]);
        return result;
    }

    void draw_codewords(const std::vector<std::uint8_t>& data) {
        std::size_t i = 0;
        const std::size_t total = data.size() * 8;
        for (int right = size_ - 1; right >= 1; right -= 2) {
            if (right == 6) right = 5;  // the vertical timing pattern
            for (int vert = 0; vert < size_; ++vert)
                for (int j = 0; j < 2; ++j) {
                    const int x = right - j;
                    const bool upward = ((right + 1) & 2) == 0;
                    const int y = upward ? size_ - 1 - vert : vert;
                    if (!is_function(x, y) && i < total) {
                        set(x, y, ((data[i >> 3] >> (7 - (i & 7))) & 1) != 0);
                        ++i;
                    }
                    // Remainder bits (0 to 7) stay light.
                }
        }
    }

    void apply_mask(int mask) {
        for (int y = 0; y < size_; ++y)
            for (int x = 0; x < size_; ++x) {
                bool invert = false;
                switch (mask) {
                    case 0: invert = (x + y) % 2 == 0; break;
                    case 1: invert = y % 2 == 0; break;
                    case 2: invert = x % 3 == 0; break;
                    case 3: invert = (x + y) % 3 == 0; break;
                    case 4: invert = (x / 3 + y / 2) % 2 == 0; break;
                    case 5: invert = x * y % 2 + x * y % 3 == 0; break;
                    case 6: invert = (x * y % 2 + x * y % 3) % 2 == 0; break;
                    case 7: invert = ((x + y) % 2 + x * y % 3) % 2 == 0; break;
                    default: break;
                }
                if (invert && !is_function(x, y)) set(x, y, !get(x, y));
            }
    }

    // The penalty rules of the standard (section 7.8.3), to pick the mask.
    long penalty() const {
        constexpr long kN1 = 3, kN2 = 3, kN3 = 40, kN4 = 10;
        long result = 0;
        for (int pass = 0; pass < 2; ++pass) {  // rows, then columns
            for (int a = 0; a < size_; ++a) {
                bool run_color = false;
                int run = 0;
                std::vector<int> history(7, 0);
                for (int b = 0; b < size_; ++b) {
                    const bool m = pass == 0 ? get(b, a) : get(a, b);
                    if (m == run_color) {
                        ++run;
                        if (run == 5) result += kN1;
                        else if (run > 5) ++result;
                    } else {
                        add_history(run, history);
                        if (!run_color) result += finder_like(history) * kN3;
                        run_color = m;
                        run = 1;
                    }
                }
                result += terminate_and_count(run_color, run, history) * kN3;
            }
        }
        for (int y = 0; y < size_ - 1; ++y)
            for (int x = 0; x < size_ - 1; ++x) {
                const bool c = get(x, y);
                if (c == get(x + 1, y) && c == get(x, y + 1) && c == get(x + 1, y + 1)) result += kN2;
            }
        long dark = 0;
        for (const std::uint8_t m : modules_) dark += m;
        const long total = static_cast<long>(size_) * size_;
        const long k = (std::labs(dark * 20 - total * 10) + total - 1) / total - 1;
        result += k * kN4;
        return result;
    }

    void add_history(int run, std::vector<int>& history) const {
        if (history[0] == 0) run += size_;  // the light border before the first run
        history.pop_back();
        history.insert(history.begin(), run);
    }

    long finder_like(const std::vector<int>& h) const {
        const int n = h[1];
        const bool core = n > 0 && h[2] == n && h[3] == n * 3 && h[4] == n && h[5] == n;
        return (core && h[0] >= n * 4 && h[6] >= n ? 1 : 0) + (core && h[6] >= n * 4 && h[0] >= n ? 1 : 0);
    }

    long terminate_and_count(bool run_color, int run, std::vector<int>& history) const {
        if (run_color) {
            add_history(run, history);
            run = 0;
        }
        run += size_;  // the light border after the last run
        add_history(run, history);
        return finder_like(history);
    }

    int version_;
    int size_;
    Ecc ecc_;
    std::vector<std::uint8_t> modules_;
    std::vector<std::uint8_t> function_;
};

}  // namespace

std::size_t byte_capacity(int version, Ecc ecc) {
    if (version < 1 || version > 40) return 0;
    const int bits = data_codewords(version, ecc) * 8 - 4 - count_bits(version);
    return bits > 0 ? static_cast<std::size_t>(bits / 8) : 0;
}

Code encode(ByteSpan data, Ecc ecc, int min_version, int mask) {
    if (min_version < 1 || min_version > 40) throw Error("QR code version out of range");
    if (mask < -1 || mask > 7) throw Error("QR code mask out of range");
    int version = min_version;
    while (version <= 40 && byte_capacity(version, ecc) < data.size()) ++version;
    if (version > 40) throw Error("too much data for a QR code");

    // Mode, count, the bytes, a terminator of up to four zero bits, zero bits to a whole byte,
    // then alternating pad bytes.
    std::vector<bool> bits;
    const auto append = [&bits](std::uint32_t value, int n) {
        for (int i = n - 1; i >= 0; --i) bits.push_back(((value >> i) & 1) != 0);
    };
    append(0x4, 4);
    append(static_cast<std::uint32_t>(data.size()), count_bits(version));
    for (const std::uint8_t b : data) append(b, 8);
    const std::size_t capacity = static_cast<std::size_t>(data_codewords(version, ecc)) * 8;
    append(0, static_cast<int>(std::min<std::size_t>(4, capacity - bits.size())));
    append(0, static_cast<int>((8 - bits.size() % 8) % 8));
    for (std::uint8_t pad = 0xEC; bits.size() < capacity; pad ^= 0xEC ^ 0x11) append(pad, 8);

    std::vector<std::uint8_t> codewords(bits.size() / 8, 0);
    for (std::size_t i = 0; i < bits.size(); ++i)
        if (bits[i]) codewords[i >> 3] |= static_cast<std::uint8_t>(1 << (7 - (i & 7)));
    return Builder(version, ecc).build(codewords, mask);
}

Code encode_text(std::string_view utf8, Ecc ecc, int min_version, int mask) {
    return encode(ByteSpan(reinterpret_cast<const std::uint8_t*>(utf8.data()), utf8.size()), ecc, min_version, mask);
}

}  // namespace vaultik::qr
