#pragma once

#include "screens/screen.hpp"

namespace opm::client::game { struct LevelEditor; }

namespace opm::client {

// The level editor: paints tiles and actors onto a LevelData, supports
// camera pan/zoom, save/load via the server, and spawning a Test Play
// session. Owns a LevelEditor instance for the in-progress level.
class LevelCreatorScreen final : public Screen {
public:
    [[nodiscard]] ScreenId id() const noexcept override { return ScreenId::LevelCreator; }
    ScreenTransition tick(ScreenContext& ctx, double deltaSeconds) override;
    void render(ScreenContext& ctx) override;
    void renderUI(ScreenContext& ctx) override;
};

} // namespace opm::client
