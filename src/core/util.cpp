#include "nandprog/util.hpp"

#include "nandprog/error.hpp"

#include <cctype>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace nandprog {

std::uint64_t parse_number(const std::string &text) {
    if (text.empty())
        throw Error("Expected a number");

    std::string number = text;
    std::uint64_t multiplier = 1;
    const char suffix = static_cast<char>(std::tolower(number.back()));
    if (suffix == 'k' || suffix == 'm' || suffix == 'g') {
        number.pop_back();
        multiplier = suffix == 'k' ? 1024ULL
                    : suffix == 'm' ? 1024ULL * 1024ULL
                                    : 1024ULL * 1024ULL * 1024ULL;
    }

    std::size_t consumed = 0;
    std::uint64_t value = 0;
    try {
        value = std::stoull(number, &consumed, 0);
    } catch (const std::exception &) {
        throw Error("Invalid number: " + text);
    }
    if (consumed != number.size())
        throw Error("Invalid number: " + text);
    if (value > UINT64_MAX / multiplier)
        throw Error("Number is too large: " + text);
    return value * multiplier;
}

std::vector<std::string> split_command_line(const std::string &line) {
    std::vector<std::string> result;
    std::string current;
    char quote = 0;
    bool escaped = false;

    for (char character : line) {
        if (escaped) {
            current.push_back(character);
            escaped = false;
        } else if (character == '\\' && quote != '\'') {
            escaped = true;
        } else if (quote != 0) {
            if (character == quote)
                quote = 0;
            else
                current.push_back(character);
        } else if (character == '\'' || character == '"') {
            quote = character;
        } else if (std::isspace(static_cast<unsigned char>(character))) {
            if (!current.empty()) {
                result.push_back(current);
                current.clear();
            }
        } else {
            current.push_back(character);
        }
    }

    if (escaped || quote != 0)
        throw Error("Unterminated quote or escape in command line");
    if (!current.empty())
        result.push_back(current);
    return result;
}

std::string hex_number(std::uint64_t value, unsigned width) {
    std::ostringstream stream;
    stream << "0x" << std::hex << std::setfill('0');
    if (width != 0)
        stream << std::setw(static_cast<int>(width));
    stream << value;
    return stream.str();
}

std::filesystem::path find_database(const std::filesystem::path &requested,
                                    const std::filesystem::path &executable) {
    if (!requested.empty()) {
        if (std::filesystem::is_regular_file(requested))
            return requested;
        throw Error("Chip database not found: " + requested.string());
    }

    const auto executable_dir = std::filesystem::absolute(executable).parent_path();
    const std::vector<std::filesystem::path> candidates = {
        executable_dir / "nando_parallel_chip_db.csv",
        executable_dir / "data" / "nando_parallel_chip_db.csv",
        executable_dir.parent_path() / "share" / "nandprog" /
            "nando_parallel_chip_db.csv",
        std::filesystem::current_path() / "data" /
            "nando_parallel_chip_db.csv",
    };
    for (const auto &candidate : candidates) {
        if (std::filesystem::is_regular_file(candidate))
            return candidate;
    }
    throw Error("Could not find nando_parallel_chip_db.csv; use --db PATH");
}

namespace {
bool g_debug_enabled = false;
}

void set_debug_enabled(bool enabled) {
    g_debug_enabled = enabled;
}

bool is_debug_enabled() {
    if (g_debug_enabled)
        return true;
    const char *env = std::getenv("NANDPROG_DEBUG");
    return env != nullptr && std::string(env) != "0";
}

void log_debug(const std::string &message) {
    if (is_debug_enabled()) {
        std::cerr << "[DEBUG] " << message << std::endl;
    }
}

std::string hex_dump(const std::uint8_t *data, std::size_t size, std::size_t max_bytes) {
    std::ostringstream stream;
    const std::size_t print_size = std::min(size, max_bytes);
    for (std::size_t i = 0; i < print_size; ++i) {
        if (i > 0) stream << " ";
        stream << hex_number(data[i], 2);
    }
    if (size > max_bytes)
        stream << " ... (" << size << " bytes total)";
    return stream.str();
}

} // namespace nandprog
