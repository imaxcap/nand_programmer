#include "nandprog/chip_db.hpp"
#include "nandprog/error.hpp"
#include "nandprog/mibib.hpp"
#include "nandprog/nand_client.hpp"
#include "nandprog/protocol.hpp"
#include "nandprog/qpic.hpp"
#include "nandprog/transport.hpp"
#include "nandprog/util.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
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

    void flush() override { ++flush_count; }

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

    void data(std::initializer_list<std::uint8_t> bytes) {
        responses.push_back(0);
        responses.push_back(static_cast<std::uint8_t>(bytes.size()));
        responses.insert(responses.end(), bytes);
    }

    void ack(std::uint64_t bytes) {
        responses.insert(responses.end(), {1, 3});
        for (unsigned shift = 0; shift < 64; shift += 8)
            responses.push_back(static_cast<std::uint8_t>(bytes >> shift));
        responses.insert(responses.end(), {0, 0, 0, 0});
    }

    bool open_ = true;
    std::size_t flush_count = 0;
    std::vector<std::vector<std::uint8_t>> packets;
    std::vector<std::uint8_t> responses;
    std::size_t read_offset = 0;
};

void require(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}

std::uint32_t fnv1a(const std::vector<std::uint8_t> &data) {
    std::uint32_t hash = 2166136261U;
    for (const auto byte : data) {
        hash ^= byte;
        hash *= 16777619U;
    }
    return hash;
}

std::vector<std::uint8_t> write_payload(const FakeTransport &transport) {
    std::vector<std::uint8_t> payload;
    for (std::size_t index = 1; index + 1 < transport.packets.size(); ++index) {
        const auto &packet = transport.packets[index];
        require(packet.size() <= nandprog::protocol::max_packet_size,
                "write packet must fit one CDC packet");
        require(packet[0] == 4, "write-data opcode");
        require(packet[1] == packet.size() - 2, "write-data length");
        payload.insert(payload.end(), packet.begin() + 2, packet.end());
    }
    return payload;
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

    nandprog::protocol::Flags qpic_bch4_flags;
    qpic_bch4_flags.skip_bad = true;
    qpic_bch4_flags.qpic_bch4 = true;
    require(qpic_bch4_flags.encode() == (1 | 8), "qpic bch4 flags bitmask");

    nandprog::protocol::Flags qpic_bch8_flags;
    qpic_bch8_flags.skip_bad = true;
    qpic_bch8_flags.qpic_bch8 = true;
    require(qpic_bch8_flags.encode() == (1 | 16), "qpic bch8 flags bitmask");

    const auto scrub_packet = nandprog::protocol::encode_scrub(0x0, 0x8000000);
    require(scrub_packet.size() == 17, "scrub packet size");
    require(scrub_packet[0] == 0x10, "scrub opcode 0x10");

    const auto test_packet_block = nandprog::protocol::encode_test(
        0x0, 0x8000000, nandprog::protocol::TestMode::full_block, true, 0x12345678);
    require(test_packet_block.size() == 23, "test packet size");
    require(test_packet_block[0] == 0x11, "test opcode 0x11");
    require(test_packet_block[17] == 0, "test mode full_block (0)");
    require(test_packet_block[18] == 1, "test mark bad true");

    const auto test_packet_chip = nandprog::protocol::encode_test(
        0x0, 0x8000000, nandprog::protocol::TestMode::full_chip, false, 0xA5A55A5A);
    require(test_packet_chip.size() == 23, "test packet size");
    require(test_packet_chip[0] == 0x11, "test opcode 0x11");
    require(test_packet_chip[17] == 3, "test mode full_chip (3)");
    require(test_packet_chip[18] == 0, "test mark bad false");

    // Test ONFI payload decoding
    std::vector<std::uint8_t> onfi_payload(54, 0);
    std::memcpy(onfi_payload.data(), "MICRON      ", 12);
    std::memcpy(onfi_payload.data() + 12, "MT29F2G08ABAEA      ", 20);
    // page_size = 2048 (0x00000800)
    onfi_payload[32] = 0x00; onfi_payload[33] = 0x08; onfi_payload[34] = 0x00; onfi_payload[35] = 0x00;
    // block_size = 131072 (0x00020000)
    onfi_payload[36] = 0x00; onfi_payload[37] = 0x00; onfi_payload[38] = 0x02; onfi_payload[39] = 0x00;
    // total_size = 268435456 (0x10000000)
    onfi_payload[40] = 0x00; onfi_payload[41] = 0x00; onfi_payload[42] = 0x00; onfi_payload[43] = 0x10;
    // spare_size = 64
    onfi_payload[48] = 64;
    // row_cycles = 3, col_cycles = 2
    onfi_payload[52] = 3;
    onfi_payload[53] = 2;

    const auto decoded_onfi = nandprog::protocol::decode_onfi(onfi_payload);
    require(decoded_onfi.manufacturer == "MICRON", "ONFI decoded manufacturer");
    require(decoded_onfi.model == "MT29F2G08ABAEA", "ONFI decoded model");
    require(decoded_onfi.page_size == 2048, "ONFI decoded page size");
    require(decoded_onfi.spare_size == 64, "ONFI decoded spare size");
    require(decoded_onfi.block_size == 131072, "ONFI decoded block size");
    require(decoded_onfi.total_size == 268435456ULL, "ONFI decoded total size");
    require(decoded_onfi.row_cycles == 3, "ONFI decoded row cycles");
    require(decoded_onfi.col_cycles == 2, "ONFI decoded col cycles");
}

