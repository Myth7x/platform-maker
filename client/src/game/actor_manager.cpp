#include "game/actor_manager.hpp"

#include <algorithm>

namespace opm::client::game {

opm::engine::PlayerState remoteStateToPlayerState(const opm::client::net::RemotePlayerState& s)
{
    opm::engine::PlayerState p;
    p.position.x = s.positionX;
    p.position.y = s.positionY;
    p.velocity.x = s.velocityX;
    p.velocity.y = s.velocityY;
    p.onGround = s.onGround;
    p.facingRight = s.facingRight;
    p.skidding = s.skidding;
    p.crouching = s.crouching;
    p.pSpeedActive = s.pSpeedActive;
    p.pSpeedMeter = s.pSpeedMeter;
    p.style = static_cast<opm::engine::PlayerStyle>(s.style);
    p.powerupTransitionFrames = s.powerupTransitionFrames;
    p.invincibilityFrames = s.invincibilityFrames;
    return p;
}

void ActorManager::resetLocalOnly()
{
    actors_.clear();
    Actor local;
    local.isLocal = true;
    actors_.push_back(local);
    lastServerTick_ = 0;
}

Actor* ActorManager::localActor()
{
    if (actors_.empty()) {
        return nullptr;
    }
    return &actors_.front();
}

Actor* ActorManager::findByServerIndex(const std::uint16_t serverIndex)
{
    for (auto& a : actors_) {
        if (a.serverIndex == serverIndex && serverIndex != kInvalidServerIndex) {
            return &a;
        }
    }
    return nullptr;
}

Actor& ActorManager::spawnRemote(const std::uint16_t serverIndex, const opm::protocol::PlayerInfo& info)
{
    Actor remote;
    remote.isLocal = false;
    remote.serverIndex = serverIndex;
    remote.info = info;
    actors_.push_back(remote);
    return actors_.back();
}

void ActorManager::despawnRemote(const std::uint16_t serverIndex)
{
    actors_.erase(
        std::remove_if(actors_.begin() + (actors_.empty() ? 0 : 1), actors_.end(),
            [serverIndex](const Actor& a) { return a.serverIndex == serverIndex; }),
        actors_.end());
}

void ActorManager::bindLocalToServer(const std::uint16_t serverIndex)
{
    if (Actor* local = localActor()) {
        local->serverIndex = serverIndex;
    }
}

void ActorManager::applyRoster(const std::vector<opm::protocol::PlayerInfo>& roster, const std::uint16_t localServerIndex)
{
    for (const auto& info : roster) {
        if (info.playerIndex == localServerIndex) {
            if (Actor* local = localActor()) {
                local->info = info;
            }
            continue;
        }
        Actor* existing = findByServerIndex(info.playerIndex);
        if (info.connected && existing == nullptr) {
            spawnRemote(info.playerIndex, info);
        } else if (!info.connected && existing != nullptr) {
            despawnRemote(info.playerIndex);
        } else if (existing != nullptr) {
            existing->info = info;
        }
    }
}

void ActorManager::applyStateUpdate(const opm::client::net::StateUpdate& update, const std::uint16_t localServerIndex)
{
    lastServerTick_ = update.serverTick;
    // Wire format is sparse — each record carries its own slotIndex.
    // Don't use the array position; the server only sends active slots.
    for (const auto& netPlayer : update.players) {
        const auto serverIndex = netPlayer.slotIndex;
        if (serverIndex == localServerIndex) {
            if (Actor* local = localActor()) {
                local->state = remoteStateToPlayerState(netPlayer);
            }
            continue;
        }
        Actor* actor = findByServerIndex(serverIndex);
        if (actor == nullptr) {
            // Spawning is driven exclusively by roster updates so that the
            // server's default-spawn state for empty slots does not create
            // phantom remote actors.
            continue;
        }
        actor->state = remoteStateToPlayerState(netPlayer);
    }
    worldActors_.clear();
    worldActors_.reserve(update.actors.size());
    for (const auto& a : update.actors) {
        opm::engine::ActorState s {};
        s.position = {a.positionX, a.positionY};
        s.velocity = {a.velocityX, a.velocityY};
        s.alive = a.alive;
        s.facingRight = a.facingRight;
        s.script = static_cast<opm::engine::ActorScript>(a.script);
        s.enemyKind = a.enemyKind;
        s.category = static_cast<opm::engine::ActorCategory>(a.category);
        worldActors_.push_back(s);
    }
}

void ActorManager::updateLocalState(const opm::engine::PlayerState& state)
{
    if (Actor* local = localActor()) {
        local->state = state;
    }
}

} // namespace opm::client::game
