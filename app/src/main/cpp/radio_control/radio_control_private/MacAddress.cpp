#include <cassert>
#include "MacAddress.h"
#include "StrUtils.h"

/////////////////////////////////////////////////////////////////////////////

MacAddress::MacAddress()
{
    clear();
}

/////////////////////////////////////////////////////////////////////////////

MacAddress::MacAddress(const std::string &str)
{
    fromString(str);
}

/////////////////////////////////////////////////////////////////////////////

MacAddress::MacAddress(const ByteArray &data)
{
    fromByteArray(data);
}

/////////////////////////////////////////////////////////////////////////////

void MacAddress::clear()
{
    m_address = ByteArray(NumBytes, 0);
}

/////////////////////////////////////////////////////////////////////////////

bool MacAddress::isValid() const
{
    assert(m_address.size() == NumBytes);

    if (m_address.size() != NumBytes)
        return false;

    // Invalid if all zeros, so look for the first non-zero byte
    const auto* d = m_address.data();
    for (size_t n = 0; n < m_address.size(); n++) {
        if (*d++)
            return true;
    }

    return false;
}

/////////////////////////////////////////////////////////////////////////////

ByteArray MacAddress::data() const
{
    return m_address;
}

/////////////////////////////////////////////////////////////////////////////

void MacAddress::fromString(const std::string &str)
{
    fromByteArray(ByteArray::fromHexString(str));
}

/////////////////////////////////////////////////////////////////////////////

std::string MacAddress::toString() const
{
    assert(m_address.size() == NumBytes);

    std::string str = m_address.toHexString(':');

    return upper(str);
}

/////////////////////////////////////////////////////////////////////////////

void MacAddress::fromByteArray(const ByteArray &data)
{
    if (data.size() == NumBytes)
        m_address = data;
    else
        clear();
}

/////////////////////////////////////////////////////////////////////////////

bool MacAddress::operator==(const MacAddress &other) const
{
    return (m_address == other.m_address);
}

/////////////////////////////////////////////////////////////////////////////

bool MacAddress::operator!=(const MacAddress &other) const
{
    return !(*this == other);
}

/////////////////////////////////////////////////////////////////////////////
