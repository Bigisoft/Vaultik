#include "vaultik/platform/usn_journal.hpp"

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
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

std::vector<std::string> other_names(const std::string& volume_path, const FileRef& ref) {
    // Any handle on the volume serves as the hint for OpenFileById: its root folder.
    wchar_t root[MAX_PATH + 1] = {};
    if (!GetVolumePathNameW(widen(volume_path).c_str(), root, MAX_PATH)) throw_last_error("cannot find the volume of " + volume_path);
    const UniqueHandle hint(CreateFileW(root, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!hint.valid()) throw_last_error("cannot open the volume of " + volume_path);
    FILE_ID_DESCRIPTOR id{};
    id.dwSize = sizeof id;
    id.Type = ExtendedFileIdType;
    std::memcpy(id.ExtendedFileId.Identifier, &ref.low, 8);
    std::memcpy(id.ExtendedFileId.Identifier + 8, &ref.high, 8);
    const UniqueHandle file(OpenFileById(hint.get(), &id, FILE_READ_ATTRIBUTES,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                         FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT));
    if (!file.valid()) {
        const DWORD err = GetLastError();
        if (err == ERROR_INVALID_PARAMETER || err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND ||
            err == ERROR_DELETE_PENDING)
            return {};  // gone since: its folders have records of their own
        throw_win32("cannot open a changed file by its id", err);
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file.get(), &info)) throw_last_error("cannot read a changed file");
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 || info.nNumberOfLinks <= 1) return {};
    // Each name, as "\folder\file" on the volume, put after the volume's root ("C:\").
    std::wstring prefix = root;
    while (!prefix.empty() && prefix.back() == L'\\') prefix.pop_back();
    std::wstring path(MAX_PATH, L'\0');
    DWORD n = GetFinalPathNameByHandleW(file.get(), path.data(), static_cast<DWORD>(path.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_NONE);
    if (n >= path.size()) {
        path.resize(n + 1);
        n = GetFinalPathNameByHandleW(file.get(), path.data(), static_cast<DWORD>(path.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_NONE);
    }
    if (n == 0 || n >= path.size()) throw_last_error("cannot name a changed file");
    path.resize(n);
    const std::wstring any = prefix + path;
    std::vector<std::string> out;
    std::wstring name(1024, L'\0');
    DWORD len = static_cast<DWORD>(name.size());
    HANDLE find = FindFirstFileNameW(any.c_str(), 0, &len, name.data());
    if (find == INVALID_HANDLE_VALUE && GetLastError() == ERROR_MORE_DATA) {
        name.resize(len + 1);
        len = static_cast<DWORD>(name.size());
        find = FindFirstFileNameW(any.c_str(), 0, &len, name.data());
    }
    if (find == INVALID_HANDLE_VALUE) throw_last_error("cannot list the names of a changed file");
    for (int guard = 0; guard < 1100; ++guard) {  // NTFS allows 1024 names per file
        out.push_back(narrow(prefix + std::wstring(name.c_str())));
        len = static_cast<DWORD>(name.size());
        if (!FindNextFileNameW(find, &len, name.data())) {
            if (GetLastError() != ERROR_MORE_DATA) break;
            name.resize(len + 1);
            len = static_cast<DWORD>(name.size());
            if (!FindNextFileNameW(find, &len, name.data())) break;
        }
    }
    FindClose(find);
    return out;
}

}  // namespace vaultik::os
