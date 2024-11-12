#pragma once

#include "ByteArray.h"

class Crc32
{
public:
    Crc32();

    void clear();
    uint32_t getCrc();

    void append(const ByteArray& a);
    void append(const uint8_t* buf, size_t length);

    static uint32_t crc32(const ByteArray& a);

private:
    uint32_t m_crc;
};