void test_database() {
    const auto database_path = std::filesystem::path(NANDPROG_TEST_DB);
    const auto adjacent_database = nandprog::find_database(
        {}, database_path.parent_path() / "nandprog-test-executable");
    require(std::filesystem::equivalent(adjacent_database, database_path),
            "database lookup beside executable");

    nandprog::ChipDatabase database;
    database.load(database_path);
    require(database.chips().size() == 20, "parallel chip count");
    const auto *chip = database.find_by_name("MT29F2G08ABAEA");
    require(chip != nullptr, "Micron chip lookup");
    require(chip->raw_page_size() == 2112, "raw page geometry");
    const auto hal = chip->hal_configuration();
    require(hal.size() == 22, "FSMC configuration size");
    require(hal[17] == 112, "status command mapping");
    require(hal[18] == 239, "set-features command mapping");

    nandprog::protocol::ChipId id{{44, 218, 144, 149, 0xff}};
    require(database.find_by_id(id) == chip, "chip ID wildcard matching");

    nandprog::protocol::ChipId extended_id{
        {44, 218, 144, 149, 0xff, 0x12, 0x34, 0x56}};
    require(database.find_by_id(extended_id) == chip,
            "extra raw ID bytes must not break five-column CSV matching");

    nandprog::protocol::ChipId mxic_id{
        {0xc2, 0xa1, 0x80, 0x15, 0x02, 0x00}};
    const auto *mxic = database.find_by_id(mxic_id);
    require(mxic != nullptr && mxic->name == "MX30UF1G18AC",
            "MX30UF1G18AC ID lookup");
    require(mxic->page_size == 2048 && mxic->spare_size == 64 &&
                mxic->block_size == 128 * 1024 &&
                mxic->total_size == 128ULL * 1024 * 1024,
            "MX30UF1G18AC geometry");
    const auto mxic_hal = mxic->hal_configuration();
    require(mxic_hal[6] == 2 && mxic_hal[7] == 2 && mxic_hal[8] == 0x00 &&
                mxic_hal[9] == 0x30,
            "MX30UF1G18AC address cycles and read commands");
}

void test_variable_length_id() {
    FakeTransport transport;
    transport.data({0x2c, 0xda, 0x90, 0x95, 0x46, 0x76, 0x00, 0x15});

    nandprog::NandClient client(transport);
    const auto id = client.read_id();
    require(id.bytes == std::vector<std::uint8_t>(
                            {0x2c, 0xda, 0x90, 0x95, 0x46, 0x76, 0x00, 0x15}),
            "read_id must preserve every byte returned by firmware");
    require(transport.packets.size() == 1 &&
                transport.packets.front() == std::vector<std::uint8_t>{0},
            "read_id command encoding");
}

