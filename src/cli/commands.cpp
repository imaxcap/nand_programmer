#include "nandprog/commands.hpp"

#include "nandprog/error.hpp"
#include "nandprog/qpic.hpp"
#include "nandprog/util.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <thread>

namespace nandprog {
namespace {

inline std::optional<unsigned> parse_duration_seconds(const std::string &str) {
    if (str.empty())
        return std::nullopt;
    unsigned multiplier = 1;
    std::string num_str = str;
    char unit = str.back();
    if (unit == 's' || unit == 'S') {
        multiplier = 1;
        num_str.pop_back();
    } else if (unit == 'm' || unit == 'M') {
        multiplier = 60;
        num_str.pop_back();
    } else if (unit == 'h' || unit == 'H') {
        multiplier = 3600;
        num_str.pop_back();
    }
    try {
        return static_cast<unsigned>(std::stoul(num_str) * multiplier);
    } catch (...) {
        return std::nullopt;
    }
}

inline std::string format_data_size(std::uint64_t bytes) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(2);
    if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
        ss << (static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0)) << " GiB";
    } else if (bytes >= 1024ULL * 1024ULL) {
        ss << (static_cast<double>(bytes) / (1024.0 * 1024.0)) << " MiB";
    } else if (bytes >= 1024ULL) {
        ss << (static_cast<double>(bytes) / 1024.0) << " KiB";
    } else {
        ss << bytes << " B";
    }
    return ss.str();
}

inline std::string format_throughput(double bytes_per_sec) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(2);
    if (bytes_per_sec >= 1024.0 * 1024.0) {
        ss << (bytes_per_sec / (1024.0 * 1024.0)) << " MB/s";
    } else if (bytes_per_sec >= 1024.0) {
        ss << (bytes_per_sec / 1024.0) << " KB/s";
    } else {
        ss << bytes_per_sec << " B/s";
    }
    return ss.str();
}

inline std::string format_duration(double seconds) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(1);
    if (seconds >= 60.0) {
        const unsigned minutes = static_cast<unsigned>(seconds) / 60;
        const double rem_sec = seconds - minutes * 60.0;
        ss << minutes << "m " << rem_sec << "s";
    } else {
        ss << seconds << "s";
    }
    return ss.str();
}

class ProgressDisplay {
public:
    ProgressDisplay(std::string operation, std::uint64_t total)
        : operation_(std::move(operation)), total_(total),
          start_time_(std::chrono::steady_clock::now()),
          last_update_time_(start_time_) {}

    void update(std::uint64_t value) {
        if (total_ == 0)
            return;
        const auto now = std::chrono::steady_clock::now();
        const unsigned percent = static_cast<unsigned>(
            std::min<std::uint64_t>(100, value * 100 / total_));

        const double elapsed_since_last =
            std::chrono::duration<double>(now - last_update_time_).count();
        if (percent == last_percent_ && elapsed_since_last < 0.15)
            return;

        last_percent_ = percent;
        last_update_time_ = now;

        const double elapsed =
            std::chrono::duration<double>(now - start_time_).count();
        const double speed = (elapsed > 0.001)
                                 ? (static_cast<double>(value) / elapsed)
                                 : 0.0;

        std::cerr << '\r' << operation_ << ": " << std::setw(3) << percent
                  << "% (" << format_data_size(value) << " / "
                  << format_data_size(total_) << ", "
                  << format_throughput(speed) << ")    " << std::flush;
    }

    void finish() {
        const auto now = std::chrono::steady_clock::now();
        const double elapsed =
            std::chrono::duration<double>(now - start_time_).count();
        const double avg_speed = (elapsed > 0.001)
                                     ? (static_cast<double>(total_) / elapsed)
                                     : 0.0;

        std::cerr << '\r' << operation_ << ": 100% ["
                  << format_data_size(total_) << " in "
                  << format_duration(elapsed) << ", "
                  << format_throughput(avg_speed) << "]          \n"
                  << std::flush;
    }

    void log_bad_block(const BadBlockEvent &event) {
        ++bad_blocks_;
        std::ostringstream ss;
        ss << (event.skipped ? "[WARN] Skipped bad block at "
                             : "[WARN] Bad block reported at ")
           << hex_number(event.address) << ", size "
           << hex_number(event.size);
        std::cerr << "\r" << std::string(79, ' ') << "\r" << ss.str() << '\n'
                  << std::flush;
        last_percent_ = std::numeric_limits<unsigned>::max();
    }

    std::size_t bad_blocks() const noexcept { return bad_blocks_; }

private:
    std::string operation_;
    std::uint64_t total_;
    unsigned last_percent_ = std::numeric_limits<unsigned>::max();
    std::size_t bad_blocks_ = 0;
    std::chrono::steady_clock::time_point start_time_;
    std::chrono::steady_clock::time_point last_update_time_;
};

void print_bad_block(const BadBlockEvent &event) {
    std::cerr << (event.skipped ? "[WARN] Skipped bad block at "
                                : "[WARN] Bad block reported at ")
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
    std::cout << "nandprog 0.2.0 - type 'help' for commands\n";
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
            (void)execute(arguments);
        } catch (const VerifyMismatch &error) {
            std::cerr << "verify failed: " << error.what() << '\n';
        } catch (const Error &error) {
            std::cerr << "error: " << error.what() << '\n';
        } catch (const std::exception &error) {
            std::cerr << "unexpected error: " << error.what() << '\n';
        }
    }
}

