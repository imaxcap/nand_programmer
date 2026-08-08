#include "nandprog/transport.hpp"

#include "nandprog/error.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

namespace nandprog {
namespace {

class PosixSerialTransport final : public Transport {
public:
    ~PosixSerialTransport() override { close(); }

    void open(const std::string &device, std::uint32_t baud_rate) override {
        if (fd_ >= 0)
            throw Error("Serial port is already open");
        fd_ = ::open(device.c_str(), O_RDWR | O_NOCTTY | O_CLOEXEC);
        if (fd_ < 0)
            throw Error("Failed to open " + device + ": " + std::strerror(errno));

        try {
            configure(baud_rate);
            if (tcflush(fd_, TCIOFLUSH) != 0)
                throw Error("Failed to flush " + device + ": " +
                            std::strerror(errno));
        } catch (...) {
            close();
            throw;
        }
    }

    void close() noexcept override {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    bool is_open() const noexcept override { return fd_ >= 0; }

    void write_packet(const std::uint8_t *data, std::size_t size,
                      unsigned timeout_ms) override {
        if (size == 0 || size > 64)
            throw Error("Serial protocol packet must contain 1..64 bytes");
        wait(POLLOUT, timeout_ms);
        const ssize_t written = ::write(fd_, data, size);
        if (written < 0)
            throw Error("Serial write failed: " + std::string(std::strerror(errno)));
        if (static_cast<std::size_t>(written) != size)
            throw Error("Serial write was partial; command packet was not sent");
    }

    void read_exact(std::uint8_t *data, std::size_t size,
                    unsigned timeout_ms) override {
        using Clock = std::chrono::steady_clock;
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
        std::size_t offset = 0;
        while (offset < size) {
            const auto now = Clock::now();
            if (now >= deadline)
                throw Error("Serial read timed out");
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - now);
            wait(POLLIN, static_cast<unsigned>(remaining.count() + 1));
            const ssize_t count = ::read(fd_, data + offset, size - offset);
            if (count < 0) {
                if (errno == EINTR)
                    continue;
                throw Error("Serial read failed: " +
                            std::string(std::strerror(errno)));
            }
            if (count == 0)
                continue;
            offset += static_cast<std::size_t>(count);
        }
    }

private:
    int fd_ = -1;

    void configure(std::uint32_t baud_rate) {
        termios settings{};
        if (tcgetattr(fd_, &settings) != 0)
            throw Error("Failed to read serial settings: " +
                        std::string(std::strerror(errno)));
        cfmakeraw(&settings);
        settings.c_cflag |= CLOCAL | CREAD;
        settings.c_cflag &= static_cast<tcflag_t>(~(CSTOPB | PARENB | CRTSCTS));
        settings.c_cflag &= static_cast<tcflag_t>(~CSIZE);
        settings.c_cflag |= CS8;

#ifdef B4000000
        if (baud_rate != 4000000)
            throw Error("Only the firmware CDC rate of 4000000 is supported");
        const speed_t speed = B4000000;
#else
        (void)baud_rate;
        throw Error("This platform does not define the required B4000000 rate");
#endif
        if (cfsetispeed(&settings, speed) != 0 ||
            cfsetospeed(&settings, speed) != 0 ||
            tcsetattr(fd_, TCSANOW, &settings) != 0) {
            throw Error("Failed to configure serial port: " +
                        std::string(std::strerror(errno)));
        }
    }

    void wait(short events, unsigned timeout_ms) {
        if (fd_ < 0)
            throw Error("Serial port is not open");
        pollfd descriptor{fd_, events, 0};
        int result = 0;
        do {
            result = ::poll(&descriptor, 1, static_cast<int>(timeout_ms));
        } while (result < 0 && errno == EINTR);
        if (result == 0)
            throw Error(events == POLLIN ? "Serial read timed out"
                                         : "Serial write timed out");
        if (result < 0)
            throw Error("Serial poll failed: " + std::string(std::strerror(errno)));
        if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
            throw Error("Serial port disconnected");
    }
};

} // namespace

std::unique_ptr<Transport> make_serial_transport() {
    return std::make_unique<PosixSerialTransport>();
}

} // namespace nandprog
