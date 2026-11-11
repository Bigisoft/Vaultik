// Windows implementation of the OS layer.

#include "vaultik/platform/os.hpp"

#include <algorithm>
#include <thread>
#include <map>
#include <atomic>
#include <mutex>
#include <cstring>
#include <limits>
#include <vector>

#include "vaultik/util/resources.hpp"
#include "vaultik/util/win.hpp"
#include "vaultik/util/win_handle.hpp"

// After <windows.h> (pulled in by win_handle.hpp).
#include <dpapi.h>
#include <netlistmgr.h>
#include <shellapi.h>
#include <shlobj.h>
#include <winioctl.h>

namespace vaultik::os {

namespace {

std::wstring native(const std::string& path) {
    return long_path(widen(path));
}

HANDLE as_handle(std::intptr_t h) {
    return reinterpret_cast<HANDLE>(h);
}

bool is_link_tag(DWORD tag) {
    return tag == IO_REPARSE_TAG_SYMLINK || tag == IO_REPARSE_TAG_MOUNT_POINT;
}

// Cloud files (OneDrive and similar) that are not on this disk; reading them would download them.
bool is_cloud_placeholder(DWORD attrs) {
    return (attrs & (FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_OFFLINE)) !=
           0;
}

EntryType type_of(DWORD attrs, DWORD reparse_tag) {
    if (((attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0 && is_link_tag(reparse_tag)) || is_cloud_placeholder(attrs))
        return EntryType::Other;
    return (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0 ? EntryType::Dir : EntryType::File;
}

FileInfo info_from(DWORD attrs, DWORD reparse_tag, const FILETIME& created, const FILETIME& written, DWORD size_high,
                   DWORD size_low) {
    FileInfo info;
    info.type = type_of(attrs, reparse_tag);
    info.attributes = attrs;
    info.ctime = filetime_to_u64(created);
    info.mtime = filetime_to_u64(written);
    info.size = info.type == EntryType::File ? (static_cast<std::uint64_t>(size_high) << 32) | size_low : 0;
    return info;
}

// "C:" / "C:\" / "\\server\share" (with or without a trailing backslash).
bool is_root(std::string_view p) {
    if (p.size() <= 3 && p.size() >= 2 && p[1] == ':') return true;
    if (p.starts_with("\\\\")) {
        std::string_view rest = p.substr(2);
        while (!rest.empty() && rest.back() == '\\') rest.remove_suffix(1);
        const std::size_t sep = rest.find('\\');
        return sep == std::string_view::npos || rest.find('\\', sep + 1) == std::string_view::npos;
    }
    return false;
}

constexpr DWORD kRestorableAttributes = FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM |
                                        FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED;

}  // namespace

// ---- errors -------------------------------------------------------------------------------

ErrorCode last_error() {
    return GetLastError();
}

std::string error_message(ErrorCode code) {
    return win32_message(code);
}

bool is_not_found(ErrorCode code) {
    return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND;
}

bool is_exists(ErrorCode code) {
    return code == ERROR_FILE_EXISTS || code == ERROR_ALREADY_EXISTS;
}

bool is_not_supported(ErrorCode code) {
    return code == ERROR_NOT_SUPPORTED;
}

void throw_error(const std::string& what, ErrorCode code) {
    throw OsError(what, code);
}

void throw_last_error(const std::string& what) {
    throw OsError(what, GetLastError());
}

// ---- paths --------------------------------------------------------------------------------

std::string absolute_path(std::string_view path) {
    std::string out = narrow(full_path(widen(path)));
    std::replace(out.begin(), out.end(), '/', '\\');
    while (out.size() > 3 && out.back() == '\\') out.pop_back();
    if (out.size() == 2 && out[1] == ':') out.push_back('\\');
    return out;
}

std::string join(std::string_view dir, std::string_view name) {
    std::string out(dir);
    if (!out.empty() && out.back() != '\\') out.push_back('\\');
    out.append(name);
    return out;
}

std::string parent_of(std::string_view absolute) {
    if (is_root(absolute)) return {};
    const std::size_t last = absolute.find_last_of('\\');
    if (last == std::string_view::npos) return {};
    std::string parent(absolute.substr(0, last));
    if (parent.size() == 2 && parent[1] == ':') parent.push_back('\\');
    return parent;
}

std::vector<std::string> path_components(std::string_view absolute) {
    std::string p = narrow(strip_long_prefix(widen(absolute)));
    std::replace(p.begin(), p.end(), '/', '\\');
    std::vector<std::string> comps;
    std::size_t pos = 0;
    if (p.starts_with("\\\\")) {
        comps.emplace_back("UNC");
        pos = 2;
    } else if (p.size() >= 2 && p[1] == ':') {
        const char drive = p[0];
        comps.emplace_back(1, (drive >= 'a' && drive <= 'z') ? static_cast<char>(drive - 'a' + 'A') : drive);
        pos = 2;
    } else {
        throw Error("not an absolute path: " + p);
    }
    while (pos < p.size()) {
        while (pos < p.size() && p[pos] == '\\') ++pos;
        const std::size_t end = p.find('\\', pos);
        std::string part = p.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        if (!part.empty()) comps.push_back(std::move(part));
        if (end == std::string::npos) break;
        pos = end;
    }
    return comps;
}

std::string fold_case(std::string_view utf8) {
    return narrow(to_lower(widen(utf8)));
}

// ---- metadata -----------------------------------------------------------------------------

bool stat(const std::string& path, FileInfo& info, ErrorCode& err) {
    const std::wstring p = native(path);
    WIN32_FILE_ATTRIBUTE_DATA ad;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &ad)) {
        err = GetLastError();
        return false;
    }
    DWORD tag = 0;
    if (ad.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        // Only the find data carries the reparse tag.
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW(p.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
        if (h != INVALID_HANDLE_VALUE) {
            tag = fd.dwReserved0;
            FindClose(h);
        }
    }
    info = info_from(ad.dwFileAttributes, tag, ad.ftCreationTime, ad.ftLastWriteTime, ad.nFileSizeHigh,
                     ad.nFileSizeLow);
    err = 0;
    return true;
}

bool is_directory(const std::string& path) {
    const DWORD attrs = GetFileAttributesW(native(path).c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool exists(const std::string& path) {
    return GetFileAttributesW(native(path).c_str()) != INVALID_FILE_ATTRIBUTES;
}

struct DirReader::Impl {
    HANDLE handle = INVALID_HANDLE_VALUE;
    WIN32_FIND_DATAW fd{};
    bool pending = false;  // fd holds an entry not returned yet
};

DirReader::DirReader(const std::string& path) : impl_(std::make_unique<Impl>()), path_(path) {
    impl_->handle = FindFirstFileExW(native(join(path, "*")).c_str(), FindExInfoBasic, &impl_->fd,
                                     FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (impl_->handle == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        if (err != ERROR_FILE_NOT_FOUND) error_ = err;  // an empty volume root reports "not found"
    } else {
        impl_->pending = true;
    }
}

DirReader::~DirReader() {
    if (impl_->handle != INVALID_HANDLE_VALUE) FindClose(impl_->handle);
}

bool DirReader::next(std::string& name, FileInfo& info) {
    if (impl_->handle == INVALID_HANDLE_VALUE) return false;
    for (;;) {
        if (!impl_->pending) {
            if (!FindNextFileW(impl_->handle, &impl_->fd)) {
                const DWORD err = GetLastError();
                if (err != ERROR_NO_MORE_FILES) error_ = err;
                FindClose(impl_->handle);
                impl_->handle = INVALID_HANDLE_VALUE;
                return false;
            }
        }
        impl_->pending = false;
        const WIN32_FIND_DATAW& fd = impl_->fd;
        const std::wstring_view wn = fd.cFileName;
        if (wn == L"." || wn == L"..") continue;
        name = narrow(wn);
        info = info_from(fd.dwFileAttributes, fd.dwReserved0, fd.ftCreationTime, fd.ftLastWriteTime,
                         fd.nFileSizeHigh, fd.nFileSizeLow);
        return true;
    }
}

// ---- files --------------------------------------------------------------------------------

File::~File() {
    close();
}

File::File(File&& other) noexcept : h_(other.h_) {
    other.h_ = -1;
}

File& File::operator=(File&& other) noexcept {
    if (this != &other) {
        close();
        h_ = other.h_;
        other.h_ = -1;
    }
    return *this;
}

namespace {

std::intptr_t open_handle(const std::string& path, DWORD access, DWORD share, DWORD disposition, DWORD flags,
                          ErrorCode& err) {
    HANDLE h = CreateFileW(native(path).c_str(), access, share, nullptr, disposition, flags, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        err = GetLastError();
        return -1;
    }
    err = 0;
    apply_io_priority(h);
    return reinterpret_cast<std::intptr_t>(h);
}

}  // namespace

File File::open_read(const std::string& path, ErrorCode& err) {
    return File(open_handle(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OPEN_EXISTING,
                            FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_BACKUP_SEMANTICS, err));
}

File File::open_source(const std::string& path, ErrorCode& err) {
    constexpr DWORD kShare = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    constexpr DWORD kFlags = FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_BACKUP_SEMANTICS;
    // Open the entry itself, not what it may point to: it was a regular file when it was scanned,
    // and if it has been swapped for a link since, the backup must not follow it with its
    // (possibly elevated) rights.
    File f(open_handle(path, GENERIC_READ, kShare, OPEN_EXISTING, kFlags | FILE_FLAG_OPEN_REPARSE_POINT, err));
    if (!f.valid()) return f;
    FILE_ATTRIBUTE_TAG_INFO tag{};
    if (!GetFileInformationByHandleEx(as_handle(f.h_), FileAttributeTagInfo, &tag, sizeof tag)) {
        err = GetLastError();
        return File();
    }
    if ((tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) return f;
    if (is_link_tag(tag.ReparseTag)) {
        err = ERROR_STOPPED_ON_SYMLINK;
        return File();
    }
    // Other reparse points (deduplicated or WOF-compressed files) need their file system filter
    // to read the content: open again normally and check that it is still the same file.
    FILE_ID_INFO before{};
    const bool have_id = GetFileInformationByHandleEx(as_handle(f.h_), FileIdInfo, &before, sizeof before) != 0;
    File g(open_handle(path, GENERIC_READ, kShare, OPEN_EXISTING, kFlags, err));
    if (!g.valid() || !have_id) return g;
    FILE_ID_INFO after{};
    if (!GetFileInformationByHandleEx(as_handle(g.h_), FileIdInfo, &after, sizeof after) ||
        after.VolumeSerialNumber != before.VolumeSerialNumber ||
        std::memcmp(&after.FileId, &before.FileId, sizeof after.FileId) != 0) {
        err = ERROR_STOPPED_ON_SYMLINK;
        return File();
    }
    return g;
}

File File::open_random(const std::string& path, ErrorCode& err) {
    return File(open_handle(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, err));
}

File File::open_append(const std::string& path, ErrorCode& err, std::uint32_t /*mode*/) {
    return File(open_handle(path, FILE_APPEND_DATA | SYNCHRONIZE, FILE_SHARE_READ, OPEN_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, err));
}

File File::create(const std::string& path, bool overwrite, ErrorCode& err, std::uint32_t /*mode*/) {
    constexpr DWORD kFlags = FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN;
    // CREATE_NEW never follows a link at `path` (it fails because something exists). To overwrite,
    // the old entry is deleted first rather than truncated: DeleteFileW removes a link itself, not
    // its target, and a fresh file cannot be a hard link to some other file.
    for (int attempt = 0; attempt < 3; ++attempt) {
        File f(open_handle(path, GENERIC_WRITE | DELETE, 0, CREATE_NEW, kFlags, err));
        if (f.valid() || !overwrite || !is_exists(err)) return f;
        if (!DeleteFileW(native(path).c_str())) {
            err = GetLastError();
            if (!is_not_found(err)) return File();  // a folder, or not ours to delete
        }
    }
    return File();
}

File File::create_directory(const std::string& path, ErrorCode& err) {
    // NtCreateFile is the one call that both creates a folder and returns a handle to it.
    using NtCreateFileFn = LONG(NTAPI*)(PHANDLE, ACCESS_MASK, PVOID, PVOID, PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG,
                                        PVOID, ULONG);
    using StatusToError = ULONG(NTAPI*)(LONG);
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto nt_create = reinterpret_cast<NtCreateFileFn>(
        reinterpret_cast<void*>(GetProcAddress(ntdll, "NtCreateFile")));
    const auto to_error = reinterpret_cast<StatusToError>(
        reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlNtStatusToDosError")));
    if (nt_create == nullptr || to_error == nullptr) {
        err = ERROR_NOT_SUPPORTED;
        return File();
    }
    // "\\?\C:\x" -> "\??\C:\x" (and "\\?\UNC\..." -> "\??\UNC\..."), the NT name of the path.
    std::wstring name = native(path);
    if (name.starts_with(L"\\\\?\\")) name[1] = L'?';
    if (name.size() * sizeof(wchar_t) > 65534) {  // the most a UNICODE_STRING holds
        err = ERROR_FILENAME_EXCED_RANGE;
        return File();
    }
    struct UnicodeString {
        USHORT Length;
        USHORT MaximumLength;
        PWSTR Buffer;
    } us{static_cast<USHORT>(name.size() * sizeof(wchar_t)), static_cast<USHORT>(name.size() * sizeof(wchar_t)),
         name.data()};
    struct ObjectAttributes {
        ULONG Length;
        HANDLE RootDirectory;
        UnicodeString* ObjectName;
        ULONG Attributes;
        PVOID SecurityDescriptor;
        PVOID SecurityQualityOfService;
    } oa{sizeof(ObjectAttributes), nullptr, &us, 0x40 /* OBJ_CASE_INSENSITIVE */, nullptr, nullptr};
    struct IoStatus {
        ULONG_PTR Status;
        ULONG_PTR Information;
    } iosb{};
    constexpr ULONG kFileCreate = 2;
    constexpr ULONG kOptions = 0x00000001 /* FILE_DIRECTORY_FILE */ | 0x00000020 /* FILE_SYNCHRONOUS_IO_NONALERT */ |
                               0x00200000 /* FILE_OPEN_REPARSE_POINT */;
    // FILE_TRAVERSE takes part in sharing checks, so with no delete or write sharing nobody can
    // rename, delete or turn the folder into a junction while it is open; DELETE lets a folder
    // made in the wrong place be removed again.
    const ACCESS_MASK base = SYNCHRONIZE | READ_CONTROL | WRITE_DAC | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES |
                             FILE_TRAVERSE | DELETE;
    // With the right to set another owner (elevated) ask for it too; without, the folder is made
    // with what the account may have.
    for (const ACCESS_MASK access : {base | WRITE_OWNER, base}) {
        HANDLE h = nullptr;
        const LONG status = nt_create(&h, access, &oa, &iosb, nullptr, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ,
                                      kFileCreate, kOptions, nullptr, 0);
        if (status >= 0) {
            err = 0;
            return File(reinterpret_cast<std::intptr_t>(h));
        }
        err = to_error(status);
        // Refused for the owner right: try again without it. Anything else is the answer.
        if (err != ERROR_ACCESS_DENIED && err != ERROR_PRIVILEGE_NOT_HELD && err != ERROR_INVALID_OWNER) break;
    }
    return File();
}

File File::hold_directory(const std::string& path, bool real_folder, ErrorCode& err) {
    // Traverse access takes part in sharing checks, and without delete or write sharing nobody
    // can open the folder to rename or delete it, or to make it a junction, while this is open.
    File f(open_handle(path, FILE_TRAVERSE | SYNCHRONIZE, FILE_SHARE_READ, OPEN_EXISTING,
                       FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, err));
    if (!f.valid() || !real_folder) return f;
    FILE_ATTRIBUTE_TAG_INFO tag{};
    if (!GetFileInformationByHandleEx(as_handle(f.h_), FileAttributeTagInfo, &tag, sizeof tag)) {
        err = GetLastError();
        return File();
    }
    if ((tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 || (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        err = ERROR_STOPPED_ON_SYMLINK;
        return File();
    }
    return f;
}

File File::open_for_delete(const std::string& path, ErrorCode& err) {
    return File(open_handle(path, DELETE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OPEN_EXISTING,
                            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, err));
}

std::string File::final_path() const {
    if (!valid()) return {};
    std::wstring buf(1024, L'\0');
    DWORD n = GetFinalPathNameByHandleW(as_handle(h_), buf.data(), static_cast<DWORD>(buf.size()), FILE_NAME_NORMALIZED);
    if (n >= buf.size()) {
        buf.resize(n + 1);
        n = GetFinalPathNameByHandleW(as_handle(h_), buf.data(), static_cast<DWORD>(buf.size()), FILE_NAME_NORMALIZED);
    }
    if (n == 0 || n >= buf.size()) return {};
    buf.resize(n);
    return narrow(strip_long_prefix(buf));
}

ErrorCode File::discard() {
    if (!valid()) return ERROR_INVALID_HANDLE;
    // FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE.
    FILE_DISPOSITION_INFO_EX ex{0x1 | 0x10};
    if (SetFileInformationByHandle(as_handle(h_), FileDispositionInfoEx, &ex, sizeof ex)) return 0;
    // Older Windows: clear read-only first, then the plain way.
    FILE_BASIC_INFO basic{};
    if (GetFileInformationByHandleEx(as_handle(h_), FileBasicInfo, &basic, sizeof basic) &&
        (basic.FileAttributes & FILE_ATTRIBUTE_READONLY) != 0) {
        basic.FileAttributes &= ~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY);
        if (basic.FileAttributes == 0) basic.FileAttributes = FILE_ATTRIBUTE_NORMAL;
        SetFileInformationByHandle(as_handle(h_), FileBasicInfo, &basic, sizeof basic);
    }
    FILE_DISPOSITION_INFO plain{TRUE};
    if (SetFileInformationByHandle(as_handle(h_), FileDispositionInfo, &plain, sizeof plain)) return 0;
    return GetLastError();
}

ErrorCode File::set_security(ByteSpan descriptor) {
    if (!valid()) return ERROR_INVALID_HANDLE;
    if (descriptor.empty()) return 0;
    Bytes sd(descriptor.begin(), descriptor.end());
    auto* psd = reinterpret_cast<PSECURITY_DESCRIPTOR>(sd.data());
    if (!IsValidSecurityDescriptor(psd)) return ERROR_INVALID_SECURITY_DESCR;
    if (SetKernelObjectSecurity(as_handle(h_), OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION |
                                                    DACL_SECURITY_INFORMATION, psd))
        return 0;
    if (SetKernelObjectSecurity(as_handle(h_), DACL_SECURITY_INFORMATION, psd)) return 0;
    return GetLastError();
}

std::optional<FileId> File::id() const {
    FILE_ID_INFO info{};
    if (!valid() || !GetFileInformationByHandleEx(as_handle(h_), FileIdInfo, &info, sizeof info)) return std::nullopt;
    FileId out;
    out.volume = info.VolumeSerialNumber;
    std::memcpy(out.id.data(), info.FileId.Identifier, out.id.size());
    return out;
}

File File::create_stream(const std::string& path, const std::string& name, const FileId& expected,
                         ErrorCode& err) {
    // CREATE_NEW: the file was just restored, so the stream cannot exist yet; if it does, the
    // file is not ours and an existing stream of someone else's file is never truncated.
    File f(open_handle(path + ":" + name, GENERIC_WRITE | DELETE, 0, CREATE_NEW,
                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, err));
    if (!f.valid()) return f;
    FILE_ATTRIBUTE_TAG_INFO tag{};
    FILE_STANDARD_INFO standard{};
    const bool known = GetFileInformationByHandleEx(as_handle(f.h_), FileAttributeTagInfo, &tag, sizeof tag) &&
                       GetFileInformationByHandleEx(as_handle(f.h_), FileStandardInfo, &standard, sizeof standard);
    if (known && (tag.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) == 0 &&
        standard.NumberOfLinks == 1 && f.id() == expected)
        return f;
    err = known ? ERROR_STOPPED_ON_SYMLINK : GetLastError();
    // Not our file after all: take the stream just made away again.
    FILE_DISPOSITION_INFO gone{TRUE};
    SetFileInformationByHandle(as_handle(f.h_), FileDispositionInfo, &gone, sizeof gone);
    return File();
}

bool File::valid() const {
    return h_ != -1 && h_ != 0;
}

void File::close() {
    if (valid()) CloseHandle(as_handle(h_));
    h_ = -1;
}

std::size_t File::read(std::uint8_t* dst, std::size_t max) {
    DWORD got = 0;
    const DWORD want = static_cast<DWORD>(std::min<std::size_t>(max, 1u << 30));
    if (!ReadFile(as_handle(h_), dst, want, &got, nullptr)) throw_last_error("read failed");
    throttle_read(got);
    return got;
}

void File::read_at(std::uint64_t offset, std::uint8_t* dst, std::size_t length) {
    std::size_t done = 0;
    while (done < length) {
        const DWORD want = static_cast<DWORD>(std::min<std::size_t>(length - done, 16u << 20));
        OVERLAPPED ov{};
        const std::uint64_t pos = offset + done;
        ov.Offset = static_cast<DWORD>(pos & 0xFFFFFFFFu);
        ov.OffsetHigh = static_cast<DWORD>(pos >> 32);
        DWORD got = 0;
        if (!ReadFile(as_handle(h_), dst + done, want, &got, &ov)) {
            const DWORD err = GetLastError();
            if (err == ERROR_HANDLE_EOF) throw Error("file is truncated");
            throw_error("read failed", err);
        }
        if (got == 0) throw Error("file is truncated");
        done += got;
        throttle_read(got);
    }
}

void File::write_all(ByteSpan data) {
    std::size_t done = 0;
    while (done < data.size()) {
        const DWORD want = static_cast<DWORD>(std::min<std::size_t>(data.size() - done, 16u << 20));
        DWORD wrote = 0;
        if (!WriteFile(as_handle(h_), data.data() + done, want, &wrote, nullptr)) throw_last_error("write failed");
        done += wrote;
        throttle_write(wrote);
    }
}

void File::sync() {
    if (!FlushFileBuffers(as_handle(h_))) throw_last_error("flush failed");
}

void File::preallocate(std::uint64_t size) {
    FILE_ALLOCATION_INFO alloc{};
    alloc.AllocationSize.QuadPart = static_cast<LONGLONG>(size);
    SetFileInformationByHandle(as_handle(h_), FileAllocationInfo, &alloc, sizeof alloc);
}

ErrorCode make_dir(const std::string& path) {
    return CreateDirectoryW(native(path).c_str(), nullptr) ? 0 : GetLastError();
}

void rename_replace(const std::string& from, const std::string& to) {
    // POSIX semantics: the target is replaced even while other processes have it open (with
    // FILE_SHARE_DELETE, as Vaultik and most readers do); MoveFileEx refuses that with access
    // denied. NTFS supports it; elsewhere (FAT, exFAT, some shares) the plain move follows.
    const std::wstring target = native(to);
    HANDLE h = CreateFileW(native(from).c_str(), DELETE | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        const std::size_t name_bytes = target.size() * sizeof(wchar_t);
        std::vector<std::uint8_t> buffer(sizeof(FILE_RENAME_INFO) + name_bytes);
        auto* info = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
        info->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
        info->RootDirectory = nullptr;
        info->FileNameLength = static_cast<DWORD>(name_bytes);
        std::memcpy(info->FileName, target.data(), name_bytes);
        const BOOL renamed = SetFileInformationByHandle(h, FileRenameInfoEx, info, static_cast<DWORD>(buffer.size()));
        CloseHandle(h);
        if (renamed) return;
    }
    if (!MoveFileExW(native(from).c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw_last_error("cannot finalize " + to);
}

ErrorCode remove_file(const std::string& path) {
    return DeleteFileW(native(path).c_str()) ? 0 : GetLastError();
}

void make_writable(const std::string& path) {
    SetFileAttributesW(native(path).c_str(), FILE_ATTRIBUTE_NORMAL);
}

void set_metadata(const std::string& path, const FileInfo& info) {
    const DWORD attrs = info.attributes & kRestorableAttributes;
    if (info.mtime == 0 && info.ctime == 0 && attrs == 0) return;
    // Through a handle on the entry itself, so a path swapped for a link cannot redirect it.
    UniqueHandle h(CreateFileW(native(path).c_str(), FILE_WRITE_ATTRIBUTES,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!h.valid()) return;
    FILE_BASIC_INFO basic{};  // zero fields are left unchanged
    basic.CreationTime.QuadPart = static_cast<LONGLONG>(info.ctime);
    basic.LastWriteTime.QuadPart = static_cast<LONGLONG>(info.mtime);
    basic.FileAttributes = attrs;
    SetFileInformationByHandle(h.get(), FileBasicInfo, &basic, sizeof basic);
}

ErrorCode make_symlink(const std::string& target, const std::string& path) {
    std::string t = target;
    std::replace(t.begin(), t.end(), '/', '\\');
    // A directory link needs its own flag; resolve relative targets against the link's folder.
    const bool absolute = (t.size() >= 2 && t[1] == ':') || t.starts_with("\\\\");
    const std::string resolved = absolute ? t : join(parent_of(path), t);
    DWORD flags = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
    if (is_directory(resolved)) flags |= SYMBOLIC_LINK_FLAG_DIRECTORY;
    return CreateSymbolicLinkW(native(path).c_str(), widen(t).c_str(), flags) ? 0 : GetLastError();
}

ErrorCode read_symlink(const std::string& /*path*/, std::string& /*target*/) {
    return ERROR_NOT_SUPPORTED;  // Windows links are stored as metadata only
}

bool is_pseudo_filesystem(const std::string& /*path*/) {
    return false;
}

VolumeLocation volume_of(const std::string& path) {
    VolumeLocation loc;
    const std::wstring full = full_path(widen(path));
    if (full.rfind(L"\\\\", 0) == 0) return loc;  // a network share
    std::wstring root(MAX_PATH + 1, L'\0');
    if (!GetVolumePathNameW(full.c_str(), root.data(), static_cast<DWORD>(root.size()))) return loc;
    root.resize(wcslen(root.c_str()));
    if (root.empty() || GetDriveTypeW(root.c_str()) == DRIVE_REMOTE) return loc;
    wchar_t name[64] = {};
    if (!GetVolumeNameForVolumeMountPointW(root.c_str(), name, 64)) return loc;
    loc.volume = narrow(name);
    loc.root = narrow(root);
    loc.relative = full.size() > root.size() ? narrow(std::wstring_view(full).substr(root.size())) : std::string();
    wchar_t windows[MAX_PATH] = {};
    const UINT n = GetSystemWindowsDirectoryW(windows, MAX_PATH);
    loc.system = n >= 3 && _wcsnicmp(windows, root.c_str(), root.size()) == 0;

    // External: by how the disk is attached (a USB hard disk reports itself as fixed, not removable).
    loc.external = GetDriveTypeW(root.c_str()) == DRIVE_REMOVABLE;
    std::wstring device(name);
    if (!device.empty() && device.back() == L'\\') device.pop_back();  // the volume itself, not its root folder
    const HANDLE h = CreateFileW(device.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        STORAGE_PROPERTY_QUERY query{};
        query.PropertyId = StorageDeviceProperty;
        query.QueryType = PropertyStandardQuery;
        STORAGE_DEVICE_DESCRIPTOR desc{};
        DWORD got = 0;
        if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof query, &desc, sizeof desc, &got, nullptr)) {
            switch (desc.BusType) {
                case BusTypeUsb:
                case BusType1394:
                case BusTypeSd:
                case BusTypeMmc: loc.external = true; break;
                default: break;
            }
            if (desc.RemovableMedia) loc.external = true;
        }
        CloseHandle(h);
    }
    return loc;
}

std::string volume_root(const std::string& volume) {
    if (volume.empty()) return {};
    const std::wstring name = widen(volume);
    std::wstring paths(1024, L'\0');
    DWORD needed = 0;
    if (!GetVolumePathNamesForVolumeNameW(name.c_str(), paths.data(), static_cast<DWORD>(paths.size()), &needed)) {
        if (GetLastError() != ERROR_MORE_DATA) return {};
        paths.assign(needed, L'\0');
        if (!GetVolumePathNamesForVolumeNameW(name.c_str(), paths.data(), needed, &needed)) return {};
    }
    // A list of mount paths, each ending in \0; the first one (usually the drive letter) will do.
    return narrow(std::wstring(paths.c_str()));
}

Bytes protect_secret(ByteSpan plain, bool machine) {
    DATA_BLOB in{static_cast<DWORD>(plain.size()), const_cast<BYTE*>(plain.data())};
    DATA_BLOB out{};
    const DWORD flags = CRYPTPROTECT_UI_FORBIDDEN | (machine ? CRYPTPROTECT_LOCAL_MACHINE : 0);
    if (!CryptProtectData(&in, L"Vaultik", nullptr, nullptr, nullptr, flags, &out))
        throw_last_error("cannot protect the stored password");
    Bytes sealed(out.pbData, out.pbData + out.cbData);
    LocalFree(out.pbData);
    return sealed;
}

Bytes unprotect_secret(ByteSpan sealed) {
    DATA_BLOB in{static_cast<DWORD>(sealed.size()), const_cast<BYTE*>(sealed.data())};
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        throw Error("cannot read the stored password (it was saved by another user or computer)");
    Bytes plain(out.pbData, out.pbData + out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return plain;
}

// ---- process ------------------------------------------------------------------------------

namespace {

// Quotes one argument the way CommandLineToArgvW and the MSVC runtime parse it.
std::wstring quote_arg(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
        } else if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(c);
            backslashes = 0;
        } else {
            out.append(backslashes, L'\\');
            out.push_back(c);
            backslashes = 0;
        }
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

}  // namespace

// A bare program name is taken from the system folder only. Letting Windows search would look in
// the program's own folder and the current folder first, where a planted schtasks.exe could run
// instead.
static std::wstring program_path(const std::string& name) {
    std::wstring program = widen(name);
    if (program.find_first_of(L"\\/") == std::wstring::npos) {
        wchar_t sys[MAX_PATH];
        const UINT n = GetSystemDirectoryW(sys, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) throw_last_error("cannot find the system folder");
        program = std::wstring(sys, n) + L"\\" + program;
    }
    return program;
}

int run_command_elevated(const std::vector<std::string>& argv) {
    if (argv.empty()) throw Error("run_command_elevated: no program");
    const std::wstring program = program_path(argv[0]);
    std::wstring params;
    for (std::size_t i = 1; i < argv.size(); ++i) params += (params.empty() ? L"" : L" ") + quote_arg(widen(argv[i]));

    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof info;
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"runas";
    info.lpFile = program.c_str();
    info.lpParameters = params.c_str();
    info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info)) {
        if (GetLastError() == ERROR_CANCELLED) throw Error("administrator approval was declined");
        throw_last_error("cannot start " + argv[0] + " as administrator");
    }
    if (info.hProcess == nullptr) throw Error("cannot start " + argv[0] + " as administrator");
    UniqueHandle process(info.hProcess);
    WaitForSingleObject(process.get(), INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(process.get(), &code);
    return static_cast<int>(code);
}

int run_command(const std::vector<std::string>& argv, std::string* output) {
    if (argv.empty()) throw Error("run_command: no program");
    std::wstring cmdline;
    for (const std::string& a : argv) cmdline += (cmdline.empty() ? L"" : L" ") + quote_arg(widen(a));

    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    if (output != nullptr) {
        if (!CreatePipe(&read_end, &write_end, &sa, 0)) throw_last_error("cannot create pipe");
        SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
    }
    UniqueHandle r(read_end);
    UniqueHandle w(write_end);

    STARTUPINFOW si{};
    si.cb = sizeof si;
    if (output != nullptr) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = w.get();
        si.hStdError = w.get();
    }
    const std::wstring program = program_path(argv[0]);

    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(program.c_str(), cmdline.data(), nullptr, nullptr, output != nullptr, CREATE_NO_WINDOW, nullptr,
                        nullptr, &si, &pi))
        throw_last_error("cannot start " + argv[0]);
    UniqueHandle process(pi.hProcess);
    UniqueHandle thread(pi.hThread);
    w.reset();  // only the child writes now, so the read below ends when it exits

    if (output != nullptr) {
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(r.get(), buf, sizeof buf, &n, nullptr) && n > 0) output->append(buf, n);
    }
    WaitForSingleObject(process.get(), INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(process.get(), &code);
    return static_cast<int>(code);
}

bool metered_connection() {
    // The Network List Manager knows the cost of the preferred connection.
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool metered = false;
    INetworkCostManager* costs = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_NetworkListManager, nullptr, CLSCTX_ALL, IID_INetworkCostManager,
                                   reinterpret_cast<void**>(&costs)))) {
        DWORD cost = 0;
        if (SUCCEEDED(costs->GetCost(&cost, nullptr)))
            metered = (cost & (NLM_CONNECTION_COST_FIXED | NLM_CONNECTION_COST_VARIABLE |
                               NLM_CONNECTION_COST_OVERDATALIMIT | NLM_CONNECTION_COST_CONGESTED |
                               NLM_CONNECTION_COST_ROAMING | NLM_CONNECTION_COST_APPROACHINGDATALIMIT)) != 0;
        costs->Release();
    }
    if (SUCCEEDED(init)) CoUninitialize();
    return metered;
}

void set_env(const std::string& name, const std::string& value) {
    SetEnvironmentVariableW(widen(name).c_str(), widen(value).c_str());
}

int run_shell(const std::string& command, std::string* output) {
    ShellOptions o;
    o.output = output;
    return run_shell(command, o);
}

// Runs `program` with `cmdline` without a window and with empty input, captures its output (the
// first `output_limit` bytes), and stops it with everything it started after the timeout.
static int run_process(const std::wstring& program, std::wstring cmdline, const ShellOptions& o,
                       std::size_t output_limit) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &sa, 0)) throw_last_error("cannot create pipe");
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
    UniqueHandle r(read_end);
    UniqueHandle w(write_end);
    // No input: a command that asks for something gets end-of-file instead of waiting forever.
    UniqueHandle nul(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr));
    if (!nul.valid()) throw_last_error("cannot open NUL");

    // Only these two handles are passed on, not every inheritable handle of this process (such
    // as the frontend's pipes), so nothing the command leaves running keeps them open.
    HANDLE inherit[2] = {w.get(), nul.get()};
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<std::uint8_t> attr_buf(attr_size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    if (!InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size)) throw_last_error("cannot start the command");
    struct AttrGuard {
        LPPROC_THREAD_ATTRIBUTE_LIST a;
        ~AttrGuard() { DeleteProcThreadAttributeList(a); }
    } attr_guard{attrs};
    if (!UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof inherit, nullptr,
                                   nullptr))
        throw_last_error("cannot start the command");

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = nul.get();
    si.StartupInfo.hStdOutput = w.get();
    si.StartupInfo.hStdError = w.get();
    si.lpAttributeList = attrs;

    // A fixed, minimal environment when asked (commands run with administrator rights): the
    // system's own folders only, nothing the user's settings could point elsewhere.
    std::wstring env;
    if (o.clean_environment) {
        wchar_t windows[MAX_PATH] = {};
        GetSystemWindowsDirectoryW(windows, MAX_PATH);
        const std::wstring root = windows;
        std::map<std::wstring, std::wstring, std::less<>> vars = {
            {L"SystemRoot", root},
            {L"windir", root},
            {L"SystemDrive", root.substr(0, 2)},
            {L"ComSpec", root + L"\\System32\\cmd.exe"},
            {L"PATHEXT", L".COM;.EXE;.BAT;.CMD"},  // a bare name never opens a script by its file type
            {L"Path", root + L"\\System32;" + root + L";" + root + L"\\System32\\Wbem;" + root +
                          L"\\System32\\WindowsPowerShell\\v1.0;" + root + L"\\System32\\OpenSSH"},
            {L"TEMP", root + L"\\Temp"},
            {L"TMP", root + L"\\Temp"},
            {L"OS", L"Windows_NT"},
            // Modules only from the system's folders, not the user's Documents.
            {L"PSModulePath", root + L"\\System32\\WindowsPowerShell\\v1.0\\Modules"},
        };
        // The usual system values many tools expect, from the system rather than this process's
        // environment (which the user's own settings make up).
        const auto known = [&](REFKNOWNFOLDERID id, const wchar_t* name) {
            PWSTR p = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &p)) && p != nullptr) vars[name] = p;
            CoTaskMemFree(p);
        };
        known(FOLDERID_ProgramData, L"ProgramData");
        known(FOLDERID_ProgramData, L"ALLUSERSPROFILE");
        known(FOLDERID_ProgramFiles, L"ProgramFiles");
        known(FOLDERID_ProgramFilesX86, L"ProgramFiles(x86)");
        known(FOLDERID_ProgramFilesCommon, L"CommonProgramFiles");
        known(FOLDERID_ProgramFilesCommonX86, L"CommonProgramFiles(x86)");
        known(FOLDERID_Public, L"PUBLIC");
        wchar_t computer[MAX_COMPUTERNAME_LENGTH + 1] = {};
        DWORD computer_len = MAX_COMPUTERNAME_LENGTH + 1;
        if (GetComputerNameW(computer, &computer_len)) vars[L"COMPUTERNAME"] = computer;
        SYSTEM_INFO info{};
        GetNativeSystemInfo(&info);
        vars[L"NUMBER_OF_PROCESSORS"] = std::to_wstring(info.dwNumberOfProcessors);
        vars[L"PROCESSOR_ARCHITECTURE"] = info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64 ? L"ARM64"
                                          : info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL ? L"x86"
                                                                                                         : L"AMD64";
        for (const auto& [name, value] : o.extra_environment) vars[widen(name)] = widen(value);
        for (const auto& [name, value] : vars) env += name + L"=" + value + L'\0';
        env += L'\0';
    }
    const std::wstring directory = o.directory.empty() ? std::wstring() : widen(o.directory);

    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(program.c_str(), cmdline.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED |
                            (o.clean_environment ? CREATE_UNICODE_ENVIRONMENT : 0),
                        o.clean_environment ? env.data() : nullptr, directory.empty() ? nullptr : directory.c_str(),
                        &si.StartupInfo, &pi))
        throw_last_error("cannot start the command");
    UniqueHandle process(pi.hProcess);
    UniqueHandle thread(pi.hThread);
    // Everything the command starts is in this job, so a command that runs too long can be
    // stopped with all it started. (Not killed when the job handle closes: a command may start a
    // program that is meant to keep running, such as a database it stopped before the backup.)
    UniqueHandle job(CreateJobObjectW(nullptr, nullptr));
    if (job.valid()) AssignProcessToJobObject(job.get(), process.get());
    ResumeThread(thread.get());
    w.reset();
    nul.reset();

    // The output is read on its own thread, so waiting can time out and a program the command
    // left running (holding the pipe open) does not keep this from returning.
    std::string captured;
    std::atomic<bool> stop_reading{false};
    std::atomic<bool> reader_done{false};
    std::thread reader([&] {
        char buf[4096];
        DWORD n = 0;
        while (!stop_reading && ReadFile(r.get(), buf, sizeof buf, &n, nullptr) && n > 0)
            if (captured.size() < output_limit) captured.append(buf, n);  // the start is enough for a log
        reader_done = true;
    });
    const auto deadline = std::chrono::steady_clock::now() + o.timeout;
    bool stopped = false;
    while (WaitForSingleObject(process.get(), 500) == WAIT_TIMEOUT) {
        if (std::chrono::steady_clock::now() >= deadline || (o.cancelled && o.cancelled())) {
            if (job.valid()) TerminateJobObject(job.get(), 1);
            else TerminateProcess(process.get(), 1);
            stopped = true;
            WaitForSingleObject(process.get(), 5000);
            break;
        }
    }
    // Give the last output a moment to arrive, then stop reading: a program the command left
    // running may hold the pipe open. (No PeekNamedPipe here: it would wait behind the reader's
    // ReadFile on the same handle.)
    Sleep(200);
    stop_reading = true;
    while (!reader_done) {  // (repeated: the reader may not be blocked in ReadFile yet)
        CancelSynchronousIo(reader.native_handle());
        Sleep(20);
    }
    reader.join();
    if (o.output != nullptr) *o.output = std::move(captured);
    if (stopped) throw Error(o.cancelled && o.cancelled() ? "the command was cancelled"
                                                          : "the command did not finish in time and was stopped");
    DWORD code = 0;
    GetExitCodeProcess(process.get(), &code);
    return static_cast<int>(code);
}

