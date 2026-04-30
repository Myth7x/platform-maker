#pragma once

namespace opm::server {

// Bumps the Windows scheduler timer to 1 ms while the object is alive. This is
// a defensive fallback for systems without high-resolution waitable timers;
// TickPacer prefers the HRT path. No-op on POSIX. Implementation hides the
// timeapi.h header to keep windows.h out of the public surface.
class ScopedTimerResolution {
public:
    ScopedTimerResolution() noexcept;
    ~ScopedTimerResolution();
    ScopedTimerResolution(const ScopedTimerResolution&) = delete;
    ScopedTimerResolution& operator=(const ScopedTimerResolution&) = delete;

private:
    bool active_ {false};
};

} // namespace opm::server