void test_qpic_bch_vectors() {
    const std::vector<std::uint8_t> zero(516, 0x00);
    const std::vector<std::uint8_t> erased(516, 0xff);
    std::vector<std::uint8_t> increment(516);
    std::vector<std::uint8_t> linear(516);
    for (std::size_t index = 0; index < increment.size(); ++index) {
        increment[index] = static_cast<std::uint8_t>(index);
        linear[index] = static_cast<std::uint8_t>(index * 37U + 11U);
    }

    nandprog::qpic::BchEncoder bch4(nandprog::qpic::EccMode::bch4);
    require(bch4.encode(zero.data(), zero.size()) ==
                std::vector<std::uint8_t>({0x00, 0x00, 0x00, 0x00,
                                           0x00, 0x00, 0x00}),
            "QPIC BCH4 zero vector");
    require(bch4.encode(erased.data(), erased.size()) ==
                std::vector<std::uint8_t>({0x07, 0x3f, 0xfb, 0xde,
                                           0x8b, 0x0a, 0xb0}),
            "QPIC BCH4 erased vector");
    require(bch4.encode(increment.data(), increment.size()) ==
                std::vector<std::uint8_t>({0xd0, 0xbb, 0x92, 0x24,
                                           0x50, 0xb4, 0x20}),
            "QPIC BCH4 increment vector");
    require(bch4.encode(linear.data(), linear.size()) ==
                std::vector<std::uint8_t>({0xa9, 0x50, 0xc3, 0xbb,
                                           0x3c, 0x6d, 0x20}),
            "QPIC BCH4 linear vector");

    nandprog::qpic::BchEncoder bch8(nandprog::qpic::EccMode::bch8);
    require(bch8.encode(zero.data(), zero.size()) ==
                std::vector<std::uint8_t>({0x00, 0x00, 0x00, 0x00, 0x00,
                                           0x00, 0x00, 0x00, 0x00, 0x00,
                                           0x00, 0x00, 0x00}),
            "QPIC BCH8 zero vector");
    require(bch8.encode(erased.data(), erased.size()) ==
                std::vector<std::uint8_t>({0xdd, 0xdd, 0x13, 0x2f, 0x6a,
                                           0xa3, 0x2f, 0x59, 0x31, 0x60,
                                           0x5d, 0x95, 0x7f}),
            "QPIC BCH8 erased vector");
    require(bch8.encode(increment.data(), increment.size()) ==
                std::vector<std::uint8_t>({0xdb, 0x40, 0x67, 0xd6, 0x20,
                                           0xd6, 0xc0, 0x33, 0x4a, 0xbb,
                                           0xee, 0xb3, 0xee}),
            "QPIC BCH8 increment vector");
    require(bch8.encode(linear.data(), linear.size()) ==
                std::vector<std::uint8_t>({0x6e, 0xbb, 0xaf, 0x59, 0xdf,
                                           0x94, 0x5e, 0xcc, 0xae, 0x0b,
                                           0x91, 0xc9, 0x19}),
            "QPIC BCH8 linear vector");
}

