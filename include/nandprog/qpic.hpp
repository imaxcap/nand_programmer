#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace nandprog::qpic {

enum class EccMode : unsigned {
    bch4 = 4,
    bch8 = 8,
};

class BchEncoder {
public:
    explicit BchEncoder(EccMode mode);

    std::vector<std::uint8_t> encode(const std::uint8_t *data,
                                     std::size_t size) const;
    std::size_t parity_size() const noexcept { return parity_size_; }

private:
    struct Remainder {
        std::array<std::uint64_t, 2> words{};
    };

    unsigned degree_ = 0;
    std::size_t parity_size_ = 0;
    Remainder generator_{};
    std::array<Remainder, 256> byte_table_{};

    static void xor_remainder(Remainder &destination,
                              const Remainder &source) noexcept;
    static bool remainder_bit(const Remainder &value, unsigned bit) noexcept;
    static void set_remainder_bit(Remainder &value, unsigned bit) noexcept;
    bool top_bit(const Remainder &value) const noexcept;
    std::uint8_t top_byte(const Remainder &value) const noexcept;
    void trim(Remainder &value) const noexcept;
    void shift_left(Remainder &value) const noexcept;
    void shift_left_byte(Remainder &value) const noexcept;
    void update_bit(Remainder &value, bool bit) const noexcept;
};

class PageEncoder {
public:
    PageEncoder(std::uint32_t page_size, std::uint32_t oob_size, EccMode mode);

    std::vector<std::uint8_t> encode(const std::uint8_t *data,
                                     std::size_t size,
                                     std::uint8_t page_padding = 0x00) const;

    std::uint32_t page_size() const noexcept { return page_size_; }
    std::uint32_t oob_size() const noexcept { return oob_size_; }
    std::uint64_t raw_page_size() const noexcept {
        return static_cast<std::uint64_t>(page_size_) + oob_size_;
    }
    std::uint32_t codeword_count() const noexcept { return codeword_count_; }
    std::uint32_t codeword_size() const noexcept { return codeword_size_; }
    std::uint32_t bbm_position() const noexcept { return bbm_position_; }
    EccMode mode() const noexcept { return mode_; }

private:
    static constexpr std::uint32_t ecc_step_size = 512;
    static constexpr std::uint32_t codeword_data_size = 516;
    static constexpr std::uint32_t bbm_size = 1;

    std::uint32_t page_size_;
    std::uint32_t oob_size_;
    EccMode mode_;
    BchEncoder bch_;
    std::uint32_t codeword_count_;
    std::uint32_t codeword_size_;
    std::uint32_t bbm_position_;
    std::uint32_t codeword_padding_;
};

} // namespace nandprog::qpic
