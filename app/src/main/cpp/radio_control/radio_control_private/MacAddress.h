#pragma once

#include "ByteArray.h"

class MacAddress
{
public:
    enum { NumBytes = 6 };

    explicit MacAddress();
    explicit MacAddress(const std::string &str);
    explicit MacAddress(const ByteArray &data);

    void clear();
    bool isValid() const;

    ByteArray data() const;

    void fromString(const std::string &str);
    std::string toString() const;

    void fromByteArray(const ByteArray &data);

    bool operator==(const MacAddress& other) const;
    bool operator!=(const MacAddress& other) const;

private:
    ByteArray m_address;
};
