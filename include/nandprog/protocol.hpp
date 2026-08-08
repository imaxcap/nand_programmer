#pragma once

#include "nandprog/transport.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nandprog::protocol {

constexpr std::size_t max_packet_size = 64;
constexpr std::size_t max_write_payload = 62;

enum class Command : std::uint8_t {
    read_id = 0x00,
    erase = 0x01,
    read = 0x02,
    write_start = 0x03,
    write_data = 0x04,
    write_end = 0x05,
    configure = 0x06,
    read_bad_blocks = 0x07,
    version_get = 0x08,
};

enum class ResponseCode : std::uint8_t { data = 0x00, status = 0x01 };

enum class Status : std::uint8_t {
    ok = 0x00,
    error = 0x01,
    bad_block = 0x02,
    write_ack = 0x03,
    bad_block_skip = 0x04,
    progress = 0x05,
};

struct Flags {
    bool skip_bad = false;
    bool include_spare = false;
    bool enable_hardware_ecc = false;

    std::uint8_t encode() const noexcept;
};

struct FirmwareVersion {
    std::uint8_t major = 0;
    std::uint8_t minor = 0;
    std::uint16_t build = 0;
};

struct ChipId {
    std::array<std::uint8_t, 5> bytes{};
};

struct Response {
    ResponseCode code{};
    std::uint8_t info = 0;
    std::vector<std::uint8_t> payload;
};

std::vector<std::uint8_t> encode_simple(Command command);
std::vector<std::uint8_t> encode_range(Command command, std::uint64_t address,
                                       std::uint64_t length, Flags flags);
std::vector<std::uint8_t> encode_write_data(const std::uint8_t *data,
                                            std::size_t size);
std::vector<std::uint8_t> encode_configure(
    std::uint8_t hal, std::uint32_t page_size, std::uint32_t block_size,
    std::uint64_t total_size, std::uint32_t spare_size,
    std::uint8_t bad_block_mark_offset,
    const std::vector<std::uint8_t> &hal_configuration);

Response read_response(Transport &transport, unsigned timeout_ms);
std::uint64_t decode_u64(const std::uint8_t *data);
std::uint32_t decode_u32(const std::uint8_t *data);
std::string firmware_error_message(std::uint8_t code);

} // namespace nandprog::protocol