int run_shell(const std::string& command, const ShellOptions& o) {
    // "cmd /s /c "<command>"": cmd drops the outer quotes and runs the rest exactly as typed. /d:
    // no AutoRun command from the registry. With the fixed environment also /e:on /v:off, so the
    // user's own registry settings do not change how the command is read.
    std::wstring cmdline = std::wstring(L"cmd.exe /d ") + (o.clean_environment ? L"/e:on /v:off " : L"") +
                           L"/s /c \"" + widen(command) + L"\"";
    return run_process(program_path("cmd.exe"), std::move(cmdline), o, 64u << 10);
}

int run_command(const std::vector<std::string>& argv, const CommandOptions& options) {
    if (argv.empty()) throw Error("run_command: no program");
    std::wstring cmdline;
    for (const std::string& a : argv) cmdline += (cmdline.empty() ? L"" : L" ") + quote_arg(widen(a));
    ShellOptions o;
    o.output = options.output;
    o.timeout = options.timeout;
    o.cancelled = options.cancelled;
    o.clean_environment = options.clean_environment;
    o.extra_environment = options.extra_environment;
    return run_process(program_path(argv[0]), std::move(cmdline), o, 1u << 20);
}

struct PipedProcess::Impl {
    Direction direction = Direction::FromProgram;
    UniqueHandle process;
    UniqueHandle job;
    UniqueHandle data;  // our end of the stream
    UniqueHandle messages_pipe;
    std::thread reader;
    std::atomic<bool> reader_done{false};
    mutable std::mutex lock;
    std::string messages;
    bool waited = false;
    int exit_code = 0;

