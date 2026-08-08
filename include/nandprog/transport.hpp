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
    virtual void read_exact(std::uint8_t *data, std::size_t size,
                            unsigned timeout_ms) = 0;

    void write_packet(const std::vector<std::uint8_t> &packet,
                      unsigned timeout_ms) {
        write_packet(packet.data(), packet.size(), timeout_ms);
    }
};

std::unique_ptr<Transport> make_serial_transport();

} // namespace nandprog
