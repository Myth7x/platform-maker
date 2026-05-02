#pragma once

#include <cstdint>
#include <optional>

namespace opm::client {

class ClientApp;

namespace render { class RenderContext; class AssetRegistry; }
namespace net    { class SessionClient; }
namespace game   { class ActorManager; struct LevelEditor; }

enum class ScreenId : std::uint8_t {
    MainMenu,
    LevelPicker,
    OnlineLevelSelect,
    LevelCreator,
    Playing,
};

// Returned by Screen::tick() to request a transition. Empty `next` keeps
// the current screen active; otherwise the stack pops the current screen
// and pushes the requested one.
struct ScreenTransition {
    std::optional<ScreenId> next {};
};

// Bundle of services every screen needs. Lifetime owned by ClientApp;
// passed by reference into Screen virtuals every frame.
//
// `session` may be null when the app hasn't connected to a server yet
// (e.g. on MainMenu before the first connect attempt) — screens that
// need it must check.
struct ScreenContext {
    ClientApp*             app;     // null until ClientApp owns the runWindow body
    render::RenderContext& render;
    render::AssetRegistry& assets;
    net::SessionClient*    session;
};

// Base interface for one of the five top-level UI screens
// (MainMenu / LevelPicker / OnlineLevelSelect / LevelCreator / Playing).
// Mirrors the per-message handler family on the server side: each
// concrete screen owns its own state and decides when to hand control
// back via the ScreenTransition return value.
class Screen {
public:
    virtual ~Screen() = default;

    Screen(const Screen&) = delete;
    Screen& operator=(const Screen&) = delete;

    [[nodiscard]] virtual ScreenId id() const noexcept = 0;

    // Called once when this screen becomes active.
    virtual void onEnter(ScreenContext&) {}
    // Called once just before this screen is replaced.
    virtual void onExit(ScreenContext&) {}

    // Per-frame phases. tick() drives input, networking, simulation and
    // returns a transition request; render() / renderUI() do drawing
    // (split so ImGui can be sequenced after the world).
    virtual ScreenTransition tick(ScreenContext&, double deltaSeconds) = 0;
    virtual void render(ScreenContext&) {}
    virtual void renderUI(ScreenContext&) {}

protected:
    Screen() = default;
};

} // namespace opm::client
