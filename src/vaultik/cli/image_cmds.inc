// Disk images, the recovery menu and the recovery drive: the "image", "recovery" and
// "recovery-media" commands. Included into main.cpp inside its anonymous namespace (it uses the
// argument, output and repository helpers there).

// ---- disk images -----------------------------------------------------------------------------

// Runs a command line through the shell in this console, so it can ask the user things itself
// ("net use" asks for a password).
int interactive_command(const std::string& command) {
#ifdef _WIN32
    return _wsystem(widen(command).c_str());
#else
    return std::system(command.c_str());
#endif
}

// A disk given on the command line: its number, "system", or a stable id ("gpt-...", "mbr-...").
int disk_arg(const std::vector<image::DiskInfo>& disks, const std::string& s) {
    if (!s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); })) {
        const int n = std::stoi(s);
        for (const image::DiskInfo& d : disks)
            if (d.number == n) return n;
        throw Error("there is no disk " + s + " (see vaultik image disks)");
    }
    const int n = image::find_disk(disks, s);
    if (n < 0) throw Error("disk " + s + " is not connected (see vaultik image disks)");
    return n;
}

// The disks a restore must never write to besides the one Windows runs from: the disk holding
// the backup (a folder) and the one Vaultik runs from.
std::vector<int> protected_disks(const std::string& repo_location) {
    std::vector<int> out;
    if (!repo_location.empty() && !is_cloud_location(repo_location)) out = os::disk_numbers(os::absolute_path(repo_location));
    for (const int n : os::disk_numbers(os::executable_path())) out.push_back(n);
    return out;
}

nlohmann::json disk_json(const image::DiskInfo& d) {
    nlohmann::json vols = nlohmann::json::array();
    for (const image::DiskVolume& v : d.volumes)
        vols.push_back({{"mount", v.mount}, {"label", v.label}, {"filesystem", v.filesystem}, {"size", v.length}});
    return {{"number", d.number}, {"model", d.model},   {"serial", d.serial},       {"bus", d.bus},
            {"size", d.size},     {"style", d.style},   {"id", d.id},               {"system", d.system},
            {"removable", d.removable}, {"sector_size", d.sector_size}, {"volumes", vols}};
}

std::string volumes_text(const image::DiskInfo& d) {
    std::string s;
    for (const image::DiskVolume& v : d.volumes) {
        std::string one = v.mount.empty() ? std::string() : v.mount.substr(0, 2);
        if (!v.label.empty()) one += (one.empty() ? "" : " ") + v.label;
        if (one.empty()) continue;
        s += (s.empty() ? "" : ", ") + one;
    }
    return s;
}

int cmd_image_disks(const Args&) {
    const std::vector<image::DiskInfo> disks = image::list_disks();
    if (g_json) {
        nlohmann::json list = nlohmann::json::array();
        for (const image::DiskInfo& d : disks) list.push_back(disk_json(d));
        emit({{"event", "disks"}, {"disks", list}});
        return kExitOk;
    }
    if (disks.empty()) {
        out("No disks found.\n");
        return kExitOk;
    }
    for (const image::DiskInfo& d : disks) {
        out(image::describe(d) + "\n");
        out("  " + d.style + ", " + std::to_string(d.partitions) + " partition(s)" +
            (d.id.empty() ? std::string() : ", id " + d.id) + "\n");
        const std::string vols = volumes_text(d);
        if (!vols.empty()) out("  " + vols + "\n");
    }
    return kExitOk;
}

// The newest image snapshot of exactly these disks (the version before the next one).
std::optional<Id> image_parent(Repository& repo, const std::vector<std::string>& paths) {
    std::optional<Id> best;
    std::uint64_t when = 0;
    for (const SnapshotRef& s : load_all_snapshots(repo))
        if (s.snapshot.image && s.snapshot.paths == paths && s.snapshot.hostname == os::hostname() && s.snapshot.time >= when) {
            best = s.id;
            when = s.snapshot.time;
        }
    return best;
}

