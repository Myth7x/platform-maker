#pragma once

#include "screens/screen.hpp"

#include <memory>
#include <unordered_map>

namespace opm::client {

// Tiny dispatcher: holds one instance per ScreenId and routes per-frame
// calls to whichever screen is currently active. Transitions returned by
// Screen::tick() are applied after the tick (so the rest of the frame
// runs against a stable active screen).
//
// Not literally a stack — there's no back-stack, since the app's UX is
// flat (every screen returns to MainMenu rather than nesting). Kept the
// name to signal intent and leave room to add real back-stack semantics
// later if needed.
class ScreenStack {
public:
    void registerScreen(ScreenId id, std::unique_ptr<Screen> screen)
    {
        screens_[id] = std::move(screen);
    }

    void setActive(ScreenId id, ScreenContext& ctx)
    {
        if (active_ == id) {
            return;
        }
        if (Screen* prev = current()) {
            prev->onExit(ctx);
        }
        active_ = id;
        if (Screen* next = current()) {
            next->onEnter(ctx);
        }
    }

    [[nodiscard]] ScreenId activeId() const noexcept { return active_; }

    [[nodiscard]] Screen* current()
    {
        const auto it = screens_.find(active_);
        return it == screens_.end() ? nullptr : it->second.get();
    }

    void applyTransition(const ScreenTransition& t, ScreenContext& ctx)
    {
        if (t.next.has_value()) {
            setActive(*t.next, ctx);
        }
    }

private:
    std::unordered_map<ScreenId, std::unique_ptr<Screen>> screens_;
    ScreenId active_ {ScreenId::MainMenu};
};

} // namespace opm::client
