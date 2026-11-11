// Physical disks on Windows: listing, image backup sources, restore targets, VHDX attach and
// making a restored disk start.

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

#include "vaultik/image/disk.hpp"
#include "vaultik/platform/drive_health.hpp"
#include "vaultik/platform/os.hpp"
#include "vaultik/platform/vss.hpp"
#include "vaultik/util/win.hpp"
#include "vaultik/util/win_handle.hpp"

#include <winioctl.h>
#include <virtdisk.h>

namespace vaultik::image {

namespace {

// VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT, defined here so no GUID library is needed.
constexpr GUID kVendorMicrosoft = {0xEC984AEC, 0xA0F9, 0x47E9, {0x90, 0x1F, 0x71, 0x41, 0x5A, 0x66, 0x34, 0x5B}};

std::wstring disk_path(int number) {
    return L"\\\\.\\PhysicalDrive" + std::to_wstring(number);
}

// "\\?\Volume{GUID}\" -> "\\.\Volume{GUID}" (the device itself, not its root folder).
std::wstring volume_device(const std::string& name) {
    std::wstring w = widen(name);
    if (!w.empty() && w.back() == L'\\') w.pop_back();
    if (w.starts_with(L"\\\\?\\")) w[2] = L'.';
    return w;
}

UniqueHandle open_device(const std::wstring& path, DWORD access, DWORD flags = 0) {
    return UniqueHandle(CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                    flags, nullptr));
}

// An IOCTL whose answer can be long: retried with a larger buffer. Empty on failure.
Bytes ioctl(HANDLE h, DWORD code, const void* in = nullptr, DWORD in_size = 0, std::size_t start = 4096) {
    Bytes out(start);
    for (int tries = 0; tries < 12; ++tries) {
        DWORD got = 0;
        if (DeviceIoControl(h, code, const_cast<void*>(in), in_size, out.data(), static_cast<DWORD>(out.size()), &got,
                            nullptr)) {
            out.resize(got);
            return out;
        }
        const DWORD e = GetLastError();
        if (e != ERROR_INSUFFICIENT_BUFFER && e != ERROR_MORE_DATA) return {};
        out.resize(out.size() * 2);
    }
    return {};
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
    std::size_t b = 0;
    while (b < s.size() && s[b] == ' ') ++b;
    return s.substr(b);
}

std::string descriptor_string(const Bytes& buf, DWORD offset) {
    if (offset == 0 || offset >= buf.size()) return {};
    const char* p = reinterpret_cast<const char*>(buf.data()) + offset;
    const std::size_t max = buf.size() - offset;
    return trim(std::string(p, strnlen(p, max)));
}

const char* bus_name(STORAGE_BUS_TYPE t) {
    switch (t) {
        case BusTypeNvme: return "NVMe";
        case BusTypeSata: return "SATA";
        case BusTypeAta: return "ATA";
        case BusTypeUsb: return "USB";
        case BusTypeSas: return "SAS";
        case BusTypeScsi: return "SCSI";
        case BusTypeRAID: return "RAID";
        case BusTypeSd: return "SD card";
        case BusTypeMmc: return "MMC";
        case BusTypeFileBackedVirtual:
        case BusTypeVirtual: return "Virtual";
        case BusType1394: return "FireWire";
        case BusTypeiScsi: return "iSCSI";
        case BusTypeSpaces: return "Storage Spaces";
        default: return "";
    }
}

std::string guid_text(const GUID& g) {
    Guid b{};
    std::memcpy(b.data(), &g, 16);
    return format_guid(b);
}

GUID to_guid(const std::string& text) {
    const Guid b = parse_guid(text);
    GUID g;
    std::memcpy(&g, b.data(), 16);
    return g;
}

// Every volume of the computer with where it lies (single-extent volumes only: spanned and
// striped dynamic volumes cannot be imaged partition by partition).
struct VolumeOnDisk {
    DiskVolume v;
    int disk = -1;
};

std::vector<VolumeOnDisk> all_volumes() {
    std::vector<VolumeOnDisk> out;
    wchar_t name[MAX_PATH + 1];
    HANDLE find = FindFirstVolumeW(name, MAX_PATH);
    if (find == INVALID_HANDLE_VALUE) return out;
    do {
        VolumeOnDisk vd;
        vd.v.name = narrow(name);
        UniqueHandle h = open_device(volume_device(vd.v.name), 0);
        if (!h.valid()) continue;
        const Bytes ext = ioctl(h.get(), IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS);
        if (ext.size() < sizeof(VOLUME_DISK_EXTENTS)) continue;
        const auto* e = reinterpret_cast<const VOLUME_DISK_EXTENTS*>(ext.data());
        if (e->NumberOfDiskExtents != 1) continue;
        vd.disk = static_cast<int>(e->Extents[0].DiskNumber);
        vd.v.offset = static_cast<std::uint64_t>(e->Extents[0].StartingOffset.QuadPart);
        vd.v.length = static_cast<std::uint64_t>(e->Extents[0].ExtentLength.QuadPart);
        wchar_t paths[1024];
        DWORD len = 0;
        if (GetVolumePathNamesForVolumeNameW(name, paths, 1024, &len) && paths[0] != L'\0') vd.v.mount = narrow(paths);
        wchar_t label[MAX_PATH + 1] = {};
        wchar_t fs[MAX_PATH + 1] = {};
        if (GetVolumeInformationW(name, label, MAX_PATH, nullptr, nullptr, nullptr, fs, MAX_PATH)) {
            vd.v.label = narrow(label);
            vd.v.filesystem = narrow(fs);
        }
        out.push_back(std::move(vd));
    } while (FindNextVolumeW(find, name, MAX_PATH));
    FindVolumeClose(find);
    return out;
}

std::vector<DiskVolume> volumes_of(int disk) {
    std::vector<DiskVolume> out;
    for (VolumeOnDisk& v : all_volumes())
        if (v.disk == disk) out.push_back(std::move(v.v));
    std::sort(out.begin(), out.end(), [](const DiskVolume& a, const DiskVolume& b) { return a.offset < b.offset; });
    return out;
}

struct RawLayout {
    bool ok = false;
    PARTITION_STYLE style = PARTITION_STYLE_RAW;
    std::string disk_guid;
    std::uint32_t signature = 0;
    std::uint32_t max_entries = 128;
    std::vector<Partition> partitions;
};

RawLayout read_layout(HANDLE h) {
    RawLayout r;
    const Bytes buf = ioctl(h, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, nullptr, 0, 16384);
    if (buf.size() < sizeof(DRIVE_LAYOUT_INFORMATION_EX)) return r;
    const auto* l = reinterpret_cast<const DRIVE_LAYOUT_INFORMATION_EX*>(buf.data());
    r.ok = true;
    r.style = static_cast<PARTITION_STYLE>(l->PartitionStyle);
    if (r.style == PARTITION_STYLE_GPT) {
        r.disk_guid = guid_text(l->Gpt.DiskId);
        r.max_entries = l->Gpt.MaxPartitionCount;
    } else if (r.style == PARTITION_STYLE_MBR) {
        r.signature = l->Mbr.Signature;
    }
    const std::size_t count = std::min<std::size_t>(
        l->PartitionCount, (buf.size() - offsetof(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry)) / sizeof(PARTITION_INFORMATION_EX));
    for (std::size_t i = 0; i < count; ++i) {
        const PARTITION_INFORMATION_EX& pe = l->PartitionEntry[i];
        if (pe.PartitionLength.QuadPart <= 0) continue;
        if (pe.PartitionStyle == PARTITION_STYLE_MBR && pe.Mbr.PartitionType == 0) continue;
        Partition p;
        p.number = static_cast<int>(pe.PartitionNumber);
        p.offset = static_cast<std::uint64_t>(pe.StartingOffset.QuadPart);
        p.length = static_cast<std::uint64_t>(pe.PartitionLength.QuadPart);
        if (pe.PartitionStyle == PARTITION_STYLE_GPT) {
            p.type = guid_text(pe.Gpt.PartitionType);
            p.guid = guid_text(pe.Gpt.PartitionId);
            p.attributes = pe.Gpt.Attributes;
            p.name = narrow(std::wstring_view(pe.Gpt.Name, wcsnlen(pe.Gpt.Name, 36)));
        } else {
            p.mbr_type = pe.Mbr.PartitionType;
            p.active = pe.Mbr.BootIndicator != FALSE;
        }
        r.partitions.push_back(std::move(p));
    }
    std::sort(r.partitions.begin(), r.partitions.end(), [](const Partition& a, const Partition& b) {
        return a.offset < b.offset || (a.offset == b.offset && a.length > b.length);  // a container before its first logical
    });
    return r;
}

// Page-aligned memory for unbuffered device I/O.
class AlignedBuffer {
public:
    explicit AlignedBuffer(std::size_t size) : size_(size) {
        p_ = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (p_ == nullptr) throw_last_error("allocating an I/O buffer");
    }
    ~AlignedBuffer() { VirtualFree(p_, 0, MEM_RELEASE); }
    AlignedBuffer(const AlignedBuffer&) = delete;
    AlignedBuffer& operator=(const AlignedBuffer&) = delete;
    std::uint8_t* data() { return p_; }
    std::size_t size() const { return size_; }

private:
    std::uint8_t* p_ = nullptr;
    std::size_t size_;
};

void read_at(HANDLE h, std::uint64_t offset, std::uint8_t* dst, std::size_t length, AlignedBuffer& buf,
             const std::string& what) {
    while (length > 0) {
        const std::size_t n = std::min(length, buf.size());
        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>(offset);
        ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
        DWORD got = 0;
        if (!ReadFile(h, buf.data(), static_cast<DWORD>(n), &got, &ov))
            throw_last_error("reading " + what + " at " + std::to_string(offset));
        if (got != n) throw Error("reading " + what + " at " + std::to_string(offset) + ": cut short");
        std::memcpy(dst, buf.data(), n);
        dst += n;
        offset += n;
        length -= n;
    }
}

std::string windows_root() {
    wchar_t dir[MAX_PATH + 1];
    const UINT n = GetWindowsDirectoryW(dir, MAX_PATH);
    if (n == 0 || n > MAX_PATH) return {};
    std::string s = narrow(std::wstring_view(dir, n));
    return s.size() >= 3 ? s.substr(0, 3) : std::string();
}

std::string upper(std::string s) {
    for (char& c : s)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return s;
}

// Byte ranges of a file's clusters on its volume (the page file and the like). Empty when the
// file is not there or cannot be opened.
Ranges file_clusters(const std::wstring& path, std::uint32_t cluster) {
    Ranges out;
    UniqueHandle h(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!h.valid()) return out;
    STARTING_VCN_INPUT_BUFFER in{};
    in.StartingVcn.QuadPart = 0;
    Bytes buf(64 * 1024);
    for (;;) {
        DWORD got = 0;
        const BOOL ok = DeviceIoControl(h.get(), FSCTL_GET_RETRIEVAL_POINTERS, &in, sizeof in, buf.data(),
                                        static_cast<DWORD>(buf.size()), &got, nullptr);
        const DWORD e = ok ? ERROR_SUCCESS : GetLastError();
        if (!ok && e != ERROR_MORE_DATA) break;
        const auto* rp = reinterpret_cast<const RETRIEVAL_POINTERS_BUFFER*>(buf.data());
        LONGLONG vcn = rp->StartingVcn.QuadPart;
        for (DWORD i = 0; i < rp->ExtentCount; ++i) {
            const LONGLONG next = rp->Extents[i].NextVcn.QuadPart;
            const LONGLONG lcn = rp->Extents[i].Lcn.QuadPart;
            if (lcn >= 0 && next > vcn)
                out.emplace_back(static_cast<std::uint64_t>(lcn) * cluster, static_cast<std::uint64_t>(next - vcn) * cluster);
            vcn = next;
        }
        if (ok || rp->ExtentCount == 0) break;
        in.StartingVcn.QuadPart = vcn;
    }
    return out;
}

// What the shadow copies of all sources share.
struct Shadow {
    std::unique_ptr<os::ShadowCopy> copy;
};

class WinDiskSource : public DiskSource {
public:
    WinDiskSource(int number, DiskLayout layout, std::shared_ptr<Shadow> shadow, std::vector<std::string> volumes)
        : number_(number), layout_(std::move(layout)), shadow_(std::move(shadow)), volumes_(std::move(volumes)),
          buf_(4u << 20) {
        disk_ = open_device(disk_path(number), GENERIC_READ, FILE_FLAG_NO_BUFFERING);
        if (!disk_.valid()) throw_last_error("opening disk " + std::to_string(number) + " (administrator rights are needed)");
        handles_.resize(layout_.partitions.size());
        fs_end_.resize(layout_.partitions.size(), 0);
    }

