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

} // namespace nandprog
