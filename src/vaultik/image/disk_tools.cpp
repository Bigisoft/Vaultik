// The parts of disk_tools.hpp that do not talk to the system: the erase passes and their
// verification, the free space fill, the partition plan and the checks of what the user typed.

#include "vaultik/image/disk_tools.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>

#include "vaultik/crypto/crypto.hpp"
#include "vaultik/ops/backup.hpp"
#include "vaultik/platform/os.hpp"
#include "vaultik/util/hex.hpp"

namespace vaultik::image {

namespace {

constexpr std::uint64_t kMiB = 1ull << 20;
constexpr std::size_t kChunk = 4u << 20;  // bytes per write

std::string squeeze_upper(const std::string& s) {
    std::string out;
    for (const char c : s)
        if (!std::isspace(static_cast<unsigned char>(c))) out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    return out;
}

// A fast stream of random-looking bytes from a 256-bit seed (xoshiro256**): good enough to
// overwrite data with, and it can be played again from the seed for the verification, so the
// written data never has to be kept.
class PatternStream {
public:
    explicit PatternStream(const std::uint8_t (&seed)[32]) {
        std::memcpy(s_, seed, sizeof s_);
        if ((s_[0] | s_[1] | s_[2] | s_[3]) == 0) s_[0] = 1;  // the one state it cannot leave
    }
    void fill(std::uint8_t* dst, std::size_t n) {
        while (n >= 8) {
            const std::uint64_t v = next();
            std::memcpy(dst, &v, 8);
            dst += 8;
            n -= 8;
        }
        if (n > 0) {
            const std::uint64_t v = next();
            std::memcpy(dst, &v, n);
        }
    }

private:
    static std::uint64_t rotl(std::uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
    std::uint64_t next() {
        const std::uint64_t result = rotl(s_[1] * 5, 7) * 9;
        const std::uint64_t t = s_[1] << 17;
        s_[2] ^= s_[0];
        s_[3] ^= s_[1];
        s_[1] ^= s_[2];
        s_[0] ^= s_[3];
        s_[2] ^= t;
        s_[3] = rotl(s_[3], 45);
        return result;
    }
    std::uint64_t s_[4];
};

double seconds_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}

// Calls back at most about twice a second (and always for the first and last block).
class Ticker {
public:
    bool due(bool last) {
        const auto now = std::chrono::steady_clock::now();
        if (!last && started_ && now - last_ < std::chrono::milliseconds(500)) return false;
        started_ = true;
        last_ = now;
        return true;
    }

private:
    bool started_ = false;
    std::chrono::steady_clock::time_point last_;
};

std::size_t utf16_units(const std::string& s) {
    std::size_t n = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const auto c = static_cast<unsigned char>(s[i]);
        if ((c & 0xC0) == 0x80) continue;  // continuation byte
        n += c >= 0xF0 ? 2 : 1;            // four-byte sequences are a surrogate pair
    }
    return n;
}

}  // namespace

// ---- names and confirmations ----------------------------------------------------------------

std::string erase_text(const DiskInfo& d) {
#ifdef _WIN32
    return confirmation_text(d.number);
#else
    std::string name = d.path;
    if (name.starts_with("/dev/")) name = name.substr(5);
    return "ERASE DISK " + (name.empty() ? std::to_string(d.number) : name);
#endif
}

std::string erase_volume_text(const std::string& volume) {
    std::string name = volume;
    if (name.starts_with("/dev/")) name = name.substr(5);
    while (name.size() > 2 && (name.back() == '\\' || name.back() == '/')) name.pop_back();
    return "ERASE " + name;
}

bool typed_matches(const std::string& typed, const std::string& want) {
    return !want.empty() && squeeze_upper(typed) == squeeze_upper(want);
}

bool solid_state(const DiskInfo& d) {
    return d.media == "SSD" || d.bus == "NVMe" || d.bus == "SD" || d.bus == "MMC";
}

