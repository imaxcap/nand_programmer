#pragma once

#include "nandprog/chip_db.hpp"
#include "nandprog/nand_client.hpp"
#include "nandprog/transport.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace nandprog {

struct GlobalOptions {
    std::string device;
    std::filesystem::path database;
    std::optional<std::string> forced_chip;
};

class CommandShell {
public:
    CommandShell(GlobalOptions options, std::unique_ptr<Transport> transport);
    int run_repl();
    int execute(const std::vector<std::string> &arguments);
    static void print_help();

private:
    GlobalOptions options_;
    std::unique_ptr<Transport> transport_;
    NandClient client_;
    ChipDatabase database_;
    const Chip *chip_ = nullptr;
    protocol::FirmwareVersion firmware_version_{};
    protocol::ChipId chip_id_{};
    bool probed_ = false;

    void ensure_open();
    void ensure_probe();
    void command_id(const std::vector<std::string> &arguments);
    void command_probe(const std::vector<std::string> &arguments);
    void command_info() const;
    void command_read(const std::vector<std::string> &arguments, bool raw);
    void command_erase(const std::vector<std::string> &arguments);
    void command_write(const std::vector<std::string> &arguments, bool raw);
    void command_write_qpic(const std::vector<std::string> &arguments);
    void command_verify(const std::vector<std::string> &arguments);
};

} // namespace nandprog