    DiskLayout& layout() override { return layout_; }

    std::vector<Extent> plan(std::size_t i) override {
        Partition& p = layout_.partitions[i];
        const std::string& volume = volumes_[i];
        if (p.method != ReadMethod::Used || volume.empty()) {
            p.method = ReadMethod::Raw;
            return {{0, p.length, Extent::Kind::Data}};
        }
        // From the shadow copy when there is one, otherwise the live volume.
        std::wstring device = volume_device(volume);
        std::wstring root = widen(volume);  // where its files are, with a trailing backslash
        if (shadow_->copy) {
            const std::string mapped = shadow_->copy->map(volume);
            if (mapped != volume) {
                root = widen(mapped);
                if (root.empty() || root.back() != L'\\') root.push_back(L'\\');
                device = root.substr(0, root.size() - 1);
                p.shadow_copy = true;
            }
        }
        UniqueHandle h = open_device(device, GENERIC_READ, FILE_FLAG_NO_BUFFERING);
        if (!h.valid()) throw_last_error("opening " + describe(p));
        NTFS_VOLUME_DATA_BUFFER nv{};
        DWORD got = 0;
        if (!DeviceIoControl(h.get(), FSCTL_GET_NTFS_VOLUME_DATA, nullptr, 0, &nv, sizeof nv, &got, nullptr)) {
            // Not NTFS after all: every byte.
            p.method = ReadMethod::Raw;
            p.shadow_copy = false;
            return {{0, p.length, Extent::Kind::Data}};
        }
        p.cluster_size = nv.BytesPerCluster;
        const std::uint64_t clusters = static_cast<std::uint64_t>(nv.TotalClusters.QuadPart);
        STARTING_LCN_INPUT_BUFFER in{};
        in.StartingLcn.QuadPart = 0;
        Bytes bm = ioctl(h.get(), FSCTL_GET_VOLUME_BITMAP, &in, sizeof in,
                         static_cast<std::size_t>(sizeof(VOLUME_BITMAP_BUFFER) + clusters / 8 + 64));
        if (bm.size() < sizeof(VOLUME_BITMAP_BUFFER)) throw_last_error("reading which clusters of " + describe(p) + " are in use");
        const auto* vb = reinterpret_cast<const VOLUME_BITMAP_BUFFER*>(bm.data());
        const std::size_t header = offsetof(VOLUME_BITMAP_BUFFER, Buffer);
        const std::uint64_t bits = std::min<std::uint64_t>(static_cast<std::uint64_t>(vb->BitmapSize.QuadPart),
                                                           (bm.size() - header) * 8);
        const ByteSpan bitmap(bm.data() + header, static_cast<std::size_t>((bits + 7) / 8));
        // The page file, hibernation file and swap file come back empty after a restore: Windows
        // makes them again. Their clusters are asked of the volume read (the shadow copy), as they
        // were at that moment: the live files may have moved or grown since, onto clusters that
        // held other files then. Their first 1 MiB is still written by a restore, as zeros: a
        // hibernation file left as it was on the target (another Windows, hibernated or shut
        // down with fast startup) would be resumed over the restored volume.
        Ranges unused;
        Ranges keep;
        for (const wchar_t* f : {L"pagefile.sys", L"hiberfil.sys", L"swapfile.sys"}) {
            const Ranges r = file_clusters(root + f, nv.BytesPerCluster);
            if (r.empty()) continue;
            unused.insert(unused.end(), r.begin(), r.end());
            keep.emplace_back(r.front().first, std::min<std::uint64_t>(r.front().second, 1u << 20));
        }
        // Bytes past the last cluster (the copy of the boot sector) lie outside the file system,
        // and a volume handle refuses them: read() takes them from the disk.
        fs_end_[i] = std::min<std::uint64_t>(clusters * nv.BytesPerCluster, p.length);
        handles_[i] = std::move(h);
        return plan_extents(bitmap, std::min<std::uint64_t>(clusters, bits), nv.BytesPerCluster, p.length, unused,
                            kMinHole, keep);
    }

