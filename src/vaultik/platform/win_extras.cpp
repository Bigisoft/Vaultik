// Windows file extras: security descriptors, alternate data streams, symlink and junction targets.

#include "vaultik/platform/os.hpp"

#include <cstring>
#include <vector>

#include "vaultik/util/win.hpp"
#include "vaultik/util/win_handle.hpp"

#include <winioctl.h>

namespace vaultik::os {

namespace {

std::wstring native_path(const std::string& path) {
    return long_path(widen(path));
}

constexpr SECURITY_INFORMATION kSecurityParts =
    OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;

// REPARSE_DATA_BUFFER is in the DDK headers only.
struct ReparseBuffer {
    ULONG ReparseTag;
    USHORT ReparseDataLength;
    USHORT Reserved;
    USHORT SubstituteNameOffset;
    USHORT SubstituteNameLength;
    USHORT PrintNameOffset;
    USHORT PrintNameLength;
    // Symlinks have a ULONG Flags here; mount points do not.
};
constexpr ULONG kSymlinkFlagRelative = 1;

// "\??\C:\x" -> "C:\x", "\??\UNC\server\share" -> "\\server\share".
std::wstring without_nt_prefix(std::wstring s) {
    if (s.starts_with(L"\\??\\UNC\\")) return L"\\\\" + s.substr(8);
    if (s.starts_with(L"\\??\\")) return s.substr(4);
    return s;
}

}  // namespace

Bytes read_security(const std::string& path) {
    const std::wstring p = native_path(path);
    DWORD needed = 0;
    GetFileSecurityW(p.c_str(), kSecurityParts, nullptr, 0, &needed);
    if (needed == 0 || needed > (1u << 20)) return {};
    Bytes sd(needed);
    if (!GetFileSecurityW(p.c_str(), kSecurityParts, reinterpret_cast<PSECURITY_DESCRIPTOR>(sd.data()), needed,
                          &needed))
        return {};
    sd.resize(GetSecurityDescriptorLength(reinterpret_cast<PSECURITY_DESCRIPTOR>(sd.data())));
    return sd;
}

std::string final_path(const std::string& path) {
    UniqueHandle h(CreateFileW(native_path(path).c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!h.valid()) return {};
    std::wstring buf(1024, L'\0');
    DWORD n = GetFinalPathNameByHandleW(h.get(), buf.data(), static_cast<DWORD>(buf.size()), FILE_NAME_NORMALIZED);
    if (n >= buf.size()) {
        buf.resize(n + 1);
        n = GetFinalPathNameByHandleW(h.get(), buf.data(), static_cast<DWORD>(buf.size()), FILE_NAME_NORMALIZED);
    }
    if (n == 0 || n >= buf.size()) return {};
    buf.resize(n);
    return narrow(strip_long_prefix(buf));
}

std::optional<FileId> file_id(const std::string& path) {
    UniqueHandle h(CreateFileW(native_path(path).c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                               nullptr));
    FILE_ID_INFO info{};
    if (!h.valid() || !GetFileInformationByHandleEx(h.get(), FileIdInfo, &info, sizeof info)) return std::nullopt;
    FileId out;
    out.volume = info.VolumeSerialNumber;
    std::memcpy(out.id.data(), info.FileId.Identifier, out.id.size());
    return out;
}

ErrorCode write_security(const std::string& path, ByteSpan descriptor, const FileId* expected) {
    if (descriptor.empty()) return 0;
    Bytes sd(descriptor.begin(), descriptor.end());  // the API wants a non-const pointer
    auto* psd = reinterpret_cast<PSECURITY_DESCRIPTOR>(sd.data());
    if (!IsValidSecurityDescriptor(psd)) return ERROR_INVALID_SECURITY_DESCR;
    // Through a handle on the item itself, never through a link: someone who can change the
    // restore folder could swap the item for a link between its creation and this call, and an
    // elevated restore would then rewrite the permissions of whatever the link points at.
    const std::wstring p = native_path(path);
    const auto open = [&](DWORD access) {
        return UniqueHandle(CreateFileW(p.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                        nullptr, OPEN_EXISTING,
                                        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    };
    // Setting another account as owner needs the restore privilege (administrators): without
    // it, keep the access list at least.
    SECURITY_INFORMATION parts = kSecurityParts;
    UniqueHandle h = open(READ_CONTROL | WRITE_DAC | WRITE_OWNER);
    if (!h.valid()) {
        parts = DACL_SECURITY_INFORMATION;
        h = open(READ_CONTROL | WRITE_DAC);
    }
    if (!h.valid()) return GetLastError();
    FILE_ATTRIBUTE_TAG_INFO tag{};
    FILE_STANDARD_INFO standard{};
    if (!GetFileInformationByHandleEx(h.get(), FileAttributeTagInfo, &tag, sizeof tag) ||
        !GetFileInformationByHandleEx(h.get(), FileStandardInfo, &standard, sizeof standard))
        return GetLastError();
    // A link, or a file with another name elsewhere (a hard link to some other file).
    if ((tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        ((tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 && standard.NumberOfLinks != 1))
        return ERROR_STOPPED_ON_SYMLINK;
    // Another item than the one restored: a folder on the way was swapped for a link.
    if (expected != nullptr) {
        FILE_ID_INFO info{};
        if (!GetFileInformationByHandleEx(h.get(), FileIdInfo, &info, sizeof info) ||
            info.VolumeSerialNumber != expected->volume ||
            std::memcmp(info.FileId.Identifier, expected->id.data(), expected->id.size()) != 0)
            return ERROR_STOPPED_ON_SYMLINK;
    }
    if (SetKernelObjectSecurity(h.get(), parts, psd)) return 0;
    if (parts != DACL_SECURITY_INFORMATION && SetKernelObjectSecurity(h.get(), DACL_SECURITY_INFORMATION, psd))
        return 0;
    return GetLastError();
}

std::vector<StreamEntry> list_streams(const std::string& path) {
    std::vector<StreamEntry> out;
    WIN32_FIND_STREAM_DATA data{};
    const HANDLE h = FindFirstStreamW(native_path(path).c_str(), FindStreamInfoStandard, &data, 0);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        // ":name:$DATA"; the unnamed "::$DATA" is the file's own content.
        std::wstring name = data.cStreamName;
        if (!name.starts_with(L":") || !name.ends_with(L":$DATA")) continue;
        name = name.substr(1, name.size() - 1 - 6);
        if (name.empty()) continue;
        out.push_back({narrow(name), static_cast<std::uint64_t>(data.StreamSize.QuadPart)});
    } while (FindNextStreamW(h, &data));
    FindClose(h);
    return out;
}

std::string stream_path(const std::string& path, const std::string& name) {
    return path + ":" + name;
}

bool read_link(const std::string& path, ReparseLink& out) {
    UniqueHandle h(CreateFileW(native_path(path).c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                               nullptr));
    if (!h.valid()) return false;
    std::vector<std::uint8_t> buf(MAXIMUM_REPARSE_DATA_BUFFER_SIZE);
    DWORD got = 0;
    if (!DeviceIoControl(h.get(), FSCTL_GET_REPARSE_POINT, nullptr, 0, buf.data(), static_cast<DWORD>(buf.size()),
                         &got, nullptr) ||
        got < sizeof(ReparseBuffer))
        return false;
    ReparseBuffer rb;
    std::memcpy(&rb, buf.data(), sizeof rb);
    std::size_t names = sizeof(ReparseBuffer);
    ULONG flags = 0;
    if (rb.ReparseTag == IO_REPARSE_TAG_SYMLINK) {
        if (got < names + sizeof(ULONG)) return false;
        std::memcpy(&flags, buf.data() + names, sizeof flags);
        names += sizeof(ULONG);
    } else if (rb.ReparseTag != IO_REPARSE_TAG_MOUNT_POINT) {
        return false;  // another kind of reparse point (cloud files, deduplication): not a link
    }
    const std::size_t off = names + rb.SubstituteNameOffset;
    if (off + rb.SubstituteNameLength > got) return false;
    std::wstring target(rb.SubstituteNameLength / sizeof(wchar_t), L'\0');
    std::memcpy(target.data(), buf.data() + off, rb.SubstituteNameLength);
    out.tag = rb.ReparseTag;
    out.relative = (flags & kSymlinkFlagRelative) != 0;
    out.target = narrow(out.relative ? target : without_nt_prefix(target));
    return !out.target.empty();
}

ErrorCode create_link(const std::string& path, const ReparseLink& link, bool directory) {
    const std::wstring p = native_path(path);
    const std::wstring target = widen(link.target);
    if (link.tag == IO_REPARSE_TAG_SYMLINK) {
        DWORD flags = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
        if (directory) flags |= SYMBOLIC_LINK_FLAG_DIRECTORY;
        return CreateSymbolicLinkW(p.c_str(), target.c_str(), flags) ? 0 : GetLastError();
    }
    if (link.tag != IO_REPARSE_TAG_MOUNT_POINT) return ERROR_NOT_SUPPORTED;
    // A junction: an empty folder with a mount point reparse buffer.
    const bool unc = target.starts_with(L"\\\\");
    if (unc) return ERROR_NOT_SUPPORTED;  // junctions point at local volumes only
    const std::wstring substitute = L"\\??\\" + target;
    const std::wstring& print = target;
    if (!CreateDirectoryW(p.c_str(), nullptr)) return GetLastError();
    const std::size_t names = (substitute.size() + 1 + print.size() + 1) * sizeof(wchar_t);
    std::vector<std::uint8_t> buf(sizeof(ReparseBuffer) + names);
    ReparseBuffer rb{};
    rb.ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
    rb.ReparseDataLength = static_cast<USHORT>(buf.size() - 8);
    rb.SubstituteNameOffset = 0;
    rb.SubstituteNameLength = static_cast<USHORT>(substitute.size() * sizeof(wchar_t));
    rb.PrintNameOffset = static_cast<USHORT>((substitute.size() + 1) * sizeof(wchar_t));
    rb.PrintNameLength = static_cast<USHORT>(print.size() * sizeof(wchar_t));
    std::memcpy(buf.data(), &rb, sizeof rb);
    std::memcpy(buf.data() + sizeof rb, substitute.c_str(), (substitute.size() + 1) * sizeof(wchar_t));
    std::memcpy(buf.data() + sizeof rb + rb.PrintNameOffset, print.c_str(), (print.size() + 1) * sizeof(wchar_t));
    UniqueHandle h(CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    DWORD err = 0;
    if (!h.valid()) {
        err = GetLastError();
    } else {
        DWORD got = 0;
        if (!DeviceIoControl(h.get(), FSCTL_SET_REPARSE_POINT, buf.data(), static_cast<DWORD>(buf.size()), nullptr, 0,
                             &got, nullptr))
            err = GetLastError();
    }
    if (err != 0) {
        h.reset();
        RemoveDirectoryW(p.c_str());
    }
    return err;
}

}  // namespace vaultik::os