const DiskInfo& find_tool_disk(const std::vector<DiskInfo>& disks, const std::string& arg) {
    if (!arg.empty() && std::all_of(arg.begin(), arg.end(), [](unsigned char c) { return std::isdigit(c); })) {
        const int n = std::stoi(arg);
        for (const DiskInfo& d : disks)
            if (d.number == n) return d;
        throw Error("there is no disk " + arg + " (see vaultik disk list)");
    }
    for (const DiskInfo& d : disks)
        if (!d.path.empty() && (d.path == arg || d.path == "/dev/" + arg)) return d;
    const int n = find_disk(disks, arg);
    for (const DiskInfo& d : disks)
        if (n >= 0 && d.number == n) return d;
    throw Error("disk " + arg + " is not connected (see vaultik disk list)");
}

std::string tool_refusal(const DiskInfo& d, const std::vector<int>& running_from, const std::vector<int>& protected_disks) {
    std::string why = target_refusal(d, running_from, protected_disks);
    if (!why.empty()) {
#ifdef _WIN32
        if (why.starts_with("Windows is running")) why = "Windows is running or starts from this disk";
#else
        if (why.starts_with("Windows is running")) why = "the system is running from this disk";
#endif
        if (why.starts_with("this disk holds the backup")) why = "Vaultik is running from this disk";
        return why;
    }
    if (d.dynamic)
        return "it is a dynamic disk or part of a Storage Spaces pool; remove its volumes (or the disk from the "
               "pool) in Disk Management or Storage Spaces first";
    if (d.offline)
        return "Windows keeps this disk offline or read-only; bring it online in Disk Management first (right-click "
               "the disk, Online)";
    for (const DiskVolume& v : d.volumes) {
        if (v.spans)
            return "it holds a part of a volume that lies on several disks" +
                   (v.mount.empty() ? std::string() : " (" + v.mount + ")") + "; remove that volume first";
#ifdef _WIN32
        const std::string use = windows_use_of(v);
        if (!use.empty()) return use;
#else
        if (v.filesystem == "swap") return v.name + " is in use as swap";
        if (v.filesystem == "holder") return v.name + " is in use (LVM, RAID or encryption)";
        if (!v.mount.empty()) return v.name + " is mounted on " + v.mount + "; unmount it first";
#endif
    }
    return {};
}

bool pool_partition(const Partition& p, bool gpt) {
    if (!gpt) return p.mbr_type == 0x42;  // a dynamic disk's (LDM) partition
    static constexpr std::string_view kPoolTypes[] = {
        "5808C8AA-7E8F-42E0-85D2-E1E90434CFB3",  // LDM metadata (a dynamic disk)
        "AF9B60A0-1431-4F62-BC68-3311714A69AD",  // LDM data
        "E75CAF8F-F680-4CEE-AFA3-B001E56EFC2D",  // Storage Spaces
    };
    return std::any_of(std::begin(kPoolTypes), std::end(kPoolTypes), [&](std::string_view t) { return squeeze_upper(p.type) == t; });
}

std::string partition_refusal(const Partition& p, bool gpt) {
    if (pool_partition(p, gpt)) return "it is part of a dynamic disk or of a Storage Spaces pool";
    if (gpt) {
        const std::string type = squeeze_upper(p.type);
        if (type == kEfiSystemType) return "it is an EFI system partition, which a computer starts from";
        if (type == kMsrType) return "it is the Microsoft reserved partition";
        if (type == kRecoveryType) return "it is a recovery partition";
        if ((p.attributes & 1) != 0) return "the computer's firmware needs this partition";  // GPT_ATTRIBUTE_PLATFORM_REQUIRED
        return {};
    }
    if (p.mbr_type == 0xEF) return "it is an EFI system partition, which a computer starts from";
    if (p.mbr_type == 0x27) return "it is a recovery partition";
    if (p.active) return "it is the active partition, which a BIOS computer starts from; erase the whole disk instead";
    return {};
}

DiskIdentity volume_identity(const VolumeTarget& v) {
    return {v.serial, v.size, v.id};
}

std::string volume_mismatch(const VolumeTarget& now, const DiskIdentity& expected) {
    const std::string name = now.name.empty() ? std::string("the drive") : now.name;
    const std::string again = "; list the drives again";
    if (now.size != expected.size) return name + " is not the drive that was chosen any more (its size differs)" + again;
    if (squeeze_upper(now.id) != squeeze_upper(expected.id))
        return name + " is not the drive that was chosen any more (its id differs)" + again;
    if (squeeze_upper(now.serial) != squeeze_upper(expected.serial))
        return name + " is not the drive that was chosen any more (its serial number differs)" + again;
    return {};
}