    void read(std::size_t i, std::uint64_t offset, std::uint8_t* dst, std::size_t length) override {
        const Partition& p = layout_.partitions[i];
        if (handles_[i].valid() && offset < fs_end_[i]) {
            const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(length, fs_end_[i] - offset));
            read_at(handles_[i].get(), offset, dst, n, buf_, describe(p));
            offset += n;
            dst += n;
            length -= n;
        }
        if (length > 0) read_at(disk_.get(), p.offset + offset, dst, length, buf_, "disk " + std::to_string(number_));
    }

    void read_disk(std::uint64_t offset, std::uint8_t* dst, std::size_t length) override {
        read_at(disk_.get(), offset, dst, length, buf_, "disk " + std::to_string(number_));
    }

private:
    int number_;
    DiskLayout layout_;
    std::shared_ptr<Shadow> shadow_;
    std::vector<std::string> volumes_;  // per partition: its volume name, or empty
    UniqueHandle disk_;
    std::vector<UniqueHandle> handles_;  // per partition: the volume read from (Used)
    std::vector<std::uint64_t> fs_end_;  // per partition: where the volume's clusters end (Used)
    AlignedBuffer buf_;
};

class WinDiskTarget : public DiskTarget {
public:
    explicit WinDiskTarget(const DiskInfo& d) : number_(d.number), size_(d.size), sector_(d.sector_size), buf_(4u << 20) {
        // Every volume on the disk goes offline first, so nothing writes to it meanwhile.
        for (const DiskVolume& v : volumes_of(d.number)) {
            UniqueHandle h = open_device(volume_device(v.name), GENERIC_READ | GENERIC_WRITE);
            if (!h.valid()) throw_last_error("opening " + (v.mount.empty() ? v.name : v.mount));
            DWORD got = 0;
            if (!DeviceIoControl(h.get(), FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &got, nullptr))
                throw Error((v.mount.empty() ? std::string("a volume of the disk") : v.mount) +
                            " is in use; close every program using it and try again");
            DeviceIoControl(h.get(), FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0, &got, nullptr);
            locked_.push_back(std::move(h));
        }
        disk_ = open_device(disk_path(d.number), GENERIC_READ | GENERIC_WRITE, FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH);
        if (!disk_.valid()) throw_last_error("opening disk " + std::to_string(d.number) + " for writing");
    }

