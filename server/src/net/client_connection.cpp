#include "net/client_connection.hpp"

#include "net/sender.hpp"

#include <cstring>

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

void ClientConnection::consumeRecv(std::size_t bytes) noexcept
{
    recvOffset_ += bytes;
    // Compact when the unconsumed tail fits in the first half of the buffer.
    // This amortises the memmove cost: we shift at most once per two full
    // buffer traversals instead of on every packet dispatch.
    if (recvOffset_ >= recvBuffer_.size()) {
        recvBuffer_.clear();
        recvOffset_ = 0;
    } else if (recvOffset_ >= recvBuffer_.capacity() / 2) {
        const std::size_t remaining = recvBuffer_.size() - recvOffset_;
        std::memmove(recvBuffer_.data(), recvBuffer_.data() + recvOffset_, remaining);
        recvBuffer_.resize(remaining);
        recvOffset_ = 0;
    }
}

bool ClientConnection::send(std::span<const std::uint8_t> data) const
{
    return sendAll(fd_, data);
}

} // namespace opm::server
