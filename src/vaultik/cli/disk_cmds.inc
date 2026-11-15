// Disk tools: the "disk" command (list, init, wipe, wipe-free). Included into main.cpp inside its
// anonymous namespace, after image_cmds.inc (it uses the disk listing and confirmation helpers
// there).

// ---- disk tools ------------------------------------------------------------------------------

#ifdef _WIN32
constexpr bool kMsrOnGpt = true;  // Windows puts a reserved partition first on a GPT disk
constexpr const char* kDefaultFs = "ntfs";
#else
constexpr bool kMsrOnGpt = false;
constexpr const char* kDefaultFs = "ext4";
#endif

// "Pass 2 of 3: 45%, 12 GB of 27 GB, 180 MB/s, 2 min left".
std::string wipe_progress_text(const image::WipeProgress& p) {
    const int pct = p.total == 0 ? 0 : static_cast<int>(p.done * 100 / p.total);
    std::string s = p.verifying     ? std::string("Checking")
                    : p.passes > 1 ? "Pass " + std::to_string(p.pass) + " of " + std::to_string(p.passes)
                                   : std::string("Writing");
    s += ": " + std::to_string(pct) + "%, " + format_bytes(p.done) + " of " + format_bytes(p.total);
    if (p.bytes_per_second > 0) s += ", " + format_bytes(static_cast<std::uint64_t>(p.bytes_per_second)) + "/s";
    if (p.seconds_left >= 0) s += ", " + format_duration(p.seconds_left) + " left";
    return s;
}

// The confirmation for erasing something: --confirm, or typed at the console after `what` was
// shown. Throws Error when it is not `want`.
void confirm_typed(const Args& a, const std::string& want, const std::string& what) {
    std::string typed;
    if (auto v = a.value("--confirm")) {
        typed = *v;
    } else {
        if (g_json) throw Error("this needs --confirm \"" + want + "\"");
        out("\n" + what + "\n");
        typed = read_line("Type " + want + " to go on (anything else stops): ");
    }
    if (!image::typed_matches(typed, want)) throw Error("not confirmed; nothing was written");
}

std::string disk_volumes_text(const image::DiskInfo& d) {
    std::string s;
    for (const image::DiskVolume& v : d.volumes) {
        std::string one = v.mount;
#ifdef _WIN32
        if (!one.empty()) one = one.substr(0, 2);
#else
        if (one.empty() && v.offset != 0) one = v.name;
#endif
        if (!v.label.empty()) one += (one.empty() ? "" : " ") + v.label;
        if (one.empty()) continue;
        s += (s.empty() ? "" : ", ") + one;
    }
    return s;
}

std::string tool_disk_title(const image::DiskInfo& d) {
    std::string s = image::describe(d);
#ifndef _WIN32
    s += " (" + d.path + ")";
#endif
    return s;
}

int cmd_disk_list(const Args&) {
    const std::vector<image::DiskInfo> disks = image::list_tool_disks();
    const std::vector<int> running = image::system_disks();
    const std::vector<int> keep = image::protected_tool_disks();
    if (g_json) {
        nlohmann::json list = nlohmann::json::array();
        for (const image::DiskInfo& d : disks) {
            nlohmann::json j = disk_json(d);
            j["path"] = d.path;
            j["media"] = d.media;
            j["refusal"] = image::tool_refusal(d, running, keep);
#ifndef _WIN32
            j["in_use"] = image::mounted_volumes(d);  // mounted: unmounted with --force-dismount
#endif
            j["erase_text"] = image::erase_text(d);
            list.push_back(j);
        }
        emit({{"event", "disks"}, {"disks", list}});
        return kExitOk;
    }
    if (disks.empty()) {
        out("No disks found.\n");
        return kExitOk;
    }
    for (const image::DiskInfo& d : disks) {
        out(tool_disk_title(d) + "\n");
        out("  " + d.style + ", " + std::to_string(d.partitions) + " partition(s)" + (d.media.empty() ? "" : ", " + d.media) +
            (d.id.empty() ? std::string() : ", id " + d.id) + "\n");
        const std::string vols = disk_volumes_text(d);
        if (!vols.empty()) out("  " + vols + "\n");
        const std::string why = image::tool_refusal(d, running, keep);
        if (!why.empty()) out("  Cannot be erased: " + why + "\n");
    }
    return kExitOk;
}

