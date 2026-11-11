#pragma once

#include <stdexcept>
#include <string>

namespace vaultik {

// Base class for every error Vaultik reports to the user.
class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Authentication failed: wrong password, or data was tampered with or corrupted.
class AuthError : public Error {
public:
    using Error::Error;
};

// A repository file is in archive storage (S3 Glacier Flexible Retrieval or Deep Archive) and
// must be restored for a while ("vaultik cloud-thaw") before it can be read.
class ArchivedError : public Error {
public:
    using Error::Error;
};

}  // namespace vaultik
