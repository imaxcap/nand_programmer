#pragma once

#include "nandprog/protocol.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace nandprog {

struct Chip {
    static constexpr std::size_t parameter_count = 35;

    std::string name;
    std::uint32_t page_size = 0;
    std::uint32_t block_size = 0;
    std::uint64_t total_size = 0;
    std::uint32_t spare_size = 0;
    std::uint8_t bad_block_mark_offset = 0;
    std::array<std::uint64_t, parameter_count> parameters{};

    std::uint64_t page_count() const;
    std::uint64_t raw_page_size() const;
    std::uint64_t raw_total_size() const;
    std::vector<std::uint8_t> hal_configuration() const;
    bool matches(const protocol::ChipId &id) const;
};

class ChipDatabase {
public:
    void load(const std::filesystem::path &path);
    const Chip &first() const;
    const Chip *find_by_name(const std::string &name) const;
    const Chip *find_by_id(const protocol::ChipId &id) const;
    bool empty() const noexcept { return chips_.empty(); }
    const std::vector<Chip> &chips() const noexcept { return chips_; }

private:
    std::vector<Chip> chips_;
};

} // namespace nandprog
