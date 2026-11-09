#pragma once

#include <memory>
#include <string>

#include "vaultik/repo/backend.hpp"
#include "vaultik/repo/local_backend.hpp"
#include "vaultik/repo/s3_backend.hpp"

namespace vaultik {

// The storage for a repository location: cloud storage for "s3:..." (keys from `cloud`, or from
// the environment when that is null or empty), a folder otherwise (created with `create`).
std::unique_ptr<Backend> open_backend(const std::string& location, const CloudCredentials* cloud = nullptr,
                                      bool create = false);

}  // namespace vaultik