    std::uint64_t size() const override { return size_; }
    std::uint32_t sector_size() const override { return sector_; }
    bool zeroed() const override { return false; }
    std::uint32_t alignment() const override { return sector_; }

    void write(std::uint64_t offset, ByteSpan data) override {
        if (offset % sector_ != 0 || data.size() % sector_ != 0) throw Error("internal error: unaligned disk write");
        std::size_t done = 0;
        while (done < data.size()) {
            const std::size_t n = std::min(buf_.size(), data.size() - done);
            std::memcpy(buf_.data(), data.data() + done, n);
            OVERLAPPED ov{};
            const std::uint64_t at = offset + done;
            ov.Offset = static_cast<DWORD>(at);
            ov.OffsetHigh = static_cast<DWORD>(at >> 32);
            DWORD put = 0;
            if (!WriteFile(disk_.get(), buf_.data(), static_cast<DWORD>(n), &put, &ov) || put != n)
                throw_last_error("writing disk " + std::to_string(number_) + " at " + std::to_string(at));
            done += n;
        }
    }

    void finish() override {
        FlushFileBuffers(disk_.get());
        DWORD got = 0;
        DeviceIoControl(disk_.get(), IOCTL_DISK_UPDATE_PROPERTIES, nullptr, 0, nullptr, 0, &got, nullptr);
        locked_.clear();
    }

private:
    int number_;
    std::uint64_t size_;
    std::uint32_t sector_;
    UniqueHandle disk_;
    std::vector<UniqueHandle> locked_;
    AlignedBuffer buf_;
};

