#include "vaultik/repo/backends.hpp"

#include "vaultik/repo/gdrive_backend.hpp"
#include "vaultik/repo/mega_backend.hpp"
#include "vaultik/repo/onedrive_backend.hpp"
#include "vaultik/repo/pcloud_backend.hpp"
#include "vaultik/repo/webdav_backend.hpp"
#ifdef _WIN32
#include "vaultik/repo/sftp_backend.hpp"
#endif

namespace vaultik {

std::unique_ptr<Backend> open_backend(const std::string& location, const CloudCredentials* cloud, bool create) {
    const CloudKind kind = cloud_kind(location);
    if (kind == CloudKind::None) return std::make_unique<LocalBackend>(location, create);

    CloudCredentials c = cloud != nullptr && !cloud->empty() ? *cloud : CloudCredentials::from_environment();
    if (cloud != nullptr) {
        if (c.region.empty()) c.region = cloud->region;
        if (!c.on_secret_changed) c.on_secret_changed = cloud->on_secret_changed;
    }
    switch (kind) {
        case CloudKind::S3: return std::make_unique<S3Backend>(location, std::move(c));
        case CloudKind::PCloud: return std::make_unique<PCloudBackend>(location, std::move(c));
        case CloudKind::OneDrive: return std::make_unique<OneDriveBackend>(location, std::move(c));
        case CloudKind::GoogleDrive: return std::make_unique<GoogleDriveBackend>(location, std::move(c));
        case CloudKind::Mega: return std::make_unique<MegaBackend>(location, std::move(c));
        case CloudKind::WebDav: return std::make_unique<WebDavBackend>(location, std::move(c));
#ifdef _WIN32
        case CloudKind::Sftp: return std::make_unique<SftpBackend>(location, std::move(c));
#else
        case CloudKind::Sftp: throw Error("SFTP is only supported on Windows so far");
#endif
        case CloudKind::None: break;
    }
    throw Error("unknown storage location: " + location);
}

}  // namespace vaultik
