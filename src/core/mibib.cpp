#include "nandprog/mibib.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace nandprog::mibib {
namespace {

constexpr std::uint32_t MIBIB_HEADER_MAGIC1 = 0xFE569FAC;
constexpr std::uint32_t MIBIB_HEADER_MAGIC2 = 0xCD7F127A;
constexpr std::uint32_t SYS_TABLE_MAGIC1    = 0x55EE73AA;
constexpr std::uint32_t SYS_TABLE_MAGIC2    = 0xE35EBDDB;
constexpr std::uint32_t USR_TABLE_MAGIC1    = 0xAA7D1B9A;
constexpr std::uint32_t USR_TABLE_MAGIC2    = 0x1F7D48BC;

std::uint32_t read_u32_le(const std::uint8_t *data) {
    return static_cast<std::uint32_t>(data[0]) |
           (static_cast<std::uint32_t>(data[1]) << 8) |
           (static_cast<std::uint32_t>(data[2]) << 16) |
           (static_cast<std::uint32_t>(data[3]) << 24);
}

std::uint16_t read_u16_le(const std::uint8_t *data) {
    return static_cast<std::uint16_t>(data[0]) |
           (static_cast<std::uint16_t>(data[1]) << 8);
}

std::string normalize_name(std::string_view name) {
    std::string result;
    result.reserve(name.size());
    for (char c : name) {
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    // Strip "0:" prefix if present
    if (result.rfind("0:", 0) == 0) {
        result = result.substr(2);
    }
    return result;
}

} // namespace

const PartitionEntry *PartitionTable::find(const std::string &name) const {
    const std::string norm_target = normalize_name(name);
    for (const auto &entry : partitions) {
        if (entry.name == name)
            return &entry;
    }
    for (const auto &entry : partitions) {
        if (normalize_name(entry.name) == norm_target)
            return &entry;
    }
    return nullptr;
}

std::optional<PartitionTable> parse_mibib(const std::uint8_t *data, std::size_t size,
                                         std::uint32_t block_size, std::uint64_t total_size,
                                         std::uint64_t base_offset) {
    if (size < 32)
        return std::nullopt;

    std::size_t mibib_offset = static_cast<std::size_t>(-1);
    std::size_t table_offset = static_cast<std::size_t>(-1);
    bool is_sys_table = true;
    std::uint32_t num_parts = 0;

    // Priority 1: Sliding scan for MIBIB Header (0xFE569FAC / 0xCD7F127A) with Table Header at +16
    for (std::size_t offset = 0; offset + 32 <= size; offset += 4) {
        if (read_u32_le(data + offset) == MIBIB_HEADER_MAGIC1 &&
            read_u32_le(data + offset + 4) == MIBIB_HEADER_MAGIC2) {
            const std::size_t test_table_offset = offset + 16;
            if (test_table_offset + 16 <= size) {
                const std::uint32_t m1 = read_u32_le(data + test_table_offset);
                const std::uint32_t m2 = read_u32_le(data + test_table_offset + 4);
                const std::uint32_t parts = read_u32_le(data + test_table_offset + 12);

                if (m1 == SYS_TABLE_MAGIC1 && m2 == SYS_TABLE_MAGIC2 && parts > 0 && parts <= 128) {
                    mibib_offset = offset;
                    table_offset = test_table_offset;
                    is_sys_table = true;
                    num_parts = parts;
                    break;
                } else if (m1 == USR_TABLE_MAGIC1 && m2 == USR_TABLE_MAGIC2 && parts > 0 && parts <= 128) {
                    mibib_offset = offset;
                    table_offset = test_table_offset;
                    is_sys_table = false;
                    num_parts = parts;
                    break;
                }
            }
        }
    }

    // Priority 2: Standalone Table Header Scan (0x55EE73AA or 0xAA7D1B9A)
    if (table_offset == static_cast<std::size_t>(-1)) {
        for (std::size_t offset = 0; offset + 16 <= size; offset += 4) {
            const std::uint32_t m1 = read_u32_le(data + offset);
            const std::uint32_t m2 = read_u32_le(data + offset + 4);
            const std::uint32_t parts = read_u32_le(data + offset + 12);

            if (m1 == SYS_TABLE_MAGIC1 && m2 == SYS_TABLE_MAGIC2 && parts > 0 && parts <= 128) {
                table_offset = offset;
                is_sys_table = true;
                num_parts = parts;
                break;
            } else if (m1 == USR_TABLE_MAGIC1 && m2 == USR_TABLE_MAGIC2 && parts > 0 && parts <= 128) {
                table_offset = offset;
                is_sys_table = false;
                num_parts = parts;
                break;
            }
        }
    }

    if (table_offset == static_cast<std::size_t>(-1))
        return std::nullopt;

    const std::uint32_t table_version = read_u32_le(data + table_offset + 8);
    const std::uint32_t mibib_version = (mibib_offset != static_cast<std::size_t>(-1))
                                            ? read_u32_le(data + mibib_offset + 8)
                                            : table_version;

    PartitionTable result;
    result.table_type = is_sys_table ? "system" : "user";
    result.mibib_offset = base_offset + ((mibib_offset != static_cast<std::size_t>(-1)) ? mibib_offset : table_offset);
    result.mibib_version = mibib_version;
    result.table_version = table_version;

    constexpr std::size_t header_size = 16;
    constexpr std::size_t entry_size = 28;

    std::uint64_t current_block_offset = 0;

    for (std::uint32_t i = 0; i < num_parts; ++i) {
        const std::size_t entry_offset = table_offset + header_size + i * entry_size;
        if (entry_offset + entry_size > size)
            break;

        const std::uint8_t *entry_data = data + entry_offset;

        // 16 bytes ASCII name
        std::string name;
        for (std::size_t k = 0; k < 16; ++k) {
            if (entry_data[k] == 0)
                break;
            name.push_back(static_cast<char>(entry_data[k]));
        }

        PartitionEntry entry;
        entry.name = name;

        if (is_sys_table) {
            entry.start_block = read_u32_le(entry_data + 16);
            entry.size_blocks = read_u32_le(entry_data + 20);
            entry.attr1 = entry_data[24];
            entry.attr2 = entry_data[25];
            entry.attr3 = entry_data[26];
            entry.which_flash = entry_data[27];

            entry.start_offset = static_cast<std::uint64_t>(entry.start_block) * block_size;
            if (entry.size_blocks == 0xFFFFFFFF || entry.size_blocks == 0) {
                entry.size_bytes = (total_size > entry.start_offset) ? (total_size - entry.start_offset) : 0;
            } else {
                entry.size_bytes = static_cast<std::uint64_t>(entry.size_blocks) * block_size;
            }
        } else {
            const std::uint32_t size_kb = read_u32_le(entry_data + 16);
            const std::uint16_t pad_kb = read_u16_le(entry_data + 20);
            entry.which_flash = static_cast<std::uint8_t>(read_u16_le(entry_data + 22));
            entry.attr1 = entry_data[24];
            entry.attr2 = entry_data[25];
            entry.attr3 = entry_data[26];
            entry.attr4 = entry_data[27];

            entry.start_offset = current_block_offset;
            const std::uint64_t raw_size = static_cast<std::uint64_t>(size_kb) * 1024;
            const std::uint64_t pad_size = static_cast<std::uint64_t>(pad_kb) * 1024;
            entry.size_bytes = raw_size;
            if (block_size > 0) {
                entry.start_block = static_cast<std::uint32_t>(entry.start_offset / block_size);
                entry.size_blocks = static_cast<std::uint32_t>((raw_size + block_size - 1) / block_size);
            }
            current_block_offset += raw_size + pad_size;
        }

        result.partitions.push_back(std::move(entry));
    }

    return result;
}

std::vector<std::uint8_t> deinterleave_qpic(const std::uint8_t *raw_data, std::size_t raw_size,
                                            std::uint32_t page_size, std::uint32_t spare_size) {
    if (page_size == 0 || raw_size == 0 || page_size % 512 != 0)
        return {};

    const std::size_t cws_per_page = page_size / 512;
    const std::size_t min_oob_for_bch8 = cws_per_page * 20;
    const bool is_bch8 = (spare_size >= min_oob_for_bch8);
    const std::uint32_t cw_size = is_bch8 ? 532U : 528U;
    const std::uint32_t bbm_pos = page_size % cw_size;
    constexpr std::uint32_t cw_data_size = 516U;
    const std::size_t raw_page_size = page_size + spare_size;

    if (raw_page_size == 0 || bbm_pos == 0 || bbm_pos > cw_data_size)
        return {};

    std::vector<std::uint8_t> user_data;
    user_data.reserve((raw_size / raw_page_size + 1) * page_size);

    std::size_t raw_page_offset = 0;
    while (raw_page_offset + raw_page_size <= raw_size) {
        std::size_t page_bytes_collected = 0;
        for (std::size_t cw = 0; cw < cws_per_page && page_bytes_collected < page_size; ++cw) {
            const std::size_t cw_start = raw_page_offset + cw * cw_size;
            if (cw_start + cw_size > raw_size)
                break;

            // Part 1: before BBM (bbm_pos bytes)
            const std::size_t part1_size = std::min<std::size_t>(bbm_pos, page_size - page_bytes_collected);
            user_data.insert(user_data.end(), raw_data + cw_start, raw_data + cw_start + part1_size);
            page_bytes_collected += part1_size;

            // Part 2: after BBM (cw_data_size - bbm_pos bytes)
            const std::size_t part2_start = cw_start + bbm_pos + 1; // skip 1 BBM byte
            const std::size_t part2_avail = cw_data_size - bbm_pos;
            const std::size_t part2_size = std::min<std::size_t>(part2_avail, page_size - page_bytes_collected);
            user_data.insert(user_data.end(), raw_data + part2_start, raw_data + part2_start + part2_size);
            page_bytes_collected += part2_size;
        }
        raw_page_offset += raw_page_size;
    }

    return user_data;
}

} // namespace nandprog::mibib
