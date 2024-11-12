#include "OperatingSystem.h"
#include <cstdlib>
#include "RandomNumber.h"

/////////////////////////////////////////////////////////////////////////////

static inline uint32_t rot32(uint32_t x, int k)
{
    return (x << k) | (x >> (32 - k));
}

/////////////////////////////////////////////////////////////////////////////

static inline uint32_t mix(uint32_t a, uint32_t b, uint32_t c)
{
    // printf("0x%08X 0x%08X 0x%08X", a, b, c);

    a -= c; a ^= rot32(c, 4); c += b;
    b -= a; b ^= rot32(a, 6); a += c;
    c -= b; c ^= rot32(b, 8); b += a;
    a -= c; a ^= rot32(c, 16); c += b;
    b -= a; b ^= rot32(a, 19); a += c;
    c -= b; c ^= rot32(b, 4); b += a;

    // printf(" -> 0x%08X\n", c);
    return c;
}

/////////////////////////////////////////////////////////////////////////////

void RandomNumber::init()
{
    uint64_t now = OperatingSystem::getCurrentTime();

    uint32_t seed = mix(uint32_t(now >> 32), uint32_t(now),
#ifdef OS_WINDOWS
        GetCurrentProcessId()
#else
        getpid()
#endif
    );

    srand(seed);
}

/////////////////////////////////////////////////////////////////////////////

uint8_t RandomNumber::randU8()
{
    return uint8_t(rand());
}

/////////////////////////////////////////////////////////////////////////////

uint16_t RandomNumber::randU16()
{
    return (uint16_t(randU8()) << 8) | randU8();
}

/////////////////////////////////////////////////////////////////////////////

uint32_t RandomNumber::randU32()
{
    return (uint32_t(randU16()) << 16) | randU16();
}

/////////////////////////////////////////////////////////////////////////////
