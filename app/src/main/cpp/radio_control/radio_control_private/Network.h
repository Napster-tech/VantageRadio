#pragma once

#include "MacAddress.h"
#include "HostAddress.h"

class Network
{
public:
    Network();
    virtual ~Network();

    static std::vector<MacAddress> getMacAddresses();
    static std::vector<HostAddress> getNetworkAddresses();
};
