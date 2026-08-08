#pragma once

#include "nandprog/chip_db.hpp"
#include "nandprog/protocol.hpp"
#include "nandprog/transport.hpp"

#include <cstdint>
#include <functional>
#include <istream>
#include <optional>
#include <string>

namespace nandprog {

struct BadBlockEvent {
    std::uint64_t address = 0;
    std::uint32_t size = 0;
    bool skipped = false;
};

using DataCallback = std::function<void(const std::uint8_t *, std::size_t)>;
using PageProvider = std::function<void(std::uint8_t *, std::size_t)>;
using ProgressCallback = std::function<void(std::uint64_t)>;
using BadBlockCallback = std::function<void(const BadBlockEvent &)>;

class NandClient {
public:
    explicit NandClient(Transport &transport) : transport_(transport) {}

    protocol::FirmwareVersion firmware_version();
    protocol::ChipId read_id();
    void configure(const Chip &chip);
    const Chip &probe(const ChipDatabase &database,
                      const std::optional<std::string> &forced_chip = {});

    void read(std::uint64_t address, std::uint64_t length,
              protocol::Flags flags, const DataCallback &on_data,
              const ProgressCallback &on_progress = {},
              const BadBlockCallback &on_bad_block = {});
    void erase(std::uint64_t address, std::uint64_t length,
               protocol::Flags flags,
               const ProgressCallback &on_progress = {},
               const BadBlockCallback &on_bad_block = {});
    void write(std::istream &input, std::uint64_t address,
               std::uint64_t length, std::uint32_t transfer_page_size,
               protocol::Flags flags,
               const ProgressCallback &on_progress = {},
               const BadBlockCallback &on_bad_block = {});
    void write_pages(const PageProvider &provide_page, std::uint64_t address,
                     std::uint64_t length,
                     std::uint32_t transfer_page_size,
                     protocol::Flags flags,
                     const ProgressCallback &on_progress = {},
                     const BadBlockCallback &on_bad_block = {});

private:
    Transport &transport_;

    protocol::Response expect_data(unsigned timeout_ms);
    void expect_ok(unsigned timeout_ms,
                   const BadBlockCallback &on_bad_block = {});
    [[noreturn]] void throw_firmware_error(
        const protocol::Response &response) const;
    static BadBlockEvent decode_bad_block(const protocol::Response &response,
                                          bool skipped);
};

} // namespace nandprog