    void finish_reader() {
        if (!reader.joinable()) return;
        // The program has ended; a program it left running may still hold the pipe open.
        for (int i = 0; i < 100 && !reader_done; ++i) Sleep(20);
        while (!reader_done) {
            CancelSynchronousIo(reader.native_handle());
            Sleep(20);
        }
        reader.join();
    }
};

PipedProcess::PipedProcess(const std::vector<std::string>& argv, Direction direction,
                           const std::vector<std::pair<std::string, std::string>>& environment)
    : impl_(std::make_unique<Impl>()) {
    if (argv.empty()) throw Error("PipedProcess: no program");
    impl_->direction = direction;
    std::wstring cmdline;
    for (const std::string& a : argv) cmdline += (cmdline.empty() ? L"" : L" ") + quote_arg(widen(a));
    const std::wstring program = program_path(argv[0]);

    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE data_read = nullptr, data_write = nullptr, msg_read = nullptr, msg_write = nullptr;
    // A large pipe buffer: the stream moves in big pieces (a distribution is gigabytes).
    if (!CreatePipe(&data_read, &data_write, &sa, 1u << 20)) throw_last_error("cannot create pipe");
    UniqueHandle dr(data_read), dw(data_write);
    if (!CreatePipe(&msg_read, &msg_write, &sa, 0)) throw_last_error("cannot create pipe");
    UniqueHandle mr(msg_read), mw(msg_write);
    SetHandleInformation(mr.get(), HANDLE_FLAG_INHERIT, 0);
    UniqueHandle nul(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr));
    if (!nul.valid()) throw_last_error("cannot open NUL");

    HANDLE child_in = nullptr, child_out = nullptr;
    if (direction == Direction::FromProgram) {
        SetHandleInformation(dr.get(), HANDLE_FLAG_INHERIT, 0);
        child_in = nul.get();
        child_out = dw.get();
    } else {
        SetHandleInformation(dw.get(), HANDLE_FLAG_INHERIT, 0);
        child_in = dr.get();
        child_out = mw.get();
    }
    // Only the program's own handles are passed on (see run_process).
    std::vector<HANDLE> inherit = {child_in, child_out};
    if (child_out != mw.get()) inherit.push_back(mw.get());
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<std::uint8_t> attr_buf(attr_size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    if (!InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size)) throw_last_error("cannot start " + argv[0]);
    struct AttrGuard {
        LPPROC_THREAD_ATTRIBUTE_LIST a;
        ~AttrGuard() { DeleteProcThreadAttributeList(a); }
    } attr_guard{attrs};
    if (!UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit.data(),
                                   inherit.size() * sizeof(HANDLE), nullptr, nullptr))
        throw_last_error("cannot start " + argv[0]);

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = child_in;
    si.StartupInfo.hStdOutput = child_out;
    si.StartupInfo.hStdError = mw.get();
    si.lpAttributeList = attrs;

    // This process's environment with the given variables replaced or added.
    std::wstring env;
    if (!environment.empty()) {
        std::map<std::wstring, std::wstring> vars;
        std::map<std::wstring, std::wstring> folded;  // upper-case name -> name as found
        if (wchar_t* block = GetEnvironmentStringsW()) {
            for (const wchar_t* p = block; *p != L'\0'; p += wcslen(p) + 1) {
                const std::wstring entry = p;
                const std::size_t eq = entry.find(L'=', 1);  // ("=C:=C:\..." entries start with '=')
                if (eq == std::wstring::npos) continue;
                vars[entry.substr(0, eq)] = entry.substr(eq + 1);
            }
            FreeEnvironmentStringsW(block);
        }
        for (const auto& [name, value] : vars) {
            std::wstring upper = name;
            CharUpperBuffW(upper.data(), static_cast<DWORD>(upper.size()));
            folded[upper] = name;
        }
        for (const auto& [name, value] : environment) {
            std::wstring w = widen(name);
            std::wstring upper = w;
            CharUpperBuffW(upper.data(), static_cast<DWORD>(upper.size()));
            if (const auto it = folded.find(upper); it != folded.end()) vars.erase(it->second);
            vars[w] = widen(value);
        }
        for (const auto& [name, value] : vars) env += name + L"=" + value + L'\0';
        env += L'\0';
    }

    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(program.c_str(), cmdline.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED |
                            (env.empty() ? 0 : CREATE_UNICODE_ENVIRONMENT),
                        env.empty() ? nullptr : env.data(), nullptr, &si.StartupInfo, &pi))
        throw_last_error("cannot start " + argv[0]);
    impl_->process = UniqueHandle(pi.hProcess);
    UniqueHandle thread(pi.hThread);
    // Everything it starts is in a job, so stop() ends it all; closing the job (this object going
    // away) ends it too.
    impl_->job = UniqueHandle(CreateJobObjectW(nullptr, nullptr));
    if (impl_->job.valid()) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(impl_->job.get(), JobObjectExtendedLimitInformation, &limits, sizeof limits);
        AssignProcessToJobObject(impl_->job.get(), impl_->process.get());
    }
    ResumeThread(thread.get());

    // Our ends only: the program holds its own copies.
    impl_->data = direction == Direction::FromProgram ? std::move(dr) : std::move(dw);
    impl_->messages_pipe = std::move(mr);
    dr.reset();
    dw.reset();
    mw.reset();
    nul.reset();
    Impl* impl = impl_.get();
    impl_->reader = std::thread([impl] {
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(impl->messages_pipe.get(), buf, sizeof buf, &n, nullptr) && n > 0) {
            const std::lock_guard<std::mutex> guard(impl->lock);
            if (impl->messages.size() < (64u << 10)) impl->messages.append(buf, n);
        }
        impl->reader_done = true;
    });
}

