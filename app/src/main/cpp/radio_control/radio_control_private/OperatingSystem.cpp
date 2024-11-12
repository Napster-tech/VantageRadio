#include "OperatingSystem.h"

#ifdef OS_WINDOWS
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#endif

/////////////////////////////////////////////////////////////////////////////

// Get the current time in microseconds
//
uint64_t OperatingSystem::getCurrentTime()
{
    uint64_t u64;

#ifdef OS_WINDOWS
    static const uint64_t epoch = 116444736000000000ULL;

    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);

    // 100ns intervals
    u64 = (uint64_t(ft.dwHighDateTime) << 32) + uint64_t(ft.dwLowDateTime) - epoch;

    // Microseconds
    u64 /= 10;
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    u64 = (uint64_t(tv.tv_sec) * 1'000'000) + uint64_t(tv.tv_usec);
#endif

    return u64;
}

/////////////////////////////////////////////////////////////////////////////