int CommandShell::execute(const std::vector<std::string> &arguments) {
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
        command_erase(arguments);
    else if (command == "scrub")
        command_scrub(arguments);
    else if (command == "test" || command == "nandtest" || command == "test.write" || command == "test.verify")
        command_test(arguments);
    else if (command == "write")
        command_write(arguments, false);
    else if (command == "write.raw")
        command_write(arguments, true);
    else if (command == "verify" || command == "verify.raw")
        command_verify(arguments);
    else if (command == "write.qpic")
        command_write_qpic(arguments);
    else if (command == "read.qpic")
        command_read_qpic(arguments);
    else if (command == "verify.qpic")
        command_verify_qpic(arguments);
    else if (command == "flash")
        command_flash(arguments);
    else if (command == "part" || command == "partitions" || command == "mibib" || command == "smem")
        command_part(arguments);
    else if (command == "debug" || command == "verbose") {
        if (arguments.size() >= 2 && (arguments[1] == "off" || arguments[1] == "0" || arguments[1] == "false")) {
            set_debug_enabled(false);
            std::cout << "Debug logging disabled\n";
        } else {
            set_debug_enabled(true);
            std::cout << "Debug logging enabled\n";
        }
    } else if (command == "exit" || command == "quit")
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

void CommandShell::ensure_firmware_version(unsigned min_major, unsigned min_minor,
                                           const std::string &feature_name) {
    ensure_probe();
    if (firmware_version_.major < min_major ||
        (firmware_version_.major == min_major && firmware_version_.minor < min_minor)) {
        throw Error("Command '" + feature_name + "' requires firmware v" +
                    std::to_string(min_major) + "." + std::to_string(min_minor) +
                    ".0 or later (device is currently running v" +
                    std::to_string(firmware_version_.major) + "." +
                    std::to_string(firmware_version_.minor) + "." +
                    std::to_string(firmware_version_.build) + ").\n" +
                    "Please update your STM32 firmware to v3.6.0 to use this command.");
    }
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
        print_firmware_version(firmware_version_);
        print_chip_id(chip_id_);
    } else {
        client_.configure(database_.first());
        chip_id_ = client_.read_id();
        print_firmware_version(firmware_version_);
        print_chip_id(chip_id_);

        auto onfi = client_.probe_onfi();
        if (onfi) {
            std::cout << "[ONFI 1.0 Match] Manufacturer: " << (onfi->manufacturer.empty() ? "Generic" : onfi->manufacturer)
                      << ", Model: " << (onfi->model.empty() ? "ONFI NAND" : onfi->model) << '\n';
            Chip c;
            c.name = onfi->model.empty() ? onfi->manufacturer : onfi->model;
            c.page_size = onfi->page_size;
            c.spare_size = onfi->spare_size;
            c.block_size = onfi->block_size;
            c.total_size = onfi->total_size;
            c.bad_block_mark_offset = 0;
            c.parameters.fill(0);
            c.parameters[0] = 20; // t_cs
            c.parameters[1] = 12; // t_cls
            c.parameters[2] = 12; // t_als
            c.parameters[3] = 10; // t_clr
            c.parameters[4] = 10; // t_ar
            c.parameters[5] = 12; // t_wp
            c.parameters[6] = 12; // t_rp
            c.parameters[7] = 12; // t_ds
            c.parameters[8] = 5;  // t_ch
            c.parameters[9] = 5;  // t_clh
            c.parameters[10] = 5; // t_alh
            c.parameters[11] = 25;// t_wc
            c.parameters[12] = 25;// t_rc
            c.parameters[13] = 20;// t_rea
            c.parameters[14] = onfi->row_cycles;
            c.parameters[15] = onfi->col_cycles;
            c.parameters[16] = 0x00; // read1
            c.parameters[17] = 0x30; // read2
            c.parameters[18] = 0xFF; // read_spare
            c.parameters[19] = 0x90; // read_id
            c.parameters[20] = 0xFF; // reset
            c.parameters[21] = 0x80; // write1
            c.parameters[22] = 0x10; // write2
            c.parameters[23] = 0x60; // erase1
            c.parameters[24] = 0xD0; // erase2
            c.parameters[25] = 0x70; // status
            c.parameters[26] = 0xEF; // set_features
            c.parameters[27] = 0x90; // enable_ecc_address
            c.parameters[28] = 0x08; // enable_ecc_value
            c.parameters[29] = 0x00; // disable_ecc_value
            dynamic_chip_ = c;
            chip_ = &(*dynamic_chip_);
        } else {
            chip_ = database_.find_by_id(chip_id_);
            if (chip_ == nullptr) {
                std::ostringstream message;
                message << "Unknown NAND ID:" << format_chip_id(chip_id_);
                message << "; add it to the database or use 'probe CHIP-NAME'";
                throw Error(message.str());
            }
            client_.configure(*chip_);
        }
    }

    cached_mibib_ = std::nullopt;
    probed_ = true;
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

    transport_->flush();
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
        [&progress](const BadBlockEvent &event) { progress.log_bad_block(event); });
    output.close();
    if (!output)
        throw Error("Failed to finalize output file: " + arguments[1]);
    progress.finish();
    std::cout << "Read " << length << " bytes to " << arguments[1] << '\n';
}

void CommandShell::command_erase(const std::vector<std::string> &arguments) {
    const bool erase_all = arguments.size() == 2 && arguments[1] == "all";
    const bool erase_range =
        arguments.size() == 3 && arguments[1] != "all";
    if (!erase_all && !erase_range)
        throw Error("Usage: erase all | erase OFFSET LENGTH");
    ensure_probe();

    const std::uint64_t address =
        arguments[1] == "all" ? 0 : parse_number(arguments[1]);
    const std::uint64_t length = arguments[1] == "all"
                                     ? chip_->total_size
                                     : parse_number(arguments[2]);
    if (address % chip_->block_size != 0 || length == 0 ||
        length % chip_->block_size != 0)
        throw Error("Erase offset and length must be block aligned");
    if (address >= chip_->total_size || length > chip_->total_size - address)
        throw Error("Erase range is outside the chip");

    const std::uint64_t block_count = length / chip_->block_size;
    std::cout << "Erasing " << (erase_all ? "entire chip" : "range") << " [offset "
              << hex_number(address) << ", length " << hex_number(length)
              << " (" << format_data_size(length) << ", " << block_count << " blocks)]...\n";

    transport_->flush();
    ProgressDisplay progress("erase", length);
    protocol::Flags flags;
    flags.skip_bad = true;
    client_.erase(address, length, flags,
                  [&progress](std::uint64_t value) { progress.update(value); },
                  [&progress](const BadBlockEvent &event) { progress.log_bad_block(event); });
    progress.finish();

    if (progress.bad_blocks() > 0) {
        std::cout << "Erase completed successfully (" << block_count << " blocks, "
                  << progress.bad_blocks() << " bad blocks skipped)\n";
    } else {
        std::cout << "Erase completed successfully (" << block_count << " blocks)\n";
    }
}

void CommandShell::command_scrub(const std::vector<std::string> &arguments) {
    ensure_firmware_version(3, 6, "scrub");

    bool scrub_all = (arguments.size() == 1 || (arguments.size() == 2 && arguments[1] == "all"));
    std::uint64_t address = 0;
    std::uint64_t length = chip_->total_size;

    if (!scrub_all) {
        if (arguments.size() != 3)
            throw Error("Usage: scrub [all | OFFSET LENGTH]");
        address = parse_number(arguments[1]);
        length = parse_number(arguments[2]);
    }

    if (address % chip_->block_size != 0 || length == 0 ||
        length % chip_->block_size != 0)
        throw Error("Scrub offset and length must be block aligned");
    if (address >= chip_->total_size || length > chip_->total_size - address)
        throw Error("Scrub range is outside the chip");

    const std::uint64_t block_count = length / chip_->block_size;

    std::cerr << "\n"
              << "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n"
              << "!! WARNING: SCRUB WILL UNCONDITIONALLY ERASE ALL PHYSICAL BLOCKS!!\n"
              << "!! THIS WILL DESTROY FACTORY BAD BLOCK MARKERS IN OOB!          !!\n"
              << "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n"
              << "Target: " << (scrub_all ? "entire chip" : "range") << " [offset "
              << hex_number(address) << ", length " << hex_number(length)
              << " (" << format_data_size(length) << ", " << block_count << " blocks)]\n\n"
              << "Type 'YES' to proceed with physical scrub: " << std::flush;

    std::string confirm;
    if (std::cin >> confirm && confirm != "YES") {
        std::cout << "Scrub cancelled.\n";
        return;
    }

    std::cout << "Scrubbing " << (scrub_all ? "entire chip" : "range") << " [offset "
              << hex_number(address) << ", length " << hex_number(length)
              << " (" << format_data_size(length) << ", " << block_count << " blocks)]...\n";

    transport_->flush();
    ProgressDisplay progress("scrub", length);
    client_.scrub(address, length,
                  [&progress](std::uint64_t value) { progress.update(value); },
                  [&progress](const BadBlockEvent &event) { progress.log_bad_block(event); });
    progress.finish();

    std::cout << "Scrub completed successfully (" << block_count << " physical blocks erased)\n";
}

