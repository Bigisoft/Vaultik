// Windows implementation of the OS layer.

#include "vaultik/platform/os.hpp"

#include <algorithm>
#include <limits>

#include "vaultik/util/resources.hpp"
#include "vaultik/util/win.hpp"
#include "vaultik/util/win_handle.hpp"

// After <windows.h> (pulled in by win_handle.hpp).
#include <dpapi.h>

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

File File::open_random(const std::string& path, ErrorCode& err) {
    return File(open_handle(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, err));
}

File File::open_append(const std::string& path, ErrorCode& err, std::uint32_t /*mode*/) {
    return File(open_handle(path, FILE_APPEND_DATA | SYNCHRONIZE, FILE_SHARE_READ, OPEN_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, err));
}

File File::create(const std::string& path, bool overwrite, ErrorCode& err, std::uint32_t /*mode*/) {
    return File(open_handle(path, GENERIC_WRITE, 0, overwrite ? CREATE_ALWAYS : CREATE_NEW,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, err));
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
    if (!MoveFileExW(native(from).c_str(), native(to).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw_last_error("cannot finalize " + to);
}

ErrorCode remove_file(const std::string& path) {
    return DeleteFileW(native(path).c_str()) ? 0 : GetLastError();
}

void make_writable(const std::string& path) {
    SetFileAttributesW(native(path).c_str(), FILE_ATTRIBUTE_NORMAL);
}

void set_metadata(const std::string& path, const FileInfo& info) {
    const std::wstring p = native(path);
    const bool is_dir = info.type == EntryType::Dir;
    if (info.mtime != 0 || info.ctime != 0) {
        UniqueHandle h(CreateFileW(p.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING,
                                   is_dir ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL, nullptr));
        if (h.valid()) {
            const FILETIME c = u64_to_filetime(info.ctime);
            const FILETIME m = u64_to_filetime(info.mtime);
            SetFileTime(h.get(), info.ctime ? &c : nullptr, nullptr, info.mtime ? &m : nullptr);
        }
    }
    const DWORD attrs = info.attributes & kRestorableAttributes;
    if (attrs != 0) SetFileAttributesW(p.c_str(), attrs | (is_dir ? FILE_ATTRIBUTE_DIRECTORY : 0));
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

Bytes protect_secret(ByteSpan plain) {
    DATA_BLOB in{static_cast<DWORD>(plain.size()), const_cast<BYTE*>(plain.data())};
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"Vaultik", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
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
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, output != nullptr, CREATE_NO_WINDOW, nullptr,
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

std::string username() {
    return user_name();
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

}  // namespace vaultik::os
