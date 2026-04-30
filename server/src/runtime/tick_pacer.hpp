#pragma once
#include <chrono>

namespace opm::server {

// On Windows 10 1803+, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION gives 100 ns timer
// granularity without bumping system timer resolution and without spinning a
// core. The deadline is held in monotonic steady_clock and the *remaining*
// duration is translated into a relative SetWaitableTimer call so the kernel
// does the wait. Falls back to std::this_thread::sleep_until on POSIX or
// older Windows builds.
class TickPacer {
public:
    explicit TickPacer(std::chrono::microseconds interval) noexcept;
    ~TickPacer();
    TickPacer(const TickPacer&) = delete;
    TickPacer& operator=(const TickPacer&) = delete;

    void waitForNext();

private:
    std::chrono::microseconds interval_;
    std::chrono::steady_clock::time_point next_;
#ifdef _WIN32
    void* timer_ {nullptr}; // Opaque HANDLE; binary-compatible with Windows HANDLE.
#endif
};

} // namespace opm::server
