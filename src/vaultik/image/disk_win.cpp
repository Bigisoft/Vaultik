// Physical disks on Windows: listing, image backup sources, restore targets, VHDX attach and
// making a restored disk start.

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cwctype>
#include <thread>

#include "vaultik/crypto/crypto.hpp"
#include "vaultik/image/disk.hpp"
#include "vaultik/image/disk_tools.hpp"
#include "vaultik/platform/acl.hpp"
#include "vaultik/platform/drive_health.hpp"
#include "vaultik/platform/os.hpp"
#include "vaultik/platform/vss.hpp"
#include "vaultik/util/win.hpp"
#include "vaultik/util/win_handle.hpp"

#include <objbase.h>
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

// Every volume of the computer with where it lies: one entry per disk extent, so a volume over
// several disks (spanned, striped or mirrored dynamic volumes, Storage Spaces) is on each of its
// disks, marked DiskVolume::spans (such a volume cannot be imaged or erased partition by
// partition, and none of its disks may be erased).
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
        DiskVolume v;
        v.name = narrow(name);
        UniqueHandle h = open_device(volume_device(v.name), 0);
        if (!h.valid()) continue;
        // Grown as needed: a volume may lie on many disks.
        const Bytes ext = ioctl(h.get(), IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS);
        if (ext.size() < sizeof(VOLUME_DISK_EXTENTS)) continue;
        const auto* e = reinterpret_cast<const VOLUME_DISK_EXTENTS*>(ext.data());
        const std::size_t count = std::min<std::size_t>(
            e->NumberOfDiskExtents, (ext.size() - offsetof(VOLUME_DISK_EXTENTS, Extents)) / sizeof(DISK_EXTENT));
        if (count == 0) continue;
        wchar_t paths[1024];
        DWORD len = 0;
        if (GetVolumePathNamesForVolumeNameW(name, paths, 1024, &len) && paths[0] != L'\0') v.mount = narrow(paths);
        wchar_t label[MAX_PATH + 1] = {};
        wchar_t fs[MAX_PATH + 1] = {};
        if (GetVolumeInformationW(name, label, MAX_PATH, nullptr, nullptr, nullptr, fs, MAX_PATH)) {
            v.label = narrow(label);
            v.filesystem = narrow(fs);
        }
        v.spans = count > 1;
        for (std::size_t i = 0; i < count; ++i) {
            VolumeOnDisk vd;
            vd.v = v;
            vd.disk = static_cast<int>(e->Extents[i].DiskNumber);
            vd.v.offset = static_cast<std::uint64_t>(e->Extents[i].StartingOffset.QuadPart);
            vd.v.length = static_cast<std::uint64_t>(e->Extents[i].ExtentLength.QuadPart);
            out.push_back(std::move(vd));
        }
    } while (FindNextVolumeW(find, name, MAX_PATH));
    FindVolumeClose(find);
    return out;
}

// The volumes on `disk` (each once), in the order they lie.
std::vector<DiskVolume> volumes_on(const std::vector<VolumeOnDisk>& all, int disk) {
    std::vector<DiskVolume> out;
    for (const VolumeOnDisk& v : all) {
        if (v.disk != disk) continue;
        if (std::any_of(out.begin(), out.end(), [&](const DiskVolume& o) { return o.name == v.v.name; })) continue;
        out.push_back(v.v);
    }
    std::sort(out.begin(), out.end(), [](const DiskVolume& a, const DiskVolume& b) { return a.offset < b.offset; });
    return out;
}

std::vector<DiskVolume> volumes_of(int disk) {
    return volumes_on(all_volumes(), disk);
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

// What disk `n`, open as `h`, is: size, sectors, model, serial, bus, partition style and id
// (not its volumes, nor whether Windows runs from it). False when it has no medium.
bool probe_disk(HANDLE h, int n, DiskInfo& d) {
    d.number = n;
    const Bytes geo = ioctl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0, 1024);
    if (geo.size() < sizeof(DISK_GEOMETRY_EX) - 1) return false;  // no medium (an empty card reader)
    const auto* g = reinterpret_cast<const DISK_GEOMETRY_EX*>(geo.data());
    d.size = static_cast<std::uint64_t>(g->DiskSize.QuadPart);
    d.sector_size = g->Geometry.BytesPerSector;
    d.physical_sector_size = d.sector_size;
    d.removable = g->Geometry.MediaType == RemovableMedia;

    STORAGE_PROPERTY_QUERY q{};
    q.PropertyId = StorageDeviceProperty;
    q.QueryType = PropertyStandardQuery;
    const Bytes desc = ioctl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, 4096);
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
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, &al, sizeof al, &got, nullptr) &&
        got >= sizeof al && al.BytesPerPhysicalSector != 0)
        d.physical_sector_size = al.BytesPerPhysicalSector;
    // A drive without a seek penalty is a solid-state one.
    q.PropertyId = StorageDeviceSeekPenaltyProperty;
    DEVICE_SEEK_PENALTY_DESCRIPTOR seek{};
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, &seek, sizeof seek, &got, nullptr) &&
        got >= sizeof seek)
        d.media = seek.IncursSeekPenalty ? "HDD" : "SSD";
    d.path = narrow(disk_path(n));

    const RawLayout l = read_layout(h);
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
    for (const Partition& p : l.partitions) {
        if (!is_container(p.mbr_type)) ++d.partitions;
        if (pool_partition(p, l.style == PARTITION_STYLE_GPT)) d.dynamic = true;
    }
    GET_DISK_ATTRIBUTES attrs{};
    if (DeviceIoControl(h, IOCTL_DISK_GET_DISK_ATTRIBUTES, nullptr, 0, &attrs, sizeof attrs, &got, nullptr) &&
        (attrs.Attributes & (DISK_ATTRIBUTE_OFFLINE | DISK_ATTRIBUTE_READ_ONLY)) != 0)
        d.offline = true;
    return true;
}

