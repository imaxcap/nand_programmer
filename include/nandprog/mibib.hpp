#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace nandprog::mibib {

struct PartitionEntry {
    std::string name;
    std::uint32_t start_block = 0;
    std::uint32_t size_blocks = 0;
    std::uint64_t start_offset = 0;
    std::uint64_t size_bytes = 0;
    std::uint8_t attr1 = 0;
    std::uint8_t attr2 = 0;
    std::uint8_t attr3 = 0;
    std::uint8_t attr4 = 0;
    std::uint8_t which_flash = 0;
};

struct PartitionTable {
    std::string table_type; // "system" or "user"
    std::uint64_t mibib_offset = 0;
    std::uint32_t mibib_version = 0;
    std::uint32_t table_version = 0;
    std::vector<PartitionEntry> partitions;

    const PartitionEntry *find(const std::string &name) const;
};

std::optional<PartitionTable> parse_mibib(const std::uint8_t *data, std::size_t size,
                                         std::uint32_t block_size, std::uint64_t total_size,
                                         std::uint64_t base_offset = 0);

std::vector<std::uint8_t> deinterleave_qpic(const std::uint8_t *raw_data, std::size_t raw_size,
                                            std::uint32_t page_size, std::uint32_t spare_size);

} // namespace nandprog::mibib