// ---- erasing ----------------------------------------------------------------------------------

std::optional<WipeMethod> parse_wipe_method(const std::string& text) {
    std::string t = text;
    std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (t == "zeros" || t == "zero") return WipeMethod::Zeros;
    if (t == "random") return WipeMethod::Random;
    if (t == "dod3" || t == "dod") return WipeMethod::Dod3;
    return std::nullopt;
}

const char* to_string(WipeMethod m) {
    switch (m) {
        case WipeMethod::Zeros: return "zeros";
        case WipeMethod::Random: return "random";
        case WipeMethod::Dod3: return "dod3";
    }
    return "zeros";
}

std::vector<bool> wipe_passes(WipeMethod m) {
    switch (m) {
        case WipeMethod::Zeros: return {false};
        case WipeMethod::Random: return {true};
        case WipeMethod::Dod3: return {true, true, false};
    }
    return {false};
}

WipeResult wipe_device(WipeDevice& device, const WipeOptions& o) {
    const std::uint64_t size = device.size();
    const std::uint32_t block = std::max<std::uint32_t>(1, device.block_size());
    if (size == 0) throw Error("there is nothing to erase (the size is 0)");
    if (size % block != 0) throw Error("the size is not a whole number of sectors");
    const std::size_t chunk = std::max<std::size_t>(block, kChunk / block * block);
    const std::vector<bool> passes = wipe_passes(o.method);
    const std::uint64_t job = size * passes.size() + (o.verify ? size : 0);

    Bytes buf(chunk);
    Bytes back(o.verify ? chunk : 0);
    std::uint8_t seed[32] = {};
    const auto start = std::chrono::steady_clock::now();
    std::uint64_t job_done = 0;
    Ticker ticker;
    WipeProgress p;
    p.passes = static_cast<int>(passes.size());
    p.total = size;
    const auto report = [&](bool last) {
        if (!o.on_progress || !ticker.due(last)) return;
        const double secs = seconds_since(start);
        p.bytes_per_second = secs > 0.2 ? static_cast<double>(job_done) / secs : 0;
        p.seconds_left = p.bytes_per_second > 0 ? static_cast<double>(job - job_done) / p.bytes_per_second : -1;
        o.on_progress(p);
    };
    const auto cancelled = [&] { return o.should_cancel && o.should_cancel(); };

    WipeResult result;
    for (std::size_t i = 0; i < passes.size(); ++i) {
        const bool random = passes[i];
        if (random) random_bytes(seed);
        PatternStream stream(seed);
        if (!random) std::fill(buf.begin(), buf.end(), std::uint8_t{0});
        p.pass = static_cast<int>(i) + 1;
        p.verifying = false;
        p.done = 0;
        for (std::uint64_t at = 0; at < size;) {
            if (cancelled()) throw Cancelled();
            const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(chunk, size - at));
            if (random) stream.fill(buf.data(), n);
            device.write_at(at, ByteSpan(buf.data(), n));
            at += n;
            p.done = at;
            job_done += n;
            result.bytes += n;
            report(at == size);
        }
        device.flush();
    }
    if (o.verify) {
        // The last pass again, compared with what the device returns.
        const bool random = passes.back();
        PatternStream stream(seed);
        if (!random) std::fill(buf.begin(), buf.end(), std::uint8_t{0});
        p.verifying = true;
        p.done = 0;
        for (std::uint64_t at = 0; at < size;) {
            if (cancelled()) throw Cancelled();
            const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(chunk, size - at));
            if (random) stream.fill(buf.data(), n);
            device.read_at(at, back.data(), n);
            if (std::memcmp(buf.data(), back.data(), n) != 0) {
                std::size_t k = 0;
                while (k < n && buf[k] == back[k]) ++k;
                throw Error("the verification found other data than was written at byte " + std::to_string(at + k) +
                            "; the drive may be failing or did not write everything");
            }
            at += n;
            p.done = at;
            job_done += n;
            report(at == size);
        }
        result.verified = true;
    }
    secure_wipe(seed, sizeof seed);
    if (o.clear_partition_table && passes.back()) {
        // No partition table left at either end (GPT keeps a copy in the last sectors).
        std::fill(buf.begin(), buf.end(), std::uint8_t{0});
        const std::uint64_t edge = std::min<std::uint64_t>(size, kMiB / block * block == 0 ? block : kMiB / block * block);
        device.write_at(0, ByteSpan(buf.data(), static_cast<std::size_t>(edge)));
        device.write_at(size - edge, ByteSpan(buf.data(), static_cast<std::size_t>(edge)));
        device.flush();
    }
    result.seconds = seconds_since(start);
    return result;
}

