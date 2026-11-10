#include "vaultik/platform/drive_health.hpp"

#include <windows.h>
#include <winioctl.h>
#include <wbemidl.h>
#include <nvme.h>
#include <ntddscsi.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <map>
#include <vector>

#include "vaultik/platform/acl.hpp"
#include "vaultik/platform/os.hpp"
#include "vaultik/util/win.hpp"

namespace vaultik::os {

namespace {

// COM for this thread, for as long as it lives (a thread that already has it keeps its own).
struct ComScope {
    bool owned = false;
    ComScope() { owned = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)); }
    ~ComScope() {
        if (owned) CoUninitialize();
    }
};

template <class T>
struct ComPtr {
    T* p = nullptr;
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ~ComPtr() {
        if (p != nullptr) p->Release();
    }
    T** put() { return &p; }
    T* operator->() const { return p; }
};

struct Bstr {
    BSTR b;
    explicit Bstr(const wchar_t* s) : b(SysAllocString(s)) {}
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
    ~Bstr() { SysFreeString(b); }
};

struct Variant {
    VARIANT v;
    Variant() { VariantInit(&v); }
    Variant(const Variant&) = delete;
    Variant& operator=(const Variant&) = delete;
    ~Variant() { VariantClear(&v); }
};

// WMI's integer types arrive as VT_I4 (8 to 32 bits) or, for 64 bits, as a decimal string.
std::optional<std::uint64_t> number(IWbemClassObject* o, const wchar_t* name) {
    Variant v;
    if (FAILED(o->Get(name, 0, &v.v, nullptr, nullptr))) return std::nullopt;
    switch (v.v.vt) {
        case VT_I4: return static_cast<std::uint64_t>(static_cast<std::uint32_t>(v.v.lVal));
        case VT_UI1: return v.v.bVal;
        case VT_I2: return static_cast<std::uint64_t>(static_cast<std::uint16_t>(v.v.iVal));
        case VT_UI4: return v.v.ulVal;
        case VT_BSTR: return std::wcstoull(v.v.bstrVal, nullptr, 10);
        default: return std::nullopt;
    }
}

std::string text(IWbemClassObject* o, const wchar_t* name) {
    Variant v;
    if (FAILED(o->Get(name, 0, &v.v, nullptr, nullptr)) || v.v.vt != VT_BSTR || v.v.bstrVal == nullptr) return {};
    return narrow(v.v.bstrVal);
}

std::vector<std::uint16_t> numbers(IWbemClassObject* o, const wchar_t* name) {
    std::vector<std::uint16_t> out;
    Variant v;
    if (FAILED(o->Get(name, 0, &v.v, nullptr, nullptr)) || (v.v.vt & VT_ARRAY) == 0 || v.v.parray == nullptr) return out;
    LONG lo = 0, hi = -1;
    SafeArrayGetLBound(v.v.parray, 1, &lo);
    SafeArrayGetUBound(v.v.parray, 1, &hi);
    for (LONG i = lo; i <= hi; ++i) {
        LONG value = 0;
        if (SUCCEEDED(SafeArrayGetElement(v.v.parray, &i, &value))) out.push_back(static_cast<std::uint16_t>(value));
    }
    return out;
}

const char* bus_name(std::uint64_t b) {
    switch (b) {
        case 1: return "SCSI";
        case 3: return "ATA";
        case 6: return "Fibre Channel";
        case 7: return "USB";
        case 8: return "RAID";
        case 9: return "iSCSI";
        case 10: return "SAS";
        case 11: return "SATA";
        case 12: return "SD";
        case 13: return "MMC";
        case 15: return "File-backed";
        case 16: return "Storage Spaces";
        case 17: return "NVMe";
        default: return "";
    }
}

const char* media_name(std::uint64_t m) {
    switch (m) {
        case 3: return "HDD";
        case 4: return "SSD";
        case 5: return "SCM";
        default: return "";
    }
}

// The worst operational state that is not plain "OK", in words.
std::string condition_of(const std::vector<std::uint16_t>& states) {
    for (const std::uint16_t s : states) {
        switch (s) {
            case 3: return "Degraded";
            case 4: return "Stressed";
            case 5: return "Predictive failure";
            case 6: return "Error";
            case 7: return "Non-recoverable error";
            case 10: return "Stopped";
            case 12: return "No contact";
            case 13: return "Lost communication";
            case 16: return "Supporting part in error";
            case 0xD00D: return "In maintenance";
            case 0xD012: return "Transient error";
            case 0xD013: return "Failed media";
            default: break;
        }
    }
    return {};
}

