#include "AriusControl.h"

#include <arpa/inet.h>
#include <errno.h>
#ifdef __ANDROID__
#include "ifaddrs.h"
#else
#include <ifaddrs.h>
#endif
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <iostream>
#include <sstream>

#include <arpa/inet.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <netinet/ether.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <fcntl.h>
#include <netinet/ip.h>
#include <string.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <chrono>

namespace radio_control {

AriusControl::AriusControl(bool central_node)
    : vrsbs_address{""},
      keep_running{true},
      is_host{central_node},
      vrsbs_adapter{""},
      detected_model{RadioModel::VR_SBS},
      factory_reset{false} {
  handler = std::make_shared<std::thread>([this] { Handler(); });
  radio_info.rssi = 0;
  radio_info.noise_floor = 0;
  radio_info.channel_freq = -1;
  radio_info.channel_bw = -1;
  radio_info.tx_power = -1;
  radio_info.network_id = "";
  radio_info.network_password = "";
  radio_info.operation_mode = "";
  radio_info.lan_ok = -1;
}

AriusControl::AriusControl(bool central_node, std::string adapter,
                           std::string mh_ip)
    : vrsbs_address{mh_ip},
      keep_running{true},
      is_host{central_node},
      vrsbs_adapter{adapter},
      detected_model{RadioModel::UNKNOWN},
      factory_reset{false} {
  handler = std::make_shared<std::thread>([this] { Handler(); });
  radio_info.rssi = 0;
  radio_info.noise_floor = 0;
  radio_info.channel_freq = -1;
  radio_info.channel_bw = -1;
  radio_info.tx_power = -1;
  radio_info.network_id = "";
  radio_info.network_password = "";
  radio_info.operation_mode = "";
  radio_info.lan_ok = -1;
}

AriusControl::~AriusControl() {
  keep_running = false;
  while (!handler->joinable()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    radio_control::vrc_log("Child thread not joinable.\n");
  }
  handler->join();
}

bool AriusControl::IsConnected(void) {
  if (current_state == RadioState::CONNECTED) {
    return true;
  }
  return false;
}

void AriusControl::Handler(void) {
  // reset:
  current_state = RadioState::UNKNOWN;
  radio_control::vrc_log("Handler active.\n");

  while (keep_running) {
    if (current_state == RadioState::BOOTING ||
        current_state == RadioState::UNKNOWN || vrsbs_adapter == "") {
      // Get the name of the Arius's network adapter.
      while (vrsbs_adapter == "" && keep_running) {
        vrsbs_adapter = GetAdapterName();
        radio_control::vrc_log("Waiting for Arius adapter.\n");

        // Only wait if we haven't found anything yet.
        if (vrsbs_adapter == "") {
          std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }
      }
      radio_control::vrc_log("Using probable Arius adapter: " + vrsbs_adapter +
                             "\n");
      current_state = RadioState::BOOTING;

      // Make sure device is up:
      system_wrap(std::string("ip link set " + vrsbs_adapter + " up"));
      // Flush ip settings that exist:
      system_wrap(std::string("ip addr flush dev " + vrsbs_adapter));
      system_wrap(std::string("ip addr add 192.168.254.4/16 brd 255.255.255.255 dev " + vrsbs_adapter));
      system_wrap(std::string("ip link set " + vrsbs_adapter + " up"));
      system_wrap(std::string("ip rule flush"));
      system_wrap(std::string("ip rule add from all lookup main"));
      system_wrap(std::string("ip rule add from all lookup default"));
      // TODO: Make this a little more intelligent ^
    }

    int rssi = DetectArius(vrsbs_adapter);
    if (rssi == -1) {
      // We need to find the adapter again...
      current_state = RadioState::UNKNOWN;
      vrsbs_adapter = "";
    } else if (rssi == 0) {
      current_state = RadioState::DISCONNECTED;
    } else {
      current_state = RadioState::CONNECTED;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    // TODO: determine state based on RSSI value reported.

    while (config_queue.size() && keep_running) {
      if (config_queue.size()) {
        QueueItem cmd = config_queue.front();
        switch (cmd.action) {
          case radio_control::Action::ACTION_CHANGE_FREQUENCY: {  // We're going
                                                                  // to ingore
                                                                  // setting BW
                                                                  // for the
                                                                  // moment...
            auto data = any::any_cast<std::tuple<int, float, int>>(cmd.item);
            int frequency = std::get<0>(data);
            float bandwidth = std::get<1>(data);
            int channel = std::get<2>(data);

            (void)frequency;
            (void)bandwidth;
            (void)channel;
            radio_control::vrc_log(
                "Change frequency not currently implemented on SBS radio "
                "driver.\n");
            break;
          }
          case radio_control::Action::ACTION_CHANGE_NETWORK_ID: {
            auto id = any::any_cast<std::string>(cmd.item);
            std::vector<uint8_t> payload;
            payload.push_back(static_cast<uint8_t>(
                radio_control::AriusMGMTActions::MGMT_SET_RADIO_NAME));
            if (id.size() > 15) {
              payload.push_back(15);
            } else {
              payload.push_back(id.size());
            }

            // Fill the payload.
            int count = 0;
            for (char c : id) {
              if (count >= 15) {
                break;
              }
              payload.push_back(c);
              count++;
            }

            radio_control::send_udp(vrsbs_adapter, MGMT_IP, MGMT_PORT, payload);
            radio_control::vrc_log("Changing network ID / Credentials. len: " +
                                   std::to_string(payload.size()) + "\n");
            break;
          }
          case radio_control::Action::ACTION_CHANGE_PASSWORD: {
            // TODO: How do we set this on the fly?
            auto pw = any::any_cast<std::string>(cmd.item);
            (void)pw;
            radio_control::vrc_log(
                "Change network password action is currently ignored - secure "
                "password auto generated.\n");
            break;
          }
          case radio_control::Action::ACTION_RESTART_SERVICES: {
            current_state = RadioState::CONFIGURING;
            std::vector<uint8_t> payload;
            payload.push_back(static_cast<uint8_t>(
                radio_control::AriusMGMTActions::MGMT_REBOOT));
            radio_control::send_udp(vrsbs_adapter, MGMT_IP, MGMT_PORT, payload);
            radio_control::vrc_log("Restarting radio.\n");
            break;
          }
          case radio_control::Action::ACTION_SET_AIR_RATE_IMMEDIATE: {
            auto rate = any::any_cast<int>(cmd.item);
            (void)rate;
            radio_control::vrc_log(
                "ACTION_SET_AIR_RATE_IMMEDIATE not currently implemented on "
                "SBS radio driver.\n");
            break;
          }
          case radio_control::Action::ACTION_SET_AIR_RATE: {
            auto rate = any::any_cast<int>(cmd.item);
            (void)rate;
            radio_control::vrc_log(
                "ACTION_SET_AIR_RATE not currently implemented on SBS radio "
                "driver.\n");
            break;
          }
          case radio_control::Action::ACTION_SET_OUTPUT_POWER: {
            auto dbm = any::any_cast<int>(cmd.item);
            (void)dbm;
            radio_control::vrc_log(
                "ACTION_SET_OUTPUT_POWER not currently implemented on SBS "
                "radio driver.\n");
            break;
          }
          case radio_control::Action::ACTION_SET_OUTPUT_POWER_IMMEDIATE: {
            auto dbm = any::any_cast<int>(cmd.item);
            (void)dbm;
            radio_control::vrc_log(
                "ACTION_SET_OUTPUT_POWER_IMMEDIATE not currently implemented "
                "on SBS radio driver.\n");
            break;
          }
          case radio_control::Action::ACTION_CHANGE_BANDWIDTH_IMMEDIATE: {
            auto band = any::any_cast<int>(cmd.item);
            (void)band;
            radio_control::vrc_log(
                "ACTION_CHANGE_BANDWIDTH_IMMEDIATE not currently implemented "
                "on SBS radio driver.\n");
            break;
          }
          default:
            break;
        }
        config_queue.pop_front();

        // Kick the return queue:
        std::string temp("\n");
        send(sock, temp.c_str(), temp.length(), 0);
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }  // namespace radio_control

  radio_control::vrc_log("Handler exiting.\n");
}  // namespace radio_control

// Function returns likely adapter names for us to work with.
// TODO: Move into RadioControl class as a utility function?
std::string AriusControl::GetAdapterName(void) {
  struct ifaddrs *addresses;
  if (getifaddrs(&addresses) == -1) {
    radio_control::vrc_log("getifaddrs call failed\n");
    return "";
  }

  std::vector<std::string> likely_adapter_names;
  likely_adapter_names.push_back("usb");
  likely_adapter_names.push_back("enp");
  likely_adapter_names.push_back("enx");
  likely_adapter_names.push_back("eth");
  likely_adapter_names.push_back("eno");

  volatile struct ifaddrs *address = nullptr;
  address = addresses;
  std::vector<std::string> adapters;
  while ((address != nullptr) && (keep_running)) {
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

  for (auto ifa_name : adapters) {
    for (auto name : likely_adapter_names) {
      if (ifa_name.find(name) != std::string::npos) {
        radio_control::vrc_log("Trying " + ifa_name + "\n");
        // Make sure device is up:
        system_wrap(std::string("ip link set " + ifa_name + " up"));
        // Flush ip settings that exist:
        system_wrap(std::string("ip addr flush dev " + ifa_name));
        if (is_host) {
          system_wrap(std::string(
            "ip addr add 192.168.20.4/16 brd 255.255.255.255 dev " +
            ifa_name));
        } else {
           system_wrap(std::string(
            "ip addr add 192.168.20.30/16 brd 255.255.255.255 dev " +
            ifa_name));
        }
        int detected = DetectArius(name);
        if (detected != -1) {
          radio_control::vrc_log("Found a vrsbs on: " + ifa_name + "\n");
          return (ifa_name);
        } else {
          if (is_host) {
            system_wrap(
                std::string("ip addr d 192.168.20.4/16 dev " + ifa_name));
          } else {
            system_wrap(
                std::string("ip addr d 192.168.20.30/16 dev " + ifa_name));
          }
        }
      }
    }
  }
  return ("");
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-conversion"
int AriusControl::DetectArius(std::string adapter) {
  (void)adapter;
  std::vector<std::string> outputs;

  // RECEIVER:
  //struct sockaddr_in broadcastRxAddr;
  //int rx_sock;
  int tx_sock;

  // if ((rx_sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
  //   radio_control::vrc_log("Can't open receive socket\n");
  //   char buffer[256];
  //   strerror_r(errno, buffer, 256);
  //   std::string error(buffer);
  //   radio_control::vrc_log(error + "\n");
  // }

  // int one = 1;
  // if (setsockopt(rx_sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0) {
  //   radio_control::vrc_log("Can't set sockopt\n");
  //   char buffer[256];
  //   strerror_r(errno, buffer, 256);
  //   std::string error(buffer);
  //   radio_control::vrc_log(error + "\n");
  // }

  // // Add a 500mS timeout. Every time we get a response we will try to receive
  // // another, just in case we have many radios on the network.
  // // TODO: SEE IF THIS IS REQUIRED FOR THE ARIUS RADIOS
  // struct timeval tv;
  // tv.tv_sec = 0;
  // tv.tv_usec = 500000;
  // setsockopt(rx_sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof tv);

  // memset(&broadcastRxAddr, 0, sizeof(broadcastRxAddr));
  // broadcastRxAddr.sin_family = AF_INET;
  // if (is_host) {
  //    broadcastRxAddr.sin_addr.s_addr = inet_addr("192.168.20.4");
  // } else {
  //    broadcastRxAddr.sin_addr.s_addr = inet_addr("192.168.20.30");
  // }
  // //broadcastRxAddr.sin_addr.s_addr = inet_addr("255.255.255.255");
  // broadcastRxAddr.sin_port = htons(5001);

  // // Bind to the broadcast port
  // if (bind(rx_sock, (struct sockaddr *)&broadcastRxAddr,
  //          sizeof(broadcastRxAddr)) < 0) {
  //   radio_control::vrc_log("Can't bind receive socket\n");
  //   char buffer[256];
  //   strerror_r(errno, buffer, 256);
  //   std::string error(buffer);
  //   radio_control::vrc_log(error + "\n");
  // }

  // SENDER:
  struct sockaddr_in broadcastAddr;
  struct sockaddr_in broadcastBindAddr;
  int broadcastPermission;
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

  // Broadcast details
  memset(&broadcastAddr, 0, sizeof(broadcastAddr));
  broadcastAddr.sin_family = AF_INET;
  broadcastAddr.sin_addr.s_addr = inet_addr("255.255.255.255");
  broadcastAddr.sin_port = htons(5000);

  // Bind details
  memset(&broadcastBindAddr, 0, sizeof(broadcastBindAddr));
  broadcastBindAddr.sin_family = AF_INET;
  if (is_host) {
    broadcastBindAddr.sin_addr.s_addr = inet_addr("192.168.20.4");
  } else {
    broadcastBindAddr.sin_addr.s_addr = inet_addr("192.168.20.30"); 
  }
  broadcastBindAddr.sin_port = htons(13370);

  if (bind(tx_sock, (sockaddr *)&broadcastBindAddr, sizeof(broadcastBindAddr)) <
      0) {
    radio_control::vrc_log("Can't bind broadcast socket\n");
    char buffer[256];
    strerror_r(errno, buffer, 256);
    std::string error(buffer);
    radio_control::vrc_log(error + "\n");
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(250));
  uint8_t sbuf[25] = {'R', 'p', 'x', 'E', 0xFF, 0xFF, 0xFF,
                    0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00,
                    0x00, 0x00, 'R', 'p', 'x', 'C', ':', 'P',
                    'I', 'N', 'G'};

  int ret = sendto(tx_sock, sbuf, 25, 0, (sockaddr *)&broadcastAddr,
                   sizeof(broadcastAddr));
  radio_control::vrc_log("Discovery send response: " + std::to_string(ret) +
                         "\n");

  // RX!
  // Assuming that the first response we get is from our local microhard!
  // TODO: Make sure this is always the case...
  // struct sockaddr_in fromMH;
  // socklen_t addrlen = sizeof(struct sockaddr_in);
  // for (int i = 0; i < 255; i++) {
  //   char buffer[1024];
  //   int received = 0;
  //   memset(&fromMH, 0, sizeof(fromMH));

  //   // Add a 2S timeout for select.
  //   // Using the usec timeout field misbehaves on android...
  //   struct timeval tv2;
  //   tv2.tv_sec = 2;
  //   tv2.tv_usec = 0;
  //   fd_set rset;
  //   // Needed for select
  //   FD_ZERO(&rset);
  //   FD_SET(rx_sock, &rset);

  //   // select stops us from getting stuck in receive from. The timeout added to
  //   // the rx_sock doesn't always work.
  //   if (select(rx_sock + 1, &rset, NULL, NULL, &tv2) == 1) {
  //     if ((received = recvfrom(rx_sock, buffer, 1024, 0,
  //                              (struct sockaddr *)&fromMH, &addrlen)) < 0) {
  //       break;
  //     }
  //   } else {
  //     received = -1;
  //     break;
  //   }

  //   std::string ip(inet_ntoa(fromMH.sin_addr));
  //   radio_control::vrc_log("Response from: " + ip + "\n");
  //   outputs.push_back(ip);
  // }

  close(tx_sock);
  //close(rx_sock);
  return -1;
}
#pragma GCC diagnostic pop
#pragma GCC diagnostic pop

std::vector<std::tuple<int, float, int>>
AriusControl::GetSupportedFreqAndMaxBWPer(void) {
  std::vector<std::tuple<int, float, int>> options;
  int step_size = 1;
  // 356 - 380 MHz
  for (int center_freq = 356; center_freq <= 380; center_freq += step_size) {
    float bw_allowed = 2;
    options.push_back(
        std::make_tuple(center_freq, bw_allowed, (center_freq - 380)));
  }
  return options;
}

int AriusControl::SetFrequencyAndBW(int freq, float bw) {
  bool bw_ok, freq_ok = false;
  int channel_number = 0;
  for (auto bw_available : GetSupportedBWs()) {
    if (bw == static_cast<float>(bw_available)) {
      bw_ok = true;
      break;
    }
  }

  for (auto freq_avail : GetSupportedFreqAndMaxBWPer()) {
    if (freq == std::get<0>(freq_avail) &&
        bw <= static_cast<float>(std::get<1>(freq_avail))) {
      freq_ok = true;
      channel_number = std::get<2>(freq_avail);
      break;
    }
  }

  if (freq_ok && bw_ok) {
    QueueItem item = {Action::ACTION_CHANGE_FREQUENCY,
                      std::make_tuple(freq, bw, channel_number)};
    config_queue.push_back(item);
    return 0;
  } else {
    return -1;
  }
}

void AriusControl::SetNetworkID(std::string id) {
  QueueItem item = {Action::ACTION_CHANGE_NETWORK_ID, id};
  config_queue.push_back(item);
}

void AriusControl::SetNetworkPassword(std::string pw) {
  QueueItem item = {Action::ACTION_CHANGE_PASSWORD, pw};
  config_queue.push_back(item);
}

void AriusControl::ApplySettings(void) {
  QueueItem item = {Action::ACTION_RESTART_SERVICES, nullptr};
  config_queue.push_back(item);
}

void AriusControl::SetOutputPower(int dbm) {
  QueueItem item = {Action::ACTION_SET_OUTPUT_POWER, dbm};
  config_queue.push_back(item);
}

}  // namespace radio_control

// TODO: Check LAN
// TODO: Change Arius IP if in factory configuration.
// TODO: Make sure that LAN does not have DHCP enabled
// TODO: Make sure that LAN's default gateway is correct
// TODO: Check MWVMODE and set to MESH
// AT+MNLAN="lan",EDIT,0,192.168.20.20,255.255.255.0,0
// TODO: Drones get 5-99, GCS gets 20.4, and all vrsbs static IPs are 20.104
// - 20.199 If GCS, you're 20.4, and local vrsbs should be 104. If drone
// you'll start out as 20.5, with vrsbs at 105. If there's a conflict /
// there are already systems on the network, it will change the local vrsbs
// IP.

// to get encryption key: AT+MWVENCRYPT?
