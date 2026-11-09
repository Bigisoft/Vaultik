#include "vaultik/ops/mount.hpp"

#include <windows.h>
#include <objbase.h>
#include <projectedfslib.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <vector>

#include "vaultik/model/tree.hpp"
#include "vaultik/platform/os.hpp"
#include "vaultik/util/win.hpp"

// ProjectedFSLib.dll exists only while the optional Windows feature is on, so programs using this
// file link it with /DELAYLOAD and call available() first.

namespace vaultik {

namespace {

struct Entry {
    std::wstring name;
    bool dir = false;
    std::uint64_t size = 0;
    std::uint64_t ctime = 0;
    std::uint64_t mtime = 0;
    std::uint32_t attributes = 0;
};

struct Session {
    std::vector<Entry> entries;  // in the file system's name order
    std::size_t next = 0;
    std::wstring search;
    bool search_set = false;
};

struct GuidLess {
    bool operator()(const GUID& a, const GUID& b) const { return std::memcmp(&a, &b, sizeof a) < 0; }
};

// Shown entries: files and folders with names that are safe here (no links or metadata-only
// items, which have no content to show).
bool shown(const Node& n) {
    return (n.kind == NodeKind::File || n.kind == NodeKind::Dir) && is_safe_name(n.name);
}

Entry entry_of(const Node& n) {
    Entry e;
    e.name = widen(n.name);
    e.dir = n.kind == NodeKind::Dir;
    e.size = e.dir ? 0 : n.size;
    e.ctime = n.ctime;
    e.mtime = n.mtime;
    // Read-only, so editors offer "save as" instead of changing a file in the view.
    e.attributes = (n.attributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE)) |
                   (e.dir ? 0u : static_cast<std::uint32_t>(FILE_ATTRIBUTE_READONLY));
    return e;
}

PRJ_FILE_BASIC_INFO basic_info(const Entry& e) {
    PRJ_FILE_BASIC_INFO info{};
    info.IsDirectory = e.dir ? TRUE : FALSE;
    info.FileSize = static_cast<INT64>(e.size);
    info.CreationTime.QuadPart = static_cast<LONGLONG>(e.ctime);
    info.LastWriteTime.QuadPart = static_cast<LONGLONG>(e.mtime);
    info.ChangeTime.QuadPart = static_cast<LONGLONG>(e.mtime);
    info.LastAccessTime.QuadPart = static_cast<LONGLONG>(e.mtime);
    info.FileAttributes = e.attributes;
    return info;
}

}  // namespace

struct SnapshotMount::Impl {
    Repository* repo = nullptr;  // null while only cleaning up a leftover folder
    Snapshot snapshot;
    std::string folder;
    std::wstring root;
    PRJ_NAMESPACE_VIRTUALIZATION_CONTEXT ctx = nullptr;
    std::mutex mu;  // the repository, the tree cache and the sessions
    std::map<Id, std::shared_ptr<const Tree>> trees;
    std::map<GUID, Session, GuidLess> sessions;
    // While removing the folder: nothing is projected any more and deleting is allowed.
    std::atomic<bool> closing{false};

    std::shared_ptr<const Tree> tree(const Id& id) {
        const auto it = trees.find(id);
        if (it != trees.end()) return it->second;
        auto t = std::make_shared<const Tree>(parse_tree(repo->load_blob(BlobType::Tree, id)));
        if (trees.size() >= 4096) trees.clear();  // a simple bound; folders are cheap to load again
        trees.emplace(id, t);
        return t;
    }

