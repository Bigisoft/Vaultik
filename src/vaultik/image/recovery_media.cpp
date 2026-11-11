#include "vaultik/image/recovery_media.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>

#include "vaultik/platform/acl.hpp"
#include "vaultik/platform/drive_health.hpp"
#include "vaultik/platform/os.hpp"

#ifdef _WIN32
#include "vaultik/util/win.hpp"
#include "vaultik/util/win_handle.hpp"
#endif

namespace vaultik::image {

namespace {

std::uint32_t le32(ByteSpan b, std::size_t at) {
    if (at + 4 > b.size()) throw Error("short");
    return b[at] | (b[at + 1] << 8) | (b[at + 2] << 16) | (static_cast<std::uint32_t>(b[at + 3]) << 24);
}
std::uint16_t le16(ByteSpan b, std::size_t at) {
    if (at + 2 > b.size()) throw Error("short");
    return static_cast<std::uint16_t>(b[at] | (b[at + 1] << 8));
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

std::vector<std::string> pe_imports(ByteSpan exe, bool delayed) {
    std::vector<std::string> out;
    try {
        if (exe.size() < 0x40 || exe[0] != 'M' || exe[1] != 'Z') return out;
        const std::size_t pe = le32(exe, 0x3C);
        if (le32(exe, pe) != 0x00004550) return out;  // "PE\0\0"
        const std::size_t coff = pe + 4;
        const std::uint16_t sections = le16(exe, coff + 2);
        const std::uint16_t opt_size = le16(exe, coff + 16);
        const std::size_t opt = coff + 20;
        if (le16(exe, opt) != 0x20B) return out;  // PE32+ only
        const std::size_t dirs = opt + 112;
        const std::uint32_t entry = delayed ? 13 : 1;
        if (le32(exe, opt + 108) <= entry) return out;
        const std::uint32_t table = le32(exe, dirs + entry * 8);
        if (table == 0) return out;
        const std::size_t sec = opt + opt_size;
        auto file_offset = [&](std::uint32_t rva) -> std::size_t {
            for (std::uint16_t i = 0; i < sections; ++i) {
                const std::size_t s = sec + i * 40u;
                const std::uint32_t va = le32(exe, s + 12);
                const std::uint32_t vsize = std::max(le32(exe, s + 8), le32(exe, s + 16));
                const std::uint32_t raw = le32(exe, s + 20);
                if (rva >= va && rva < va + vsize) return raw + (rva - va);
            }
            throw Error("rva outside the sections");
        };
        const std::size_t step = delayed ? 32 : 20;
        const std::size_t name_at = delayed ? 4 : 12;
        for (std::size_t at = file_offset(table); at + step <= exe.size(); at += step) {
            const std::uint32_t name_rva = le32(exe, at + name_at);
            if (name_rva == 0) break;
            std::size_t p = file_offset(name_rva);
            std::string name;
            while (p < exe.size() && exe[p] != 0 && name.size() < 260) name.push_back(static_cast<char>(exe[p++]));
            out.push_back(name);
        }
    } catch (const Error&) {
        return {};
    }
    return out;
}

std::string startnet_script() {
    return "@echo off\r\n"
           "rem Vaultik recovery drive: starts Windows PE, then the restore menu.\r\n"
           "wpeinit\r\n"
           "title Vaultik recovery\r\n"
           "X:\\Vaultik\\vaultik.exe recovery\r\n"
           "echo.\r\n"
           "echo Type X:\\Vaultik\\vaultik.exe recovery to open the restore menu again,\r\n"
           "echo or wpeutil reboot to restart the computer.\r\n";
}

std::string adk_help() {
    return "A recovery drive is built from Windows PE, which comes with Microsoft's free Windows ADK. "
           "Install the \"Windows ADK\" and its \"Windows PE add-on\" (from "
           "https://learn.microsoft.com/windows-hardware/get-started/adk-install, or with "
           "\"winget install Microsoft.WindowsADK\" and \"winget install Microsoft.ADKPEAddon\"), then try again.";
}

#ifdef _WIN32

namespace {

std::string kits_root() {
    wchar_t buf[MAX_PATH + 1] = {};
    DWORD size = sizeof buf;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots", L"KitsRoot10",
                     RRF_RT_REG_SZ | RRF_SUBKEY_WOW6432KEY, nullptr, buf, &size) == ERROR_SUCCESS)
        return narrow(buf);
    wchar_t pf[MAX_PATH + 1] = {};
    const DWORD n = GetEnvironmentVariableW(L"ProgramFiles(x86)", pf, MAX_PATH);
    if (n == 0 || n > MAX_PATH) return {};
    return narrow(pf) + "\\Windows Kits\\10\\";
}

std::string with_slash(std::string s) {
    if (!s.empty() && s.back() != '\\') s.push_back('\\');
    return s;
}

void run(const std::vector<std::string>& argv, const std::string& what) {
    std::string output;
    const int code = os::run_command(argv, &output);
    if (code != 0) {
        while (!output.empty() && (output.back() == '\n' || output.back() == '\r' || output.back() == ' ')) output.pop_back();
        if (output.size() > 2000) output = "..." + output.substr(output.size() - 2000);
        throw Error(what + " failed (exit code " + std::to_string(code) + ")" + (output.empty() ? "" : ":\n" + output));
    }
}

// The folder the drive is built in: <ProgramData>\Vaultik\winpe, which only administrators can
// change. The build runs elevated and hands files from there to DISM and a script to diskpart; in
// the user's temporary folder a program running as the user could swap them (a diskpart script
// that cleans another disk, or files of the recovery drive).
std::string work_root() {
    const std::string vaultik = os::join(os::program_data_dir(), "Vaultik");
    os::make_admin_dir(vaultik);
    const std::string root = os::join(vaultik, "winpe");
    os::make_admin_dir(root);
    return root;
}

}  // namespace

AdkInfo find_adk() {
    AdkInfo a;
    const std::string root = with_slash(kits_root());
    a.kit = root + "Assessment and Deployment Kit\\";
    a.winpe = a.kit + "Windows Preinstallation Environment\\amd64\\";
    a.oscdimg = a.kit + "Deployment Tools\\amd64\\Oscdimg\\";
    a.bootsect = a.kit + "Deployment Tools\\amd64\\BCDBoot\\bootsect.exe";
    if (root.size() <= 1 || !os::exists(a.kit + "Deployment Tools")) a.missing = "the Windows ADK is not installed";
    else if (!os::exists(a.winpe + "en-us\\winpe.wim")) a.missing = "the Windows PE add-on of the Windows ADK is not installed";
    else if (!os::exists(a.oscdimg + "oscdimg.exe")) a.missing = "the Deployment Tools of the Windows ADK are not installed";
    a.found = a.missing.empty();
    return a;
}

std::string usb_refusal(const std::string& drive, const std::vector<std::string>& backup_locations) {
    if (drive.size() < 2 || drive[1] != ':' || !std::isalpha(static_cast<unsigned char>(drive[0])))
        return "give a drive letter, such as E:";
    const std::string root = std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(drive[0])))) + ":\\";
    const UINT type = GetDriveTypeW(widen(root).c_str());
    if (type == DRIVE_NO_ROOT_DIR) return root + " is not connected";
    const os::VolumeLocation loc = os::volume_of(root);
    if (loc.system) return "Windows runs from " + root;
    if (type != DRIVE_REMOVABLE && !loc.external) return root + " is not a USB stick or other removable drive";
    for (const std::string& b : backup_locations) {
        if (b.size() >= 2 && std::toupper(static_cast<unsigned char>(b[0])) == root[0] && b[1] == ':')
            return root + " holds a backup (" + b + "); use another USB stick";
    }
    return {};
}

