#include "nandprog/chip_db.hpp"
#include "nandprog/error.hpp"
#include "nandprog/nand_client.hpp"
#include "nandprog/protocol.hpp"
#include "nandprog/transport.hpp"
#include "nandprog/util.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

class FakeTransport final : public nandprog::Transport {
public:
    void open(const std::string &, std::uint32_t) override { open_ = true; }
    void close() noexcept override { open_ = false; }
    bool is_open() const noexcept override { return open_; }

    void write_packet(const std::uint8_t *data, std::size_t size,
                      unsigned) override {
        packets.emplace_back(data, data + size);
    }

    void read_exact(std::uint8_t *data, std::size_t size, unsigned) override {
        if (read_offset + size > responses.size())
            throw nandprog::Error("Fake transport response underflow");
        std::copy_n(responses.data() + read_offset, size, data);
        read_offset += size;
    }

    void ok() { responses.insert(responses.end(), {1, 0}); }

    void ack(std::uint64_t bytes) {
        responses.insert(responses.end(), {1, 3});
        for (unsigned shift = 0; shift < 64; shift += 8)
            responses.push_back(static_cast<std::uint8_t>(bytes >> shift));
        responses.insert(responses.end(), 4, 0);
    }

    bool open_ = true;
    std::vector<std::vector<std::uint8_t>> packets;
    std::vector<std::uint8_t> responses;
    std::size_t read_offset = 0;
};

void require(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}

void test_protocol_encoding() {
    nandprog::protocol::Flags flags;
    flags.include_spare = true;
    const auto range = nandprog::protocol::encode_range(
        nandprog::protocol::Command::write_start, 0x0102030405060708ULL,
        0x1112131415161718ULL, flags);
    require(range.size() == 18, "range command size");
    require(range[0] == 3, "range command opcode");
    require(range[1] == 0x08 && range[8] == 0x01,
            "range address must be little endian");
    require(range[9] == 0x18 && range[16] == 0x11,
            "range length must be little endian");
    require(range[17] == 2, "raw flags must only set include-spare");
}

void test_database() {
    nandprog::ChipDatabase database;
    database.load(NANDPROG_TEST_DB);
    require(database.chips().size() == 19, "parallel chip count");
    const auto *chip = database.find_by_name("MT29F2G08ABAEA");
    require(chip != nullptr, "Micron chip lookup");
    require(chip->raw_page_size() == 2112, "raw page geometry");
    const auto hal = chip->hal_configuration();
    require(hal.size() == 22, "FSMC configuration size");
    require(hal[17] == 112, "status command mapping");
    require(hal[18] == 239, "set-features command mapping");

    nandprog::protocol::ChipId id{{44, 218, 144, 149, 0xff}};
    require(database.find_by_id(id) == chip, "chip ID wildcard matching");
}

void test_raw_write_identity() {
    constexpr std::uint32_t raw_page_size = 2112;
    constexpr std::uint64_t image_size = raw_page_size * 2ULL;
    FakeTransport transport;
    transport.ok();
    transport.ack(raw_page_size);
    transport.ack(image_size);
    transport.ok();

    std::string bytes(image_size, '\0');
    for (std::size_t index = 0; index < bytes.size(); ++index)
        bytes[index] = static_cast<char>((index * 37U + 11U) & 0xffU);
    std::istringstream input(bytes);

    nandprog::NandClient client(transport);
    nandprog::protocol::Flags flags;
    flags.include_spare = true;
    client.write(input, 0, image_size, raw_page_size, flags);

    require(transport.packets.size() > 64,
            "raw write must exercise multi-packet pages");
    require(transport.packets.front().size() == 18,
            "raw write-start packet size");
    require(transport.packets.front()[17] == 2,
            "raw write must disable skip-bad and hardware ECC");
    require(transport.packets.back() == std::vector<std::uint8_t>{5},
            "raw write-end packet");

    std::vector<std::uint8_t> payload;
    for (std::size_t index = 1; index + 1 < transport.packets.size(); ++index) {
        const auto &packet = transport.packets[index];
        require(packet.size() <= nandprog::protocol::max_packet_size,
                "raw write packet must fit one CDC packet");
        require(packet[0] == 4, "raw write-data opcode");
        require(packet[1] == packet.size() - 2, "raw write-data length");
        payload.insert(payload.end(), packet.begin() + 2, packet.end());
    }
    require(payload == std::vector<std::uint8_t>(bytes.begin(), bytes.end()),
            "raw write payload must be byte-for-byte identical");
}

void test_normal_write_padding() {
    FakeTransport transport;
    transport.ok();
    transport.ack(8);
    transport.ok();
    std::istringstream input(std::string("abc"));

    nandprog::NandClient client(transport);
    nandprog::protocol::Flags flags;
    flags.skip_bad = true;
    client.write(input, 0, 8, 8, flags);

    const auto &packet = transport.packets[1];
    require(packet.size() == 10, "padded write-data packet size");
    require(packet[2] == 'a' && packet[3] == 'b' && packet[4] == 'c',
            "normal write data prefix");
    require(std::all_of(packet.begin() + 5, packet.end(),
                        [](std::uint8_t value) { return value == 0xff; }),
            "normal write tail must be FF padded");
}

void test_command_line_parser() {
    const auto values = nandprog::split_command_line(
        "write.raw \"image with spaces.bin\" 0x10");
    require(values.size() == 3, "quoted command argument count");
    require(values[1] == "image with spaces.bin", "quoted filename parsing");
    require(nandprog::parse_number("2M") == 2 * 1024 * 1024,
            "number suffix parsing");
}

} // namespace

int main() {
    try {
        test_protocol_encoding();
        test_database();
        test_raw_write_identity();
        test_normal_write_padding();
        test_command_line_parser();
        std::cout << "All nandprog tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "Test failed: " << error.what() << '\n';
        return 1;
    }
}