    // The item at a path relative to the root ("" is the root), names in any case. `canonical`
    // gets the path in the snapshot's own spelling.
    bool resolve(const wchar_t* path, Node& out, std::wstring* canonical) {
        Node cur;
        cur.kind = NodeKind::Dir;
        cur.subtree = snapshot.tree;
        if (canonical != nullptr) canonical->clear();
        const std::wstring_view p = path != nullptr ? path : L"";
        std::size_t start = 0;
        while (start < p.size()) {
            std::size_t end = p.find(L'\\', start);
            if (end == std::wstring_view::npos) end = p.size();
            const std::wstring_view part = p.substr(start, end - start);
            start = end + 1;
            if (part.empty()) continue;
            if (cur.kind != NodeKind::Dir) return false;
            const std::shared_ptr<const Tree> t = tree(cur.subtree);
            const std::string name = narrow(part);
            const Node* found = t->find(name);
            if (found == nullptr) {
                const std::string folded = os::fold_case(name);
                for (const Node& n : t->nodes)
                    if (os::fold_case(n.name) == folded) {
                        found = &n;
                        break;
                    }
            }
            if (found == nullptr || !shown(*found)) return false;
            cur = *found;
            if (canonical != nullptr) *canonical += (canonical->empty() ? L"" : L"\\") + widen(cur.name);
        }
        out = std::move(cur);
        return true;
    }
};