// Backs up whole disks with console or JSON reporting; fills `record` like backup_and_report.
int image_backup_and_report(Repository& repo, const std::vector<int>& disks, const std::vector<std::string>& tags,
                            const std::string& repo_location, bool shadow_copy, jobs::RunRecord& record, RepoLock& lock) {
    const auto start = std::chrono::steady_clock::now();
    record.started = os::now();
    try {
        // Never image the disk the backup goes to: the image would hold the backup itself.
        if (!is_cloud_location(repo_location)) {
            for (const int n : os::disk_numbers(os::absolute_path(repo_location)))
                if (std::find(disks.begin(), disks.end(), n) != disks.end())
                    throw Error("the backup destination is on disk " + std::to_string(n) +
                                ", which is being backed up; choose a destination on another disk");
        }
        if (!g_json) status("Reading the disks and making a shadow copy...");
        std::vector<std::unique_ptr<image::DiskSource>> sources =
            image::open_disk_sources(disks, shadow_copy, [](const std::string& w) { warning(w); });
        image::ImageBackupOptions o;
        std::vector<std::string> paths;
        for (const auto& s : sources) {
            o.disks.push_back(s.get());
            paths.push_back(image::image_path(s->layout()));
        }
        o.tags = tags;
        o.parent = image_parent(repo, paths);
        o.on_warning = [](const std::string& w) { warning(w); };
        o.should_cancel = [&lock] { return stop_requested() || lock.lost(); };
        o.before_save = [&lock] {
            if (!lock.held()) throw LockedError(kLockLost);
        };
        o.on_progress = [&](const image::ImageProgress& p) {
            if (g_json) {
                emit({{"event", "progress"},
                      {"image", true},
                      {"files", 0},
                      {"bytes", p.read},
                      {"done", p.done},
                      {"total", p.total},
                      {"new_bytes", p.new_bytes},
                      {"current", p.current}});
                return;
            }
            const double secs = std::max(seconds_since(start), 0.001);
            const int pct = p.total == 0 ? 0 : static_cast<int>(p.done * 100 / p.total);
            status(std::to_string(pct) + "%, " + format_bytes(p.read) + " read (" +
                   format_bytes(static_cast<std::uint64_t>(static_cast<double>(p.read) / secs)) + "/s), new " +
                   format_bytes(p.new_bytes) + "  " + p.current);
        };
        if (!g_json) {
            clear_status();
            for (const auto& s : sources) {
                const image::DiskLayout& l = s->layout();
                out("Disk " + std::to_string(l.number) + ": " + l.model + "\n");
                for (const image::Partition& p : l.partitions)
                    if (!(l.style == image::PartitionStyle::Mbr && image::is_container(p.mbr_type)))
                        out("  " + image::describe(p) + "\n");
            }
        }
        const image::ImageBackupResult r = image::run_image_backup(repo, o);
        record.finished = os::now();
        record.status = jobs::RunStatus::Success;
        record.snapshot = id_hex(r.snapshot_id);
        record.files = r.snapshot.stats.files;
        record.bytes = r.snapshot.stats.bytes;
        record.new_bytes = r.snapshot.stats.new_bytes;
        record.stored_bytes = r.snapshot.stats.stored_bytes;
        std::uint64_t live = 0;
        for (const auto& s : sources)
            for (const image::Partition& p : s->layout().partitions)
                if (p.method == image::ReadMethod::Used && !p.shadow_copy) ++live;
        record.message = "disk image of " + std::to_string(sources.size()) + " disk(s)";
        if (live > 0) record.message += "; " + std::to_string(live) + " volume(s) were read while in use";
        if (g_json) {
            emit({{"event", "done"},
                  {"status", "success"},
                  {"snapshot", record.snapshot},
                  {"image", true},
                  {"files", record.files},
                  {"bytes", record.bytes},
                  {"new_bytes", record.new_bytes},
                  {"stored_bytes", record.stored_bytes},
                  {"errors", 0},
                  {"seconds", seconds_since(start)}});
            return kExitOk;
        }
        clear_status();
        out("Disk image " + short_id(r.snapshot_id) + " saved\n");
        out("  disks:    " + format_bytes(record.bytes) + " in " + std::to_string(record.files) + " partition(s)\n");
        out("  new data: " + format_bytes(record.new_bytes) + ", stored " + format_bytes(record.stored_bytes) +
            " after compression and encryption\n");
        out("  time:     " + format_duration(seconds_since(start)) + "\n");
        return kExitOk;
    } catch (const Cancelled&) {
        record.finished = os::now();
        if (lock.lost()) {
            record.status = jobs::RunStatus::Failed;
            record.message = kLockLost;
            throw LockedError(kLockLost);
        }
        record.status = jobs::RunStatus::Cancelled;
        record.message = "cancelled";
        throw;
    } catch (const std::exception& e) {
        record.finished = os::now();
        record.status = jobs::RunStatus::Failed;
        record.message = e.what();
        throw;
    }
}

// The disk numbers of a job's disks, connected now.
std::vector<int> job_disks(const jobs::Job& job) {
    const std::vector<image::DiskInfo> disks = image::list_disks();
    std::vector<int> out;
    for (const std::string& id : job.image_disks) {
        const int n = image::find_disk(disks, id);
        if (n < 0) throw Error("the disk " + id + " of this job is not connected");
        if (std::find(out.begin(), out.end(), n) == out.end()) out.push_back(n);
    }
    return out;
}

int cmd_image_backup(const Args& a) {
    if (a.positional.size() < 2) throw Error("usage: vaultik image backup -r <repo> <disk>... (see vaultik image disks)");
    const std::vector<image::DiskInfo> disks = image::list_disks();
    std::vector<int> numbers;
    for (std::size_t i = 1; i < a.positional.size(); ++i) {
        const int n = disk_arg(disks, a.positional[i]);
        if (std::find(numbers.begin(), numbers.end(), n) == numbers.end()) numbers.push_back(n);
    }
    auto repo = open_repo(a);
    apply_parallelism(*repo, a, repo_path(a), resource_policy(a));
    RepoLock lock(*repo, RepoLock::Mode::Shared, "image backup", lock_wait(a));
    repo->reload_index();
    jobs::RunRecord record;
    return image_backup_and_report(*repo, numbers, a.all("--tag"), repo_path(a), vss_arg(a, VssMode::Auto) != VssMode::Off,
                                   record, lock);
}

