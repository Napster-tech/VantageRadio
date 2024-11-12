#include "RadioControl.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <deque>
#include <string.h>
#include <string>
#include <strings.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#ifdef __ANDROID__
#include "ifaddrs.h"
#include <sys/system_properties.h>
#else
#include <ifaddrs.h>
#endif

namespace radio_control {
int count_lines(std::string in) {
  int newlines = 0;
  int size = static_cast<int>(in.size());
  const char *p = &(in.c_str()[0]);
  for (int i = 0; i < size; i++) {
    if (p[i] == '\n') {
      newlines++;
    }
  }
  return newlines;
}

std::string get_str_between_two_str(const std::string &s,
                                    const std::string &start_delim,
                                    const std::string &stop_delim) {
  long unsigned int first_delim_pos = s.find(start_delim);
  long unsigned int end_pos_of_first_delim =
      first_delim_pos + start_delim.length();
  long unsigned int last_delim_pos = s.find(stop_delim);

  return s.substr(end_pos_of_first_delim,
                  last_delim_pos - end_pos_of_first_delim);
}

// Blind system() wrap. No retval
void system_wrap(std::string command) {
#if defined(__ANDROID__) || defined(__android__)
  // Hacky, uses our vendor build string id form used on Vision2.
  // Determine if we're on secure or "insecure" build and switch
  // ip invocation accordingly.
  char sdk_ver_str[256];
  __system_property_get("ro.system.build.id", sdk_ver_str);
  vrc_log(std::string(sdk_ver_str) + "\n");
  sdk_ver_str[2] = '\0';
  int major_version = atoi(&(sdk_ver_str[1]));
  sdk_ver_str[4] = '\0';
  int minor_1 = atoi(&(sdk_ver_str[3]));
  sdk_ver_str[6] = '\0';
  int minor_2 = atoi(&(sdk_ver_str[5]));
  sdk_ver_str[9] = '\0';
  int minor_3 = atoi(&(sdk_ver_str[7]));
  bool use_insecure = false;
  if (major_version == 1 && minor_1 == 0 && minor_2 == 0) {
    if (minor_3 < 24) {
      use_insecure = true;
    } else {
      use_insecure = false;
    }
  } else {
    vrc_log("ENGINEERING: Update system version check call.\n");
    use_insecure = false;
  }
  if (use_insecure) {
    system(std::string("su -c " + command).c_str());
  } else {
    system(std::string(command).c_str());
  }
#else
#if BUILD_WITH_RDB
  // Here we're assuming that we're running as root on the drone
  system(command.c_str());
#else
  // Assumes that ip is on the NOPASSWD list in /etc/sudoers
  system(std::string("sudo " + command).c_str());
#endif
#endif
}

// TODO: More intelligent stuff.
std::deque<std::string> log_queue;
std::mutex log_mutex;
void vrc_log(std::string input) {
  {
    std::lock_guard<std::mutex> log_lock(log_mutex);
#if defined(__ANDROID__) || defined(__android__)
    // Don't let the log queue get gigantic between
    // app flushes.
    if (log_queue.size() > 500) {
      log_queue.pop_back();
    }
    log_queue.emplace_front(input);
#else
    std::cout << "[VRC] " << input;
#endif
  }
}

int get_vrc_log_len(void) { return log_queue.size(); }

std::string get_vrc_log(void) {
  {
    std::lock_guard<std::mutex> log_lock(log_mutex);
    std::string oldest = log_queue.back();
    log_queue.pop_back();
    return oldest;
  }
}

bool send_udp(const std::string adapter, const std::string ip, const int port,
              std::vector<uint8_t> payload) {
  (void)adapter;
  sockaddr_in servaddr;
  int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0) {
    vrc_log("cannot open socket\n");
    return false;
  }