// An NVMe drive's own health log (SMART / Health Information, log page 2). Unlike the ATA
// S.M.A.R.T. data, Windows hands it to any user through IOCTL_STORAGE_QUERY_PROPERTY on a
// handle opened without read or write access. Fills in what WMI left unknown, and turns the
// drive's critical warnings into a warning when Windows still calls it healthy.
void nvme_health(DriveHealth& d) {
    const std::wstring device = L"\\\\.\\PhysicalDrive" + std::to_wstring(d.disk);
    const HANDLE h = CreateFileW(device.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    constexpr DWORD kLog = sizeof(NVME_HEALTH_INFO_LOG);
    const DWORD size = FIELD_OFFSET(STORAGE_PROPERTY_QUERY, AdditionalParameters) + sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA) + kLog;
    std::vector<std::uint8_t> buffer(size);
    auto* query = reinterpret_cast<STORAGE_PROPERTY_QUERY*>(buffer.data());
    auto* request = reinterpret_cast<STORAGE_PROTOCOL_SPECIFIC_DATA*>(query->AdditionalParameters);
    query->PropertyId = StorageDeviceProtocolSpecificProperty;
    query->QueryType = PropertyStandardQuery;
    request->ProtocolType = ProtocolTypeNvme;
    request->DataType = NVMeDataTypeLogPage;
    request->ProtocolDataRequestValue = NVME_LOG_PAGE_HEALTH_INFO;
    request->ProtocolDataRequestSubValue = 0;
    request->ProtocolDataOffset = sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA);
    request->ProtocolDataLength = kLog;
    DWORD got = 0;
    const BOOL ok = DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, buffer.data(), size, buffer.data(), size, &got, nullptr);
    CloseHandle(h);
    if (!ok || got < sizeof(STORAGE_PROTOCOL_DATA_DESCRIPTOR)) return;
    const auto* answer = reinterpret_cast<const STORAGE_PROTOCOL_DATA_DESCRIPTOR*>(buffer.data());
    const STORAGE_PROTOCOL_SPECIFIC_DATA& data = answer->ProtocolSpecificData;
    const std::size_t start = offsetof(STORAGE_PROTOCOL_DATA_DESCRIPTOR, ProtocolSpecificData) + data.ProtocolDataOffset;
    if (data.ProtocolDataLength < kLog || start + kLog > got) return;
    NVME_HEALTH_INFO_LOG log;
    std::memcpy(&log, buffer.data() + start, kLog);

    if (!d.wear) d.wear = log.PercentageUsed;
    const int kelvin = log.Temperature[0] | (log.Temperature[1] << 8);
    if (!d.temperature && kelvin > 273) d.temperature = kelvin - 273;
    std::uint8_t critical = 0;
    std::memcpy(&critical, &log.CriticalWarning, 1);
    if (critical != 0 && d.status == DriveHealth::Status::Healthy) {
        d.status = DriveHealth::Status::Warning;
        // The bits of the critical warning, most serious first (several can be set at once).
        static constexpr std::pair<std::uint8_t, const char*> kWarnings[] = {
            {0x04, "Reliability degraded"},
            {0x08, "Read-only"},
            {0x01, "Spare space low"},
            {0x02, "Temperature out of range"},
        };
        if (d.condition.empty()) {
            d.condition = "Critical warning";
            for (const auto& [bit, text] : kWarnings)
                if (critical & bit) {
                    d.condition = text;
                    break;
                }
        }
    }
}

// ATA S.M.A.R.T. commands: SMART READ DATA (feature 0xD0) and READ THRESHOLDS (0xD1), each
// answering one 512-byte sector.
constexpr std::uint8_t kSmartCommand = 0xB0;
constexpr std::uint8_t kReadData = 0xD0;
constexpr std::uint8_t kReadThresholds = 0xD1;