// The disk named on the command line, checked: it may be erased, and (with --expect-*) it is the
// disk the caller showed the user. Linux: a mounted partition on it is in use (VolumesInUse),
// unless --force-dismount unmounts it later, once the confirmation was typed.
const image::DiskInfo& tool_target(const Args& a, const std::vector<image::DiskInfo>& disks, const std::string& arg,
                                   image::DiskIdentity& confirmed) {
    const image::DiskInfo& d = image::find_tool_disk(disks, arg);
    const std::string why = image::tool_refusal(d, image::system_disks(), image::protected_tool_disks(), true);
    if (!why.empty()) throw Error(tool_disk_title(d) + " cannot be erased: " + why);
    const auto expected = expected_disk(a);
    if (expected) {
        const std::string other = image::identity_mismatch(d, *expected);
        if (!other.empty()) throw Error(other + "; nothing was written");
    }
#ifndef _WIN32
    const std::vector<std::string> busy = image::mounted_volumes(d);
    if (!busy.empty() && !a.has("--force-dismount")) throw image::VolumesInUse(busy);
#endif
    confirmed = expected ? *expected : image::identity_of(d);
    return d;
}

// The file system, label, letter and mount point of a disk to set up (disk init, and disk wipe
// with --then-init), as given; checked later.
image::NewDiskOptions new_disk_options(const Args& a, image::TableStyle style) {
    image::NewDiskOptions o;
    o.style = style;
    o.filesystem = a.value("--fs").value_or(std::string());
    std::transform(o.filesystem.begin(), o.filesystem.end(), o.filesystem.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    o.label = a.value("--label").value_or(std::string());
    if (auto v = a.value("--letter")) {
        std::string l = *v;
        while (!l.empty() && (l.back() == ':' || l.back() == '\\')) l.pop_back();
        if (l.size() != 1 || !std::isalpha(static_cast<unsigned char>(l[0]))) throw Error("--letter takes one letter, such as E");
        o.letter = static_cast<char>(std::toupper(static_cast<unsigned char>(l[0])));
    }
    if (auto v = a.value("--mount")) o.mount = os::absolute_path(*v);
    return o;
}

int cmd_disk_init(const Args& a) {
    if (a.positional.size() < 2)
        throw Error("usage: vaultik disk init <disk> [--gpt|--mbr] [--fs ntfs|exfat|ext4] [--label <name>] [--letter <X>]");
    if (a.has("--gpt") && a.has("--mbr")) throw Error("give --gpt or --mbr, not both");
    image::NewDiskOptions o = new_disk_options(a, a.has("--mbr") ? image::TableStyle::Mbr : image::TableStyle::Gpt);
    for (const std::string& s : a.all("--size")) o.sizes.push_back(s == "rest" ? 0 : parse_size(s));
    image::check_init_options(o);
#ifdef _WIN32
    if (!os::process_elevated()) throw Error("setting up a disk needs administrator rights");
#else
    if (::geteuid() != 0) throw Error("setting up a disk needs root (sudo)");
#endif

    const std::vector<image::DiskInfo> disks = image::list_tool_disks();
    image::DiskIdentity confirmed;
    const image::DiskInfo& d = tool_target(a, disks, a.positional[1], confirmed);
    const bool with_msr = o.style == image::TableStyle::Gpt && kMsrOnGpt;
    // Planned before the question, so a plan that cannot fit stops before anything is asked.
    const std::vector<image::NewPartition> plan = image::plan_partitions(d.size, d.sector_size, o.style, o.sizes, with_msr);
    std::string what = "Everything on this disk is erased and it is set up as a new disk:\n  " + tool_disk_title(d);
    const std::string vols = disk_volumes_text(d);
    if (!vols.empty()) what += "\n  It holds now: " + vols;
    what += "\n  New: " + std::string(o.style == image::TableStyle::Gpt ? "GPT" : "MBR") + ", ";
    int n = 0;
    for (const image::NewPartition& p : plan)
        if (!p.reserved) what += (n++ == 0 ? "" : " + ") + format_bytes(p.length);
    what += " " + (o.filesystem.empty() ? std::string(kDefaultFs) : o.filesystem);
    confirm_typed(a, image::erase_text(d), what);

    o.force_dismount = a.has("--force-dismount");
    o.on_step = [](const std::string& s) {
        if (g_json) emit({{"event", "step"}, {"message", s}});
        else {
            clear_status();
            out(s + "...\n");
        }
    };
    const image::NewDiskResult r = image::init_disk(d, confirmed, image::protected_tool_disks(), o);
    std::string list;
    for (const std::string& v : r.volumes) list += (list.empty() ? "" : ", ") + v;
    if (g_json) {
        nlohmann::json vj = nlohmann::json::array();
        for (const std::string& v : r.volumes) vj.push_back(v);
        emit({{"event", "done"}, {"status", "success"}, {"volumes", vj}, {"message", "the disk is ready: " + list}});
    } else {
        out("The disk is ready: " + list + "\n");
    }
    return kExitOk;
}

std::function<void(const image::WipeProgress&)> wipe_progress() {
    return [](const image::WipeProgress& p) {
        if (g_json) {
            emit({{"event", "progress"}, {"wipe", true}, {"pass", p.pass}, {"passes", p.passes}, {"verifying", p.verifying},
                  {"done", p.done}, {"total", p.total}, {"bytes", p.done}, {"speed", p.bytes_per_second},
                  {"seconds_left", p.seconds_left}, {"current", wipe_progress_text(p)}});
            return;
        }
        status(wipe_progress_text(p));
    };
}

// "F:" for "F:\" (Windows); a device or folder as it is (Linux).
std::string volume_name(const std::string& v) {
#ifdef _WIN32
    if (v.size() == 3 && v[1] == ':') return v.substr(0, 2);
#endif
    return v;
}

// "NTFS", "exFAT", "ext4".
std::string filesystem_title(const std::string& fs) {
    if (fs == "ntfs") return "NTFS";
    if (fs == "exfat") return "exFAT";
    return fs;
}

// The setup asked for with --then-init (none without it). --fs, --label, --letter and --mount
// go with it only.
std::optional<image::NewDiskOptions> setup_after_wipe(const Args& a) {
    const auto style = a.value("--then-init");
    if (!style) {
        for (const char* o : {"--fs", "--label", "--letter", "--mount"})
            if (a.value(o)) throw Error(std::string(o) + " goes with --then-init (disk wipe), or with disk init");
        return std::nullopt;
    }
    std::string s = *style;
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (s != "gpt" && s != "mbr") throw Error("--then-init takes gpt or mbr");
    image::NewDiskOptions o = new_disk_options(a, s == "mbr" ? image::TableStyle::Mbr : image::TableStyle::Gpt);
    if (o.filesystem.empty()) o.filesystem = kDefaultFs;
    return o;
}

int cmd_disk_wipe(const Args& a) {
    const auto volume = a.value("--volume");
    if ((a.positional.size() < 2) == !volume.has_value() || (a.positional.size() >= 2 && volume))
        throw Error("usage: vaultik disk wipe <disk> | --volume <X:> [--method zeros|random|dod3] [--verify] "
                    "[--expect-size <bytes> --expect-serial <s> --expect-id <id>] [--force-dismount] "
                    "[--then-init gpt|mbr [--fs <type>] [--label <name>] [--letter <X>]]");
    image::WipeOptions o;
    if (auto m = a.value("--method")) {
        const auto parsed = image::parse_wipe_method(*m);
        if (!parsed) throw Error("--method is zeros, random or dod3");
        o.method = *parsed;
    }
    o.verify = a.has("--verify");
    const bool force = a.has("--force-dismount");
    const std::optional<image::NewDiskOptions> setup = setup_after_wipe(a);
    if (setup && volume) throw Error("--then-init sets up a whole disk; it does not go with --volume");
    o.should_cancel = [] { return stop_requested(); };
    o.on_progress = wipe_progress();
#ifdef _WIN32
    if (!os::process_elevated()) throw Error("erasing needs administrator rights");
#else
    if (::geteuid() != 0) throw Error("erasing a disk needs root (sudo)");
#endif
    const std::string method = std::string(image::to_string(o.method)) + (o.verify ? ", then checked" : "");
    const std::string not_set_up = setup ? "; the drive was not set up" : "";
    image::WipeResult r;
    image::EraseSetupResult whole;
    std::string erased;
    try {
        if (volume) {
            const image::VolumeTarget v = image::find_wipe_volume(*volume, true);
            // With --expect-*: the drive the caller showed the user (a letter or /dev/sdb1 may
            // belong to another drive by now), checked again on the opened drive.
            const auto expected = expected_disk(a);
            if (expected) {
                const std::string other = image::volume_mismatch(v, *expected);
                if (!other.empty()) throw Error(other + "; nothing was written");
            }
            const image::DiskIdentity confirmed = expected ? *expected : image::volume_identity(v);
            // Linux: mounted is in use; --force-dismount unmounts it once the confirmation was typed.
            if (!v.mount.empty() && !force) throw image::VolumesInUse({v.mount});
            std::string what = "Everything on " + v.name + " is overwritten (" + method + ") and cannot be brought back:\n  " +
                               v.name + (v.label.empty() ? "" : " " + v.label) +
                               (v.filesystem.empty() ? "" : ", " + v.filesystem) + ", " + format_bytes(v.size);
            confirm_typed(a, image::erase_volume_text(v.name), what);
            auto dev = image::open_wipe_volume(v, confirmed, force);
            r = image::wipe_device(*dev, o);
            erased = v.name;
        } else {
            const std::vector<image::DiskInfo> disks = image::list_tool_disks();
            image::DiskIdentity confirmed;
            const image::DiskInfo& d = tool_target(a, disks, a.positional[1], confirmed);
            // A setup that cannot work (a file system or letter that cannot be used, MBR on a
            // disk over 2 TiB) stops before the question, so nothing is erased for nothing.
            if (setup) image::check_setup_after_erase(d, *setup);
            std::string what = "Everything on this disk is overwritten (" + method + ") and cannot be brought back:\n  " +
                               tool_disk_title(d);
            const std::string vols = disk_volumes_text(d);
            if (!vols.empty()) what += "\n  It holds now: " + vols;
            if (setup) {
                what += "\n  Then it is set up again: " + std::string(setup->style == image::TableStyle::Gpt ? "GPT" : "MBR") +
                        ", " + filesystem_title(setup->filesystem);
                if (!setup->label.empty()) what += " \"" + setup->label + "\"";
                if (setup->letter != 0) what += ", " + std::string(1, setup->letter) + ":";
            }
            if (image::solid_state(d))
                what += "\n  This is a solid-state drive: overwriting reaches every block the system can see, but not\n"
                        "  the spare blocks the drive keeps for itself. For those, use the drive maker's secure\n"
                        "  erase tool (or the computer's firmware setup) as well.";
            confirm_typed(a, image::erase_text(d), what);
            image::EraseSetupSteps steps;
            const std::vector<int> keep = image::protected_tool_disks();
            steps.open = [&] { return image::open_wipe_disk(d, confirmed, keep, force); };
            steps.list = [] { return image::list_tool_disks(); };
            steps.init = [&](const image::DiskInfo& found, const image::DiskIdentity& pinned, const image::NewDiskOptions& n) {
                image::NewDiskOptions with_steps = n;
#ifdef _WIN32
                // The letter the erased volume had is free once Windows has let go of that volume,
                // which may take a moment after the disk was seen blank.
                for (int i = 0; i < 40 && n.letter != 0 && os::exists(std::string(1, n.letter) + ":\\"); ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
#endif
                with_steps.on_step = [](const std::string& s) {
                    if (g_json) emit({{"event", "step"}, {"phase", "setup"}, {"message", s}});
                    else out(s + "...\n");
                };
                // The protected-disk rules and identity checks of disk init, on the disk erased.
                return image::init_disk(found, pinned, image::protected_tool_disks(), with_steps);
            };
            steps.stop_asked = [] { return stop_requested(); };
            steps.on_setup = [] {
                if (g_json) emit({{"event", "step"}, {"phase", "setup"}, {"message", "Setting up the drive"}});
                else {
                    clear_status();
                    out("Erased. Setting up the drive...\n");
                }
            };
            whole = image::erase_and_set_up(d, confirmed, o, setup, steps);
            r = whole.wipe;
            erased = tool_disk_title(d);
        }
    } catch (const vaultik::Cancelled&) {
        const std::string msg = "cancelled; what was written so far stays overwritten, the rest is as it was" + not_set_up;
        if (g_json) emit({{"event", "done"}, {"status", "cancelled"}, {"message", msg}});
        else {
            clear_status();
            err(msg + "\n");
        }
        return kExitCancelled;
    } catch (const image::VolumesInUse&) {
        throw;  // nothing was written: the caller may run it again with --force-dismount
    } catch (const Error& e) {
        if (setup) throw Error(e.what() + not_set_up);
        throw;
    }
    const std::string msg = "Erased " + erased + ": " + format_bytes(r.bytes) + " written in " + format_duration(r.seconds) +
                            (r.verified ? ", checked" : "") + ".";
    if (!setup) {
        if (g_json) emit({{"event", "done"}, {"status", "success"}, {"message", msg}});
        else {
            clear_status();
            out(msg + "\n");
        }
        return kExitOk;
    }
    if (whole.stopped) {
        const std::string stopped = msg + " It was stopped before it was set up; the drive was not set up.";
        if (g_json) emit({{"event", "done"}, {"status", "cancelled"}, {"erased", true}, {"message", stopped}});
        else {
            clear_status();
            err(stopped + "\n");
        }
        return kExitCancelled;
    }
    if (!whole.set_up) {
        const std::string failed = "Erased, but setting it up failed: " + whole.setup_error;
        if (g_json) emit({{"event", "done"}, {"status", "setup_failed"}, {"erased", true}, {"message", failed}});
        else {
            clear_status();
            err(msg + "\n" + failed + "\n");
        }
        return kExitSetupFailed;
    }
    std::string list;
    for (const std::string& v : whole.setup.volumes) list += (list.empty() ? "" : ", ") + volume_name(v);
    const std::string ready = "Disk erased and ready as " + list + " (" + (setup->label.empty() ? "" : setup->label + ", ") +
                              filesystem_title(setup->filesystem) + ").";
    if (g_json) {
        nlohmann::json vj = nlohmann::json::array();
        for (const std::string& v : whole.setup.volumes) vj.push_back(volume_name(v));
        emit({{"event", "done"}, {"status", "success"}, {"volumes", vj}, {"label", setup->label},
              {"filesystem", filesystem_title(setup->filesystem)}, {"message", ready}});
    } else {
        clear_status();
        out(msg + "\n" + ready + "\n");
    }
    return kExitOk;
}

int cmd_disk_wipe_free(const Args& a) {
    if (a.positional.size() < 2) throw Error("usage: vaultik disk wipe-free <folder or drive> [--method zeros|random|dod3]");
    image::FreeSpaceOptions o;
    if (auto m = a.value("--method")) {
        const auto parsed = image::parse_wipe_method(*m);
        if (!parsed) throw Error("--method is zeros, random or dod3");
        o.method = *parsed;
    }
    std::string folder = a.positional[1];
#ifdef _WIN32
    if (folder.size() == 2 && folder[1] == ':') folder += "\\";
#endif
    o.should_cancel = [] { return stop_requested(); };
    o.on_progress = [](const image::FreeSpaceProgress& p) {
        image::WipeProgress w;
        w.done = p.done;
        w.total = p.total;
        w.bytes_per_second = p.bytes_per_second;
        w.seconds_left = p.seconds_left;
        if (g_json) {
            emit({{"event", "progress"}, {"wipe", true}, {"done", p.done}, {"total", p.total}, {"bytes", p.done},
                  {"speed", p.bytes_per_second}, {"seconds_left", p.seconds_left}, {"current", wipe_progress_text(w)}});
            return;
        }
        status(wipe_progress_text(w));
    };
    if (!g_json) out("Filling the free space of " + os::absolute_path(folder) + " (files there are not touched)...\n");
    std::uint64_t bytes = 0;
    const auto start = std::chrono::steady_clock::now();
    try {
        bytes = image::wipe_free_space(folder, o);
    } catch (const vaultik::Cancelled&) {
        const std::string msg = "cancelled; the temporary file was removed";
        if (g_json) emit({{"event", "done"}, {"status", "cancelled"}, {"message", msg}});
        else {
            clear_status();
            err(msg + "\n");
        }
        return kExitCancelled;
    }
    const std::string msg = "The free space was overwritten: " + format_bytes(bytes) + " in " + format_duration(seconds_since(start)) + ".";
    if (g_json) emit({{"event", "done"}, {"status", "success"}, {"message", msg}});
    else {
        clear_status();
        out(msg + "\n");
    }
    return kExitOk;
}

int cmd_disk(const Args& a) {
    const std::string sub = a.positional.empty() ? std::string() : a.positional[0];
    if (sub == "list") return cmd_disk_list(a);
    if (sub == "init") return cmd_disk_init(a);
    if (sub == "wipe") return cmd_disk_wipe(a);
    if (sub == "wipe-free") return cmd_disk_wipe_free(a);
    throw Error("usage: vaultik disk list|init|wipe|wipe-free (see vaultik --help)");
}
