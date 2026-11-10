// The change journal reader. file_ref works unelevated; the journal itself needs administrator
// rights, so those tests accept both outcomes.
#ifdef _WIN32

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <unordered_set>

#include "vaultik/platform/usn_journal.hpp"
#include "vaultik/util/error.hpp"

namespace fs = std::filesystem;
using namespace vaultik;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        std::random_device rd;
        path = fs::temp_directory_path() / ("vaultik-usn-" + std::to_string(rd()) + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    std::string str(const std::string& name = {}) const { return name.empty() ? path.string() : (path / name).string(); }
};

void write(const std::string& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << text;
}

}  // namespace

TEST_CASE("file_ref is stable and tells files and folders apart") {
    TempDir dir;
    fs::create_directory(dir.path / "sub");
    write(dir.str("a.txt"), "a");
    write(dir.str("b.txt"), "b");

    const os::FileRef a = os::file_ref(dir.str("a.txt"));
    CHECK(a == os::file_ref(dir.str("a.txt")));
    CHECK_FALSE(a == os::file_ref(dir.str("b.txt")));
    const os::FileRef sub = os::file_ref(dir.str("sub"));
    CHECK(sub == os::file_ref(dir.str("sub")));
    CHECK_FALSE(sub == a);
    CHECK_FALSE(sub == os::file_ref(dir.str()));

    // The id survives a change of content and a rename.
    write(dir.str("a.txt"), "changed");
    CHECK(a == os::file_ref(dir.str("a.txt")));
    fs::rename(dir.path / "a.txt", dir.path / "c.txt");
    CHECK(a == os::file_ref(dir.str("c.txt")));

    CHECK_THROWS_AS(os::file_ref(dir.str("missing")), Error);
}

TEST_CASE("FileRefHash works in a set") {
    TempDir dir;
    write(dir.str("a.txt"), "a");
    write(dir.str("b.txt"), "b");
    std::unordered_set<os::FileRef, os::FileRefHash> set;
    set.insert(os::file_ref(dir.str("a.txt")));
    set.insert(os::file_ref(dir.str("a.txt")));
    set.insert(os::file_ref(dir.str("b.txt")));
    CHECK(set.size() == 2);
    CHECK(set.count(os::file_ref(dir.str("b.txt"))) == 1);
    CHECK(set.count(os::FileRef{1, 2}) == 0);
}

TEST_CASE("the change journal reports touched files and their folders") {
    TempDir dir;
    fs::create_directory(dir.path / "sub");
    write(dir.str("sub\\one.txt"), "1");

    std::optional<os::UsnJournalState> before;
    try {
        before = os::usn_journal_state(dir.str());
    } catch (const Error& e) {
        // Not elevated: the volume cannot be opened.
        CHECK_THROWS_AS(os::usn_journal_state(dir.str()), Error);
        CHECK_THROWS_AS(os::usn_changes_since(dir.str(), 1, 0, 100), Error);
        MESSAGE("elevated part skipped (" << std::string(e.what()) << ")");
        return;
    }
    if (!before) {
        MESSAGE("elevated part skipped: this volume keeps no change journal");
        return;
    }

    write(dir.str("sub\\two.txt"), "2");
    write(dir.str("sub\\one.txt"), "changed");
    fs::rename(dir.path / "sub" / "two.txt", dir.path / "sub" / "three.txt");
    const auto after = os::usn_journal_state(dir.str());
    REQUIRE(after.has_value());
    CHECK(after->journal_id == before->journal_id);
    CHECK(after->next_usn > before->next_usn);

    const auto changes = os::usn_changes_since(dir.str(), before->journal_id, before->next_usn, after->next_usn);
    REQUIRE(changes.has_value());
    CHECK(changes->changed.count(os::file_ref(dir.str("sub\\one.txt"))) == 1);
    CHECK(changes->changed.count(os::file_ref(dir.str("sub\\three.txt"))) == 1);
    CHECK(changes->parents.count(os::file_ref(dir.str("sub"))) == 1);

    // A different journal id cannot answer.
    CHECK_FALSE(os::usn_changes_since(dir.str(), before->journal_id + 1, before->next_usn, after->next_usn));
}

#endif
