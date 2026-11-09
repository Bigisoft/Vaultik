#include <doctest/doctest.h>

#include "vaultik/util/bytes.hpp"
#include "vaultik/util/hex.hpp"
#include "vaultik/util/line_diff.hpp"

#include <random>
#include <string>
#include <vector>
#include "vaultik/platform/os.hpp"
#ifdef _WIN32
#include "vaultik/util/win.hpp"
#endif

using namespace vaultik;

TEST_CASE("varint and integer round trip") {
    ByteWriter w;
    const std::uint64_t values[] = {0, 1, 127, 128, 300, 16384, 0xFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull};
    for (auto v : values) w.varint(v);
    w.u32(0xDEADBEEF);
    w.u64(0x0123456789ABCDEFull);
    w.str("héllo");
    const Bytes b = w.take();

    ByteReader r(b);
    for (auto v : values) CHECK(r.varint() == v);
    CHECK(r.u32() == 0xDEADBEEF);
    CHECK(r.u64() == 0x0123456789ABCDEFull);
    CHECK(r.str() == "héllo");
    CHECK_NOTHROW(r.expect_done());
}

TEST_CASE("reader rejects truncated and oversized input") {
    const Bytes truncated = {0x80, 0x80};
    ByteReader r1(truncated);
    CHECK_THROWS_AS(r1.varint(), Error);

    ByteWriter w;
    w.varint(1000);  // claims a 1000-byte string with nothing after it
    const Bytes b = w.take();
    ByteReader r2(b);
    CHECK_THROWS_AS(r2.str(), Error);
}

TEST_CASE("hex round trip and validation") {
    const Bytes b = {0x00, 0x0f, 0xa5, 0xff};
    CHECK(to_hex(b) == "000fa5ff");
    CHECK(from_hex("000FA5ff") == b);
    CHECK_THROWS_AS(from_hex("abc"), Error);
    CHECK_THROWS_AS(from_hex("zz"), Error);
}

#ifdef _WIN32
TEST_CASE("long path prefixes") {
    CHECK(long_path(L"C:\\x") == L"\\\\?\\C:\\x");
    CHECK(long_path(L"\\\\srv\\share\\x") == L"\\\\?\\UNC\\srv\\share\\x");
    CHECK(strip_long_prefix(L"\\\\?\\UNC\\srv\\share") == L"\\\\srv\\share");
    CHECK(strip_long_prefix(L"\\\\?\\C:\\x") == L"C:\\x");
}

TEST_CASE("Windows paths") {
    CHECK(os::join("C:\\", "a") == "C:\\a");
    CHECK(os::join("C:\\x", "a") == "C:\\x\\a");
    CHECK(os::parent_of("C:\\x\\y") == "C:\\x");
    CHECK(os::parent_of("C:\\x") == "C:\\");
    CHECK(os::parent_of("C:\\").empty());
    CHECK(os::parent_of("\\\\srv\\share").empty());
    CHECK(os::parent_of("\\\\srv\\share\\d") == "\\\\srv\\share");
    CHECK(os::absolute_path("C:/a/b/") == "C:\\a\\b");
    CHECK(os::absolute_path("C:\\a\\..\\b") == "C:\\b");
    CHECK(os::absolute_path("C:\\") == "C:\\");
    CHECK(os::fold_case("\xc3\x84" "bC") == "\xc3\xa4" "bc");
}

TEST_CASE("snapshot path components (Windows)") {
    CHECK(os::path_components("c:\\Users\\me") == std::vector<std::string>{"C", "Users", "me"});
    CHECK(os::path_components("D:\\") == std::vector<std::string>{"D"});
    CHECK(os::path_components("\\\\srv\\share\\dir") == std::vector<std::string>{"UNC", "srv", "share", "dir"});
    CHECK(os::path_components("C:/a/b") == std::vector<std::string>{"C", "a", "b"});
}
#else
TEST_CASE("POSIX paths") {
    CHECK(os::join("/", "a") == "/a");
    CHECK(os::join("/x", "a") == "/x/a");
    CHECK(os::parent_of("/x/y") == "/x");
    CHECK(os::parent_of("/x") == "/");
    CHECK(os::parent_of("/").empty());
    CHECK(os::absolute_path("/a/./b/../c/") == "/a/c");
    CHECK(os::absolute_path("/../..") == "/");
    CHECK(os::absolute_path("rel").front() == '/');
    CHECK(os::fold_case("AbC") == "AbC");
}

