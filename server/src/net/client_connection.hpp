#pragma once
#include "game/peer_session.hpp"
#include "net/socket_compat.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace opm::server {

inline constexpr std::size_t kRecvChunkSize = 4096U;

// One connected peer: socket fd + recv buffer + lobby session state.
class ClientConnection {
public:
    explicit ClientConnection(socket_t fd);

    [[nodiscard]] socket_t fd() const noexcept { return fd_; }
    [[nodiscard]] std::vector<std::uint8_t>& recvBuffer() noexcept { return recvBuffer_; }
    [[nodiscard]] std::size_t recvOffset() const noexcept { return recvOffset_; }
    // Advance the read cursor after packets have been consumed. Compacts the
    // buffer (one memmove) when more than half the capacity has been consumed.
    void consumeRecv(std::size_t bytes) noexcept;
    [[nodiscard]] PeerSession& session() noexcept { return session_; }
    [[nodiscard]] const PeerSession& session() const noexcept { return session_; }

    // Drains all readable bytes into recvBuffer_. Returns false iff peer closed.
    [[nodiscard]] bool drainRecv(std::span<std::uint8_t> chunk);

    [[nodiscard]] bool send(std::span<const std::uint8_t> data) const;

private:
    socket_t fd_;
    std::vector<std::uint8_t> recvBuffer_;
    std::size_t recvOffset_ {0};
    PeerSession session_ {};
};

} // namespace opm::server
