#pragma once

#include "net_client.hpp"

#include "opm/engine.hpp"
#include "opm/protocol.hpp"

#include <cstdint>
#include <vector>

namespace opm::client::game {

inline constexpr std::uint16_t kInvalidServerIndex = 0xFFFFU;

struct Actor {
    bool isLocal {false};
    std::uint16_t serverIndex {kInvalidServerIndex};
    opm::protocol::PlayerInfo info {};
    opm::engine::PlayerState state {};
};

opm::engine::PlayerState remoteStateToPlayerState(const opm::client::net::RemotePlayerState& s);

// Owns the local + remote player roster for a session. The local actor
// always lives at actors_[0] and is created by resetLocalOnly(); remote
// actors are added/removed in response to RosterUpdate broadcasts and
// updated in place from StateUpdate messages.
class ActorManager {
public:
    void resetLocalOnly();

    Actor* localActor();
    Actor* findByServerIndex(std::uint16_t serverIndex);

    Actor& spawnRemote(std::uint16_t serverIndex, const opm::protocol::PlayerInfo& info = {});
    void despawnRemote(std::uint16_t serverIndex);
    void bindLocalToServer(std::uint16_t serverIndex);

    void applyRoster(const std::vector<opm::protocol::PlayerInfo>& roster, std::uint16_t localServerIndex);
    void applyStateUpdate(const opm::client::net::StateUpdate& update, std::uint16_t localServerIndex);

    void setWorldActors(const std::vector<opm::engine::ActorState>& actors) { worldActors_ = actors; }
    [[nodiscard]] const std::vector<opm::engine::ActorState>& worldActors() const { return worldActors_; }

    void updateLocalState(const opm::engine::PlayerState& state);

    [[nodiscard]] const std::vector<Actor>& actors() const { return actors_; }
    [[nodiscard]] std::size_t size() const { return actors_.size(); }
    [[nodiscard]] std::uint32_t lastServerTick() const { return lastServerTick_; }

private:
    std::vector<Actor> actors_;
    std::vector<opm::engine::ActorState> worldActors_ {};
    std::uint32_t lastServerTick_ {0};
};

} // namespace opm::client::game
