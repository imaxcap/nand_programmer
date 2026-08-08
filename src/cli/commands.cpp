#include "nandprog/commands.hpp"

#include "nandprog/error.hpp"
#include "nandprog/util.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>

namespace nandprog {
namespace {

class ProgressDisplay {
public:
    ProgressDisplay(std::string operation, std::uint64_t total)
        : operation_(std::move(operation)), total_(total) {}

    void update(std::uint64_t value) {
        if (total_ == 0)
            return;
        const unsigned percent = static_cast<unsigned>(
            std::min<std::uint64_t>(100, value * 100 / total_));
        if (percent == last_percent_)
            return;
        last_percent_ = percent;
        std::cerr << '\r' << operation_ << ": " << std::setw(3) << percent
                  << "%" << std::flush;
    }

    void finish() {
        update(total_);
        std::cerr << '\n';
    }

private:
    std::string operation_;
    std::uint64_t total_;
    unsigned last_percent_ = std::numeric_limits<unsigned>::max();
};

void print_bad_block(const BadBlockEvent &event) {
    std::cerr << "\n" << (event.skipped ? "Skipped bad block at "
                                           : "Bad block reported at ")
              << hex_number(event.address) << ", size "
              << hex_number(event.size) << '\n';
}

std::uint64_t checked_product(std::uint64_t left, std::uint64_t right,
                              const std::string &description) {
    if (right != 0 && left > UINT64_MAX / right)
        throw Error(description + " is too large");
    return left * right;
}

std::uint64_t rounded_up(std::uint64_t value, std::uint64_t alignment) {
    if (alignment == 0 || value > UINT64_MAX - (alignment - 1))
        throw Error("Size is too large");
    return (value + alignment - 1) / alignment * alignment;
}

std::uint64_t input_file_size(const std::filesystem::path &path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error)
        throw Error("Failed to read file size for " + path.string() + ": " +
                    error.message());
    if (size == 0)
        throw Error("Input file is empty: " + path.string());
    return size;
}

std::string format_chip_id(const protocol::ChipId &id) {
    std::ostringstream output;
    for (const auto byte : id.bytes)
        output << ' ' << hex_number(byte, 2);
    return output.str();
}

void print_firmware_version(const protocol::FirmwareVersion &version) {
    std::cout << "Programmer firmware " << static_cast<unsigned>(version.major)
              << '.' << static_cast<unsigned>(version.minor) << '.'
              << version.build << '\n';
}

void print_chip_id(const protocol::ChipId &id) {
    std::cout << "NAND ID (" << id.bytes.size()
              << (id.bytes.size() == 1 ? " byte):" : " bytes):")
              << format_chip_id(id) << '\n';
}

} // namespace

CommandShell::CommandShell(GlobalOptions options,
                           std::unique_ptr<Transport> transport)
    : options_(std::move(options)), transport_(std::move(transport)),
      client_(*transport_) {
    database_.load(options_.database);
}

int CommandShell::run_repl() {
    std::cout << "nandprog 0.1.1 - type 'help' for commands\n";
    std::string line;
    while (true) {
        std::cout << "nand> " << std::flush;
        if (!std::getline(std::cin, line)) {
            std::cout << '\n';
            return 0;
        }
        try {
            const auto arguments = split_command_line(line);
            if (arguments.empty())
                continue;
            if (arguments.front() == "exit" || arguments.front() == "quit")
                return 0;
            (void)execute(arguments, true);
        } catch (const VerifyMismatch &error) {
            std::cerr << "verify failed: " << error.what() << '\n';
        } catch (const Error &error) {
            std::cerr << "error: " << error.what() << '\n';
        } catch (const std::exception &error) {
            std::cerr << "unexpected error: " << error.what() << '\n';
        }
    }
}

int CommandShell::execute(const std::vector<std::string> &arguments,
                          bool interactive) {
    if (arguments.empty())
        return 0;
    const std::string &command = arguments.front();
    if (command == "help" || command == "--help" || command == "-h")
        print_help();
    else if (command == "id")
        command_id(arguments);
    else if (command == "probe")
        command_probe(arguments);
    else if (command == "info") {
        ensure_probe();
        command_info();
    } else if (command == "read")
        command_read(arguments, false);
    else if (command == "read.raw")
        command_read(arguments, true);
    else if (command == "erase")
        command_erase(arguments, interactive);
    else if (command == "write")
        command_write(arguments, false);
    else if (command == "write.raw")
        command_write(arguments, true);
    else if (command == "verify" || command == "verify.raw")
        command_verify(arguments);
    else if (command == "write.qpic")
        throw Error("write.qpic is reserved but QPIC ECC/layout is not implemented");
    else if (command == "exit" || command == "quit")
        return 0;
    else
        throw Error("Unknown command: " + command + "; use 'help'");
    return 0;
}

