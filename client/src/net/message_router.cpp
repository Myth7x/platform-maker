#include "net/message_router.hpp"

#include "net_client.hpp"

#include <chrono>
#include <cstring>
#include <iterator>

namespace opm::client::net {
namespace {

LevelSnapshot levelDataToSnapshot(const opm::engine::LevelData& level)
{
    LevelSnapshot snap;
    snap.width = level.foliage.width;
    snap.height = level.foliage.height;
    snap.spawnX = level.spawnX;
    snap.spawnY = level.spawnY;
    snap.goalX = level.goalX;
    snap.goalY = level.goalY;
    snap.background = level.background.tileIndices;
    snap.foliage = level.foliage.tileIndices;
    snap.foreground = level.foreground.tileIndices;
    return snap;
}

} // namespace

bool MessageRouter::dispatch(const opm::protocol::Message& message,
                             const bool treatLevelSnapshotAsSideChannel)
{
    switch (message.type) {
        case opm::protocol::MessageType::RosterUpdate:
            pendingRosters_.push_back(opm::protocol::decodeRosterUpdatePayload(message.payload));
            return true;
        case opm::protocol::MessageType::LevelSnapshot:
            if (!treatLevelSnapshotAsSideChannel) {
                return false;
            }
            pendingLevelSnapshots_.push_back(
                levelDataToSnapshot(opm::protocol::decodeLevelSnapshotPayload(message.payload)));
            return true;
        case opm::protocol::MessageType::Pong:
            recordPong(message);
            return true;
        case opm::protocol::MessageType::MapVoteUpdate:
            pendingMapVoteUpdates_.push_back(opm::protocol::decodeMapVoteUpdatePayload(message.payload));
            return true;
        default:
            return false;
    }
}

void MessageRouter::recordPong(const opm::protocol::Message& message)
{
    if (message.payload.size() != sizeof(std::uint64_t)) {
        return;
    }
    std::uint64_t echoedNs = 0;
    std::memcpy(&echoedNs, message.payload.data(), sizeof(echoedNs));
    const auto sentAt = std::chrono::steady_clock::time_point(std::chrono::nanoseconds(echoedNs));
    const auto rtt = std::chrono::steady_clock::now() - sentAt;
    const auto rttMs = std::chrono::duration_cast<std::chrono::milliseconds>(rtt).count();
    if (rttMs < 0 || rttMs >= 60000) {
        return;
    }
    if (!hasPingSample_) {
        pingMs_ = static_cast<std::uint32_t>(rttMs);
        hasPingSample_ = true;
    } else {
        // Exponential moving average to smooth jitter.
        pingMs_ = static_cast<std::uint32_t>(
            (pingMs_ * 3 + static_cast<std::uint32_t>(rttMs)) / 4);
    }
}

void MessageRouter::drainRosterUpdates(std::vector<std::vector<opm::protocol::PlayerInfo>>& out)
{
    out.insert(out.end(),
        std::make_move_iterator(pendingRosters_.begin()),
        std::make_move_iterator(pendingRosters_.end()));
    pendingRosters_.clear();
}

void MessageRouter::drainLevelSnapshots(std::vector<LevelSnapshot>& out)
{
    out.insert(out.end(),
        std::make_move_iterator(pendingLevelSnapshots_.begin()),
        std::make_move_iterator(pendingLevelSnapshots_.end()));
    pendingLevelSnapshots_.clear();
}

void MessageRouter::drainMapVoteUpdates(std::vector<std::vector<opm::protocol::MapVote>>& out)
{
    out.insert(out.end(),
        std::make_move_iterator(pendingMapVoteUpdates_.begin()),
        std::make_move_iterator(pendingMapVoteUpdates_.end()));
    pendingMapVoteUpdates_.clear();
}

void MessageRouter::reset()
{
    pendingRosters_.clear();
    pendingLevelSnapshots_.clear();
    pendingMapVoteUpdates_.clear();
    pingMs_ = 0;
    hasPingSample_ = false;
}

} // namespace opm::client::net
