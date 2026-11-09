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

private:
    struct Result;
    // Sends a signed request, retrying network failures, throttling and server errors.
    Result request(const std::string& method, const std::string& key, const std::string& query,
                   std::vector<std::pair<std::string, std::string>> extra_headers, ByteSpan body);
    std::string key_of(const std::string& name) const;

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

}  // namespace s3

}  // namespace vaultik
