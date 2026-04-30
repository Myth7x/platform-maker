#include "runtime/tick_pacer.hpp"

#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#endif

namespace opm::server {

TickPacer::TickPacer(std::chrono::microseconds interval) noexcept
    : interval_(interval)
    , next_(std::chrono::steady_clock::now() + interval)
{
#ifdef _WIN32
    timer_ = ::CreateWaitableTimerExW(nullptr, nullptr,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
        TIMER_ALL_ACCESS);
    // If HRT is unavailable on this kernel, timer_ stays null and we fall
    // through to std::this_thread::sleep_until in waitForNext().
#endif
}

TickPacer::~TickPacer()
{
#ifdef _WIN32
    if (timer_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(timer_));
    }
#endif
}

void TickPacer::waitForNext()
{
    const auto deadline = next_;
    const auto now = std::chrono::steady_clock::now();
    if (now < deadline) {
        const auto remaining = deadline - now;
#ifdef _WIN32
        if (timer_ != nullptr) {
            using HundredNs = std::chrono::duration<long long, std::ratio<1, 10'000'000>>;
            LARGE_INTEGER due;
            due.QuadPart = -std::chrono::duration_cast<HundredNs>(remaining).count();
            const auto h = static_cast<HANDLE>(timer_);
            if (due.QuadPart < 0 && ::SetWaitableTimer(h, &due, 0, nullptr, nullptr, FALSE)) {
                ::WaitForSingleObject(h, INFINITE);
            } else {
                std::this_thread::sleep_until(deadline);
            }
        } else {
            std::this_thread::sleep_until(deadline);
        }
#else
        std::this_thread::sleep_until(deadline);
#endif
    }
    next_ += interval_;
    const auto post = std::chrono::steady_clock::now();
    if (next_ < post) {
        next_ = post + interval_;
    }
}

} // namespace opm::server
