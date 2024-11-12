#include "PoplarControl.h"

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

    PoplarControl::PoplarControl(bool central_node)
            : Poplar_address{""},
              keep_running{true},
              is_host{central_node},
              Poplar_adapter{""},
              detected_model{RadioModel::VR_POPLAR},
              factory_reset{false} {
        radio_info.rssi = 0;
        radio_info.noise_floor = 0;
        radio_info.channel_freq = -1;
        radio_info.channel_bw = -1;
        radio_info.tx_power = -1;
        radio_info.network_id = "";
        radio_info.network_password = "";
        radio_info.operation_mode = "";
        radio_info.lan_ok = -1;
        handler = std::make_shared<std::thread>([this] { Handler(); });
    }

    PoplarControl::PoplarControl(bool central_node, std::string adapter,
                                 std::string net_ip)
            : Poplar_address{""},
              keep_running{true},
              is_host{central_node},
              Poplar_adapter{adapter},
              detected_model{RadioModel::VR_POPLAR},
              factory_reset{false},
              config_ip_network{net_ip} {
        radio_info.rssi = 0;
        radio_info.noise_floor = 0;
        radio_info.channel_freq = -1;
        radio_info.channel_bw = -1;
        radio_info.tx_power = -1;
        radio_info.network_id = "";
        radio_info.network_password = "";
        radio_info.operation_mode = "";
        radio_info.lan_ok = -1;
        handler = std::make_shared<std::thread>([this] { Handler(); });
    }

    PoplarControl::~PoplarControl() {
        keep_running = false;
        while (!handler->joinable()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            radio_control::vrc_log("Child thread not joinable.\n");
        }
        handler->join();
    }

    bool PoplarControl::IsConnected(void) {
        if (current_state == RadioState::CONNECTED) {
            return true;
        }
        return false;
    }

    void PoplarControl::Handler(void) {
        // reset:
        current_state = RadioState::UNKNOWN;
        radio_control::vrc_log("Handler active.\n");

        while (keep_running) {
            if (current_state == RadioState::BOOTING ||
                current_state == RadioState::UNKNOWN || Poplar_adapter == "") {
                // Get the name of the Poplar's network adapter.
                while (Poplar_adapter == "" && keep_running) {
                    Poplar_adapter = GetAdapterName();
                    radio_control::vrc_log("Waiting for Poplar adapter.\n");

                    // Only wait if we haven't found anything yet.
                    if (Poplar_adapter == "") {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                    }
                }
                radio_control::vrc_log("Using probable Poplar adapter: " + Poplar_adapter +
                                       "\n");
                current_state = RadioState::BOOTING;

                // Make sure device is up:
                system_wrap(std::string("ip link set " + Poplar_adapter + " up"));
                // Flush ip settings that exist:
                system_wrap(std::string("ip addr flush dev " + Poplar_adapter));

                // Set network IP address from configuration
                // or default to common IPs used lacking configuration.
                Poplar_address = GetNetworkIPAddr();
                std::string Poplar_subnet = get_subnet(Poplar_address);

                // Set our config address.
                system_wrap(std::string("ip addr flush dev " + Poplar_adapter));
                // Add an address on the subnet which will allow us to bind a broadcast to
                // it.
                system_wrap(std::string("ip addr add " + Poplar_address +
                                        "/24 brd " + Poplar_subnet + ".255 dev " + Poplar_adapter));
                system_wrap(std::string("ip link set " + Poplar_adapter + " up"));
                system_wrap(std::string("ip route add " + Poplar_subnet + ".0/16 dev " + Poplar_adapter));
                system_wrap(std::string("ip route add 224.0.0.0/4 dev " + Poplar_adapter));
                // Purge ip rules
                system_wrap(std::string("ip rule flush"));
                system_wrap(std::string("ip rule add from all lookup main"));
                system_wrap(std::string("ip rule add from all lookup default"));
                // TODO: Make this a little more intelligent ^
            }

            int rssi = GetPoplarRSSI(Poplar_adapter);
            if (rssi == -1) {
                // We need to find the adapter again...
                current_state = RadioState::UNKNOWN;
                Poplar_adapter = "";
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
                                    "Change frequency not currently implemented on Poplar radio "
                                    "driver.\n");
                            break;
                        }
                        case radio_control::Action::ACTION_CHANGE_NETWORK_ID: {
                            auto id = any::any_cast<std::string>(cmd.item);
                            std::vector<uint8_t> payload;
                            payload.push_back(static_cast<uint8_t>(
                                                      radio_control::PoplarMGMTActions::MGMT_SET_RADIO_NAME));
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

                            radio_control::send_udp(Poplar_adapter, MGMT_IP, MGMT_PORT, payload);
                            radio_control::vrc_log("Changing network ID / Credentials. len: " +
                                                   std::to_string(payload.size()) + "\n");
                            radio_info.network_id = id;
                            break;
                        }
                        case radio_control::Action::ACTION_SET_RF_TEST_MODE: {
                            auto params = any::any_cast<std::vector<int>>(cmd.item);
                            std::vector<uint8_t> payload;
                            payload.push_back(static_cast<uint8_t>(
                                                      radio_control::PoplarMGMTActions::MGMT_SET_RF_TEST));
                            radio_control::vrc_log("[RF TEST MODE] ACTIVE / INACTIVE: " +
                                                   std::to_string(params.at(0)) + "\n");
                            payload.push_back(static_cast<uint8_t>(params.at(0)));
                            payload.push_back(static_cast<uint8_t>(params.at(1)));
                            payload.push_back(static_cast<uint8_t>(params.at(2)));
                            radio_control::send_udp(Poplar_adapter, MGMT_IP, MGMT_PORT, payload);
                            break;
                        }
                        case radio_control::Action::ACTION_CHANGE_PASSWORD: {
                            auto pw = any::any_cast<std::string>(cmd.item);
                            std::vector<uint8_t> payload;
                            payload.push_back(static_cast<uint8_t>(
                                                      radio_control::PoplarMGMTActions::MGMT_SET_PASSWORD));
                            if (pw.size() > 32) {
                                payload.push_back(32);
                            } else {
                                payload.push_back(pw.size());
                            }

                            // Fill the payload.
                            int count = 0;
                            for (char c : pw) {
                                if (count >= 31) {
                                    break;
                                }
                                payload.push_back(c);
                                count++;
                            }

                            radio_control::send_udp(Poplar_adapter, MGMT_IP, MGMT_PORT, payload);
                            radio_control::vrc_log(
                                    "Setting password after radio reboots.\n");
                            radio_info.network_password = pw;
                            break;
                        }
                        case radio_control::Action::ACTION_RESTART_SERVICES: {
                            current_state = RadioState::CONFIGURING;
                            std::vector<uint8_t> payload;
                            payload.push_back(static_cast<uint8_t>(
                                                      radio_control::PoplarMGMTActions::MGMT_REBOOT));
                            radio_control::send_udp(Poplar_adapter, MGMT_IP, MGMT_PORT, payload);
                            radio_control::vrc_log("Restarting radio.\n");
                            break;
                        }
                        case radio_control::Action::ACTION_SET_AIR_RATE_IMMEDIATE: {
                            auto rate = any::any_cast<int>(cmd.item);
                            std::vector<uint8_t> payload;
                            payload.push_back(static_cast<uint8_t>(
                                                      radio_control::PoplarMGMTActions::MGMT_SET_BW));
                            payload.push_back(static_cast<uint8_t>(rate));

                            radio_control::send_udp(Poplar_adapter, MGMT_IP, MGMT_PORT, payload);
                            radio_control::vrc_log("Changing TX BW to: " + std::to_string(rate) + "\n");
                            break;
                        }
                        case radio_control::Action::ACTION_SET_AIR_RATE: {
                            auto rate = any::any_cast<int>(cmd.item);
                            (void)rate;
                            radio_control::vrc_log(
                                    "ACTION_SET_AIR_RATE not currently implemented on Poplar radio "
                                    "driver.\n");
                            break;
                        }
                        case radio_control::Action::ACTION_SET_OUTPUT_POWER: {
                            auto dbm = any::any_cast<int>(cmd.item);
                            (void)dbm;
                            radio_control::vrc_log(
                                    "ACTION_SET_OUTPUT_POWER not currently implemented on Poplar "
                                    "radio driver.\n");
                            break;
                        }
                        case radio_control::Action::ACTION_SET_OUTPUT_POWER_IMMEDIATE: {
                            auto dbm = any::any_cast<int>(cmd.item);
                            (void)dbm;
                            radio_control::vrc_log(
                                    "ACTION_SET_OUTPUT_POWER_IMMEDIATE not currently implemented "
                                    "on Poplar radio driver.\n");
                            break;
                        }
                        case radio_control::Action::ACTION_CHANGE_BANDWIDTH_IMMEDIATE: {
                            auto band = any::any_cast<int>(cmd.item);
                            (void)band;
                            radio_control::vrc_log(
                                    "ACTION_CHANGE_BANDWIDTH_IMMEDIATE not currently implemented "
                                    "on Poplar radio driver.\n");
                            break;
                        }
                        default:
                            break;
                    }
                    config_queue.pop_front();
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }  // namespace radio_control

        radio_control::vrc_log("Handler exiting.\n");
    }  // namespace radio_control

