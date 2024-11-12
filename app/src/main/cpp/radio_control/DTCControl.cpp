#include "DTCControl.h"

#include <arpa/inet.h>
#include <errno.h>
#ifdef __ANDROID__
#include "ifaddrs.h"
#else
#include <ifaddrs.h>
#endif
#include <fstream>
#include <iostream>
#include <netdb.h>
#include <netinet/in.h>
#include <sstream>
#include <sys/socket.h>

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

#include <chrono>
#include <fcntl.h>
#include <netinet/ip.h>
#include <string.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "httplib.h"
#include "picojson.h"
#include "radio_control_private/DeviceDiscoveryProtocol.h"
#include "radio_control_private/RandomNumber.h"

namespace radio_control {
// DTC provided device discovery protocol object.
// We only use this for creating and decoding payloads.
// We handle all of the network hardware interactions
// ourselves.
DeviceDiscoveryProtocol ddp;

DTCControl::DTCControl(bool central_node)
    : keep_running{true}, dtc_adapter{""}, detected_model{RadioModel::DTC} {
  handler = std::make_shared<std::thread>([this] { Handler(); });
  radio_info.rssi = 0;
  radio_info.noise_floor = 0;
  radio_info.channel_freq = -1;
  radio_info.channel_bw = -1;
  radio_info.max_freq = -1;
  radio_info.min_freq = -1;
  radio_info.network_id = "";
  radio_info.network_password = "";
  if (central_node) {
    config_ip_network = "192.168.20.4";
    config_ip_radio = "192.168.20.104";
  } else {
    config_ip_network = "192.168.20.30";
    config_ip_radio = "192.168.20.105";
  }
  dtc_subnet = "192.168.20";
  pw_reset_token = "";
  pw_reset = false;
  is_host = central_node;
}

DTCControl::DTCControl(bool central_node, std::string adapter,
                       std::string dtc_ip, std::string net_ip)
    : keep_running{true}, dtc_adapter{adapter}, config_ip_network{net_ip},
      config_ip_radio{dtc_ip}, detected_model{RadioModel::DTC} {
  handler = std::make_shared<std::thread>([this] { Handler(); });
  dtc_subnet = (!dtc_ip.empty()) ? get_subnet(dtc_ip) : "";
  radio_info.rssi = 0;
  radio_info.noise_floor = 0;
  radio_info.channel_freq = -1;
  radio_info.channel_bw = -1;
  radio_info.max_freq = -1;
  radio_info.min_freq = -1;
  radio_info.network_id = "";
  radio_info.network_password = "";
  pw_reset_token = "";
  pw_reset = false;
  is_host = central_node;
}

DTCControl::~DTCControl() {
  keep_running = false;
  while (!handler->joinable()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    radio_control::vrc_log("Child thread not joinable.\n");
  }
  handler->join();
}

bool DTCControl::IsConnected(void) {
  if (current_state == RadioState::CONNECTED) {
    return true;
  }
  return false;
}

void DTCControl::Handler(void) {
  // DTC Random number init.
  RandomNumber::init();
  // reset:
  current_state = RadioState::UNKNOWN;
  radio_control::vrc_log("Handler active.\n");
  std::string local_ip("192.168.254.4");

  

  while (keep_running) {
    // Get the name of the DTC's network adapter.
    while (dtc_adapter == "" && keep_running) {
      dtc_adapter = GetAdapterName();

      // Only wait if we haven't found anything yet.
      if (dtc_adapter == "") {
        radio_control::vrc_log("Waiting for DTC adapter.\n");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      } else {
        radio_control::vrc_log("DTC radio found on " + dtc_adapter + "\n");
      }
    }

    // Set up network data.
    // TODO: If is-host
    static bool port_configured = false;
    if (!port_configured) {
      current_state = RadioState::BOOTING;
      vrc_log("Sending network configuration.\n");
      uint32_t message_id_out = ddp.getNextMessageId();
      DeviceDiscoveryProtocolMessage command;
      command.clear();
      command.setLocalOnly(true);
      command.setTargetMacAddress(MacAddress(dtc_device_mac));
      command.setSourceMacAddress(MacAddress(dtc_adapter_mac));
      command.setMessageId(message_id_out);
      command.setCommandId(DeviceDiscoveryProtocolMessage::ConfigureNetwork);
      command.appendData(DeviceDiscoveryProtocolMessage::DhcpEnabled, false);
      command.appendData(DeviceDiscoveryProtocolMessage::StaticIpAddress,
                         HostAddress(config_ip_radio));
      command.appendData(DeviceDiscoveryProtocolMessage::StaticNetmask,
                         HostAddress("255.255.255.0"));
      command.appendData(DeviceDiscoveryProtocolMessage::StaticGateway,
                         HostAddress(config_ip_radio));
      std::vector<uint8_t> data;
      ByteArray commandData = command.getMessage();
      for (size_t i = 0; i < commandData.size(); i++) {
        data.emplace_back(commandData.data()[i]);
      }

      std::string detected_ip = "";
      // Attempt to find DTC radio on the network.
      std::vector<uint8_t> response = radio_control::broadcast_and_listen(
          data, 22484, 22484, 13370, "192.168.254.4", 50, &detected_ip);

      if (response.size()) {
        vrc_log("Network config response size: " +
                std::to_string(response.size()) + "\n");
        DeviceDiscoveryProtocolMessage message;
        message.clear();
        message.parse(response, MacAddress(dtc_adapter_mac));
        uint8_t errorCode = message.getErrorCode();
        uint32_t message_id_in = message.getMessageId();
        vrc_log("sentID: " + std::to_string(message_id_out) +
                " inID: " + std::to_string(message_id_in) + "\n");
        uint8_t command_id = message.getCommandId();
        if (errorCode == DeviceDiscoveryProtocolMessage::NoError &&
            command_id == DeviceDiscoveryProtocolMessage::ConfigureNetwork &&
            (detected_ip == config_ip_radio)) {

          // Set our config address.
          system_wrap(std::string("ip addr add dev " + dtc_adapter));
          // Add an address on the subnet which will allow us to bind a
          // broadcast to it.
          system_wrap(std::string("ip addr add " + config_ip_network +
                                  "/24 brd " + dtc_subnet + ".255 dev " +
                                  dtc_adapter));
          system_wrap(std::string("ip link set " + dtc_adapter + " up"));
          system_wrap(
              std::string("ip route add 224.0.0.0/4 dev " + dtc_adapter));
          // Purge ip rules
          system_wrap(std::string("ip rule flush"));
          system_wrap(std::string("ip rule add from all lookup main"));
          system_wrap(std::string("ip rule add from all lookup default"));
          dtc_device_ip = detected_ip;
          port_configured = true;
        } else {
          current_state = RadioState::CONFIGURING;
        }
        if (command_id != DeviceDiscoveryProtocolMessage::ConfigureNetwork) {
          vrc_log("Received unexpected command response to config: " +
                  std::to_string(command_id) + "\n");
        }
        if (errorCode != DeviceDiscoveryProtocolMessage::NoError) {
          vrc_log("error: " + std::to_string(errorCode) + " " +
                  message.dataToString(
                      DeviceDiscoveryProtocolMessage::ErrorMessage));
        }
      } else {
        vrc_log("NO NETWORK CONFIGURATION RESPONSE!\n");
      }
    }

    // HTTP GET of status json.
    std::string address = "http://" + dtc_device_ip;
    httplib::Client cli(address);
    cli.set_connection_timeout(500);
    cli.set_write_timeout(500);
    cli.set_basic_auth("admin", "Eastwood");
    auto res = cli.Get("/status.json", {{"Accept-Encoding", "identity"}});
    if (res) {
      if (res->status == 200) {
        picojson::value v;
        std::string err = picojson::parse(v, res->body);
        if (!err.empty()) {
          vrc_log("error parsing status json\n");
        } else {
          picojson::value status = v.get("Status");
          picojson::value mesh = status.get("Mesh1");
          radio_info.min_freq = std::stoi(status.get("minFreq").to_str());
          radio_info.max_freq = std::stoi(status.get("maxFreq").to_str());
          radio_info.channel_freq =
              std::stoi(mesh.get("MeshCentreFreq").to_str());

          // Convert bandwidth index to bw value that the radio_control libary
          // API needs.
          uint32_t bw_index = std::stoul(mesh.get("ChanBandwidth").to_str());
          auto bws = GetSupportedBWs();
          if (bw_index < bws.size()) {
            radio_info.channel_bw = bws.at(bw_index);
          } else {
            radio_info.channel_bw = -1;
          }
          radio_info.network_id = mesh.get("MeshId").to_str();
          radio_info.node_id = std::stoi(mesh.get("NodeId").to_str());

          // Check to see if we're connected. Later we can use the SNR value to set bitrate in a 2 node network.
          auto snrs = mesh.get("LocalDemodStatus").get("SNR").get<picojson::array>();
          auto rssis = mesh.get("LocalDemodStatus").get("sigLevA").get<picojson::array>();

          // TODO: determine what bitrates to use based on the mesh path states.
          float max_rssi = -120;
          float max_snr = -3;
          for (auto snr : snrs) {
            float node_snr = std::stof(snr.to_str());
            if (node_snr > max_snr) {
              max_snr = node_snr;
            }
          }
          for (auto rssi : rssis) {
            float node_rssi = std::stof(rssi.to_str());
            if (node_rssi > max_rssi) {
              max_rssi = node_rssi;
            }
          }
          if (max_rssi != 120 && max_snr != -3) {
            current_state = RadioState::CONNECTED;
            radio_info.rssi = max_rssi;
            radio_info.noise_floor = max_rssi - max_snr; // Needs to be fixed
            vrc_log("RSSI/SNR: " + std::to_string(radio_info.rssi) + "/" +
                  std::to_string(max_snr) + "\n");
          } else {
            current_state = RadioState::DISCONNECTED;
          }
          vrc_log("Freq/BW: " + std::to_string(radio_info.channel_freq) + "/" +
                  std::to_string(radio_info.channel_bw) + "\n");
        }
      }
    } else {
      auto err = res.error();
      current_state = radio_control::RadioState::UNKNOWN;
      std::cout << "HTTP error: " << httplib::to_string(err) << std::endl;
    }

    while (config_queue.size() && keep_running && port_configured) {
      if (config_queue.size()) {
        QueueItem cmd = config_queue.front();
        switch (cmd.action) {
        case radio_control::Action::ACTION_CHANGE_FREQUENCY: { // We're going
                                                               // to ingore
                                                               // setting BW
                                                               // for the
                                                               // moment...
          auto data = any::any_cast<std::tuple<int, float, int>>(cmd.item);
          int frequency = std::get<0>(data);
          float bandwidth = std::get<1>(data);
          int bw_index = std::get<2>(data);
          (void)bandwidth;

          picojson::object freq;
          freq["CentreFreq"] = picojson::value(static_cast<float>(frequency));
          freq["CentreFreqD"] = picojson::value(static_cast<float>(frequency));
          freq["ChanBandwidth"] = picojson::value(static_cast<float>(bw_index));
          freq["ModOnOff"] = picojson::value(static_cast<bool>(true));
          picojson::object modulation;
          modulation["Modulation"] = picojson::value(freq);

          picojson::object mesh_config;
          // Hard-coded for now
          mesh_config["MeshId"] = picojson::value(static_cast<float>(13));
          if (is_host) {
            mesh_config["NodeId"] = picojson::value(static_cast<float>(1));
          } else {
            mesh_config["NodeId"] = picojson::value(static_cast<float>(2));
          }

          picojson::object mesh1;
          mesh1["Mesh1"] = picojson::value(modulation);
          mesh1["MeshConfig"] = picojson::value(mesh_config);
          picojson::object config1;
          config1["Config1"] = picojson::value(mesh1);
          std::stringstream freq_json;
          freq_json << picojson::value(config1);
          std::cout << freq_json.str() << std::endl;

          // Post the config change.
          auto cfg_res = cli.Post("/cfgs.cgi", freq_json.str(), "text/json");

          if (cfg_res) {
            if (cfg_res->status == 200) {
              std::cout << cfg_res->body << std::endl;
            }
          }
          break;
        }
        case radio_control::Action::ACTION_CHANGE_NETWORK_ID: {
          auto id = any::any_cast<std::string>(cmd.item);
          (void)id;
          radio_control::vrc_log(
              "Change network ID not currently implemented.\n");
          break;
        }
        case radio_control::Action::ACTION_CHANGE_PASSWORD: {
          // TODO: How do we set this on the fly?
          auto pw = any::any_cast<std::string>(cmd.item);
          (void)pw;
          radio_control::vrc_log(
              "Change network ID not currently implemented.\n");
          break;
        }
        case radio_control::Action::ACTION_RESTART_SERVICES: {
          radio_control::vrc_log("Restarting radio not currently supported.\n");
          break;
        }
        case radio_control::Action::ACTION_SET_AIR_RATE_IMMEDIATE: {
          auto rate = any::any_cast<int>(cmd.item);
          (void)rate;
          radio_control::vrc_log(
              "ACTION_SET_AIR_RATE_IMMEDIATE not currently implemented on "
              "DTC radio driver.\n");
          break;
        }
        case radio_control::Action::ACTION_SET_AIR_RATE: {
          auto rate = any::any_cast<int>(cmd.item);
          (void)rate;
          radio_control::vrc_log(
              "ACTION_SET_AIR_RATE not currently implemented on DTC radio "
              "driver.\n");
          break;
        }
        case radio_control::Action::ACTION_SET_OUTPUT_POWER: {
          auto dbm = any::any_cast<int>(cmd.item);
          (void)dbm;
          radio_control::vrc_log(
              "ACTION_SET_OUTPUT_POWER not currently implemented on DTC "
              "radio driver.\n");
          break;
        }
        case radio_control::Action::ACTION_SET_OUTPUT_POWER_IMMEDIATE: {
          auto dbm = any::any_cast<int>(cmd.item);
          (void)dbm;
          radio_control::vrc_log(
              "ACTION_SET_OUTPUT_POWER_IMMEDIATE not currently implemented "
              "on DTC radio driver.\n");
          break;
        }
        case radio_control::Action::ACTION_CHANGE_BANDWIDTH_IMMEDIATE: {
          auto band = any::any_cast<int>(cmd.item);
          (void)band;
          radio_control::vrc_log(
              "ACTION_CHANGE_BANDWIDTH_IMMEDIATE not currently implemented "
              "on DTC radio driver.\n");
          break;
        }
        default:
          break;
        }
        config_queue.pop_front();
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
  } // namespace radio_control

  radio_control::vrc_log("Handler exiting.\n");
} // namespace radio_control

// Function returns likely adapter names for us to work with.
// TODO: Move into RadioControl class as a utility function?
std::string DTCControl::GetAdapterName(void) {
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
        system_wrap(std::string(
            "ip addr add 192.168.254.4/16 brd 255.255.255.255 dev " + 
            ifa_name));
        system_wrap(
              std::string("ip route add default dev " + ifa_name));
        system_wrap(
              std::string("ip route add 224.0.0.0/4 dev " + ifa_name));

        // Get mac address of this adapter:
        std::ifstream mac("/sys/class/net/" + ifa_name + "/address");
        std::stringstream mac_buffer;
        mac_buffer << mac.rdbuf();
        std::string detected_ip;
        int detected = DetectDTC(ifa_name, mac_buffer.str(), detected_ip);
        if (detected != -1) {
          radio_control::vrc_log("Found a dtc on: " + ifa_name + "\n");
          dtc_adapter_mac = mac_buffer.str();
          return (ifa_name);
        } else {
          system_wrap(
              std::string("ip route flush dev" + ifa_name));
          system_wrap(
              std::string("ip addr d 192.168.254.4/16 dev " + ifa_name));
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
int DTCControl::DetectDTC(std::string adapter, std::string adapter_mac,
                          std::string &detected_ip) {
  // TODO: Try passing in candidate list directly?
  std::vector<HostAddress> interfaces;
  interfaces.emplace_back(HostAddress(adapter));
  ddp.setLocalOnly(true);
  ddp.setInterfaces(interfaces);
  vrc_log(adapter_mac);
  ByteArray packet = ddp.discoverVantageArray(MacAddress(adapter_mac), true);
  std::vector<uint8_t> data;
  for (size_t i = 0; i < packet.size(); i++) {
    data.emplace_back(packet.data()[i]);
  }
  std::vector<uint8_t> response = radio_control::broadcast_and_listen(
      data, 22484, 22484, 13370, "192.168.254.4", 50, &detected_ip);
  if (response.size()) {
    vrc_log("Discover response size: " + std::to_string(response.size()) +
            "\n");

    DeviceDiscoveryProtocolMessage message;
    message.parse(response, MacAddress(adapter_mac));
    auto dtc_discover = ddp.toMap(message, true);
    dtc_device_mac = dtc_discover.at("macAddress");
    dtc_device_ip = dtc_discover.at("ipAddress");
    dtc_device_esn = dtc_discover.at("esn");
    dtc_device_ip =
        dtc_device_ip.substr(1, dtc_device_ip.size() - 2); // drop ""'s
    uint8_t errorCode = message.getErrorCode();
    uint8_t id = message.getCommandId();
    if (id != DeviceDiscoveryProtocolMessage::DeviceQuery) {
      vrc_log("Received unexpected command response to device query: " +
              std::to_string(id) + "\n");
    }
    if (errorCode != DeviceDiscoveryProtocolMessage::NoError) {
      vrc_log(
          "error: " + std::to_string(errorCode) + " " +
          message.dataToString(DeviceDiscoveryProtocolMessage::ErrorMessage));
    }

    // For debug:
    std::cout << ddp.toJson(message, "    ", true);
    return 1;
  }

  // if (ddp.discover(true)) {
  //   return -1;
  // }
  return -1;
}
#pragma GCC diagnostic pop
#pragma GCC diagnostic pop

std::vector<std::tuple<int, float, int>>
DTCControl::GetSupportedFreqAndMaxBWPer(void) {
  std::vector<std::tuple<int, float, int>> options;
  int step_size = 1;
  // 2100 - 2500
  int min = 2110;
  int max = 2490;
  for (int center_freq = min; center_freq <= max; center_freq += step_size) {
    options.push_back(std::make_tuple(center_freq, 20, (center_freq - max)));
  }
  return options;
}

int DTCControl::SetFrequencyAndBW(int freq, float bw) {
  bool bw_ok, freq_ok = false;
  int bandwidth_index = 0;
  for (auto bw_available : GetSupportedBWs()) {
    if (bw == static_cast<float>(bw_available)) {
      bw_ok = true;
      break;
    }
    bandwidth_index++;
  }

  for (auto freq_avail : GetSupportedFreqAndMaxBWPer()) {
    if (freq == std::get<0>(freq_avail) &&
        bw <= static_cast<float>(std::get<1>(freq_avail))) {
      freq_ok = true;
      break;
    }
  }

  if (freq_ok && bw_ok) {
    QueueItem item = {Action::ACTION_CHANGE_FREQUENCY,
                      std::make_tuple(freq, bw, bandwidth_index)};
    config_queue.push_back(item);
    return 0;
  } else {
    return -1;
  }
}

void DTCControl::SetNetworkID(std::string id) {
  QueueItem item = {Action::ACTION_CHANGE_NETWORK_ID, id};
  config_queue.push_back(item);
}

void DTCControl::SetNetworkPassword(std::string pw) {
  QueueItem item = {Action::ACTION_CHANGE_PASSWORD, pw};
  config_queue.push_back(item);
}

void DTCControl::ApplySettings(void) {
  QueueItem item = {Action::ACTION_RESTART_SERVICES, nullptr};
  config_queue.push_back(item);
}

void DTCControl::SetOutputPower(int dbm) {
  QueueItem item = {Action::ACTION_SET_OUTPUT_POWER, dbm};
  config_queue.push_back(item);
}

} // namespace radio_control
void die(const char *msg);
void die(const char *msg) { (void)msg; }
