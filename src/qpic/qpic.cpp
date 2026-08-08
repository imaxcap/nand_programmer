#include "nandprog/qpic.hpp"

#include "nandprog/error.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace nandprog::qpic {
namespace {

constexpr unsigned field_degree = 13;
constexpr unsigned field_size = 1U << field_degree;
constexpr unsigned field_order = field_size - 1;
constexpr unsigned primitive_polynomial = 0x201b;

class GaloisField {
public:
    GaloisField() : powers_(field_order * 2), logarithms_(field_size) {
        unsigned value = 1;
        for (unsigned index = 0; index < field_order; ++index) {
            powers_[index] = static_cast<std::uint16_t>(value);
            logarithms_[value] = static_cast<std::uint16_t>(index);
            value <<= 1;
            if ((value & field_size) != 0)
                value ^= primitive_polynomial;
        }
        if (value != 1)
            throw Error("QPIC BCH primitive polynomial is invalid");
        for (unsigned index = field_order; index < powers_.size(); ++index)
            powers_[index] = powers_[index - field_order];
    }

    std::uint16_t power(unsigned exponent) const {
        return powers_[exponent % field_order];
    }

    std::uint16_t multiply(std::uint16_t left, std::uint16_t right) const {
        if (left == 0 || right == 0)
            return 0;
        return powers_[logarithms_[left] + logarithms_[right]];
    }

private:
    std::vector<std::uint16_t> powers_;
    std::vector<std::uint16_t> logarithms_;
};

std::vector<std::uint8_t> generator_polynomial(unsigned strength) {
    GaloisField field;
    std::vector<bool> roots(field_order, false);
    for (unsigned exponent = 1; exponent <= strength * 2; ++exponent) {
        unsigned conjugate = exponent % field_order;
        do {
            roots[conjugate] = true;
            conjugate = (conjugate * 2) % field_order;
        } while (conjugate != exponent);
    }

    std::vector<std::uint16_t> polynomial{1};
    for (unsigned exponent = 0; exponent < roots.size(); ++exponent) {
        if (!roots[exponent])
            continue;
        const std::uint16_t root = field.power(exponent);
        std::vector<std::uint16_t> next(polynomial.size() + 1, 0);
        for (std::size_t index = 0; index < polynomial.size(); ++index) {
            next[index] ^= field.multiply(polynomial[index], root);
            next[index + 1] ^= polynomial[index];
        }
        polynomial = std::move(next);
    }

    std::vector<std::uint8_t> binary(polynomial.size());
    for (std::size_t index = 0; index < polynomial.size(); ++index) {
        if (polynomial[index] > 1)
            throw Error("QPIC BCH generator polynomial is not binary");
        binary[index] = static_cast<std::uint8_t>(polynomial[index]);
    }
    return binary;
}

} // namespace

BchEncoder::BchEncoder(EccMode mode) {
    const unsigned strength = static_cast<unsigned>(mode);
    if (strength != 4 && strength != 8)
        throw Error("QPIC supports only BCH4 and BCH8");

    const auto polynomial = generator_polynomial(strength);
    degree_ = static_cast<unsigned>(polynomial.size() - 1);
    if (degree_ == 0 || degree_ > generator_.words.size() * 64)
        throw Error("QPIC BCH generator degree is unsupported");
    parity_size_ = (degree_ + 7) / 8;

    for (unsigned bit = 0; bit < degree_; ++bit) {
        if (polynomial[bit] != 0)
            set_remainder_bit(generator_, bit);
    }

    for (unsigned value = 0; value < byte_table_.size(); ++value) {
        Remainder remainder;
        for (unsigned bit = 0; bit < 8; ++bit)
            update_bit(remainder, (value & (0x80U >> bit)) != 0);
        byte_table_[value] = remainder;
    }
}

void BchEncoder::xor_remainder(Remainder &destination,
                               const Remainder &source) noexcept {
    destination.words[0] ^= source.words[0];
    destination.words[1] ^= source.words[1];
}

bool BchEncoder::remainder_bit(const Remainder &value, unsigned bit) noexcept {
    return (value.words[bit / 64] & (std::uint64_t{1} << (bit % 64))) != 0;
}

void BchEncoder::set_remainder_bit(Remainder &value, unsigned bit) noexcept {
    value.words[bit / 64] |= std::uint64_t{1} << (bit % 64);
}

bool BchEncoder::top_bit(const Remainder &value) const noexcept {
    return remainder_bit(value, degree_ - 1);
}

std::uint8_t BchEncoder::top_byte(const Remainder &value) const noexcept {
    std::uint8_t result = 0;
    for (unsigned bit = 0; bit < 8; ++bit) {
        if (remainder_bit(value, degree_ - 1 - bit))
            result |= static_cast<std::uint8_t>(0x80U >> bit);
    }
    return result;
}

