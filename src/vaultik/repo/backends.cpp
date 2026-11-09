#include "vaultik/repo/backends.hpp"

namespace vaultik {

std::unique_ptr<Backend> open_backend(const std::string& location, const CloudCredentials* cloud, bool create) {
    if (is_cloud_location(location)) {
        CloudCredentials c = cloud != nullptr && !cloud->empty() ? *cloud : CloudCredentials::from_environment();
        if (c.region.empty() && cloud != nullptr) c.region = cloud->region;
        return std::make_unique<S3Backend>(location, std::move(c));
    }
    return std::make_unique<LocalBackend>(location, create);
}

}  // namespace vaultik
