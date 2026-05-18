#include "opm/protocol.hpp"

#include <array>
#include <cstring>
#include <stdexcept>

namespace opm::protocol {
namespace {

constexpr std::array<std::uint8_t, 3> kMagic {'O', 'P', 'M'};
constexpr std::uint8_t kVersion = 1;
constexpr std::size_t kHeaderSize = 9;

void writeU32(std::vector<std::uint8_t>& out, const std::uint32_t value)
{
    out.push_back(static_cast<std::uint8_t>((value >> 0) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFFU));
}

void writeU16(std::vector<std::uint8_t>& out, const std::uint16_t value)
{
    out.push_back(static_cast<std::uint8_t>((value >> 0) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFU));
}

void writeF32(std::vector<std::uint8_t>& out, const float value)
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(float));
    writeU32(out, bits);
}

void writeBool(std::vector<std::uint8_t>& out, const bool value)
{
    out.push_back(static_cast<std::uint8_t>(value ? 1U : 0U));
}

void writeString(std::vector<std::uint8_t>& out, const std::string& value)
{
    writeU32(out, static_cast<std::uint32_t>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
}

std::uint32_t readU32(const std::vector<std::uint8_t>& bytes, const std::size_t offset)
{
    return static_cast<std::uint32_t>(bytes[offset + 0]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
}

std::uint16_t readU16(const std::vector<std::uint8_t>& bytes, const std::size_t offset)
{
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 0]) |
                                      (static_cast<std::uint16_t>(bytes[offset + 1]) << 8U));
}

class PayloadReader {
public:
    explicit PayloadReader(const std::vector<std::uint8_t>& bytes)
        : bytes_(bytes)
    {
    }

    [[nodiscard]] bool done() const
    {
        return cursor_ == bytes_.size();
    }

    std::uint8_t readU8()
    {
        require(1U);
        return bytes_[cursor_++];
    }

    std::uint32_t readU32()
    {
        require(4U);
        const auto value = opm::protocol::readU32(bytes_, cursor_);
        cursor_ += 4U;
        return value;
    }

    std::uint16_t readU16()
    {
        require(2U);
        const auto value = opm::protocol::readU16(bytes_, cursor_);
        cursor_ += 2U;
        return value;
    }

    float readF32()
    {
        const std::uint32_t bits = readU32();
        float value = 0.0F;
        std::memcpy(&value, &bits, sizeof(float));
        return value;
    }

    bool readBool()
    {
        return readU8() != 0U;
    }

    std::string readString()
    {
        const auto length = static_cast<std::size_t>(readU32());
        require(length);
        const auto begin = bytes_.begin() + static_cast<std::ptrdiff_t>(cursor_);
        const auto end = begin + static_cast<std::ptrdiff_t>(length);
        cursor_ += length;
        return std::string(begin, end);
    }

private:
    void require(const std::size_t count) const
    {
        if (cursor_ + count > bytes_.size()) {
            throw std::runtime_error("Protocol payload underflow");
        }
    }

    const std::vector<std::uint8_t>& bytes_;
    std::size_t cursor_ {0};
};

} // namespace

std::vector<std::uint8_t> encodeMessage(const Message& message)
{
    std::vector<std::uint8_t> bytes;
    bytes.reserve(kHeaderSize + message.payload.size());

    bytes.insert(bytes.end(), kMagic.begin(), kMagic.end());
    bytes.push_back(kVersion);
    bytes.push_back(static_cast<std::uint8_t>(message.type));
    writeU32(bytes, static_cast<std::uint32_t>(message.payload.size()));
    bytes.insert(bytes.end(), message.payload.begin(), message.payload.end());

    return bytes;
}