// Function returns likely adapter names for us to work with.
// TODO: Move into RadioControl class as a utility function?
    std::string PoplarControl::GetAdapterName(void) {
        struct ifaddrs *addresses;
        if (getifaddrs(&addresses) == -1) {
            radio_control::vrc_log("getifaddrs call failed\n");
            return "";
        }

        // Set network IP address from configuration
        // or default to common IPs used lacking configuration.
        std::string ip_address = GetNetworkIPAddr();

        std::vector<std::string> likely_adapter_names;
        likely_adapter_names.push_back("usb");
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
                            "ip addr add " + ip_address + "/16 brd 255.255.255.255 dev " +
                            ifa_name));
                    int detected = GetPoplarRSSI(name);
                    if (detected != -1) {
                        radio_control::vrc_log("Found a Poplar on: " + ifa_name + "\n");
                        return (ifa_name);
                    } else {
                        system_wrap(
                                std::string("ip addr d " + ip_address + "/16 dev " + ifa_name));
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
    int PoplarControl::GetPoplarRSSI(std::string adapter) {
        (void)adapter;
        struct sockaddr_in rxaddr;
        int rx_sock;

        if ((rx_sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
            radio_control::vrc_log("Can't open receive socket\n");
            char buffer[256];
            strerror_r(errno, buffer, 256);
            std::string error(buffer);
            radio_control::vrc_log(error + "\n");
            return -1;
        }

        int one = 1;
        if (setsockopt(rx_sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0) {
            close(rx_sock);
            radio_control::vrc_log("Can't set sockopt\n");
            char buffer[256];
            strerror_r(errno, buffer, 256);
            std::string error(buffer);
            radio_control::vrc_log(error + "\n");
            return -1;
        }

        // Add a 1100mS timeout.
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 1100000;
        setsockopt(rx_sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof tv);

        // Add a 2S timeout for select.
        // Using the usec timeout field misbehaves on android...
        struct timeval tv2;
        tv2.tv_sec = 2;
        tv2.tv_usec = 0;

        std::string ip_address = GetNetworkIPAddr();

        memset(&rxaddr, 0, sizeof(rxaddr));
        rxaddr.sin_family = AF_INET;
        rxaddr.sin_addr.s_addr = inet_addr(ip_address.c_str());
        rxaddr.sin_port = htons(18889);

        /* Bind to the broadcast port */
        if (bind(rx_sock, (struct sockaddr *)&rxaddr, sizeof(rxaddr)) < 0) {
            close(rx_sock);
            radio_control::vrc_log("Can't bind receive socket\n");
            char buffer[256];
            strerror_r(errno, buffer, 256);
            std::string error(buffer);
            radio_control::vrc_log(error + "\n");
            return -1;
        }

        struct sockaddr_in fromradio;
        socklen_t addrlen = sizeof(struct sockaddr_in);
        char buffer[256];
        int received = -1;
        memset(&fromradio, 0, sizeof(fromradio));

        fd_set rset;
        // Needed for select
        FD_ZERO(&rset);
        FD_SET(rx_sock, &rset);

//#ifndef __ANDROID__
        if (select(rx_sock + 1, &rset, NULL, NULL, &tv2) == 1) {
            if (FD_ISSET(rx_sock, &rset)) {
                // RX!
//#endif
                received = recvfrom(rx_sock, buffer, 256, 0,
                                    (struct sockaddr *)&fromradio, &addrlen);
//#ifndef __ANDROID__
            }
        } else {
            radio_control::vrc_log("Select timeout.\n");
            received = -1;
        }
//#endif

        // TODO: We will never see more than -23 dBm on the output of the radio
        // but there is some ambiguity on the return values here...
        if (received < 0) {
            close(rx_sock);
            return -1;
        } else {
            std::string payload(buffer);
            std::string ip(inet_ntoa(fromradio.sin_addr));
            radio_control::vrc_log("Response from: " + ip + " : " + payload + "\n");
            try {
                auto val = get_str_between_two_str(payload, "LOCAL RSSI IS: ", " dBm");
                if (val.size()) {
                    radio_info.rssi = std::stoi(val);
                    radio_control::vrc_log("Val: " + std::to_string(radio_info.rssi) +
                                           "\n");
                    close (rx_sock);
                    return radio_info.rssi;
                }
            } catch (...) {
                std::exception_ptr p = std::current_exception();
                // Doesn't compile on NDK...
                // radio_control::vrc_log((p ? p.__cxa_exception_type()->name() : "null")+
                // "\n");
            }
            radio_control::vrc_log("Issue getting rssi / status.\n");
            close(rx_sock);
            return -1;
        }
        radio_control::vrc_log("Unexpected return in GetPoplarRSSI.\n");
        close(rx_sock);
        return -1;
    }
#pragma GCC diagnostic pop
#pragma GCC diagnostic pop

    std::vector<std::tuple<int, float, int>>
    PoplarControl::GetSupportedFreqAndMaxBWPer(void) {
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

    void PoplarControl::SetRateImmediate(int rate) {
        QueueItem item = {Action::ACTION_SET_AIR_RATE_IMMEDIATE, rate};
        config_queue.push_back(item);
    }

    int PoplarControl::SetFrequencyAndBW(int freq, float bw) {
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

    void PoplarControl::SetNetworkID(std::string id) {
        QueueItem item = {Action::ACTION_CHANGE_NETWORK_ID, id};
        config_queue.push_back(item);
    }

    void PoplarControl::EnableRFTestMode(int enable, int power, int channel) {
        QueueItem item = {Action::ACTION_SET_RF_TEST_MODE, std::vector<int>({enable, power, channel})};
        config_queue.push_back(item);
    }

    void PoplarControl::SetNetworkPassword(std::string pw) {
        QueueItem item = {Action::ACTION_CHANGE_PASSWORD, pw};
        config_queue.push_back(item);
    }

    void PoplarControl::SetNetworkIPAddr(std::string ipaddr) {
        (void)ipaddr;
    }

    void PoplarControl::SetRadioIPAddr(std::string radio_ipaddr) {
        (void)radio_ipaddr;
    }

    void PoplarControl::SetTopology(Topology topology) {
        (void)topology;
        // only supports STATION topology
    }

    void PoplarControl::ApplySettings(void) {
        QueueItem item = {Action::ACTION_RESTART_SERVICES, nullptr};
        config_queue.push_back(item);
    }

    void PoplarControl::SetOutputPower(int dbm) {
        QueueItem item = {Action::ACTION_SET_OUTPUT_POWER, dbm};
        config_queue.push_back(item);
    }

    std::string PoplarControl::GetNetworkIPAddr(void) {
        std::string ip_address;
        if (!config_ip_network.empty()) {
            ip_address = config_ip_network;
        } else {
            if (is_host) {
                ip_address = "192.168.20.4";
            } else {
                ip_address = "192.168.20.30";
            }
        }
        return ip_address;
    }

}  // namespace radio_control

// TODO: Check LAN
// TODO: Change Poplar IP if in factory configuration.
// TODO: Make sure that LAN does not have DHCP enabled
// TODO: Make sure that LAN's default gateway is correct
// TODO: Check MWVMODE and set to MESH
// AT+MNLAN="lan",EDIT,0,192.168.20.20,255.255.255.0,0
// TODO: Drones get 5-99, GCS gets 20.4, and all Poplar static IPs are 20.104
// - 20.199 If GCS, you're 20.4, and local Poplar should be 104. If drone
// you'll start out as 20.5, with Poplar at 105. If there's a conflict /
// there are already systems on the network, it will change the local Poplar
// IP.

// to get encryption key: AT+MWVENCRYPT?
