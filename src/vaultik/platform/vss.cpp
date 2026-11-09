#include "vaultik/platform/vss.hpp"

#include <algorithm>
#include <cstdio>

#include "vaultik/util/error.hpp"

#ifdef _WIN32

#include "vaultik/util/win.hpp"
#include "vaultik/util/win_handle.hpp"

// After <windows.h>.
#include <objbase.h>
#include <vss.h>
#include <vswriter.h>
#include <vsbackup.h>

namespace vaultik::os {

namespace {

std::string hresult_text(HRESULT hr) {
    switch (hr) {
        case E_ACCESSDENIED:
            return "needs administrator rights (run Vaultik elevated)";
        case VSS_E_BAD_STATE:
            return "the shadow copy service is in an unexpected state";
        case VSS_E_VOLUME_NOT_SUPPORTED:
            return "the volume does not support shadow copies";
        case VSS_E_SNAPSHOT_SET_IN_PROGRESS:
            return "another shadow copy is being created right now";
        case VSS_E_MAXIMUM_NUMBER_OF_SNAPSHOTS_REACHED:
        case VSS_E_MAXIMUM_NUMBER_OF_VOLUMES_REACHED:
            return "too many shadow copies exist already";
        case VSS_E_INSUFFICIENT_STORAGE:
            return "not enough free space for a shadow copy";
        case VSS_E_FLUSH_WRITES_TIMEOUT:
        case VSS_E_HOLD_WRITES_TIMEOUT:
            return "the disk was too busy to freeze in time";
        case VSS_E_UNEXPECTED_PROVIDER_ERROR:
        case VSS_E_PROVIDER_VETO:
            return "the shadow copy provider refused";
        default: {
            char buf[32];
            std::snprintf(buf, sizeof buf, "error 0x%08lX", static_cast<unsigned long>(hr));
            return buf;
        }
    }
}

void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) throw Error(std::string("shadow copy: ") + what + " failed: " + hresult_text(hr));
}

// Waits for an asynchronous VSS operation and checks its outcome.
void wait(IVssAsync* async, const char* what) {
    HRESULT hr = async->Wait();
    HRESULT status = S_OK;
    if (SUCCEEDED(hr)) hr = async->QueryStatus(&status, nullptr);
    async->Release();
    check(hr, what);
    check(status, what);
}

std::string upper(std::string s) {
    for (char& c : s)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return s;
}

}  // namespace

struct ShadowCopy::Impl {
    bool com = false;
    IVssBackupComponents* backup = nullptr;
    VSS_ID set{};
    bool have_set = false;

    ~Impl() {
        if (backup != nullptr) {
            if (have_set) {
                IVssAsync* async = nullptr;
                if (SUCCEEDED(backup->BackupComplete(&async)) && async != nullptr) {
                    async->Wait();
                    async->Release();
                }
                LONG deleted = 0;
                VSS_ID failed{};
                backup->DeleteSnapshots(set, VSS_OBJECT_SNAPSHOT_SET, TRUE, &deleted, &failed);
            }
            backup->Release();
        }
        if (com) CoUninitialize();
    }
};

