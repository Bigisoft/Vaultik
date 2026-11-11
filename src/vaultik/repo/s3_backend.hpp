#pragma once

// Repository in S3-compatible cloud storage: Cloudflare R2, Backblaze B2, Amazon S3, MinIO, ...
//
// Location: "s3:https://<endpoint>/<bucket>[/<folder>]", for example
//   s3:https://<account id>.r2.cloudflarestorage.com/backups/my-pc
//   s3:https://s3.eu-central-003.backblazeb2.com/my-bucket/vaultik
//   s3:http://localhost:9000/vaultik
// Requests use path-style addresses and AWS Signature Version 4. Every repository file is one object
// ("<folder>/data/ab/ab12..."); S3 replaces objects atomically, so no temporary names are needed.
// The bucket must exist; Vaultik never creates or deletes buckets.
//
// Archive storage: with CloudCredentials::storage_class set, packs of file data and their repair
// data are uploaded in that class (x-amz-storage-class); everything else (config, keys, index,
// snapshots, folder listings, locks) stays in the standard class. Reading a file in GLACIER or
// DEEP_ARCHIVE fails with ArchivedError until it is thawed (RestoreObject, see thaw).

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "vaultik/repo/backend.hpp"
#include "vaultik/repo/cloud.hpp"

namespace vaultik {

namespace http {
class Client;
}


struct S3Location {
    std::string endpoint;  // "https://host[:port]"
    std::string host;      // as in the Host header ("host" or "host:port")
    std::string bucket;
    std::string folder;    // without leading or trailing '/', may be empty
};
// Throws Error with what is wrong.
S3Location parse_s3_location(const std::string& location);

// The signing region for an endpoint host when none is given.
std::string default_region(const std::string& host);

class S3Backend final : public Backend {
public:
    // Throws Error for a bad location or missing keys. Does not contact the server.
    S3Backend(const std::string& location, CloudCredentials credentials);
    ~S3Backend() override;

    std::string location() const override;
    bool exists(const std::string& name) override;
    void put(const std::string& name, ByteSpan data) override;
    Bytes get(const std::string& name) override;
    Bytes get_range(const std::string& name, std::uint64_t offset, std::size_t length) override;
    std::uint64_t size(const std::string& name) override;
    std::vector<std::string> list(const std::string& dir) override;
    void remove(const std::string& name) override;
    std::string data_class() const override { return credentials_.storage_class; }
    bool data_needs_thaw() const override;
    void put_data(const std::string& name, ByteSpan data) override;

    // A repository file's storage class and thaw, from a HEAD request.
    struct ArchiveState {
        std::string storage_class;  // as the server reports it; empty: standard
        bool archived = false;      // in a class that needs a thaw before reading
        bool thawing = false;       // a thaw was asked for and is not finished
        bool thawed = false;        // a readable copy is there (until `expiry`)
        std::string expiry;         // "Fri, 21 Dec 2012 00:00:00 GMT"
    };
    ArchiveState archive_state(const std::string& name);

    enum class ThawOutcome { Requested, InProgress, Available };
    // Asks for a readable copy of an archived file for `days` days (RestoreObject) at `tier`
    // ("Standard", "Bulk" or "Expedited"). Available: the file is not archived, or a copy is
    // there already; InProgress: a thaw asked for earlier is still running.
    ThawOutcome thaw(const std::string& name, int days, const std::string& tier);

    // Whether the bucket has Object Lock turned on (needed for CloudCredentials::lock_days).
    bool object_lock_enabled();

