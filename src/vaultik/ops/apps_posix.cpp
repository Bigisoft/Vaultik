#include "vaultik/ops/apps.hpp"

#include "vaultik/platform/os.hpp"

namespace vaultik {

// Linux: the Flatpak applications (the usual way to install programs on SteamOS, whose system
// packages are part of the read-only image), so the list shows what to install again. Programs of
// the distribution's own package manager are not listed. Empty when Flatpak is not there.
std::vector<UninstallEntry> read_uninstall_entries() {
    std::vector<UninstallEntry> out;
    std::string text;
    try {
        os::CommandOptions options;
        options.output = &text;
        options.timeout = std::chrono::seconds(60);
        std::vector<std::string> argv = {"flatpak", "list", "--app", "--columns=application,name,version"};
        // Inside Vaultik's own Flatpak, the host's list.
        if (!os::get_env("FLATPAK_ID").empty()) argv.insert(argv.begin(), {"flatpak-spawn", "--host"});
        if (os::run_command(argv, options) != 0) return out;
    } catch (const Error&) {
        return out;
    }
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        // "org.mozilla.firefox\tFirefox\t133.0"
        const std::size_t t1 = line.find('\t');
        if (t1 == std::string::npos) continue;
        const std::size_t t2 = line.find('\t', t1 + 1);
        UninstallEntry e;
        e.key_name = line.substr(0, t1);
        e.display_name = line.substr(t1 + 1, (t2 == std::string::npos ? line.size() : t2) - t1 - 1);
        if (t2 != std::string::npos) e.display_version = line.substr(t2 + 1);
        if (e.display_name.empty()) e.display_name = e.key_name;
        e.publisher = "Flatpak " + e.key_name;
        out.push_back(std::move(e));
    }
    return out;
}

std::string winget_program() {
    return {};
}

}  // namespace vaultik
