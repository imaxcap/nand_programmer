#include "nandprog/chip_db.hpp"

#include "nandprog/error.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>

namespace nandprog {
namespace {

constexpr std::uint64_t undefined_parameter = 0xffffffffULL;

enum Parameter : std::size_t {
    t_cs,
    t_cls,
    t_als,
    t_clr,
    t_ar,
    t_wp,
    t_rp,
    t_ds,
    t_ch,
    t_clh,
    t_alh,
    t_wc,
    t_rc,
    t_rea,
    row_cycles,
    column_cycles,
    read_first_command,
    read_second_command,
    read_spare_command,
    read_id_command,
    reset_command,
    write_first_command,
    write_second_command,
    erase_first_command,
    erase_second_command,
    status_command,
    set_features_command,
    enable_ecc_address,
    enable_ecc_value,
    disable_ecc_value,
    id_1,
    id_2,
    id_3,
    id_4,
    id_5,
};

std::string trim(const std::string &value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::vector<std::string> split_csv(const std::string &line) {
    std::vector<std::string> fields;
    std::stringstream stream(line);
    std::string field;
    while (std::getline(stream, field, ','))
        fields.push_back(trim(field));
    return fields;
}

std::uint64_t parse_parameter(const std::string &value, bool optional) {
    if (optional && value == "-")
        return undefined_parameter;
    std::size_t consumed = 0;
    std::uint64_t result = 0;
    try {
        result = std::stoull(value, &consumed, 10);
    } catch (const std::exception &) {
        throw Error("Invalid chip database value: " + value);
    }
    if (consumed != value.size())
        throw Error("Invalid chip database value: " + value);
    return result;
}

template <typename T>
T checked_integer(std::uint64_t value, const std::string &field) {
    if (value > std::numeric_limits<T>::max())
        throw Error("Chip database " + field + " is out of range");
    return static_cast<T>(value);
}

std::uint8_t byte_parameter(std::uint64_t value) {
    if (value == undefined_parameter)
        return 0xff;
    return checked_integer<std::uint8_t>(value, "byte parameter");
}

std::uint8_t timing_value(double value) {
    if (value < 0.0)
        value = 0.0;
    value = std::ceil(value);
    if (value > 255.0)
        throw Error("Calculated FSMC timing exceeds one byte");
    return static_cast<std::uint8_t>(value);
}

} // namespace

std::uint64_t Chip::page_count() const {
    if (page_size == 0)
        throw Error("Chip page size is zero");
    return total_size / page_size;
}

std::uint64_t Chip::raw_page_size() const {
    return static_cast<std::uint64_t>(page_size) + spare_size;
}

std::uint64_t Chip::raw_total_size() const {
    return page_count() * raw_page_size();
}

std::vector<std::uint8_t> Chip::hal_configuration() const {
    constexpr double hclk_period_ns = 13.88;
    constexpr double data_setup_to_noe_ns = 25.0;

    const auto p = [this](Parameter parameter) {
        return static_cast<double>(parameters[parameter]);
    };

    double setup = std::max({p(t_cs), p(t_cls), p(t_als), p(t_clr), p(t_ar)}) -
                   p(t_wp);
    setup = setup / hclk_period_ns - 1.0;
    setup = setup <= 0.0 ? 1.0 : std::ceil(setup);

    double wait = std::max(p(t_wp), p(t_rp)) / hclk_period_ns - 1.0;
    wait = wait <= 0.0 ? 0.0 : std::ceil(wait);
    double read_wait = (p(t_rea) + data_setup_to_noe_ns) / hclk_period_ns - 1.0;
    read_wait = read_wait <= 0.0 ? 0.0 : std::ceil(read_wait);
    wait = std::max(wait, read_wait);

    double hiz = std::max({p(t_cs), p(t_als), p(t_cls)}) + p(t_wp) - p(t_ds);
    hiz = hiz / hclk_period_ns - 1.0;
    hiz = hiz <= 0.0 ? 0.0 : std::ceil(hiz);

    double hold = std::max({p(t_ch), p(t_clh), p(t_alh)}) / hclk_period_ns - 1.0;
    hold = hold <= 0.0 ? 2.0 : std::ceil(hold);

    while (((setup + 1.0) + (wait + 1.0) + (hold + 1.0)) * hclk_period_ns <
           std::max(p(t_wc), p(t_rc))) {
        setup += 1.0;
    }

    double ar = p(t_ar) / hclk_period_ns - 4.0 - setup;
    double clr = p(t_clr) / hclk_period_ns - 4.0 - setup;

    std::vector<std::uint8_t> configuration;
    configuration.reserve(22);
    configuration.push_back(timing_value(setup));
    configuration.push_back(timing_value(wait));
    configuration.push_back(timing_value(hold));
    configuration.push_back(timing_value(hiz));
    configuration.push_back(timing_value(clr));
    configuration.push_back(timing_value(ar));
    configuration.push_back(byte_parameter(parameters[row_cycles]));
    configuration.push_back(byte_parameter(parameters[column_cycles]));
    configuration.push_back(byte_parameter(parameters[read_first_command]));
    configuration.push_back(byte_parameter(parameters[read_second_command]));
    configuration.push_back(byte_parameter(parameters[read_spare_command]));
    configuration.push_back(byte_parameter(parameters[read_id_command]));
    configuration.push_back(byte_parameter(parameters[reset_command]));
    configuration.push_back(byte_parameter(parameters[write_first_command]));
    configuration.push_back(byte_parameter(parameters[write_second_command]));
    configuration.push_back(byte_parameter(parameters[erase_first_command]));
    configuration.push_back(byte_parameter(parameters[erase_second_command]));
    configuration.push_back(byte_parameter(parameters[status_command]));
    configuration.push_back(byte_parameter(parameters[set_features_command]));
    configuration.push_back(byte_parameter(parameters[enable_ecc_address]));
    configuration.push_back(byte_parameter(parameters[enable_ecc_value]));
    configuration.push_back(byte_parameter(parameters[disable_ecc_value]));
    return configuration;
}

bool Chip::matches(const protocol::ChipId &id) const {
    for (std::size_t index = 0; index < id.bytes.size(); ++index) {
        const auto expected = parameters[id_1 + index];
        if (expected != undefined_parameter && expected != id.bytes[index])
            return false;
    }
    return true;
}

void ChipDatabase::load(const std::filesystem::path &path) {
    std::ifstream input(path);
    if (!input)
        throw Error("Failed to open chip database: " + path.string());

    chips_.clear();
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        const std::string clean = trim(line);
        if (clean.empty() || clean.front() == '#')
            continue;
        try {
            const auto fields = split_csv(clean);
            if (fields.size() != 6 + Chip::parameter_count)
                throw Error("expected 41 fields, got " +
                            std::to_string(fields.size()));

            Chip chip;
            chip.name = fields[0];
            chip.page_size = checked_integer<std::uint32_t>(
                parse_parameter(fields[1], false), "page size");
            chip.block_size = checked_integer<std::uint32_t>(
                parse_parameter(fields[2], false), "block size");
            chip.total_size = parse_parameter(fields[3], false);
            chip.spare_size = checked_integer<std::uint32_t>(
                parse_parameter(fields[4], false), "spare size");
            chip.bad_block_mark_offset = checked_integer<std::uint8_t>(
                parse_parameter(fields[5], false), "bad-block mark offset");
            for (std::size_t index = 0; index < Chip::parameter_count; ++index)
                chip.parameters[index] =
                    parse_parameter(fields[index + 6], true);

            if (chip.page_size == 0 || chip.block_size == 0 ||
                chip.total_size == 0 || chip.block_size % chip.page_size != 0 ||
                chip.total_size % chip.block_size != 0) {
                throw Error("invalid NAND geometry");
            }
            chips_.push_back(std::move(chip));
        } catch (const Error &error) {
            throw Error(path.string() + ":" + std::to_string(line_number) +
                        ": " + error.what());
        }
    }
    if (chips_.empty())
        throw Error("Chip database is empty: " + path.string());
}

const Chip &ChipDatabase::first() const {
    if (chips_.empty())
        throw Error("Chip database is not loaded");
    return chips_.front();
}

const Chip *ChipDatabase::find_by_name(const std::string &name) const {
    const auto iterator = std::find_if(chips_.begin(), chips_.end(),
                                       [&name](const Chip &chip) {
                                           return chip.name == name;
                                       });
    return iterator == chips_.end() ? nullptr : &*iterator;
}

const Chip *ChipDatabase::find_by_id(const protocol::ChipId &id) const {
    const auto iterator = std::find_if(chips_.begin(), chips_.end(),
                                       [&id](const Chip &chip) {
                                           return chip.matches(id);
                                       });
    return iterator == chips_.end() ? nullptr : &*iterator;
}

} // namespace nandprog
