#pragma once

#include <map>
#include <memory>

#include "UdpSocket.h"
#include "HostAddress.h"

class UdpBroadcaster
{
public:
    UdpBroadcaster(uint16_t port);

    void setInterfaces(const std::vector<HostAddress> &interfaces);

    void broadcast(const ByteArray &packet);

private:
    void broadcast(const HostAddress &address, const ByteArray &packet);
    std::vector<HostAddress> m_interfaces;
    std::map<uint32_t, std::unique_ptr<UdpSocket>> m_sockets;
};