void CommandShell::ensure_open() {
    if (!transport_->is_open())
        transport_->open(options_.device, 4000000);
}

void CommandShell::ensure_probe() {
    if (!probed_)
        command_probe({"probe"});
}

void CommandShell::command_id(const std::vector<std::string> &arguments) {
    if (arguments.size() != 1)
        throw Error("Usage: id");
    ensure_open();

    firmware_version_ = client_.firmware_version();
    if (!probed_)
        client_.configure(database_.first());
    chip_id_ = client_.read_id();

    print_firmware_version(firmware_version_);
    print_chip_id(chip_id_);
    const Chip *match = database_.find_by_id(chip_id_);
    if (match == nullptr)
        std::cout << "Database match: none\n";
    else
        std::cout << "Database match: " << match->name << '\n';
}

void CommandShell::command_probe(const std::vector<std::string> &arguments) {
    if (arguments.size() > 2)
        throw Error("Usage: probe [chip-name]");
    ensure_open();

    firmware_version_ = client_.firmware_version();
    std::optional<std::string> forced = options_.forced_chip;
    if (arguments.size() == 2)
        forced = arguments[1];

    if (forced) {
        chip_ = database_.find_by_name(*forced);
        if (chip_ == nullptr)
            throw Error("Chip not found in database: " + *forced);
        client_.configure(*chip_);
        chip_id_ = client_.read_id();
    } else {
        client_.configure(database_.first());
        chip_id_ = client_.read_id();
        print_firmware_version(firmware_version_);
        print_chip_id(chip_id_);
        chip_ = database_.find_by_id(chip_id_);
        if (chip_ == nullptr) {
            std::ostringstream message;
            message << "Unknown NAND ID:" << format_chip_id(chip_id_);
            message << "; add it to the database or use 'probe CHIP-NAME'";
            throw Error(message.str());
        }
        client_.configure(*chip_);
    }

    probed_ = true;
    if (forced) {
        print_firmware_version(firmware_version_);
        print_chip_id(chip_id_);
    }
    std::cout << "Detected: " << chip_->name << '\n';
}

void CommandShell::command_info() const {
    if (!probed_ || chip_ == nullptr)
        throw Error("Run probe first");
    std::cout << "Device:          " << options_.device << '\n'
              << "Chip:            " << chip_->name << '\n'
              << "NAND ID:         " << format_chip_id(chip_id_) << '\n'
              << "Data page:       " << chip_->page_size << " bytes\n"
              << "OOB:             " << chip_->spare_size << " bytes\n"
              << "Raw page:        " << chip_->raw_page_size() << " bytes\n"
              << "Block:           " << chip_->block_size << " data bytes\n"
              << "Pages:           " << chip_->page_count() << '\n'
              << "Data capacity:   " << chip_->total_size << " bytes\n"
              << "Raw capacity:    " << chip_->raw_total_size() << " bytes\n"
              << "BB mark offset:  "
              << static_cast<unsigned>(chip_->bad_block_mark_offset) << '\n'
              << "Geometry source: " << options_.database.string() << '\n';
}

void CommandShell::command_read(const std::vector<std::string> &arguments,
                                bool raw) {
    if (arguments.size() < 2 || arguments.size() > 4)
        throw Error(raw ? "Usage: read.raw FILE [START-PAGE] [PAGE-COUNT]"
                        : "Usage: read FILE [OFFSET] [LENGTH]");
    ensure_probe();

    std::uint64_t address = 0;
    std::uint64_t length = 0;
    if (raw) {
        const std::uint64_t start_page =
            arguments.size() >= 3 ? parse_number(arguments[2]) : 0;
        if (start_page >= chip_->page_count())
            throw Error("Raw read start page is outside the chip");
        const std::uint64_t page_count = arguments.size() >= 4
                                             ? parse_number(arguments[3])
                                             : chip_->page_count() - start_page;
        if (page_count == 0 || page_count > chip_->page_count() - start_page)
            throw Error("Raw read page range is outside the chip");
        address = checked_product(start_page, chip_->raw_page_size(),
                                  "Raw read address");
        length = checked_product(page_count, chip_->raw_page_size(),
                                 "Raw read length");
    } else {
        address = arguments.size() >= 3 ? parse_number(arguments[2]) : 0;
        if (address >= chip_->total_size)
            throw Error("Read offset is outside the chip");
        length = arguments.size() >= 4 ? parse_number(arguments[3])
                                       : chip_->total_size - address;
        if (address % chip_->page_size != 0 || length == 0 ||
            length % chip_->page_size != 0)
            throw Error("Read offset and length must be data-page aligned");
        if (length > chip_->total_size - address)
            throw Error("Read range is outside the chip");
    }

    std::ofstream output(arguments[1], std::ios::binary | std::ios::trunc);
    if (!output)
        throw Error("Failed to open output file: " + arguments[1]);

    ProgressDisplay progress(raw ? "read.raw" : "read", length);
    protocol::Flags flags;
    flags.skip_bad = !raw;
    flags.include_spare = raw;
    client_.read(
        address, length, flags,
        [&output](const std::uint8_t *data, std::size_t size) {
            output.write(reinterpret_cast<const char *>(data),
                         static_cast<std::streamsize>(size));
            if (!output)
                throw Error("Failed to write output file");
        },
        [&progress](std::uint64_t value) { progress.update(value); },
        print_bad_block);
    output.close();
    if (!output)
        throw Error("Failed to finalize output file: " + arguments[1]);
    progress.finish();
    std::cout << "Read " << length << " bytes to " << arguments[1] << '\n';
}

