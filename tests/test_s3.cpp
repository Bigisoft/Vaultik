#include <doctest/doctest.h>

#include <string>

#include "vaultik/crypto/crypto.hpp"
#include "vaultik/platform/http.hpp"
#include "vaultik/repo/s3_backend.hpp"
#include "vaultik/util/hex.hpp"

using namespace vaultik;

namespace {

ByteSpan span_of(std::string_view s) {
    return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

std::string hex32(const Key32& k) {
    return to_hex(ByteSpan(k.data(), k.size()));
}

}  // namespace

TEST_CASE("SHA-256 and HMAC-SHA256 known answers") {
    CHECK(hex32(sha256(span_of("abc"))) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(hex32(sha256({})) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    // RFC 4231, test case 2.
    CHECK(hex32(hmac_sha256(span_of("Jefe"), span_of("what do ya want for nothing?"))) ==
          "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
}

TEST_CASE("S3 signature matches the AWS example") {
    // "GET Object" example from the AWS Signature Version 4 documentation for S3.
    s3::SignedRequest r;
    r.method = "GET";
    r.canonical_uri = "/test.txt";
    r.payload_hash = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    r.amz_date = "20130524T000000Z";
    r.headers = {{"x-amz-date", r.amz_date},
                 {"range", "bytes=0-9"},
                 {"host", "examplebucket.s3.amazonaws.com"},
                 {"x-amz-content-sha256", r.payload_hash}};
    const CloudCredentials creds("AKIAIOSFODNN7EXAMPLE", "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY");
    CHECK(s3::authorization(r, creds, "us-east-1") ==
          "AWS4-HMAC-SHA256 Credential=AKIAIOSFODNN7EXAMPLE/20130524/us-east-1/s3/aws4_request, "
          "SignedHeaders=host;range;x-amz-content-sha256;x-amz-date, "
          "Signature=f0e8bdb87c964420e857bd35b5d6ed310bd44f0170aba48dd91039c6036bdb41");
}

TEST_CASE("S3 URI encoding") {
    CHECK(s3::uri_encode("data/ab/ab12", true) == "data/ab/ab12");
    CHECK(s3::uri_encode("data/ab/ab12", false) == "data%2Fab%2Fab12");
    CHECK(s3::uri_encode("a b+c~_.-", false) == "a%20b%2Bc~_.-");
}

TEST_CASE("cloud storage locations") {
    CHECK(is_cloud_location("s3:https://x/y"));
    CHECK_FALSE(is_cloud_location("E:\\Backups"));
    CHECK_FALSE(is_cloud_location("s:\\x"));

    S3Location l = parse_s3_location("s3:https://acc.r2.cloudflarestorage.com/backups/my-pc/");
    CHECK(l.endpoint == "https://acc.r2.cloudflarestorage.com");
    CHECK(l.host == "acc.r2.cloudflarestorage.com");
    CHECK(l.bucket == "backups");
    CHECK(l.folder == "my-pc");

    l = parse_s3_location("s3:http://localhost:9000/vaultik");
    CHECK(l.endpoint == "http://localhost:9000");
    CHECK(l.host == "localhost:9000");
    CHECK(l.bucket == "vaultik");
    CHECK(l.folder.empty());

    CHECK_THROWS_AS(parse_s3_location("s3:https://host"), Error);
    CHECK_THROWS_AS(parse_s3_location("s3:ftp://host/b"), Error);
    CHECK_THROWS_AS(parse_s3_location("s3:https://host/b?x=1"), Error);

    CHECK(default_region("acc.r2.cloudflarestorage.com") == "auto");
    CHECK(default_region("s3.eu-central-003.backblazeb2.com") == "eu-central-003");
    CHECK(default_region("s3.us-west-2.amazonaws.com") == "us-west-2");
    CHECK(default_region("s3.amazonaws.com") == "us-east-1");
    CHECK(default_region("localhost:9000") == "us-east-1");

    const http::Url u = http::parse_url("https://[::1]:8443/a?b");
    CHECK(u.host == "[::1]");
    CHECK(u.port == 8443);
    CHECK(u.path_and_query == "/a?b");
}

TEST_CASE("S3 list responses") {
    const std::string xml = R"(<?xml version="1.0" encoding="UTF-8"?>
<ListBucketResult xmlns="http://s3.amazonaws.com/doc/2006-03-01/">
  <Name>b</Name><Prefix>pc/data/</Prefix><KeyCount>2</KeyCount>
  <Contents><Key>pc/data/ab/ab12</Key><Size>1024</Size></Contents>
  <Contents><Key>pc/data/cd/cd&amp;34</Key><Size>7</Size></Contents>
  <IsTruncated>true</IsTruncated>
  <NextContinuationToken>1ueGcxLPRx1Tr/XYExHnhbYLgveDs2J/wm36Hy4vbOwM=</NextContinuationToken>
</ListBucketResult>)";
    const s3::ListPage page = s3::parse_list(xml);
    REQUIRE(page.objects.size() == 2);
    CHECK(page.objects[0].first == "pc/data/ab/ab12");
    CHECK(page.objects[0].second == 1024);
    CHECK(page.objects[1].first == "pc/data/cd/cd&34");
    CHECK(page.truncated);
    CHECK(page.next_token == "1ueGcxLPRx1Tr/XYExHnhbYLgveDs2J/wm36Hy4vbOwM=");

    CHECK(s3::xml_value("<Error><Code>NoSuchKey</Code></Error>", "Code") == "NoSuchKey");
    CHECK(s3::xml_value("<a/>", "Code").empty());
}
