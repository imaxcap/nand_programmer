#include "nandprog/commands.hpp"

#include "nandprog/error.hpp"
#include "nandprog/transport.hpp"
#include "nandprog/util.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

void print_global_help() {
    std::cout
        << "Usage: nandprog [OPTIONS] [COMMAND ...]\n"
        << "With no COMMAND, nandprog starts an interactive REPL.\n\n"
        << "Options:\n"
        << "  -d, --device PATH   CDC serial device (/dev/ttyACM0 or COM3)\n"
        << "  --db PATH           parallel NAND CSV database\n"
        << "  --chip NAME         force a database chip during automatic probe\n"
        << "  -h, --help          show help\n"
        << "  --version           show host CLI version\n";
}

} // namespace

int main(int argc, char **argv) {
    try {
        nandprog::GlobalOptions options;
#ifdef _WIN32
        options.device = "COM1";
#else
        options.device = "/dev/ttyACM0";
#endif
        std::filesystem::path requested_database;
        std::vector<std::string> command;

        for (int index = 1; index < argc; ++index) {
            const std::string argument = argv[index];
            if (command.empty() && (argument == "-d" || argument == "--device")) {
                if (++index >= argc)
                    throw nandprog::Error(argument + " requires a value");
                options.device = argv[index];
            } else if (command.empty() && argument == "--db") {
                if (++index >= argc)
                    throw nandprog::Error("--db requires a value");
                requested_database = argv[index];
            } else if (command.empty() && argument == "--chip") {
                if (++index >= argc)
                    throw nandprog::Error("--chip requires a value");
                options.forced_chip = argv[index];
            } else if (command.empty() && argument == "--version") {
                std::cout << "nandprog 0.2.0\n";
                return 0;
            } else if (command.empty() && (argument == "-h" || argument == "--help")) {
                print_global_help();
                std::cout << '\n';
                nandprog::CommandShell::print_help();
                return 0;
            } else {
                command.push_back(argument);
            }
        }

        options.database = nandprog::find_database(requested_database, argv[0]);
        nandprog::CommandShell shell(std::move(options),
                                     nandprog::make_serial_transport());
        if (command.empty())
            return shell.run_repl();
        return shell.execute(command);
    } catch (const nandprog::VerifyMismatch &error) {
        std::cerr << "verify failed: " << error.what() << '\n';
        return 5;
    } catch (const nandprog::FirmwareError &error) {
        std::cerr << "firmware error: " << error.what() << '\n';
        return 4;
    } catch (const nandprog::Error &error) {
        std::cerr << "error: " << error.what() << '\n';
        return 3;
    } catch (const std::exception &error) {
        std::cerr << "unexpected error: " << error.what() << '\n';
        return 4;
    }
}
