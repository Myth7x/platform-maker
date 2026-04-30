#include "runtime/scoped_timer_resolution.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#endif

namespace opm::server {

ScopedTimerResolution::ScopedTimerResolution() noexcept
{
#ifdef _WIN32
    active_ = (::timeBeginPeriod(1U) == TIMERR_NOERROR);
#endif
}

ScopedTimerResolution::~ScopedTimerResolution()
{
#ifdef _WIN32
    if (active_) {
        ::timeEndPeriod(1U);
    }
#endif
}

} // namespace opm::server
