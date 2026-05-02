#pragma once

#include "opm/protocol.hpp"

#include <chrono>
#include <cstdint>
#include <vector>

namespace opm::client::net {

struct LevelSnapshot;

// Owns the side-channel message queues that any incoming-message
// handler may need to defer for later consumer pickup, plus the ping
// state. Holds the dispatch logic in one place — both pollStateUpdate
// and awaitMessage on SessionClient feed messages here so they share
// the RosterUpdate / LevelSnapshot / Pong handling.
//
// Mirrors the role of the server's per-message handler family: turns a
// raw protocol message into either a queued side-channel update or a
// "this isn't mine — caller handle it" return.
class MessageRouter {
public:
    // Returns true if the message was a side-channel update (Roster /
    // LevelSnapshot / Pong) and was consumed by the router; false if
    // the caller still needs to handle the message themselves.
    //
    // `treatLevelSnapshotAsSideChannel` is false when the caller is
    // explicitly waiting for a LevelSnapshot (e.g. receiveLevelSnapshot
    // and awaitMessage(LevelSnapshot)) — in that case the snapshot is
    // returned to the caller instead of being queued.
    [[nodiscard]] bool dispatch(const opm::protocol::Message& message,
                                bool treatLevelSnapshotAsSideChannel = true);

    // Drain accumulated side-channel updates into `out`. Idempotent:
    // calling again immediately yields nothing.
    void drainRosterUpdates(std::vector<std::vector<opm::protocol::PlayerInfo>>& out);
    void drainLevelSnapshots(std::vector<LevelSnapshot>& out);
    void drainMapVoteUpdates(std::vector<std::vector<opm::protocol::MapVote>>& out);

    // Smoothed round-trip time in milliseconds. 0 when no Pong has
    // arrived yet (or after reset()).
    [[nodiscard]] std::uint32_t pingMs() const noexcept { return pingMs_; }

    // Drop all queued updates and reset ping state. Called by
    // SessionClient::disconnect.
    void reset();

private:
    void recordPong(const opm::protocol::Message& message);

    std::vector<std::vector<opm::protocol::PlayerInfo>> pendingRosters_ {};
    std::vector<LevelSnapshot>                          pendingLevelSnapshots_ {};
    std::vector<std::vector<opm::protocol::MapVote>>    pendingMapVoteUpdates_ {};
    std::uint32_t                                       pingMs_ {0};
    bool                                                hasPingSample_ {false};
};

} // namespace opm::client::net