namespace {

SnapshotMount::Impl* impl_of(const PRJ_CALLBACK_DATA* data) {
    return static_cast<SnapshotMount::Impl*>(data->InstanceContext);
}

HRESULT CALLBACK start_enumeration(const PRJ_CALLBACK_DATA* data, const GUID* id) {
    SnapshotMount::Impl* m = impl_of(data);
    try {
        std::lock_guard<std::mutex> lock(m->mu);
        Session s;
        if (!m->closing && m->repo != nullptr) {
            Node dir;
            if (!m->resolve(data->FilePathName, dir, nullptr) || dir.kind != NodeKind::Dir)
                return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
            for (const Node& n : m->tree(dir.subtree)->nodes)
                if (shown(n)) s.entries.push_back(entry_of(n));
            std::sort(s.entries.begin(), s.entries.end(), [](const Entry& a, const Entry& b) {
                return PrjFileNameCompare(a.name.c_str(), b.name.c_str()) < 0;
            });
        }
        m->sessions[*id] = std::move(s);
        return S_OK;
    } catch (const std::exception&) {
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }
}

HRESULT CALLBACK get_enumeration(const PRJ_CALLBACK_DATA* data, const GUID* id, PCWSTR search,
                                 PRJ_DIR_ENTRY_BUFFER_HANDLE buffer) {
    SnapshotMount::Impl* m = impl_of(data);
    std::lock_guard<std::mutex> lock(m->mu);
    const auto it = m->sessions.find(*id);
    if (it == m->sessions.end()) return E_INVALIDARG;
    Session& s = it->second;
    if ((data->Flags & PRJ_CB_DATA_FLAG_ENUM_RESTART_SCAN) != 0) {
        s.next = 0;
        s.search_set = false;
    }
    if (!s.search_set) {
        s.search = search != nullptr && *search != L'\0' ? search : L"*";
        s.search_set = true;
    }
    bool added = false;
    for (; s.next < s.entries.size(); ++s.next) {
        const Entry& e = s.entries[s.next];
        if (!PrjFileNameMatch(e.name.c_str(), s.search.c_str())) continue;
        PRJ_FILE_BASIC_INFO info = basic_info(e);
        const HRESULT hr = PrjFillDirEntryBuffer(e.name.c_str(), &info, buffer);
        if (FAILED(hr)) return added ? S_OK : hr;  // full: the rest comes with the next call
        added = true;
    }
    return S_OK;
}

HRESULT CALLBACK end_enumeration(const PRJ_CALLBACK_DATA* data, const GUID* id) {
    SnapshotMount::Impl* m = impl_of(data);
    std::lock_guard<std::mutex> lock(m->mu);
    m->sessions.erase(*id);
    return S_OK;
}

HRESULT CALLBACK get_placeholder(const PRJ_CALLBACK_DATA* data) {
    SnapshotMount::Impl* m = impl_of(data);
    if (m->closing || m->repo == nullptr) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    try {
        std::lock_guard<std::mutex> lock(m->mu);
        Node n;
        std::wstring canonical;
        if (!m->resolve(data->FilePathName, n, &canonical)) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        PRJ_PLACEHOLDER_INFO info{};
        info.FileBasicInfo = basic_info(entry_of(n));
        return PrjWritePlaceholderInfo(m->ctx, canonical.c_str(), &info, sizeof info);
    } catch (const std::exception&) {
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }
}

HRESULT CALLBACK get_file_data(const PRJ_CALLBACK_DATA* data, UINT64 offset, UINT32 length) {
    SnapshotMount::Impl* m = impl_of(data);
    if (m->repo == nullptr) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    constexpr UINT32 kBlock = 4u << 20;  // written in pieces of this size (sector aligned)
    void* buffer = PrjAllocateAlignedBuffer(m->ctx, kBlock);
    if (buffer == nullptr) return E_OUTOFMEMORY;
    struct Free {
        void* p;
        ~Free() { PrjFreeAlignedBuffer(p); }
    } guard{buffer};
    auto* out = static_cast<std::uint8_t*>(buffer);
    try {
        std::lock_guard<std::mutex> lock(m->mu);
        Node n;
        if (!m->resolve(data->FilePathName, n, nullptr) || n.kind != NodeKind::File)
            return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        const UINT64 end = offset + length;
        UINT64 file_pos = 0;     // where the current chunk starts in the file
        UINT64 write_pos = offset;
        UINT32 filled = 0;
        for (const Id& id : n.content) {
            if (file_pos >= end) break;
            const BlobLocation* loc = m->repo->index().find(id);
            if (loc == nullptr) return HRESULT_FROM_WIN32(ERROR_FILE_CORRUPT);
            const UINT64 size = loc->raw_len;
            if (file_pos + size <= offset) {
                file_pos += size;
                continue;
            }
            const Bytes chunk = m->repo->load_blob(BlobType::Data, id);
            std::size_t from = offset > file_pos ? static_cast<std::size_t>(offset - file_pos) : 0;
            const std::size_t to = static_cast<std::size_t>(std::min<UINT64>(chunk.size(), end - file_pos));
            while (from < to) {
                const std::size_t n_copy = std::min<std::size_t>(to - from, kBlock - filled);
                std::memcpy(out + filled, chunk.data() + from, n_copy);
                filled += static_cast<UINT32>(n_copy);
                from += n_copy;
                if (filled == kBlock) {
                    const HRESULT hr = PrjWriteFileData(m->ctx, &data->DataStreamId, buffer, write_pos, filled);
                    if (FAILED(hr)) return hr;
                    write_pos += filled;
                    filled = 0;
                }
            }
            file_pos += size;
        }
        if (filled > 0) return PrjWriteFileData(m->ctx, &data->DataStreamId, buffer, write_pos, filled);
        return S_OK;
    } catch (const std::exception&) {
        return HRESULT_FROM_WIN32(ERROR_FILE_CORRUPT);  // damaged or unreadable backup data
    }
}

HRESULT CALLBACK notification(const PRJ_CALLBACK_DATA* data, BOOLEAN, PRJ_NOTIFICATION kind, PCWSTR,
                              PRJ_NOTIFICATION_PARAMETERS*) {
    if (impl_of(data)->closing) return S_OK;
    switch (kind) {
        case PRJ_NOTIFICATION_PRE_DELETE:
        case PRJ_NOTIFICATION_PRE_RENAME:
        case PRJ_NOTIFICATION_PRE_SET_HARDLINK:
            return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);  // a version is read-only
        default:
            return S_OK;
    }
}

[[noreturn]] void fail(const std::string& what, HRESULT hr) {
    throw Error(what + ": " + win32_message(static_cast<std::uint32_t>(HRESULT_CODE(hr))));
}

