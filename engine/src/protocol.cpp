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
    out.push_back(payload.playerIndex);
    writeU32(out, payload.tickRateHz);
    writeString(out, payload.lobbyName);
    writeString(out, payload.reason);
    writeU32(out, static_cast<std::uint32_t>(payload.roster.size()));
    for (const auto& player : payload.roster) {
        out.push_back(player.playerIndex);
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
    response.playerIndex = reader.readU8();
    response.tickRateHz = reader.readU32();
    response.lobbyName = reader.readString();
    response.reason = reader.readString();
    const std::uint32_t rosterCount = reader.readU32();
    response.roster.reserve(static_cast<std::size_t>(rosterCount));
    for (std::uint32_t i = 0; i < rosterCount; ++i) {
        PlayerInfo info;
        info.playerIndex = reader.readU8();
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

std::vector<std::uint8_t> encodeLevelSnapshotPayload(const opm::engine::LevelData& level)
{
    std::vector<std::uint8_t> out;
    writeU32(out, level.groundLayer.width);
    writeU32(out, level.groundLayer.height);
    writeF32(out, level.spawnX);
    writeF32(out, level.spawnY);
    writeF32(out, level.goalX);
    writeF32(out, level.goalY);

    writeU32(out, static_cast<std::uint32_t>(level.groundLayer.tileIndices.size()));
    for (const auto tile : level.groundLayer.tileIndices) {
        writeU16(out, tile);
    }
    return out;
}

opm::engine::LevelData decodeLevelSnapshotPayload(const std::vector<std::uint8_t>& payload)
{
    PayloadReader reader(payload);
    opm::engine::LevelData level;
    level.groundLayer.width = reader.readU32();
    level.groundLayer.height = reader.readU32();
    level.spawnX = reader.readF32();
    level.spawnY = reader.readF32();
    level.goalX = reader.readF32();
    level.goalY = reader.readF32();

    const auto tileCount = static_cast<std::size_t>(reader.readU32());
    level.groundLayer.tileIndices.reserve(tileCount);
    for (std::size_t i = 0; i < tileCount; ++i) {
        level.groundLayer.tileIndices.push_back(reader.readU16());
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
    for (const auto& player : update.players) {
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
    }
    return out;
}

StateUpdateData decodeStateUpdatePayload(const std::vector<std::uint8_t>& payload)
{
    PayloadReader reader(payload);
    StateUpdateData update;
    update.serverTick = reader.readU32();
    for (auto& player : update.players) {
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
    }

    if (!reader.done()) {
        throw std::runtime_error("State update payload has trailing bytes");
    }

    return update;
}

} // namespace opm::protocol
