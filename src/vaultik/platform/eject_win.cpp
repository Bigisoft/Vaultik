// Windows: "Safely remove" for a backup drive, without administrator rights where Windows allows
// it: the volume is flushed, locked and dismounted when this account may (otherwise left to the
// eject request), then the device is asked to eject (CM_Request_Device_Eject), which Windows
// refuses, naming the reason, while a program still uses the drive.

#include <windows.h>
#include <cfgmgr32.h>
#include <setupapi.h>
#include <winioctl.h>

#include <cwctype>
#include <vector>

#include "vaultik/platform/os.hpp"
#include "vaultik/util/win.hpp"
#include "vaultik/util/win_handle.hpp"

namespace vaultik::os {

namespace {

// GUID_DEVINTERFACE_DISK, without initguid.h.
constexpr GUID kDiskInterface = {0x53f56307, 0xb6bf, 0x11d0, {0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b}};

bool device_number(HANDLE h, STORAGE_DEVICE_NUMBER& n) {
    DWORD got = 0;
    return DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, &n, sizeof n, &got, nullptr) != 0;
}

// The device instance of the disk with this number; 0 when not found.
DEVINST disk_instance(const STORAGE_DEVICE_NUMBER& wanted) {
    HDEVINFO set = SetupDiGetClassDevsW(&kDiskInterface, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return 0;
    DEVINST found = 0;
    SP_DEVICE_INTERFACE_DATA iface{};
    iface.cbSize = sizeof iface;
    for (DWORD i = 0; found == 0 && SetupDiEnumDeviceInterfaces(set, nullptr, &kDiskInterface, i, &iface); ++i) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &iface, nullptr, 0, &need, nullptr);
        if (need == 0) continue;
        std::vector<std::uint8_t> buf(need);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA info{};
        info.cbSize = sizeof info;
        if (!SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, need, nullptr, &info)) continue;
        UniqueHandle disk(CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                                      nullptr));
        if (!disk.valid()) continue;
        STORAGE_DEVICE_NUMBER n{};
        if (device_number(disk.get(), n) && n.DeviceNumber == wanted.DeviceNumber && n.DeviceType == wanted.DeviceType)
            found = info.DevInst;
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

}  // namespace

std::string eject_drive(const std::string& root) {
    if (root.size() < 2 || root[1] != ':') return "only a drive with a letter can be ejected";
    wchar_t windows[MAX_PATH] = {};
    if (GetSystemWindowsDirectoryW(windows, MAX_PATH) > 0 && towupper(windows[0]) == towupper(static_cast<wchar_t>(root[0])))
        return "the drive Windows runs from cannot be ejected";
    const std::wstring device = L"\\\\.\\" + widen(root.substr(0, 2));
    STORAGE_DEVICE_NUMBER number{};
    {
        // With write access the volume is flushed, locked and dismounted first; without it (not
        // allowed for this account) the eject request below does the same, or refuses.
        UniqueHandle volume(CreateFileW(device.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                        nullptr, OPEN_EXISTING, 0, nullptr));
        if (volume.valid()) {
            FlushFileBuffers(volume.get());
            DWORD got = 0;
            if (DeviceIoControl(volume.get(), FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &got, nullptr))
                DeviceIoControl(volume.get(), FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0, &got, nullptr);
        } else {
            volume.reset(CreateFileW(device.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                                     nullptr));
        }
        if (!volume.valid()) return "the drive " + root + " cannot be opened";
        if (!device_number(volume.get(), number)) return "Windows does not say which device " + root + " is on";
    }
    DEVINST inst = disk_instance(number);
    if (inst == 0) return "the device of " + root + " was not found";
    // The disk, or the device above it (a USB adapter) that can be removed.
    for (int level = 0; level < 3 && inst != 0; ++level) {
        PNP_VETO_TYPE veto = PNP_VetoTypeUnknown;
        wchar_t name[MAX_PATH] = {};
        const CONFIGRET cr = CM_Request_Device_EjectW(inst, &veto, name, MAX_PATH, 0);
        if (cr == CR_SUCCESS && veto == PNP_VetoTypeUnknown) return {};
        if (veto != PNP_VetoTypeUnknown) {
            const std::string by = narrow(name);
            return "Windows cannot eject " + root + " now: a program still uses it" +
                   (by.empty() ? std::string() : " (" + by + ")") + "; close it and try again";
        }
        DEVINST parent = 0;
        if (CM_Get_Parent(&parent, inst, 0) != CR_SUCCESS) break;
        inst = parent;
    }
    return "Windows cannot eject " + root + " (it may not be a removable drive)";
}

}  // namespace vaultik::os
