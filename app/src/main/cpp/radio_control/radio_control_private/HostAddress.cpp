#include "OperatingSystem.h"
#include "HostAddress.h"
#include "Network.h"

/////////////////////////////////////////////////////////////////////////////

HostAddress::HostAddress(const std::string& str)
{
    fromString(str);
}

/////////////////////////////////////////////////////////////////////////////

bool HostAddress::fromString(const std::string& str)
{
    struct sockaddr_in sa;

    bool ok = (inet_pton(AF_INET, str.c_str(), &(sa.sin_addr)) == 1);

    if (ok)
        m_address = ntohl(sa.sin_addr.s_addr);
    else
        m_address = 0;

    return ok;
}

/////////////////////////////////////////////////////////////////////////////

std::string HostAddress::toString() const
{
    struct sockaddr_in sa;
    sa.sin_addr.s_addr = htonl(m_address);

    char buf[32];
    inet_ntop(AF_INET, &(sa.sin_addr), buf, sizeof(buf));

    return std::string(buf);
}

/////////////////////////////////////////////////////////////////////////////
