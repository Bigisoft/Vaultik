#include "vaultik/image/layout.hpp"

#include <algorithm>
#include <cstdio>

#include <nlohmann/json.hpp>

#include "vaultik/crypto/crypto.hpp"

namespace vaultik::image {

namespace {

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Table-driven reflected CRC-32 for a polynomial.
struct CrcTable {
    std::uint32_t t[256];
    explicit CrcTable(std::uint32_t poly) {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (c >> 1) ^ poly : c >> 1;
            t[i] = c;
        }
    }
    std::uint32_t run(ByteSpan data, std::uint32_t crc) const {
        crc = ~crc;
        for (const std::uint8_t b : data) crc = t[(crc ^ b) & 0xFF] ^ (crc >> 8);
        return ~crc;
    }
};

void put_u16(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
}
void put_u32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}
void put_u64(std::uint8_t* p, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}

// UTF-8 to UTF-16 code units (invalid bytes become U+FFFD).
std::vector<std::uint16_t> utf16(std::string_view s) {
    std::vector<std::uint16_t> out;
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        std::uint32_t cp = 0xFFFD;
        std::size_t n = 1;
        if (c < 0x80) {
            cp = c;
        } else if ((c >> 5) == 6 && i + 1 < s.size()) {
            cp = ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3Fu);
            n = 2;
        } else if ((c >> 4) == 14 && i + 2 < s.size()) {
            cp = ((c & 0x0Fu) << 12) | ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 6) |
                 (static_cast<unsigned char>(s[i + 2]) & 0x3Fu);
            n = 3;
        } else if ((c >> 3) == 30 && i + 3 < s.size()) {
            cp = ((c & 0x07u) << 18) | ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 12) |
                 ((static_cast<unsigned char>(s[i + 2]) & 0x3Fu) << 6) | (static_cast<unsigned char>(s[i + 3]) & 0x3Fu);
            n = 4;
        }
        i += n;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<std::uint16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<std::uint16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<std::uint16_t>(cp));
        }
    }
    return out;
}

const char* style_name(PartitionStyle s) {
    return s == PartitionStyle::Gpt ? "gpt" : "mbr";
}

const char* method_name(ReadMethod m) {
    return m == ReadMethod::Used ? "used" : "raw";
}

std::string size_text(std::uint64_t bytes) {
    char buf[32];
    const double gb = static_cast<double>(bytes) / 1e9;
    if (gb >= 10) std::snprintf(buf, sizeof buf, "%.0f GB", gb);
    else if (gb >= 1) std::snprintf(buf, sizeof buf, "%.1f GB", gb);
    else std::snprintf(buf, sizeof buf, "%.0f MB", static_cast<double>(bytes) / 1e6);
    return buf;
}

}  // namespace

Guid parse_guid(std::string_view text) {
    if (text.size() == 38 && text.front() == '{' && text.back() == '}') text = text.substr(1, 36);
    if (text.size() != 36 || text[8] != '-' || text[13] != '-' || text[18] != '-' || text[23] != '-')
        throw Error("not a GUID: " + std::string(text));
    std::uint8_t b[16];
    std::size_t k = 0;
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] == '-') {
            ++i;
            continue;
        }
        const int hi = hex_value(text[i]);
        const int lo = hex_value(text[i + 1]);
        if (hi < 0 || lo < 0) throw Error("not a GUID: " + std::string(text));
        b[k++] = static_cast<std::uint8_t>((hi << 4) | lo);
        i += 2;
    }
    // Text order is big-endian per field; the first three fields are stored little-endian.
    return Guid{b[3], b[2], b[1], b[0], b[5], b[4], b[7], b[6], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]};
}

std::string format_guid(const Guid& g) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X", g[3], g[2],
                  g[1], g[0], g[5], g[4], g[7], g[6], g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
    return buf;
}

Guid random_guid() {
    Guid g{};
    random_bytes(g);
    g[7] = static_cast<std::uint8_t>((g[7] & 0x0F) | 0x40);  // version 4 (Data3's high byte, stored second)
    g[8] = static_cast<std::uint8_t>((g[8] & 0x3F) | 0x80);  // RFC 4122 variant
    return g;
}

bool is_zero(const Guid& g) {
    return std::all_of(g.begin(), g.end(), [](std::uint8_t b) { return b == 0; });
}

std::uint32_t crc32(ByteSpan data, std::uint32_t crc) {
    static const CrcTable table(0xEDB88320u);
    return table.run(data, crc);
}

