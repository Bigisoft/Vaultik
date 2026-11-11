#include "vaultik/platform/usn_journal.hpp"

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <cstring>
#include <vector>

#include "vaultik/util/error.hpp"
#include "vaultik/util/win.hpp"
#include "vaultik/util/win_handle.hpp"

namespace vaultik::os {

namespace {

constexpr std::size_t kBufferSize = 256 * 1024;

// The volume that holds `path`, as a device path without trailing backslash (\\?\Volume{guid}).
// By volume name rather than drive letter so that a folder mounted from another volume works too.
std::wstring volume_device_path(const std::string& path) {
    const std::wstring absolute = full_path(widen(path));
    std::vector<wchar_t> root(32768);
    if (!GetVolumePathNameW(long_path(absolute).c_str(), root.data(), static_cast<DWORD>(root.size())))
        throw_last_error("cannot find the volume of " + path);
    std::vector<wchar_t> name(MAX_PATH);
    if (!GetVolumeNameForVolumeMountPointW(root.data(), name.data(), static_cast<DWORD>(name.size())))
        throw_last_error("cannot name the volume of " + path);
    std::wstring device = name.data();
    while (!device.empty() && device.back() == L'\\') device.pop_back();
    return device;
}

UniqueHandle open_volume(const std::string& path) {
    UniqueHandle h(CreateFileW(volume_device_path(path).c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
    if (!h.valid()) throw_last_error("cannot open the volume of " + path);
    return h;
}

// Errors that mean "this volume cannot answer from a journal" rather than "something broke".
bool no_journal(DWORD e) {
    return e == ERROR_JOURNAL_NOT_ACTIVE || e == ERROR_JOURNAL_DELETE_IN_PROGRESS ||
           e == ERROR_INVALID_FUNCTION || e == ERROR_NOT_SUPPORTED;
}

bool query(HANDLE volume, USN_JOURNAL_DATA_V2& data, DWORD& error) {
    DWORD got = 0;
    // The V2 structure is the V1 one plus range fields; the system fills what it knows.
    if (!DeviceIoControl(volume, FSCTL_QUERY_USN_JOURNAL, nullptr, 0, &data, sizeof data, &got, nullptr)) {
        error = GetLastError();
        return false;
    }
    return true;
}

template <class T>
T load(const std::uint8_t* p) {
    T value;
    std::memcpy(&value, p, sizeof value);
    return value;
}

FileRef ref64(std::uint64_t frn) {
    return FileRef{frn, 0};
}

FileRef ref128(const FILE_ID_128& id) {
    FileRef r;
    std::memcpy(&r.low, id.Identifier, 8);
    std::memcpy(&r.high, id.Identifier + 8, 8);
    return r;
}

}  // namespace

std::optional<UsnJournalState> usn_journal_state(const std::string& path) {
    const UniqueHandle volume = open_volume(path);
    USN_JOURNAL_DATA_V2 data{};
    DWORD error = 0;
    if (!query(volume.get(), data, error)) {
        if (no_journal(error)) return std::nullopt;
        throw_win32("cannot read the change journal of " + path, error);
    }
    return UsnJournalState{data.UsnJournalID, data.FirstUsn, data.NextUsn};
}

std::optional<UsnChanges> usn_changes_since(const std::string& path, std::uint64_t journal_id, std::int64_t since,
                                            std::int64_t until) {
    const UniqueHandle volume = open_volume(path);
    USN_JOURNAL_DATA_V2 data{};
    DWORD error = 0;
    if (!query(volume.get(), data, error)) {
        if (no_journal(error)) return std::nullopt;
        throw_win32("cannot read the change journal of " + path, error);
    }
    if (data.UsnJournalID != journal_id || since < data.FirstUsn || since > data.NextUsn) return std::nullopt;

    UsnChanges out;
    out.next_usn = since;
    std::int64_t position = since;
    std::vector<std::uint8_t> buffer(kBufferSize);
    while (position < until) {
        READ_USN_JOURNAL_DATA_V1 request{};
        request.StartUsn = position;
        request.ReasonMask = 0xFFFFFFFF;
        request.ReturnOnlyOnClose = 0;
        request.Timeout = 0;
        request.BytesToWaitFor = 0;
        request.UsnJournalID = journal_id;
        request.MinMajorVersion = 2;
        request.MaxMajorVersion = 3;
        DWORD got = 0;
        if (!DeviceIoControl(volume.get(), FSCTL_READ_USN_JOURNAL, &request, sizeof request, buffer.data(),
                             static_cast<DWORD>(buffer.size()), &got, nullptr)) {
            const DWORD e = GetLastError();
            // The journal wrapped past `position` or was removed while reading: it cannot answer.
            if (e == ERROR_JOURNAL_ENTRY_DELETED || e == ERROR_JOURNAL_DELETE_IN_PROGRESS ||
                e == ERROR_JOURNAL_NOT_ACTIVE)
                return std::nullopt;
            throw_win32("cannot read the change journal of " + path, e);
        }
        if (got < sizeof(USN)) throw Error("the change journal returned a short answer");

        const std::int64_t next = load<std::int64_t>(buffer.data());
        std::size_t offset = sizeof(USN);
        bool reached_until = false;
        while (offset < got) {
            const std::size_t left = got - offset;
            if (left < 8) throw Error("the change journal returned a truncated record");
            const std::uint8_t* rec = buffer.data() + offset;
            const std::uint32_t length = load<std::uint32_t>(rec);
            const std::uint16_t major = load<std::uint16_t>(rec + 4);
            if (length < 8 || length > left || length % 8 != 0)
                throw Error("the change journal returned a record of an invalid length");
            if (major == 2) {
                if (length < sizeof(USN_RECORD_V2)) throw Error("the change journal returned a short record");
                const USN_RECORD_V2 r = load<USN_RECORD_V2>(rec);
                if (r.Usn >= until) {
                    reached_until = true;
                    break;
                }
                out.changed.insert(ref64(r.FileReferenceNumber));
                out.parents.insert(ref64(r.ParentFileReferenceNumber));
            } else if (major == 3) {
                if (length < sizeof(USN_RECORD_V3)) throw Error("the change journal returned a short record");
                const USN_RECORD_V3 r = load<USN_RECORD_V3>(rec);
                if (r.Usn >= until) {
                    reached_until = true;
                    break;
                }
                out.changed.insert(ref128(r.FileReferenceNumber));
                out.parents.insert(ref128(r.ParentFileReferenceNumber));
            }
            // Other versions (range-tracking V4) were not asked for; they carry no file ids.
            offset += length;
        }
        if (reached_until) {
            out.next_usn = until;
            return out;
        }
        if (next <= position) break;  // no progress: nothing more to read
        position = next;
        out.next_usn = next;
        if (got == sizeof(USN)) break;  // no records came back
    }
    return out;
}

FileRef file_ref(const std::string& path) {
    const std::wstring native = long_path(full_path(widen(path)));
    const UniqueHandle h(CreateFileW(native.c_str(), FILE_READ_ATTRIBUTES,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                     FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!h.valid()) throw_last_error("cannot open " + path);
    FILE_ID_INFO id{};
    if (GetFileInformationByHandleEx(h.get(), FileIdInfo, &id, sizeof id)) return ref128(id.FileId);
    const DWORD error = GetLastError();
    // Older file systems and some network shares have no 128-bit id; the 64-bit index is the same
    // number NTFS puts in the low half.
    if (error != ERROR_INVALID_PARAMETER && error != ERROR_NOT_SUPPORTED && error != ERROR_INVALID_FUNCTION)
        throw_win32("cannot read the file id of " + path, error);
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(h.get(), &info)) throw_last_error("cannot read the file id of " + path);
    return ref64((static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow);
}

namespace {

// FileHardLinkInformation (46) of NtQueryInformationFile: every name of a file, each as its
// folder's 64-bit id and the name in it.
struct LinkEntry {
    ULONG next_entry_offset;
    LONGLONG parent_file_id;
    ULONG file_name_length;
    WCHAR file_name[1];
};
struct LinksInfo {
    ULONG bytes_needed;
    ULONG entries_returned;
    LinkEntry entry;
};
struct IoStatus {
    union {
        LONG status;
        PVOID pointer;
    };
    ULONG_PTR information;
};
using QueryInformationFile = LONG(NTAPI*)(HANDLE, IoStatus*, PVOID, ULONG, int);
constexpr int kFileHardLinkInformation = 46;
constexpr LONG kBufferOverflow = static_cast<LONG>(0x80000005L);
constexpr LONG kBufferTooSmall = static_cast<LONG>(0xC0000023L);

}  // namespace

std::vector<FileRef> link_parents(const std::string& volume_path, const std::vector<FileRef>& refs) {
    std::vector<FileRef> out;
    if (refs.empty()) return out;
    static const auto query = reinterpret_cast<QueryInformationFile>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationFile")));
    if (query == nullptr) throw Error("cannot list the names of changed files on this system");
    // Any handle on the volume serves as the hint for OpenFileById: its root folder.
    wchar_t root[MAX_PATH + 1] = {};
    if (!GetVolumePathNameW(widen(volume_path).c_str(), root, MAX_PATH)) throw_last_error("cannot find the volume of " + volume_path);
    const UniqueHandle hint(CreateFileW(root, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!hint.valid()) throw_last_error("cannot open the volume of " + volume_path);
    std::vector<std::uint8_t> buffer(4096);
    for (const FileRef& ref : refs) {
        FILE_ID_DESCRIPTOR id{};
        id.dwSize = sizeof id;
        id.Type = ExtendedFileIdType;
        std::memcpy(id.ExtendedFileId.Identifier, &ref.low, 8);
        std::memcpy(id.ExtendedFileId.Identifier + 8, &ref.high, 8);
        // No access rights are needed to read a file's names, so files the account cannot read
        // open too.
        const UniqueHandle file(OpenFileById(hint.get(), &id, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                             nullptr, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT));
        if (!file.valid()) {
            const DWORD err = GetLastError();
            // Gone since: its folders have records of their own. A file being deleted (still open
            // elsewhere) answers "access denied" (STATUS_DELETE_PENDING maps to it); with no access
            // rights asked for and the backup privilege on, that is the only way to get it here.
            if (err == ERROR_INVALID_PARAMETER || err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND ||
                err == ERROR_DELETE_PENDING || err == ERROR_CANT_ACCESS_FILE || err == ERROR_ACCESS_DENIED)
                continue;
            throw_win32("cannot open a changed file by its id", err);
        }
        LONG status = 0;
        for (int attempt = 0; attempt < 4; ++attempt) {
            IoStatus io{};
            status = query(file.get(), &io, buffer.data(), static_cast<ULONG>(buffer.size()), kFileHardLinkInformation);
            if (status != kBufferOverflow && status != kBufferTooSmall) break;
            const ULONG needed = reinterpret_cast<const LinksInfo*>(buffer.data())->bytes_needed;
            buffer.resize(std::max<std::size_t>(buffer.size() * 2, needed + 64));
        }
        if (status < 0) {
            // Folders and some special files have no list of names: one name, nothing to add.
            if (status == static_cast<LONG>(0xC0000003L) /* invalid info class */ ||
                status == static_cast<LONG>(0xC00000BBL) /* not supported */ ||
                status == static_cast<LONG>(0xC0000010L) /* invalid device request */ ||
                status == static_cast<LONG>(0xC00000BAL) /* is a directory */)
                continue;
            throw Error("cannot list the names of a changed file (status " + std::to_string(static_cast<unsigned long>(status)) + ")");
        }
        const auto* info = reinterpret_cast<const LinksInfo*>(buffer.data());
        if (info->entries_returned <= 1) continue;
        std::size_t offset = offsetof(LinksInfo, entry);
        for (ULONG i = 0; i < info->entries_returned && offset + sizeof(LinkEntry) <= buffer.size(); ++i) {
            const auto* e = reinterpret_cast<const LinkEntry*>(buffer.data() + offset);
            out.push_back(ref64(static_cast<std::uint64_t>(e->parent_file_id)));
            if (e->next_entry_offset == 0) break;
            offset += e->next_entry_offset;
        }
    }
    return out;
}

}  // namespace vaultik::os
