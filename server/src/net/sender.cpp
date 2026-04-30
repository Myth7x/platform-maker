#include "net/sender.hpp"

#include <chrono>
#include <thread>

namespace opm::server {

bool sendAll(socket_t fd, std::span<const std::uint8_t> data)
{
    std::size_t sent = 0;
    int wouldBlockRetries = 0;
    while (sent < data.size()) {
        const ssize_t n = ::send(fd,
            reinterpret_cast<const char*>(data.data() + sent),
            static_cast<int>(data.size() - sent), 0);
        if (n > 0) {
            sent += static_cast<std::size_t>(n);
            wouldBlockRetries = 0;
            continue;
        }
        if (n < 0 && sockWouldBlock() && wouldBlockRetries < 4) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            ++wouldBlockRetries;
            continue;
        }
        return false;
    }
    return true;
}

} // namespace opm::server