class WinDiskTarget : public DiskTarget {
public:
    WinDiskTarget(const DiskInfo& d, const DiskIdentity& expected)
        : number_(d.number), size_(d.size), sector_(d.sector_size), buf_(4u << 20) {
        // The disk first, and the disk this handle reaches checked against what the user chose:
        // its number may belong to another disk by now.
        disk_ = open_device(disk_path(d.number), GENERIC_READ | GENERIC_WRITE, FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH);
        if (!disk_.valid()) throw_last_error("opening disk " + std::to_string(d.number) + " for writing");
        DiskInfo now;
        if (!probe_disk(disk_.get(), d.number, now)) throw Error("disk " + std::to_string(d.number) + " has no medium");
        const std::string other = identity_mismatch(now, expected);
        if (!other.empty()) throw Error(other + "; nothing was written");
        if (now.sector_size != sector_ || now.size != size_) throw Error("disk " + std::to_string(d.number) + " changed; nothing was written");
        // Every volume on the disk goes offline, so nothing writes to it meanwhile.
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
        // Every byte on the disk before the volumes come back (a failure here means the restore
        // did not finish).
        if (!FlushFileBuffers(disk_.get())) throw_last_error("writing disk " + std::to_string(number_));
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
    ~WinAttachedDisk() override {
        // Letters given here are removed again, or Windows would keep them for the disk's volumes.
        for (const std::wstring& m : added_) DeleteVolumeMountPointW(m.c_str());
        DetachVirtualDisk(h_.get(), DETACH_VIRTUAL_DISK_FLAG_NONE, 0);
    }
    int number() const override { return number_; }
    std::vector<std::string> mounts() const override {
        std::vector<std::string> out;
        for (const DiskVolume& v : volumes_of(number_))
            if (!v.mount.empty()) out.push_back(v.mount);
        return out;
    }
    void add_letter(const std::wstring& mount) { added_.push_back(mount); }

private:
    UniqueHandle h_;
    int number_;
    std::vector<std::wstring> added_;
};

char free_letter() {
    const DWORD used = GetLogicalDrives();
    for (char c = 'S'; c <= 'Z'; ++c)
        if (!(used & (1u << (c - 'A')))) return c;
    for (char c = 'R'; c >= 'D'; --c)
        if (!(used & (1u << (c - 'A')))) return c;
    return 0;
}

// Whether Windows gives new volumes drive letters by itself ("mountvol /N" turns that off, as
// on many servers).
bool automount_enabled() {
    DWORD value = 0;
    DWORD size = sizeof value;
    const LSTATUS e = RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\mountmgr", L"NoAutoMount",
                                   RRF_RT_REG_DWORD, nullptr, &value, &size);
    return e != ERROR_SUCCESS || value == 0;
}

// Whether a partition holds files someone would open: not the EFI system partition, the
// Microsoft reserved partition or a recovery partition (Windows gives those no letter either).
bool data_partition(const Partition& p, bool gpt) {
    if (gpt) return p.type != kEfiSystemType && p.type != kMsrType && p.type != kRecoveryType;
    return p.mbr_type != 0x27 && p.mbr_type != 0xEF;
}

// Whether this is Windows PE (a recovery drive).
bool in_windows_pe() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\MiniNT", 0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;
    RegCloseKey(key);
    return true;
}

// The volume holding the Windows folder ("\\?\Volume{GUID}\"), also when it is mounted in a
// folder; empty when it cannot be told.
std::string windows_volume() {
    wchar_t dir[MAX_PATH + 1];
    const UINT n = GetWindowsDirectoryW(dir, MAX_PATH);
    if (n == 0 || n > MAX_PATH) return {};
    wchar_t mount[MAX_PATH + 1];
    if (!GetVolumePathNameW(dir, mount, MAX_PATH)) return {};
    wchar_t volume[MAX_PATH + 1];
    if (!GetVolumeNameForVolumeMountPointW(mount, volume, MAX_PATH)) return {};
    return narrow(volume);
}

// The NT device of the partition the firmware started this Windows from ("\Device\HarddiskVolume1":
// the EFI system partition, or the active "System Reserved" partition of a BIOS computer), as
// Windows records it at every start. Empty when it cannot be read.
std::wstring system_partition_device() {
    wchar_t value[1024] = {};
    DWORD size = sizeof value;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\Setup", L"SystemPartition", RRF_RT_REG_SZ, nullptr, value, &size) !=
        ERROR_SUCCESS)
        return {};
    return value;
}

// The NT device of a volume ("\Device\HarddiskVolume3") from its name ("\\?\Volume{GUID}\").
std::wstring nt_device_of(const std::string& volume) {
    std::wstring w = widen(volume);
    if (w.starts_with(L"\\\\?\\")) w = w.substr(4);
    if (!w.empty() && w.back() == L'\\') w.pop_back();
    if (w.empty()) return {};
    wchar_t target[1024] = {};
    if (QueryDosDeviceW(w.c_str(), target, 1024) == 0) return {};
    return target;
}

// Whether a file is there. Asked of the folder (FindFirstFileW), not by opening the file: the
// page file and hibernation file in use cannot be opened at all. Anything but "not found" counts
// as there.
bool file_there(const std::wstring& path) {
    WIN32_FIND_DATAW data;
    const HANDLE find = FindFirstFileW(path.c_str(), &data);
    if (find != INVALID_HANDLE_VALUE) {
        FindClose(find);
        return true;
    }
    const DWORD e = GetLastError();
    // A volume without a file system Windows knows (erased, or never formatted) holds no files:
    // Windows cannot page or hibernate to it either.
    return e != ERROR_FILE_NOT_FOUND && e != ERROR_PATH_NOT_FOUND && e != ERROR_NO_MORE_FILES &&
           e != ERROR_UNRECOGNIZED_VOLUME;
}