PipedProcess::~PipedProcess() {
    if (!impl_->waited) {
        stop();
        impl_->data.reset();
        WaitForSingleObject(impl_->process.get(), 5000);
    }
    impl_->finish_reader();
}

std::size_t PipedProcess::read(std::uint8_t* dst, std::size_t max) {
    if (impl_->direction != Direction::FromProgram || !impl_->data.valid()) return 0;
    std::size_t total = 0;
    // Fill the buffer as far as the pipe allows, so chunking sees large reads.
    while (total < max) {
        DWORD n = 0;
        const DWORD want = static_cast<DWORD>(std::min<std::size_t>(max - total, 1u << 24));
        if (!ReadFile(impl_->data.get(), dst + total, want, &n, nullptr)) {
            const DWORD e = GetLastError();
            if (e == ERROR_BROKEN_PIPE) break;
            throw OsError("cannot read the program's output", e);
        }
        if (n == 0) break;
        total += n;
        if (total >= (1u << 16)) break;  // enough for one call; the next one continues
    }
    return total;
}

void PipedProcess::write(ByteSpan data) {
    if (impl_->direction != Direction::ToProgram || !impl_->data.valid()) throw Error("the program's input is closed");
    std::size_t done = 0;
    while (done < data.size()) {
        DWORD n = 0;
        const DWORD want = static_cast<DWORD>(std::min<std::size_t>(data.size() - done, 1u << 24));
        if (!WriteFile(impl_->data.get(), data.data() + done, want, &n, nullptr))
            throw OsError("the program stopped reading its input", GetLastError());
        done += n;
    }
}

