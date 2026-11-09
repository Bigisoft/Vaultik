#pragma once

// The shortest edit script between two sequences of lines (Myers' O((N+M)D) algorithm), for
// showing what changed in a text file. Works on any random-access sequence whose elements compare
// with ==, e.g. std::vector<std::string> or QStringList.

#include <algorithm>
#include <cstddef>
#include <optional>
#include <vector>

namespace vaultik {

enum class DiffOp : unsigned char { Equal, Delete, Insert };

struct DiffEdit {
    DiffOp op = DiffOp::Equal;
    std::size_t old_line = 0;  // index in the old sequence (Equal, Delete)
    std::size_t new_line = 0;  // index in the new sequence (Equal, Insert)
};

// The edits turning `a` into `b`, in order. Empty optional when more than `max_edits` lines differ
// (the work and memory grow with the square of that; such files are better shown whole).
template <class Seq>
std::optional<std::vector<DiffEdit>> line_diff(const Seq& a, const Seq& b, int max_edits = 4000) {
    const int n = static_cast<int>(a.size());
    const int m = static_cast<int>(b.size());
    // The common start and end need no search.
    int pre = 0;
    while (pre < n && pre < m && a[pre] == b[pre]) ++pre;
    int suf = 0;
    while (suf < n - pre && suf < m - pre && a[n - 1 - suf] == b[m - 1 - suf]) ++suf;
    const int NN = n - pre - suf;  // the differing middle
    const int MM = m - pre - suf;

    std::vector<DiffEdit> out;
    out.reserve(static_cast<std::size_t>(std::max(n, m)));
    for (int i = 0; i < pre; ++i) out.push_back({DiffOp::Equal, static_cast<std::size_t>(i), static_cast<std::size_t>(i)});

    if (NN > 0 || MM > 0) {
        const auto A = [&](int i) -> decltype(a[0]) { return a[pre + i]; };
        const auto B = [&](int j) -> decltype(b[0]) { return b[pre + j]; };
        const int max = NN + MM;
        const int limit = std::min(max, max_edits);
        const int off = max + 1;
        std::vector<int> v(static_cast<std::size_t>(2 * max + 3), 0);
        // v as it was before each step d, for k in [-(d+1), d+1]: enough to walk back.
        struct Slice {
            int lo;
            std::vector<int> values;
        };
        std::vector<Slice> trace;
        int found = -1;
        for (int d = 0; d <= limit && found < 0; ++d) {
            Slice s{-(d + 1), {}};
            s.values.assign(v.begin() + (off - d - 1), v.begin() + (off + d + 2));
            trace.push_back(std::move(s));
            for (int k = -d; k <= d; k += 2) {
                int x = (k == -d || (k != d && v[off + k - 1] < v[off + k + 1])) ? v[off + k + 1] : v[off + k - 1] + 1;
                int y = x - k;
                while (x < NN && y < MM && A(x) == B(y)) {
                    ++x;
                    ++y;
                }
                v[off + k] = x;
                if (x >= NN && y >= MM) {
                    found = d;
                    break;
                }
            }
        }
        if (found < 0) return std::nullopt;

        // Walk back from the end to the start, collecting the edits in reverse.
        std::vector<DiffEdit> middle;
        int x = NN, y = MM;
        for (int d = found; d >= 0; --d) {
            const Slice& s = trace[static_cast<std::size_t>(d)];
            const auto V = [&](int k) { return s.values[static_cast<std::size_t>(k - s.lo)]; };
            const int k = x - y;
            const int prev_k = (k == -d || (k != d && V(k - 1) < V(k + 1))) ? k + 1 : k - 1;
            const int prev_x = d == 0 ? 0 : V(prev_k);
            const int prev_y = d == 0 ? 0 : prev_x - prev_k;
            while (x > prev_x && y > prev_y) {
                --x;
                --y;
                middle.push_back({DiffOp::Equal, static_cast<std::size_t>(pre + x), static_cast<std::size_t>(pre + y)});
            }
            if (d > 0) {
                if (x == prev_x) middle.push_back({DiffOp::Insert, 0, static_cast<std::size_t>(pre + prev_y)});
                else middle.push_back({DiffOp::Delete, static_cast<std::size_t>(pre + prev_x), 0});
            }
            x = prev_x;
            y = prev_y;
        }
        out.insert(out.end(), middle.rbegin(), middle.rend());
    }

    for (int i = 0; i < suf; ++i)
        out.push_back({DiffOp::Equal, static_cast<std::size_t>(n - suf + i), static_cast<std::size_t>(m - suf + i)});
    return out;
}

}  // namespace vaultik
