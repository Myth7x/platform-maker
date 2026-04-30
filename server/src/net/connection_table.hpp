#pragma once
#include "net/client_connection.hpp"
#include "net/socket_compat.hpp"

#include <unordered_map>

namespace opm::server {

// Owns the set of live ClientConnection objects keyed by socket fd.
class ConnectionTable {
public:
    [[nodiscard]] ClientConnection* find(socket_t fd) noexcept
    {
        const auto it = connections_.find(fd);
        return it == connections_.end() ? nullptr : &it->second;
    }
    void add(socket_t fd) { connections_.emplace(fd, ClientConnection {fd}); }
    void remove(socket_t fd) { connections_.erase(fd); }
    [[nodiscard]] auto& map() noexcept { return connections_; }

private:
    std::unordered_map<socket_t, ClientConnection> connections_;
};

} // namespace opm::server