void CommandShell::command_test(const std::vector<std::string> &arguments) {
    ensure_firmware_version(3, 6, "test");

    const std::string &cmd_name = arguments.front();
    protocol::TestMode mode = protocol::TestMode::full_chip; // default to full-disk RDT spanning
    if (cmd_name == "test.write")
        mode = protocol::TestMode::write_only;
    else if (cmd_name == "test.verify")
        mode = protocol::TestMode::verify_only;

    unsigned passes = 1;
    bool mark_bad = true; // default is true!
    std::optional<std::uint32_t> custom_seed;
    std::optional<unsigned> delay_seconds;
    std::vector<std::string> pos_args;

    for (std::size_t i = 1; i < arguments.size(); ++i) {
        const std::string &arg = arguments[i];
        if (arg == "--no-mark-bad") {
            mark_bad = false;
        } else if (arg == "--mark-bad") {
            mark_bad = true;
        } else if (arg == "--mode" || arg == "-m") {
            if (++i >= arguments.size())
                throw Error("Option --mode requires 'block' (per-block) or 'chip'/'rdt' (full-disk write-then-read)");
            if (arguments[i] == "block" || arguments[i] == "per-block")
                mode = protocol::TestMode::full_block;
            else if (arguments[i] == "chip" || arguments[i] == "rdt" || arguments[i] == "full")
                mode = protocol::TestMode::full_chip;
            else
                throw Error("Unknown test mode: " + arguments[i] + "; use 'block' or 'chip'");
        } else if (arg == "--per-block" || arg == "--block" || arg == "-b") {
            mode = protocol::TestMode::full_block;
        } else if (arg == "--rdt" || arg == "--full-chip") {
            mode = protocol::TestMode::full_chip;
        } else if (arg == "--passes" || arg == "-p") {
            if (++i >= arguments.size())
                throw Error("Option --passes requires an integer count");
            passes = static_cast<unsigned>(parse_number(arguments[i]));
            if (passes == 0)
                throw Error("Test passes count must be >= 1");
        } else if (arg == "--seed" || arg == "-s") {
            if (++i >= arguments.size())
                throw Error("Option --seed requires a 32-bit number");
            custom_seed = static_cast<std::uint32_t>(parse_number(arguments[i]));
        } else if (arg == "--delay" || arg == "-d") {
            if (++i >= arguments.size())
                throw Error("Option --delay requires a duration (e.g. 10m or 60s)");
            auto dur = parse_duration_seconds(arguments[i]);
            if (!dur)
                throw Error("Invalid delay duration: " + arguments[i]);
            delay_seconds = *dur;
        } else {
            pos_args.push_back(arg);
        }
    }

    std::uint64_t address = 0;
    std::uint64_t length = chip_->total_size;
    std::string target_desc = "entire chip";

    if (!pos_args.empty() && pos_args[0] != "all") {
        auto table = read_mibib_table(false);
        bool found_part = false;
        if (table) {
            for (const auto &part : table->partitions) {
                if (part.name == pos_args[0] || part.name == ("0:" + pos_args[0])) {
                    address = part.start_offset;
                    length = (pos_args.size() >= 2) ? parse_number(pos_args[1]) : part.size_bytes;
                    target_desc = "partition '" + part.name + "'";
                    found_part = true;
                    break;
                }
            }
        }
        if (!found_part) {
            address = parse_number(pos_args[0]);
            if (pos_args.size() < 2)
                throw Error("Length is required when testing by offset: test OFFSET LENGTH");
            length = parse_number(pos_args[1]);
            target_desc = "offset " + hex_number(address);
        }
    }

    if (address % chip_->block_size != 0 || length == 0 || length % chip_->block_size != 0)
        throw Error("Test offset and length must be block aligned");
    if (address >= chip_->total_size || length > chip_->total_size - address)
        throw Error("Test range is outside the chip");

    const std::uint64_t block_count = length / chip_->block_size;
    const std::uint32_t base_seed = custom_seed.value_or(0x5A5AA5A5U);

    if (delay_seconds && (mode == protocol::TestMode::full_chip || mode == protocol::TestMode::full_block)) {
        std::cout << "Starting retention self-test on " << target_desc << " [offset "
                  << hex_number(address) << ", length " << hex_number(length)
                  << " (" << format_data_size(length) << ", " << block_count << " blocks)]...\n";

        std::cout << "\n[Phase 1/2] Writing PRBS32 random pattern + ECC (seed: "
                  << hex_number(base_seed) << ")...\n";
        transport_->flush();
        ProgressDisplay write_prog("test.write", length);
        client_.nand_test(address, length, protocol::TestMode::write_only, mark_bad, base_seed,
                          [&write_prog](std::uint64_t val) { write_prog.update(val); },
                          [&write_prog](const BadBlockEvent &ev) { write_prog.log_bad_block(ev); });
        write_prog.finish();

        std::cout << "\n[Retention Window] Waiting for " << format_duration(*delay_seconds)
                  << " for charge retention testing...\n";
        const auto start_wait = std::chrono::steady_clock::now();
        const auto end_wait = start_wait + std::chrono::seconds(*delay_seconds);
        while (std::chrono::steady_clock::now() < end_wait) {
            const auto rem = std::chrono::duration_cast<std::chrono::seconds>(
                end_wait - std::chrono::steady_clock::now()).count();
            std::cerr << "\rRemaining retention time: " << format_duration(static_cast<double>(rem)) << "    " << std::flush;
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        std::cerr << "\rRemaining retention time: 0.0s                      \n" << std::flush;

        std::cout << "\n[Phase 2/2] Reading back and verifying PRBS32 + ECC...\n";
        transport_->flush();
        ProgressDisplay verify_prog("test.verify", length);
        client_.nand_test(address, length, protocol::TestMode::verify_only, mark_bad, base_seed,
                          [&verify_prog](std::uint64_t val) { verify_prog.update(val); },
                          [&verify_prog](const BadBlockEvent &ev) { verify_prog.log_bad_block(ev); });
        verify_prog.finish();

        std::cout << "\nRetention test completed on " << target_desc << " (" << block_count << " blocks, "
                  << verify_prog.bad_blocks() << " bad blocks marked)\n";
        return;
    }

    for (unsigned pass = 1; pass <= passes; ++pass) {
        const std::uint32_t pass_seed = base_seed ^ (static_cast<std::uint32_t>(pass - 1) * 0x9E3779B9U);
        std::string mode_desc = (mode == protocol::TestMode::write_only) ? "test.write (write-only baseline)" :
                                (mode == protocol::TestMode::verify_only) ? "test.verify (verify retention)" :
                                (mode == protocol::TestMode::full_block) ? "test (per-block write-then-read)" :
                                "test (SSD RDT full-disk spanning)";
        std::string tag = (mode == protocol::TestMode::write_only) ? "test.write" :
                          (mode == protocol::TestMode::verify_only) ? "test.verify" : "test";

        if (passes > 1)
            std::cout << "\n=== [Pass " << pass << "/" << passes << "] " << mode_desc << " | Seed: " << hex_number(pass_seed) << " ===\n";
        else
            std::cout << "Running " << mode_desc << " on " << target_desc << " [offset "
                      << hex_number(address) << ", length " << hex_number(length)
                      << " (" << format_data_size(length) << ", " << block_count << " blocks)] (Seed: "
                      << hex_number(pass_seed) << ")...\n";

        transport_->flush();
        ProgressDisplay progress(tag, length);
        client_.nand_test(address, length, mode, mark_bad, pass_seed,
                          [&progress](std::uint64_t val) { progress.update(val); },
                          [&progress](const BadBlockEvent &ev) { progress.log_bad_block(ev); });
        progress.finish();

        if (mode == protocol::TestMode::write_only) {
            std::cout << "Test baseline pattern written successfully (seed: " << hex_number(pass_seed) << ").\n"
                      << "You may power off or let the device sit. When ready to verify, run:\n"
                      << "    test.verify --seed " << hex_number(pass_seed) << "\n";
        } else if (progress.bad_blocks() > 0) {
            std::cout << tag << " pass " << pass << " completed with " << progress.bad_blocks()
                      << " damaged blocks marked bad\n";
        } else {
            std::cout << tag << (passes > 1 ? (" pass " + std::to_string(pass)) : "")
                      << " completed successfully (100% healthy, 0 errors)\n";
        }
    }
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
    transport_->flush();
    ProgressDisplay progress(raw ? "write.raw" : "write", length);
    protocol::Flags flags;
    flags.skip_bad = !raw;
    flags.include_spare = raw;
    flags.enable_hardware_ecc = false;
    client_.write(input, address, length,
                  static_cast<std::uint32_t>(transfer_page_size), flags,
                  [&progress](std::uint64_t value) { progress.update(value); },
                  [&progress](const BadBlockEvent &event) { progress.log_bad_block(event); });
    progress.finish();
    std::cout << "Wrote " << length << " bytes from " << path.string() << '\n';
}

void CommandShell::command_write_qpic(
    const std::vector<std::string> &arguments) {
    if (arguments.size() < 2)
        throw Error("Usage: write.qpic FILE [NAND-OFFSET] [--ecc bch4|bch8]");

    std::optional<qpic::EccMode> ecc_mode;
    std::optional<std::uint64_t> nand_offset;
    for (std::size_t index = 2; index < arguments.size(); ++index) {
        if (arguments[index] == "--ecc") {
            if (ecc_mode || ++index >= arguments.size())
                throw Error("Usage: write.qpic FILE [NAND-OFFSET] [--ecc bch4|bch8]");
            if (arguments[index] == "bch4")
                ecc_mode = qpic::EccMode::bch4;
            else if (arguments[index] == "bch8")
                ecc_mode = qpic::EccMode::bch8;
            else
                throw Error("QPIC ECC must be bch4 or bch8");
        } else if (arguments[index].rfind("--", 0) == 0) {
            throw Error("Unknown write.qpic option: " + arguments[index]);
        } else {
            if (nand_offset)
                throw Error("write.qpic accepts only one NAND offset");
            nand_offset = parse_number(arguments[index]);
        }
    }

    ensure_probe();
    if (!ecc_mode) {
        // Auto-detect ECC mode according to Qualcomm QPIC controller standard formula:
        // Codewords per page = page_size / 512
        // Min OOB bytes for 8-bit ECC = (page_size / 512) * 20
        const std::size_t cws_per_page = chip_->page_size / 512;
        const std::size_t min_oob_for_bch8 = cws_per_page * 20;
        if (chip_->spare_size >= min_oob_for_bch8) {
            ecc_mode = qpic::EccMode::bch8;
        } else {
            ecc_mode = qpic::EccMode::bch4;
        }
        std::cout << "Auto-detected QPIC ECC: "
                  << (*ecc_mode == qpic::EccMode::bch8 ? "bch8" : "bch4")
                  << " (page=" << chip_->page_size
                  << ", oob=" << chip_->spare_size << ")\n";
    }

    const std::filesystem::path path = arguments[1];
    const std::uint64_t input_size = input_file_size(path);
    const std::uint64_t data_address = nand_offset.value_or(0);
    if (data_address % chip_->page_size != 0)
        throw Error("QPIC NAND offset must be data-page aligned");

    const qpic::PageEncoder encoder(chip_->page_size, chip_->spare_size,
                                    *ecc_mode);
    const std::uint64_t page_count =
        rounded_up(input_size, chip_->page_size) / chip_->page_size;
    const std::uint64_t start_page = data_address / chip_->page_size;
    if (start_page >= chip_->page_count() ||
        page_count > chip_->page_count() - start_page)
        throw Error("QPIC image does not fit in the selected chip range");

    const std::uint64_t raw_page_size = encoder.raw_page_size();
    const std::uint64_t raw_address =
        checked_product(start_page, raw_page_size, "QPIC raw write address");
    const std::uint64_t raw_length =
        checked_product(page_count, raw_page_size, "QPIC raw write length");

    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw Error("Failed to open input file: " + path.string());
    transport_->flush();
    std::uint64_t remaining = input_size;
    ProgressDisplay progress("write.qpic", raw_length);
    protocol::Flags flags;
    flags.skip_bad = true;
    flags.include_spare = true;
    flags.enable_hardware_ecc = false;

    if (firmware_version_.major == 3 && firmware_version_.minor <= 5) {
        std::cerr << "warning: firmware 3.5.x acknowledges write-end before "
                     "checking the final NAND busy/status; read.raw and compare "
                     "the result\n";
    }

    client_.write_pages(
        [&](std::uint8_t *raw_page, std::size_t size) {
            const std::size_t data_size = static_cast<std::size_t>(
                std::min<std::uint64_t>(remaining, chip_->page_size));
            std::vector<std::uint8_t> data(data_size);
            input.read(reinterpret_cast<char *>(data.data()),
                       static_cast<std::streamsize>(data.size()));
            if (input.bad() || input.gcount() !=
                                   static_cast<std::streamsize>(data.size()))
                throw Error("Failed to read input file during QPIC write");
            const auto encoded = encoder.encode(data.data(), data.size());
            if (encoded.size() != size)
                throw Error("Internal QPIC raw page size mismatch");
            std::copy(encoded.begin(), encoded.end(), raw_page);
            remaining -= data_size;
        },
        raw_address, raw_length, static_cast<std::uint32_t>(raw_page_size),
        flags,
        [&progress](std::uint64_t value) { progress.update(value); },
        [&progress](const BadBlockEvent &event) { progress.log_bad_block(event); });
    progress.finish();
    std::cout << "Wrote " << input_size << " data bytes as QPIC "
              << (*ecc_mode == qpic::EccMode::bch4 ? "BCH4" : "BCH8")
              << " (" << raw_length << " raw bytes) at NAND offset "
              << hex_number(data_address) << '\n';
}

void CommandShell::command_read_qpic(const std::vector<std::string> &arguments) {
    if (arguments.size() < 2)
        throw Error("Usage: read.qpic FILE <PARTITION|OFFSET> [LENGTH] [--ecc bch4|bch8]");

    std::optional<qpic::EccMode> ecc_mode;
    std::string file_arg;
    std::string target_arg;
    std::optional<std::uint64_t> explicit_length;

    for (std::size_t index = 1; index < arguments.size(); ++index) {
        if (arguments[index] == "--ecc") {
            if (ecc_mode || ++index >= arguments.size())
                throw Error("Usage: read.qpic FILE <PARTITION|OFFSET> [LENGTH] [--ecc bch4|bch8]");
            if (arguments[index] == "bch4")
                ecc_mode = qpic::EccMode::bch4;
            else if (arguments[index] == "bch8")
                ecc_mode = qpic::EccMode::bch8;
            else
                throw Error("Unknown ECC mode: " + arguments[index]);
            continue;
        }
        if (file_arg.empty()) {
            file_arg = arguments[index];
        } else if (target_arg.empty()) {
            target_arg = arguments[index];
        } else if (!explicit_length) {
            explicit_length = parse_number(arguments[index]);
        } else {
            throw Error("Too many arguments for read.qpic");
        }
    }

    if (file_arg.empty() || target_arg.empty())
        throw Error("Usage: read.qpic FILE <PARTITION|OFFSET> [LENGTH] [--ecc bch4|bch8]");

    ensure_probe();

    std::uint64_t data_address = 0;
    std::uint64_t length = 0;
    std::string target_desc;

    auto table = read_mibib_table(false);
    bool found_partition = false;
    if (table) {
        for (const auto &part : table->partitions) {
            if (part.name == target_arg) {
                data_address = part.start_offset;
                length = explicit_length ? *explicit_length : part.size_bytes;
                target_desc = "partition '" + part.name + "'";
                found_partition = true;
                break;
            }
        }
    }

    if (!found_partition) {
        data_address = parse_number(target_arg);
        if (!explicit_length)
            throw Error("Length is required when reading by raw offset");
        length = *explicit_length;
        target_desc = "offset " + hex_number(data_address);
    }

    if (data_address % chip_->page_size != 0)
        throw Error("Read offset must be page aligned");
    if (length == 0 || length % chip_->page_size != 0)
        throw Error("Read length must be a non-zero multiple of page size");
    if (data_address >= chip_->total_size || length > chip_->total_size - data_address)
        throw Error("Read range is outside the chip");

    if (!ecc_mode) {
        const std::size_t cws_per_page = chip_->page_size / 512;
        const std::size_t min_oob_for_bch8 = cws_per_page * 20;
        if (chip_->spare_size >= min_oob_for_bch8) {
            ecc_mode = qpic::EccMode::bch8;
        } else {
            ecc_mode = qpic::EccMode::bch4;
        }
        std::cout << "Auto-detected QPIC ECC: "
                  << (*ecc_mode == qpic::EccMode::bch8 ? "bch8" : "bch4")
                  << " (page=" << chip_->page_size
                  << ", oob=" << chip_->spare_size << ")\n";
    }

    const qpic::PageEncoder encoder(chip_->page_size, chip_->spare_size, *ecc_mode);
    const std::uint64_t raw_page_size = encoder.raw_page_size();
    const std::uint64_t page_count = length / chip_->page_size;
    const std::uint64_t start_page = data_address / chip_->page_size;
    const std::uint64_t raw_address = checked_product(start_page, raw_page_size, "QPIC raw read address");
    const std::uint64_t raw_length = checked_product(page_count, raw_page_size, "QPIC raw read length");

    const std::filesystem::path path = file_arg;
    std::ofstream output(path, std::ios::binary);
    if (!output)
        throw Error("Failed to open output file: " + path.string());

    std::uint64_t remaining = length;
    ProgressDisplay progress("read.qpic", length);
    protocol::Flags flags;
    flags.skip_bad = true;
    flags.include_spare = true;
    flags.enable_hardware_ecc = false;

    std::vector<std::uint8_t> raw_page_buf;
    client_.read(
        raw_address, raw_length, flags,
        [&](const std::uint8_t *data, std::size_t size) {
            raw_page_buf.insert(raw_page_buf.end(), data, data + size);
            while (raw_page_buf.size() >= raw_page_size && remaining > 0) {
                auto user_data = mibib::deinterleave_qpic(
                    raw_page_buf.data(), raw_page_size, chip_->page_size, chip_->spare_size);
                raw_page_buf.erase(raw_page_buf.begin(), raw_page_buf.begin() + raw_page_size);

                const std::size_t to_write = static_cast<std::size_t>(
                    std::min<std::uint64_t>(remaining, user_data.size()));
                if (to_write > 0) {
                    output.write(reinterpret_cast<const char *>(user_data.data()),
                                 static_cast<std::streamsize>(to_write));
                    remaining -= to_write;
                    progress.update(length - remaining);
                }
            }
        },
        nullptr,
        [&progress](const BadBlockEvent &event) { progress.log_bad_block(event); });
    progress.finish();

    std::cout << "Read " << length << " bytes from " << target_desc
              << " and saved to " << path.string()
              << " (deinterleaved QPIC " << (*ecc_mode == qpic::EccMode::bch4 ? "BCH4" : "BCH8") << ")\n";
}

void CommandShell::command_verify_qpic(const std::vector<std::string> &arguments) {
    if (arguments.size() < 2)
        throw Error("Usage: verify.qpic FILE [PARTITION|OFFSET] [--ecc bch4|bch8]");

    std::optional<qpic::EccMode> ecc_mode;
    std::string file_arg;
    std::string target_arg;

    for (std::size_t index = 1; index < arguments.size(); ++index) {
        if (arguments[index] == "--ecc") {
            if (ecc_mode || ++index >= arguments.size())
                throw Error("Usage: verify.qpic FILE [PARTITION|OFFSET] [--ecc bch4|bch8]");
            if (arguments[index] == "bch4")
                ecc_mode = qpic::EccMode::bch4;
            else if (arguments[index] == "bch8")
                ecc_mode = qpic::EccMode::bch8;
            else
                throw Error("Unknown ECC mode: " + arguments[index]);
            continue;
        }
        if (file_arg.empty()) {
            file_arg = arguments[index];
        } else if (target_arg.empty()) {
            target_arg = arguments[index];
        } else {
            throw Error("Too many arguments for verify.qpic");
        }
    }

    if (file_arg.empty())
        throw Error("Usage: verify.qpic FILE [PARTITION|OFFSET] [--ecc bch4|bch8]");

    ensure_probe();

    const std::filesystem::path path = file_arg;
    const std::uint64_t input_size = input_file_size(path);

    std::uint64_t data_address = 0;
    std::string target_desc;

    if (target_arg.empty()) {
        const std::string stem = path.stem().string();
        auto table = read_mibib_table(false);
        bool found_partition = false;
        if (table) {
            for (const auto &part : table->partitions) {
                if (part.name == stem || part.name == ("0:" + stem)) {
                    data_address = part.start_offset;
                    target_desc = "partition '" + part.name + "'";
                    found_partition = true;
                    break;
                }
            }
        }
        if (!found_partition) {
            data_address = 0;
            target_desc = "offset 0x0";
        }
    } else {
        auto table = read_mibib_table(false);
        bool found_partition = false;
        if (table) {
            for (const auto &part : table->partitions) {
                if (part.name == target_arg) {
                    data_address = part.start_offset;
                    target_desc = "partition '" + part.name + "'";
                    found_partition = true;
                    break;
                }
            }
        }
        if (!found_partition) {
            data_address = parse_number(target_arg);
            target_desc = "offset " + hex_number(data_address);
        }
    }

    if (data_address % chip_->page_size != 0)
        throw Error("Verify offset must be page aligned");
    if (data_address >= chip_->total_size || input_size > chip_->total_size - data_address)
        throw Error("Verify range is outside the chip");

    if (!ecc_mode) {
        const std::size_t cws_per_page = chip_->page_size / 512;
        const std::size_t min_oob_for_bch8 = cws_per_page * 20;
        if (chip_->spare_size >= min_oob_for_bch8) {
            ecc_mode = qpic::EccMode::bch8;
        } else {
            ecc_mode = qpic::EccMode::bch4;
        }
        std::cout << "Auto-detected QPIC ECC: "
                  << (*ecc_mode == qpic::EccMode::bch8 ? "bch8" : "bch4")
                  << " (page=" << chip_->page_size
                  << ", oob=" << chip_->spare_size << ")\n";
    }

    const qpic::PageEncoder encoder(chip_->page_size, chip_->spare_size, *ecc_mode);
    const std::uint64_t page_count = rounded_up(input_size, chip_->page_size) / chip_->page_size;
    const std::uint64_t start_page = data_address / chip_->page_size;
    const std::uint64_t raw_page_size = encoder.raw_page_size();
    const std::uint64_t raw_address = checked_product(start_page, raw_page_size, "QPIC raw verify address");
    const std::uint64_t raw_length = checked_product(page_count, raw_page_size, "QPIC raw verify length");

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
    ProgressDisplay progress("verify.qpic", input_size);
    protocol::Flags flags;
    flags.skip_bad = true;
    flags.include_spare = true;
    flags.enable_hardware_ecc = false;

    std::vector<std::uint8_t> raw_page_buf;
    client_.read(
        raw_address, raw_length, flags,
        [&](const std::uint8_t *data, std::size_t size) {
            raw_page_buf.insert(raw_page_buf.end(), data, data + size);
            while (raw_page_buf.size() >= raw_page_size && compared < input_size) {
                auto user_data = mibib::deinterleave_qpic(
                    raw_page_buf.data(), raw_page_size, chip_->page_size, chip_->spare_size);
                raw_page_buf.erase(raw_page_buf.begin(), raw_page_buf.begin() + raw_page_size);

                const std::size_t to_compare = static_cast<std::size_t>(
                    std::min<std::uint64_t>(input_size - compared, user_data.size()));
                if (to_compare > 0) {
                    std::vector<std::uint8_t> wanted(to_compare);
                    expected.read(reinterpret_cast<char *>(wanted.data()),
                                  static_cast<std::streamsize>(to_compare));
                    if (expected.bad() || expected.gcount() != static_cast<std::streamsize>(to_compare))
                        throw Error("Failed while reading verify file");

                    if (!mismatch) {
                        for (std::size_t i = 0; i < to_compare; ++i) {
                            if (wanted[i] != user_data[i]) {
                                mismatch = Mismatch{compared + i, wanted[i], user_data[i]};
                                break;
                            }
                        }
                    }
                    compared += to_compare;
                    progress.update(compared);
                }
            }
        },
        nullptr,
        [&progress](const BadBlockEvent &event) { progress.log_bad_block(event); });
    progress.finish();

    if (mismatch) {
        std::ostringstream message;
        message << "mismatch at image offset " << hex_number(mismatch->offset)
                << ": expected " << hex_number(mismatch->expected, 2)
                << ", got " << hex_number(mismatch->actual, 2);
        throw VerifyMismatch(mismatch->offset, mismatch->expected,
                             mismatch->actual, message.str());
    }
    std::cout << "Verified " << input_size << " bytes successfully against "
              << target_desc << " (deinterleaved QPIC "
              << (*ecc_mode == qpic::EccMode::bch4 ? "BCH4" : "BCH8") << ")\n";
}

std::optional<mibib::PartitionTable> CommandShell::read_mibib_table(bool force_refresh) {
    if (!force_refresh && cached_mibib_)
        return cached_mibib_;

    ensure_probe();
    constexpr std::uint64_t max_scan_limit = 4 * 1024 * 1024; // Scan up to 4MB
    constexpr std::uint64_t scan_step = 64 * 1024;            // 64KB aligned steps
    const std::uint64_t total_scan = std::min(chip_->total_size, max_scan_limit);

    const std::uint64_t raw_page_size = chip_->raw_page_size();
    const std::uint64_t raw_read_size = raw_page_size * 2; // Read 2 raw pages per block

    for (std::uint64_t offset = 0; offset < total_scan; offset += scan_step) {
        log_debug("Scanning for MIBIB at offset=" + hex_number(offset));

        // 1. Try QPIC raw pages read with de-interleaving
        const std::uint64_t start_page = offset / chip_->page_size;
        const std::uint64_t raw_address = start_page * raw_page_size;

        std::vector<std::uint8_t> raw_buffer;
        raw_buffer.reserve(static_cast<std::size_t>(raw_read_size));
        protocol::Flags raw_flags;
        raw_flags.skip_bad = true;
        raw_flags.include_spare = true;
        raw_flags.enable_hardware_ecc = false;

        try {
            client_.read(
                raw_address, raw_read_size, raw_flags,
                [&raw_buffer](const std::uint8_t *data, std::size_t size) {
                    raw_buffer.insert(raw_buffer.end(), data, data + size);
                },
                nullptr, nullptr);
        } catch (const std::exception &) {
            // Bad block or read error, continue
        }

        if (!raw_buffer.empty()) {
            auto deinterleaved = mibib::deinterleave_qpic(
                raw_buffer.data(), raw_buffer.size(), chip_->page_size, chip_->spare_size);
            if (!deinterleaved.empty()) {
                auto table = mibib::parse_mibib(
                    deinterleaved.data(), deinterleaved.size(), chip_->block_size, chip_->total_size, offset);
                if (table && !table->partitions.empty()) {
                    log_debug("Found QPIC deinterleaved MIBIB table at offset " + hex_number(offset));
                    cached_mibib_ = std::move(table);
                    return cached_mibib_;
                }
            }

            auto table_raw = mibib::parse_mibib(
                raw_buffer.data(), raw_buffer.size(), chip_->block_size, chip_->total_size, offset);
            if (table_raw && !table_raw->partitions.empty()) {
                log_debug("Found raw MIBIB table at offset " + hex_number(offset));
                cached_mibib_ = std::move(table_raw);
                return cached_mibib_;
            }
        }

        // 2. Try linear data read
        std::vector<std::uint8_t> linear_buffer;
        linear_buffer.reserve(static_cast<std::size_t>(chip_->page_size * 2));
        protocol::Flags linear_flags;
        linear_flags.skip_bad = true;
        linear_flags.include_spare = false;
        linear_flags.enable_hardware_ecc = false;

        try {
            client_.read(
                offset, chip_->page_size * 2, linear_flags,
                [&linear_buffer](const std::uint8_t *data, std::size_t size) {
                    linear_buffer.insert(linear_buffer.end(), data, data + size);
                },
                nullptr, nullptr);
        } catch (const std::exception &) {
            // Continue
        }

        if (!linear_buffer.empty()) {
            auto table = mibib::parse_mibib(
                linear_buffer.data(), linear_buffer.size(), chip_->block_size, chip_->total_size, offset);
            if (table && !table->partitions.empty()) {
                log_debug("Found linear MIBIB table at offset " + hex_number(offset));
                cached_mibib_ = std::move(table);
                return cached_mibib_;
            }
        }
    }

    return std::nullopt;
}

void CommandShell::command_part(const std::vector<std::string> &arguments) {
    bool refresh = false;
    for (std::size_t i = 1; i < arguments.size(); ++i) {
        if (arguments[i] == "--refresh" || arguments[i] == "-r")
            refresh = true;
    }

    ensure_probe();
    auto table = read_mibib_table(refresh);
    if (!table) {
        std::cout << "No SMEM/MIBIB partition table found in the first 4MB of NAND.\n";
        return;
    }

    std::cout << "smem ptable found: ver: " << table->table_version
              << " len: " << table->partitions.size()
              << " (at offset " << hex_number(table->mibib_offset) << ")\n";
    std::cout << std::string(75, '-') << '\n';
    std::cout << std::right << std::setw(4) << "#" << "  "
              << std::left << std::setw(18) << "Name"
              << std::setw(14) << "Start Block"
              << std::setw(12) << "Blocks"
              << std::setw(16) << "Offset"
              << "Size\n";
    std::cout << std::string(75, '-') << '\n';

    for (std::size_t i = 0; i < table->partitions.size(); ++i) {
        const auto &part = table->partitions[i];
        std::ostringstream size_str;
        if (part.size_blocks == 0xFFFFFFFF) {
            size_str << "remaining (" << (part.size_bytes / (1024 * 1024)) << " MiB)";
        } else if (part.size_bytes >= 1024 * 1024 && part.size_bytes % (1024 * 1024) == 0) {
            size_str << (part.size_bytes / (1024 * 1024)) << " MiB";
        } else if (part.size_bytes >= 1024 * 1024) {
            double mib = static_cast<double>(part.size_bytes) / (1024.0 * 1024.0);
            size_str << std::fixed << std::setprecision(1) << mib << " MiB";
        } else if (part.size_bytes >= 1024) {
            size_str << (part.size_bytes / 1024) << " KiB";
        } else {
            size_str << hex_number(part.size_bytes) << " B";
        }

        std::cout << std::right << std::setw(4) << i << "  "
                  << std::left << std::setw(18) << part.name
                  << std::setw(14) << part.start_block
                  << std::setw(12) << (part.size_blocks == 0xFFFFFFFF ? "all" : std::to_string(part.size_blocks))
                  << std::setw(16) << hex_number(part.start_offset)
                  << size_str.str() << '\n';
    }
    std::cout << std::string(75, '-') << '\n';
}

void CommandShell::command_flash(const std::vector<std::string> &arguments) {
    if (arguments.size() < 3)
        throw Error("Usage: flash FILE <PARTITION|OFFSET> [--ecc bch4|bch8]");

    std::optional<qpic::EccMode> ecc_mode;
    std::string file_arg;
    std::string dest_arg;

    for (std::size_t index = 1; index < arguments.size(); ++index) {
        if (arguments[index] == "--ecc") {
            if (ecc_mode || ++index >= arguments.size())
                throw Error("Usage: flash FILE <PARTITION|OFFSET> [--ecc bch4|bch8]");
            if (arguments[index] == "bch4")
                ecc_mode = qpic::EccMode::bch4;
            else if (arguments[index] == "bch8")
                ecc_mode = qpic::EccMode::bch8;
            else
                throw Error("QPIC ECC must be bch4 or bch8");
        } else if (arguments[index].rfind("--", 0) == 0) {
            throw Error("Unknown flash option: " + arguments[index]);
        } else if (file_arg.empty()) {
            file_arg = arguments[index];
        } else if (dest_arg.empty()) {
            dest_arg = arguments[index];
        } else {
            throw Error("Too many arguments for flash command");
        }
    }

    if (file_arg.empty() || dest_arg.empty())
        throw Error("Usage: flash FILE <PARTITION|OFFSET> [--ecc bch4|bch8]");

    ensure_probe();
    const std::filesystem::path path = file_arg;
    const std::uint64_t input_size = input_file_size(path);

    std::uint64_t target_address = 0;
    std::uint64_t erase_length = 0;
    std::string dest_desc;

    // Check if dest_arg matches a partition in MIBIB table
    const auto mibib = read_mibib_table();
    const mibib::PartitionEntry *matched_part = nullptr;
    if (mibib) {
        matched_part = mibib->find(dest_arg);
    }

    if (matched_part != nullptr) {
        target_address = matched_part->start_offset;
        erase_length = (matched_part->size_blocks == 0xFFFFFFFF || matched_part->size_blocks == 0)
                           ? rounded_up(input_size, chip_->block_size)
                           : matched_part->size_bytes;
        dest_desc = "partition '" + matched_part->name + "'";
        if (input_size > erase_length) {
            throw Error("Input file size (" + std::to_string(input_size) +
                        " bytes) exceeds partition '" + matched_part->name +
                        "' capacity (" + std::to_string(erase_length) + " bytes)");
        }
    } else {
        target_address = parse_number(dest_arg);
        if (target_address % chip_->page_size != 0)
            throw Error("Flash NAND offset must be data-page aligned");
        erase_length = rounded_up(input_size, chip_->block_size);
        dest_desc = "offset " + hex_number(target_address);
    }

    if (target_address >= chip_->total_size || erase_length > chip_->total_size - target_address)
        throw Error("Flash range is outside the chip");

    // Auto-detect QPIC ECC mode if not specified
    if (!ecc_mode) {
        const std::size_t cws_per_page = chip_->page_size / 512;
        const std::size_t min_oob_for_bch8 = cws_per_page * 20;
        if (chip_->spare_size >= min_oob_for_bch8) {
            ecc_mode = qpic::EccMode::bch8;
        } else {
            ecc_mode = qpic::EccMode::bch4;
        }
        std::cout << "Auto-detected QPIC ECC: "
                  << (*ecc_mode == qpic::EccMode::bch8 ? "bch8" : "bch4")
                  << " (page=" << chip_->page_size
                  << ", oob=" << chip_->spare_size << ")\n";
    }

    std::cout << "Flashing " << path.filename().string() << " (" << input_size << " bytes) to "
              << dest_desc << " [offset " << hex_number(target_address)
              << ", erase length " << hex_number(erase_length) << "] with QPIC "
              << (*ecc_mode == qpic::EccMode::bch4 ? "BCH4" : "BCH8") << "...\n";

    // Step 1: Erase
    ProgressDisplay erase_progress("erase", erase_length);
    protocol::Flags erase_flags;
    erase_flags.skip_bad = true;
    client_.erase(
        target_address, erase_length, erase_flags,
        [&erase_progress](std::uint64_t value) { erase_progress.update(value); },
        [&erase_progress](const BadBlockEvent &event) { erase_progress.log_bad_block(event); });
    erase_progress.finish();

    // Step 2: Write via QPIC
    const qpic::PageEncoder encoder(chip_->page_size, chip_->spare_size, *ecc_mode);
    const std::uint64_t page_count = rounded_up(input_size, chip_->page_size) / chip_->page_size;
    const std::uint64_t start_page = target_address / chip_->page_size;
    const std::uint64_t raw_page_size = encoder.raw_page_size();
    const std::uint64_t raw_address = checked_product(start_page, raw_page_size, "QPIC raw write address");
    const std::uint64_t raw_length = checked_product(page_count, raw_page_size, "QPIC raw write length");

    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw Error("Failed to open input file: " + path.string());

    std::uint64_t remaining = input_size;
    ProgressDisplay flash_progress("flash", raw_length);
    protocol::Flags write_flags;
    write_flags.skip_bad = true;
    write_flags.include_spare = true;
    write_flags.enable_hardware_ecc = false;

    if (firmware_version_.major == 3 && firmware_version_.minor <= 5) {
        std::cerr << "warning: firmware 3.5.x acknowledges write-end before "
                     "checking the final NAND busy/status; read.raw and compare "
                     "the result\n";
    }

    client_.write_pages(
        [&](std::uint8_t *raw_page, std::size_t size) {
            const std::size_t data_size = static_cast<std::size_t>(
                std::min<std::uint64_t>(remaining, chip_->page_size));
            std::vector<std::uint8_t> data(data_size);
            input.read(reinterpret_cast<char *>(data.data()),
                       static_cast<std::streamsize>(data.size()));
            if (input.bad() || input.gcount() !=
                                   static_cast<std::streamsize>(data.size()))
                throw Error("Failed to read input file during flash write");
            const auto encoded = encoder.encode(data.data(), data.size());
            if (encoded.size() != size)
                throw Error("Internal QPIC raw page size mismatch");
            std::copy(encoded.begin(), encoded.end(), raw_page);
            remaining -= data_size;
        },
        raw_address, raw_length, static_cast<std::uint32_t>(raw_page_size),
        write_flags,
        [&flash_progress](std::uint64_t value) { flash_progress.update(value); },
        [&flash_progress](const BadBlockEvent &event) { flash_progress.log_bad_block(event); });
    flash_progress.finish();

    std::cout << "Flashed " << input_size << " bytes successfully to " << dest_desc << '\n';
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
        [&progress](const BadBlockEvent &event) { progress.log_bad_block(event); });
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
        << "  smem [--refresh]                          show Qualcomm SMEM/MIBIB partition table\n"
        << "  flash FILE <PARTITION|OFFSET> [--ecc MODE]erase & flash image in QPIC layout\n"
        << "  read FILE [OFFSET] [LENGTH]               read data area\n"
        << "  read.raw FILE [START-PAGE] [PAGE-COUNT]   read data+OOB verbatim\n"
        << "  erase all                                 erase the complete NAND immediately\n"
        << "  erase OFFSET LENGTH                       erase a block-aligned range immediately\n"
        << "  scrub [all | OFFSET LENGTH]               force unconditioned physical erase (wipes OOB markers)\n"
        << "  test [all | PART|OFF LEN] [--mode b|c]    hardware self-test (mode: block=per-block, chip=RDT full-span)\n"
        << "  test.write [all | PART|OFF LEN] [--seed S]write PRBS32 baseline for delayed retention test\n"
        << "  test.verify [all | PART|OFF LEN] [--seed] verify PRBS32 retention and mark leaking bad blocks\n"
        << "  write FILE [OFFSET]                       write data, pad tail with FF\n"
        << "  write.raw FILE [START-PAGE]               write data+OOB verbatim\n"
        << "  verify FILE [OFFSET] [--raw]              stream-compare NAND to file\n"
        << "  write.qpic FILE [NAND-OFFSET] [--ecc MODE]write QPIC layout (auto-detects BCH4/8)\n"
        << "  read.qpic FILE <PARTITION|OFFSET> [LEN]   read & de-interleave QPIC data\n"
        << "  verify.qpic FILE [PARTITION|OFFSET]       verify file against de-interleaved QPIC\n"
        << "  debug [on|off]                            toggle verbose debug logging\n"
        << "  help                                      show this help\n"
        << "  exit                                      leave the REPL\n"
        << "Numbers accept decimal, 0x hexadecimal, and K/M/G suffixes.\n"
        << "Write commands never erase NAND automatically (use 'flash' to auto-erase).\n";
}

} // namespace nandprog