// The drive letters of the page files Windows uses now ("\??\C:\pagefile.sys" in the registry).
std::string page_file_letters() {
    std::string out;
    std::vector<wchar_t> buf(8192, L'\0');
    DWORD size = static_cast<DWORD>(buf.size() * sizeof(wchar_t));
    const wchar_t* key = L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management";
    for (const wchar_t* value : {L"ExistingPageFiles", L"PagingFiles"}) {
        size = static_cast<DWORD>(buf.size() * sizeof(wchar_t));
        std::fill(buf.begin(), buf.end(), L'\0');
        if (RegGetValueW(HKEY_LOCAL_MACHINE, key, value, RRF_RT_REG_MULTI_SZ, nullptr, buf.data(), &size) != ERROR_SUCCESS)
            continue;
        for (const wchar_t* p = buf.data(); p < buf.data() + buf.size() && *p != L'\0'; p += wcslen(p) + 1) {
            std::wstring entry = p;
            if (entry.starts_with(L"\\??\\")) entry = entry.substr(4);
            if (entry.size() >= 2 && entry[1] == L':' && iswalpha(entry[0])) {
                const char c = static_cast<char>(towupper(entry[0]));
                if (out.find(c) == std::string::npos) out.push_back(c);
            } else if (entry.starts_with(L"?:")) {
                out.push_back('?');  // "on whichever drive is best": not one known letter
            }
        }
    }
    return out;
}

// A program of Windows by its full path (C:\Windows\System32\<name>): a program started with
// administrator rights is never looked up on the PATH or in the current folder.
std::string system_tool(const char* name) {
    wchar_t dir[MAX_PATH + 1];
    const UINT n = GetSystemDirectoryW(dir, MAX_PATH);
    if (n == 0 || n > MAX_PATH) throw Error("the Windows system folder was not found");
    return narrow(std::wstring_view(dir, n)) + "\\" + name;
}

}  // namespace

std::vector<int> system_disks() {
    const std::vector<VolumeOnDisk> all = all_volumes();
    std::vector<int> out;
    const auto add = [&](int n) {
        if (std::find(out.begin(), out.end(), n) == out.end()) out.push_back(n);
    };
    const std::string root = windows_root();
    if (!root.empty())
        for (const int n : os::disk_numbers(root)) add(n);
    if (in_windows_pe()) return out;  // runs from memory (and was started from the recovery drive)
    // The volume holding the Windows folder and the partition the firmware starts Windows from,
    // each on every disk it lies on. Either one not found: no disk is safe to write to.
    const std::string windows = windows_volume();
    const std::wstring boot = system_partition_device();
    bool found_windows = false;
    bool found_boot = false;
    for (const VolumeOnDisk& v : all) {
        if (!windows.empty() && upper(v.v.name) == upper(windows)) {
            found_windows = true;
            add(v.disk);
        }
        if (!boot.empty() && to_lower(nt_device_of(v.v.name)) == to_lower(boot)) {
            found_boot = true;
            add(v.disk);
        }
    }
    if (!found_windows || !found_boot) add(kUnknownSystemDisk);
    return out;
}

std::vector<DiskInfo> list_disks() {
    std::vector<DiskInfo> out;
    const std::vector<int> sys = system_disks();
    const std::vector<VolumeOnDisk> volumes = all_volumes();
    for (int n = 0; n < 128; ++n) {
        UniqueHandle h = open_device(disk_path(n), 0);
        if (!h.valid()) continue;
        DiskInfo d;
        if (!probe_disk(h.get(), n, d)) continue;
        d.volumes = volumes_on(volumes, n);
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
                if (v.offset != p.offset || v.spans) continue;
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
            for (const std::string& s : shadow->copy->warnings())
                if (warn) warn(s);
        } catch (const Error& e) {
            if (warn) warn(std::string("the disks are read while in use, without a shadow copy (") + e.what() + ")");
        }
    }
    std::vector<std::unique_ptr<DiskSource>> out;
    for (Prepared& pr : prepared)
        out.push_back(std::make_unique<WinDiskSource>(pr.number, std::move(pr.layout), shadow, std::move(pr.volumes)));
    return out;
}

