#pragma once

#include "screens/screen.hpp"

namespace opm::client {

// Browses levels stored on the server. Lets the user pick one to play
// or to load into the editor.
//
// Stub: see comment in main_menu_screen.cpp for the carve-out plan.
class LevelPickerScreen final : public Screen {
public:
    [[nodiscard]] ScreenId id() const noexcept override { return ScreenId::LevelPicker; }
    ScreenTransition tick(ScreenContext& ctx, double deltaSeconds) override;
    void renderUI(ScreenContext& ctx) override;
};

} // namespace opm::client