int PipedProcess::wait() {
    if (impl_->waited) return impl_->exit_code;
    impl_->data.reset();
    WaitForSingleObject(impl_->process.get(), INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(impl_->process.get(), &code);
    impl_->finish_reader();
    impl_->waited = true;
    impl_->exit_code = static_cast<int>(code);
    return impl_->exit_code;
}

void PipedProcess::stop() {
    if (impl_->job.valid()) TerminateJobObject(impl_->job.get(), 1);
    else if (impl_->process.valid()) TerminateProcess(impl_->process.get(), 1);
}

std::string PipedProcess::messages() const {
    const std::lock_guard<std::mutex> guard(impl_->lock);
    return impl_->messages;
}

std::string executable_path() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) throw_last_error("cannot find the program path");
        if (n < buf.size()) {
            buf.resize(n);
            return narrow(strip_long_prefix(buf));
        }
        buf.resize(buf.size() * 2);
    }
}

void enable_backup_privileges() {
    enable_privilege(L"SeBackupPrivilege");
}

void enable_restore_privileges() {
    enable_privilege(L"SeRestorePrivilege");
}

std::string hostname() {
    return computer_name();
}

std::string machine_id() {
    // The 64-bit view: a 32-bit process would otherwise read a redirected (missing) key.
    wchar_t buf[128] = {};
    DWORD size = sizeof buf;
    if (::RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography", L"MachineGuid",
                       RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, buf, &size) == ERROR_SUCCESS &&
        buf[0] != 0)
        return narrow(buf);
    return "host:" + computer_name();
}

