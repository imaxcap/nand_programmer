#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace nandprog {

class Transport {
public:
    virtual ~Transport() = default;
    virtual void open(const std::string &device, std::uint32_t baud_rate) = 0;
    virtual void close() noexcept = 0;
    virtual bool is_open() const noexcept = 0;
    virtual void write_packet(const std::uint8_t *data, std::size_t size,
                              unsigned timeout_ms) = 0;
    virtual void write_buffer(const std::uint8_t *data, std::size_t size,
                              unsigned timeout_ms) {
        std::size_t offset = 0;
        while (offset < size) {
            std::size_t chunk = (size - offset > 64) ? 64 : (size - offset);
            write_packet(data + offset, chunk, timeout_ms);
            offset += chunk;
        }
    }
    virtual void read_exact(std::uint8_t *data, std::size_t size,
                            unsigned timeout_ms) = 0;
    virtual void flush() {}

    void write_packet(const std::vector<std::uint8_t> &packet,
                      unsigned timeout_ms) {
        write_packet(packet.data(), packet.size(), timeout_ms);
    }

    void write_buffer(const std::vector<std::uint8_t> &buffer,
                      unsigned timeout_ms) {
        write_buffer(buffer.data(), buffer.size(), timeout_ms);
    }
};

std::unique_ptr<Transport> make_serial_transport();

} // namespace nandprog
