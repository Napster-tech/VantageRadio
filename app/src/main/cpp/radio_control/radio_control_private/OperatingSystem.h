#pragma once

#if defined (_WIN32)
#define OS_WINDOWS
#elif defined (__linux__)
#define OS_LINUX
#endif

#ifdef OS_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#endif

#ifdef OS_LINUX
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <memory.h>
#include <unistd.h>
#include <dirent.h>
#include <ifaddrs.h>
#include <netdb.h>

#define SOCKET_ERROR (-1)
#define ioctlsocket ioctl

using SOCKET = int;
using SOCKADDR = struct sockaddr;
using SOCKADDR_IN = struct sockaddr_in;
#endif

#if __has_cpp_attribute(fallthrough)
#define FALLTHROUGH() [[fallthrough]]
#else
#define FALLTHROUGH (void)0
#endif

#include <cstdint>

class OperatingSystem
{
public:
    static uint64_t getCurrentTime();
};
