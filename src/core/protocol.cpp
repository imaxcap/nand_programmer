#include "nandprog/protocol.hpp"

#include "nandprog/error.hpp"
#include "nandprog/util.hpp"

#include <sstream>

namespace nandprog::protocol {
namespace {

void append_u32(std::vector<std::uint8_t> &buffer, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        buffer.push_back(static_cast<std::uint8_t>(value >> shift));
}

void append_u64(std::vector<std::uint8_t> &buffer, std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8)
        buffer.push_back(static_cast<std::uint8_t>(value >> shift));
}

std::size_t status_payload_size(std::uint8_t info) {
    switch (static_cast<Status>(info)) {
    case Status::ok:
        return 0;
    case Status::error:
    case Status::bad_block:
    case Status::bad_block_skip:
    case Status::write_ack:
        return 12;
    case Status::progress:
        return 8;
    }
    throw Error("Unknown response status " + std::to_string(info));
}

} // namespace

std::uint8_t Flags::encode() const noexcept {
    return static_cast<std::uint8_t>((skip_bad ? 1U : 0U) |
                                     (include_spare ? 2U : 0U) |
                                     (enable_hardware_ecc ? 4U : 0U) |
                                     (qpic_bch4 ? 8U : 0U) |
                                     (qpic_bch8 ? 16U : 0U));
}

std::vector<std::uint8_t> encode_simple(Command command) {
    return {static_cast<std::uint8_t>(command)};
}

std::vector<std::uint8_t> encode_range(Command command, std::uint64_t address,
                                       std::uint64_t length, Flags flags) {
    std::vector<std::uint8_t> result;
    result.reserve(18);
    result.push_back(static_cast<std::uint8_t>(command));
    append_u64(result, address);
    append_u64(result, length);
    result.push_back(flags.encode());
    return result;
}

std::vector<std::uint8_t> encode_scrub(std::uint64_t address, std::uint64_t length) {
    std::vector<std::uint8_t> result;
    result.reserve(17);
    result.push_back(static_cast<std::uint8_t>(Command::scrub));
    append_u64(result, address);
    append_u64(result, length);
    return result;
}

std::vector<std::uint8_t> encode_test(std::uint64_t address, std::uint64_t length,
                                      TestMode mode, bool mark_bad, std::uint32_t seed) {
    std::vector<std::uint8_t> result;
    result.reserve(23);
    result.push_back(static_cast<std::uint8_t>(Command::test));
    append_u64(result, address);
    append_u64(result, length);
    result.push_back(static_cast<std::uint8_t>(mode));
    result.push_back(mark_bad ? 1U : 0U);
    append_u32(result, seed);
    return result;
}

std::vector<std::uint8_t> encode_write_data(const std::uint8_t *data,
                                            std::size_t size) {
    if (size == 0 || size > max_write_payload)
        throw Error("Invalid write payload size " + std::to_string(size));

    std::vector<std::uint8_t> result;
    result.reserve(size + 2);
    result.push_back(static_cast<std::uint8_t>(Command::write_data));
    result.push_back(static_cast<std::uint8_t>(size));
    result.insert(result.end(), data, data + size);
    return result;
}

std::vector<std::uint8_t> encode_configure(
    std::uint8_t hal, std::uint32_t page_size, std::uint32_t block_size,
    std::uint64_t total_size, std::uint32_t spare_size,
    std::uint8_t bad_block_mark_offset,
    const std::vector<std::uint8_t> &hal_configuration) {
    std::vector<std::uint8_t> result;
    result.reserve(23 + hal_configuration.size());
    result.push_back(static_cast<std::uint8_t>(Command::configure));
    result.push_back(hal);
    append_u32(result, page_size);
    append_u32(result, block_size);
    append_u64(result, total_size);
    append_u32(result, spare_size);
    result.push_back(bad_block_mark_offset);
    result.insert(result.end(), hal_configuration.begin(),
                  hal_configuration.end());
    if (result.size() > max_packet_size)
        throw Error("Chip configuration exceeds the 64-byte protocol packet");
    return result;
}

