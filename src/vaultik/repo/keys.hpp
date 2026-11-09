#pragma once

// Password changes and the recovery key.
//
// The repository's master key never changes; only the way it is unlocked does. `config` holds it
// sealed with a key derived from the password. The optional `recovery` file holds it sealed with
// a key derived from a random recovery key (160 bits, shown to the user once as 8 groups of 4
// characters). Either one unlocks the repository and can set a new password, so a forgotten
// password is recoverable only when a recovery key was made beforehand.

#include <string>
#include <string_view>

#include "vaultik/repo/backend.hpp"
#include "vaultik/repo/config.hpp"

namespace vaultik {

// How to unlock a repository: its password or its recovery key.
struct Credential {
    std::string secret;
    bool recovery_key = false;
};

constexpr std::size_t kMinPasswordLength = 8;

// The master key, unlocked with `credential`. Throws AuthError when it does not fit (or the key
// files were changed), Error when the repository has no recovery key and one was given.
MasterKey unlock_master_key(Backend& backend, const Credential& credential);

// Whether the repository has a recovery key.
bool has_recovery_key(Backend& backend);

// Makes a new recovery key and returns it for the user to keep; an older recovery key stops
// working. Throws AuthError when `credential` does not unlock the repository.
std::string create_recovery_key(Backend& backend, const Credential& credential);

// Seals the master key with `new_password` (at least kMinPasswordLength characters) and a fresh
// salt. The old password stops working; snapshots and data are unchanged. A recovery key keeps
// working.
void change_password(Backend& backend, const Credential& credential, std::string_view new_password);

// Recovery key text: format_recovery_key gives "XXXX-XXXX-...", parse_recovery_key accepts it
// with any case, spaces or dashes and the look-alikes O (zero), I and L (one). Throws Error on a
// malformed key.
std::string format_recovery_key(ByteSpan key);
Bytes parse_recovery_key(std::string_view text);

}  // namespace vaultik
