#pragma once

// The recovery drive: a Windows PE system (from Microsoft's free Windows ADK and its Windows PE
// add-on) with vaultik.exe on it, which starts straight into "vaultik recovery", a text menu that
// restores a disk image onto this computer's disk. Written to a USB stick or an ISO file.
//
// Why the ADK and not the computer's own recovery environment (WinRE, from reagentc): WinRE lives
// in a hidden partition, differs by Windows version and edition, is not meant to be copied, and
// on many computers is missing or broken; Windows PE from the ADK is the documented way to build
// boot media, the same everywhere, and can be made on any computer that has the ADK.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "vaultik/util/bytes.hpp"

namespace vaultik::image {

struct AdkInfo {
    bool found = false;
    std::string kit;       // "...\Assessment and Deployment Kit\"
    std::string winpe;     // "...\Windows Preinstallation Environment\amd64\"
    std::string oscdimg;   // "...\Deployment Tools\amd64\Oscdimg\"
    std::string bootsect;  // "...\Deployment Tools\amd64\BCDBoot\bootsect.exe"
    std::string missing;   // what is missing, when not found
};
AdkInfo find_adk();
// How to get the ADK, in plain words.
std::string adk_help();

struct RecoveryMediaOptions {
    std::string iso;  // an ISO file to write, or
    std::string usb;  // a USB drive ("E:") to format and write
    std::string program;  // vaultik.exe to put on it (default: this program)
    // Called with each step ("Copying Windows PE...") as it starts.
    std::function<void(const std::string&)> on_step;
    std::function<void(const std::string&)> on_warning;
};
// Builds the recovery drive (administrator rights: DISM mounts the Windows PE image). Throws
// Error with a readable reason; the work folder is removed either way.
void make_recovery_media(const RecoveryMediaOptions& options);

// The startnet.cmd Windows PE runs at start.
std::string startnet_script();

// The DLLs a Windows program loads at start (`delayed`: those it loads only when first used),
// from its import tables. Empty when `exe` is not a 64-bit PE file.
std::vector<std::string> pe_imports(ByteSpan exe, bool delayed);

// Why a drive must not be made into a recovery drive ("E:"): not a USB or removable drive, the
// drive Windows runs from, or one holding a backup. Empty when it may.
std::string usb_refusal(const std::string& drive, const std::vector<std::string>& backup_locations);

}  // namespace vaultik::image
