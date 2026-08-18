#include "nandprog/transport.hpp"

#include "nandprog/error.hpp"

#include <windows.h>

#include <algorithm>
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
            dcb.fDtrControl = DTR_CONTROL_DISABLE;
            dcb.fDsrSensitivity = FALSE;
            dcb.fOutX = FALSE;
            dcb.fInX = FALSE;
            dcb.fRtsControl = RTS_CONTROL_DISABLE;
            if (!SetCommState(handle_, &dcb))
                throw Error(windows_error("SetCommState failed"));
            if (!PurgeComm(handle_, PURGE_RXCLEAR | PURGE_TXCLEAR))
                throw Error(windows_error("PurgeComm failed"));
        } catch (...) {
            close();
            throw;
        }
    }

    void close() noexcept override {
        if (is_open()) {
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
        DWORD written = 0;
        if (!WriteFile(handle_, data, static_cast<DWORD>(size), &written, nullptr))
            throw Error(windows_error("Serial write failed"));
        if (written != size)
            throw Error("Serial write was partial; command packet was not sent");
    }

    void read_exact(std::uint8_t *data, std::size_t size,
                    unsigned timeout_ms) override {
        set_timeouts(timeout_ms);
        std::size_t offset = 0;
        while (offset < size) {
            DWORD count = 0;
            const DWORD requested = static_cast<DWORD>(
                std::min<std::size_t>(size - offset, MAXDWORD));
            if (!ReadFile(handle_, data + offset, requested, &count, nullptr))
                throw Error(windows_error("Serial read failed"));
            if (count == 0)
                throw Error("Serial read timed out");
            offset += count;
        }
    }

    void flush() override {}

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;

    void set_timeouts(unsigned timeout_ms) {
        COMMTIMEOUTS timeouts{};
        timeouts.ReadIntervalTimeout = timeout_ms;
        timeouts.ReadTotalTimeoutConstant = timeout_ms;
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
