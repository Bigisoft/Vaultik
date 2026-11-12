#pragma once

// The recovery kit: one printable page with a backup's recovery key (as text and as a QR code),
// where the backup is, which computer it belongs to, and the steps to restore on a new computer.
// It holds no password and no cloud storage key: with the recovery key a password can be set
// again, but the storage itself must still be reached.

#include <cstdint>
#include <string>

#include "vaultik/jobs/report.hpp"
#include "vaultik/util/bytes.hpp"
#include "vaultik/util/qr.hpp"

namespace vaultik::jobs {

struct RecoveryKit {
    std::string key;       // "XXXX-XXXX-..." as format_recovery_key writes it
    std::string location;  // where the backup is, without secrets (see public_location)
    std::string kind;      // a fixed English phrase from location_kind (translated when shown)
    std::string kind_arg;  // its %1, if any (a cloud drive's name)
    std::string computer;
    std::uint64_t made = 0;  // FILETIME ticks
    std::string version;     // Vaultik's
};

// A location as it may be printed: a password in a URL's user part, and any query or fragment
// (which can carry tokens), are left out.
std::string public_location(const std::string& location);

// What kind of place a backup location is, as a fixed English phrase and its %1: "a folder on an
// external drive", "a network share", "S3-compatible cloud storage", "the cloud drive %1" ...
// Looks at the drive for a folder.
std::pair<std::string, std::string> location_kind(const std::string& location);

// The kit for `key` and `location`, made now on this computer.
RecoveryKit make_recovery_kit(const std::string& key, const std::string& location);

// A self-contained page (inline styles, no scripts, no links, every text escaped). The QR code is
// inline SVG unless `qr_html` gives the markup to use in its place (the frontend's own image).
std::string render_kit_html(const RecoveryKit& kit, const ReportWords& words = {}, const std::string& qr_html = {});
// A one-page A4 PDF, in English. The PDF's built-in fonts have Western European letters only: other
// letters (a computer or folder name in Cyrillic, Greek, Chinese ...) are drawn with a font of this
// system that has them, embedded with just those letters; '?' when no font has one. Letters are
// placed one after another, so scripts that join or reorder letters (Arabic, Hebrew, Indian
// scripts) do not come out as they are written.
Bytes render_kit_pdf(const RecoveryKit& kit);

// The QR code of the kit (the key as text, error correction Q).
qr::Code kit_qr(const RecoveryKit& kit);
// A QR code as SVG, `module` units per module, with the 4-module light border around it.
std::string qr_svg(const qr::Code& code, int module = 4);

// Writes the kit to `path` (".pdf" for the PDF, anything else for the page), readable by this
// account only where the system supports it. Throws Error.
void write_recovery_kit(const RecoveryKit& kit, const std::string& path);

}  // namespace vaultik::jobs
