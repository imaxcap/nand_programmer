#include "nandprog/transport.hpp"

#include "nandprog/error.hpp"
#include "nandprog/util.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <string>

namespace nandprog {
namespace {

std::string windows_error(const std::string &prefix) {
    return prefix + " (Win32 error " + std::to_string(GetLastError()) + ")";
}

std::string device_path(const std::string &device) {
    if (device.rfind("\\\\.\\", 0) == 0)
        return device;
    if (device.size() >= 3 && (device[0] == 'C' || device[0] == 'c') &&
        (device[1] == 'O' || device[1] == 'o') &&
        (device[2] == 'M' || device[2] == 'm'))
        return "\\\\.\\" + device;
    return device;
}

class Win32SerialTransport final : public Transport {
public:
    ~Win32SerialTransport() override { close(); }

    void open(const std::string &device, std::uint32_t baud_rate) override {
        if (is_open())
            throw Error("Serial port is already open");
        const std::string path = device_path(device);
        log_debug("Win32SerialTransport: opening " + path + " at baud " + std::to_string(baud_rate));
        handle_ = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                              nullptr, OPEN_EXISTING, 0, nullptr);
        if (!is_open())
            throw Error(windows_error("Failed to open " + device));

        try {
            if (!SetupComm(handle_, 4096, 4096))
                throw Error(windows_error("SetupComm failed"));
            DCB dcb{};
            dcb.DCBlength = sizeof(dcb);
            if (!GetCommState(handle_, &dcb))
                throw Error(windows_error("GetCommState failed"));
            dcb.BaudRate = baud_rate;
            dcb.ByteSize = 8;
            dcb.Parity = NOPARITY;
            dcb.StopBits = ONESTOPBIT;
            dcb.fBinary = TRUE;
            dcb.fParity = FALSE;
            dcb.fOutxCtsFlow = FALSE;
            dcb.fOutxDsrFlow = FALSE;
            dcb.fDtrControl = DTR_CONTROL_ENABLE;
            dcb.fDsrSensitivity = FALSE;
            dcb.fOutX = FALSE;
            dcb.fInX = FALSE;
            dcb.fRtsControl = RTS_CONTROL_ENABLE;
            if (!SetCommState(handle_, &dcb))
                throw Error(windows_error("SetCommState failed"));
            set_timeouts(5000);
            if (!PurgeComm(handle_, PURGE_RXCLEAR | PURGE_TXCLEAR))
                throw Error(windows_error("PurgeComm failed"));
            log_debug("Win32SerialTransport: opened successfully with DTR/RTS enabled");
        } catch (...) {
            close();
            throw;
        }
    }

    void close() noexcept override {
        if (is_open()) {
            log_debug("Win32SerialTransport: closing port handle");
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
    }

    bool is_open() const noexcept override {
        return handle_ != INVALID_HANDLE_VALUE;
    }

    void write_packet(const std::uint8_t *data, std::size_t size,
                      unsigned timeout_ms) override {
        if (size == 0 || size > 64)
            throw Error("Serial protocol packet must contain 1..64 bytes");
        set_timeouts(timeout_ms);
        log_debug("Win32 TX (" + std::to_string(size) + "B): " + hex_dump(data, size));
        DWORD written = 0;
        if (!WriteFile(handle_, data, static_cast<DWORD>(size), &written, nullptr))
            throw Error(windows_error("Serial write failed"));
        if (written != size)
            throw Error("Serial write was partial; command packet was not sent");
    }

    void write_buffer(const std::uint8_t *data, std::size_t size,
                      unsigned timeout_ms) override {
        if (size == 0)
            return;
        set_timeouts(timeout_ms);
        std::size_t offset = 0;
        while (offset < size) {
            DWORD written = 0;
            if (!WriteFile(handle_, data + offset, static_cast<DWORD>(size - offset), &written, nullptr))
                throw Error(windows_error("Serial buffer write failed"));
            if (written == 0)
                throw Error("Serial buffer write failed with zero bytes written");
            offset += static_cast<std::size_t>(written);
        }
    }

    void read_exact(std::uint8_t *data, std::size_t size,
                    unsigned timeout_ms) override {
        using Clock = std::chrono::steady_clock;
        const auto start_time = Clock::now();
        const auto deadline = start_time + std::chrono::milliseconds(timeout_ms);
        log_debug("Win32 RX waiting for " + std::to_string(size) + "B (timeout=" + std::to_string(timeout_ms) + "ms)...");
        std::size_t offset = 0;
        while (offset < size) {
            const auto now = Clock::now();
            if (now >= deadline) {
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time).count();
                log_debug("Win32 RX TIMEOUT after " + std::to_string(elapsed) + "ms, received " + std::to_string(offset) + "/" + std::to_string(size) + "B");
                throw Error("Serial read timed out");
            }
            DWORD count = 0;
            const DWORD requested = static_cast<DWORD>(
                std::min<std::size_t>(size - offset, MAXDWORD));
            if (!ReadFile(handle_, data + offset, requested, &count, nullptr))
                throw Error(windows_error("Serial read failed"));
            if (count == 0) {
                Sleep(1);
                continue;
            }
            log_debug("Win32 RX got " + std::to_string(count) + "B: " + hex_dump(data + offset, count));
            offset += count;
        }
    }

    void flush() override {
        if (is_open()) {
            log_debug("Win32SerialTransport: flushing RX/TX buffers");
            PurgeComm(handle_, PURGE_RXCLEAR | PURGE_TXCLEAR);
        }
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;

    void set_timeouts(unsigned timeout_ms) {
        COMMTIMEOUTS timeouts{};
        timeouts.ReadIntervalTimeout = MAXDWORD;
        timeouts.ReadTotalTimeoutMultiplier = 0;
        timeouts.ReadTotalTimeoutConstant = 0;
        timeouts.WriteTotalTimeoutMultiplier = 0;
        timeouts.WriteTotalTimeoutConstant = timeout_ms;
        if (!SetCommTimeouts(handle_, &timeouts))
            throw Error(windows_error("SetCommTimeouts failed"));
    }
};

} // namespace

std::unique_ptr<Transport> make_serial_transport() {
    return std::make_unique<Win32SerialTransport>();
}

} // namespace nandprog