std::uint32_t crc32c(ByteSpan data, std::uint32_t crc) {
    static const CrcTable table(0x82F63B78u);
    return table.run(data, crc);
}

bool is_container(std::uint8_t mbr_type) {
    return mbr_type == 0x05 || mbr_type == 0x0F || mbr_type == 0x85;
}

std::string disk_id(const DiskLayout& layout) {
    if (layout.style == PartitionStyle::Gpt) return "gpt-" + layout.disk_guid;
    char buf[16];
    std::snprintf(buf, sizeof buf, "%08X", layout.signature);
    return std::string("mbr-") + buf;
}

std::string to_json(const DiskLayout& l) {
    nlohmann::json parts = nlohmann::json::array();
    for (const Partition& p : l.partitions) {
        nlohmann::json holes = nlohmann::json::array();
        for (const auto& [off, len] : p.holes) holes.push_back({off, len});
        parts.push_back({{"number", p.number},
                         {"offset", p.offset},
                         {"length", p.length},
                         {"type", p.type},
                         {"guid", p.guid},
                         {"name", p.name},
                         {"attributes", p.attributes},
                         {"mbr_type", p.mbr_type},
                         {"active", p.active},
                         {"filesystem", p.filesystem},
                         {"label", p.label},
                         {"mount", p.mount},
                         {"serial", p.serial},
                         {"cluster_size", p.cluster_size},
                         {"windows", p.windows},
                         {"method", method_name(p.method)},
                         {"shadow_copy", p.shadow_copy},
                         {"used_bytes", p.used_bytes},
                         {"holes", holes}});
    }
    nlohmann::json regions = nlohmann::json::array();
    for (const Region& r : l.regions) regions.push_back({{"offset", r.offset}, {"length", r.length}});
    const nlohmann::json j = {{"version", l.version},
                              {"style", style_name(l.style)},
                              {"sector_size", l.sector_size},
                              {"physical_sector_size", l.physical_sector_size},
                              {"size", l.size},
                              {"disk_guid", l.disk_guid},
                              {"signature", l.signature},
                              {"gpt_entries", l.gpt_entries},
                              {"number", l.number},
                              {"model", l.model},
                              {"serial", l.serial},
                              {"system", l.system},
                              {"id", l.id},
                              {"partitions", parts},
                              {"regions", regions}};
    return j.dump(1, ' ', false, nlohmann::json::error_handler_t::replace);
}

DiskLayout layout_from_json(std::string_view text) {
    DiskLayout l;
    try {
        const nlohmann::json j = nlohmann::json::parse(text);
        l.version = j.at("version").get<int>();
        if (l.version != 1) throw Error("this disk image was made by a newer Vaultik (layout version " +
                                        std::to_string(l.version) + ")");
        const std::string style = j.at("style").get<std::string>();
        if (style == "gpt") l.style = PartitionStyle::Gpt;
        else if (style == "mbr") l.style = PartitionStyle::Mbr;
        else throw Error("unknown partition style " + style);
        l.sector_size = j.at("sector_size").get<std::uint32_t>();
        l.physical_sector_size = j.value("physical_sector_size", l.sector_size);
        l.size = j.at("size").get<std::uint64_t>();
        l.disk_guid = j.value("disk_guid", std::string());
        l.signature = j.value("signature", 0u);
        l.gpt_entries = j.value("gpt_entries", 128u);
        l.number = j.value("number", -1);
        l.model = j.value("model", std::string());
        l.serial = j.value("serial", std::string());
        l.system = j.value("system", false);
        l.id = j.value("id", std::string());
        for (const auto& pj : j.at("partitions")) {
            Partition p;
            p.number = pj.value("number", 0);
            p.offset = pj.at("offset").get<std::uint64_t>();
            p.length = pj.at("length").get<std::uint64_t>();
            p.type = pj.value("type", std::string());
            p.guid = pj.value("guid", std::string());
            p.name = pj.value("name", std::string());
            p.attributes = pj.value("attributes", std::uint64_t{0});
            p.mbr_type = pj.value("mbr_type", std::uint8_t{0});
            p.active = pj.value("active", false);
            p.filesystem = pj.value("filesystem", std::string());
            p.label = pj.value("label", std::string());
            p.mount = pj.value("mount", std::string());
            p.serial = pj.value("serial", 0u);
            p.cluster_size = pj.value("cluster_size", 0u);
            p.windows = pj.value("windows", false);
            const std::string m = pj.value("method", std::string("raw"));
            if (m == "used") p.method = ReadMethod::Used;
            else if (m == "raw") p.method = ReadMethod::Raw;
            else throw Error("unknown read method " + m);
            p.shadow_copy = pj.value("shadow_copy", false);
            p.used_bytes = pj.value("used_bytes", std::uint64_t{0});
            for (const auto& h : pj.value("holes", nlohmann::json::array())) {
                const std::uint64_t off = h.at(0).get<std::uint64_t>();
                const std::uint64_t len = h.at(1).get<std::uint64_t>();
                if (off > p.length || len > p.length - off) throw Error("a stored range lies outside its partition");
                p.holes.emplace_back(off, len);
            }
            l.partitions.push_back(std::move(p));
        }
        for (const auto& rj : j.value("regions", nlohmann::json::array()))
            l.regions.push_back({rj.at("offset").get<std::uint64_t>(), rj.at("length").get<std::uint64_t>()});
    } catch (const nlohmann::json::exception& e) {
        throw Error(std::string("the disk layout cannot be read: ") + e.what());
    }
    if (l.sector_size != 512 && l.sector_size != 4096) throw Error("unsupported sector size in the disk layout");
    if (l.gpt_entries == 0 || l.gpt_entries > 1024) throw Error("unsupported GPT size in the disk layout");
    for (const Partition& p : l.partitions)
        if (p.offset % l.sector_size != 0 || p.length % l.sector_size != 0 || p.offset > l.size ||
            p.length > l.size - p.offset)
            throw Error("a partition in the disk layout is out of range");
    for (const Region& r : l.regions)
        if (r.offset > l.size || r.length > l.size - r.offset || r.length > (16u << 20))
            throw Error("a stored region in the disk layout is out of range");
    return l;
}