// The classic SMART interface of the storage drivers (SATA on the standard AHCI driver).
bool smart_rcv(HANDLE h, std::uint8_t feature, std::uint8_t* out512) {
    SENDCMDINPARAMS in{};
    in.cBufferSize = 512;
    in.irDriveRegs.bFeaturesReg = feature;
    in.irDriveRegs.bSectorCountReg = 1;
    in.irDriveRegs.bSectorNumberReg = 1;
    in.irDriveRegs.bCylLowReg = SMART_CYL_LOW;
    in.irDriveRegs.bCylHighReg = SMART_CYL_HI;
    in.irDriveRegs.bDriveHeadReg = 0xA0;
    in.irDriveRegs.bCommandReg = kSmartCommand;
    std::vector<std::uint8_t> buffer(sizeof(SENDCMDOUTPARAMS) - 1 + 512);
    DWORD got = 0;
    if (!DeviceIoControl(h, SMART_RCV_DRIVE_DATA, &in, sizeof(in) - 1, buffer.data(), static_cast<DWORD>(buffer.size()),
                         &got, nullptr))
        return false;
    const auto* answer = reinterpret_cast<const SENDCMDOUTPARAMS*>(buffer.data());
    if (answer->DriverStatus.bDriverError != 0 || got < sizeof(SENDCMDOUTPARAMS) - 1 + 512) return false;
    std::memcpy(out512, answer->bBuffer, 512);
    return true;
}

// ATA pass-through (drivers that do not offer the classic interface, some RAID drivers).
bool ata_pass_through(HANDLE h, std::uint8_t feature, std::uint8_t* out512) {
    struct {
        ATA_PASS_THROUGH_EX apt;
        ULONG filler;
        std::uint8_t data[512];
    } buffer{};
    buffer.apt.Length = sizeof(ATA_PASS_THROUGH_EX);
    buffer.apt.AtaFlags = ATA_FLAGS_DATA_IN | ATA_FLAGS_DRDY_REQUIRED;
    buffer.apt.DataTransferLength = 512;
    buffer.apt.TimeOutValue = 5;
    buffer.apt.DataBufferOffset = offsetof(decltype(buffer), data);
    // Features, sector count, LBA low, LBA mid, LBA high, device, command.
    UCHAR* task = buffer.apt.CurrentTaskFile;
    task[0] = feature;
    task[1] = 1;
    task[2] = 1;
    task[3] = SMART_CYL_LOW;
    task[4] = SMART_CYL_HI;
    task[5] = 0xA0;
    task[6] = kSmartCommand;
    DWORD got = 0;
    if (!DeviceIoControl(h, IOCTL_ATA_PASS_THROUGH, &buffer, sizeof buffer, &buffer, sizeof buffer, &got, nullptr))
        return false;
    if ((buffer.apt.CurrentTaskFile[6] & 0x01) != 0) return false;  // the drive's status: error
    std::memcpy(out512, buffer.data, 512);
    return true;
}

// SCSI-ATA translation (SAT): what USB enclosures understand. The 12-byte ATA PASS-THROUGH
// command first, the 16-byte one for bridges that only know that.
bool sat_pass_through(HANDLE h, std::uint8_t feature, std::uint8_t* out512, bool sixteen) {
    struct {
        SCSI_PASS_THROUGH spt;
        ULONG filler;
        std::uint8_t sense[32];
        std::uint8_t data[512];
    } buffer{};
    buffer.spt.Length = sizeof(SCSI_PASS_THROUGH);
    buffer.spt.CdbLength = sixteen ? 16 : 12;
    buffer.spt.SenseInfoLength = sizeof buffer.sense;
    buffer.spt.DataIn = SCSI_IOCTL_DATA_IN;
    buffer.spt.DataTransferLength = 512;
    buffer.spt.TimeOutValue = 5;
    buffer.spt.DataBufferOffset = offsetof(decltype(buffer), data);
    buffer.spt.SenseInfoOffset = offsetof(decltype(buffer), sense);
    UCHAR* cdb = buffer.spt.Cdb;
    if (!sixteen) {
        cdb[0] = 0xA1;  // ATA PASS-THROUGH (12)
        cdb[1] = 4 << 1;  // protocol: PIO data-in
        cdb[2] = 0x0E;  // to the host, length in sectors, from the sector count
        cdb[3] = feature;
        cdb[4] = 1;
        cdb[5] = 1;
        cdb[6] = SMART_CYL_LOW;
        cdb[7] = SMART_CYL_HI;
        cdb[8] = 0xA0;
        cdb[9] = kSmartCommand;
    } else {
        cdb[0] = 0x85;  // ATA PASS-THROUGH (16)
        cdb[1] = 4 << 1;
        cdb[2] = 0x0E;
        cdb[4] = feature;
        cdb[6] = 1;
        cdb[8] = 1;
        cdb[10] = SMART_CYL_LOW;
        cdb[12] = SMART_CYL_HI;
        cdb[13] = 0xA0;
        cdb[14] = kSmartCommand;
    }
    DWORD got = 0;
    if (!DeviceIoControl(h, IOCTL_SCSI_PASS_THROUGH, &buffer, sizeof buffer, &buffer, sizeof buffer, &got, nullptr))
        return false;
    if (buffer.spt.ScsiStatus != 0 || buffer.spt.DataTransferLength < 512) return false;
    std::memcpy(out512, buffer.data, 512);
    return true;
}

