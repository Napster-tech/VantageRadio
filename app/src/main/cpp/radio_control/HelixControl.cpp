#include "HelixControl.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <iostream>
#include <libssh/libsshpp.hpp>
#include <sstream>

namespace radio_control {

// We're assuming that we're a Helix 2x2 at the moment.
HelixControl::HelixControl(bool central_node)
    : helix_address{""},
      keep_running{true},
      is_host{central_node},
      current_state{RadioState::UNKNOWN},
      detected_model{RadioModel::HELIX2X2} {
  handler = std::make_shared<std::thread>([this] { Handler(); });
  (void)rssi;
  (void)detected_model;
}

HelixControl::~HelixControl() {
  keep_running = false;
  while (!handler->joinable()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    radio_control::vrc_log("Child thread not joinable.\n");
  }
  handler->join();
}

void HelixControl::Handler(void) {
  radio_control::vrc_log("Handler active.\n");
  current_state = radio_control::RadioState::BOOTING;
  // Get the name of the helix's network adapter.
#ifdef __android__
  std::string adapter = "eth0";
#else
  std::string adapter = GetAdapterName();
  while (adapter == "" && keep_running) {
    adapter = GetAdapterName();
    radio_control::vrc_log("Waiting for helix adapter.\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  }
#endif
  radio_control::vrc_log("Found probable helix adapter: " + adapter + "\n");

  // Execute a broadcast which the helix will respond to, this will tell us its
  // ip addresses. Adapter arg not currently used.
  auto result = GetHelixIPs(adapter);

  // keep trying to get helix IP response.
  while ((result.size() != 2) && keep_running) {
    result = GetHelixIPs(adapter);
    radio_control::vrc_log(
        "Issue getting adapter IPs with discovery service... Will retry.\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  }

  // IP address structs.
  struct sockaddr_in sa_config;
  struct sockaddr_in sa_network;
  struct sockaddr_in sa_local_ip_config;
  char str[INET_ADDRSTRLEN];

  // store this IP address in sa:
  inet_pton(AF_INET, result.at(0).c_str(), &(sa_config.sin_addr));
  inet_pton(AF_INET, result.at(0).c_str(), &(sa_local_ip_config.sin_addr));
  inet_pton(AF_INET, result.at(1).c_str(), &(sa_network.sin_addr));

  // Set initial config local ip's last octet to a random value.
  sa_local_ip_config.sin_addr.s_addr &= 0x00FFFFFF;
  srand(static_cast<unsigned int>(time(NULL)));
  uint8_t random = static_cast<uint8_t>(rand());
  radio_control::vrc_log("Random: " + std::to_string(random) + "\n");
  sa_local_ip_config.sin_addr.s_addr += (((uint32_t)random) << 24);

  // now get it back and print it
  inet_ntop(AF_INET, &(sa_config.sin_addr), str, INET_ADDRSTRLEN);
  std::string conf_ip(str);
  inet_ntop(AF_INET, &(sa_local_ip_config.sin_addr), str, INET_ADDRSTRLEN);
  std::string loc_ip(str);
  inet_ntop(AF_INET, &(sa_network.sin_addr), str, INET_ADDRSTRLEN);
  std::string net_ip(str);

  // Some debug statements for us to consume.
  radio_control::vrc_log("config_ip: " + conf_ip + "\n");
  radio_control::vrc_log("local_ip_config: " + loc_ip + "\n");
  radio_control::vrc_log("net_ip: " + net_ip + "\n");

  // Set our ssh config address.
  system_wrap(std::string("ip addr flush dev " + adapter));
  // Add an address on the subnet which will allow us to bind a broadcast to it.
  system_wrap(std::string("ip addr add " + loc_ip +
                          "/16 brd 255.255.255.255 dev " + adapter));

  // TODO: We need to add error handling here, ssh_session should try to
  // reconnect unless keep_running is false. Get our SSH session up.
  ssh_session session = ssh_new();
  int verbosity = SSH_LOG_NONE;
  int port = 22;

  if (session == nullptr) {
    // TODO: What should we do in this case??
    keep_running = false;
  }

  ssh_options_set(session, SSH_OPTIONS_HOST, conf_ip.c_str());
  ssh_options_set(session, SSH_OPTIONS_USER, "root");
  ssh_options_set(session, SSH_OPTIONS_LOG_VERBOSITY, &verbosity);
  ssh_options_set(session, SSH_OPTIONS_PORT, &port);
  ssh_options_set(session, SSH_OPTIONS_STRICTHOSTKEYCHECK, 0);

  int rc = ssh_connect(session);
  if (rc != SSH_OK) {
    radio_control::vrc_log("Error connecting to localhost: " +
                           std::string(ssh_get_error(session)) + "\n");
    // Again, what to do here? Keep retrying?
  }

  // IMPORTANT! We're accepting that the host is known here given this assumes
  // we're just doing an ssh connection locally on the drone, or within the
  // mesh. We may need to harden this in some way in the future.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  ssh_write_knownhost(session);
#pragma GCC diagnostic pop

  rc = ssh_userauth_password(session, NULL, "doodlesrm");
  if (rc != SSH_AUTH_SUCCESS) {
    radio_control::vrc_log("Error authenticating with password: " +
                           std::string(ssh_get_error(session)) + " attempting no pw\n");
    rc = ssh_userauth_none(session, NULL);
    if (rc != SSH_AUTH_SUCCESS) {
      radio_control::vrc_log("Error authenticating without password:\n");
    }
  }

  // Determine how many other radios are on the mesh before assigning an ip
  // address. We will determine what IP to use based on if we're the second,
  // third, or nth on the network. GCS will have a static ip of .4
  bool ip_determined = false;
  if (is_host) {
    system_wrap(std::string(
        "ip addr add 192.168.20.4/24 brd 255.255.255.255 dev " + adapter));
    ip_determined = true;
  }

  while (keep_running) {
    // Get connection details once per second.
    // std::string cmd("iw wlan0 station dump");
    std::string out =
        RunCommand(std::string("iwinfo wlan0 assoclist | head -n1"), session);

    // Look for RSSI value.
    std::string line;
    std::istringstream blob(out);
    // Wrap all of the output scraping / parsing in a try catch
    try {
      bool have_details = false;
      while (std::getline(blob, line)) {
        if (line.find("SNR") != std::string::npos) {
          have_details = true;
          radio_control::vrc_log(line + "\n");
          // TODO: Set for RSSI value
          radio_info.rssi = 0;
        }
      }

      if (have_details) {
        current_state = radio_control::RadioState::DISCONNECTED;
        radio_control::vrc_log("Likely not connected.\n");
      } else {
        current_state = radio_control::RadioState::CONNECTED;
      }

      out.clear();
      out = RunCommand(std::string("iw dev wlan0 info"), session);
      if (out.size()) {
        // Frequency parse
        {
          auto val = get_str_between_two_str(out, " (", " MHz)");
          if (val.size()) {
            radio_info.channel_freq = std::stoi(val);
            radio_control::vrc_log(std::string("Radio Frequency: ")
                                   + std::to_string(radio_info.channel_freq) + "\n");
          }
        }
        // Bandwidth parse
        {
          auto val = get_str_between_two_str(out, "width: ", " MHz,");
          if (val.size()) {
            radio_info.channel_bw = std::stoi(val);
            radio_control::vrc_log(std::string("Radio BW: ")
                                   + std::to_string(radio_info.channel_bw) + "\n");
          }
        }
      }
    } catch (...) {
      std::exception_ptr p = std::current_exception();
      // Doesn't compile on NDK...
      //radio_control::vrc_log((p ? p.__cxa_exception_type()->name() : "null")+ "\n");
    }

    while (config_queue.size()) {
      QueueItem cmd = config_queue.front();
      switch (cmd.action) {
        case radio_control::Action::
            ACTION_CHANGE_FREQUENCY: {  // We're going to ingore setting BW for
                                        // the moment...
          // std::make_tuple(freq, bw); is in the second element of the pair
          // TODO: Switch the "submodel" based on what band we're trying to
          // operate in.
          // TODO: Switch the bandwidth if different.
          auto data = any::any_cast<std::tuple<int, float, int>>(cmd.item);
          int frequency = std::get<0>(data);
          float bandwidth = std::get<1>(data);
          int channel = std::get<2>(data);

          radio_control::vrc_log(
              "Changing channel to: " + std::to_string(channel) + "\n");
          radio_control::vrc_log(
                  "Changing bandwidth to: " + std::to_string(static_cast<int>(bandwidth)) + "\n");

          (void)frequency;

          // This no longer works as of the latest Helix software update. We
          // need to figure out why. Let's add a new action:
          // ACTION_CHANGE_FREQ_NOW RunCommand(std::string("ip link set wlan0
          // down && iw wlan0 set freq "
          // +
          //                        std::to_string(frequency) +
          //                        " && ip link set wlan0 up"),
          //            session);
          // TODO: Handle changing model type as well, i.e. changing to 1.6 GHz
          // from 2.4.
          RunCommand(std::string("uci set wireless.wifi0.channel=\"" +
                                 std::to_string(channel) + "\""),
                     session);
          RunCommand(std::string("uci set wireless.radio0.channel=\"" +
                                 std::to_string(channel) + "\""),
                     session);
          RunCommand(std::string("uci set wireless.radio0.chanbw=\"" +
                                 std::to_string(static_cast<int>(bandwidth)) + "\""),
                     session);
          break;
        }
        case radio_control::Action::ACTION_CHANGE_NETWORK_ID: {
          auto id = any::any_cast<std::string>(cmd.item);
          radio_control::vrc_log("Setting mesh id to: " + id + "\n");
          // This still works, but we don't fully understand on-the-fly
          // switching requirements here. RunCommand(
          //     std::string("ip link set wlan0 down && iw wlan0 set meshid " +
          //     id
          //     +
          //                 " && ip link set wlan0 up"),
          //     session);
          RunCommand(
              std::string("uci set wireless.wifi0.mesh_id=\"" + id + "\""),
              session);
          break;
        }
        case radio_control::Action::ACTION_CHANGE_PASSWORD: {
          // TODO: How do we set this on the fly?
          auto pw = any::any_cast<std::string>(cmd.item);
          radio_control::vrc_log("Changing network pw.\n");
          RunCommand(std::string("uci set wireless.wifi0.key=\"" + pw + "\""),
                     session);
          break;
        }

        case radio_control::Action::ACTION_RESTART_SERVICES: {
          radio_control::vrc_log("Restarting services.\n");
          current_state = radio_control::RadioState::CONFIGURING;
          RunCommand(std::string("uci commit && /etc/init.d/network reload"),
                     session);
          break;
        }
        case radio_control::Action::ACTION_SET_AIR_RATE: {
          break;
        }
        case radio_control::Action::ACTION_SET_OUTPUT_POWER: {
          auto dbm = any::any_cast<int>(cmd.item);
          radio_control::vrc_log(
              "setting output power to: " + std::to_string(dbm) + "\n");
          // It takes in mdbm, so multiply by 100, e.g. 27 should be 2700
          RunCommand(std::string("iw wlan0 set txpower fixed " +
                                 std::to_string(dbm * 100)),
                     session);
          break;
        }
        default:
          break;
      }
      config_queue.pop_front();
    }

    // If we haven't yet determined out IP, try to discover neigbors.
    if (!ip_determined) {
      radio_control::vrc_log("Discovering peers.\n");
      int nodes = GetHelixCount(adapter);
      if (nodes > 1) {
        radio_control::vrc_log(
            "Nodes on the network: " + std::to_string(nodes) + "\n");
        // We're going to set out IP based on number of nodes on the network.
        system_wrap(std::string("ip addr add 192.168.20." +
                                std::to_string(4 + (nodes - 1)) +
                                "/24 brd 255.255.255.255 dev " + adapter));
        ip_determined = true;
      }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }

  radio_control::vrc_log("Handler exiting.\n");
  ssh_disconnect(session);
  ssh_free(session);
}

std::string HelixControl::RunCommand(std::string cmd, void *s) {
  ssh_session session = reinterpret_cast<ssh_session>(s);
  ssh_channel channel;
  int rc;
  char buffer[2048];
  int nbytes;
  std::string output("");

  channel = ssh_channel_new(session);

  rc = ssh_channel_open_session(channel);
  if (rc != SSH_OK) {
    ssh_channel_free(channel);
    return output;
  }

  rc = ssh_channel_request_exec(channel, cmd.c_str());
  if (rc != SSH_OK) {
    ssh_channel_close(channel);
    ssh_channel_free(channel);
    return output;
  }

  nbytes = ssh_channel_read(channel, buffer, sizeof(buffer), 0);
  while (nbytes > 0) {
    if (write(1, buffer, (long unsigned int)nbytes) != (unsigned int)nbytes) {
      ssh_channel_close(channel);
      ssh_channel_free(channel);
      return output;
    }
    nbytes = ssh_channel_read(channel, buffer, sizeof(buffer), 0);
  }

  if (nbytes < 0) {
    ssh_channel_close(channel);
    ssh_channel_free(channel);
    return output;
  }

  output = std::string(buffer);

  ssh_channel_send_eof(channel);
  ssh_channel_close(channel);
  ssh_channel_free(channel);

  return output;
}

std::string HelixControl::GetAdapterName(void) {
  struct ifaddrs *addresses;
  if (getifaddrs(&addresses) == -1) {
    radio_control::vrc_log("getifaddrs call failed\n");
    return "";
  }

  std::vector<std::string> likely_adapter_names;
  likely_adapter_names.push_back("usb");
  likely_adapter_names.push_back("eth");
  likely_adapter_names.push_back("enx");

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

  // TODO: Bring the socat broadcast detection logic into this function so that
  // we can choose the right adapter in the case where multiple of the likely
  // adapter names exist in the system. This could fail if more than one
  // likely adapter exists as it just optimistically returns the first likely
  // adapter.
  for (auto ifa_name : adapters) {
    for (auto name : likely_adapter_names) {
      if (ifa_name.find(name) != std::string::npos) {
        return (ifa_name);
      }
    }
  }
  return ("");
}

int HelixControl::GetHelixCount(std::string adapter) {
  (void)adapter;

  char buffer[1024];
  std::vector<std::string> output;
  std::string result;
  std::string command(
      "echo \"Hello\" | socat - "
      "udp-datagram:10.223.255.255:11111,broadcast,sp=5000");
  // Open pipe to file
  FILE *pipe = popen(command.c_str(), "r");
  if (!pipe) {
    return 0;
  }

  // read till end of process:
  while (!feof(pipe)) {
    // use buffer to read and add to result
    if (fgets(buffer, 1024, pipe) != NULL) result += buffer;
  }

  if (result.size() == 0) {
    return 0;
  }

  return count_lines(std::string(result));
}

std::vector<std::string> HelixControl::GetHelixIPs(std::string adapter) {
  // Flush ip settings that exist:
  // Adapter must be up before making changes on our system.
  system_wrap(std::string("ip link set " + adapter + " up"));
  system_wrap(std::string("ip addr flush dev " + adapter));
  // Use a temp address to allow us to bind a broadcast to it.
  system_wrap(std::string("ip addr add 10.223.1.1/16 brd 255.255.255.255 dev " +
                          adapter));
  system_wrap(std::string("ip route flush dev " + adapter));
  system_wrap(std::string("ip route add default dev " + adapter));
  // Got rid of this to make sure that we don't overwrite the rules for wifi.
  // system_wrap(std::string("ip rule flush"));
  system_wrap(std::string("ip rule add from all lookup main"));
  system_wrap(std::string("ip rule add from all lookup default"));

  char buffer[1024];
  std::vector<std::string> output;
  std::string result;
  std::string command(
      "echo \"Hello\" | socat - "
      "udp-datagram:10.223.255.255:11111,broadcast,sp=5000");
  // Open pipe to file
  FILE *pipe = popen(command.c_str(), "r");
  if (!pipe) {
    return output;
  }

  // read till end of process:
  while (!feof(pipe)) {
    // use buffer to read and add to result
    if (fgets(buffer, 1024, pipe) != NULL) result += buffer;
  }

  if (result.size() == 0) {
    return output;
  }

  radio_control::vrc_log(std::string(result));

  //{"ingress":["eth0"],"IPaddr":["192.168.100.1/24","10.223.174.103/16","192.168.153.1/24","fe80::230:1aff:fe4f:ae68/64"]}
  //{"ingress":["eth0"],"IPaddr":["100.223.174.103/16","192.168.153.100/24","fe80::230:1aff:fe4f:ae68/64"]}
  std::string config_ip, network_ip;

  if (result.size() > 24) {
    config_ip = std::string("10.223." +
                            get_str_between_two_str(result, "10.223.", "/16"));
    network_ip = std::string("192.168.153.1");
    output.push_back(config_ip);
    output.push_back(network_ip);
  } else {
    return output;
  }

  pclose(pipe);
  return output;
}

HelixControl::RFInfo HelixControl::ParseInfo(std::string raw) {
  HelixControl::RFInfo info = {};
  radio_control::vrc_log(raw);
  return info;
}

std::vector<std::tuple<int, float, int>>
HelixControl::GetSupportedFreqAndMaxBWPer(void) {
  std::vector<std::tuple<int, float, int>> options;
  // 1625 - 1725 MHz
  // Start Freq: 1630
  // End Freq: 1720
  // Stepsize: 1 MHz
  int step_size = 1;
  for (int center_freq = 1630; center_freq <= 1720; center_freq += step_size) {
    float bw_allowed = 3;
    if (((center_freq - 40) > 1625) && ((center_freq + 40) < 1725)) {
      bw_allowed = 40;
    } else if (((center_freq - 26) > 1625) && ((center_freq + 26) < 1725)) {
      bw_allowed = 26;
    } else if (((center_freq - 20) > 1625) && ((center_freq + 20) < 1725)) {
      bw_allowed = 20;
    } else if (((center_freq - 15) > 1625) && ((center_freq + 15) < 1725)) {
      bw_allowed = 15;
    } else if (((center_freq - 10) > 1625) && ((center_freq + 10) < 1725)) {
      bw_allowed = 10;
    } else if (((center_freq - 5) > 1625) && ((center_freq + 5) < 1725)) {
      bw_allowed = 5;
    }
    options.push_back(
        std::make_tuple(center_freq, bw_allowed, (center_freq - 1625) + 1));
  }

  // 1780 - 1850 MHz
  step_size = 1;
  for (int center_freq = 1785; center_freq <= 1845; center_freq += step_size) {
    float bw_allowed = 3;
    if (((center_freq - 40) > 1780) && ((center_freq + 40) < 1850)) {
      bw_allowed = 40;
    } else if (((center_freq - 26) > 1780) && ((center_freq + 26) < 1850)) {
      bw_allowed = 26;
    } else if (((center_freq - 20) > 1780) && ((center_freq + 20) < 1850)) {
      bw_allowed = 20;
    } else if (((center_freq - 15) > 1780) && ((center_freq + 15) < 1850)) {
      bw_allowed = 15;
    } else if (((center_freq - 10) > 1780) && ((center_freq + 10) < 1850)) {
      bw_allowed = 10;
    } else if (((center_freq - 5) > 1780) && ((center_freq + 5) < 1850)) {
      bw_allowed = 5;
    }
    options.push_back(
        std::make_tuple(center_freq, bw_allowed, (center_freq - 1780) + 1));
  }

  // 2025 - 2110 MHz
  step_size = 1;
  for (int center_freq = 2030; center_freq <= 2105; center_freq += step_size) {
    float bw_allowed = 3;
    if (((center_freq - 40) > 2025) && ((center_freq + 40) < 2110)) {
      bw_allowed = 40;
    } else if (((center_freq - 26) > 2025) && ((center_freq + 26) < 2110)) {
      bw_allowed = 26;
    } else if (((center_freq - 20) > 2025) && ((center_freq + 20) < 2110)) {
      bw_allowed = 20;
    } else if (((center_freq - 15) > 2025) && ((center_freq + 15) < 2110)) {
      bw_allowed = 15;
    } else if (((center_freq - 10) > 2025) && ((center_freq + 10) < 2110)) {
      bw_allowed = 10;
    } else if (((center_freq - 5) > 2025) && ((center_freq + 5) < 2110)) {
      bw_allowed = 5;
    }
    options.push_back(
        std::make_tuple(center_freq, bw_allowed, (center_freq - 2025) + 1));
  }

  // 2200 - 2290 MHz
  step_size = 1;
  for (int center_freq = 2205; center_freq <= 2285; center_freq += step_size) {
    float bw_allowed = 3;
    if (((center_freq - 40) > 2200) && ((center_freq + 40) < 2290)) {
      bw_allowed = 40;
    } else if (((center_freq - 26) > 2200) && ((center_freq + 26) < 2290)) {
      bw_allowed = 26;
    } else if (((center_freq - 20) > 2200) && ((center_freq + 20) < 2290)) {
      bw_allowed = 20;
    } else if (((center_freq - 15) > 2200) && ((center_freq + 15) < 2290)) {
      bw_allowed = 15;
    } else if (((center_freq - 10) > 2200) && ((center_freq + 10) < 2290)) {
      bw_allowed = 10;
    } else if (((center_freq - 5) > 2200) && ((center_freq + 5) < 2290)) {
      bw_allowed = 5;
    }
    options.push_back(
        std::make_tuple(center_freq, bw_allowed, (center_freq - 2200) + 1));
  }

  // 2310 - 2390 MHz
  step_size = 1;
  for (int center_freq = 2315; center_freq <= 2385; center_freq += step_size) {
    float bw_allowed = 3;
    if (((center_freq - 40) > 2310) && ((center_freq + 40) < 2390)) {
      bw_allowed = 40;
    } else if (((center_freq - 26) > 2310) && ((center_freq + 26) < 2390)) {
      bw_allowed = 26;
    } else if (((center_freq - 20) > 2310) && ((center_freq + 20) < 2390)) {
      bw_allowed = 20;
    } else if (((center_freq - 15) > 2310) && ((center_freq + 15) < 2390)) {
      bw_allowed = 15;
    } else if (((center_freq - 10) > 2310) && ((center_freq + 10) < 2390)) {
      bw_allowed = 10;
    } else if (((center_freq - 5) > 2310) && ((center_freq + 5) < 2390)) {
      bw_allowed = 5;
    }
    options.push_back(
        std::make_tuple(center_freq, bw_allowed, (center_freq - 2310) + 1));
  }

  // 2400 - 2500 MHz
  step_size = 1;
  for (int center_freq = 2402; center_freq <= 2498; center_freq += step_size) {
    float bw_allowed = 3;
    if (((center_freq - 40) > 2400) && ((center_freq + 40) < 2500)) {
      bw_allowed = 40;
    } else if (((center_freq - 26) > 2400) && ((center_freq + 26) < 2500)) {
      bw_allowed = 26;
    } else if (((center_freq - 20) > 2400) && ((center_freq + 20) < 2500)) {
      bw_allowed = 20;
    } else if (((center_freq - 15) > 2400) && ((center_freq + 15) < 2500)) {
      bw_allowed = 15;
    } else if (((center_freq - 10) > 2400) && ((center_freq + 10) < 2500)) {
      bw_allowed = 10;
    } else if (((center_freq - 5) > 2400) && ((center_freq + 5) < 2500)) {
      bw_allowed = 5;
    }
    options.push_back(
        std::make_tuple(center_freq, bw_allowed, (center_freq - 2400) + 1));
  }

  return options;
}

int HelixControl::SetFrequencyAndBW(int freq, float bw) {
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

void HelixControl::SetNetworkID(std::string id) {
  QueueItem item = {Action::ACTION_CHANGE_NETWORK_ID, id};
  config_queue.push_back(item);
}

void HelixControl::SetNetworkPassword(std::string pw) {
  QueueItem item = {Action::ACTION_CHANGE_PASSWORD, pw};
  config_queue.push_back(item);
}

void HelixControl::SetNetworkIPAddr(std::string ipaddr) {
  network_ipaddr = ipaddr;
}

void HelixControl::SetRadioIPAddr(std::string radio_ipaddr) {
  helix_address = radio_ipaddr;
}

void HelixControl::SetTopology(Topology topology) {
  (void)topology;
  // only support mesh mode
}

void HelixControl::ApplySettings(void) {
  QueueItem item = {Action::ACTION_RESTART_SERVICES, nullptr};
  config_queue.push_back(item);
}

void HelixControl::SetOutputPower(int dbm) {
  QueueItem item = {Action::ACTION_SET_OUTPUT_POWER, dbm};
  config_queue.push_back(item);
}
}  // namespace radio_control

// RFInfo inf;
// inf.rssi = std::stoi(get_str_between_two_str(raw, "signal avg:     ", " ["));
// inf.tx_bitrate =
//     std::stof(get_str_between_two_str(raw, "tx bitrate:     ", " MBit/s"));
// inf.rx_bitrate =
//     std::stof(get_str_between_two_str(raw, "rx bitrate:     ", " MBit/s"));
// inf.rx_packets =
//     std::stoi(get_str_between_two_str(raw, "rx packets:     ", "\n"));
// inf.tx_packets =
//     std::stoi(get_str_between_two_str(raw, "tx packets:     ", "\n"));

// Discovery:
// echo "Hello" | socat - udp-datagram:255.255.255.255:11111,broadcast,sp=5000
//
// Setting 10.223.174.103oot@smartradio-301a4fae67:~# ifconfig wlan0 down
// root@smartradio-301a4fae67:~# iw wlan0 set meshid testtest
// root@smartradio-301a4fae67:~# iw wlan0 set freq 2420
// root@smartradio-301a4fae67:~# ifconfig wlan0 up

// Submodel config files exist in . /usr/share/.doodlelabs/fes/"$sr_model"
// directory is used with /etc/rc.d/S02check_model to set the model in UCI?

// /var/run/fes_state
// /usr/sbin/fes_model.sh

// For 1x1:
// sub_model0="RM-1675-1L-X"
// sub_model1="RM-1815-1L-X"
// sub_model2="RM-2065-1L-X"
// sub_model3="RM-2245-1L-X"
// sub_model4="RM-2350-1L-X"
// sub_model5="RM-2455-1L-X"
// sub_model6="RM-2450-1L-X"
