#include "UdpSocket.h"

/////////////////////////////////////////////////////////////////////////////

UdpSocket::UdpSocket()
{
    m_socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
}

/////////////////////////////////////////////////////////////////////////////

UdpSocket::~UdpSocket()
{
    if (isValid())
#ifdef OS_WINDOWS
        ::closesocket(m_socket);
#else
        close(m_socket);
#endif
}

/////////////////////////////////////////////////////////////////////////////

bool UdpSocket::isValid() const
{
#ifdef OS_WINDOWS
    return (m_socket != INVALID_SOCKET);
#else
    return (m_socket >= 0);
#endif
}

/////////////////////////////////////////////////////////////////////////////

bool UdpSocket::enableReuseAddr()
{
#ifdef OS_WINDOWS
    char reuseaddr = 1;
#else
    int reuseaddr = 1;
#endif
    return (::setsockopt(m_socket, SOL_SOCKET, SO_REUSEADDR, &reuseaddr, sizeof(reuseaddr)) != SOCKET_ERROR);
}

/////////////////////////////////////////////////////////////////////////////

bool UdpSocket::enableBroadcast()
{
#ifdef OS_WINDOWS
    char broadcast = 1;
#else
    int broadcast = 1;
#endif
    return (::setsockopt(m_socket, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast)) != SOCKET_ERROR);
}

/////////////////////////////////////////////////////////////////////////////

bool UdpSocket::enableNonBlocking()
{
    unsigned long nonblock = 1;
    return (::ioctlsocket(m_socket, FIONBIO, &nonblock) != SOCKET_ERROR);
}

/////////////////////////////////////////////////////////////////////////////

bool UdpSocket::bind(const HostAddress& address, uint16_t port)
{
    SOCKADDR_IN bindAddr;

    memset(&bindAddr, 0, sizeof(bindAddr));
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_port = htons(port);
    bindAddr.sin_addr.s_addr = htonl(address.data());

    return (::bind(m_socket, (SOCKADDR *)&bindAddr, sizeof(bindAddr)) != SOCKET_ERROR);
}

/////////////////////////////////////////////////////////////////////////////

bool UdpSocket::sendTo(const ByteArray &packet, const HostAddress& address, uint16_t port)
{
    SOCKADDR_IN sendAddr;

    memset(&sendAddr, 0, sizeof(sendAddr));
    sendAddr.sin_family = AF_INET;
    sendAddr.sin_port = htons(port);
    sendAddr.sin_addr.s_addr = htonl(address.data());

    return ::sendto(m_socket, reinterpret_cast<const char *>(packet.data()), int(packet.size()), 0, (SOCKADDR *)&sendAddr, sizeof(sendAddr)) != SOCKET_ERROR;
}

/////////////////////////////////////////////////////////////////////////////

ByteArray UdpSocket::read(int maxSize)
{
    ByteArray bytes;
    bytes.resize(maxSize);

    int len = recv(m_socket, (char*)bytes.data(), maxSize, 0);
    if (len > 0)
        bytes.resize(len);

    return bytes;
}

/////////////////////////////////////////////////////////////////////////////