class WinRandomFile : public RandomFile {
public:
    explicit WinRandomFile(UniqueHandle h) : h_(std::move(h)) {}
    void write_at(std::uint64_t offset, ByteSpan data) override {
        std::size_t done = 0;
        while (done < data.size()) {
            const DWORD n = static_cast<DWORD>(std::min<std::size_t>(data.size() - done, 64u << 20));
            OVERLAPPED ov{};
            const std::uint64_t at = offset + done;
            ov.Offset = static_cast<DWORD>(at);
            ov.OffsetHigh = static_cast<DWORD>(at >> 32);
            DWORD put = 0;
            if (!WriteFile(h_.get(), data.data() + done, n, &put, &ov) || put != n) throw_last_error("writing the disk file");
            done += n;
        }
    }
    void read_at(std::uint64_t offset, std::uint8_t* dst, std::size_t length) override {
        std::size_t done = 0;
        while (done < length) {
            const DWORD n = static_cast<DWORD>(std::min<std::size_t>(length - done, 64u << 20));
            OVERLAPPED ov{};
            const std::uint64_t at = offset + done;
            ov.Offset = static_cast<DWORD>(at);
            ov.OffsetHigh = static_cast<DWORD>(at >> 32);
            DWORD got = 0;
            if (!ReadFile(h_.get(), dst + done, n, &got, &ov)) throw_last_error("reading the disk file");
            if (got != n) throw Error("reading the disk file: cut short");
            done += n;
        }
    }
    std::uint64_t size() override {
        LARGE_INTEGER s{};
        if (!GetFileSizeEx(h_.get(), &s)) throw_last_error("reading the size of the disk file");
        return static_cast<std::uint64_t>(s.QuadPart);
    }
    void resize(std::uint64_t size) override {
        FILE_END_OF_FILE_INFO eof{};
        eof.EndOfFile.QuadPart = static_cast<LONGLONG>(size);
        if (!SetFileInformationByHandle(h_.get(), FileEndOfFileInfo, &eof, sizeof eof))
            throw_last_error("growing the disk file");
    }
    void flush() override {
        if (!FlushFileBuffers(h_.get())) throw_last_error("writing the disk file");
    }

private:
    UniqueHandle h_;
};

class WinAttachedDisk : public AttachedDisk {
public:
    WinAttachedDisk(UniqueHandle h, int number) : h_(std::move(h)), number_(number) {}
    ~WinAttachedDisk() override { DetachVirtualDisk(h_.get(), DETACH_VIRTUAL_DISK_FLAG_NONE, 0); }
    int number() const override { return number_; }
    std::vector<std::string> mounts() const override {
        std::vector<std::string> out;
        for (const DiskVolume& v : volumes_of(number_))
            if (!v.mount.empty()) out.push_back(v.mount);
        return out;
    }

private:
    UniqueHandle h_;
    int number_;
};