std::string layout_title(const image::DiskLayout& l) {
    return (l.model.empty() ? std::string("Disk") : l.model) + " (" + format_bytes(l.size) + ", " +
           (l.style == image::PartitionStyle::Gpt ? "GPT" : "MBR") + ")";
}

int cmd_image_info(const Args& a) {
    if (a.positional.size() < 2) throw Error("usage: vaultik image info <snapshot>");
    auto repo = open_repo(a);
    RepoLock lock(*repo, RepoLock::Mode::Shared, "image info", lock_wait(a));
    repo->reload_index();
    const SnapshotRef ref = resolve_snapshot(*repo, a.positional[1]);
    const std::vector<image::DiskLayout> disks = image::image_disks(*repo, ref.snapshot);
    // How far each disk's partitions can shrink (read from each NTFS volume's bitmap).
    std::vector<std::vector<std::uint64_t>> limits;
    std::vector<std::vector<std::string>> notes(disks.size());
    for (std::size_t i = 0; i < disks.size(); ++i) limits.push_back(image::shrink_limits(*repo, ref.snapshot, i, &notes[i]));
    if (g_json) {
        nlohmann::json list = nlohmann::json::array();
        for (std::size_t i = 0; i < disks.size(); ++i) {
            nlohmann::json d = nlohmann::json::parse(image::to_json(disks[i]));
            d["smallest_target"] = image::smallest_disk(disks[i], limits[i]);
            d["min_lengths"] = limits[i];
            list.push_back(std::move(d));
        }
        emit({{"event", "image"}, {"snapshot", id_hex(ref.id)}, {"time", ref.snapshot.time}, {"disks", list}});
        return kExitOk;
    }
    out("Disk image " + short_id(ref.id) + " of " + ref.snapshot.hostname + ", " +
        os::format_time_local(ref.snapshot.time) + "\n");
    for (std::size_t i = 0; i < disks.size(); ++i) {
        const image::DiskLayout& l = disks[i];
        out(std::to_string(i + 1) + ". " + layout_title(l) + (l.system ? ", Windows ran from it" : "") + "\n");
        for (std::size_t k = 0; k < l.partitions.size(); ++k) {
            const image::Partition& p = l.partitions[k];
            if (l.style == image::PartitionStyle::Mbr && image::is_container(p.mbr_type)) continue;
            std::string how = p.method == image::ReadMethod::Used
                                  ? format_bytes(p.used_bytes) + " in use" + (p.shadow_copy ? "" : ", read while in use")
                                  : "every byte";
            if (limits[i][k] < p.length) how += "; can shrink to " + format_bytes(limits[i][k]);
            out("   " + image::describe(p) + ": " + how + "\n");
        }
        const std::uint64_t full = image::smallest_disk(l, {});
        const std::uint64_t smallest = image::smallest_disk(l, limits[i]);
        out("   Restores onto a disk of " + format_bytes(full) + " (" + std::to_string(full) + " bytes) or larger");
        if (smallest < full)
            out(", or down to " + format_bytes(smallest) + " (" + std::to_string(smallest) + " bytes) with its NTFS\n"
                "   partitions shrunk to their used space");
        out("\n");
        for (const std::string& n : notes[i]) out("   Not shrunk: " + n + "\n");
    }
    return kExitOk;
}

// Reports restore progress on the console or as JSON.
std::function<void(const image::ImageProgress&)> restore_progress(std::chrono::steady_clock::time_point start) {
    return [start](const image::ImageProgress& p) {
        if (g_json) {
            emit({{"event", "progress"}, {"image", true}, {"bytes", p.read}, {"done", p.done}, {"total", p.total},
                  {"current", p.current}});
            return;
        }
        const double secs = std::max(seconds_since(start), 0.001);
        const int pct = p.total == 0 ? 0 : static_cast<int>(p.done * 100 / p.total);
        status(std::to_string(pct) + "%, " + format_bytes(p.read) + " written (" +
               format_bytes(static_cast<std::uint64_t>(static_cast<double>(p.read) / secs)) + "/s)  " + p.current);
    };
}

// The confirmation for erasing disk `number`: --confirm, or typed at the console.
void confirm_erase(const Args& a, const image::DiskInfo& target) {
    const std::string want = image::confirmation_text(target.number);
    std::string typed;
    if (auto v = a.value("--confirm")) {
        typed = *v;
    } else {
        if (g_json) throw Error("restoring onto a disk needs --confirm \"" + want + "\"");
        out("\nEverything on this disk is replaced by the image:\n  " + image::describe(target) + "\n");
        const std::string vols = volumes_text(target);
        if (!vols.empty()) out("  It holds now: " + vols + "\n");
        typed = read_line("Type " + want + " to go on (anything else stops): ");
    }
    if (!image::confirmed(typed, target.number)) throw Error("not confirmed; nothing was written");
}