// An ATA drive's attribute table, through whichever interface its driver answers (needs
// administrator rights: the drive is opened for reading and writing to send commands).
void ata_smart(DriveHealth& d) {
    const std::wstring device = L"\\\\.\\PhysicalDrive" + std::to_wstring(d.disk);
    const HANDLE h = CreateFileW(device.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    std::uint8_t values[512];
    std::uint8_t thresholds[512];
    const auto try_all = [&](auto&& read) {
        if (!read(kReadData, values)) return false;
        std::vector<SmartAttribute> table = parse_smart(values, read(kReadThresholds, thresholds) ? thresholds : nullptr);
        if (table.empty()) return false;
        d.smart = std::move(table);
        return true;
    };
    const auto rcv = [&](std::uint8_t f, std::uint8_t* o) { return smart_rcv(h, f, o); };
    const auto ata = [&](std::uint8_t f, std::uint8_t* o) { return ata_pass_through(h, f, o); };
    const auto sat12 = [&](std::uint8_t f, std::uint8_t* o) { return sat_pass_through(h, f, o, false); };
    const auto sat16 = [&](std::uint8_t f, std::uint8_t* o) { return sat_pass_through(h, f, o, true); };
    // A USB enclosure is asked in its own language first; other drives through their driver.
    const bool usb = d.bus == "USB";
    bool ok = !usb && (try_all(rcv) || try_all(ata));
    if (!ok) ok = try_all(sat12) || try_all(sat16);
    if (!ok && usb) try_all(ata);
    CloseHandle(h);
    apply_smart(d);
}

std::vector<DriveHealth> query(const std::wstring& where) {
    std::vector<DriveHealth> out;
    try {
        const ComScope com;
        ComPtr<IWbemLocator> locator;
        if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator,
                                    reinterpret_cast<void**>(locator.put()))))
            return out;
        ComPtr<IWbemServices> services;
        const Bstr ns(L"ROOT\\Microsoft\\Windows\\Storage");
        if (FAILED(locator->ConnectServer(ns.b, nullptr, nullptr, nullptr, 0, nullptr, nullptr, services.put())))
            return out;
        CoSetProxyBlanket(services.p, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
                          RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
        const Bstr wql(L"WQL");
        const auto run = [&](const std::wstring& q, auto&& each) {
            ComPtr<IEnumWbemClassObject> rows;
            const Bstr text_q(q.c_str());
            if (FAILED(services->ExecQuery(wql.b, text_q.b, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                                           nullptr, rows.put())))
                return;
            for (;;) {
                ComPtr<IWbemClassObject> row;
                ULONG got = 0;
                if (FAILED(rows->Next(10000, 1, row.put(), &got)) || got == 0) break;
                each(row.p);
            }
        };

        std::map<int, std::size_t> by_disk;
        run(L"SELECT DeviceId, FriendlyName, SerialNumber, BusType, MediaType, HealthStatus, OperationalStatus, Size "
            L"FROM MSFT_PhysicalDisk" + where,
            [&](IWbemClassObject* row) {
                DriveHealth d;
                d.disk = std::atoi(text(row, L"DeviceId").c_str());
                d.model = text(row, L"FriendlyName");
                d.serial = text(row, L"SerialNumber");
                d.bus = bus_name(number(row, L"BusType").value_or(0));
                d.media = media_name(number(row, L"MediaType").value_or(0));
                d.size = number(row, L"Size").value_or(0);
                switch (number(row, L"HealthStatus").value_or(5)) {
                    case 0: d.status = DriveHealth::Status::Healthy; break;
                    case 1: d.status = DriveHealth::Status::Warning; break;
                    case 2: d.status = DriveHealth::Status::Unhealthy; break;
                    default: d.status = DriveHealth::Status::Unknown; break;
                }
                d.condition = condition_of(numbers(row, L"OperationalStatus"));
                by_disk[d.disk] = out.size();
                out.push_back(std::move(d));
            });
        if (out.empty()) return out;
        // S.M.A.R.T. details: only administrators may read them; without the rights the query
        // simply finds nothing.
        run(L"SELECT DeviceId, Temperature, Wear, PowerOnHours, ReadErrorsUncorrected FROM MSFT_StorageReliabilityCounter",
            [&](IWbemClassObject* row) {
                const auto it = by_disk.find(std::atoi(text(row, L"DeviceId").c_str()));
                if (it == by_disk.end()) return;
                DriveHealth& d = out[it->second];
                if (const auto t = number(row, L"Temperature"); t && *t > 0) d.temperature = static_cast<int>(*t);
                if (const auto w = number(row, L"Wear"); w && d.media == "SSD") d.wear = static_cast<int>(*w);
                if (const auto h = number(row, L"PowerOnHours"); h && *h > 0) d.power_on_hours = *h;
                if (const auto e = number(row, L"ReadErrorsUncorrected")) d.read_errors_uncorrected = *e;
            });
        const bool elevated = process_elevated();
        for (DriveHealth& d : out) {
            if (d.bus == "NVMe" || d.media == "SSD") nvme_health(d);
            if (elevated && d.bus != "NVMe") ata_smart(d);
        }
    } catch (...) {
        // best effort
    }
    return out;
}

