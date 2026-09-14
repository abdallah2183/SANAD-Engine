// NF/Core/Time.cpp

#include <NF/Core/Time.hpp>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
#else
    #include <thread>
#endif

namespace nf {

f64 Time::now() {
    return Clock::seconds_since_epoch();
}

u64 Time::now_ticks() {
    return Clock::tick_count();
}

void Time::sleep_ms(u32 ms) {
#ifdef _WIN32
    ::Sleep(ms);
#else
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
#endif
}

} // namespace nf
