#pragma once

#include "game/level_editor.hpp"

#include "opm/engine.hpp"
#include "opm/level.hpp"
#include "opm/protocol.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace opm::client::game {

enum class AppState : std::uint8_t {
    MainMenu,
    LobbyBrowser,
    LevelPicker,
    OnlineLevelSelect,
    LevelCreator,
    Playing,
};

// Mirrors the server's PeerSession: the per-app state machine plus the
// transient UI fields each screen needs. ClientApp owns one of these
// for the lifetime of the run; screens read and mutate it through
// ScreenContext.
struct GameSession {
    AppState state {AppState::MainMenu};
    bool isOnline {false};
    opm::engine::Simulation simulation;
    opm::engine::LevelData activeLevel;
    LayeredEntries entries;
    char addressInput[64] {"127.0.0.1:34900"};
    std::string menuStatus;

    // LobbyBrowser state.
    struct LobbyEntry {
        std::string name {};
        std::uint32_t players {0};
        std::uint32_t capacity {0};
    };
    std::vector<LobbyEntry> availableLobbies {};
    int selectedLobbyIndex {-1};
    std::string lobbyBrowserStatus {};

    // LevelPicker state.
    std::vector<std::string> serverLevels {};
    std::string pickerStatus {};
    int selectedLevelIndex {-1};
    enum class PickerIntent : std::uint8_t {
        EditOnServer = 0,
    };
    PickerIntent pickerIntent {PickerIntent::EditOnServer};

    // OnlineLevelSelect state (after a multiplayer lobby join, before play).
    std::vector<std::string> onlineLevels {};
    int onlineLevelSelected {-1};
    std::string onlineLevelStatus {};
    // Latest map-vote tally from the server. One entry per ballot (one
    // per voting player). Updated by the lobby screen's onPollServer.
    std::vector<opm::protocol::MapVote> mapVoteTally {};

    // Mirrored game-phase fields from the latest StateUpdate. The lobby
    // and gameplay HUDs read these to render the countdown / winner
    // overlay. Always updated whenever a StateUpdate arrives.
    opm::protocol::GamePhase gamePhase {opm::protocol::GamePhase::PreGame};
    std::uint32_t countdownTicks {0};
    std::uint16_t winnerSlot {0xFFFFU};
    std::string selectedMap {};
    bool selectedTiebreak {false};

    // LevelCreator state.
    LevelEditor editor {};

    // Set to true when Playing state was entered via "Test Play" from the
    // editor. Allows the "Back to Editor" button in the Playing HUD.
    bool fromEditor {false};
};

} // namespace opm::client::game