void test_qpic_page_layout() {
    const std::vector<std::uint8_t> erased(2048, 0xff);
    nandprog::qpic::PageEncoder bch4(2048, 64,
                                     nandprog::qpic::EccMode::bch4);
    const auto page = bch4.encode(erased.data(), erased.size());
    require(page.size() == 2112, "QPIC BCH4 raw page size");
    require(bch4.codeword_count() == 4 && bch4.codeword_size() == 528 &&
                bch4.bbm_position() == 464,
            "QPIC BCH4 2K layout geometry");
    const std::vector<std::uint8_t> parity =
        {0x07, 0x3f, 0xfb, 0xde, 0x8b, 0x0a, 0xb0};
    for (std::size_t codeword = 0; codeword < 4; ++codeword) {
        const std::size_t offset = codeword * 528;
        require(page[offset + 464] == 0xff, "QPIC codeword BBM");
        require(std::equal(parity.begin(), parity.end(),
                           page.begin() + static_cast<std::ptrdiff_t>(offset + 517)),
                "QPIC codeword parity placement");
    }

    nandprog::qpic::PageEncoder bch8(4096, 256,
                                     nandprog::qpic::EccMode::bch8);
    require(bch8.codeword_count() == 8 && bch8.codeword_size() == 532 &&
                bch8.bbm_position() == 372 && bch8.raw_page_size() == 4352,
            "QPIC BCH8 4K layout geometry");

    bool rejected = false;
    try {
        (void)nandprog::qpic::PageEncoder(2048, 64,
                                          nandprog::qpic::EccMode::bch8);
    } catch (const nandprog::Error &) {
        rejected = true;
    }
    require(rejected, "QPIC layout must reject insufficient OOB");
}

void test_qpic_upstream_golden_pages() {
    // Full-page hashes from ecsv/qcom-nandc-pagify commit 71c3c19b106ab.
    // Its tests/resources/*.bin vectors are published under CC0-1.0.
    const std::vector<std::uint8_t> zero2k(2048, 0x00);
    const std::vector<std::uint8_t> upstream_ff2k(2048, 0xff);
    nandprog::qpic::PageEncoder bch4(2048, 64,
                                     nandprog::qpic::EccMode::bch4);
    require(fnv1a(bch4.encode(zero2k.data(), zero2k.size())) == 0xf8c588f3U,
            "upstream QPIC BCH4 zero-page golden vector");
    require(fnv1a(bch4.encode(upstream_ff2k.data(), upstream_ff2k.size())) ==
                0x41025cd5U,
            "upstream QPIC BCH4 ff-page golden vector");

    const std::vector<std::uint8_t> zero4k(4096, 0x00);
    const std::vector<std::uint8_t> upstream_ff4k(4096, 0xff);
    nandprog::qpic::PageEncoder bch8(4096, 256,
                                     nandprog::qpic::EccMode::bch8);
    require(fnv1a(bch8.encode(zero4k.data(), zero4k.size())) == 0xaf825a3eU,
            "upstream QPIC BCH8 zero-page golden vector");
    require(fnv1a(bch8.encode(upstream_ff4k.data(), upstream_ff4k.size())) ==
                0x40a955c5U,
            "upstream QPIC BCH8 ff-page golden vector");

    const std::vector<std::uint8_t> partial = {'a', 'b', 'c'};
    std::vector<std::uint8_t> zero_padded(2048, 0x00);
    std::copy(partial.begin(), partial.end(), zero_padded.begin());
    require(bch4.encode(partial.data(), partial.size()) ==
                bch4.encode(zero_padded.data(), zero_padded.size()),
            "QPIC partial input page must use upstream zero padding");
}

void test_qpic_write_transport() {
    constexpr std::uint32_t raw_page_size = 2112;
    FakeTransport transport;
    transport.ok();
    transport.ack(raw_page_size);
    transport.ok();

    std::vector<std::uint8_t> data(2048);
    for (std::size_t index = 0; index < data.size(); ++index)
        data[index] = static_cast<std::uint8_t>(index * 37U + 11U);
    const nandprog::qpic::PageEncoder encoder(
        2048, 64, nandprog::qpic::EccMode::bch4);
    const auto encoded = encoder.encode(data.data(), data.size());

    nandprog::NandClient client(transport);
    nandprog::protocol::Flags flags;
    flags.skip_bad = true;
    flags.include_spare = true;
    client.write_pages(
        [&encoded](std::uint8_t *page, std::size_t size) {
            require(size == encoded.size(), "QPIC provider page size");
            std::copy(encoded.begin(), encoded.end(), page);
        },
        raw_page_size * 2ULL, raw_page_size, raw_page_size, flags);

    require(transport.packets.front()[17] == 3,
            "QPIC write must skip bad blocks, include OOB, and disable HW ECC");
    require(nandprog::protocol::decode_u64(transport.packets.front().data() + 1) ==
                raw_page_size * 2ULL,
            "QPIC data-space page offset must map to raw-space address");
    require(write_payload(transport) == encoded,
            "QPIC write transport must preserve the generated raw page");
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

    require(write_payload(transport) ==
                std::vector<std::uint8_t>(bytes.begin(), bytes.end()),
            "raw write payload must be byte-for-byte identical");
}