std::unique_ptr<DiskTarget> open_disk_target(int number, const DiskIdentity& expected,
                                             const std::vector<int>& protected_disks) {
    const std::vector<DiskInfo> disks = list_disks();
    auto it = std::find_if(disks.begin(), disks.end(), [number](const DiskInfo& d) { return d.number == number; });
    if (it == disks.end()) throw Error("disk " + std::to_string(number) + " is not connected");
    const std::string why = target_refusal(*it, system_disks(), protected_disks);
    if (!why.empty()) throw Error("disk " + std::to_string(number) + " cannot be restored to: " + why);
    const std::string other = identity_mismatch(*it, expected);
    if (!other.empty()) throw Error(other + "; nothing was written");
    return std::make_unique<WinDiskTarget>(*it, expected);
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
    const bool automount = automount_enabled();
    // The volumes show up a moment later, and with automount Windows gives them letters.
    auto ready = [&] {
        const std::vector<DiskVolume> vols = volumes_of(number);
        if (!automount) return !vols.empty();
        return std::any_of(vols.begin(), vols.end(), [](const DiskVolume& v) { return !v.mount.empty(); });
    };
    for (int i = 0; i < 50 && !ready(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    if (!disk->mounts().empty()) return disk;

    // No letters (automount is off, or Windows did not get to it): give the volumes that hold
    // files one each.
    UniqueHandle dh = open_device(disk_path(number), 0);
    const RawLayout l = dh.valid() ? read_layout(dh.get()) : RawLayout{};
    const bool gpt = l.ok && l.style == PARTITION_STYLE_GPT;
    auto wanted = [&](const DiskVolume& v) {
        if (!v.mount.empty()) return false;
        const auto part = std::find_if(l.partitions.begin(), l.partitions.end(),
                                       [&](const Partition& p) { return p.offset == v.offset; });
        return part == l.partitions.end() || data_partition(*part, gpt);
    };
    // With automount off, Windows leaves the volumes of a newly attached disk offline: no file
    // system is mounted on them until they are brought online.
    bool brought_online = false;
    for (const DiskVolume& v : volumes_of(number)) {
        if (!wanted(v) || !v.filesystem.empty()) continue;
        UniqueHandle vh = open_device(volume_device(v.name), GENERIC_READ | GENERIC_WRITE);
        DWORD got = 0;
        if (vh.valid() && DeviceIoControl(vh.get(), IOCTL_VOLUME_ONLINE, nullptr, 0, nullptr, 0, &got, nullptr))
            brought_online = true;
    }
    auto has_files = [&] {
        const std::vector<DiskVolume> vols = volumes_of(number);
        return std::any_of(vols.begin(), vols.end(), [&](const DiskVolume& v) { return wanted(v) && !v.filesystem.empty(); });
    };
    for (int i = 0; brought_online && i < 25 && !has_files(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    // Only volumes with a file system get a letter: one without (unknown, damaged or encrypted)
    // would make Explorer offer to format it.
    for (const DiskVolume& v : volumes_of(number)) {
        if (!wanted(v) || v.filesystem.empty()) continue;
        const char c = free_letter();
        if (c == 0) break;
        const std::wstring m = std::wstring(1, static_cast<wchar_t>(c)) + L":\\";
        if (SetVolumeMountPointW(m.c_str(), widen(v.name).c_str())) disk->add_letter(m);
    }
    return disk;
}

std::string repair_boot(int number) {
    // The new partitions, and the file systems on them, show up a moment after the restore.
    std::vector<DiskVolume> vols;
    auto has_ntfs = [&] {
        return std::any_of(vols.begin(), vols.end(), [](const DiskVolume& v) { return v.filesystem == "NTFS"; });
    };
    for (int i = 0; i < 75; ++i) {
        vols = volumes_of(number);
        if (has_ntfs()) break;
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
        // Only NTFS volumes get a letter to look for Windows: a volume without a file system
        // (unknown, damaged or encrypted) would make Explorer offer to format it.
        if (v.filesystem != "NTFS") continue;
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
        {system_tool("bcdboot.exe"), os::join(win_root, "Windows"), "/s", sys_root, "/f", uefi ? "UEFI" : "BIOS"}, &output);
    if (code != 0) throw Error("bcdboot failed (exit code " + std::to_string(code) + "): " + output);
    return "Boot files written from " + win_root + "Windows to the " +
           (uefi ? std::string("EFI system partition") : std::string("active partition")) + ".";
}


// ---- disk tools (disk_tools.hpp) -------------------------------------------------------------

namespace {

// A disk or volume opened for writing, with its volumes locked and dismounted until it is
// closed. Unbuffered: every write goes through an aligned buffer.
class WinWipeDevice : public WipeDevice {
public:
    WinWipeDevice(UniqueHandle h, std::vector<UniqueHandle> locks, std::uint64_t size, std::uint32_t sector, std::string what,
                  bool whole_disk = false)
        : h_(std::move(h)), locks_(std::move(locks)), size_(size), sector_(sector), what_(std::move(what)),
          whole_disk_(whole_disk), buf_(4u << 20) {}
    void forget_partition_table() override {
        // On the handle that was checked to be the chosen disk (not the disk number opened again).
        if (!whole_disk_) return;
        DWORD got = 0;
        DeviceIoControl(h_.get(), IOCTL_DISK_DELETE_DRIVE_LAYOUT, nullptr, 0, nullptr, 0, &got, nullptr);
        DeviceIoControl(h_.get(), IOCTL_DISK_UPDATE_PROPERTIES, nullptr, 0, nullptr, 0, &got, nullptr);
    }
    std::uint64_t size() const override { return size_; }
    std::uint32_t block_size() const override { return sector_; }
    void write_at(std::uint64_t offset, ByteSpan data) override {
        std::size_t done = 0;
        while (done < data.size()) {
            const std::size_t n = std::min(buf_.size(), data.size() - done);
            std::memcpy(buf_.data(), data.data() + done, n);
            OVERLAPPED ov{};
            const std::uint64_t at = offset + done;
            ov.Offset = static_cast<DWORD>(at);
            ov.OffsetHigh = static_cast<DWORD>(at >> 32);
            DWORD put = 0;
            if (!WriteFile(h_.get(), buf_.data(), static_cast<DWORD>(n), &put, &ov) || put != n)
                throw_last_error("writing " + what_ + " at byte " + std::to_string(at));
            done += n;
        }
    }
    void read_at(std::uint64_t offset, std::uint8_t* dst, std::size_t length) override {
        vaultik::image::read_at(h_.get(), offset, dst, length, buf_, what_);
    }
    void flush() override {
        if (!FlushFileBuffers(h_.get())) throw_last_error("writing " + what_);
    }

private:
    UniqueHandle h_;
    std::vector<UniqueHandle> locks_;
    std::uint64_t size_;
    std::uint32_t sector_;
    std::string what_;
    bool whole_disk_;
    AlignedBuffer buf_;
};

class WinFillFile : public FillFile {
public:
    explicit WinFillFile(UniqueHandle h) : h_(std::move(h)) {}
    std::optional<std::uint64_t> allocated() override {
        FILE_STANDARD_INFO st{};
        if (!GetFileInformationByHandleEx(h_.get(), FileStandardInfo, &st, sizeof st)) return std::nullopt;
        auto took = static_cast<std::uint64_t>(st.AllocationSize.QuadPart);
        // A compressed file: what its compressed data takes.
        FILE_COMPRESSION_INFO ci{};
        if (GetFileInformationByHandleEx(h_.get(), FileCompressionInfo, &ci, sizeof ci) && ci.CompressionFormat != 0)
            took = std::min(took, static_cast<std::uint64_t>(ci.CompressedFileSize.QuadPart));
        return took;
    }
    FillResult write_at(std::uint64_t offset, ByteSpan data) override {
        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>(offset);
        ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
        DWORD put = 0;
        if (WriteFile(h_.get(), data.data(), static_cast<DWORD>(data.size()), &put, &ov) && put == data.size())
            return FillResult::Written;
        const DWORD e = GetLastError();
        if (e == ERROR_FILE_TOO_LARGE) return FillResult::TooLarge;
        if (e == ERROR_DISK_FULL || e == ERROR_HANDLE_DISK_FULL || put != 0) return FillResult::Full;
        throw_win32("writing the free space", e);
    }
    void flush() override {
        if (!FlushFileBuffers(h_.get()) && GetLastError() != ERROR_DISK_FULL) throw_last_error("writing the free space");
    }

private:
    UniqueHandle h_;
};

// Refuses a disk Windows keeps offline or read-only (the SAN policy of servers does that to new
// disks): bringing it online here would mount volumes nobody listed or locked. The user brings
// it online in Disk Management, and the disk is listed again.
void check_online(HANDLE h, int number) {
    GET_DISK_ATTRIBUTES attrs{};
    DWORD got = 0;
    if (!DeviceIoControl(h, IOCTL_DISK_GET_DISK_ATTRIBUTES, nullptr, 0, &attrs, sizeof attrs, &got, nullptr)) return;
    if ((attrs.Attributes & (DISK_ATTRIBUTE_OFFLINE | DISK_ATTRIBUTE_READ_ONLY)) != 0)
        throw Error("disk " + std::to_string(number) + " is offline or read-only in Windows; bring it online in Disk "
                    "Management first (right-click the disk, Online), then try again. Nothing was written");
}

// A volume opened to be locked (by its volume id, so the handle stays that volume whatever its
// letter), for lock_volumes. After a forced dismount, a lock that fails on the handle that
// dismounted it is tried on a new handle (the next open mounts it again, without the other
// programs' handles).
class WinVolumeLock : public VolumeLock {
public:
    WinVolumeLock(std::wstring device, std::string name, DWORD flags = 0)
        : device_(std::move(device)), name_(std::move(name)), flags_(flags) {
        h_ = open_device(device_, GENERIC_READ | GENERIC_WRITE, flags_);
        if (!h_.valid()) throw_last_error("opening " + name_);
    }
    std::string name() const override { return name_; }
    bool lock() override {
        DWORD got = 0;
        if (h_.valid() && DeviceIoControl(h_.get(), FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &got, nullptr)) return true;
        if (!forced_) return false;
        UniqueHandle again = open_device(device_, GENERIC_READ | GENERIC_WRITE, flags_);
        if (!again.valid()) return false;
        h_ = std::move(again);
        return DeviceIoControl(h_.get(), FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &got, nullptr) != 0;
    }
    void force_dismount() override {
        // Without the lock: Windows takes the volume away from every program that has it open.
        DWORD got = 0;
        DeviceIoControl(h_.get(), FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0, &got, nullptr);
        forced_ = true;
    }
    // The locked handle, kept open for as long as the volume must stay locked.
    UniqueHandle take() { return std::move(h_); }

private:
    std::wstring device_;
    std::string name_;
    DWORD flags_;
    UniqueHandle h_;
    bool forced_ = false;
};

// How the user knows volume `v`: its drive letter ("F:"), its folder, or its volume id.
std::string volume_title(const DiskVolume& v) {
    if (v.mount.size() >= 2 && v.mount.size() <= 3 && v.mount[1] == ':') return upper(v.mount.substr(0, 2));
    return v.mount.empty() ? v.name : v.mount;
}

// Disk `d` opened for writing, checked to be the disk the user chose, with every volume on it
// locked and dismounted (the handles in `locks`). A volume another program uses stops it with
// VolumesInUse, unless `force` (see lock_volumes).
UniqueHandle open_locked_disk(const DiskInfo& d, const DiskIdentity& expected, std::vector<UniqueHandle>& locks,
                              bool force = false) {
    UniqueHandle disk = open_device(disk_path(d.number), GENERIC_READ | GENERIC_WRITE, FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH);
    if (!disk.valid()) throw_last_error("opening disk " + std::to_string(d.number) + " for writing");
    DiskInfo now;
    if (!probe_disk(disk.get(), d.number, now)) throw Error("disk " + std::to_string(d.number) + " has no medium");
    const std::string other = identity_mismatch(now, expected);
    if (!other.empty()) throw Error(other + "; nothing was written");
    if (now.dynamic) throw Error("disk " + std::to_string(d.number) + " is a dynamic disk or part of a Storage Spaces pool; nothing was written");
    check_online(disk.get(), d.number);
    std::vector<std::unique_ptr<WinVolumeLock>> volumes;
    std::vector<VolumeLock*> to_lock;
    for (const DiskVolume& v : volumes_of(d.number)) {
        if (v.spans) throw Error("disk " + std::to_string(d.number) + " holds a part of a volume over several disks; nothing was written");
        // Checked again here, so nothing the running Windows needs is ever forced closed.
        const std::string use = windows_use_of(v);
        if (!use.empty()) throw Error("disk " + std::to_string(d.number) + " cannot be erased: " + use + "; nothing was written");
        volumes.push_back(std::make_unique<WinVolumeLock>(volume_device(v.name), volume_title(v)));
        to_lock.push_back(volumes.back().get());
    }
    lock_volumes(to_lock, force);
    for (auto& v : volumes) {
        UniqueHandle h = v->take();
        DWORD got = 0;
        DeviceIoControl(h.get(), FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0, &got, nullptr);
        locks.push_back(std::move(h));
    }
    return disk;
}

// The disk at d.number now, checked again: it may be erased and is still the one chosen.
DiskInfo recheck(const DiskInfo& d, const DiskIdentity& expected, const std::vector<int>& protected_disks) {
    if (!os::process_elevated()) throw Error("writing to a disk needs administrator rights");
    const std::vector<DiskInfo> disks = list_disks();
    auto it = std::find_if(disks.begin(), disks.end(), [&](const DiskInfo& x) { return x.number == d.number; });
    if (it == disks.end()) throw Error("disk " + std::to_string(d.number) + " is not connected any more");
    const std::string why = tool_refusal(*it, system_disks(), protected_disks);
    if (!why.empty()) throw Error("disk " + std::to_string(d.number) + " cannot be erased: " + why);
    const std::string other = identity_mismatch(*it, expected);
    if (!other.empty()) throw Error(other + "; nothing was written");
    return *it;
}

std::string drive_of(const std::string& path) {
    return path.size() >= 2 && path[1] == ':' ? upper(path.substr(0, 2)) : std::string();
}

}  // namespace

std::vector<DiskInfo> list_tool_disks() {
    return list_disks();
}

std::vector<int> protected_tool_disks() {
    return os::disk_numbers(os::executable_path());
}

std::unique_ptr<WipeDevice> open_wipe_file(const std::string& path) {
    UniqueHandle h(CreateFileW(long_path(full_path(widen(path))).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr));
    if (!h.valid()) throw_last_error("opening " + path);
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h.get(), &size)) throw_last_error("reading the size of " + path);
    const auto bytes = static_cast<std::uint64_t>(size.QuadPart);
    return std::make_unique<WinWipeDevice>(std::move(h), std::vector<UniqueHandle>{}, bytes, bytes % 512 == 0 ? 512 : 1, path);
}

std::unique_ptr<WipeDevice> open_wipe_disk(const DiskInfo& chosen, const DiskIdentity& expected,
                                           const std::vector<int>& protected_disks, bool force_dismount) {
    const DiskInfo d = recheck(chosen, expected, protected_disks);
    std::vector<UniqueHandle> locks;
    UniqueHandle disk = open_locked_disk(d, expected, locks, force_dismount);
    return std::make_unique<WinWipeDevice>(std::move(disk), std::move(locks), d.size, d.sector_size,
                                           "disk " + std::to_string(d.number), true);
}

std::string windows_use_of(const DiskVolume& v) {
    const std::string where = v.mount.empty() ? std::string("a volume of it") : v.mount.size() <= 3 ? v.mount.substr(0, 2) : v.mount;
    if (!v.name.empty()) {
        if (upper(v.name) == upper(windows_volume())) return where + " holds the Windows that is running";
        const std::wstring boot = system_partition_device();
        if (!boot.empty() && to_lower(nt_device_of(v.name)) == to_lower(boot))
            return where + " is the partition this computer starts Windows from (EFI system or System Reserved partition)";
    }
    if (v.mount.empty()) return {};
    for (const wchar_t* file : {L"pagefile.sys", L"swapfile.sys", L"hiberfil.sys"})
        if (file_there(join_path(widen(v.mount), file))) return where + " holds the Windows page or hibernation file";
    if (v.mount.size() <= 3 && v.mount.size() >= 2 && v.mount[1] == ':' &&
        page_file_letters().find(upper(v.mount.substr(0, 1))[0]) != std::string::npos)
        return where + " holds the Windows page file";
    return {};
}

VolumeTarget find_wipe_volume(const std::string& given, bool) {
    std::string name = upper(given);
    while (name.size() > 2 && (name.back() == '\\' || name.back() == '/')) name.pop_back();
    if (name.size() == 1) name += ":";
    if (name.size() != 2 || name[1] != ':' || name[0] < 'A' || name[0] > 'Z')
        throw Error("give the volume as a drive letter, such as E:");
    const std::wstring root = widen(name + "\\");
    wchar_t volume[MAX_PATH + 1] = {};
    if (!GetVolumeNameForVolumeMountPointW(root.c_str(), volume, MAX_PATH)) throw Error(name + " is not a drive of this computer");
    const UINT type = GetDriveTypeW(root.c_str());
    if (type == DRIVE_REMOTE || type == DRIVE_CDROM) throw Error(name + " is not a drive of this computer");
    if (drive_of(windows_root()) == name) throw Error("Windows is running from " + name);
    if (drive_of(os::executable_path()) == name) throw Error("Vaultik is running from " + name);
    const std::vector<VolumeOnDisk> all = all_volumes();
    auto it = std::find_if(all.begin(), all.end(), [&](const VolumeOnDisk& v) { return upper(v.v.name) == upper(narrow(volume)); });
    if (it == all.end()) throw Error(name + " is not on a disk of this computer");
    if (it->v.spans) throw Error(name + " lies on several disks (a dynamic or Storage Spaces volume)");
    const std::string use = windows_use_of(it->v);
    if (!use.empty()) throw Error(use);
    // What its partition is: an EFI system, reserved, recovery or active partition is refused,
    // and so is a volume that is not a partition of a basic disk.
    {
        UniqueHandle disk = open_device(disk_path(it->disk), 0);
        const RawLayout layout = disk.valid() ? read_layout(disk.get()) : RawLayout{};
        const bool gpt = layout.style == PARTITION_STYLE_GPT;
        auto part = std::find_if(layout.partitions.begin(), layout.partitions.end(),
                                 [&](const Partition& p) { return p.offset == it->v.offset && !is_container(p.mbr_type); });
        if (!layout.ok || part == layout.partitions.end()) throw Error(name + " is not a partition of a basic disk");
        const std::string why = partition_refusal(*part, gpt);
        if (!why.empty()) throw Error(name + " cannot be erased: " + why);
        if (std::any_of(layout.partitions.begin(), layout.partitions.end(), [&](const Partition& p) { return pool_partition(p, gpt); }))
            throw Error(name + " is on a dynamic disk or a Storage Spaces disk");
    }
    VolumeTarget t;
    t.name = name;
    t.device = narrow(volume_device(it->v.name));
    t.size = it->v.length;
    t.filesystem = it->v.filesystem;
    t.label = it->v.label;
    t.disk = it->disk;
    t.id = it->v.name;
    DWORD serial = 0;
    if (GetVolumeInformationW(volume, nullptr, 0, &serial, nullptr, nullptr, nullptr, 0)) {
        char text[16];
        std::snprintf(text, sizeof text, "%08X", static_cast<unsigned>(serial));
        t.serial = text;
    }
    return t;
}

std::unique_ptr<WipeDevice> open_wipe_volume(const VolumeTarget& given, const DiskIdentity& expected,
                                             bool force_dismount) {
    if (!os::process_elevated()) throw Error("erasing a volume needs administrator rights");
    // As the letter is now (another drive may have it by now), then on the opened volume.
    const VolumeTarget v = find_wipe_volume(given.name);
    std::string other = volume_mismatch(v, expected);
    if (!other.empty()) throw Error(other + "; nothing was written");
    // Opened by its volume id, so the handle is that volume whatever letter it has.
    WinVolumeLock volume(widen(v.device), v.name, FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH);
    // The serial number through the volume's own path, just before the lock: a handle to the
    // volume device itself is not a handle to a file, so GetVolumeInformationByHandleW may not
    // answer for it below.
    std::string serial_before;
    {
        DWORD serial = 0;
        if (GetVolumeInformationW(widen(v.id).c_str(), nullptr, 0, &serial, nullptr, nullptr, nullptr, 0)) {
            char text[16];
            std::snprintf(text, sizeof text, "%08X", static_cast<unsigned>(serial));
            serial_before = text;
        }
    }
    // Another program using it stops here (VolumesInUse), unless it is to be forced closed.
    lock_volumes({&volume}, force_dismount);
    UniqueHandle h = volume.take();
    DWORD got = 0;
    // Locked: nothing can change it any more. Its file system's serial number, read while it is
    // still mounted (on this handle where Windows answers: by handle, or NTFS's own volume data,
    // whose low 32 bits are the serial number shown; otherwise the one read just before the
    // lock), and its size, on this handle.
    VolumeTarget opened = v;
    opened.serial = serial_before;
    DWORD serial = 0;
    NTFS_VOLUME_DATA_BUFFER ntfs{};
    if (GetVolumeInformationByHandleW(h.get(), nullptr, 0, &serial, nullptr, nullptr, nullptr, 0)) {
        char text[16];
        std::snprintf(text, sizeof text, "%08X", static_cast<unsigned>(serial));
        opened.serial = text;
    } else if (DeviceIoControl(h.get(), FSCTL_GET_NTFS_VOLUME_DATA, nullptr, 0, &ntfs, sizeof ntfs, &got, nullptr)) {
        char text[16];
        std::snprintf(text, sizeof text, "%08X", static_cast<unsigned>(ntfs.VolumeSerialNumber.QuadPart & 0xFFFFFFFFu));
        opened.serial = text;
    }
    GET_LENGTH_INFORMATION len{};
    if (!DeviceIoControl(h.get(), IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0, &len, sizeof len, &got, nullptr))
        throw_last_error("reading the size of " + v.name);
    opened.size = static_cast<std::uint64_t>(len.Length.QuadPart);
    other = volume_mismatch(opened, expected);
    if (!other.empty()) throw Error(other + "; nothing was written");
    DeviceIoControl(h.get(), FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0, &got, nullptr);
    // Its last sectors too, past the end of the file system.
    DeviceIoControl(h.get(), FSCTL_ALLOW_EXTENDED_DASD_IO, nullptr, 0, nullptr, 0, &got, nullptr);
    std::uint32_t sector = 512;
    const Bytes geo = ioctl(h.get(), IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0, 1024);
    if (geo.size() >= sizeof(DISK_GEOMETRY)) {
        const auto* g = reinterpret_cast<const DISK_GEOMETRY_EX*>(geo.data());
        if (g->Geometry.BytesPerSector != 0) sector = g->Geometry.BytesPerSector;
    }
    const auto size = opened.size / sector * sector;
    return std::make_unique<WinWipeDevice>(std::move(h), std::vector<UniqueHandle>{}, size, sector, v.name);
}

std::unique_ptr<FillFile> create_fill_file(const std::string& path) {
    // Deleted when closed, also when the program is killed.
    UniqueHandle h(CreateFileW(long_path(full_path(widen(path))).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_DELETE_ON_CLOSE, nullptr));
    if (!h.valid()) throw_last_error("creating " + path);
    // Not compressed, also in a compressed folder (zeros would take no room); file systems
    // without compression refuse this, which is fine.
    USHORT none = COMPRESSION_FORMAT_NONE;
    DWORD got = 0;
    DeviceIoControl(h.get(), FSCTL_SET_COMPRESSION, &none, sizeof none, nullptr, 0, &got, nullptr);
    return std::make_unique<WinFillFile>(std::move(h));
}

NewDiskResult init_disk(const DiskInfo& chosen, const DiskIdentity& expected, const std::vector<int>& protected_disks,
                     const NewDiskOptions& o) {
    check_init_options(o);
    const std::string fs = o.filesystem.empty() ? std::string("ntfs") : o.filesystem;
    const bool gpt = o.style == TableStyle::Gpt;
    const auto step = [&](const std::string& s) {
        if (o.on_step) o.on_step(s);
    };
    const DiskInfo d = recheck(chosen, expected, protected_disks);
    const std::vector<NewPartition> parts = plan_partitions(d.size, d.sector_size, o.style, o.sizes, gpt);
    const std::string disk_name = "disk " + std::to_string(d.number);
    const std::string format = system_tool("format.com");
    std::string new_id;  // the id of the new partition table, to know the disk again below
    {
        std::vector<UniqueHandle> locks;
        UniqueHandle disk = open_locked_disk(d, expected, locks);
        HANDLE h = disk.get();
        DWORD got = 0;
        step("Removing the partitions of " + disk_name);
        DeviceIoControl(h, IOCTL_DISK_DELETE_DRIVE_LAYOUT, nullptr, 0, nullptr, 0, &got, nullptr);
        DeviceIoControl(h, IOCTL_DISK_UPDATE_PROPERTIES, nullptr, 0, nullptr, 0, &got, nullptr);

        step(std::string("Writing a new ") + (gpt ? "GPT" : "MBR") + " partition table");
        CREATE_DISK cd{};
        GUID disk_id{};
        std::uint32_t signature = 0;
        if (gpt) {
            if (FAILED(CoCreateGuid(&disk_id))) throw Error("cannot make a disk id");
            new_id = "gpt-" + guid_text(disk_id);
            cd.PartitionStyle = PARTITION_STYLE_GPT;
            cd.Gpt.DiskId = disk_id;
            cd.Gpt.MaxPartitionCount = 128;
        } else {
            while (signature == 0) random_bytes(std::span(reinterpret_cast<std::uint8_t*>(&signature), sizeof signature));
            char sig[16];
            std::snprintf(sig, sizeof sig, "%08X", signature);
            new_id = std::string("mbr-") + sig;
            cd.PartitionStyle = PARTITION_STYLE_MBR;
            cd.Mbr.Signature = signature;
        }
        if (!DeviceIoControl(h, IOCTL_DISK_CREATE_DISK, &cd, sizeof cd, nullptr, 0, &got, nullptr))
            throw_last_error("writing a new partition table on " + disk_name);
        DeviceIoControl(h, IOCTL_DISK_UPDATE_PROPERTIES, nullptr, 0, nullptr, 0, &got, nullptr);

        // The new layout: the table Windows just wrote (for GPT its usable area), with the partitions.
        const DWORD count = gpt ? static_cast<DWORD>(parts.size()) : 4;
        Bytes buf(offsetof(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry) + count * sizeof(PARTITION_INFORMATION_EX));
        auto* dl = reinterpret_cast<DRIVE_LAYOUT_INFORMATION_EX*>(buf.data());
        if (gpt) {
            const Bytes now = ioctl(h, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, nullptr, 0, 16384);
            if (now.size() < offsetof(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry))
                throw_last_error("reading the new partition table of " + disk_name);
            dl->Gpt = reinterpret_cast<const DRIVE_LAYOUT_INFORMATION_EX*>(now.data())->Gpt;
        } else {
            dl->Mbr.Signature = signature;
        }
        dl->PartitionStyle = gpt ? PARTITION_STYLE_GPT : PARTITION_STYLE_MBR;
        dl->PartitionCount = count;
        for (DWORD i = 0; i < count; ++i) {
            PARTITION_INFORMATION_EX& pe = dl->PartitionEntry[i];
            pe.PartitionStyle = gpt ? PARTITION_STYLE_GPT : PARTITION_STYLE_MBR;
            pe.RewritePartition = TRUE;
            if (i >= parts.size()) continue;  // an unused MBR entry
            const NewPartition& p = parts[i];
            pe.StartingOffset.QuadPart = static_cast<LONGLONG>(p.offset);
            pe.PartitionLength.QuadPart = static_cast<LONGLONG>(p.length);
            pe.PartitionNumber = i + 1;
            if (gpt) {
                pe.Gpt.PartitionType = to_guid(std::string(p.reserved ? kMsrType : kBasicDataType));
                if (FAILED(CoCreateGuid(&pe.Gpt.PartitionId))) throw Error("cannot make a partition id");
                pe.Gpt.Attributes = 0;
                const wchar_t* label = p.reserved ? L"Microsoft reserved partition" : L"Basic data partition";
                wcsncpy_s(pe.Gpt.Name, label, _TRUNCATE);
            } else {
                pe.Mbr.PartitionType = PARTITION_IFS;
                pe.Mbr.BootIndicator = FALSE;
                pe.Mbr.RecognizedPartition = TRUE;
                pe.Mbr.HiddenSectors = static_cast<DWORD>(p.offset / d.sector_size);
            }
        }
        if (!DeviceIoControl(h, IOCTL_DISK_SET_DRIVE_LAYOUT_EX, buf.data(), static_cast<DWORD>(buf.size()), nullptr, 0, &got,
                             nullptr))
            throw_last_error("writing the partitions of " + disk_name);
        DeviceIoControl(h, IOCTL_DISK_UPDATE_PROPERTIES, nullptr, 0, nullptr, 0, &got, nullptr);
        if (!FlushFileBuffers(h)) throw_last_error("writing " + disk_name);
    }  // the old volumes' locks go here

    NewDiskResult r;
    bool first = true;
    for (const NewPartition& p : parts) {
        if (p.reserved) continue;
        // The volume Windows makes for the partition, a moment later.
        std::optional<DiskVolume> vol;
        for (int i = 0; i < 100 && !vol; ++i) {
            for (const DiskVolume& v : volumes_of(d.number))
                if (v.offset == p.offset) vol = v;
            if (!vol) std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        if (!vol) throw Error("Windows did not make a volume for the new partition of " + disk_name);
        // The disk was let go of: checked again to be the one written to (its serial number,
        // size and the id of the table just written) before anything is formatted.
        {
            UniqueHandle again = open_device(disk_path(d.number), 0);
            DiskInfo now;
            if (!again.valid() || !probe_disk(again.get(), d.number, now) || now.size != d.size ||
                trim(now.serial) != trim(expected.serial) || upper(now.id) != upper(new_id))
                throw Error(disk_name + " is not the disk that was set up any more; nothing was formatted");
        }
        char letter = first && o.letter != 0 ? static_cast<char>(std::toupper(static_cast<unsigned char>(o.letter))) : 0;
        if (letter == 0 && !vol->mount.empty() && vol->mount.size() <= 3) letter = vol->mount[0];
        if (letter == 0) letter = free_letter();
        if (letter == 0) throw Error("no drive letter is free for the new partition");
        const std::string root = std::string(1, letter) + ":\\";
        if (upper(vol->mount) != upper(root)) {
            if (!vol->mount.empty()) DeleteVolumeMountPointW(widen(vol->mount).c_str());
            if (!SetVolumeMountPointW(widen(root).c_str(), widen(vol->name).c_str()))
                throw_last_error("giving the new partition the letter " + root.substr(0, 2));
        }
        step("Formatting " + root.substr(0, 2) + " as " + (fs == "ntfs" ? std::string("NTFS") : std::string("exFAT")));
        const std::string label = o.label.empty() ? std::string("New Volume") : o.label;
        std::string output;
        const int code = os::run_command({format, root.substr(0, 2), fs == "ntfs" ? "/FS:NTFS" : "/FS:exFAT", "/Q",
                                          "/V:" + label, "/Y"},
                                         &output);
        if (code != 0) throw Error("format.com failed on " + root.substr(0, 2) + " (exit code " + std::to_string(code) + "): " + trim(output));
        r.volumes.push_back(root);
        first = false;
    }
    return r;
}

}  // namespace vaultik::image