// The disk numbers a volume lies on.
std::vector<int> disks_of(const std::string& path) {
    std::vector<int> disks;
    const VolumeLocation loc = volume_of(path);
    if (loc.volume.empty()) return disks;
    std::wstring device = widen(loc.volume);
    if (!device.empty() && device.back() == L'\\') device.pop_back();
    const HANDLE h = CreateFileW(device.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return disks;
    std::vector<std::uint8_t> buffer(sizeof(VOLUME_DISK_EXTENTS) + 7 * sizeof(DISK_EXTENT));
    DWORD got = 0;
    if (DeviceIoControl(h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, buffer.data(),
                        static_cast<DWORD>(buffer.size()), &got, nullptr)) {
        const auto* extents = reinterpret_cast<const VOLUME_DISK_EXTENTS*>(buffer.data());
        for (DWORD i = 0; i < extents->NumberOfDiskExtents && i < 8; ++i) {
            const int n = static_cast<int>(extents->Extents[i].DiskNumber);
            if (std::find(disks.begin(), disks.end(), n) == disks.end()) disks.push_back(n);
        }
    }
    CloseHandle(h);
    return disks;
}

}  // namespace

std::vector<DriveHealth> drive_health(const std::string& path) {
    const std::vector<int> disks = disks_of(path);
    if (disks.empty()) return {};
    std::wstring where = L" WHERE ";
    for (std::size_t i = 0; i < disks.size(); ++i)
        where += (i ? L" OR " : L"") + std::wstring(L"DeviceId='") + std::to_wstring(disks[i]) + L"'";
    return query(where);
}

std::vector<int> disk_numbers(const std::string& path) {
    return disks_of(path);
}

std::vector<DriveHealth> all_drives() {
    std::vector<DriveHealth> all = query(L"");
    std::sort(all.begin(), all.end(), [](const DriveHealth& a, const DriveHealth& b) { return a.disk < b.disk; });
    return all;
}

const char* to_string(DriveHealth::Status status) {
    switch (status) {
        case DriveHealth::Status::Healthy: return "healthy";
        case DriveHealth::Status::Warning: return "warning";
        case DriveHealth::Status::Unhealthy: return "unhealthy";
        case DriveHealth::Status::Unknown: break;
    }
    return "unknown";
}

}  // namespace vaultik::os
