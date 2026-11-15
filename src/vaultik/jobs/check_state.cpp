#include "vaultik/jobs/check_state.hpp"

#include <cctype>

#include <nlohmann/json.hpp>

#include "vaultik/platform/os.hpp"
#include "vaultik/repo/cloud.hpp"
#include "vaultik/util/hex.hpp"

namespace vaultik::jobs {

namespace {

std::string state_path(const std::string& dir, const std::string& key) {
    for (const char c : key)
        if (!std::isxdigit(static_cast<unsigned char>(c))) throw Error("invalid check state name");
    return os::join(os::join(dir, "checks"), key + ".json");
}

}  // namespace

std::string check_state_key(Repository& repo) {
    return check_state_key(repo, repo.backend().location());
}

std::string check_state_key(const Repository& repo, const std::string& location) {
    // Where it is: the drive (its volume id, which stays the same under another drive letter or
    // mount point) and the folder on it; cloud storage by its location.
    std::string place = location;
    if (!is_cloud_location(location)) {
        try {
            const std::string full = os::absolute_path(location);
            const os::VolumeLocation v = os::volume_of(full);
            if (!v.volume.empty() && full.size() >= v.root.size()) place = v.volume + "|" + full.substr(v.root.size());
            else place = full;
        } catch (const std::exception&) {
        }
        while (place.size() > 1 && (place.back() == '/' || place.back() == '\\')) place.pop_back();
#ifdef _WIN32
        place = os::fold_case(place);
        for (char& c : place)
            if (c == '/') c = '\\';
#endif
    }
    const auto& id = repo.config().repo_id;
    Bytes key(id.begin(), id.end());
    key.push_back(0);
    key.insert(key.end(), place.begin(), place.end());
    const Id h = blake3(key);
    return to_hex(ByteSpan(h.data(), 16));
}

CheckState load_check_state(const std::string& dir, const std::string& key) {
    CheckState out;
    try {
        const Bytes data = os::read_file(state_path(dir, key), 256u << 20);
        const nlohmann::json j = nlohmann::json::parse(std::string(data.begin(), data.end()));
        for (const auto& [hex, at] : j.at("packs").items()) out.packs[id_from_hex(hex)] = at.get<std::uint64_t>();
        out.cycle_started = j.value("cycle_started", std::uint64_t{0});
    } catch (const std::exception&) {
        out = CheckState{};  // none yet, or unreadable: start from scratch
    }
    return out;
}

void save_check_state(const std::string& dir, const std::string& key, const CheckState& state, const Repository& repo) {
    nlohmann::json packs = nlohmann::json::object();
    for (const Id& pack : repo.index().packs()) {
        const auto it = state.packs.find(pack);
        if (it != state.packs.end()) packs[id_hex(pack)] = it->second;
    }
    nlohmann::json j{{"packs", packs}};
    if (state.cycle_started != 0) j["cycle_started"] = state.cycle_started;
    const std::string text = j.dump();
    (void)os::make_dir(os::join(dir, "checks"));
    os::write_file_atomic(state_path(dir, key), ByteSpan(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()), 0600);
}

std::uint64_t packs_unread_since(const PackTimes& times, const Repository& repo, std::uint64_t since, std::uint64_t now) {
    const std::vector<bool> archived = repo.archived_packs();  // a check cannot read them
    std::uint64_t due = 0;
    const auto& packs = repo.index().packs();
    for (std::size_t p = 0; p < packs.size(); ++p) {
        if (p < archived.size() && archived[p]) continue;
        const auto it = times.find(packs[p]);
        if (it == times.end() || it->second < since || it->second > now) ++due;
    }
    return due;
}

std::uint64_t plan_full_check(CheckState& state, bool resumable, std::uint64_t now) {
    if (!resumable) return 0;
    if (state.cycle_started == 0 || state.cycle_started > now) state.cycle_started = now;
    return state.cycle_started;
}

std::uint64_t finish_full_check(CheckState& state, const Repository& repo, std::uint64_t since, std::uint64_t now) {
    const std::uint64_t left = packs_unread_since(state.packs, repo, since, now);
    // A cycle going on is complete once every pack was read since it started (also when a full
    // check of the user's read them all).
    if (state.cycle_started != 0 && packs_unread_since(state.packs, repo, state.cycle_started, now) == 0)
        state.cycle_started = 0;
    return left;
}

}  // namespace vaultik::jobs