// The disk the caller showed the user (--expect-size, --expect-serial, --expect-id, as the GUI
// passes them), or nothing when none is given.
std::optional<image::DiskIdentity> expected_disk(const Args& a) {
    const auto size = a.value("--expect-size");
    if (!size) {
        if (a.value("--expect-serial") || a.value("--expect-id")) throw Error("--expect-serial and --expect-id need --expect-size");
        return std::nullopt;
    }
    image::DiskIdentity d;
    if (size->empty() || !std::all_of(size->begin(), size->end(), [](unsigned char c) { return std::isdigit(c); }))
        throw Error("--expect-size takes a number of bytes");
    d.size = std::stoull(*size);
    d.serial = a.value("--expect-serial").value_or(std::string());
    d.id = a.value("--expect-id").value_or(std::string());
    return d;
}

// Whether image disk `index` (`layout`) fits on a disk of `size` bytes, shrinking its NTFS
// partitions when it has to: an empty string, or why not.
std::string restore_fit(Repository& repo, const SnapshotRef& ref, std::size_t index, const image::DiskLayout& layout,
                        std::uint64_t size, std::uint32_t sector_size) {
    const std::string why = image::fits(layout, size, sector_size);
    if (why.empty() || sector_size != layout.sector_size) return why;
    return image::fits(layout, size, sector_size, image::shrink_limits(repo, ref.snapshot, index));
}

// Shows what a restore changes (a partition shrunk or moved) before it writes.
void restore_note(const std::string& note) {
    if (g_json) emit({{"event", "note"}, {"message", note}});
    else {
        clear_status();
        out(note + "\n");
    }
}

// Restores image disk `index` of `ref` onto physical disk `number`, after every check. The repo
// location protects its own disk. `expected` is the disk the user chose from an earlier listing:
// the restore stops when the disk at `number` is another one by now (disks come and go, and
// their numbers move). Without it, the disk listed here is the one confirmed and written.
void restore_onto_disk(Repository& repo, const SnapshotRef& ref, std::size_t index, int number,
                       const std::string& repo_location, const Args* a, bool boot,
                       const std::optional<image::DiskIdentity>& expected) {
    const std::vector<image::DiskInfo> disks = image::list_disks();
    auto it = std::find_if(disks.begin(), disks.end(), [number](const image::DiskInfo& d) { return d.number == number; });
    if (it == disks.end()) throw Error("there is no disk " + std::to_string(number));
    const std::vector<int> keep = protected_disks(repo_location);
    const std::string why = image::target_refusal(*it, image::system_disks(), keep);
    if (!why.empty()) throw Error("disk " + std::to_string(number) + " cannot be restored to: " + why);
    if (expected) {
        const std::string other = image::identity_mismatch(*it, *expected);
        if (!other.empty()) throw Error(other + "; nothing was written");
    }
    const image::DiskIdentity confirmed = expected ? *expected : image::identity_of(*it);
    const std::vector<image::DiskLayout> layouts = image::image_disks(repo, ref.snapshot);
    if (index >= layouts.size()) throw Error("the image has no disk " + std::to_string(index + 1));
    const std::string fit = restore_fit(repo, ref, index, layouts[index], it->size, it->sector_size);
    if (!fit.empty()) throw Error(fit);
    if (a != nullptr) confirm_erase(*a, *it);

    image::ImageRestoreOptions o;
    o.disk = index;
    o.should_cancel = [] { return stop_requested(); };
    o.on_note = restore_note;
    const auto start = std::chrono::steady_clock::now();
    o.on_progress = restore_progress(start);
    // Opened by number, and checked again there to be the disk confirmed.
    auto target = image::open_disk_target(number, confirmed, keep);
    image::run_image_restore(repo, ref.snapshot, *target, o);
    target.reset();
    if (!g_json) {
        clear_status();
        out("Disk " + std::to_string(number) + " restored in " + format_duration(seconds_since(start)) + ".\n");
    }
    if (boot) {
        if (!g_json) status("Writing the boot files...");
        const std::string done = image::repair_boot(number);
        if (!g_json) {
            clear_status();
            out(done + "\n");
        }
    }
}