void test_nand_operations_do_not_flush_transport() {
    {
        FakeTransport transport;
        transport.ok();

        nandprog::NandClient client(transport);
        nandprog::protocol::Flags flags;
        client.erase(0, 8, flags);

        require(transport.flush_count == 0,
                "erase must not flush the transport RX buffer");
    }

    {
        FakeTransport transport;
        transport.ok();
        transport.ack(8);
        transport.ok();

        std::istringstream input(std::string("abc"));
        nandprog::NandClient client(transport);
        nandprog::protocol::Flags flags;
        flags.skip_bad = true;
        client.write(input, 0, 8, 8, flags);

        require(transport.flush_count == 0,
                "write must not flush the transport RX buffer");
    }
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

void test_mibib_parser() {
    std::vector<std::uint8_t> buffer(512, 0);
    // MIBIB header
    *reinterpret_cast<std::uint32_t *>(&buffer[0]) = 0xFE569FAC;
    *reinterpret_cast<std::uint32_t *>(&buffer[4]) = 0xCD7F127A;
    *reinterpret_cast<std::uint32_t *>(&buffer[8]) = 4; // version 4

    // System Table header at offset 16
    *reinterpret_cast<std::uint32_t *>(&buffer[16]) = 0x55EE73AA;
    *reinterpret_cast<std::uint32_t *>(&buffer[20]) = 0xE35EBDDB;
    *reinterpret_cast<std::uint32_t *>(&buffer[24]) = 4; // table version 4
    *reinterpret_cast<std::uint32_t *>(&buffer[28]) = 2; // 2 partitions

    // Entry 0: "0:SBL1", start_block = 0, size_blocks = 4
    std::memcpy(&buffer[32], "0:SBL1", 6);
    *reinterpret_cast<std::uint32_t *>(&buffer[48]) = 0;
    *reinterpret_cast<std::uint32_t *>(&buffer[52]) = 4;

    // Entry 1: "0:APPSBL", start_block = 32, size_blocks = 16
    std::memcpy(&buffer[60], "0:APPSBL", 8);
    *reinterpret_cast<std::uint32_t *>(&buffer[76]) = 32;
    *reinterpret_cast<std::uint32_t *>(&buffer[80]) = 16;

    constexpr std::uint32_t block_size = 128 * 1024;
    constexpr std::uint64_t total_size = 128 * 1024 * 1024;
    const auto table = nandprog::mibib::parse_mibib(buffer.data(), buffer.size(), block_size, total_size, 0x20000);
    require(table.has_value(), "MIBIB table parse");
    require(table->partitions.size() == 2, "MIBIB partition count");
    require(table->partitions[0].name == "0:SBL1", "Partition 0 name");
    require(table->partitions[0].start_offset == 0, "Partition 0 start offset");
    require(table->partitions[0].size_bytes == 4 * block_size, "Partition 0 size");

    require(table->partitions[1].name == "0:APPSBL", "Partition 1 name");
    require(table->partitions[1].start_offset == 32 * block_size, "Partition 1 start offset");
    require(table->partitions[1].size_bytes == 16 * block_size, "Partition 1 size");

    // Test find with various formats
    require(table->find("0:APPSBL") != nullptr, "Find exact");
    require(table->find("appsbl") != nullptr, "Find case-insensitive without 0:");
    require(table->find("0:appsbl") != nullptr, "Find case-insensitive with 0:");
    require(table->find("NONEXISTENT") == nullptr, "Find non-existent");
}

void test_qpic_deinterleave_roundtrip() {
    // 2KB + 64B BCH4
    const nandprog::qpic::PageEncoder encoder_bch4(2048, 64, nandprog::qpic::EccMode::bch4);
    std::vector<std::uint8_t> original_data(2048);
    for (std::size_t i = 0; i < 2048; ++i) {
        original_data[i] = static_cast<std::uint8_t>((i * 7 + 13) & 0xff);
    }
    const auto encoded_bch4 = encoder_bch4.encode(original_data.data(), original_data.size());
    require(encoded_bch4.size() == 2112, "BCH4 encoded raw page size");

    const auto deinterleaved_bch4 = nandprog::mibib::deinterleave_qpic(
        encoded_bch4.data(), encoded_bch4.size(), 2048, 64);
    require(deinterleaved_bch4.size() == 2048, "BCH4 deinterleaved size");
    require(deinterleaved_bch4 == original_data, "BCH4 deinterleave roundtrip identity");

    // 2KB + 128B BCH8
    const nandprog::qpic::PageEncoder encoder_bch8(2048, 128, nandprog::qpic::EccMode::bch8);
    const auto encoded_bch8 = encoder_bch8.encode(original_data.data(), original_data.size());
    require(encoded_bch8.size() == 2176, "BCH8 encoded raw page size");

    const auto deinterleaved_bch8 = nandprog::mibib::deinterleave_qpic(
        encoded_bch8.data(), encoded_bch8.size(), 2048, 128);
    require(deinterleaved_bch8.size() == 2048, "BCH8 deinterleaved size");
    require(deinterleaved_bch8 == original_data, "BCH8 deinterleave roundtrip identity");
}

void test_scan_mibib_protocol() {
    FakeTransport transport;
    nandprog::NandClient client(transport);

    // Test successful scan_mibib response
    // Payload: uint64_t offset (e.g. 0x00040000 = 256KB), uint8_t qpic_mode (4 = BCH4)
    std::vector<std::uint8_t> payload = {
        0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, // 0x40000
        0x04,                                           // BCH4
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00        // reserved
    };
    transport.responses.push_back(0); // ResponseCode::data
    transport.responses.push_back(static_cast<std::uint8_t>(payload.size()));
    transport.responses.insert(transport.responses.end(), payload.begin(), payload.end());

    const auto loc = client.scan_mibib();
    require(loc.has_value(), "scan_mibib should succeed");
    require(loc->offset == 0x40000, "scan_mibib offset should match 0x40000");
    require(loc->qpic_mode == 4, "scan_mibib mode should match BCH4");
    require(!transport.packets.empty() && transport.packets.back()[0] == 0x13,
            "scan_mibib command opcode should be 0x13");
}

void test_interruption_handling() {
    nandprog::set_interrupted(false);
    require(!nandprog::is_interrupted(), "initial interrupted state must be false");
    nandprog::check_interrupted(); // should not throw

    nandprog::set_interrupted(true);
    require(nandprog::is_interrupted(), "interrupted state must be true after set");
    bool caught = false;
    try {
        nandprog::check_interrupted();
    } catch (const nandprog::Error &err) {
        caught = true;
        require(std::string(err.what()).find("Ctrl+C") != std::string::npos,
                "error message must mention Ctrl+C");
    }
    require(caught, "check_interrupted must throw Error when interrupted");
    nandprog::set_interrupted(false);
}

} // namespace

int main() {
    try {
        test_protocol_encoding();
        test_database();
        test_variable_length_id();
        test_qpic_bch_vectors();
        test_qpic_page_layout();
        test_qpic_upstream_golden_pages();
        test_qpic_write_transport();
        test_raw_write_identity();
        test_nand_operations_do_not_flush_transport();
        test_normal_write_padding();
        test_command_line_parser();
        test_mibib_parser();
        test_qpic_deinterleave_roundtrip();
        test_scan_mibib_protocol();
        test_interruption_handling();
        std::cout << "All nandprog tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "Test failed: " << error.what() << '\n';
        return 1;
    }
}