char free_letter() {
    const DWORD used = GetLogicalDrives();
    for (char c = 'S'; c <= 'Z'; ++c)
        if (!(used & (1u << (c - 'A')))) return c;
    for (char c = 'R'; c >= 'D'; --c)
        if (!(used & (1u << (c - 'A')))) return c;
    return 0;
}

}  // namespace

std::vector<int> system_disks() {
    const std::string root = windows_root();
    if (root.empty()) return {};
    return os::disk_numbers(root);
}

std::vector<DiskInfo> list_disks() {
    std::vector<DiskInfo> out;
    const std::vector<int> sys = system_disks();
    const std::vector<VolumeOnDisk> volumes = all_volumes();
    for (int n = 0; n < 128; ++n) {
        UniqueHandle h = open_device(disk_path(n), 0);
        if (!h.valid()) continue;
        DiskInfo d;
        d.number = n;
        const Bytes geo = ioctl(h.get(), IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0, 1024);
        if (geo.size() < sizeof(DISK_GEOMETRY_EX) - 1) continue;  // no medium (an empty card reader)
        const auto* g = reinterpret_cast<const DISK_GEOMETRY_EX*>(geo.data());
        d.size = static_cast<std::uint64_t>(g->DiskSize.QuadPart);
        d.sector_size = g->Geometry.BytesPerSector;
        d.physical_sector_size = d.sector_size;
        d.removable = g->Geometry.MediaType == RemovableMedia;

        STORAGE_PROPERTY_QUERY q{};
        q.PropertyId = StorageDeviceProperty;
        q.QueryType = PropertyStandardQuery;
        const Bytes desc = ioctl(h.get(), IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, 4096);
        if (desc.size() >= sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
            const auto* sd = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(desc.data());
            const std::string vendor = descriptor_string(desc, sd->VendorIdOffset);
            const std::string product = descriptor_string(desc, sd->ProductIdOffset);
            d.model = trim(vendor.empty() || product.starts_with(vendor) ? product : vendor + " " + product);
            d.serial = descriptor_string(desc, sd->SerialNumberOffset);
            d.bus = bus_name(sd->BusType);
            if (sd->RemovableMedia) d.removable = true;
        }
        q.PropertyId = StorageAccessAlignmentProperty;
        STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR al{};
        DWORD got = 0;
        if (DeviceIoControl(h.get(), IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, &al, sizeof al, &got, nullptr) &&
            got >= sizeof al && al.BytesPerPhysicalSector != 0)
            d.physical_sector_size = al.BytesPerPhysicalSector;

        const RawLayout l = read_layout(h.get());
        if (l.style == PARTITION_STYLE_GPT) {
            d.style = "GPT";
            d.id = "gpt-" + l.disk_guid;
        } else if (l.style == PARTITION_STYLE_MBR) {
            d.style = "MBR";
            char sig[16];
            std::snprintf(sig, sizeof sig, "%08X", l.signature);
            d.id = std::string("mbr-") + sig;
        } else {
            d.style = "RAW";
        }
        for (const Partition& p : l.partitions)
            if (!is_container(p.mbr_type)) ++d.partitions;
        for (const VolumeOnDisk& v : volumes)
            if (v.disk == n) d.volumes.push_back(v.v);
        std::sort(d.volumes.begin(), d.volumes.end(), [](const DiskVolume& a, const DiskVolume& b) { return a.offset < b.offset; });
        d.system = std::find(sys.begin(), sys.end(), n) != sys.end();
        out.push_back(std::move(d));
    }
    return out;
}