std::vector<Region> gap_regions(const DiskLayout& layout, std::uint64_t cap) {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> used;
    for (const Partition& p : layout.partitions)
        if (!(layout.style == PartitionStyle::Mbr && is_container(p.mbr_type)) && p.length > 0)
            used.emplace_back(p.offset, p.offset + p.length);
    std::sort(used.begin(), used.end());
    std::vector<Region> out;
    std::uint64_t pos = 0;
    auto gap = [&](std::uint64_t from, std::uint64_t to) {
        if (to > from) out.push_back({from, std::min(to - from, cap)});
    };
    for (const auto& [b, e] : used) {
        gap(pos, b);
        pos = std::max(pos, e);
    }
    gap(pos, layout.size);
    return out;
}

std::uint64_t used_end(const DiskLayout& layout) {
    std::uint64_t end = 0;
    for (const Partition& p : layout.partitions) end = std::max(end, p.offset + p.length);
    return end;
}

std::string fits(const DiskLayout& layout, std::uint64_t target_size, std::uint32_t sector_size) {
    if (sector_size != layout.sector_size)
        return "the disk uses " + std::to_string(sector_size) + "-byte sectors and the image " +
               std::to_string(layout.sector_size) + "-byte ones";
    std::uint64_t need = used_end(layout);
    if (layout.style == PartitionStyle::Gpt) {
        const std::uint64_t array = (std::uint64_t{layout.gpt_entries} * 128 + sector_size - 1) / sector_size;
        need += (1 + array) * sector_size;  // the backup GPT after the last partition
    }
    if (need > target_size)
        return "the disk is too small: it has " + size_text(target_size) + " and the partitions need " + size_text(need);
    return {};
}

