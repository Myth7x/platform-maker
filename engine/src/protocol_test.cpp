#include "opm/protocol.hpp"

#include <cassert>
#include <stdexcept>
#include <vector>

int main()
{
    const opm::protocol::Message outbound {
        .type = opm::protocol::MessageType::LobbyListRequest,
        .payload = opm::protocol::payloadFromString("request_lobbies")
    };

    const auto encoded = opm::protocol::encodeMessage(outbound);
    const auto decoded = opm::protocol::decodeMessage(encoded);

    assert(decoded.type == outbound.type);
    assert(decoded.payload == outbound.payload);

    bool threwOnShortPacket = false;
    try {
        (void)opm::protocol::decodeMessage(std::vector<std::uint8_t> {0x01U, 0x02U, 0x03U});
    } catch (const std::runtime_error&) {
        threwOnShortPacket = true;
    }
    assert(threwOnShortPacket);

    auto malformedMagic = encoded;
    malformedMagic[0] = 0x00U;

    bool threwOnBadMagic = false;
    try {
        (void)opm::protocol::decodeMessage(malformedMagic);
    } catch (const std::runtime_error&) {
        threwOnBadMagic = true;
    }
    assert(threwOnBadMagic);

    auto malformedSize = encoded;
    malformedSize[5] = 0xFFU;
    malformedSize[6] = 0x00U;
    malformedSize[7] = 0x00U;
    malformedSize[8] = 0x00U;

    bool threwOnBadSize = false;
    try {
        (void)opm::protocol::decodeMessage(malformedSize);
    } catch (const std::runtime_error&) {
        threwOnBadSize = true;
    }
    assert(threwOnBadSize);

    const std::vector<opm::protocol::LobbyEntry> lobbies {
        {.name = "default_lobby", .players = 1U, .capacity = 2U},
        {.name = "race_lobby", .players = 0U, .capacity = 2U},
    };
    const auto lobbyPayload = opm::protocol::encodeLobbyListPayload(lobbies);
    const auto decodedLobbies = opm::protocol::decodeLobbyListPayload(lobbyPayload);
    assert(decodedLobbies.size() == lobbies.size());
    assert(decodedLobbies[0].name == "default_lobby");

    const opm::engine::InputFrame input {
        .frameIndex = 1337U,
        .moveLeft = true,
        .moveRight = false,
        .jumpPressed = true,
        .jumpHeld = true,
        .runHeld = true,
        .crouchHeld = false,
    };
    const auto movementPayload = opm::protocol::encodeMovementInputPayload(input);
    const auto decodedInput = opm::protocol::decodeMovementInputPayload(movementPayload);
    assert(decodedInput.frameIndex == input.frameIndex);
    assert(decodedInput.moveLeft == input.moveLeft);
    assert(decodedInput.jumpHeld == input.jumpHeld);

    opm::protocol::StateUpdateData update;
    update.serverTick = 42U;
    update.players.push_back({});
    update.players[0].positionX = 10.5F;
    update.players[0].pSpeedActive = true;
    const auto updatePayload = opm::protocol::encodeStateUpdatePayload(update);
    const auto decodedUpdate = opm::protocol::decodeStateUpdatePayload(updatePayload);
    assert(decodedUpdate.serverTick == update.serverTick);
    assert(decodedUpdate.players[0].positionX == update.players[0].positionX);
    assert(decodedUpdate.players[0].pSpeedActive == update.players[0].pSpeedActive);

    return 0;
}