void system_log(LogLevel level, std::uint32_t event_id, const std::string& text) {
    try {
        // An entry holds at most 31839 characters; cut on a character boundary.
        std::size_t cut = std::min<std::size_t>(text.size(), 30000);
        while (cut > 0 && cut < text.size() && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
        const std::wstring wide = widen(text.substr(0, cut));
        const WORD type = level == LogLevel::Error     ? EVENTLOG_ERROR_TYPE
                          : level == LogLevel::Warning ? EVENTLOG_WARNING_TYPE
                                                       : EVENTLOG_INFORMATION_TYPE;
        HANDLE source = RegisterEventSourceW(nullptr, L"Vaultik");
        if (source == nullptr) return;
        const wchar_t* strings[] = {wide.c_str()};
        ReportEventW(source, type, 0, event_id, nullptr, 1, 0, strings, nullptr);
        DeregisterEventSource(source);
    } catch (...) {
        // never in the way of the run it reports
    }
}

std::string username() {
    return user_name();
}

std::uint32_t process_id() {
    return GetCurrentProcessId();
}

bool process_alive(std::uint32_t pid) {
    HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h == nullptr) return GetLastError() != ERROR_INVALID_PARAMETER;  // access denied: it exists
    const bool running = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return running;
}

std::uint64_t now() {
    return now_filetime();
}

std::string format_time_local(std::uint64_t time) {
    return format_filetime_local(time);
}

std::string get_env(const char* name) {
    return narrow(vaultik::get_env(widen(name).c_str()));
}

std::string user_folder(UserFolder folder) {
    REFKNOWNFOLDERID id = [&]() -> REFKNOWNFOLDERID {
        switch (folder) {
            case UserFolder::Profile: return FOLDERID_Profile;
            case UserFolder::Documents: return FOLDERID_Documents;
            case UserFolder::Desktop: return FOLDERID_Desktop;
            case UserFolder::Pictures: return FOLDERID_Pictures;
            case UserFolder::Music: return FOLDERID_Music;
            case UserFolder::Videos: return FOLDERID_Videos;
            case UserFolder::Downloads: return FOLDERID_Downloads;
            case UserFolder::SavedGames: return FOLDERID_SavedGames;
            case UserFolder::RoamingAppData: return FOLDERID_RoamingAppData;
            case UserFolder::LocalAppData: return FOLDERID_LocalAppData;
        }
        return FOLDERID_Profile;
    }();
    PWSTR p = nullptr;
    std::string out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &p)) && p != nullptr) out = narrow(p);
    CoTaskMemFree(p);
    return out;
}

}  // namespace vaultik::os