void BchEncoder::trim(Remainder &value) const noexcept {
    if (degree_ < 64) {
        value.words[0] &= (std::uint64_t{1} << degree_) - 1;
        value.words[1] = 0;
    } else if (degree_ < 128) {
        value.words[1] &= (std::uint64_t{1} << (degree_ - 64)) - 1;
    }
}

void BchEncoder::shift_left(Remainder &value) const noexcept {
    value.words[1] = (value.words[1] << 1) | (value.words[0] >> 63);
    value.words[0] <<= 1;
    trim(value);
}

void BchEncoder::shift_left_byte(Remainder &value) const noexcept {
    value.words[1] = (value.words[1] << 8) | (value.words[0] >> 56);
    value.words[0] <<= 8;
    trim(value);
}

void BchEncoder::update_bit(Remainder &value, bool bit) const noexcept {
    const bool feedback = bit != top_bit(value);
    shift_left(value);
    if (feedback)
        xor_remainder(value, generator_);
}

std::vector<std::uint8_t> BchEncoder::encode(const std::uint8_t *data,
                                             std::size_t size) const {
    if (data == nullptr && size != 0)
        throw Error("QPIC BCH input is null");

    Remainder remainder;
    for (std::size_t index = 0; index < size; ++index) {
        const std::uint8_t table_index = top_byte(remainder) ^ data[index];
        shift_left_byte(remainder);
        xor_remainder(remainder, byte_table_[table_index]);
    }

    std::vector<std::uint8_t> parity(parity_size_, 0);
    for (unsigned bit = 0; bit < degree_; ++bit) {
        if (remainder_bit(remainder, degree_ - 1 - bit))
            parity[bit / 8] |= static_cast<std::uint8_t>(0x80U >> (bit % 8));
    }
    return parity;
}

PageEncoder::PageEncoder(std::uint32_t page_size, std::uint32_t oob_size,
                         EccMode mode)
    : page_size_(page_size), oob_size_(oob_size), mode_(mode), bch_(mode),
      codeword_count_(page_size / ecc_step_size),
      codeword_size_(mode == EccMode::bch4 ? 528U : 532U),
      bbm_position_(page_size % codeword_size_),
      codeword_padding_(codeword_size_ - codeword_data_size - bbm_size -
                        static_cast<std::uint32_t>(bch_.parity_size())) {
    if (page_size == 0 || page_size % ecc_step_size != 0)
        throw Error("QPIC page size must be a non-zero multiple of 512 bytes");
    if (bbm_position_ == 0 || bbm_position_ > codeword_data_size)
        throw Error("QPIC page size has an unsupported bad-block marker position");
    if (raw_page_size() > std::numeric_limits<std::size_t>::max())
        throw Error("QPIC raw page size is too large");
    const std::uint64_t required =
        static_cast<std::uint64_t>(codeword_count_) * codeword_size_;
    if (required > raw_page_size())
        throw Error("QPIC ECC layout needs more bytes than the NAND OOB provides");
}

std::vector<std::uint8_t> PageEncoder::encode(const std::uint8_t *data,
                                              std::size_t size,
                                              std::uint8_t page_padding) const {
    if (size > page_size_)
        throw Error("QPIC input exceeds one NAND data page");
    if (data == nullptr && size != 0)
        throw Error("QPIC page input is null");

    std::vector<std::uint8_t> page(page_size_, page_padding);
    if (size != 0)
        std::copy_n(data, size, page.begin());
    std::vector<std::uint8_t> result(static_cast<std::size_t>(raw_page_size()),
                                     0xff);

    std::array<std::uint8_t, codeword_data_size> codeword{};
    for (std::uint32_t index = 0; index < codeword_count_; ++index) {
        codeword.fill(0xff);
        const std::uint64_t input_offset =
            static_cast<std::uint64_t>(index) * codeword_data_size;
        if (input_offset < page.size()) {
            const std::size_t available = static_cast<std::size_t>(
                std::min<std::uint64_t>(codeword_data_size,
                                        page.size() - input_offset));
            std::copy_n(page.begin() + static_cast<std::ptrdiff_t>(input_offset),
                        available, codeword.begin());
        }

        const std::size_t output_offset =
            static_cast<std::size_t>(index) * codeword_size_;
        auto output = result.begin() + static_cast<std::ptrdiff_t>(output_offset);
        output = std::copy_n(codeword.begin(), bbm_position_, output);
        *output++ = 0xff;
        output = std::copy(codeword.begin() + bbm_position_, codeword.end(), output);
        const auto parity = bch_.encode(codeword.data(), codeword.size());
        output = std::copy(parity.begin(), parity.end(), output);
        std::fill_n(output, codeword_padding_, 0xff);
    }
    return result;
}

} // namespace nandprog::qpic
