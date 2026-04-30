#include "net/client_connection.hpp"

#include "net/sender.hpp"

namespace opm::server {

ClientConnection::ClientConnection(socket_t fd) : fd_(fd)
{
    recvBuffer_.reserve(kRecvChunkSize);
}

bool ClientConnection::drainRecv(std::span<std::uint8_t> chunk)
{
    while (true) {
        const ssize_t n = ::recv(fd_,
            reinterpret_cast<char*>(chunk.data()),
            static_cast<int>(chunk.size()),
            MSG_DONTWAIT);
        if (n == 0) {
            return false;
        }
        if (n < 0) {
            return true;
        }
        recvBuffer_.insert(recvBuffer_.end(), chunk.begin(), chunk.begin() + n);
    }
}

bool ClientConnection::send(std::span<const std::uint8_t> data) const
{
    return sendAll(fd_, data);
}

} // namespace opm::server
