#pragma once

#include <cstdint>

class RandomNumber
{
public:
    static void init();

    static uint8_t randU8();
    static uint16_t randU16();
    static uint32_t randU32();
};