TEST_CASE("snapshot path components (POSIX)") {
    CHECK(os::path_components("/home/me") == std::vector<std::string>{"home", "me"});
    CHECK(os::path_components("/").empty());
    CHECK_THROWS_AS(os::path_components("relative"), Error);
}
#endif

TEST_CASE("FILETIME and Unix time conversion") {
    CHECK(os::from_unix(0, 0) == os::kUnixEpochTicks);
    CHECK(os::from_unix(1, 500) == os::kUnixEpochTicks + 10000005);
}

namespace {

// Applies the edits to `a` and checks every Equal really is equal; returns the new sequence.
std::vector<std::string> apply_edits(const std::vector<std::string>& a, const std::vector<std::string>& b,
                                     const std::vector<DiffEdit>& edits, int& changes) {
    std::vector<std::string> out;
    changes = 0;
    std::size_t next_old = 0, next_new = 0;
    for (const DiffEdit& e : edits) {
        switch (e.op) {
            case DiffOp::Equal:
                CHECK(e.old_line == next_old++);
                CHECK(e.new_line == next_new++);
                CHECK(a[e.old_line] == b[e.new_line]);
                out.push_back(a[e.old_line]);
                break;
            case DiffOp::Delete:
                CHECK(e.old_line == next_old++);
                ++changes;
                break;
            case DiffOp::Insert:
                CHECK(e.new_line == next_new++);
                out.push_back(b[e.new_line]);
                ++changes;
                break;
        }
    }
    CHECK(next_old == a.size());
    CHECK(next_new == b.size());
    return out;
}

}  // namespace

TEST_CASE("line diff") {
    using V = std::vector<std::string>;
    int changes = 0;
    // The classic example: the shortest edit script has 5 steps.
    const V a = {"a", "b", "c", "a", "b", "b", "a"};
    const V b = {"c", "b", "a", "b", "a", "c"};
    auto d = line_diff(a, b);
    REQUIRE(d.has_value());
    CHECK(apply_edits(a, b, *d, changes) == b);
    CHECK(changes == 5);

    for (const auto& [x, y] : std::vector<std::pair<V, V>>{
             {{}, {}}, {{}, {"x"}}, {{"x"}, {}}, {{"same", "lines"}, {"same", "lines"}},
             {{"head", "old", "tail"}, {"head", "new", "tail"}}, {{"a", "b"}, {"c", "d", "e"}}}) {
        d = line_diff(x, y);
        REQUIRE(d.has_value());
        CHECK(apply_edits(x, y, *d, changes) == y);
    }
    CHECK(changes == 5);  // the last pair: 2 removed, 3 added

    // Random edits of a long text still give a valid script.
    std::mt19937 rng(42);
    V big;
    for (int i = 0; i < 3000; ++i) big.push_back("line " + std::to_string(rng() % 500));
    V edited = big;
    for (int i = 0; i < 60; ++i) {
        const std::size_t at = rng() % edited.size();
        if (rng() % 2) edited.erase(edited.begin() + static_cast<std::ptrdiff_t>(at));
        else edited.insert(edited.begin() + static_cast<std::ptrdiff_t>(at), "new " + std::to_string(i));
    }
    d = line_diff(big, edited);
    REQUIRE(d.has_value());
    CHECK(apply_edits(big, edited, *d, changes) == edited);
    CHECK(changes <= 60);

    // Too different for the limit: no script.
    CHECK_FALSE(line_diff(V{"a", "b", "c"}, V{"x", "y", "z"}, 2).has_value());
}