void CommandShell::command_erase(const std::vector<std::string> &arguments,
                                 bool interactive) {
    bool confirmed = false;
    std::vector<std::string> values;
    values.push_back("erase");
    for (std::size_t index = 1; index < arguments.size(); ++index) {
        if (arguments[index] == "--yes")
            confirmed = true;
        else
            values.push_back(arguments[index]);
    }
    if (!((values.size() == 2 && values[1] == "all") || values.size() == 3))
        throw Error("Usage: erase all [--yes] | erase OFFSET LENGTH [--yes]");
    ensure_probe();

    const std::uint64_t address = values[1] == "all" ? 0 : parse_number(values[1]);
    const std::uint64_t length = values[1] == "all" ? chip_->total_size
                                                     : parse_number(values[2]);
    if (address % chip_->block_size != 0 || length == 0 ||
        length % chip_->block_size != 0)
        throw Error("Erase offset and length must be block aligned");
    if (address >= chip_->total_size || length > chip_->total_size - address)
        throw Error("Erase range is outside the chip");

    if (!confirmed && interactive) {
        std::cout << "Erase " << hex_number(length) << " bytes at "
                  << hex_number(address) << "? Type 'yes': " << std::flush;
        std::string answer;
        if (!std::getline(std::cin, answer) || answer != "yes") {
            std::cout << "Erase cancelled\n";
            return;
        }
        confirmed = true;
    }
    if (!confirmed)
        throw Error("Destructive erase requires --yes in one-shot mode");

    ProgressDisplay progress("erase", length);
    protocol::Flags flags;
    flags.skip_bad = true;
    client_.erase(address, length, flags,
                  [&progress](std::uint64_t value) { progress.update(value); },
                  print_bad_block);
    progress.finish();
    std::cout << "Erase completed\n";
}

void CommandShell::command_write(const std::vector<std::string> &arguments,
                                 bool raw) {
    if (arguments.size() < 2 || arguments.size() > 3)
        throw Error(raw ? "Usage: write.raw FILE [START-PAGE]"
                        : "Usage: write FILE [OFFSET]");
    ensure_probe();

    const std::filesystem::path path = arguments[1];
    const std::uint64_t input_size = input_file_size(path);
    const std::uint64_t transfer_page_size =
        raw ? chip_->raw_page_size() : chip_->page_size;
    std::uint64_t address = 0;
    std::uint64_t length = 0;
    if (raw) {
        if (input_size % transfer_page_size != 0)
            throw Error("Raw image size must be an exact multiple of data+OOB page size");
        const std::uint64_t start_page =
            arguments.size() == 3 ? parse_number(arguments[2]) : 0;
        if (start_page >= chip_->page_count())
            throw Error("Raw write start page is outside the chip");
        address = checked_product(start_page, transfer_page_size,
                                  "Raw write address");
        length = input_size;
        if (length > chip_->raw_total_size() - address)
            throw Error("Raw image does not fit in the selected chip range");
        if (firmware_version_.major == 3 && firmware_version_.minor <= 5) {
            std::cerr << "warning: firmware 3.5.x acknowledges write-end before "
                         "checking the final NAND busy/status; run verify --raw\n";
        }
    } else {
        address = arguments.size() == 3 ? parse_number(arguments[2]) : 0;
        if (address % chip_->page_size != 0)
            throw Error("Write offset must be data-page aligned");
        length = rounded_up(input_size, chip_->page_size);
        if (address >= chip_->total_size || length > chip_->total_size - address)
            throw Error("Image does not fit in the selected chip range");
    }

    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw Error("Failed to open input file: " + path.string());
    ProgressDisplay progress(raw ? "write.raw" : "write", length);
    protocol::Flags flags;
    flags.skip_bad = !raw;
    flags.include_spare = raw;
    flags.enable_hardware_ecc = false;
    client_.write(input, address, length,
                  static_cast<std::uint32_t>(transfer_page_size), flags,
                  [&progress](std::uint64_t value) { progress.update(value); },
                  print_bad_block);
    progress.finish();
    std::cout << "Wrote " << length << " bytes from " << path.string() << '\n';
}

