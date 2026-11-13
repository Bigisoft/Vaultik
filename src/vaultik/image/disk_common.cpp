// The parts of disk.hpp that do not talk to the system.

#include <algorithm>
#include <cctype>
#include <cstdio>

#include "vaultik/image/disk.hpp"

namespace vaultik::image {

namespace {

std::string trim_upper(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    std::string out;
    bool space = false;
    for (std::size_t i = b; i < e; ++i) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (std::isspace(c)) {
            space = true;
            continue;
        }
        if (space) out.push_back(' ');
        space = false;
        out.push_back(static_cast<char>(std::toupper(c)));
    }
    return out;
}

}  // namespace

std::string describe(const DiskInfo& d) {
    char size[32];
    const double gb = static_cast<double>(d.size) / 1e9;
    std::snprintf(size, sizeof size, gb >= 10 ? "%.0f GB" : "%.1f GB", gb);
    std::string s = "Disk " + std::to_string(d.number) + ": " + (d.model.empty() ? std::string("(no name)") : d.model) +
                    ", " + size;
    if (!d.bus.empty()) s += ", " + d.bus;
    if (!d.serial.empty()) s += ", serial " + d.serial;
    if (d.system) s += " (Windows runs from it)";
    return s;
}

std::string target_refusal(const DiskInfo& target, const std::vector<int>& running_from,
                           const std::vector<int>& protected_disks) {
    if (target.number < 0) return "no such disk";
    if (std::find(running_from.begin(), running_from.end(), kUnknownSystemDisk) != running_from.end())
        return "the disks Windows runs and starts from could not be found, so no disk is written to";
    if (target.system || std::find(running_from.begin(), running_from.end(), target.number) != running_from.end())
        return "Windows is running from this disk. Restore it from the recovery drive instead.";
    if (std::find(protected_disks.begin(), protected_disks.end(), target.number) != protected_disks.end())
        return "this disk holds the backup being restored (or Vaultik itself)";
    if (target.size == 0) return "the disk reports no size (is it connected?)";
    return {};
}

std::string confirmation_text(int number) {
    return "ERASE DISK " + std::to_string(number);
}

bool confirmed(const std::string& typed, int number) {
    return trim_upper(typed) == confirmation_text(number);
}

int find_disk(const std::vector<DiskInfo>& disks, const std::string& id) {
    if (id.empty()) return -1;
    if (id == "system") {
        for (const DiskInfo& d : disks)
            if (d.system) return d.number;
        return -1;
    }
    std::string want = id;
    std::transform(want.begin(), want.end(), want.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const DiskInfo& d : disks) {
        std::string have = d.id;
        std::transform(have.begin(), have.end(), have.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!have.empty() && have == want) return d.number;
    }
    return -1;
}

DiskIdentity identity_of(const DiskInfo& d) {
    return {d.serial, d.size, d.id};
}

std::string identity_mismatch(const DiskInfo& now, const DiskIdentity& expected) {
    const std::string number = "disk " + std::to_string(now.number);
    if (now.size != expected.size)
        return number + " is not the disk that was chosen any more (its size differs); list the disks again";
    if (trim_upper(now.serial) != trim_upper(expected.serial))
        return number + " is not the disk that was chosen any more (its serial number differs); list the disks again";
    if (trim_upper(now.id) != trim_upper(expected.id))
        return number + " is not the disk that was chosen any more (its partition table changed); list the disks again";
    return {};
}

VhdxTarget::VhdxTarget(std::unique_ptr<RandomFile> file, std::uint64_t size, std::uint32_t sector_size,
                       std::uint32_t physical_sector_size)
    : file_(std::move(file)),
      sector_(sector_size),
      writer_(*file_, size, sector_size, physical_sector_size == 512 ? 512 : 4096) {}

}  // namespace vaultik::image
