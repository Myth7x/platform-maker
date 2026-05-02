#pragma once

#include "game/game_session.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace opm::assets { struct AssetManifest; }
namespace opm::engine { struct LevelData; struct TileLayer; }
namespace opm::client::render { struct PaletteEntry; struct TileDrawEntry; }

namespace opm::client {

// Optional arguments parsed from the command line.
struct ClientArgs {
    // --test-mode: automatically connect and play on the specified level.
    bool testMode {false};
    std::string testAddress {"127.0.0.1:34900"};
    std::string testLevel {"test_room"}; // level name on the server
};

// Top-level orchestrator. Mirrors the server's `Server` class: owns the
// long-lived session state, drives the main loop, and dispatches per-screen
// work via ScreenStack. runWindow is the platform-specific entry that
// owns GLFW/ImGui setup, the per-frame loop, and the per-state render /
// input branches that haven't yet migrated into Screen subclasses.
class ClientApp {
public:
    int run();
    int run(const ClientArgs& args);

private:
    int runWindow(const opm::assets::AssetManifest& manifest,
                  const opm::engine::LevelData& fallbackLevel,
                  const ClientArgs& clientArgs);

    // ---- State-transition / level-setup helpers ----
    // Were lambdas inside runWindow; promoted to methods so screen
    // callbacks can call them by name and so the orchestrator owns its
    // mutations to session_.
    void rebuildEntriesFromLevel(opm::client::game::LayeredEntries& out,
                                 const opm::engine::LevelData& level);
    void enterPlaying(bool online, const opm::engine::LevelData& levelData);
    [[nodiscard]] std::string enterLevelPicker(
        const std::string& host, std::uint16_t port,
        opm::client::game::GameSession::PickerIntent intent);
    void enterLevelCreator(const std::vector<opm::client::render::PaletteEntry>& palette);
    void editLoadedLevel(opm::engine::LevelData loaded, const std::string& name,
                         const std::vector<opm::client::render::PaletteEntry>& palette);

    // ---- Editor-only helpers ----
    void rebuildEditorEntries();
    opm::engine::TileLayer& layerByEnum(opm::client::game::EditorLayer which);
    std::vector<opm::client::render::TileDrawEntry>& layerEntriesByEnum(
        opm::client::game::EditorLayer which);
    void rebuildActiveLayerEntries();

    // ---- Long-lived state ----
    opm::client::game::GameSession session_ {};
    const opm::assets::AssetManifest* manifest_ {nullptr};
};

} // namespace opm::client