void CommandShell::command_verify(const std::vector<std::string> &arguments) {
    bool raw = arguments.front() == "verify.raw";
    std::vector<std::string> values;
    values.push_back("verify");
    for (std::size_t index = 1; index < arguments.size(); ++index) {
        if (arguments[index] == "--raw")
            raw = true;
        else
            values.push_back(arguments[index]);
    }
    if (values.size() < 2 || values.size() > 3)
        throw Error("Usage: verify FILE [OFFSET] [--raw]");
    ensure_probe();

    const std::filesystem::path path = values[1];
    const std::uint64_t input_size = input_file_size(path);
    const std::uint64_t transfer_page_size =
        raw ? chip_->raw_page_size() : chip_->page_size;
    std::uint64_t address = 0;
    std::uint64_t length = 0;
    if (raw) {
        if (input_size % transfer_page_size != 0)
            throw Error("Raw verify file must be an exact multiple of data+OOB page size");
        const std::uint64_t start_page =
            values.size() == 3 ? parse_number(values[2]) : 0;
        if (start_page >= chip_->page_count())
            throw Error("Raw verify start page is outside the chip");
        address = checked_product(start_page, transfer_page_size,
                                  "Raw verify address");
        length = input_size;
        if (length > chip_->raw_total_size() - address)
            throw Error("Raw verify range is outside the chip");
    } else {
        address = values.size() == 3 ? parse_number(values[2]) : 0;
        if (address % chip_->page_size != 0)
            throw Error("Verify offset must be data-page aligned");
        length = rounded_up(input_size, chip_->page_size);
        if (address >= chip_->total_size || length > chip_->total_size - address)
            throw Error("Verify range is outside the chip");
    }

    std::ifstream expected(path, std::ios::binary);
    if (!expected)
        throw Error("Failed to open verify file: " + path.string());

    struct Mismatch {
        std::uint64_t offset;
        std::uint8_t expected;
        std::uint8_t actual;
    };
    std::optional<Mismatch> mismatch;
    std::uint64_t compared = 0;
    ProgressDisplay progress(raw ? "verify.raw" : "verify", length);
    protocol::Flags flags;
    flags.skip_bad = !raw;
    flags.include_spare = raw;
    client_.read(
        address, length, flags,
        [&](const std::uint8_t *actual, std::size_t size) {
            std::vector<std::uint8_t> wanted(size, 0xff);
            expected.read(reinterpret_cast<char *>(wanted.data()),
                          static_cast<std::streamsize>(size));
            const auto count = expected.gcount();
            if (expected.bad())
                throw Error("Failed while reading verify file");
            if (raw && count != static_cast<std::streamsize>(size))
                throw Error("Raw verify file ended unexpectedly");
            if (!mismatch) {
                for (std::size_t index = 0; index < size; ++index) {
                    if (wanted[index] != actual[index]) {
                        mismatch = Mismatch{compared + index, wanted[index],
                                            actual[index]};
                        break;
                    }
                }
            }
            compared += size;
        },
        [&progress](std::uint64_t value) { progress.update(value); },
        print_bad_block);
    progress.finish();

    if (mismatch) {
        std::ostringstream message;
        message << "mismatch at image offset " << hex_number(mismatch->offset)
                << ": expected " << hex_number(mismatch->expected, 2)
                << ", got " << hex_number(mismatch->actual, 2);
        throw VerifyMismatch(mismatch->offset, mismatch->expected,
                             mismatch->actual, message.str());
    }
    std::cout << "Verified " << length << " bytes successfully\n";
}

void CommandShell::print_help() {
    std::cout
        << "Commands:\n"
        << "  id                                        print raw NAND ID bytes\n"
        << "  probe [chip-name]                         detect and configure NAND\n"
        << "  info                                      show active NAND geometry\n"
        << "  read FILE [OFFSET] [LENGTH]               read data area\n"
        << "  read.raw FILE [START-PAGE] [PAGE-COUNT]   read data+OOB verbatim\n"
        << "  erase all [--yes]                         erase the complete NAND\n"
        << "  erase OFFSET LENGTH [--yes]               erase block-aligned range\n"
        << "  write FILE [OFFSET]                       write data, pad tail with FF\n"
        << "  write.raw FILE [START-PAGE]               write data+OOB verbatim\n"
        << "  verify FILE [OFFSET] [--raw]              stream-compare NAND to file\n"
        << "  write.qpic ...                            reserved for future QPIC ECC\n"
        << "  help                                      show this help\n"
        << "  exit                                      leave the REPL\n"
        << "Numbers accept decimal, 0x hexadecimal, and K/M/G suffixes.\n";
}

} // namespace nandprog
