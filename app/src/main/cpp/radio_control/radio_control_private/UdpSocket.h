#pragma once

#include "OperatingSystem.h"
#include "HostAddress.h"
#include "ByteArray.h"

class UdpSocket
{
public:
    UdpSocket();
    virtual ~UdpSocket();

    bool isValid() const;

    bool enableReuseAddr();
    bool enableBroadcast();
    bool enableNonBlocking();

    bool bind(const HostAddress& address, uint16_t port);

    bool sendTo(const ByteArray& packet, const HostAddress& address, uint16_t port);

    ByteArray read(int maxSize);

    SOCKET socket() const { return m_socket; }

private:
    SOCKET m_socket;
};
