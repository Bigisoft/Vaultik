#include <doctest/doctest.h>

#include "vaultik/model/snapshot.hpp"
#include "vaultik/model/tree.hpp"
#include "vaultik/repo/config.hpp"
#include "vaultik/repo/index.hpp"
#include "vaultik/repo/pack.hpp"

using namespace vaultik;

namespace {

Id id_of(std::uint8_t b) {
    Id id{};
    id.fill(b);
    return id;
}

}  // namespace

TEST_CASE("tree round trip and sorting") {
    Tree t;
    Node f;
    f.name = "zeta.txt";
    f.kind = NodeKind::File;
    f.size = 10;
    f.mtime = 123;
    f.content = {id_of(1), id_of(2)};
    Node d;
    d.name = "alpha";
    d.kind = NodeKind::Dir;
    d.subtree = id_of(3);
    Node l;
    l.name = "link";
    l.kind = NodeKind::Reparse;
    t.nodes = {f, d, l};
    t.sort();

    const Tree back = parse_tree(serialize_tree(t));
    REQUIRE(back.nodes.size() == 3);
    CHECK(back.nodes[0].name == "alpha");
    CHECK(back.nodes[0].subtree == id_of(3));
    CHECK(back.find("zeta.txt") != nullptr);
    CHECK(back.find("zeta.txt")->content.size() == 2);
    CHECK(back.find("missing") == nullptr);
}

TEST_CASE("unsorted trees are rejected") {
    Tree t;
    Node a;
    a.name = "b";
    Node b;
    b.name = "a";
    t.nodes = {a, b};  // not sorted
    CHECK_THROWS_AS(parse_tree(serialize_tree(t)), Error);
}

TEST_CASE("unsafe restore names") {
    CHECK(is_safe_name("report.docx"));
    CHECK(is_safe_name("тест"));
    CHECK_FALSE(is_safe_name(""));
    CHECK_FALSE(is_safe_name(".."));
    CHECK_FALSE(is_safe_name("a/b"));
    CHECK_FALSE(is_safe_name(std::string_view("a\0b", 3)));
#ifdef _WIN32
    CHECK_FALSE(is_safe_name("a\\b"));
    CHECK_FALSE(is_safe_name("C:"));
#else
    CHECK(is_safe_name("a\\b"));  // legal file names on POSIX
    CHECK(is_safe_name("C:"));
#endif
}

TEST_CASE("tree v2 keeps POSIX metadata and symlink targets") {
    Tree t;
    Node f;
    f.name = "script.sh";
    f.kind = NodeKind::File;
    f.mode = 0755;
    f.uid = 1000;
    f.gid = 100;
    f.content = {id_of(7)};
    Node l;
    l.name = "current";
    l.kind = NodeKind::Symlink;
    l.link_target = "releases/v2";
    t.nodes = {f, l};
    t.sort();

    const Tree back = parse_tree(serialize_tree(t));
    REQUIRE(back.nodes.size() == 2);
    const Node* bl = back.find("current");
    REQUIRE(bl != nullptr);
    CHECK(bl->kind == NodeKind::Symlink);
    CHECK(bl->link_target == "releases/v2");
    const Node* bf = back.find("script.sh");
    REQUIRE(bf != nullptr);
    CHECK(bf->mode == 0755);
    CHECK(bf->uid == 1000);
    CHECK(bf->gid == 100);
}

TEST_CASE("tree v1 (phase 1 repositories) still parses") {
    ByteWriter w;
    w.varint(1);  // version
    w.varint(1);  // one node
    w.str("old.txt");
    w.u8(static_cast<std::uint8_t>(NodeKind::File));
    w.u32(0x20);  // FILE_ATTRIBUTE_ARCHIVE
    w.u64(11);    // ctime
    w.u64(22);    // mtime
    w.u64(5);     // size
    w.varint(1);
    w.raw(id_of(9));
    const Tree t = parse_tree(w.take());
    REQUIRE(t.nodes.size() == 1);
    CHECK(t.nodes[0].name == "old.txt");
    CHECK(t.nodes[0].attributes == 0x20);
    CHECK(t.nodes[0].mode == 0);
    CHECK(t.nodes[0].mtime == 22);
    CHECK(t.nodes[0].content[0] == id_of(9));

    ByteWriter bad;
    bad.varint(1);
    bad.varint(1);
    bad.str("x");
    bad.u8(static_cast<std::uint8_t>(NodeKind::Symlink));  // not a v1 kind
    CHECK_THROWS_AS(parse_tree(bad.take()), Error);
}

TEST_CASE("snapshot round trip") {
    Snapshot s;
    s.time = 42;
    s.hostname = "host";
    s.username = "me";
    s.paths = {"C:\\data"};
    s.tree = id_of(7);
    s.parent = id_of(8);
    s.tags = {"daily"};
    s.stats.files = 5;
    s.stats.skipped = 2;
    const Snapshot back = parse_snapshot(serialize_snapshot(s));
    CHECK(back.hostname == "host");
    CHECK(back.paths == s.paths);
    CHECK(back.parent == s.parent);
    CHECK(back.tags == s.tags);
    CHECK(back.stats.files == 5);
    CHECK(back.stats.skipped == 2);
}

TEST_CASE("index file round trip") {
    IndexFileContent c;
    BlobEntry e;
    e.type = BlobType::Tree;
    e.id = id_of(9);
    e.offset = 1234;
    e.length = 99;
    e.raw_len = 80;
    c.emplace_back(id_of(1), std::vector<BlobEntry>{e});
    const IndexFileContent back = parse_index_file(serialize_index_file(c));
    REQUIRE(back.size() == 1);
    CHECK(back[0].first == id_of(1));
    REQUIRE(back[0].second.size() == 1);
    CHECK(back[0].second[0].offset == 1234);
    CHECK(back[0].second[0].type == BlobType::Tree);
}

TEST_CASE("pack writer produces a self-describing pack") {
    Key32 key;
    random_bytes(key);
    AesGcm aead(key);
    PackWriter w;
    const Bytes b1 = aead.seal(as_bytes("one"), {});
    const Bytes b2 = aead.seal(as_bytes("two!"), {});
    w.add(BlobType::Data, id_of(1), b1, 3);
    w.add(BlobType::Tree, id_of(2), b2, 4);
    std::vector<BlobEntry> entries;
    const Bytes pack = w.finish(aead, entries);
    CHECK(w.empty());
    REQUIRE(entries.size() == 2);

    const auto parsed = parse_pack_header(pack, aead);
    REQUIRE(parsed.size() == 2);
    CHECK(parsed[1].offset == b1.size());
    CHECK(parsed[1].type == BlobType::Tree);
    const ByteSpan blob2 = ByteSpan(pack).subspan(parsed[1].offset, parsed[1].length);
    CHECK(aead.open(blob2, {}) == Bytes{'t', 'w', 'o', '!'});
}

TEST_CASE("config key wrap") {
    RepoConfig c;
    c.kdf_iterations = 1000;
    random_bytes(c.salt);
    MasterKey k;
    random_bytes(k.enc);
    random_bytes(k.id);
    wrap_master_key(c, k, "correct horse");

    const RepoConfig back = parse_config(serialize_config(c));
    const MasterKey got = unwrap_master_key(back, "correct horse");
    CHECK(got.enc == k.enc);
    CHECK(got.id == k.id);
    CHECK_THROWS_AS(unwrap_master_key(back, "wrong"), AuthError);

    // Changing a header parameter breaks the key wrap.
    RepoConfig changed = back;
    changed.zstd_level = 19;
    CHECK_THROWS_AS(unwrap_master_key(changed, "correct horse"), AuthError);
}
