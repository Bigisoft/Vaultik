#pragma once

// Whether a newer Vaultik has been released: the newest release of github.com/Bigisoft/Vaultik.
// Only asks; nothing is downloaded or installed (the user does that from the release page, where
// the files are signed when a code signing certificate was available, see tools/package.ps1).

#include <array>
#include <optional>
#include <string>

namespace vaultik::jobs {

struct Release {
    std::string version;  // "0.7.0"
    std::string url;      // the release's page on GitHub
    std::string name;     // its title
};

// "0.10.2" -> {0, 10, 2}; a leading "v" is skipped, missing or bad parts are 0.
std::array<int, 3> version_numbers(const std::string& version);
// Whether `candidate` is a later version than `current`.
bool newer_version(const std::string& candidate, const std::string& current);

// The release described by GitHub's "latest release" JSON, or nullopt for a draft, a pre-release,
// or anything that is not a release of Vaultik's own repository. Throws nothing.
std::optional<Release> parse_release(const std::string& json);

// Asks GitHub for the newest release; nullopt when there is none newer than `current` (also when
// the repository has no releases yet). Throws Error when GitHub cannot be reached or answers
// with an error.
std::optional<Release> newer_release(const std::string& current);

}  // namespace vaultik::jobs