    // Brings every repository file back as it was at `time` (seconds since 1970) from the bucket's
    // old versions: removes later delete markers and later versions, or, when a later version is
    // itself locked, copies the old version over it. Files added after `time` stay. Needs a
    // bucket with versioning (always on with Object Lock).
    struct RollbackResult {
        std::uint64_t files_checked = 0;
        std::uint64_t restored = 0;    // deleted or overwritten files brought back
        std::uint64_t unchanged = 0;
        std::uint64_t newer = 0;       // added after `time`, left alone
        std::vector<std::string> failed;  // "<file>: <why>"
    };
    RollbackResult roll_back(std::int64_t time, bool dry_run,
                             const std::function<void(const std::string&)>& on_file = {});

private:
    struct Result;
    // Sends a signed request, retrying network failures, throttling and server errors.
    Result request(const std::string& method, const std::string& key, const std::string& query,
                   std::vector<std::pair<std::string, std::string>> extra_headers, ByteSpan body);
    std::string key_of(const std::string& name) const;
    void put_object(const std::string& name, ByteSpan data, bool data_class);

    std::string location_;
    S3Location where_;
    CloudCredentials credentials_;
    std::unique_ptr<http::Client> client_;
};

// The pieces of the S3 protocol, exposed for tests.
namespace s3 {

// Percent-encodes everything but A-Z a-z 0-9 - _ . ~ (and '/' when `keep_slash`).
std::string uri_encode(std::string_view s, bool keep_slash);

struct SignedRequest {
    std::string method;
    std::string canonical_uri;    // encoded path, e.g. "/bucket/data/ab/ab12"
    std::string canonical_query;  // encoded, sorted "a=1&b=2", or empty
    // Headers to sign, lower-case names; must include "host". Sorted by the signer.
    std::vector<std::pair<std::string, std::string>> headers;
    std::string payload_hash;  // hex SHA-256 of the body
    std::string amz_date;      // "20130524T000000Z"
};
// The Authorization header value (AWS Signature Version 4, service "s3").
std::string authorization(const SignedRequest& request, const CloudCredentials& credentials,
                          const std::string& region);

struct ListPage {
    std::vector<std::pair<std::string, std::uint64_t>> objects;  // key, size
    bool truncated = false;
    std::string next_token;
};
// Parses a ListObjectsV2 response.
ListPage parse_list(std::string_view xml);

// The text of the first <tag>...</tag> in `xml`, XML entities decoded; empty when absent.
std::string xml_value(std::string_view xml, std::string_view tag);

// One entry of a ListObjectVersions answer: a version of an object, or a delete marker.
struct ObjectVersion {
    std::string key;
    std::string version_id;
    std::string last_modified;  // "2026-11-09T20:00:00.000Z"
    bool delete_marker = false;
    bool latest = false;
};
struct VersionPage {
    std::vector<ObjectVersion> versions;  // in the server's order: by key, newest first
    bool truncated = false;
    std::string next_key_marker;
    std::string next_version_marker;
};
VersionPage parse_versions(std::string_view xml);

// "2026-12-09T20:00:00Z" for a time in seconds since 1970 (UTC).
std::string iso_time(std::int64_t unix_seconds);
// The headers that upload `body` with Object Lock (governance or compliance mode) until `until`
// (seconds since 1970): the mode, the date, and the Content-MD5 that S3 requires with them.
std::vector<std::pair<std::string, std::string>> lock_headers(ByteSpan body, std::int64_t until, bool compliance);

// The extra headers of a PUT of repository file `name` at `now` (seconds since 1970): the lock
// headers when `credentials` ask for a lock (never for locks/), and x-amz-storage-class when
// `data_class` (a pack of file data or its repair data) and a storage class is set.
std::vector<std::pair<std::string, std::string>> put_headers(const std::string& name, ByteSpan body,
                                                             const CloudCredentials& credentials, bool data_class,
                                                             std::int64_t now);

// The body of a RestoreObject request (POST ?restore). `tier`: Standard, Bulk or Expedited, any
// case; throws Error for another, or for `days` outside 1-365.
std::string restore_request(int days, const std::string& tier);

// An x-amz-restore header: `ongoing-request="true"` while a thaw runs, then
// `ongoing-request="false", expiry-date="Fri, 21 Dec 2012 00:00:00 GMT"`.
struct RestoreHeader {
    bool present = false;
    bool ongoing = false;
    std::string expiry;
};
RestoreHeader parse_restore_header(std::string_view value);

}  // namespace s3

}  // namespace vaultik