// ---- wiping free space ------------------------------------------------------------------------

namespace {

// Whether `name` is a fill file of wipe_free_space: "vaultik-free-space-<12 hex digits>-<n>.tmp".
bool fill_file_name(const std::string& name) {
    const std::string prefix = kFillFilePrefix;
    if (!name.starts_with(prefix) || !name.ends_with(".tmp")) return false;
    const std::string rest = name.substr(prefix.size(), name.size() - prefix.size() - 4);
    if (rest.size() < 14 || rest[12] != '-') return false;
    for (std::size_t i = 0; i < rest.size(); ++i) {
        const auto c = static_cast<unsigned char>(rest[i]);
        if (i == 12) continue;
        if (i < 12 ? !std::isxdigit(c) : !std::isdigit(c)) return false;
    }
    return true;
}

// Fill files an earlier run left behind when it was killed or the power went (they are removed
// when closed, but a file system may keep one after a power loss).
void remove_stale_fill_files(const std::string& dir) {
    try {
        os::DirReader reader(dir);
        std::string name;
        os::FileInfo info;
        std::vector<std::string> stale;
        while (reader.next(name, info))
            if (info.type == os::EntryType::File && fill_file_name(name)) stale.push_back(name);
        for (const std::string& n : stale) os::remove_file(os::join(dir, n));
    } catch (const std::exception&) {
        // not readable: nothing to remove
    }
}

constexpr std::uint64_t kCompressionCheck = 64 * kMiB;  // how often the fill file's size on the drive is looked at

}  // namespace

bool fill_compressed(std::uint64_t written, std::uint64_t allocated) {
    // A file that takes less than half of what was written is kept compressed (or
    // deduplicated); the slack is for the file system's own delays.
    return written >= kCompressionCheck && allocated < written / 2;
}

