#include "vaultik/jobs/check_state.hpp"

#include <cctype>

#include <nlohmann/json.hpp>

#include "vaultik/platform/os.hpp"
#include "vaultik/util/hex.hpp"

namespace vaultik::jobs {

namespace {

std::string state_path(const std::string& dir, const std::string& key) {
    for (const char c : key)
        if (!std::isxdigit(static_cast<unsigned char>(c))) throw Error("invalid check state name");
    return os::join(os::join(dir, "checks"), key + ".json");
}

}  // namespace

std::string check_state_key(const Repository& repo) {
    const auto& id = repo.config().repo_id;
    return to_hex(ByteSpan(id.data(), id.size()));
}

PackTimes load_check_state(const std::string& dir, const std::string& key) {
    PackTimes out;
    try {
        const Bytes data = os::read_file(state_path(dir, key), 256u << 20);
        const nlohmann::json j = nlohmann::json::parse(std::string(data.begin(), data.end()));
        for (const auto& [hex, at] : j.at("packs").items()) out[id_from_hex(hex)] = at.get<std::uint64_t>();
    } catch (const std::exception&) {
        out.clear();  // none yet, or unreadable: start from scratch
    }
    return out;
}

void save_check_state(const std::string& dir, const std::string& key, const PackTimes& times, const Repository& repo) {
    nlohmann::json packs = nlohmann::json::object();
    for (const Id& pack : repo.index().packs()) {
        const auto it = times.find(pack);
        if (it != times.end()) packs[id_hex(pack)] = it->second;
    }
    const std::string text = nlohmann::json{{"packs", packs}}.dump();
    (void)os::make_dir(os::join(dir, "checks"));
    os::write_file_atomic(state_path(dir, key), ByteSpan(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()), 0600);
}

std::uint64_t packs_due(const PackTimes& times, const Repository& repo, std::uint64_t now, std::uint64_t period) {
    std::uint64_t due = 0;
    for (const Id& pack : repo.index().packs()) {
        const auto it = times.find(pack);
        if (it == times.end() || it->second + period <= now) ++due;
    }
    return due;
}

}  // namespace vaultik::jobs