std::vector<std::unique_ptr<DiskSource>> open_disk_sources(const std::vector<int>& numbers, bool shadow_copy,
                                                           const std::function<void(const std::string&)>& warn) {
    const std::vector<DiskInfo> disks = list_disks();
    const std::string win = upper(windows_root());
    struct Prepared {
        int number;
        DiskLayout layout;
        std::vector<std::string> volumes;
    };
    std::vector<Prepared> prepared;
    std::vector<std::string> ntfs;  // volumes to shadow
    for (const int n : numbers) {
        auto it = std::find_if(disks.begin(), disks.end(), [n](const DiskInfo& d) { return d.number == n; });
        if (it == disks.end()) throw Error("disk " + std::to_string(n) + " is not connected");
        UniqueHandle h = open_device(disk_path(n), GENERIC_READ);
        if (!h.valid()) throw_last_error("opening disk " + std::to_string(n) + " (administrator rights are needed)");
        const RawLayout raw = read_layout(h.get());
        if (!raw.ok || raw.style == PARTITION_STYLE_RAW)
            throw Error("disk " + std::to_string(n) + " has no partition table (it may be a dynamic disk or empty)");
        Prepared pr;
        pr.number = n;
        DiskLayout& l = pr.layout;
        l.style = raw.style == PARTITION_STYLE_GPT ? PartitionStyle::Gpt : PartitionStyle::Mbr;
        l.sector_size = it->sector_size;
        l.physical_sector_size = it->physical_sector_size;
        l.size = it->size;
        l.disk_guid = raw.disk_guid;
        l.signature = raw.signature;
        l.gpt_entries = raw.max_entries == 0 ? 128 : raw.max_entries;
        l.number = n;
        l.model = it->model;
        l.serial = it->serial;
        l.system = it->system;
        l.partitions = raw.partitions;
        l.id = disk_id(l);
        for (Partition& p : l.partitions) {
            std::string volume;
            for (const DiskVolume& v : it->volumes) {
                if (v.offset != p.offset) continue;
                volume = v.name;
                p.filesystem = v.filesystem;
                p.label = v.label;
                p.mount = v.mount;
                if (!v.mount.empty() && upper(v.mount) == win) p.windows = true;
                DWORD serial = 0, maxlen = 0, flags = 0;
                if (GetVolumeInformationW(widen(v.name).c_str(), nullptr, 0, &serial, &maxlen, &flags, nullptr, 0))
                    p.serial = serial;
            }
            if (p.filesystem == "NTFS" && !volume.empty()) {
                p.method = ReadMethod::Used;
                ntfs.push_back(volume);
            } else {
                p.method = ReadMethod::Raw;
            }
            pr.volumes.push_back(p.method == ReadMethod::Used ? volume : std::string());
        }
        prepared.push_back(std::move(pr));
    }

    auto shadow = std::make_shared<Shadow>();
    if (shadow_copy && !ntfs.empty()) {
        try {
            shadow->copy = os::ShadowCopy::create(ntfs);
            for (const std::string& s : shadow->copy->skipped())
                if (warn) warn("no shadow copy of " + s + "; it is read while in use");
        } catch (const Error& e) {
            if (warn) warn(std::string("the disks are read while in use, without a shadow copy (") + e.what() + ")");
        }
    }
    std::vector<std::unique_ptr<DiskSource>> out;
    for (Prepared& pr : prepared)
        out.push_back(std::make_unique<WinDiskSource>(pr.number, std::move(pr.layout), shadow, std::move(pr.volumes)));
    return out;
}

std::unique_ptr<DiskTarget> open_disk_target(int number) {
    const std::vector<DiskInfo> disks = list_disks();
    auto it = std::find_if(disks.begin(), disks.end(), [number](const DiskInfo& d) { return d.number == number; });
    if (it == disks.end()) throw Error("disk " + std::to_string(number) + " is not connected");
    const std::string why = target_refusal(*it, system_disks(), {});
    if (!why.empty()) throw Error("disk " + std::to_string(number) + " cannot be restored to: " + why);
    return std::make_unique<WinDiskTarget>(*it);
}