std::uint64_t wipe_free_space(const std::string& folder, const FreeSpaceOptions& o) {
    const std::string dir = os::absolute_path(folder);
    if (!os::is_directory(dir)) throw Error(folder + " is not a folder");
    remove_stale_fill_files(dir);
    const std::optional<os::DiskSpace> space = os::disk_space(dir);
    const std::uint64_t free = space ? space->free : 0;
    std::uint8_t rnd[6];
    random_bytes(rnd);
    const std::string base = os::join(dir, kFillFilePrefix + to_hex(rnd));
    const std::vector<bool> passes = wipe_passes(o.method);
    // Set when the drive turns out to compress the fill file: zeros would then take no space, so
    // every pass writes random data.
    bool compressing = false;
    const auto start = std::chrono::steady_clock::now();
    const auto cancelled = [&] { return o.should_cancel && o.should_cancel(); };

    // The first pass fills the drive with as many files as it takes (one, unless the file system
    // limits a file's size); later passes write the same files again.
    struct Piece {
        std::string path;
        std::unique_ptr<FillFile> file;
        std::uint64_t length = 0;
    };
    std::vector<Piece> pieces;
    std::uint64_t written = 0;
    Ticker ticker;
    FreeSpaceProgress p;
    p.total = (o.limit != 0 ? std::min(free, o.limit) : free) * passes.size();
    const auto report = [&] {
        if (!o.on_progress || !ticker.due(false)) return;
        const double secs = seconds_since(start);
        p.done = std::min(written, p.total);
        p.bytes_per_second = secs > 0.2 ? static_cast<double>(written) / secs : 0;
        p.seconds_left = p.bytes_per_second > 0 && p.total > p.done
                             ? static_cast<double>(p.total - p.done) / p.bytes_per_second
                             : -1;
        o.on_progress(p);
    };
    const auto remove_all = [&] {
        for (Piece& piece : pieces) {
            piece.file.reset();
            os::remove_file(piece.path);
        }
    };
    try {
        Bytes buf(kChunk);
        std::uint8_t seed[32];
        for (std::size_t i = 0; i < passes.size(); ++i) {
            bool random = passes[i] || compressing;
            random_bytes(seed);
            PatternStream stream(seed);
            if (!random) std::fill(buf.begin(), buf.end(), std::uint8_t{0});
            if (i == 0) {
                bool full = false;
                while (!full) {
                    Piece piece;
                    piece.path = base + "-" + std::to_string(pieces.size() + 1) + ".tmp";
                    piece.file = create_fill_file(piece.path);
                    pieces.push_back(std::move(piece));
                    Piece& cur = pieces.back();
                    std::size_t size = kChunk;
                    std::uint64_t next_check = kCompressionCheck;
                    std::uint64_t from_length = 0;  // where the check starts (again after a switch to random data)
                    std::uint64_t from_allocated = 0;
                    for (;;) {
                        if (cancelled()) throw Cancelled();
                        if (o.limit != 0) size = static_cast<std::size_t>(std::min<std::uint64_t>(size, o.limit - std::min(o.limit, written)));
                        if (size == 0) {
                            full = true;
                            break;
                        }
                        if (random) stream.fill(buf.data(), size);
                        const FillResult r = cur.file->write_at(cur.length, ByteSpan(buf.data(), size));
                        if (r == FillResult::Written) {
                            cur.length += size;
                            written += size;
                            report();
                            if (cur.length >= next_check) {
                                next_check = cur.length + kCompressionCheck;
                                cur.file->flush();
                                const std::optional<std::uint64_t> took = cur.file->allocated();
                                const std::uint64_t grew = took && *took > from_allocated ? *took - from_allocated : 0;
                                if (took && fill_compressed(cur.length - from_length, grew)) {
                                    // Zeros take no room on this drive: random data from here on.
                                    // Random data kept that small cannot fill it at all.
                                    if (random)
                                        throw Error("the drive keeps the written data compressed (or deduplicated), so "
                                                    "its free space cannot be filled; nothing on it was changed");
                                    random = compressing = true;
                                    from_length = cur.length;
                                    from_allocated = *took;
                                }
                            }
                            continue;
                        }
                        // Smaller pieces fill what is left, down to one sector.
                        if (size > 4096) {
                            size /= 16;
                            continue;
                        }
                        full = r == FillResult::Full || cur.length == 0;
                        break;
                    }
                    cur.file->flush();  // on the drive before it is deleted or written again
                }
                continue;
            }
            for (Piece& piece : pieces) {
                for (std::uint64_t at = 0; at < piece.length;) {
                    if (cancelled()) throw Cancelled();
                    const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(kChunk, piece.length - at));
                    if (random) stream.fill(buf.data(), n);
                    if (piece.file->write_at(at, ByteSpan(buf.data(), n)) != FillResult::Written)
                        throw Error("the drive filled up while its free space was written again");
                    at += n;
                    written += n;
                    report();
                }
                piece.file->flush();
            }
        }
        secure_wipe(seed, sizeof seed);
    } catch (...) {
        remove_all();
        throw;
    }
    remove_all();
    if (o.on_progress) {
        p.done = p.total = written;
        p.seconds_left = 0;
        o.on_progress(p);
    }
    return written;
}

// ---- setting up a new disk ---------------------------------------------------------------------