void start(SnapshotMount::Impl& m) {
    PRJ_CALLBACKS callbacks{};
    callbacks.StartDirectoryEnumerationCallback = start_enumeration;
    callbacks.GetDirectoryEnumerationCallback = get_enumeration;
    callbacks.EndDirectoryEnumerationCallback = end_enumeration;
    callbacks.GetPlaceholderInfoCallback = get_placeholder;
    callbacks.GetFileDataCallback = get_file_data;
    callbacks.NotificationCallback = notification;
    PRJ_NOTIFICATION_MAPPING mapping{};
    mapping.NotificationRoot = L"";
    mapping.NotificationBitMask = PRJ_NOTIFY_PRE_DELETE | PRJ_NOTIFY_PRE_RENAME | PRJ_NOTIFY_PRE_SET_HARDLINK;
    PRJ_STARTVIRTUALIZING_OPTIONS options{};
    options.NotificationMappings = &mapping;
    options.NotificationMappingsCount = 1;
    const HRESULT hr = PrjStartVirtualizing(m.root.c_str(), &callbacks, &m, &options, &m.ctx);
    if (FAILED(hr)) fail("cannot show the folder " + m.folder, hr);
}

// With the folder still running but nothing projected (closing), only what was actually written
// to disk is left to delete; then the projection stops and the folder goes.
bool tear_down(SnapshotMount::Impl& m) {
    m.closing = true;
    std::error_code ec;
    const std::filesystem::path root(m.root);
    if (m.ctx != nullptr) {
        for (const auto& child : std::filesystem::directory_iterator(root, ec)) std::filesystem::remove_all(child.path(), ec);
        PrjStopVirtualizing(m.ctx);
        m.ctx = nullptr;
    }
    std::filesystem::remove_all(root, ec);
    return !std::filesystem::exists(root, ec);
}

}  // namespace

bool SnapshotMount::available() {
    static const bool loaded = LoadLibraryExW(L"ProjectedFSLib.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32) != nullptr;
    return loaded;
}

const char* SnapshotMount::how_to_enable() {
    return "browsing a version needs the Windows Projected File System, which is turned off: turn on "
           "\"Windows Projected File System\" in Windows Features (optionalfeatures.exe), or run "
           "\"Enable-WindowsOptionalFeature -Online -FeatureName Client-ProjFS -NoRestart\" as administrator";
}

SnapshotMount::SnapshotMount(Repository& repo, const Snapshot& snapshot, std::string folder)
    : impl_(std::make_unique<Impl>()) {
    if (!available()) throw Error(how_to_enable());
    Impl& m = *impl_;
    m.repo = &repo;
    m.snapshot = snapshot;
    m.folder = os::absolute_path(folder);
    m.root = strip_long_prefix(widen(m.folder));
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(m.root), ec);
    if (ec) throw Error("cannot create " + m.folder + ": " + ec.message());
    if (!std::filesystem::is_empty(std::filesystem::path(m.root), ec))
        throw Error(m.folder + " is not empty; choose an empty or new folder");
    GUID instance{};
    if (FAILED(CoCreateGuid(&instance))) throw Error("cannot create an id for the folder");
    const HRESULT hr = PrjMarkDirectoryAsPlaceholder(m.root.c_str(), nullptr, nullptr, &instance);
    if (FAILED(hr)) fail("cannot prepare the folder " + m.folder, hr);
    start(m);
}

SnapshotMount::~SnapshotMount() {
    tear_down(*impl_);
}

const std::string& SnapshotMount::folder() const {
    return impl_->folder;
}

bool SnapshotMount::remove_leftover(const std::string& folder) {
    if (!available()) {
        std::error_code ec;
        std::filesystem::remove_all(std::filesystem::path(widen(folder)), ec);
        return !ec;
    }
    Impl m;
    m.folder = folder;
    m.root = strip_long_prefix(widen(os::absolute_path(folder)));
    m.closing = true;
    try {
        start(m);  // works on a folder that was a projection root; otherwise it is plain files
    } catch (const Error&) {
        m.ctx = nullptr;
    }
    return tear_down(m);
}

}  // namespace vaultik