void make_recovery_media(const RecoveryMediaOptions& o) {
    if (o.iso.empty() == o.usb.empty()) throw Error("give either an ISO file or a USB drive");
    const AdkInfo adk = find_adk();
    if (!adk.found) throw Error(adk.missing + ". " + adk_help());
    const std::string program = o.program.empty() ? os::executable_path() : o.program;
    auto step = [&](const std::string& s) {
        if (o.on_step) o.on_step(s);
    };

    namespace fs = std::filesystem;
    const fs::path work = fs::path(widen(work_root())) / (std::to_wstring(GetCurrentProcessId()) + L"-" +
                                                         std::to_wstring(GetTickCount64()));
    const fs::path media = work / L"media";
    const fs::path mount = work / L"mount";
    bool mounted = false;
    struct Cleanup {
        const fs::path& work;
        const fs::path& mount;
        bool& mounted;
        ~Cleanup() {
            if (mounted) {
                std::string ignored;
                try {
                    os::run_command({"dism.exe", "/Unmount-Image", "/MountDir:" + narrow(mount.wstring()), "/Discard"}, &ignored);
                } catch (...) {
                }
            }
            std::error_code ec;
            fs::remove_all(work, ec);
        }
    } cleanup{work, mount, mounted};

    step("Copying Windows PE from the ADK...");
    fs::create_directories(media / L"sources");
    fs::create_directories(mount);
    fs::copy(fs::path(widen(adk.winpe + "Media")), media, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
    fs::copy_file(fs::path(widen(adk.winpe + "en-us\\winpe.wim")), media / L"sources" / L"boot.wim",
                  fs::copy_options::overwrite_existing);
    const fs::path wim = media / L"sources" / L"boot.wim";
    // The copy keeps the read-only flag of the ADK's file; DISM needs to write it.
    SetFileAttributesW(wim.c_str(), FILE_ATTRIBUTE_NORMAL);

    step("Opening the Windows PE image (DISM)...");
    run({"dism.exe", "/Mount-Image", "/ImageFile:" + narrow(wim.wstring()), "/Index:1", "/MountDir:" + narrow(mount.wstring())},
        "DISM (opening the Windows PE image)");
    mounted = true;

    step("Adding Vaultik...");
    fs::create_directories(mount / L"Vaultik");
    fs::copy_file(fs::path(widen(program)), mount / L"Vaultik" / L"vaultik.exe", fs::copy_options::overwrite_existing);
    const std::string script = startnet_script();
    os::write_file_atomic(narrow((mount / L"Windows" / L"System32" / L"startnet.cmd").wstring()), as_bytes(script));

    // Every library vaultik.exe needs at start must be in Windows PE, or it would not start there.
    const Bytes exe = os::read_file(program, 256u << 20);
    const fs::path sys32 = mount / L"Windows" / L"System32";
    std::vector<std::string> missing;
    for (const std::string& dll : pe_imports(exe, false)) {
        const std::string l = lower(dll);
        if (l.starts_with("api-ms-") || l.starts_with("ext-ms-")) continue;
        if (!fs::exists(sys32 / widen(dll))) missing.push_back(dll);
    }
    if (!missing.empty()) {
        std::string list;
        for (const std::string& m : missing) list += (list.empty() ? "" : ", ") + m;
        throw Error("Windows PE lacks libraries Vaultik needs: " + list);
    }
    for (const std::string& dll : pe_imports(exe, true))
        if (!fs::exists(sys32 / widen(dll)) && o.on_warning)
            o.on_warning(dll + " is not in Windows PE; what needs it (such as cloud storage or browsing) will not work there");

    step("Saving the Windows PE image (DISM)...");
    run({"dism.exe", "/Unmount-Image", "/MountDir:" + narrow(mount.wstring()), "/Commit"}, "DISM (saving the Windows PE image)");
    mounted = false;

    if (!o.iso.empty()) {
        step("Writing " + o.iso + "...");
        const std::string etfs = adk.oscdimg + "etfsboot.com";
        const std::string efi = adk.oscdimg + "efisys.bin";
        std::error_code ec;
        fs::remove(fs::path(widen(o.iso)), ec);
        run({adk.oscdimg + "oscdimg.exe", "-bootdata:2#p0,e,b" + etfs + "#pEF,e,b" + efi, "-u1", "-udfver102",
             narrow(media.wstring()), os::absolute_path(o.iso)},
            "Writing the ISO file (oscdimg)");
        return;
    }

    const std::string drive = std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(o.usb[0])))) + ":";
    const std::string why = usb_refusal(drive, {});
    if (!why.empty()) throw Error(why);
    step("Formatting " + drive + " (FAT32)...");
    const fs::path script_file = work / L"format.txt";
    const std::string letter(1, drive[0]);
    os::write_file_atomic(narrow(script_file.wstring()),
                          as_bytes("select volume=" + letter + "\r\nformat fs=fat32 label=\"VAULTIK\" quick\r\n"));
    try {
        run({"diskpart.exe", "/s", narrow(script_file.wstring())}, "Formatting " + drive);
    } catch (const Error& e) {
        throw Error(std::string(e.what()) +
                    "\nFAT32 works on drives up to 32 GB; use a smaller USB stick, or make an ISO file instead.");
    }
    // Marking the partition active (for older computers that start in BIOS mode) fails on some
    // sticks; UEFI computers do not need it.
    os::write_file_atomic(narrow(script_file.wstring()), as_bytes("select volume=" + letter + "\r\nactive\r\n"));
    try {
        run({"diskpart.exe", "/s", narrow(script_file.wstring())}, "Marking " + drive + " as startable");
        run({adk.bootsect, "/nt60", drive, "/force", "/mbr"}, "Writing the boot code (bootsect)");
    } catch (const Error& e) {
        if (o.on_warning) o.on_warning(std::string(e.what()) + " (computers that start in UEFI mode do not need it)");
    }
    step("Copying the files to " + drive + "...");
    fs::copy(media, fs::path(widen(drive + "\\")), fs::copy_options::recursive | fs::copy_options::overwrite_existing);
}

#else

AdkInfo find_adk() {
    AdkInfo a;
    a.missing = "recovery drives are made on Windows";
    return a;
}

std::string usb_refusal(const std::string&, const std::vector<std::string>&) {
    return "recovery drives are made on Windows";
}

void make_recovery_media(const RecoveryMediaOptions&) {
    throw Error("recovery drives are made on Windows");
}

#endif

}  // namespace vaultik::image