int cmd_image_restore(const Args& a) {
    if (a.positional.size() < 2)
        throw Error("usage: vaultik image restore <snapshot> --vhdx <file> | --to-disk <number> [--confirm \"ERASE DISK n\"]");
    const auto vhdx = a.value("--vhdx");
    const auto to_disk = a.value("--to-disk");
    if (vhdx.has_value() == to_disk.has_value()) throw Error("give either --vhdx <file> or --to-disk <number>");
    std::size_t index = 0;
    if (auto v = a.value("--disk-index")) index = static_cast<std::size_t>(std::max(1, std::stoi(*v)) - 1);

    auto repo = open_repo(a);
    RepoLock lock(*repo, RepoLock::Mode::Shared, "image restore", lock_wait(a));
    repo->reload_index();
    const SnapshotRef ref = resolve_snapshot(*repo, a.positional[1]);
    const std::vector<image::DiskLayout> layouts = image::image_disks(*repo, ref.snapshot);
    if (index >= layouts.size()) throw Error("the image has no disk " + std::to_string(index + 1));
    const image::DiskLayout& layout = layouts[index];

    if (to_disk) {
        const int number = disk_arg(image::list_disks(), *to_disk);
        restore_onto_disk(*repo, ref, index, number, repo_path(a), &a, a.has("--boot"), expected_disk(a));
        if (g_json) emit({{"event", "done"}, {"status", "success"}, {"message", "disk " + std::to_string(number) + " restored"}});
        return kExitOk;
    }

    const std::string path = os::absolute_path(*vhdx);
    if (os::exists(path)) throw Error(path + " exists already; give a new file name");
    std::uint64_t size = layout.size;
    if (auto v = a.value("--size")) size = parse_size(*v);
    size = (size + layout.sector_size - 1) / layout.sector_size * layout.sector_size;
    // Smaller than the image's partitions: refused before the file is made when they cannot be
    // shrunk that far.
    const std::string fit = restore_fit(*repo, ref, index, layout, size, layout.sector_size);
    if (!fit.empty()) throw Error(fit);
    image::ImageRestoreOptions o;
    o.disk = index;
    o.new_ids = a.has("--new-ids");
    o.should_cancel = [] { return stop_requested(); };
    o.on_note = restore_note;
    const auto start = std::chrono::steady_clock::now();
    o.on_progress = restore_progress(start);
    if (!g_json) out("Restoring " + layout_title(layout) + " to " + path + "\n");
    try {
        image::VhdxTarget target(image::create_random_file(path), size, layout.sector_size, layout.physical_sector_size);
        image::run_image_restore(*repo, ref.snapshot, target, o);
    } catch (...) {
        os::remove_file(path);  // never leave half a disk file behind
        throw;
    }
    if (g_json) {
        emit({{"event", "done"}, {"status", "success"}, {"file", path}, {"message", "restored to " + path}});
        return kExitOk;
    }
    clear_status();
    out("Restored in " + format_duration(seconds_since(start)) + ". Open the file in File Explorer to see its drives,\n"
        "or use it as the disk of a Hyper-V virtual machine (generation " +
        std::string(layout.style == image::PartitionStyle::Gpt ? "2" : "1") + ").\n");
    return kExitOk;
}

int cmd_image_mount(const Args& a) {
    if (a.positional.size() < 2) throw Error("usage: vaultik image mount <snapshot> [--disk-index <n>] [--target <folder>]");
    std::size_t index = 0;
    if (auto v = a.value("--disk-index")) index = static_cast<std::size_t>(std::max(1, std::stoi(*v)) - 1);
    if (!os::process_elevated()) throw Error("opening a disk image needs administrator rights");
    auto repo = open_repo(a);
    RepoLock lock(*repo, RepoLock::Mode::Shared, "image mount", lock_wait(a));
    repo->reload_index();
    const SnapshotRef ref = resolve_snapshot(*repo, a.positional[1]);
    const std::vector<image::DiskLayout> layouts = image::image_disks(*repo, ref.snapshot);
    if (index >= layouts.size()) throw Error("the image has no disk " + std::to_string(index + 1));
    const image::DiskLayout& layout = layouts[index];

    // A temporary virtual disk with new ids (so it can sit next to the original disk).
    // By default in <ProgramData>\Vaultik\images, which only administrators can change: this runs
    // elevated, deletes and creates the file and then attaches it, so in a folder the user's
    // programs can change the file could be swapped before it is attached, or the folder be a
    // link to somewhere else.
    std::string folder;
    if (auto t = a.value("--target")) {
        folder = os::absolute_path(*t);
        os::make_dirs(folder);
    } else {
        const std::string vaultik = os::join(os::program_data_dir(), "Vaultik");
        os::make_admin_dir(vaultik);
        folder = os::join(vaultik, "images");
        os::make_admin_dir(folder);
    }
    const std::string path = os::join(folder, "vaultik-" + short_id(ref.id) + "-" + std::to_string(index + 1) + ".vhdx");
    os::remove_file(path);  // left over from an earlier run that ended abnormally
    struct RemoveFile {
        std::string path;
        ~RemoveFile() { os::remove_file(path); }
    } remove_after{path};
    {
        image::ImageRestoreOptions o;
        o.disk = index;
        o.new_ids = true;
        o.should_cancel = [] { return stop_requested(); };
        o.on_progress = restore_progress(std::chrono::steady_clock::now());
        if (!g_json) out("Preparing " + layout_title(layout) + " (the used space is copied to " + path + ")...\n");
        image::VhdxTarget target(image::create_random_file(path), layout.size, layout.sector_size, layout.physical_sector_size);
        image::run_image_restore(*repo, ref.snapshot, target, o);
    }
    lock.release();
    auto attached = image::attach_vhdx(path, true);
    const std::vector<std::string> mounts = attached->mounts();
    if (g_json) {
        nlohmann::json list = nlohmann::json::array();
        for (const std::string& m : mounts) list.push_back(m);
        emit({{"event", "mounted"}, {"drives", list}});
    } else {
        clear_status();
        std::string list;
        for (const std::string& m : mounts) list += (list.empty() ? "" : ", ") + m;
        out(mounts.empty() ? std::string("The disk is attached, but Windows gave its drives no letters.\n")
                           : "Open in File Explorer (read-only): " + list + "\n");
        out("Press Enter to close it.\n");
    }
    if (g_json) {
        while (!g_cancel.load() && !g_stdin_closed.load()) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    } else {
        std::string line;
        std::getline(std::cin, line);
    }
    attached.reset();
    if (!g_json) out("Closed.\n");
    return kExitOk;
}

