#pragma once

#include "game/game_session.hpp"
#include "screens/screen.hpp"

#include <functional>
#include <string>
#include <vector>

namespace opm::client {

// The level editor: paints tiles and actors onto a LevelData, supports
// camera pan/zoom (handled outside this class for now), save/load via
// the server, and spawning a Test Play session. Owns no in-progress
// state — reads / mutates session.editor.
//
// The four editor-internal lambdas (rebuildEditorEntries,
// layerByEnum, layerEntriesByEnum, rebuildActiveLayerEntries) still
// live in runWindow because the canvas-painting input branch hasn't
// migrated yet. The renderUI() body uses callbacks for the network /
// resize operations that need them.
class LevelCreatorScreen final : public Screen {
public:
    struct Callbacks {
        // Save the editor's current level to the server. Implementation
        // handles connection bring-up via the address in
        // GameSession.addressInput. Sets session.editor.statusMessage
        // and session.editor.dirty as appropriate.
        std::function<void()> onSaveLevel;

        // Switch to Playing on the editor's level (Test Play). Caller
        // sets session.fromEditor = true so the HUD's "Back to Editor"
        // button is visible.
        std::function<void()> onTestPlay;

        // Resize the editor's level (incl. rebuild of draw entries +
        // dirty marker + status message). Width/height clamped by the
        // implementation.
        std::function<void(int width, int height)> onResize;

        // Rebuild the active editor layer's draw entries from its
        // current tile indices. Called after every paint/erase/rotate
        // that mutates a tile-layer cell so the next render picks up
        // the change.
        std::function<void()> onRebuildActiveLayer;
    };

    LevelCreatorScreen(opm::client::game::GameSession& session, Callbacks callbacks);

    [[nodiscard]] ScreenId id() const noexcept override { return ScreenId::LevelCreator; }
    ScreenTransition tick(ScreenContext& ctx, double deltaSeconds) override;
    void render(ScreenContext& ctx) override;
    void renderUI(ScreenContext& ctx) override;

private:
    // Local helpers (former runWindow lambdas) — only operate on
    // session.editor, no orchestrator state required.
    opm::engine::TileLayer& layerByEnum(opm::client::game::EditorLayer which);
    std::vector<opm::client::render::TileDrawEntry>& layerEntriesByEnum(
        opm::client::game::EditorLayer which);

    opm::client::game::GameSession* session_;
    Callbacks callbacks_;
};

} // namespace opm::client