Response read_response(Transport &transport, unsigned timeout_ms) {
    std::uint8_t header[2]{};
    transport.read_exact(header, sizeof(header), timeout_ms);

    Response response;
    if (header[0] > static_cast<std::uint8_t>(ResponseCode::status))
        throw Error("Unknown response code " + std::to_string(header[0]));
    response.code = static_cast<ResponseCode>(header[0]);
    response.info = header[1];

    const std::size_t payload_size = response.code == ResponseCode::data
                                         ? response.info
                                         : status_payload_size(response.info);
    response.payload.resize(payload_size);
    if (payload_size != 0)
        transport.read_exact(response.payload.data(), payload_size, timeout_ms);
    log_debug("protocol::read_response: code=" + std::to_string(static_cast<int>(response.code)) +
              " info=" + std::to_string(static_cast<int>(response.info)) +
              " payload=" + hex_dump(response.payload.data(), response.payload.size()));
    return response;
}

std::uint64_t decode_u64(const std::uint8_t *data) {
    std::uint64_t result = 0;
    for (unsigned index = 0; index < 8; ++index)
        result |= static_cast<std::uint64_t>(data[index]) << (index * 8);
    return result;
}

std::uint32_t decode_u32(const std::uint8_t *data) {
    std::uint32_t result = 0;
    for (unsigned index = 0; index < 4; ++index)
        result |= static_cast<std::uint32_t>(data[index]) << (index * 8);
    return result;
}

static std::string trim_ascii(const char *data, std::size_t len) {
    std::string s(data, len);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0' || s.back() == '\r' || s.back() == '\n'))
        s.pop_back();
    return s;
}

OnfiInfo decode_onfi(const std::vector<std::uint8_t> &payload) {
    if (payload.size() < 54)
        throw Error("ONFI response payload too short (" + std::to_string(payload.size()) + " bytes)");
    OnfiInfo info;
    info.manufacturer = trim_ascii(reinterpret_cast<const char *>(payload.data()), 12);
    info.model = trim_ascii(reinterpret_cast<const char *>(payload.data() + 12), 20);
    info.page_size = decode_u32(payload.data() + 32);
    info.block_size = decode_u32(payload.data() + 36);
    info.total_size = decode_u64(payload.data() + 40);
    info.spare_size = decode_u32(payload.data() + 48);
    info.row_cycles = payload[52];
    info.col_cycles = payload[53];
    return info;
}

MibibLocation decode_scan_mibib(const Response &response) {
    if (response.code != ResponseCode::data || response.payload.size() < 9)
        throw Error("Invalid scan_mibib response payload (" + std::to_string(response.payload.size()) + " bytes)");
    MibibLocation loc;
    loc.offset = decode_u64(response.payload.data());
    loc.qpic_mode = response.payload[8];
    return loc;
}

std::string firmware_error_message(std::uint8_t code) {
    switch (code) {
    case 1:
        return "Internal firmware error";
    case 100:
        return "Operation address exceeded chip size";
    case 101:
        return "Operation address is invalid";
    case 102:
        return "Operation address is not aligned to page/block size";
    case 103:
        return "Failed to write NAND";
    case 104:
        return "Failed to read NAND";
    case 105:
        return "Failed to erase NAND";
    case 106:
        return "Programmer is not configured with chip parameters";
    case 107:
        return "Command data size is invalid";
    case 108:
        return "Invalid command";
    case 109:
        return "Firmware buffer overflow";
    case 110:
        return "Length is not page/block aligned";
    case 111:
        return "Length exceeded chip size";
    case 112:
        return "Invalid data length";
    case 113:
        return "Bad-block table overflow";
    case 114:
        return "NAND reliability test verification failed";
    case 115:
        return "Qualcomm MIBIB partition table not found in flash";
    default:
        return "Unknown firmware error";
    }
}

} // namespace nandprog::protocol