int cmd_image(const Args& a) {
    const std::string sub = a.positional.empty() ? std::string() : a.positional[0];
    if (sub == "disks") return cmd_image_disks(a);
    if (sub == "backup") return cmd_image_backup(a);
    if (sub == "info") return cmd_image_info(a);
    if (sub == "restore") return cmd_image_restore(a);
    if (sub == "mount") return cmd_image_mount(a);
    throw Error("usage: vaultik image disks|backup|info|restore|mount (see vaultik --help)");
}

// ---- recovery drive --------------------------------------------------------------------------

int cmd_recovery_media(const Args& a) {
    image::RecoveryMediaOptions o;
    if (auto v = a.value("--iso")) o.iso = os::absolute_path(*v);
    if (auto v = a.value("--usb")) o.usb = *v;
    if (o.iso.empty() == o.usb.empty()) throw Error("usage: vaultik recovery-media --iso <file> | --usb <drive, e.g. E:>");
    if (!os::process_elevated()) throw Error("making a recovery drive needs administrator rights");
    if (!o.usb.empty()) {
        // Never format a drive that holds a backup of a saved job.
        std::vector<std::string> backups;
        for (const jobs::Job& j : jobs::JobStore().list()) {
            backups.push_back(j.repository);
            if (!j.second_copy.repository.empty()) backups.push_back(j.second_copy.repository);
        }
        const std::string why = image::usb_refusal(o.usb, backups);
        if (!why.empty()) throw Error(why);
        const std::string drive = std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(o.usb[0])))) + ":";
        const std::string want = "ERASE " + drive;
        std::string typed;
        if (auto v = a.value("--confirm")) typed = *v;
        else if (!g_json) typed = read_line("Everything on " + drive + " is erased. Type " + want + " to go on: ");
        std::string norm;
        for (const char c : typed)
            if (c != ' ') norm.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        if (norm != "ERASE" + drive) throw Error("not confirmed; " + drive + " was not changed");
    }
    o.on_step = [](const std::string& s) {
        if (g_json) emit({{"event", "progress"}, {"current", s}});
        else out(s + "\n");
    };
    o.on_warning = [](const std::string& w) { warning(w); };
    image::make_recovery_media(o);
    const std::string where = o.iso.empty() ? o.usb : o.iso;
    if (g_json) emit({{"event", "done"}, {"status", "success"}, {"message", "recovery drive written to " + where}});
    else out("The recovery drive is ready: " + where +
             (o.iso.empty() ? std::string() : " (write it to a USB stick or DVD, or start a virtual machine from it)") +
             ".\nStart the computer from it to restore a disk image.\n");
    return kExitOk;
}

// Repository folders on the drives (and shares) of this computer: folders holding a Vaultik
// "config" file, at most two levels below each root.
std::vector<std::string> find_repositories(const std::vector<std::string>& roots) {
    std::vector<std::string> found;
    int visited = 0;  // a few thousand folders at most: a full drive must not take minutes
    auto is_repo = [](const std::string& dir) {
        try {
            const Bytes head = os::read_file(os::join(dir, "config"), 64 * 1024);
            return head.size() >= 8 && std::memcmp(head.data(), "VAULTIK", 8) == 0;
        } catch (const std::exception&) {
            return false;
        }
    };
    std::function<void(const std::string&, int)> walk = [&](const std::string& dir, int depth) {
        if (found.size() >= 50 || ++visited > 5000) return;
        if (is_repo(dir)) {
            found.push_back(dir);
            return;
        }
        if (depth == 0) return;
        os::DirReader r(dir);
        std::string name;
        os::FileInfo info;
        int seen = 0;
        std::vector<std::string> subs;
        while (r.next(name, info) && ++seen < 500)
            if (info.type == os::EntryType::Dir && name != "$RECYCLE.BIN" && name != "System Volume Information" &&
                name != "Windows")
                subs.push_back(os::join(dir, name));
        for (const std::string& s : subs) walk(s, depth - 1);
    };
    for (const std::string& r : roots) walk(r, 3);
    return found;
}

