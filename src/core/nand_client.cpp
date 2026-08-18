#include "nandprog/nand_client.hpp"

#include "nandprog/error.hpp"
#include "nandprog/util.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <sstream>
#include <thread>
#include <vector>

namespace nandprog {
namespace {

constexpr unsigned control_timeout_ms = 5000;
constexpr unsigned operation_timeout_ms = 30000;
constexpr unsigned write_start_timeout_ms = 10000;
constexpr unsigned write_ack_timeout_ms = 10000;

protocol::Status response_status(const protocol::Response &response) {
    if (response.code != protocol::ResponseCode::status)
        throw Error("Expected a status response from the programmer");
    return static_cast<protocol::Status>(response.info);
}

} // namespace

protocol::FirmwareVersion NandClient::firmware_version() {
    transport_.write_packet(protocol::encode_simple(protocol::Command::version_get),
                            control_timeout_ms);
    const auto response = expect_data(control_timeout_ms);
    if (response.payload.size() != 4)
        throw Error("Firmware returned an invalid version response");
    return {response.payload[0], response.payload[1],
            static_cast<std::uint16_t>(response.payload[2] |
                                       (response.payload[3] << 8))};
}

protocol::ChipId NandClient::read_id() {
    transport_.write_packet(protocol::encode_simple(protocol::Command::read_id),
                            control_timeout_ms);
    const auto response = expect_data(control_timeout_ms);
    if (response.payload.empty())
        throw Error("Firmware returned an empty NAND ID response");
    return {response.payload};
}

std::optional<protocol::OnfiInfo> NandClient::probe_onfi() {
    transport_.write_packet(
        protocol::encode_simple(protocol::Command::probe_onfi),
        control_timeout_ms);
    try {
        const auto response = protocol::read_response(transport_, control_timeout_ms);
        if (response.code == protocol::ResponseCode::data) {
            return protocol::decode_onfi(response.payload);
        }
    } catch (...) {
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<protocol::MibibLocation> NandClient::scan_mibib() {
    transport_.write_packet(
        protocol::encode_simple(protocol::Command::scan_mibib),
        control_timeout_ms);
    try {
        const auto response = protocol::read_response(transport_, control_timeout_ms);
        if (response.code == protocol::ResponseCode::data) {
            return protocol::decode_scan_mibib(response);
        }
    } catch (...) {
        return std::nullopt;
    }
    return std::nullopt;
}

void NandClient::configure(const Chip &chip) {
    transport_.write_packet(
        protocol::encode_configure(0, chip.page_size, chip.block_size,
                                   chip.total_size, chip.spare_size,
                                   chip.bad_block_mark_offset,
                                   chip.hal_configuration()),
        control_timeout_ms);
    expect_ok(control_timeout_ms);
}

const Chip &NandClient::probe(
    const ChipDatabase &database,
    const std::optional<std::string> &forced_chip) {
    (void)firmware_version();

    if (forced_chip) {
        const Chip *chip = database.find_by_name(*forced_chip);
        if (chip == nullptr)
            throw Error("Chip not found in database: " + *forced_chip);
        configure(*chip);
        (void)read_id();
        return *chip;
    }

    configure(database.first());
    const protocol::ChipId id = read_id();
    const Chip *chip = database.find_by_id(id);
    if (chip == nullptr)
        throw Error("NAND ID was read but no matching chip exists in the database");
    configure(*chip);
    return *chip;
}

void NandClient::read(std::uint64_t address, std::uint64_t length,
                      protocol::Flags flags, const DataCallback &on_data,
                      const ProgressCallback &on_progress,
                      const BadBlockCallback &on_bad_block) {
    if (length == 0)
        throw Error("Read length must not be zero");
    transport_.write_packet(
        protocol::encode_range(protocol::Command::read, address, length, flags),
        control_timeout_ms);

    std::uint64_t received = 0;
    while (received < length) {
        const auto response = protocol::read_response(transport_, operation_timeout_ms);
        if (response.code == protocol::ResponseCode::data) {
            if (response.payload.empty() ||
                response.payload.size() > length - received)
                throw Error("Firmware returned an invalid amount of read data");
            on_data(response.payload.data(), response.payload.size());
            received += response.payload.size();
            if (on_progress)
                on_progress(received);
            continue;
        }

        switch (response_status(response)) {
        case protocol::Status::error:
            throw_firmware_error(response);
        case protocol::Status::bad_block:
            if (on_bad_block)
                on_bad_block(decode_bad_block(response, false));
            break;
        case protocol::Status::bad_block_skip:
            if (on_bad_block)
                on_bad_block(decode_bad_block(response, true));
            break;
        case protocol::Status::progress:
            if (response.payload.size() != 8)
                throw Error("Invalid progress response");
            if (on_progress)
                on_progress(protocol::decode_u64(response.payload.data()));
            break;
        default:
            throw Error("Unexpected status during NAND read");
        }
    }
}

void NandClient::erase(std::uint64_t address, std::uint64_t length,
                       protocol::Flags flags,
                       const ProgressCallback &on_progress,
                       const BadBlockCallback &on_bad_block) {
    if (length == 0)
        throw Error("Erase length must not be zero");
    transport_.write_packet(
        protocol::encode_range(protocol::Command::erase, address, length, flags),
        control_timeout_ms);

    while (true) {
        const auto response = protocol::read_response(transport_, operation_timeout_ms);
        switch (response_status(response)) {
        case protocol::Status::ok:
            return;
        case protocol::Status::error:
            throw_firmware_error(response);
        case protocol::Status::bad_block:
            if (on_bad_block)
                on_bad_block(decode_bad_block(response, false));
            break;
        case protocol::Status::bad_block_skip:
            if (on_bad_block)
                on_bad_block(decode_bad_block(response, true));
            break;
        case protocol::Status::progress:
            if (response.payload.size() != 8)
                throw Error("Invalid erase progress response");
            if (on_progress)
                on_progress(protocol::decode_u64(response.payload.data()));
            break;
        default:
            throw Error("Unexpected status during NAND erase");
        }
    }
}

void NandClient::scrub(std::uint64_t address, std::uint64_t length,
                        const ProgressCallback &on_progress,
                        const BadBlockCallback &on_bad_block) {
    if (length == 0)
        throw Error("Scrub length must not be zero");
    transport_.write_packet(
        protocol::encode_scrub(address, length),
        control_timeout_ms);

    while (true) {
        const auto response = protocol::read_response(transport_, operation_timeout_ms);
        switch (response_status(response)) {
        case protocol::Status::ok:
            return;
        case protocol::Status::error:
            throw_firmware_error(response);
        case protocol::Status::bad_block:
            if (on_bad_block)
                on_bad_block(decode_bad_block(response, false));
            break;
        case protocol::Status::bad_block_skip:
            if (on_bad_block)
                on_bad_block(decode_bad_block(response, true));
            break;
        case protocol::Status::progress:
            if (response.payload.size() != 8)
                throw Error("Invalid scrub progress response");
            if (on_progress)
                on_progress(protocol::decode_u64(response.payload.data()));
            break;
        default:
            throw Error("Unexpected status during NAND scrub");
        }
    }
}

void NandClient::nand_test(std::uint64_t address, std::uint64_t length,
                           protocol::TestMode mode, bool mark_bad, std::uint32_t seed,
                           const ProgressCallback &on_progress,
                           const BadBlockCallback &on_bad_block) {
    if (length == 0)
        throw Error("Test length must not be zero");
    transport_.write_packet(
        protocol::encode_test(address, length, mode, mark_bad, seed),
        control_timeout_ms);

    while (true) {
        const auto response = protocol::read_response(transport_, operation_timeout_ms);
        switch (response_status(response)) {
        case protocol::Status::ok:
            return;
        case protocol::Status::error:
            throw_firmware_error(response);
        case protocol::Status::bad_block:
            if (on_bad_block)
                on_bad_block(decode_bad_block(response, false));
            break;
        case protocol::Status::bad_block_skip:
            if (on_bad_block)
                on_bad_block(decode_bad_block(response, true));
            break;
        case protocol::Status::progress:
            if (response.payload.size() != 8)
                throw Error("Invalid test progress response");
            if (on_progress)
                on_progress(protocol::decode_u64(response.payload.data()));
            break;
        default:
            throw Error("Unexpected status during NAND test");
        }
    }
}

void NandClient::write(std::istream &input, std::uint64_t address,
                       std::uint64_t length,
                       std::uint32_t transfer_page_size,
                       protocol::Flags flags,
                       const ProgressCallback &on_progress,
                       const BadBlockCallback &on_bad_block) {
    write_pages(
        [&input](std::uint8_t *page, std::size_t size) {
            std::fill_n(page, size, 0xff);
            input.read(reinterpret_cast<char *>(page),
                       static_cast<std::streamsize>(size));
            const std::streamsize input_bytes = input.gcount();
            if (input.bad())
                throw Error("Failed to read input file during NAND write");
            if (input_bytes == 0)
                throw Error("Input file ended before the requested write length");
        },
        address, length, transfer_page_size, flags, on_progress, on_bad_block);
}

void NandClient::write_pages(const PageProvider &provide_page,
                             std::uint64_t address, std::uint64_t length,
                             std::uint32_t transfer_page_size,
                             protocol::Flags flags,
                             const ProgressCallback &on_progress,
                             const BadBlockCallback &on_bad_block) {
    if (transfer_page_size == 0 || length == 0 ||
        length % transfer_page_size != 0)
        throw Error("Write length must be a non-zero multiple of page size");
    if (!provide_page)
        throw Error("NAND write page provider is missing");

    log_debug("write_start address=" + hex_number(address) +
              ", length=" + hex_number(length) +
              ", transfer_page_size=" + std::to_string(transfer_page_size) +
              ", flags=" + std::to_string(flags.encode()));

    transport_.write_packet(
        protocol::encode_range(protocol::Command::write_start, address, length,
                               flags),
        control_timeout_ms);
    try {
        expect_ok(write_start_timeout_ms, on_bad_block);
    } catch (const Error &error) {
        throw Error(std::string("Timed out or failed while waiting for write_start OK: ") +
                    error.what());
    }

    std::vector<std::uint8_t> page(transfer_page_size);
    std::uint64_t transferred = 0;
    while (transferred < length) {
        provide_page(page.data(), page.size());
        log_debug("write_pages: sending page at offset=" + hex_number(address + transferred) +
                  " (bytes=" + std::to_string(transferred) + "/" + std::to_string(length) + ")");

        std::vector<std::uint8_t> page_packets;
        page_packets.reserve(((page.size() + protocol::max_write_payload - 1) / protocol::max_write_payload) * 64);
        std::size_t page_offset = 0;
        unsigned chunk_index = 0;
        while (page_offset < page.size()) {
            const std::size_t chunk_size = std::min(
                protocol::max_write_payload, page.size() - page_offset);
            log_debug("write_pages: chunk #" + std::to_string(chunk_index++) +
                      " offset=" + std::to_string(page_offset) + " size=" + std::to_string(chunk_size));
            const auto chunk_pkt = protocol::encode_write_data(page.data() + page_offset, chunk_size);
            page_packets.insert(page_packets.end(), chunk_pkt.begin(), chunk_pkt.end());
            page_offset += chunk_size;
        }
        transport_.write_buffer(page_packets, control_timeout_ms);

        const std::uint64_t expected_ack = transferred + page.size();
        log_debug("write_pages: page upload finished, waiting for write_ack (expected=" +
                  std::to_string(expected_ack) + ", timeout=" + std::to_string(write_ack_timeout_ms) + "ms)...");
        while (true) {
            protocol::Response response;
            try {
                response = protocol::read_response(transport_, write_ack_timeout_ms);
            } catch (const Error &error) {
                throw Error(std::string("Timed out or failed while waiting for write_ack at bytes=") +
                            std::to_string(transferred) + "/" +
                            std::to_string(length) + ": " + error.what());
            }
            switch (response_status(response)) {
            case protocol::Status::write_ack:
                if (response.payload.size() < 8 ||
                    protocol::decode_u64(response.payload.data()) != expected_ack)
                    throw Error("Firmware returned an invalid write acknowledgement");
                transferred = expected_ack;
                log_debug("write_ack bytes=" + std::to_string(transferred) +
                          ", remaining=" + std::to_string(length - transferred));
                if (on_progress)
                    on_progress(transferred);
                break;
            case protocol::Status::error:
                log_debug("write_pages: received error response from firmware");
                throw_firmware_error(response);
            case protocol::Status::bad_block:
                log_debug("write_pages: received bad_block notification before ack");
                if (on_bad_block)
                    on_bad_block(decode_bad_block(response, false));
                continue;
            case protocol::Status::bad_block_skip:
                log_debug("write_pages: received bad_block_skip notification before ack");
                if (on_bad_block)
                    on_bad_block(decode_bad_block(response, true));
                continue;
            default:
                log_debug("write_pages: received unexpected status " + std::to_string(static_cast<int>(response_status(response))));
                throw Error("Unexpected status during NAND write");
            }
            break;
        }
    }

    log_debug("write_end address=" + hex_number(address) +
              ", length=" + hex_number(length));
    transport_.write_packet(protocol::encode_simple(protocol::Command::write_end),
                            control_timeout_ms);
    try {
        expect_ok(control_timeout_ms, on_bad_block);
    } catch (const Error &error) {
        throw Error(std::string("Timed out or failed while waiting for write_end OK: ") +
                    error.what());
    }
}

protocol::Response NandClient::expect_data(unsigned timeout_ms) {
    auto response = protocol::read_response(transport_, timeout_ms);
    if (response.code == protocol::ResponseCode::status &&
        static_cast<protocol::Status>(response.info) == protocol::Status::error)
        throw_firmware_error(response);
    if (response.code != protocol::ResponseCode::data)
        throw Error("Expected a data response from the programmer");
    return response;
}

void NandClient::expect_ok(unsigned timeout_ms,
                           const BadBlockCallback &on_bad_block) {
    while (true) {
        const auto response = protocol::read_response(transport_, timeout_ms);
        switch (response_status(response)) {
        case protocol::Status::ok:
            return;
        case protocol::Status::error:
            throw_firmware_error(response);
        case protocol::Status::bad_block:
            if (on_bad_block)
                on_bad_block(decode_bad_block(response, false));
            break;
        case protocol::Status::bad_block_skip:
            if (on_bad_block)
                on_bad_block(decode_bad_block(response, true));
            break;
        default:
            throw Error("Expected an OK response from the programmer");
        }
    }
}

[[noreturn]] void NandClient::throw_firmware_error(
    const protocol::Response &response) const {
    if (response.payload.size() != 12)
        throw Error("Firmware returned a malformed error response");
    const std::uint8_t code = response.payload.front();
    throw FirmwareError(code, "Firmware error " + std::to_string(code) +
                                  ": " + protocol::firmware_error_message(code));
}

BadBlockEvent NandClient::decode_bad_block(
    const protocol::Response &response, bool skipped) {
    if (response.payload.size() != 12)
        throw Error("Firmware returned a malformed bad-block response");
    return {protocol::decode_u64(response.payload.data()),
            protocol::decode_u32(response.payload.data() + 8), skipped};
}

} // namespace nandprog