Message decodeMessage(const std::vector<std::uint8_t>& bytes)
{
    if (bytes.size() < kHeaderSize) {
        throw std::runtime_error("Protocol packet too small");
    }

    if (bytes[0] != kMagic[0] || bytes[1] != kMagic[1] || bytes[2] != kMagic[2]) {
        throw std::runtime_error("Protocol magic mismatch");
    }

    if (bytes[3] != kVersion) {
        throw std::runtime_error("Protocol version mismatch");
    }

    const auto payloadSize = static_cast<std::size_t>(readU32(bytes, 5));
    if (bytes.size() != (kHeaderSize + payloadSize)) {
        throw std::runtime_error("Protocol payload size mismatch");
    }

    Message message;
    message.type = static_cast<MessageType>(bytes[4]);
    message.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(kHeaderSize), bytes.end());
    return message;
}

std::vector<std::uint8_t> payloadFromString(const std::string& text)
{
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

std::string payloadToString(const std::vector<std::uint8_t>& payload)
{
    return std::string(payload.begin(), payload.end());
}

std::vector<std::uint8_t> encodeLobbyListPayload(const std::vector<LobbyEntry>& lobbies)
{
    std::vector<std::uint8_t> out;
    writeU32(out, static_cast<std::uint32_t>(lobbies.size()));
    for (const auto& lobby : lobbies) {
        writeString(out, lobby.name);
        writeU32(out, lobby.players);
        writeU32(out, lobby.capacity);
    }
    return out;
}

std::vector<LobbyEntry> decodeLobbyListPayload(const std::vector<std::uint8_t>& payload)
{
    PayloadReader reader(payload);
    const std::uint32_t count = reader.readU32();
    std::vector<LobbyEntry> lobbies;
    lobbies.reserve(static_cast<std::size_t>(count));

    for (std::uint32_t i = 0; i < count; ++i) {
        LobbyEntry entry;
        entry.name = reader.readString();
        entry.players = reader.readU32();
        entry.capacity = reader.readU32();
        lobbies.push_back(entry);
    }

    if (!reader.done()) {
        throw std::runtime_error("Lobby list payload has trailing bytes");
    }

    return lobbies;
}

std::vector<std::uint8_t> encodeLobbyJoinRequestPayload(const std::string& lobbyName)
{
    std::vector<std::uint8_t> out;
    writeString(out, lobbyName);
    return out;
}

std::string decodeLobbyJoinRequestPayload(const std::vector<std::uint8_t>& payload)
{
    PayloadReader reader(payload);
    const std::string lobbyName = reader.readString();
    if (!reader.done()) {
        throw std::runtime_error("Lobby join request payload has trailing bytes");
    }
    return lobbyName;
}

std::vector<std::uint8_t> encodeLobbyJoinResponsePayload(const LobbyJoinResponseData& payload)
{
    std::vector<std::uint8_t> out;
    writeBool(out, payload.accepted);
    writeU16(out, payload.playerIndex);
    writeU32(out, payload.tickRateHz);
    writeString(out, payload.lobbyName);
    writeString(out, payload.reason);
    writeU32(out, static_cast<std::uint32_t>(payload.roster.size()));
    for (const auto& player : payload.roster) {
        writeU16(out, player.playerIndex);
        writeBool(out, player.connected);
        out.push_back(player.colorR);
        out.push_back(player.colorG);
        out.push_back(player.colorB);
        writeString(out, player.displayName);
    }
    return out;
}

LobbyJoinResponseData decodeLobbyJoinResponsePayload(const std::vector<std::uint8_t>& payload)
{
    PayloadReader reader(payload);
    LobbyJoinResponseData response;
    response.accepted = reader.readBool();
    response.playerIndex = reader.readU16();
    response.tickRateHz = reader.readU32();
    response.lobbyName = reader.readString();
    response.reason = reader.readString();
    const std::uint32_t rosterCount = reader.readU32();
    response.roster.reserve(static_cast<std::size_t>(rosterCount));
    for (std::uint32_t i = 0; i < rosterCount; ++i) {
        PlayerInfo info;
        info.playerIndex = reader.readU16();
        info.connected = reader.readBool();
        info.colorR = reader.readU8();
        info.colorG = reader.readU8();
        info.colorB = reader.readU8();
        info.displayName = reader.readString();
        response.roster.push_back(info);
    }
    if (!reader.done()) {
        throw std::runtime_error("Lobby join response payload has trailing bytes");
    }
    return response;
}

namespace {

void writeTileLayer(std::vector<std::uint8_t>& out, const opm::engine::TileLayer& layer)
{
    writeU32(out, static_cast<std::uint32_t>(layer.tileIndices.size()));
    for (const auto tile : layer.tileIndices) {
        writeU16(out, tile);
    }
}

void readTileLayer(PayloadReader& reader, opm::engine::TileLayer& layer,
    std::uint32_t width, std::uint32_t height)
{
    const auto tileCount = static_cast<std::size_t>(reader.readU32());
    layer.width = width;
    layer.height = height;
    layer.tileIndices.clear();
    layer.tileIndices.reserve(tileCount);
    for (std::size_t i = 0; i < tileCount; ++i) {
        layer.tileIndices.push_back(reader.readU16());
    }
}

} // namespace

std::vector<std::uint8_t> encodeLevelSnapshotPayload(const opm::engine::LevelData& level)
{
    std::vector<std::uint8_t> out;
    writeU32(out, level.foliage.width);
    writeU32(out, level.foliage.height);
    writeF32(out, level.spawnX);
    writeF32(out, level.spawnY);
    writeF32(out, level.goalX);
    writeF32(out, level.goalY);
    writeTileLayer(out, level.background);
    writeTileLayer(out, level.foliage);
    writeTileLayer(out, level.foreground);
    writeU32(out, static_cast<std::uint32_t>(level.actors.size()));
    for (const auto& actor : level.actors) {
        writeF32(out, actor.x);
        writeF32(out, actor.y);
        out.push_back(static_cast<std::uint8_t>(actor.script));
        std::uint8_t flags = 0U;
        if (actor.diesWhenStomped)  flags |= 0x01U;
        if (actor.canJumpObstacles) flags |= 0x02U;
        if (actor.canJumpRandom)    flags |= 0x04U;
        if (actor.canFly)           flags |= 0x08U;
        out.push_back(flags);
        out.push_back(actor.enemyKind);
        out.push_back(static_cast<std::uint8_t>(actor.category));
    }

    // Per-tile collision overrides: u32 count, then for each entry
    // u16 tileId + u8 flags { bit 0=top, 1=bottom, 2=left, 3=right, 4=oneWayTop }.
    writeU32(out, static_cast<std::uint32_t>(level.tileCollisionOverrides.size()));
    for (const auto& [tileId, mask] : level.tileCollisionOverrides) {
        writeU16(out, tileId);
        std::uint8_t maskBits = 0U;
        if (mask.solidTop)    maskBits |= 0x01U;
        if (mask.solidBottom) maskBits |= 0x02U;
        if (mask.solidLeft)   maskBits |= 0x04U;
        if (mask.solidRight)  maskBits |= 0x08U;
        if (mask.oneWayTop)   maskBits |= 0x10U;
        out.push_back(maskBits);
    }
    return out;
}

opm::engine::LevelData decodeLevelSnapshotPayload(const std::vector<std::uint8_t>& payload)
{
    PayloadReader reader(payload);
    opm::engine::LevelData level;
    const auto width = reader.readU32();
    const auto height = reader.readU32();
    level.spawnX = reader.readF32();
    level.spawnY = reader.readF32();
    level.goalX = reader.readF32();
    level.goalY = reader.readF32();
    readTileLayer(reader, level.background, width, height);
    readTileLayer(reader, level.foliage,    width, height);
    readTileLayer(reader, level.foreground, width, height);

    // Actors are optional for backward compatibility with snapshots that
    // pre-date the actor layer.
    if (!reader.done()) {
        const auto actorCount = reader.readU32();
        level.actors.reserve(static_cast<std::size_t>(actorCount));
        for (std::uint32_t i = 0; i < actorCount; ++i) {
            opm::engine::ActorSpawn a {};
            a.x = reader.readF32();
            a.y = reader.readF32();
            a.script = static_cast<opm::engine::ActorScript>(reader.readU8());
            // Flags byte is appended after script. For backward compat with
            // pre-flags snapshots that only stored x/y/script, we only read
            // it if there are still bytes left before the next actor.
            if (!reader.done()) {
                const std::uint8_t flags = reader.readU8();
                a.diesWhenStomped  = (flags & 0x01U) != 0U;
                a.canJumpObstacles = (flags & 0x02U) != 0U;
                a.canJumpRandom    = (flags & 0x04U) != 0U;
                a.canFly           = (flags & 0x08U) != 0U;
            }
            if (!reader.done()) {
                a.enemyKind = reader.readU8();
            }
            if (!reader.done()) {
                a.category = static_cast<opm::engine::ActorCategory>(reader.readU8());
            }
            level.actors.push_back(a);
        }
    }

    // Tile collision overrides — optional trailing block for older payloads.
    if (!reader.done()) {
        const auto overrideCount = reader.readU32();
        for (std::uint32_t i = 0; i < overrideCount; ++i) {
            const auto tileId = reader.readU16();
            const auto bits = reader.readU8();
            opm::engine::TileCollisionMask mask {};
            mask.solidTop    = (bits & 0x01U) != 0U;
            mask.solidBottom = (bits & 0x02U) != 0U;
            mask.solidLeft   = (bits & 0x04U) != 0U;
            mask.solidRight  = (bits & 0x08U) != 0U;
            mask.oneWayTop   = (bits & 0x10U) != 0U;
            level.tileCollisionOverrides[tileId] = mask;
        }
    }

    if (!reader.done()) {
        throw std::runtime_error("Level snapshot payload has trailing bytes");
    }

    return level;
}

std::vector<std::uint8_t> encodeMovementInputPayload(const opm::engine::InputFrame& input)
{
    std::vector<std::uint8_t> out;
    writeU32(out, input.frameIndex);

    std::uint8_t flags = 0U;
    if (input.moveLeft) {
        flags |= 0x01U;
    }
    if (input.moveRight) {
        flags |= 0x02U;
    }
    if (input.jumpPressed) {
        flags |= 0x04U;
    }
    if (input.jumpHeld) {
        flags |= 0x08U;
    }
    if (input.runHeld) {
        flags |= 0x10U;
    }
    if (input.crouchHeld) {
        flags |= 0x20U;
    }
    out.push_back(flags);
    return out;
}

opm::engine::InputFrame decodeMovementInputPayload(const std::vector<std::uint8_t>& payload)
{
    PayloadReader reader(payload);
    opm::engine::InputFrame input;
    input.frameIndex = reader.readU32();
    const std::uint8_t flags = reader.readU8();
    input.moveLeft = (flags & 0x01U) != 0U;
    input.moveRight = (flags & 0x02U) != 0U;
    input.jumpPressed = (flags & 0x04U) != 0U;
    input.jumpHeld = (flags & 0x08U) != 0U;
    input.runHeld = (flags & 0x10U) != 0U;
    input.crouchHeld = (flags & 0x20U) != 0U;

    if (!reader.done()) {
        throw std::runtime_error("Movement input payload has trailing bytes");
    }

    return input;
}

std::vector<std::uint8_t> encodeStateUpdatePayload(const StateUpdateData& update)
{
    std::vector<std::uint8_t> out;
    writeU32(out, update.serverTick);
    writeU32(out, static_cast<std::uint32_t>(update.players.size()));
    for (const auto& player : update.players) {
        writeU16(out, player.slotIndex);
        writeF32(out, player.positionX);
        writeF32(out, player.positionY);
        writeF32(out, player.velocityX);
        writeF32(out, player.velocityY);
        writeBool(out, player.onGround);
        writeBool(out, player.facingRight);
        writeBool(out, player.skidding);
        writeBool(out, player.crouching);
        writeBool(out, player.pSpeedActive);
        writeF32(out, player.pSpeedMeter);
        out.push_back(player.style);
        out.push_back(player.powerupTransitionFrames);
        out.push_back(player.invincibilityFrames);
    }
    writeU32(out, static_cast<std::uint32_t>(update.actors.size()));
    for (const auto& a : update.actors) {
        writeF32(out, a.positionX);
        writeF32(out, a.positionY);
        writeF32(out, a.velocityX);
        writeF32(out, a.velocityY);
        writeBool(out, a.alive);
        writeBool(out, a.facingRight);
        out.push_back(a.script);
        out.push_back(a.enemyKind);
        out.push_back(a.category);
    }
    // Game phase tail (added later — readers tolerate older payloads).
    out.push_back(static_cast<std::uint8_t>(update.phase));
    writeU32(out, update.countdownTicks);
    writeU16(out, update.winnerSlot);
    writeString(out, update.selectedMap);
    writeBool(out, update.selectedTiebreak);
    return out;
}

StateUpdateData decodeStateUpdatePayload(const std::vector<std::uint8_t>& payload)
{
    PayloadReader reader(payload);
    StateUpdateData update;
    update.serverTick = reader.readU32();
    const auto playerCount = reader.readU32();
    update.players.reserve(static_cast<std::size_t>(playerCount));
    for (std::uint32_t i = 0; i < playerCount; ++i) {
        PlayerNetState player;
        player.slotIndex = reader.readU16();
        player.positionX = reader.readF32();
        player.positionY = reader.readF32();
        player.velocityX = reader.readF32();
        player.velocityY = reader.readF32();
        player.onGround = reader.readBool();
        player.facingRight = reader.readBool();
        player.skidding = reader.readBool();
        player.crouching = reader.readBool();
        player.pSpeedActive = reader.readBool();
        player.pSpeedMeter = reader.readF32();
        if (!reader.done()) {
            player.style = reader.readU8();
        }
        if (!reader.done()) {
            player.powerupTransitionFrames = reader.readU8();
        }
        if (!reader.done()) {
            player.invincibilityFrames = reader.readU8();
        }
        update.players.push_back(player);
    }

    if (!reader.done()) {
        const auto actorCount = reader.readU32();
        update.actors.reserve(static_cast<std::size_t>(actorCount));
        for (std::uint32_t i = 0; i < actorCount; ++i) {
            ActorNetState a;
            a.positionX = reader.readF32();
            a.positionY = reader.readF32();
            a.velocityX = reader.readF32();
            a.velocityY = reader.readF32();
            a.alive = reader.readBool();
            a.facingRight = reader.readBool();
            a.script = reader.readU8();
            if (!reader.done()) {
                a.enemyKind = reader.readU8();
            }
            if (!reader.done()) {
                a.category = reader.readU8();
            }
            update.actors.push_back(a);
        }
    }

    // Game phase tail. Optional for backward compat with older payloads
    // that didn't include it.
    if (!reader.done()) {
        update.phase = static_cast<GamePhase>(reader.readU8());
    }
    if (!reader.done()) {
        update.countdownTicks = reader.readU32();
    }
    if (!reader.done()) {
        update.winnerSlot = reader.readU16();
    }
    if (!reader.done()) {
        update.selectedMap = reader.readString();
    }
    if (!reader.done()) {
        update.selectedTiebreak = reader.readBool();
    }

    if (!reader.done()) {
        throw std::runtime_error("State update payload has trailing bytes");
    }

    return update;
}

std::vector<std::uint8_t> encodeRosterUpdatePayload(const std::vector<PlayerInfo>& roster)
{
    std::vector<std::uint8_t> out;
    writeU32(out, static_cast<std::uint32_t>(roster.size()));
    for (const auto& info : roster) {
        writeU16(out, info.playerIndex);
        writeBool(out, info.connected);
        out.push_back(info.colorR);
        out.push_back(info.colorG);
        out.push_back(info.colorB);
        writeString(out, info.displayName);
    }
    return out;
}

std::vector<PlayerInfo> decodeRosterUpdatePayload(const std::vector<std::uint8_t>& payload)
{
    PayloadReader reader(payload);
    const std::uint32_t count = reader.readU32();
    std::vector<PlayerInfo> roster;
    roster.reserve(static_cast<std::size_t>(count));
    for (std::uint32_t i = 0; i < count; ++i) {
        PlayerInfo info;
        info.playerIndex = reader.readU16();
        info.connected = reader.readBool();
        info.colorR = reader.readU8();
        info.colorG = reader.readU8();
        info.colorB = reader.readU8();
        info.displayName = reader.readString();
        roster.push_back(info);
    }
    if (!reader.done()) {
        throw std::runtime_error("Roster update payload has trailing bytes");
    }
    return roster;
}

namespace {

void writeLevelData(std::vector<std::uint8_t>& out, const opm::engine::LevelData& level)
{
    writeU32(out, level.foliage.width);
    writeU32(out, level.foliage.height);
    writeF32(out, level.spawnX);
    writeF32(out, level.spawnY);
    writeF32(out, level.goalX);
    writeF32(out, level.goalY);
    writeTileLayer(out, level.background);
    writeTileLayer(out, level.foliage);
    writeTileLayer(out, level.foreground);
}

opm::engine::LevelData readLevelData(PayloadReader& reader)
{
    opm::engine::LevelData level;
    const auto width = reader.readU32();
    const auto height = reader.readU32();
    level.spawnX = reader.readF32();
    level.spawnY = reader.readF32();
    level.goalX = reader.readF32();
    level.goalY = reader.readF32();
    readTileLayer(reader, level.background, width, height);
    readTileLayer(reader, level.foliage,    width, height);
    readTileLayer(reader, level.foreground, width, height);
    return level;
}

} // namespace

std::vector<std::uint8_t> encodeLevelListResponsePayload(const std::vector<std::string>& names)
{
    std::vector<std::uint8_t> out;
    writeU32(out, static_cast<std::uint32_t>(names.size()));
    for (const auto& name : names) {
        writeString(out, name);
    }
    return out;
}

std::vector<std::string> decodeLevelListResponsePayload(const std::vector<std::uint8_t>& payload)
{
    PayloadReader reader(payload);
    const std::uint32_t count = reader.readU32();
    std::vector<std::string> names;
    names.reserve(static_cast<std::size_t>(count));
    for (std::uint32_t i = 0; i < count; ++i) {
        names.push_back(reader.readString());
    }
    if (!reader.done()) {
        throw std::runtime_error("Level list response payload has trailing bytes");
    }
    return names;
}

std::vector<std::uint8_t> encodeLevelLoadRequestPayload(const std::string& name)
{
    std::vector<std::uint8_t> out;
    writeString(out, name);
    return out;
}

std::string decodeLevelLoadRequestPayload(const std::vector<std::uint8_t>& payload)
{
    PayloadReader reader(payload);
    const std::string name = reader.readString();
    if (!reader.done()) {
        throw std::runtime_error("Level load request payload has trailing bytes");
    }
    return name;
}

std::vector<std::uint8_t> encodeLevelLoadResponsePayload(const LevelLoadResponseData& data)
{
    std::vector<std::uint8_t> out;
    writeBool(out, data.ok);
    writeString(out, data.reason);
    if (data.ok) {
        writeLevelData(out, data.level);
    }
    return out;
}

LevelLoadResponseData decodeLevelLoadResponsePayload(const std::vector<std::uint8_t>& payload)
{
    PayloadReader reader(payload);
    LevelLoadResponseData data;
    data.ok = reader.readBool();
    data.reason = reader.readString();
    if (data.ok) {
        data.level = readLevelData(reader);
    }
    if (!reader.done()) {
        throw std::runtime_error("Level load response payload has trailing bytes");
    }
    return data;
}

std::vector<std::uint8_t> encodeLevelSaveRequestPayload(const LevelSaveRequestData& data)
{
    std::vector<std::uint8_t> out;
    writeString(out, data.name);
    writeLevelData(out, data.level);
    return out;
}

LevelSaveRequestData decodeLevelSaveRequestPayload(const std::vector<std::uint8_t>& payload)
{
    PayloadReader reader(payload);
    LevelSaveRequestData data;
    data.name = reader.readString();
    data.level = readLevelData(reader);
    if (!reader.done()) {
        throw std::runtime_error("Level save request payload has trailing bytes");
    }
    return data;
}

std::vector<std::uint8_t> encodeLevelSaveResponsePayload(const LevelSaveResponseData& data)
{
    std::vector<std::uint8_t> out;
    writeBool(out, data.ok);
    writeString(out, data.reason);
    return out;
}

LevelSaveResponseData decodeLevelSaveResponsePayload(const std::vector<std::uint8_t>& payload)
{
    PayloadReader reader(payload);
    LevelSaveResponseData data;
    data.ok = reader.readBool();
    data.reason = reader.readString();
    if (!reader.done()) {
        throw std::runtime_error("Level save response payload has trailing bytes");
    }
    return data;
}

std::vector<std::uint8_t> encodeLobbySetLevelRequestPayload(const std::string& levelName)
{
    std::vector<std::uint8_t> out;
    writeString(out, levelName);
    return out;
}

std::string decodeLobbySetLevelRequestPayload(const std::vector<std::uint8_t>& payload)
{
    PayloadReader reader(payload);
    const std::string name = reader.readString();
    if (!reader.done()) {
        throw std::runtime_error("Lobby set level request payload has trailing bytes");
    }
    return name;
}

std::vector<std::uint8_t> encodeLobbySetLevelResponsePayload(const LobbySetLevelResponseData& data)
{
    std::vector<std::uint8_t> out;
    writeBool(out, data.ok);
    writeString(out, data.reason);
    return out;
}

LobbySetLevelResponseData decodeLobbySetLevelResponsePayload(const std::vector<std::uint8_t>& payload)
{
    PayloadReader reader(payload);
    LobbySetLevelResponseData data;
    data.ok = reader.readBool();
    data.reason = reader.readString();
    if (!reader.done()) {
        throw std::runtime_error("Lobby set level response payload has trailing bytes");
    }
    return data;
}

std::vector<std::uint8_t> encodeMapVoteRequestPayload(const std::string& levelName)
{
    std::vector<std::uint8_t> out;
    writeString(out, levelName);
    return out;
}

std::string decodeMapVoteRequestPayload(const std::vector<std::uint8_t>& payload)
{
    PayloadReader reader(payload);
    const std::string name = reader.readString();
    if (!reader.done()) {
        throw std::runtime_error("Map vote request payload has trailing bytes");
    }
    return name;
}

std::vector<std::uint8_t> encodeMapVoteUpdatePayload(const std::vector<MapVote>& votes)
{
    std::vector<std::uint8_t> out;
    writeU32(out, static_cast<std::uint32_t>(votes.size()));
    for (const auto& v : votes) {
        writeU16(out, v.slotIndex);
        writeString(out, v.levelName);
    }
    return out;
}

std::vector<MapVote> decodeMapVoteUpdatePayload(const std::vector<std::uint8_t>& payload)
{
    PayloadReader reader(payload);
    const auto count = reader.readU32();
    std::vector<MapVote> votes;
    votes.reserve(static_cast<std::size_t>(count));
    for (std::uint32_t i = 0; i < count; ++i) {
        MapVote v;
        v.slotIndex = reader.readU16();
        v.levelName = reader.readString();
        votes.push_back(std::move(v));
    }
    if (!reader.done()) {
        throw std::runtime_error("Map vote update payload has trailing bytes");
    }
    return votes;
}

// ---- Authentication ----

std::vector<std::uint8_t> encodeLoginRequestPayload(const LoginRequestData& data)
{
    std::vector<std::uint8_t> out;
    writeString(out, data.username);
    writeString(out, data.password);
    return out;
}

LoginRequestData decodeLoginRequestPayload(const std::vector<std::uint8_t>& payload)
{
    PayloadReader reader(payload);
    LoginRequestData data;
    data.username = reader.readString();
    data.password = reader.readString();
    if (!reader.done()) {
        throw std::runtime_error("Login request payload has trailing bytes");
    }
    return data;
}

std::vector<std::uint8_t> encodeLoginResponsePayload(const LoginResponseData& data)
{
    std::vector<std::uint8_t> out;
    writeBool(out, data.ok);
    writeString(out, data.reason);
    writeString(out, data.token);
    writeString(out, data.displayName);
    return out;
}

LoginResponseData decodeLoginResponsePayload(const std::vector<std::uint8_t>& payload)
{
    PayloadReader reader(payload);
    LoginResponseData data;
    data.ok = reader.readBool();
    data.reason = reader.readString();
    data.token = reader.readString();
    data.displayName = reader.readString();
    if (!reader.done()) {
        throw std::runtime_error("Login response payload has trailing bytes");
    }
    return data;
}

std::vector<std::uint8_t> encodeUpdateProfileRequestPayload(const UpdateProfileRequestData& data)
{
    std::vector<std::uint8_t> out;
    writeString(out, data.displayName);
    return out;
}

UpdateProfileRequestData decodeUpdateProfileRequestPayload(const std::vector<std::uint8_t>& payload)
{
    PayloadReader reader(payload);
    UpdateProfileRequestData data;
    data.displayName = reader.readString();
    if (!reader.done()) {
        throw std::runtime_error("Update profile request payload has trailing bytes");
    }
    return data;
}

std::vector<std::uint8_t> encodeUpdateProfileResponsePayload(const UpdateProfileResponseData& data)
{
    std::vector<std::uint8_t> out;
    writeBool(out, data.ok);
    writeString(out, data.reason);
    return out;
}

UpdateProfileResponseData decodeUpdateProfileResponsePayload(const std::vector<std::uint8_t>& payload)
{
    PayloadReader reader(payload);
    UpdateProfileResponseData data;
    data.ok = reader.readBool();
    data.reason = reader.readString();
    if (!reader.done()) {
        throw std::runtime_error("Update profile response payload has trailing bytes");
    }
    return data;
}

std::vector<std::uint8_t> encodeCreateLobbyRequestPayload(const CreateLobbyRequestData& data)
{
    std::vector<std::uint8_t> out;
    writeString(out, data.lobbyName);
    return out;
}

CreateLobbyRequestData decodeCreateLobbyRequestPayload(const std::vector<std::uint8_t>& payload)
{
    PayloadReader reader(payload);
    CreateLobbyRequestData data;
    data.lobbyName = reader.readString();
    if (!reader.done()) {
        throw std::runtime_error("Create lobby request payload has trailing bytes");
    }
    return data;
}

std::vector<std::uint8_t> encodeCreateLobbyResponsePayload(const CreateLobbyResponseData& data)
{
    std::vector<std::uint8_t> out;
    writeBool(out, data.ok);
    writeString(out, data.reason);
    return out;
}

CreateLobbyResponseData decodeCreateLobbyResponsePayload(const std::vector<std::uint8_t>& payload)
{
    PayloadReader reader(payload);
    CreateLobbyResponseData data;
    data.ok = reader.readBool();
    data.reason = reader.readString();
    if (!reader.done()) {
        throw std::runtime_error("Create lobby response payload has trailing bytes");
    }
    return data;
}

} // namespace opm::protocol