std::vector<std::string> drive_roots() {
    std::vector<std::string> roots;
    for (char c = 'C'; c <= 'Z'; ++c) {
        const std::string root = std::string(1, c) + ":\\";
        if (os::is_directory(root)) roots.push_back(root);
    }
    return roots;
}

// Characters allowed in a share path or user name passed to "net use" (it runs through cmd).
bool safe_for_net(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '\\' || c == '.' || c == '-' || c == '_' || c == '$' || c == '@' || c == ' ';
    });
}

std::string ask(const std::string& prompt) {
    std::string s = read_line(prompt);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    return s;
}

// A number from 1 to `count` typed by the user; nullopt for anything else (back).
std::optional<std::size_t> pick(const std::string& prompt, std::size_t count) {
    const std::string s = ask(prompt);
    if (s.empty() || !std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); }) || s.size() > 4)
        return std::nullopt;
    const std::size_t n = static_cast<std::size_t>(std::stoul(s));
    if (n < 1 || n > count) return std::nullopt;
    return n - 1;
}

// One restore through the menu. Returns when done or when the user goes back.
void recovery_restore() {
    // 1. The backup.
    std::string location;
    while (location.empty()) {
        out("\nLooking for backups on the drives...\n");
        const std::vector<std::string> repos = find_repositories(drive_roots());
        for (std::size_t i = 0; i < repos.size(); ++i) out("  " + std::to_string(i + 1) + "  " + repos[i] + "\n");
        if (repos.empty()) out("  (none found; plug in the backup drive and choose L)\n");
        out("  P  Type the folder of the backup\n  N  Connect a network share\n  L  Look again\n  B  Back\n");
        const std::string c = ask("Your choice: ");
        if (c == "b" || c == "B") return;
        if (c == "l" || c == "L") continue;
        if (c == "p" || c == "P") {
            location = ask("Folder of the backup (for example D:\\Vaultik\\Backup): ");
            continue;
        }
        if (c == "n" || c == "N") {
            const std::string share = ask("Share (for example \\\\server\\backups): ");
            if (!share.starts_with("\\\\") || !safe_for_net(share)) {
                out("That is not a share path.\n");
                continue;
            }
            const std::string user = ask("User name on that computer (empty: none): ");
            if (!user.empty() && !safe_for_net(user)) {
                out("That user name has characters that are not allowed here.\n");
                continue;
            }
            // "net use" asks for the password itself, without showing it.
            const std::string cmd = "net use * \"" + share + "\"" +
                                    (user.empty() ? std::string() : " * /user:\"" + user + "\"") + " /persistent:no";
            if (interactive_command(cmd) != 0) {
                out("The share could not be connected.\n");
                continue;
            }
            const std::vector<std::string> on_share = find_repositories({share});
            if (on_share.size() == 1) location = on_share[0];
            else out("The share is connected; it is listed with the drives now.\n");
            continue;
        }
        if (auto n = [&]() -> std::optional<std::size_t> {
                if (c.empty() || !std::all_of(c.begin(), c.end(), [](unsigned char ch) { return std::isdigit(ch); }) || c.size() > 3)
                    return std::nullopt;
                const std::size_t k = std::stoul(c);
                return k >= 1 && k <= repos.size() ? std::optional<std::size_t>(k - 1) : std::nullopt;
            }())
            location = repos[*n];
    }

    // 2. The password.
    std::unique_ptr<Repository> repo;
    for (int tries = 0; !repo; ++tries) {
        std::string pw = read_password("Password of the backup: ");
        try {
            repo = Repository::open(open_backend(location, nullptr), pw);
        } catch (const AuthError&) {
            if (tries >= 2) {
                secure_wipe(pw.data(), pw.size());
                throw Error("the password was not right three times");
            }
            out("That password is not right; try again.\n");
        }
        secure_wipe(pw.data(), pw.size());
    }
    // A lock, when the backup can be written to (it may be on a read-only medium).
    std::unique_ptr<RepoLock> lock;
    try {
        lock = std::make_unique<RepoLock>(*repo, RepoLock::Mode::Shared, "recovery");
        repo->reload_index();
    } catch (const LockedError&) {
        throw;
    } catch (const Error& e) {
        warning(std::string("could not lock the backup (") + e.what() + "); going on without");
    }

    // 3. The version.
    std::vector<SnapshotRef> images;
    for (const SnapshotRef& s : load_all_snapshots(*repo))
        if (s.snapshot.image) images.push_back(s);
    std::reverse(images.begin(), images.end());  // newest first
    if (images.empty()) throw Error("this backup has no disk images (make one with a disk image job first)");
    out("\nDisk images in this backup (newest first):\n");
    std::vector<std::vector<image::DiskLayout>> layouts;
    for (std::size_t i = 0; i < images.size() && i < 30; ++i) {
        const Snapshot& s = images[i].snapshot;
        std::vector<image::DiskLayout> l;
        try {
            l = image::image_disks(*repo, s);
        } catch (const Error&) {
        }
        std::string what;
        for (const image::DiskLayout& d : l) what += (what.empty() ? "" : "; ") + layout_title(d);
        out("  " + std::to_string(i + 1) + "  " + os::format_time_local(s.time) + "  " + s.hostname + "  " + what +
            (s.note.empty() ? "" : "  \"" + s.note + "\"") + "\n");
        layouts.push_back(std::move(l));
    }
    const auto version = pick("Which one (Enter: back)? ", layouts.size());
    if (!version) return;
    const SnapshotRef& ref = images[*version];
    const std::vector<image::DiskLayout>& disks = layouts[*version];
    if (disks.empty()) throw Error("this disk image cannot be read");
    std::size_t index = 0;
    if (disks.size() > 1) {
        for (std::size_t i = 0; i < disks.size(); ++i) out("  " + std::to_string(i + 1) + "  " + layout_title(disks[i]) + "\n");
        const auto d = pick("Which disk of the image? ", disks.size());
        if (!d) return;
        index = *d;
    }
    const image::DiskLayout& layout = disks[index];
    out("\nThe image of " + layout_title(layout) + " holds:\n");
    for (const image::Partition& p : layout.partitions)
        if (!(layout.style == image::PartitionStyle::Mbr && image::is_container(p.mbr_type)))
            out("  " + image::describe(p) + "\n");

    // 4. The disk to restore onto.
    const std::vector<int> keep = protected_disks(location);
    const std::vector<int> running = image::system_disks();
    const std::vector<image::DiskInfo> targets = image::list_disks();
    // How far the partitions can shrink, read once when a disk is smaller than the image's.
    std::optional<std::vector<std::uint64_t>> limits;
    out("\nDisks of this computer:\n");
    for (const image::DiskInfo& d : targets) {
        std::string note;
        const std::string refuse = image::target_refusal(d, running, keep);
        std::string fit = image::fits(layout, d.size, d.sector_size);
        if (!fit.empty() && refuse.empty() && d.sector_size == layout.sector_size) {
            if (!limits) limits = image::shrink_limits(*repo, ref.snapshot, index);
            fit = image::fits(layout, d.size, d.sector_size, *limits);
            if (fit.empty()) note = "  (the NTFS partitions are shrunk to fit)";
        }
        if (!refuse.empty()) note = "  (not possible: " + refuse + ")";
        else if (!fit.empty()) note = "  (not possible: " + fit + ")";
        out("  " + image::describe(d) + note + "\n");
        const std::string vols = volumes_text(d);
        if (!vols.empty()) out("      holds " + vols + "\n");
    }
    const std::string which = ask("Number of the disk to restore onto (Enter: back): ");
    if (which.empty() || !std::all_of(which.begin(), which.end(), [](unsigned char c) { return std::isdigit(c); })) return;
    if (which.size() > 3) return;
    const int number = std::stoi(which);
    // The disk listed above under that number: the restore stops if the number is another
    // disk's by now.
    auto shown = std::find_if(targets.begin(), targets.end(), [number](const image::DiskInfo& d) { return d.number == number; });
    if (shown == targets.end()) throw Error("there is no disk " + which);

    Args none;  // the confirmation is typed at the console
    restore_onto_disk(*repo, ref, index, number, location, &none, false, image::identity_of(*shown));
    lock.reset();

    // 5. Starting Windows from it.
    const std::string boot = ask("Make the restored disk start Windows (recommended)? [Y/n]: ");
    if (boot.empty() || boot == "y" || boot == "Y") {
        try {
            status("Writing the boot files...");
            const std::string done = image::repair_boot(number);
            clear_status();
            out(done + "\n");
        } catch (const Error& e) {
            clear_status();
            out(std::string("The boot files could not be written: ") + e.what() +
                "\nThe disk may start anyway (its own boot files were restored with it).\n");
        }
    }
    out("\nDone. Remove the recovery drive and restart the computer.\n");
}

int cmd_recovery(const Args&) {
    for (;;) {
        out("\n  Vaultik recovery\n  ================\n"
            "  1  Restore a disk image onto this computer\n"
            "  2  List the disks\n"
            "  3  Open a command prompt (type exit to come back)\n"
            "  4  Restart\n"
            "  5  Shut down\n");
        std::string c;
        try {
            c = ask("Your choice: ");
        } catch (const Error&) {
            return kExitOk;  // no more input
        }
        try {
            if (c == "1") recovery_restore();
            else if (c == "2") cmd_image_disks(Args{});
            else if (c == "3") interactive_command("cmd.exe /k echo Type exit to return to Vaultik.");
            else if (c == "4") os::run_command({"wpeutil.exe", "reboot"});
            else if (c == "5") os::run_command({"wpeutil.exe", "shutdown"});
        } catch (const Cancelled&) {
            clear_status();
            out("Stopped.\n");
        } catch (const std::exception& e) {
            clear_status();
            out(std::string("\nThat did not work: ") + e.what() + "\n");
        }
    }
}
