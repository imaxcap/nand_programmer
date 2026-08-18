#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace nandprog {

std::uint64_t parse_number(const std::string &text);
std::vector<std::string> split_command_line(const std::string &line);
std::string hex_number(std::uint64_t value, unsigned width = 0);
std::filesystem::path find_database(const std::filesystem::path &requested,
                                    const std::filesystem::path &executable);
void set_debug_enabled(bool enabled);
bool is_debug_enabled();
void log_debug(const std::string &message);
std::string hex_dump(const std::uint8_t *data, std::size_t size, std::size_t max_bytes = 32);

} // namespace nandprog