std::vector<DiskWrite> partition_tables(const DiskLayout& layout, std::uint64_t target_size) {
    std::vector<DiskWrite> out;
    if (layout.style == PartitionStyle::Mbr) return out;
    const std::string why = fits(layout, target_size, layout.sector_size);
    if (!why.empty()) throw Error(why);

    const std::uint64_t ss = layout.sector_size;
    const std::uint64_t sectors = target_size / ss;
    const std::uint32_t entries = layout.gpt_entries;
    const std::uint64_t array_bytes = std::uint64_t{entries} * 128;
    const std::uint64_t array_sectors = (array_bytes + ss - 1) / ss;
    const std::uint64_t first_usable = 2 + array_sectors;
    const std::uint64_t last_usable = sectors - 2 - array_sectors;

    // The partition array, in the order of the layout.
    Bytes array(array_sectors * ss, 0);
    std::size_t k = 0;
    for (const Partition& p : layout.partitions) {
        if (k >= entries) throw Error("too many partitions for the GPT");
        const std::uint64_t first = p.offset / ss;
        const std::uint64_t last = (p.offset + p.length) / ss - 1;
        if (p.length == 0 || first < first_usable || last > last_usable)
            throw Error("partition " + std::to_string(p.number) + " does not fit inside the GPT's usable area");
        std::uint8_t* e = array.data() + k * 128;
        const Guid type = parse_guid(p.type);
        const Guid uid = parse_guid(p.guid);
        std::copy(type.begin(), type.end(), e);
        std::copy(uid.begin(), uid.end(), e + 16);
        put_u64(e + 32, first);
        put_u64(e + 40, last);
        put_u64(e + 48, p.attributes);
        const std::vector<std::uint16_t> name = utf16(p.name);
        for (std::size_t i = 0; i < name.size() && i < 36; ++i) put_u16(e + 56 + 2 * i, name[i]);
        ++k;
    }
    const std::uint32_t array_crc = crc32(ByteSpan(array.data(), array_bytes));
    const Guid disk = parse_guid(layout.disk_guid);

    auto header = [&](std::uint64_t my_lba, std::uint64_t alt_lba, std::uint64_t entries_lba) {
        Bytes h(ss, 0);
        std::copy_n("EFI PART", 8, h.begin());
        put_u32(&h[8], 0x00010000);
        put_u32(&h[12], 92);
        put_u64(&h[24], my_lba);
        put_u64(&h[32], alt_lba);
        put_u64(&h[40], first_usable);
        put_u64(&h[48], last_usable);
        std::copy(disk.begin(), disk.end(), h.begin() + 56);
        put_u64(&h[72], entries_lba);
        put_u32(&h[80], entries);
        put_u32(&h[84], 128);
        put_u32(&h[88], array_crc);
        put_u32(&h[16], crc32(ByteSpan(h.data(), 92)));
        return h;
    };

    // Protective MBR + primary header + primary array, one write.
    Bytes head(ss * 2, 0);
    {
        std::uint8_t* e = head.data() + 446;
        e[1] = 0x00;
        e[2] = 0x02;
        e[3] = 0x00;
        e[4] = 0xEE;
        e[5] = e[6] = e[7] = 0xFF;
        put_u32(e + 8, 1);
        put_u32(e + 12, static_cast<std::uint32_t>(std::min<std::uint64_t>(sectors - 1, 0xFFFFFFFFu)));
        head[510] = 0x55;
        head[511] = 0xAA;
    }
    const Bytes primary = header(1, sectors - 1, 2);
    std::copy(primary.begin(), primary.end(), head.begin() + static_cast<std::ptrdiff_t>(ss));
    head.insert(head.end(), array.begin(), array.end());
    out.push_back({0, std::move(head)});

    // Backup array, then the backup header in the last sector.
    Bytes tail = array;
    const Bytes backup = header(sectors - 1, 1, sectors - 1 - array_sectors);
    tail.insert(tail.end(), backup.begin(), backup.end());
    out.push_back({(sectors - 1 - array_sectors) * ss, std::move(tail)});
    return out;
}

void renew_ids(DiskLayout& layout) {
    if (layout.style == PartitionStyle::Gpt) {
        layout.disk_guid = format_guid(random_guid());
        for (Partition& p : layout.partitions) p.guid = format_guid(random_guid());
    } else {
        std::uint32_t sig = 0;
        while (sig == 0) {
            std::uint8_t b[4];
            random_bytes(b);
            sig = b[0] | (b[1] << 8) | (b[2] << 16) | (static_cast<std::uint32_t>(b[3]) << 24);
        }
        layout.signature = sig;
    }
}

std::string describe(const Partition& p) {
    std::string s;
    if (!p.mount.empty()) s += p.mount.substr(0, 2) + " ";
    std::string what = p.label;
    if (what.empty()) what = p.name;
    if (what.empty()) {
        if (p.type == kEfiSystemType) what = "EFI system partition";
        else if (p.type == kMsrType) what = "Microsoft reserved";
        else if (p.type == kRecoveryType || p.mbr_type == 0x27) what = "Recovery";
        else what = "Partition " + std::to_string(p.number);
    }
    s += what + " (";
    if (!p.filesystem.empty()) s += p.filesystem + ", ";
    s += size_text(p.length) + ")";
    return s;
}

}  // namespace vaultik::image