std::unique_ptr<RandomFile> create_random_file(const std::string& path) {
    UniqueHandle h(CreateFileW(long_path(full_path(widen(path))).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                               CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!h.valid()) throw_last_error("creating " + path);
    return std::make_unique<WinRandomFile>(std::move(h));
}

std::unique_ptr<RandomFile> open_random_file(const std::string& path) {
    UniqueHandle h(CreateFileW(long_path(full_path(widen(path))).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!h.valid()) throw_last_error("opening " + path);
    return std::make_unique<WinRandomFile>(std::move(h));
}

std::unique_ptr<AttachedDisk> attach_vhdx(const std::string& path, bool read_only) {
    VIRTUAL_STORAGE_TYPE type{VIRTUAL_STORAGE_TYPE_DEVICE_VHDX, kVendorMicrosoft};
    OPEN_VIRTUAL_DISK_PARAMETERS op{};
    op.Version = OPEN_VIRTUAL_DISK_VERSION_1;
    op.Version1.RWDepth = 1;
    HANDLE raw = INVALID_HANDLE_VALUE;
    const std::wstring wpath = full_path(widen(path));
    DWORD e = OpenVirtualDisk(&type, wpath.c_str(),
                              read_only ? static_cast<VIRTUAL_DISK_ACCESS_MASK>(VIRTUAL_DISK_ACCESS_ATTACH_RO | VIRTUAL_DISK_ACCESS_DETACH |
                                                                                VIRTUAL_DISK_ACCESS_GET_INFO)
                                        : VIRTUAL_DISK_ACCESS_ALL,
                              OPEN_VIRTUAL_DISK_FLAG_NONE, &op, &raw);
    if (e != ERROR_SUCCESS) throw_win32("opening the virtual disk " + path, e);
    UniqueHandle h(raw);
    ATTACH_VIRTUAL_DISK_PARAMETERS ap{};
    ap.Version = ATTACH_VIRTUAL_DISK_VERSION_1;
    e = AttachVirtualDisk(h.get(), nullptr, read_only ? ATTACH_VIRTUAL_DISK_FLAG_READ_ONLY : ATTACH_VIRTUAL_DISK_FLAG_NONE, 0,
                          &ap, nullptr);
    if (e != ERROR_SUCCESS) throw_win32("attaching the virtual disk (administrator rights are needed)", e);
    wchar_t phys[MAX_PATH + 1] = {};
    ULONG size = sizeof phys;
    e = GetVirtualDiskPhysicalPath(h.get(), &size, phys);
    if (e != ERROR_SUCCESS) {
        DetachVirtualDisk(h.get(), DETACH_VIRTUAL_DISK_FLAG_NONE, 0);
        throw_win32("finding the attached virtual disk", e);
    }
    const std::wstring p = phys;
    const std::size_t at = p.find(L"PhysicalDrive");
    const int number = at == std::wstring::npos ? -1 : _wtoi(p.c_str() + at + 13);
    auto disk = std::make_unique<WinAttachedDisk>(std::move(h), number);
    // Windows mounts the volumes on its own a moment later.
    for (int i = 0; i < 50 && disk->mounts().empty(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    return disk;
}

std::string repair_boot(int number) {
    // The new partitions show up a moment after the restore.
    std::vector<DiskVolume> vols;
    for (int i = 0; i < 75; ++i) {
        vols = volumes_of(number);
        if (!vols.empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    if (vols.empty()) throw Error("the restored disk shows no volumes yet; restart and try again");
    UniqueHandle h = open_device(disk_path(number), 0);
    const RawLayout l = h.valid() ? read_layout(h.get()) : RawLayout{};
    if (!l.ok) throw Error("cannot read the partitions of disk " + std::to_string(number));
    const bool uefi = l.style == PARTITION_STYLE_GPT;

    // Letters for volumes that have none, removed again at the end.
    std::vector<std::wstring> added;
    struct Cleanup {
        std::vector<std::wstring>& added;
        ~Cleanup() {
            for (const std::wstring& m : added) DeleteVolumeMountPointW(m.c_str());
        }
    } cleanup{added};
    auto mount_of = [&](DiskVolume& v) -> std::string {
        if (!v.mount.empty()) return v.mount;
        const char c = free_letter();
        if (c == 0) throw Error("no free drive letter");
        const std::wstring m = std::wstring(1, static_cast<wchar_t>(c)) + L":\\";
        if (!SetVolumeMountPointW(m.c_str(), widen(v.name).c_str())) throw_last_error("giving a volume a drive letter");
        added.push_back(m);
        v.mount = narrow(m);
        return v.mount;
    };

    DiskVolume* windows = nullptr;
    for (DiskVolume& v : vols) {
        if (v.filesystem != "NTFS" && !v.filesystem.empty()) continue;
        const std::string root = mount_of(v);
        if (os::exists(os::join(root, "Windows\\System32\\config\\SYSTEM"))) {
            windows = &v;
            break;
        }
    }
    if (windows == nullptr) throw Error("no Windows folder was found on the restored disk");

    std::uint64_t system_offset = windows->offset;
    for (const Partition& p : l.partitions) {
        if (uefi && p.type == kEfiSystemType) system_offset = p.offset;
        if (!uefi && p.active) system_offset = p.offset;
    }
    DiskVolume* system = nullptr;
    for (DiskVolume& v : vols)
        if (v.offset == system_offset) system = &v;
    if (system == nullptr) throw Error(uefi ? "the EFI system partition of the restored disk was not found"
                                            : "the active partition of the restored disk was not found");
    const std::string win_root = mount_of(*windows);
    std::string sys_root = mount_of(*system);
    sys_root = sys_root.substr(0, 2);
    std::string output;
    const int code = os::run_command(
        {"bcdboot.exe", os::join(win_root, "Windows"), "/s", sys_root, "/f", uefi ? "UEFI" : "BIOS"}, &output);
    if (code != 0) throw Error("bcdboot failed (exit code " + std::to_string(code) + "): " + output);
    return "Boot files written from " + win_root + "Windows to the " +
           (uefi ? std::string("EFI system partition") : std::string("active partition")) + ".";
}

}  // namespace vaultik::image
