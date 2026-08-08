#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

namespace nandprog {

class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class FirmwareError : public Error {
public:
    FirmwareError(std::uint8_t code, const std::string &message)
        : Error(message), code_(code) {}

    std::uint8_t code() const noexcept { return code_; }

private:
    std::uint8_t code_;
};

class VerifyMismatch : public Error {
public:
    VerifyMismatch(std::uint64_t offset, std::uint8_t expected,
                   std::uint8_t actual, const std::string &message)
        : Error(message), offset_(offset), expected_(expected), actual_(actual) {}

    std::uint64_t offset() const noexcept { return offset_; }
    std::uint8_t expected() const noexcept { return expected_; }
    std::uint8_t actual() const noexcept { return actual_; }

private:
    std::uint64_t offset_;
    std::uint8_t expected_;
    std::uint8_t actual_;
};

} // namespace nandprog