std::unique_ptr<ShadowCopy> ShadowCopy::create(const std::vector<std::string>& paths, bool pause_sql_server) {
    std::unique_ptr<ShadowCopy> sc(new ShadowCopy());
    sc->impl_ = std::make_unique<Impl>();
    Impl& im = *sc->impl_;

    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(init)) im.com = true;
    else if (init != RPC_E_CHANGED_MODE) check(init, "starting COM");
    // Writers call back into this process; VSS needs this security level for that. Fails harmlessly
    // when the process already set it.
    CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_PKT_PRIVACY, RPC_C_IMP_LEVEL_IDENTIFY,
                         nullptr, EOAC_DYNAMIC_CLOAKING, nullptr);

    // The local volumes the paths are on.
    std::vector<std::pair<std::string, std::wstring>> volumes;  // mount point -> volume name
    for (const std::string& p : paths) {
        const std::wstring wp = widen(p);
        wchar_t mount[MAX_PATH + 1];
        if (!GetVolumePathNameW(wp.c_str(), mount, MAX_PATH)) continue;
        const std::string mount_u8 = upper(narrow(mount));
        if (std::any_of(volumes.begin(), volumes.end(), [&](const auto& v) { return v.first == mount_u8; })) continue;
        const UINT type = GetDriveTypeW(mount);
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) {
            sc->skipped_.push_back(narrow(mount) + " (network or other non-local drive)");
            continue;
        }
        wchar_t name[64];
        if (!GetVolumeNameForVolumeMountPointW(mount, name, 64)) {
            sc->skipped_.push_back(narrow(mount) + " (" + win32_message(GetLastError()) + ")");
            continue;
        }
        volumes.emplace_back(mount_u8, name);
    }
    if (volumes.empty()) return sc;  // nothing to freeze; every path is read as it is

    check(CreateVssBackupComponents(&im.backup), "starting a shadow copy");
    check(im.backup->InitializeForBackup(nullptr), "initializing");
    check(im.backup->SetContext(VSS_CTX_BACKUP), "setting the context");
    // A copy backup: application writers see it as an extra copy and do not truncate their logs.
    check(im.backup->SetBackupState(false, false, VSS_BT_COPY, false), "setting the backup type");
    if (!pause_sql_server) {
        // Must come before GatherWriterMetadata. SqlServerWriter, and the old MSDE writer.
        static const VSS_ID sql_writers[] = {
            {0xa65faa63, 0x5ea8, 0x4ebc, {0x9d, 0xbd, 0xa0, 0xc4, 0xdb, 0x26, 0x91, 0x2a}},
            {0xf8544ac1, 0x0611, 0x4fa5, {0xb0, 0x4b, 0xf7, 0xee, 0x00, 0xb0, 0x32, 0x77}},
        };
        check(im.backup->DisableWriterClasses(sql_writers, 2), "leaving SQL Server out");
    }
    IVssAsync* async = nullptr;
    check(im.backup->GatherWriterMetadata(&async), "asking applications to prepare");
    wait(async, "asking applications to prepare");
    check(im.backup->StartSnapshotSet(&im.set), "starting the snapshot");
    im.have_set = true;

    std::vector<std::pair<std::string, VSS_ID>> added;
    for (auto& [mount, name] : volumes) {
        VSS_ID id{};
        const HRESULT hr = im.backup->AddToSnapshotSet(name.data(), GUID_NULL, &id);
        if (FAILED(hr)) {
            sc->skipped_.push_back(mount + " (" + hresult_text(hr) + ")");
            continue;
        }
        added.emplace_back(mount, id);
    }
    if (added.empty()) return sc;

    check(im.backup->PrepareForBackup(&async), "preparing");
    wait(async, "preparing");
    check(im.backup->DoSnapshotSet(&async), "creating the shadow copy");
    wait(async, "creating the shadow copy");

    for (const auto& [mount, id] : added) {
        VSS_SNAPSHOT_PROP prop{};
        check(im.backup->GetSnapshotProperties(id, &prop), "reading the shadow copy");
        std::string device = narrow(prop.m_pwszSnapshotDeviceObject);
        VssFreeSnapshotProperties(&prop);
        if (!device.ends_with('\\')) device.push_back('\\');
        // "\\?\GLOBALROOT\Device\HarddiskVolumeShadowCopyN\": already a long path, used as is.
        if (!device.starts_with("\\\\?\\")) device = "\\\\?\\GLOBALROOT" + device;
        sc->map_.emplace_back(mount, device);
        sc->volumes_.push_back(mount);
    }
    // Longest mount point first, so C:\mnt\data\ wins over C:\.
    std::sort(sc->map_.begin(), sc->map_.end(),
              [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
    return sc;
}

ShadowCopy::~ShadowCopy() = default;

std::string ShadowCopy::map(const std::string& path) const {
    const std::string up = upper(path);
    for (const auto& [mount, device] : map_) {
        if (up.starts_with(mount)) return device + path.substr(mount.size());
        // The mount point itself, without its trailing backslash ("C:" for "C:\").
        if (up.size() + 1 == mount.size() && mount.starts_with(up)) return device;
    }
    return path;
}

}  // namespace vaultik::os

#else

namespace vaultik::os {

struct ShadowCopy::Impl {};

std::unique_ptr<ShadowCopy> ShadowCopy::create(const std::vector<std::string>& /*paths*/, bool /*pause_sql_server*/) {
    throw Error("shadow copies are only available on Windows");
}

ShadowCopy::~ShadowCopy() = default;

std::string ShadowCopy::map(const std::string& path) const {
    return path;
}

}  // namespace vaultik::os

#endif
