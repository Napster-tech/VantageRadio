#include "OperatingSystem.h"
#include "Network.h"
#include "StrUtils.h"
#include "Trace.h"

extern void die(const char* msg);

/////////////////////////////////////////////////////////////////////////////

Network::Network()
{
#ifdef OS_WINDOWS
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
        die("WSAStartup() failed");
#endif
}

/////////////////////////////////////////////////////////////////////////////

Network::~Network()
{
#ifdef OS_WINDOWS
    WSACleanup();
#endif
}

/////////////////////////////////////////////////////////////////////////////

#ifdef OS_WINDOWS

#define WORKING_BUFFER_SIZE 15000
#define MAX_TRIES 3

static PIP_ADAPTER_ADDRESSES getAdaptersAddresses()
{
    PIP_ADAPTER_ADDRESSES pAddresses = nullptr;
    ULONG outBufLen = WORKING_BUFFER_SIZE;
    ULONG iterations = 0;
    DWORD retVal = 0;

    do {
        pAddresses = (IP_ADAPTER_ADDRESSES*)malloc(outBufLen);
        if (pAddresses == nullptr)
            die("Memory allocation failed");

        ULONG flags = GAA_FLAG_INCLUDE_PREFIX |
            GAA_FLAG_SKIP_DNS_SERVER |
            GAA_FLAG_SKIP_MULTICAST;

        retVal = GetAdaptersAddresses(AF_INET, flags, NULL, pAddresses, &outBufLen);
        if (retVal == ERROR_BUFFER_OVERFLOW) {
            free(pAddresses);
            pAddresses = nullptr;
        }
        else {
            break;
        }

        iterations++;
    } while (retVal == ERROR_BUFFER_OVERFLOW && iterations < MAX_TRIES);

    switch (retVal) {
    case NO_ERROR:
    case ERROR_NO_DATA:
        break;

    default:
        die("GetAdaptersAddresses failed");
        break;
    }

    return pAddresses;
}

#endif

/////////////////////////////////////////////////////////////////////////////

std::vector<MacAddress> Network::getMacAddresses()
{
    std::vector <MacAddress> macAddresses;

#ifdef OS_WINDOWS
    PIP_ADAPTER_ADDRESSES pAddresses = getAdaptersAddresses();
    if (pAddresses) {
        for (PIP_ADAPTER_ADDRESSES pCurrAddresses = pAddresses; pCurrAddresses; pCurrAddresses = pCurrAddresses->Next) {
            if (pCurrAddresses->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
                continue;

            if (pCurrAddresses->PhysicalAddressLength != MacAddress::NumBytes)
                continue;

            MacAddress macAddress(ByteArray(pCurrAddresses->PhysicalAddress, MacAddress::NumBytes));
            if (macAddress.isValid())
                macAddresses.push_back(macAddress);
        }

        free(pAddresses);
    }
#endif

#ifdef OS_LINUX
    std::string sysClassNet("/sys/class/net");
    DIR *pDir = opendir(sysClassNet.c_str());
    if (pDir) {
        struct dirent *pDirent;
        while ((pDirent = readdir(pDir)) != NULL) {
            std::string name(pDirent->d_name);
            if (startsWith(name, ".") || name == "lo")
                continue;

            std::string fqname = sysClassNet + "/" + name + "/address";

            FILE *fp = fopen(fqname.c_str(), "r");
            if (fp) {
                char buf[128];
                int n = fread(&buf, 1, sizeof(buf), fp);
                if (n > 0 && n < 128) {
                    buf[n] = 0;
                    MacAddress macAddress(buf);
                    if (macAddress.isValid())
                        macAddresses.push_back(macAddress);
                }
                fclose(fp);
            }
        }
        closedir (pDir);
    }
#endif

    return macAddresses;
}

/////////////////////////////////////////////////////////////////////////////

std::vector<HostAddress> Network::getNetworkAddresses()
{
    std::vector<HostAddress> networkAddresses;

#ifdef OS_WINDOWS
    PIP_ADAPTER_ADDRESSES pAddresses = getAdaptersAddresses();
    if (pAddresses) {
        for (PIP_ADAPTER_ADDRESSES pCurrAddresses = pAddresses; pCurrAddresses; pCurrAddresses = pCurrAddresses->Next) {
            if (pCurrAddresses->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
                continue;

            if (pCurrAddresses->PhysicalAddressLength == 0)
                continue;

            for (PIP_ADAPTER_UNICAST_ADDRESS pUnicastAddress = pCurrAddresses->FirstUnicastAddress; pUnicastAddress; pUnicastAddress = pUnicastAddress->Next) {
                if (pUnicastAddress->DadState == IpDadStateInvalid)
                    continue;

                auto pSockAddr = pUnicastAddress->Address.lpSockaddr;
                if (pSockAddr && pSockAddr->sa_family == AF_INET) {
                    auto inaddr = reinterpret_cast<const sockaddr_in*>(pSockAddr)->sin_addr;

                    networkAddresses.push_back(HostAddress(ntohl(inaddr.s_addr)));
                }
            }
        }

        free(pAddresses);
    }
#endif

#ifdef OS_LINUX
    struct ifaddrs *ifaddr;
    if (getifaddrs(&ifaddr) == 0) {
       for (struct ifaddrs *ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next)
       {
           if (ifa->ifa_addr == nullptr || ifa->ifa_addr->sa_family != AF_INET)
               continue;

           auto inaddr = reinterpret_cast<const sockaddr_in*>(ifa->ifa_addr)->sin_addr;
           uint32_t addr = ntohl(inaddr.s_addr);

           // Loopback?
           if ((addr >> 24) == 0x7F)
               continue;

           networkAddresses.push_back(HostAddress(addr));
       }
       freeifaddrs(ifaddr);
    }
#endif

    return networkAddresses;
}

/////////////////////////////////////////////////////////////////////////////
