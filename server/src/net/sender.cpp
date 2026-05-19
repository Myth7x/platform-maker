#include "net/sender.hpp"

namespace opm::server {

bool sendAll(socket_t fd, std::span<const std::uint8_t> data)
{
    std::size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t n = ::send(fd,
            reinterpret_cast<const char*>(data.data() + sent),
            static_cast<int>(data.size() - sent), 0);
        if (n > 0) {
            sent += static_cast<std::size_t>(n);
            continue;
        }
        // EAGAIN/EWOULDBLOCK: kernel send buffer is full — treat as failure
        // so the tick loop is never stalled waiting for a slow client.
        return false;
    }
    return true;
}

} // namespace opm::server