std::vector<NewPartition> plan_partitions(std::uint64_t disk_size, std::uint32_t sector_size, TableStyle style,
                                          const std::vector<std::uint64_t>& sizes_in, bool with_msr) {
    if (sector_size == 0) sector_size = 512;
    const std::vector<std::uint64_t> sizes = sizes_in.empty() ? std::vector<std::uint64_t>{0} : sizes_in;
    const bool gpt = style == TableStyle::Gpt;
    if (!gpt && sizes.size() > 4) throw Error("an MBR disk holds at most 4 partitions here; use GPT for more");
    if (gpt && sizes.size() > 127) throw Error("a GPT disk holds at most 128 partitions");
    if (!gpt && disk_size / sector_size > 0xFFFFFFFFull)
        throw Error("an MBR partition table cannot use more than 2 TiB of this disk; use GPT");
    for (std::size_t i = 0; i + 1 < sizes.size(); ++i)
        if (sizes[i] == 0) throw Error("only the last partition can take the rest of the disk");
    const std::uint64_t end = disk_size / sector_size * sector_size;
    // GPT keeps a copy of its table in the last 33 sectors: the last MiB stays free.
    const std::uint64_t usable_end = gpt ? (end > kMiB ? end - kMiB : 0) : end;
    std::vector<NewPartition> out;
    std::uint64_t at = kMiB;
    if (gpt && with_msr) {
        out.push_back({at, 16 * kMiB, true});
        at += 16 * kMiB;
    }
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        std::uint64_t length = 0;
        if (sizes[i] == 0) {
            length = usable_end > at ? (usable_end - at) / sector_size * sector_size : 0;
        } else if (sizes[i] <= usable_end && sizes[i] <= ~0ull - kMiB) {
            length = (sizes[i] + kMiB - 1) / kMiB * kMiB;
        } else {
            length = ~0ull;  // larger than any disk (and not rounded, which would wrap around)
        }
        // Compared without adding, so a huge size cannot wrap around to a small end.
        if (length < kMiB || at > usable_end || length > usable_end - at)
            throw Error(sizes.size() == 1 && sizes[0] == 0 ? "the disk is too small for a partition"
                                                           : "the partitions do not fit on the disk");
        out.push_back({at, length, false});
        at += length;
        at = (at + kMiB - 1) / kMiB * kMiB;
    }
    return out;
}

std::string sfdisk_script(TableStyle style, const std::vector<NewPartition>& parts, std::uint32_t sector_size,
                          const std::string& filesystem) {
    const bool gpt = style == TableStyle::Gpt;
    const bool linux_fs = filesystem == "ext4";
    std::string s = std::string("label: ") + (gpt ? "gpt" : "dos") + "\nunit: sectors\n\n";
    for (const NewPartition& p : parts) {
        std::string type;
        if (p.reserved) type = "E3C9E316-0B5C-4DB8-817D-F92DF00215AE";
        else if (gpt) type = linux_fs ? "0FC63DAF-8483-4772-8E79-3D69D8477DE4" : "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7";
        else type = linux_fs ? "83" : "7";
        s += "start=" + std::to_string(p.offset / sector_size) + ", size=" + std::to_string(p.length / sector_size) +
             ", type=" + type + "\n";
    }
    return s;
}

std::string partition_device(const std::string& disk, int n) {
    const bool digit = !disk.empty() && std::isdigit(static_cast<unsigned char>(disk.back()));
    return disk + (digit ? "p" : "") + std::to_string(n);
}

void check_init_options(const NewDiskOptions& o) {
    std::string fs = o.filesystem;
#ifdef _WIN32
    if (fs.empty()) fs = "ntfs";
    if (fs != "ntfs" && fs != "exfat") throw Error("Windows can format a new disk as ntfs or exfat (not " + fs + ")");
    if (o.letter != 0) {
        const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(o.letter)));
        if (c < 'D' || c > 'Z') throw Error("the drive letter must be one of D to Z");
        if (os::exists(std::string(1, c) + ":\\")) throw Error(std::string(1, c) + ": is in use already");
    }
#else
    if (fs.empty()) fs = "ext4";
    if (fs != "ntfs" && fs != "exfat" && fs != "ext4") throw Error("the file system must be ext4, ntfs or exfat");
    if (o.letter != 0) throw Error("drive letters are a Windows thing; give --mount <folder> instead");
    if (!o.mount.empty() && !o.mount.starts_with("/")) throw Error("--mount needs an absolute folder");
#endif
    const std::size_t limit = fs == "ext4" ? 16 : fs == "exfat" ? 11 : 32;
    const std::size_t length = fs == "ext4" ? o.label.size() : utf16_units(o.label);
    if (length > limit)
        throw Error("a " + fs + " label can be " + std::to_string(limit) + (fs == "ext4" ? " bytes" : " characters") +
                    " long at most");
    for (const char c : o.label) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7F || c == '"') throw Error("the label holds a character that cannot be used");
        if (fs == "exfat" && std::strchr("*?/\\|,;:+=[]<>", c) != nullptr)
            throw Error(std::string("an exFAT label cannot hold ") + c);
    }
}

}  // namespace vaultik::image
