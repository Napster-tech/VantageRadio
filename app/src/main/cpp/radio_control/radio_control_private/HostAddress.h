#pragma once

#include <string>
#include <cstdint>

class HostAddress
{
public:
    HostAddress(uint32_t address = 0) : m_address(address) {};
    HostAddress(const std::string& str);

    bool fromString(const std::string & str);
    std::string toString() const;

    uint32_t data() const { return m_address;  }

    inline bool operator==(const HostAddress &other) { return data() == other.data(); }

private:
    uint32_t m_address = 0;
};