  bzero(&servaddr, sizeof(servaddr));
  servaddr.sin_family = AF_INET;
  servaddr.sin_addr.s_addr = inet_addr(ip.c_str());
  servaddr.sin_port = htons(port);
  if (sendto(fd, payload.data(), payload.size(), 0, // +1 to include terminator
             (sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
    vrc_log("send_udp failed\n");
    close(fd);
    return false;
  }

  close(fd);
  return true;
}

// TODO: Create a system() call wrapper that allows us to prepend su -c for
// android builds...
std::string system_call(std::string command) {
  char buffer[1024];
  std::string result;
  // Open pipe to file
  FILE *pipe = popen(command.c_str(), "r");
  if (!pipe) {
    return result;
  }

  // read till end of process:
  while (!feof(pipe)) {
    // use buffer to read and add to result
    if (fgets(buffer, 1024, pipe) != NULL)
      result += buffer;
  }

  return result;
}

bool is_adapter_present(std::string adapter) {
  struct ifaddrs *addresses;
  if (getifaddrs(&addresses) == -1) {
    radio_control::vrc_log("getifaddrs call failed\n");
    return "";
  }

  volatile struct ifaddrs *address = nullptr;
  address = addresses;
  std::vector<std::string> adapters;
  while (address != nullptr) {
    if (address->ifa_addr == nullptr) {
      address = address->ifa_next;
      continue;
    }
    int family = address->ifa_addr->sa_family;
    if (family == AF_INET || family == AF_INET6 || family == AF_PACKET) {
      adapters.push_back(std::string(address->ifa_name));
    }
    address = address->ifa_next;
  }

  for (auto nic : adapters) {
    if (nic == adapter) {
      return true;
    }
  }

  return false;
}

std::string get_subnet(std::string ip_address) {
  struct sockaddr_in sa_ip_config;
  inet_pton(AF_INET, ip_address.c_str(), &(sa_ip_config.sin_addr));
  sa_ip_config.sin_addr.s_addr &= 0x00FFFFFF;

  char addr_str[INET_ADDRSTRLEN];
  sa_ip_config.sin_addr.s_addr |= (((uint32_t)255) << 24);
  inet_ntop(AF_INET, &(sa_ip_config.sin_addr), addr_str, sizeof(addr_str));
  std::string subnet_ip(addr_str);
  auto pos = subnet_ip.find_last_of('.');
  return subnet_ip.substr(0, pos - 1);
}

std::vector<uint8_t> broadcast_and_listen(std::vector<uint8_t> data,
                                          uint16_t broadcast_port,
                                          uint16_t listen_port,
                                          uint16_t from_port,
                                          std::string adapter_ip,
                                          uint16_t msec_wait,
                                          std::string *detected_ip = nullptr) {
  std::vector<uint8_t> ret_vec;
  // RECEIVER:
  struct sockaddr_in broadcastRxAddr;
  int rx_sock;
  int tx_sock;

  if ((rx_sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
    radio_control::vrc_log("Can't open receive socket\n");
    char buffer[256];
    strerror_r(errno, buffer, 256);
    std::string error(buffer);
    radio_control::vrc_log(error + "\n");
  }

  int one = 1;
  if (setsockopt(rx_sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0) {
    radio_control::vrc_log("Can't set sockopt\n");
    char buffer[256];
    strerror_r(errno, buffer, 256);
    std::string error(buffer);
    radio_control::vrc_log(error + "\n");
  }

  if (setsockopt(rx_sock, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one)) < 0) {
    radio_control::vrc_log("Can't set sockopt\n");
    char buffer[256];
    strerror_r(errno, buffer, 256);
    std::string error(buffer);
    radio_control::vrc_log(error + "\n");
  }

  // Add a 500mS timeout. Every time we get a response we will try to receive
  // another, just in case we have many microhards on the network.
  struct timeval tv;
  tv.tv_sec = 0;
  tv.tv_usec = 500000;
  setsockopt(rx_sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof tv);

  memset(&broadcastRxAddr, 0, sizeof(broadcastRxAddr));
  broadcastRxAddr.sin_family = AF_INET;
  broadcastRxAddr.sin_addr.s_addr = inet_addr("255.255.255.255");
  broadcastRxAddr.sin_port = htons(listen_port);

  /* Bind to the broadcast port */
  if (bind(rx_sock, (struct sockaddr *)&broadcastRxAddr,
           sizeof(broadcastRxAddr)) < 0) {
    radio_control::vrc_log("Can't bind receive socket\n");
    char buffer[256];
    strerror_r(errno, buffer, 256);
    std::string error(buffer);
    radio_control::vrc_log(error + "\n");
  }

  // SENDER:
  struct sockaddr_in broadcastAddr;
  struct sockaddr_in broadcastBindAddr;
  int broadcastPermission; /* Socket opt to set permission to broadcast */

  if ((tx_sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
    radio_control::vrc_log("Couldn't make socket\n");
    char buffer[256];
    strerror_r(errno, buffer, 256);
    std::string error(buffer);
    radio_control::vrc_log(error + "\n");
  }

  broadcastPermission = 1;
  if (setsockopt(tx_sock, SOL_SOCKET, SO_BROADCAST,
                 (void *)&broadcastPermission,
                 sizeof(broadcastPermission)) < 0) {
    radio_control::vrc_log("Can't set sockopt\n");
    char buffer[256];
    strerror_r(errno, buffer, 256);
    std::string error(buffer);
    radio_control::vrc_log(error + "\n");
  }

  if (setsockopt(tx_sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0) {
    radio_control::vrc_log("Can't set sockopt\n");
    char buffer[256];
    strerror_r(errno, buffer, 256);
    std::string error(buffer);
    radio_control::vrc_log(error + "\n");
  }

  if (setsockopt(tx_sock, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one)) < 0) {
    radio_control::vrc_log("Can't set sockopt\n");
    char buffer[256];
    strerror_r(errno, buffer, 256);
    std::string error(buffer);
    radio_control::vrc_log(error + "\n");
  }

  // Broadcast details
  memset(&broadcastAddr, 0, sizeof(broadcastAddr));
  broadcastAddr.sin_family = AF_INET;
  broadcastAddr.sin_addr.s_addr = inet_addr("255.255.255.255");
  broadcastAddr.sin_port = htons(broadcast_port);

  // Bind details
  memset(&broadcastBindAddr, 0, sizeof(broadcastBindAddr));
  broadcastBindAddr.sin_family = AF_INET;
  broadcastBindAddr.sin_addr.s_addr = inet_addr(adapter_ip.c_str());
  broadcastBindAddr.sin_port = htons(from_port);

  if (bind(tx_sock, (sockaddr *)&broadcastBindAddr, sizeof(broadcastBindAddr)) <
      0) {
    radio_control::vrc_log(
        "Can't bind broadcast socket. errno: " + std::to_string(errno) + "\n");
    char buffer[256];
    strerror_r(errno, buffer, 256);
    std::string error(buffer);
    radio_control::vrc_log(error + "\n");
  }

  int ret = sendto(tx_sock, data.data(), data.size(), 0,
                   (sockaddr *)&broadcastAddr, sizeof(broadcastAddr));
  (void)ret;
  std::this_thread::sleep_for(std::chrono::milliseconds(msec_wait));

  // RX!
  // Assuming that the first response we get is from our local microhard!
  // TODO: Make sure this is always the case...
  struct sockaddr_in from;
  socklen_t addrlen = sizeof(struct sockaddr_in);
  for (int i = 0; i < 255; i++) {
    char buffer[1024];
    int received = 0;
    memset(&from, 0, sizeof(from));

    // Add a 1S timeout for select.
    // Using the usec timeout field misbehaves on android...
    struct timeval tv2;
    tv2.tv_sec = 1;
    tv2.tv_usec = 0;
    fd_set rset;
    // Needed for select
    FD_ZERO(&rset);
    FD_SET(rx_sock, &rset);

    // select stop us from getting stuck in receive from. The timeout added to
    // the rx_sock doesn't always work.
    if (select(rx_sock + 1, &rset, NULL, NULL, &tv2) == 1) {
      if ((received = recvfrom(rx_sock, buffer, 1024, 0,
                               (struct sockaddr *)&from, &addrlen)) < 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        continue;
      }
    } else {
      received = -1;
      break;
    }

    std::string ip(inet_ntoa(from.sin_addr));
    if (ip == adapter_ip) {
      // We will receive our own broadcast, so skip this case.
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    } else {
      radio_control::vrc_log("Response from: " + ip + "\n");
      if (detected_ip != nullptr) {
        *detected_ip = ip;
      }
      for (int j = 0; j < received; j++) {
        ret_vec.emplace_back(buffer[j]);
      }
    }
  }

  close(tx_sock);
  close(rx_sock);
  return ret_vec;
}

} // namespace radio_control
