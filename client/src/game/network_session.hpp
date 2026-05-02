#pragma once

#include "game/actor_manager.hpp"
#include "net_client.hpp"

#include "opm/level.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace opm::client::game {

// The client-side analog of the server's PeerSession: bundles the
// owned SessionClient with the per-connection state we want to drive
// the UI from (connected flag, our local player slot, the level the
// server told us to play, and the actor roster).
//
// Held as a single global by client_app.cpp for now (matching the
// pre-refactor shape). When ClientApp eventually owns this directly
// the global will go away.
struct NetworkSessionContext {
    std::unique_ptr<opm::client::net::SessionClient> session {};
    bool connected {false};
    std::uint16_t localPlayerIndex {kInvalidServerIndex};
    opm::engine::LevelData networkLevel {};
    ActorManager actors {};
};

// Result of a connection attempt — either ok with an info message, or
// failed with an error message.
struct ConnectResult {
    bool ok {false};
    std::string message;
};

// Parses "host:port" (e.g. "127.0.0.1:34900") into hostOut + portOut.
// Returns false on malformed input.
[[nodiscard]] bool parseAddress(std::string_view input, std::string& hostOut, std::uint16_t& portOut);

// Converts a wire LevelSnapshot into the engine's LevelData. Tolerant
// of older servers that don't include every layer.
opm::engine::LevelData levelFromSnapshot(const opm::client::net::LevelSnapshot& snapshot);

// Establishes a session-less connection on `net` (used for level
// browsing / saving). Lazily creates net.session if it doesn't exist.
ConnectResult tryConnect(NetworkSessionContext& net,
                         const std::string& host, std::uint16_t port);

struct LobbyListing {
    std::string name {};
    std::uint32_t players {0};
    std::uint32_t capacity {0};
};

// Connect (if not already) and request the server's lobby list. Fills
// `out` and returns ok on success.
ConnectResult fetchLobbyList(NetworkSessionContext& net,
                             const std::string& host, std::uint16_t port,
                             std::vector<LobbyListing>& out);

// Join a specific lobby by name (must match an entry the server
// advertised). Receives the level snapshot, primes the actor roster,
// drains one StateUpdate. Caller transitions to the in-lobby screen
// on success.
ConnectResult joinNamedLobby(NetworkSessionContext& net,
                             const std::string& host, std::uint16_t port,
                             const std::string& lobbyName);

} // namespace opm::client::game
