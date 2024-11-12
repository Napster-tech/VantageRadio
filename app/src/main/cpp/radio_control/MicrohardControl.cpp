#include "MicrohardControl.h"

#include <arpa/inet.h>
#include <errno.h>
#ifdef __ANDROID__
#include "ifaddrs.h"
#else
#include <ifaddrs.h>
#endif
#include <algorithm>
#include <fstream>
#include <iostream>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <numeric>
#include <random>
#include <regex>
#include <sstream>
#include <sys/socket.h>
#include <unordered_map>

#include <chrono>
#include <fcntl.h>
#include <string.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace radio_control {

// Feature support data
    typedef struct {
        RadioFeature      feature;
        uint16_t          hardware_ver;
        uint16_t          software_ver[4];
    } FeatureSupport;

    static FeatureSupport PMDDL245O_FeatureSupport[] {
            { FEATURE_BUFFER_SIZE, 1, {999, 0, 0, 0} },
            { FEATURE_CHANNEL_HOP, 1, {999, 0, 0, 0} },
            { FEATURE_CHANNEL_SCAN, 1, {1, 4, 0, 1036} },
            { FEATURE_FREQUENCY_LIST, 1, {999, 0, 0, 0} },
            { FEATURE_MESH, 1, {1, 4, 0, 1026} },
            { FEATURE_RATE_FLOOR, 1, {999, 0, 0, 0} },
            { FEATURE_FIRMWARE_UPGRADE_FTP, 1, {1, 4, 0, 1024} },
            { FEATURE_FIRMWARE_UPGRADE_SFTP, 1, {999, 0, 0, 0} },
            { FEATURE_BUFFER_SIZE, 2, {1, 3, 5, 1040} },
            { FEATURE_CHANNEL_HOP, 2, {1, 3, 5, 1044} },
            { FEATURE_CHANNEL_SCAN, 2, {1, 3, 5, 1040} },
            { FEATURE_FREQUENCY_LIST, 2, {1, 3, 5, 1040} },
            { FEATURE_MESH, 2, {1, 3, 5, 1026} },
            { FEATURE_RATE_FLOOR, 2, {1, 3, 5, 1044} },
            { FEATURE_FIRMWARE_UPGRADE_FTP, 2, {1, 3, 5, 1024} },
            { FEATURE_FIRMWARE_UPGRADE_SFTP, 2, {1, 3, 5, 1025} },
            { FEATURE_CORE, 1, {1, 0, 0, 0} },
    };

    static FeatureSupport PMDDL162x_FeatureSupport[] {
            { FEATURE_BUFFER_SIZE, 2, {1, 3, 5, 1040} },
            { FEATURE_CHANNEL_HOP, 2, {1, 3, 5, 1044} },
            { FEATURE_CHANNEL_SCAN, 2, {1, 3, 5, 1044} },
            { FEATURE_FREQUENCY_LIST, 2, {1, 3, 5, 1040} },
            { FEATURE_MESH, 2, {1, 3, 5, 1026} },
            { FEATURE_RATE_FLOOR, 2, {1, 3, 5, 1044} },
            { FEATURE_FIRMWARE_UPGRADE_FTP, 2, {1, 3, 5, 1024} },
            { FEATURE_FIRMWARE_UPGRADE_SFTP, 2, {1, 3, 5, 1025} },
            { FEATURE_CORE, 1, {1, 0, 0, 0} },
    };

    static FeatureSupport PMDDL1800_FeatureSupport[] {
            { FEATURE_BUFFER_SIZE, 1, {999, 0, 0, 0} },
            { FEATURE_CHANNEL_HOP, 1, {999, 0, 0, 0} },
            { FEATURE_CHANNEL_SCAN, 1, {1, 3, 0, 1024} },
            { FEATURE_FREQUENCY_LIST, 1, {999, 0, 0, 0} },
            { FEATURE_MESH, 1, {1, 3, 0, 1026} },
            { FEATURE_RATE_FLOOR, 1, {999, 0, 0, 0} },
            { FEATURE_FIRMWARE_UPGRADE_FTP, 1, {1, 3, 0, 1024} },
            { FEATURE_FIRMWARE_UPGRADE_SFTP, 1, {999, 0, 0, 0} },
    };

    static const char *find_needle(const char *haystack, size_t haystack_length,
                                   const char *needle, size_t needle_length) {
        for (size_t haystack_index = 0; haystack_index < haystack_length;
             haystack_index++) {
            bool needle_found = true;
            for (size_t needle_index = 0; needle_index < needle_length;
                 needle_index++) {
                const auto haystack_character = haystack[haystack_index + needle_index];
                const auto needle_character = needle[needle_index];
                if (haystack_character == needle_character) {
                    continue;
                } else {
                    needle_found = false;
                    break;
                }
            }

            if (needle_found) {
                return &haystack[haystack_index];
            }
        }

        return nullptr;
    }

    MicrohardControl::MicrohardControl(bool central_node)
            : microhard_address{""}, keep_running{true}, is_host{central_node},
              microhard_adapter{""}, microhard_subnet{"192.168.20"},
              detected_model{RadioModel::UNKNOWN}, radio_variance{RadioVariance::NONE},
              factory_reset{false}, config_ip_network{""}, config_ip_radio{""} {

        radio_info.rssi = 0;
        radio_info.noise_floor = 0;
        radio_info.channel_freq = -1;
        radio_info.channel_bw = -1;
        radio_info.tx_power = -1;
        radio_info.tx_powerq = -1;
        radio_info.tx_modulation = -1;
        radio_info.network_id = "";
        radio_info.network_password = "";
        radio_info.operation_mode = "";
        radio_info.lan_ok = -1;
        radio_info.freq_hop_mode = -1;
        radio_info.freq_hop_stat = -1;
        use_mesh = false; // Must be able to update G1 microhards for compatibility
        // with G2 microhards before we can turn on mesh mode.
        topology_change = 0;

        current_state = RadioState::UNKNOWN;

        handler = std::make_shared<std::thread>([this] { Handler(); });
    }

    MicrohardControl::MicrohardControl(bool central_node, std::string adapter,
                                       std::string mh_ip, std::string net_ip)
            : microhard_address{""}, keep_running{true}, is_host{central_node},
              microhard_adapter{adapter}, detected_model{RadioModel::UNKNOWN},
              radio_variance{RadioVariance::NONE}, factory_reset{false},
              config_ip_network{net_ip}, config_ip_radio{mh_ip} {
        microhard_subnet = (!mh_ip.empty()) ? get_subnet(mh_ip) : "192.168.20";
        radio_info.rssi = 0;
        radio_info.noise_floor = 0;
        radio_info.channel_freq = -1;
        radio_info.channel_bw = -1;
        radio_info.tx_power = -1;
        radio_info.tx_powerq = -1;
        radio_info.tx_modulation = -1;
        radio_info.network_id = "";
        radio_info.network_password = "";
        radio_info.operation_mode = "0";
        radio_info.lan_ok = -1;
        radio_info.freq_hop_mode = -1;
        radio_info.freq_hop_stat = -1;
        use_mesh = false; // Must be able to update G1 microhards for compatibility
        // with G2 microhards before we can turn on mesh mode.
        topology_change = 0;

        handler = std::make_shared<std::thread>([this] { Handler(); });
    }

    MicrohardControl::~MicrohardControl() {
        keep_running = false;
        while (!handler->joinable()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            radio_control::vrc_log("Child thread not joinable.\n");
        }
        handler->join();
    }

    bool MicrohardControl::SupportsFeature(RadioFeature feature) const {
        static std::unordered_map<RadioModel, FeatureSupport*> feature_support_map = {
                { RadioModel::PMDDL1621, PMDDL162x_FeatureSupport },
                { RadioModel::PMDDL1622, PMDDL162x_FeatureSupport },
                { RadioModel::PMDDL1624, PMDDL162x_FeatureSupport },
                { RadioModel::FDDL1624,  PMDDL162x_FeatureSupport },
                { RadioModel::PMDDL2450, PMDDL245O_FeatureSupport },
                { RadioModel::PDDL1800, PMDDL1800_FeatureSupport  }
        };

        // Core features always true
        if (feature == FEATURE_CORE) {
            return true;
        }

        // Radio has not scanned version informtaion
        if (radio_info.hardware_version.empty() ||
            radio_info.software_version.empty()) {
            return false;
        }

        // Hardware version
        uint16_t hw_version = 0;
        if (radio_info.hardware_version.empty()) {
            hw_version = 1;
        } else if (radio_info.hardware_version.find("Rev") == 0) {
            hw_version = 1;
        } else {
            hw_version = static_cast<uint16_t>(std::atof(radio_info.hardware_version.c_str()));
        }

        if (feature_support_map.find(detected_model) != feature_support_map.end()) {
            int index = 0;
            FeatureSupport* model_support = feature_support_map[detected_model];
            FeatureSupport* feature_support = &model_support[index++];
            while (feature_support->feature != FEATURE_CORE) {
                if (feature_support->feature == feature &&
                    feature_support->hardware_ver == hw_version) {
                    if ((software_build[0] > feature_support->software_ver[0]) ||
                        (software_build[0] == feature_support->software_ver[0] &&
                         software_build[1] > feature_support->software_ver[1]) ||
                        (software_build[0] == feature_support->software_ver[0] &&
                         software_build[1] == feature_support->software_ver[1] &&
                         software_build[2] > feature_support->software_ver[2])) {
                        return true;
                    }

                    if (software_build[0] == feature_support->software_ver[0] &&
                        software_build[1] == feature_support->software_ver[1] &&
                        software_build[2] ==  feature_support->software_ver[2] &&
                        software_build[3] >= feature_support->software_ver[3]) {
                        return true;
                    }
                }
                feature_support = &model_support[index++];
            }
        }

        return false;
    }

    bool MicrohardControl::SupportsFrequencyHopping(void) const {
        bool supported = false;
        // The master is not host any longer
        // Channel hoppping only functional from master
        if (!is_host)
            supported = SupportsFeature(FEATURE_CHANNEL_HOP);
        else
            supported = SupportsFeature(FEATURE_FREQUENCY_LIST);

        return supported;
    }

    int MicrohardControl::GetMaxPower() {
        if (detected_model == RadioModel::PMDDL900) {
            return 30;
        } else if (detected_model == RadioModel::PDDL1800) {
            return 30;
        } else if (detected_model == RadioModel::PMDDL2450) {
            return 30;
        } else if ((detected_model == RadioModel::PMDDL1624) ||
                   (detected_model == RadioModel::FDDL1624)) {
            // Multi-band radios use the actual frequency, not a "channel" number.
            return 30;
        }
        return 30;
    }

    std::future<std::vector<int>>
    MicrohardControl::GetLowInterferenceFrequencyList(int count, float bw) {
        if (detected_model != RadioModel::PDDL1800) {
            int sort = 2;   // sort on second column (LVL_MAX)
            auto future_result = std::async(std::launch::async,
                                            &MicrohardControl::ScanChannels, this, sort, count, bw);
            return future_result;
        } else {
            static int band = 0;
            auto future_result = std::async(std::launch::async,
                                            &MicrohardControl::ScanChannels1800, this, band, count, bw);
            band = (band + 1) % 3;  // cycle between 1.8GHz three bands 0,1,2
            return future_result;
        }
    }

    void MicrohardControl::Handler(void) {
        std::string configured_ip("");
        std::string password("microhardsrm");
        current_state = RadioState::UNKNOWN;
        int login_attempts = 0;

        handler_top:
        radio_control::vrc_log("Handler active.\n");
        // Temporary ip expected to not be in use.
        std::string local_ip("192.168.254.4");

        // Clear breadcrumb before starting discovery over
        Breadcrumb(false);

        // Get the name of the Microhard's network adapter.
        std::vector<std::string> ips;
        while (microhard_adapter == "" && keep_running) {
            // This is the first opportunity to get confirmation that we've detected
            // the correct local microhard.
            // After this returns, the microhard adapter should have its ip config set
            // up.
            microhard_adapter = GetAdapterName(ips);

            // Only wait if we haven't found anything yet.
            if (microhard_adapter == "") {
                radio_control::vrc_log("Waiting for Microhard adapter.\n");
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
        }

        radio_control::vrc_log(
                "Using probable Microhard adapter: " + microhard_adapter + "\n");
        current_state = RadioState::BOOTING;

        // if we couldn't detect the microhard state from the initial discovery
        // fall back to older methods.
        std::vector<std::string> final_ip_list;
        {
            int retries = 5;
            while (microhard_address == "" && retries > 0) {
                // Execute a broadcast which the Microhard will respond to, this will tell
                // us its ip addresses. microhard_adapter arg not currently used.
                final_ip_list.clear();
                bool only_one = false;
                auto result = GetMicrohardIPs(microhard_adapter);
                if (result.size() >= 2) {
                    microhard_address = SelectMicrohardIP(result, configured_ip);
                } else if (result.size() == 1) {
                    // If
                    if (result.at(0) == "192.168.20.104") {
                        microhard_address = SelectMicrohardIP(result, configured_ip);
                        only_one = true;
                    }
                }
                // keep trying to get Microhard IP response.
                while ((final_ip_list.size() < 2) && keep_running && !only_one) {
                    // Now parse the previous result
                    if (result.size() > final_ip_list.size()) {
                        final_ip_list = result;
                    }

                    // Sample multiple times before moving forward with an
                    // "official" count of how many IPs are on the network.
                    for (int i = 0; i < 3; i++) {
                        vrc_log("Getting IPs " + std::to_string(i) + "\n");
                        auto result = GetMicrohardIPs(microhard_adapter);
                        if (result.size() > final_ip_list.size()) {
                            final_ip_list = result;
                        }
                        if (final_ip_list.size() >= 2) {
                            break;
                        }
                    }
                    if (final_ip_list.size() == 1) {
                        only_one = true;
                    }

                    if (!keep_running) {
                        return;
                    }

                    // Skip the wait if we've found an IP.
                    if (final_ip_list.size() < 1) {
                        vrc_log("Issue getting adapter IPs with discovery service... Will "
                                "retry.\n");
                        std::this_thread::sleep_for(std::chrono::milliseconds(500));
                        continue;
                    }

                    microhard_address = SelectMicrohardIP(final_ip_list, configured_ip);

                    if (!configured_ip.empty() && microhard_address.empty()) {
                        --retries;
                        std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    }
                }
            }
        }

        // If in factory default or IP misconfigured, then perform
        // reconfiguration to put host on <subnet>.104 and others
        // on <subnet>.105+.
        if (microhard_address != "") {
            // If default IP and IP not configured then try default password first
            if (microhard_address == "192.168.168.1" && configured_ip.empty()) {
                // In factory default, must reset values.
                if (login_attempts == 0)
                    password = "admin";
                factory_reset = true;
                radio_control::vrc_log(
                        "In factory default configuration. Configure radio.\n");
            }
            if (!config_ip_radio.empty()) {
                if (microhard_address != config_ip_radio) {
                    factory_reset = true;
                    radio_control::vrc_log("Radio IP does not match configuration, "
                                           "requires re-configuration.\n");
                }
            } else {
                if (((microhard_address == microhard_subnet + ".104") && !is_host) ||
                    ((microhard_address != microhard_subnet + ".104") && is_host)) {
                    factory_reset = true;
                    radio_control::vrc_log("Radio IP does not match default configuration, "
                                           "requires re-configuration.\n");
                }
            }
        }

        configured_ip.clear();
        radio_control::vrc_log("Microhard address " + microhard_address + "\n");

        // verify radio and network IP requested are in same subnet
        struct sockaddr_in sa_radio_ip_config;
        inet_pton(AF_INET, microhard_address.c_str(), &(sa_radio_ip_config.sin_addr));
        sa_radio_ip_config.sin_addr.s_addr &= 0x00FFFFFF;

        // IP address structs.
        // store this IP address in sa:
        struct sockaddr_in sa_local_ip_config;
        inet_pton(AF_INET, config_ip_network.c_str(), &(sa_local_ip_config.sin_addr));
        sa_local_ip_config.sin_addr.s_addr &= 0x00FFFFFF;

        if (!config_ip_network.empty() && sa_local_ip_config.sin_addr.s_addr ==
                                          sa_radio_ip_config.sin_addr.s_addr) {
            local_ip = config_ip_network;
        } else if (factory_reset && microhard_address == "192.168.168.1") {
            local_ip = std::string("192.168.168.10");
        } else {
            // Fallback to using default IP in same subnet as radio IP
            // Set initial config local ip's last octet based on configuration.
            // TODO: Use the mesh networking ip logic we defined.
            uint8_t ip = (is_host) ? 4 : 30;
            char str[INET_ADDRSTRLEN];
            sa_local_ip_config = sa_radio_ip_config;
            sa_local_ip_config.sin_addr.s_addr += (((uint32_t)ip) << 24);
            inet_ntop(AF_INET, &(sa_local_ip_config.sin_addr), str, INET_ADDRSTRLEN);
            local_ip = std::string(str);
        }
        network_address = local_ip;

        // now get it back and print it
        radio_control::vrc_log("Microhard local ip " + local_ip + "\n");

        // Set our config address.
        system_wrap(std::string("ip addr flush dev " + microhard_adapter));
        // Add an address on the subnet which will allow us to bind a broadcast to it.
        system_wrap(std::string("ip addr add " + local_ip + "/24 brd " +
                                microhard_subnet + ".255 dev " + microhard_adapter));
        system_wrap(std::string("ip link set " + microhard_adapter + " up"));
        system_wrap(std::string("ip route add 224.0.0.0/4 dev " + microhard_adapter));
        // Purge ip rules
        // See if we can get away with removing this flush operation as it can cause
        // other configs to get screwed up. system_wrap(std::string("ip rule flush"));
        system_wrap(std::string("ip rule add from all lookup main"));
        system_wrap(std::string("ip rule add from all lookup default"));
        // TODO: Make this a little more intelligent ^

        // Determine how many other radios are on the mesh before assigning an ip
        // address. We will determine what IP to use based on if we're the second,
        // third, or nth on the network. GCS will have a static ip of .4
        // TODO: Currently not implemented on MH!
        bool ip_determined = true;
        // if (is_host) {
        //   ip_determined = true;
        // }

        // Always need to login first.
        MonitorMicrohardState state;
        state = MonitorMicrohardState::LOGIN;
        sock = socket(AF_INET, SOCK_STREAM, 0);
        bool logged_in = false;
        bool connected = false;
        int adapter_not_present = 0;

        char buffer[1024] = {};
        while (keep_running) {
            MonitorMicrohardState state_prev = state;
            (void)state_prev;

            // Check if connection has been dropped by looking for our adapter.
            if (!radio_control::is_adapter_present(microhard_adapter)) {
                radio_control::vrc_log("Radio adapter: " + microhard_adapter +
                                       " no longer present, resetting handler.");
                // Only reset if lack of adapter present persists
                if (++adapter_not_present > 4) {
                    microhard_adapter = "";
                    microhard_address = "";
                    detected_model = RadioModel::UNKNOWN;
                    current_state = RadioState::REMOVED;
                    factory_reset = false;
                    login_attempts = 0;
                    adapter_not_present = 0;
                    radio_info.channel_freq = -1;
                    radio_info.channel_bw = -1;
                    radio_info.tx_power = -1;
                    radio_info.tx_powerq = -1;
                    radio_info.tx_modulation = -1;
                    radio_info.network_id = "";
                    radio_info.network_password = "";
                    radio_info.operation_mode = "";
                    radio_info.radio_name = "";
                    radio_info.lan_ok = -1;
                    radio_info.freq_hop_mode = -1;
                    radio_info.freq_hop_stat = -1;

                    if (sock) {
                        logged_in = false;
                        connected = false;
                        close(sock);
                        sock = 0;
                    }
                    goto handler_top;
                }
            }

            if (sock > 0) {
                if (!connected) {
                    radio_control::vrc_log("Connecting socket.\n");
                    if (!SocketConnect(sock, microhard_address)) {
                        radio_control::vrc_log("socket not connected\n");
                        state = MonitorMicrohardState::LOGIN;
                        close(sock);
                        sock = 0;
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        continue;
                    } else {
                        connected = true;
                        radio_control::vrc_log("socket connected\n");
                    }
                }
            } else {
                radio_control::vrc_log("Creating socket.\n");
                sock = socket(AF_INET, SOCK_STREAM, 0);
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            memset(buffer, 0, sizeof(buffer));
            ssize_t n = read(sock, buffer, sizeof(buffer));
            if (n <= 1) {
                if (n == 0) {
                    radio_control::vrc_log("Orderly TCP disconnect detected.\n");
                    shutdown(sock, SHUT_RDWR);
                    close(sock);
                    sock = 0;

                    // Could happen as result of configuration reset as
                    // Microhard IP address was changed.
                    logged_in = false;
                    connected = false;
                    microhard_adapter = "";
                    microhard_address = "";
                    detected_model = RadioModel::UNKNOWN;
                    current_state = RadioState::REMOVED;
                    factory_reset = false;
                    radio_info.channel_freq = -1;
                    radio_info.channel_bw = -1;
                    radio_info.tx_power = -1;
                    radio_info.tx_powerq = -1;
                    radio_info.tx_modulation = -1;
                    radio_info.network_id = "";
                    radio_info.network_password = "";
                    radio_info.operation_mode = "";
                    radio_info.lan_ok = -1;
                    radio_info.freq_hop_mode = -1;
                    radio_info.freq_hop_stat = -1;
                    goto handler_top;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }

            std::string output;
            std::string command;
            output += buffer;

            // Parse status responses we're interested in.
            ParseStatus(output);

            // Change state from booting to disconnected after we've gotten all of our
            // data parsed.
            //        (radio_info.network_password == "") ||
            if ((radio_info.channel_freq == -1) || (radio_info.channel_bw == -1) ||
                (radio_info.tx_power == -1) || (radio_info.tx_powerq == -1) ||
                (radio_info.operation_mode == "") || (radio_info.software_version == "") ||
                (radio_info.lan_ok == -1) || (radio_info.radio_name == "") ||
                (SupportsFrequencyHopping() && radio_info.freq_hop_stat == -1)) {
                if (current_state != RadioState::CONFIGURING) {
                    current_state = RadioState::BOOTING;
                }
                radio_control::vrc_log("Modem booting.\n");
                if (radio_info.channel_freq == -1) {
                    radio_control::vrc_log("Freq not detected.\n");
                }
                if (radio_info.channel_bw == -1) {
                    radio_control::vrc_log("BW not detected.\n");
                }
                if (radio_info.tx_power == -1 || radio_info.tx_powerq == -1) {
                    radio_control::vrc_log("Power not detected.\n");
                }
                if (radio_info.operation_mode == "") {
                    radio_control::vrc_log("Operation mode not detected.\n");
                }
                if (radio_info.software_version == "") {
                    radio_control::vrc_log("Firmware version not detected.\n");
                }
                if (radio_info.lan_ok == -1) {
                    radio_control::vrc_log("LAN settings not detected.\n");
                }
                if (radio_info.freq_hop_stat == -1 && SupportsFrequencyHopping()) {
                    radio_control::vrc_log("Frequency hopping settings not detected.\n");
                }
                radio_control::vrc_log("Current state: " + std::to_string((int)state) + "\n");
            } else {
                if (current_state == RadioState::BOOTING) {
                    current_state = RadioState::DISCONNECTED;
                }
                if (current_state == RadioState::DISCONNECTED) {
                    radio_control::vrc_log("Modem booted.\n");
                }
            }

            static bool reset_attempted = false;
            // If we need to fix the radio config right off the bat, let's do so as
            // soon as the startup checks have completed.
            if (current_state != RadioState::BOOTING && reset_attempted == false) {
                bool issue = false;
                if (radio_info.lan_ok == 0) {
                    issue = true;
                    radio_control::vrc_log("Lan not ok.\n");
                }
                if (use_mesh) {
                    if ((radio_info.operation_mode.find("Mesh") == std::string::npos) &&
                        radio_info.operation_mode.length()) {
                        issue = true;
                        radio_control::vrc_log(
                                "operation mode not ok: " + radio_info.operation_mode + "\n");
                    }
                } else {
                    if (is_host) {
                        if ((radio_info.operation_mode.find("Slave") == std::string::npos) &&
                            radio_info.operation_mode.length()) {

                            radio_control::vrc_log(
                                    "operation mode ok: " + radio_info.operation_mode + "\n");
                        }
                    } else {
                        if ((radio_info.operation_mode.find("Slave") == std::string::npos) &&
                            radio_info.operation_mode.length()) {

                            radio_control::vrc_log(
                                    "operation mode ok: " + radio_info.operation_mode + "\n");
                        }
                    }
                    // We only ever will reset the name of a radio if there's only one ip
                    // on the network, we don't want to do this in a connected state, just
                    // in case we selected the wrong radio for some reason.
                    // TODO: All factory resets should be guarded programmatically?
                    // TODO: If a factory reset is required we should force both sides to
                    // reset but first turn off thier radios, and then fully reset the
                    // driver? As of now final_ip_list will be of size 0 if we detected
                    // the adapter in the right config the first time we detected the
                    // radio.
                    if (final_ip_list.size() < 2) {
                        if (is_host &&
                            (radio_info.radio_name.find("CENTRAL") == std::string::npos) &&
                            radio_info.radio_name.length()) {
                            issue = true;
                            radio_control::vrc_log("radio should be named CENTRAL but is: " +
                                                   radio_info.radio_name + "\n");
                        }
                        if (!is_host &&
                            (radio_info.radio_name.find("REMOTE") == std::string::npos) &&
                            radio_info.radio_name.length()) {
                            issue = true;
                            radio_control::vrc_log("radio should be named REMOTE but is: " +
                                                   radio_info.radio_name + "\n");
                        }
                    }
                }
                if (issue) {
                    radio_control::vrc_log("Requesting reset due to misconfiguration.\n");
                    factory_reset = true;
                    radio_info.lan_ok = -1;
                }
            }

            switch (state) {
                case MonitorMicrohardState::LOGIN: {
                    if (output.find("login:") != std::string::npos) {
                        command = "admin\n";
                        if (send(sock, command.c_str(), command.length(), 0) == -1) {
                            connected = false;
                            state = MonitorMicrohardState::LOGIN;
                            break;
                        }
                        state = MonitorMicrohardState::PASSWORD;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    break;
                }

                case MonitorMicrohardState::PASSWORD: {
                    if (output.find("Password:") != std::string::npos) {
                        command = password + "\n";
                        if (send(sock, command.c_str(), command.length(), 0) == -1) {
                            connected = false;
                            state = MonitorMicrohardState::LOGIN;
                            break;
                        }
                        state = MonitorMicrohardState::MODEM_MODEL;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    break;
                }

                case MonitorMicrohardState::MODEM_MODEL: {
                    if (output.find("Enter") != std::string::npos) {
                        state = MonitorMicrohardState::MODEM_SUMMARY;
                        command = "AT+MSSYSI\n";
                        if (send(sock, command.c_str(), command.length(), 0) == -1) {
                            connected = false;
                            state = MonitorMicrohardState::LOGIN;
                        }
                    } else if (output.find("incorrect") != std::string::npos) {
                        if (password == "microhardsrm") {
                            password = "$wApp@bleRadioModul3";
                        } else if (password == "$wApp@bleRadioModul3") {
                            password = "vantage";
                        } else if (password == "vantage") {
                            password = "admin";
                        } else if (password == "admin") {
                            password = "microhardsrm";
                        }
                        login_attempts += 1;
                        // TODO: Handle incorrect password.
                        state = MonitorMicrohardState::LOGIN;
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    } else if (output.find("can't connect") != std::string::npos) {
                        // telnet refused connection, try again
                        state = MonitorMicrohardState::LOGIN;
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                    break;
                }

                case MonitorMicrohardState::MODEM_SUMMARY: {
                    command = "AT+MSSYSI\n";
                    if (send(sock, command.c_str(), command.length(), 0) == -1) {
                        close(sock);
                        sock = 0;
                        connected = false;
                        state = MonitorMicrohardState::LOGIN;
                    }
                    if (output.find("+MSSYSI:")) {
                        _summary = output;
                        ParseSummary(_summary);
                        // If we didn't get our radio name, get the system summary again.
                        if (radio_info.radio_name == "") {
                            state = MonitorMicrohardState::MODEM_SUMMARY;
                            command = "AT+MSSYSI\n";
                            if (send(sock, command.c_str(), command.length(), 0) == -1) {
                                close(sock);
                                sock = 0;
                                connected = false;
                                state = MonitorMicrohardState::LOGIN;
                            }
                        } else {
                            // If we're an pmddl2450 we annoyingly need to find out if we
                            // are a japan variant or not.
                            if (detected_model == RadioModel::PMDDL2450) {
                                state = MonitorMicrohardState::MODEM_CHECK_FOR_VARIANCE;
                                command = "AT+MWFREQ2400=?\n";
                                if (send(sock, command.c_str(), command.length(), 0) == -1) {
                                    sock = 0;
                                    connected = false;
                                    state = MonitorMicrohardState::LOGIN;
                                }
                            } else {
                                state = MonitorMicrohardState::MODEM_SETTINGS;
                                command = "AT+MWSTATUS\n";
                                if (send(sock, command.c_str(), command.length(), 0) == -1) {
                                    close(sock);
                                    sock = 0;
                                    connected = false;
                                    state = MonitorMicrohardState::LOGIN;
                                }
                            }
                        }
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    break;
                }

                case MonitorMicrohardState::MODEM_CHECK_FOR_VARIANCE: {
                    // Execute LAN check after asking for list of channels.
                    state = MonitorMicrohardState::MODEM_SETTINGS;
                    command = "AT+MWSTATUS\n";
                    if (send(sock, command.c_str(), command.length(), 0) == -1) {
                        close(sock);
                        sock = 0;
                        connected = false;
                        state = MonitorMicrohardState::LOGIN;
                    }
                    break;
                }

                case MonitorMicrohardState::MODEM_SETTINGS: {
                    logged_in = true;
                    Breadcrumb(true);

                    // Move on to LAN status check
                    if (radio_info.channel_freq != -1 &&
                        radio_info.channel_bw != -1 &&
                        radio_info.tx_power != -1) {
                        state = MonitorMicrohardState::MODEM_LAN_CHECK;

                        command = "AT+MWSTATUS\n";
                        if (send(sock, command.c_str(), command.length(), 0) == -1) {
                            close(sock);
                            sock = 0;
                            connected = false;
                            logged_in = false;
                            state = MonitorMicrohardState::LOGIN;
                        }
                    } else {
                        // AT+MWTXPOWERQ will override setting so using
                        // AT+MWSTATUS is not sufficient to find setting
                        if (radio_info.tx_power == -1) {
                            command = "AT+MWTXPOWER\n";
                            if (send(sock, command.c_str(), command.length(), 0) == -1) {
                                close(sock);
                                sock = 0;
                                connected = false;
                                logged_in = false;
                                state = MonitorMicrohardState::LOGIN;
                            }
                            if (output.find("+MWTXPOWER:") != std::string::npos) {
                                ParseSetting(output);
                            }
                        }
                        else if (radio_info.channel_freq == -1) {
                            if (detected_model == radio_control::RadioModel::PMDDL2450) {
                                command = "AT+MWFREQ2400\n";
                            } else if (detected_model == radio_control::RadioModel::PDDL1800) {
                                command = "AT+MWFREQ1800\n";
                            } else {
                                command = "AT+MWFREQ\n";
                            }
                            if (send(sock, command.c_str(), command.length(), 0) == -1) {
                                close(sock);
                                sock = 0;
                                connected = false;
                                logged_in = false;
                                state = MonitorMicrohardState::LOGIN;
                            }
                            if ((output.find("+MWFREQ2400:") != std::string::npos) ||
                                (output.find("+MWFREQ1800:") != std::string::npos) ||
                                (output.find("+MWFREQ:") != std::string::npos)) {
                                ParseSetting(output);
                            }
                        }
                        else if (radio_info.channel_bw == -1) {
                            command = "AT+MWBAND\n";
                            if (send(sock, command.c_str(), command.length(), 0) == -1) {
                                close(sock);
                                sock = 0;
                                connected = false;
                                logged_in = false;
                                state = MonitorMicrohardState::LOGIN;
                            }
                            if (output.find("+MWBAND:") != std::string::npos) {
                                ParseSetting(output);
                            }
                        }
                    }

                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    break;
                }

                case MonitorMicrohardState::MODEM_LAN_CHECK: {
                    // Only move forward with different commands once we've detected and
                    // parsed the radio's lan information.
                    if (radio_info.lan_ok != -1) {
                        if (SupportsFrequencyHopping()) {
                            state = MonitorMicrohardState::MODE_FREQ_HOP_CHECK;
                        } else {
                            state = MonitorMicrohardState::MODEM_STATUS;
                        }
                    }

                    command = "AT+MNLAN?\n";
                    if (send(sock, command.c_str(), command.length(), 0) == -1) {
                        connected = false;
                        state = MonitorMicrohardState::LOGIN;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    break;
                }

                case MonitorMicrohardState::MODE_FREQ_HOP_CHECK: {
                    if (!is_host) {
                        // Master enables/disable channel hopping
                        if (radio_info.freq_hop_stat == -1) {
                            radio_info.freq_list.clear();
                            command = "AT+MWCHANHOP\n";
                            if (send(sock, command.c_str(), command.length(), 0) == -1) {
                                close(sock);
                                sock = 0;
                                connected = false;
                                logged_in = false;
                                state = MonitorMicrohardState::LOGIN;
                            }
                            if (output.find("<Status>") != std::string::npos) {
                                ParseHopInfo(output);
                            } else if (output.find("ERROR:") != std::string::npos) {
                                radio_info.freq_hop_stat = 0;
                            }
                        } else {
                            command = "AT+MWFREQLIST\n";
                            if (send(sock, command.c_str(), command.length(), 0) == -1) {
                                close(sock);
                                sock = 0;
                                connected = false;
                                logged_in = false;
                                state = MonitorMicrohardState::LOGIN;
                            }
                            if (output.find("+MWFREQLIST:") != std::string::npos) {
                                ParseHopInfo(output);
                            }
                        }
                    } else {
                        // Slaves either scan single frequency or frequency list
                        command = "AT+MWFREQLIST\n";
                        if (send(sock, command.c_str(), command.length(), 0) == -1) {
                            close(sock);
                            sock = 0;
                            connected = false;
                            logged_in = false;
                            state = MonitorMicrohardState::LOGIN;
                        }
                        if (output.find("+MWFREQLIST:") != std::string::npos) {
                            ParseHopInfo(output);
                            if (radio_info.freq_list.size() > 1) {
                                radio_info.freq_hop_stat = 1;
                                radio_info.freq_hop_mode = 0;
                            } else if (radio_info.freq_list.size() == 1) {
                                radio_info.freq_hop_stat = 0;
                                radio_info.freq_hop_mode = 0;
                            } else {
                                radio_info.freq_hop_stat = -1;
                            }
                        } else if (output.find("MWFREQLIST") != std::string::npos) {
                            ParseHopInfo(output);
                        }
                    }

                    // Move forward if hop status discovered and
                    // either band hopping or frequency list obtained
                    if (radio_info.freq_hop_stat != -1) {
                        switch (radio_info.freq_hop_stat) {
                            case 0:       // Disabled
                                state = MonitorMicrohardState::MODEM_STATUS;
                                break;
                            case 1:       // Enabled
                                if (radio_info.freq_hop_mode == 2)
                                    state = MonitorMicrohardState::MODEM_STATUS;
                                else if (radio_info.freq_hop_mode >= 0 &&
                                         !radio_info.freq_list.empty()) {
                                    state = MonitorMicrohardState::MODEM_STATUS;
                                }
                        }
                    }

                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    break;
                }

                case MonitorMicrohardState::MODEM_STATUS: {
                    state = MonitorMicrohardState::MODEM_STATUS;
                    int sleep_ms = 200;
                    if (!config_queue.size() && factory_reset == false) {
                        // Use at+mwvrateq when available to query current tx modulation
                        if (SupportsFeature(FEATURE_RATE_FLOOR)) {
                            if (current_state == RadioState::DISCONNECTED ||
                                current_state == RadioState::CONNECTED) {
                                command = "AT+MWVRATEQ\n";
                                if (send(sock, command.c_str(), command.length(), 0) == -1) {
                                    close(sock);
                                    sock = 0;
                                    connected = false;
                                    logged_in = false;
                                    state = MonitorMicrohardState::LOGIN;
                                }
                                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                                sleep_ms -= 10;
                            }
                        }

                        command = "AT+MWTXPOWER\n";
                        if (send(sock, command.c_str(), command.length(), 0) == -1) {
                            close(sock);
                            sock = 0;
                            connected = false;
                            logged_in = false;
                            state = MonitorMicrohardState::LOGIN;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        sleep_ms -= 10;

                        command = "AT+MWSTATUS\n";
                        radio_control::vrc_log("RSSI: Sending mwstatus\n");
                        if (send(sock, command.c_str(), command.length(), 0) == -1) {
                            close(sock);
                            sock = 0;
                            connected = false;
                            logged_in = false;
                            state = MonitorMicrohardState::LOGIN;
                        }
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
                    break;
                }

                default:
                    break;
            }

            if (factory_reset && !logged_in) {
                radio_control::vrc_log(
                        "Factory reset required but waiting for login to complete.\n");
            }

            // Socket failed while gathering settings/status. Re-connect session.
            if (sock == 0)
                continue;

            while (
                    ((config_queue.size() && logged_in) || (factory_reset && logged_in)) &&
                    keep_running) {
                if (factory_reset) {
                    radio_control::vrc_log("Settings reset executing.\n");
                    // If we're currently the host, set us up as 20.104, otherwise we'll
                    // try the default for a drone (105)
                    // TODO: Will we ever know if we're supposed to be .106-.199 in this
                    // case?
                    RunShortCommand("at+mspwd=microhardsrm,microhardsrm\n");
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));

                    RunShortCommand("at+mwradio=1\n");
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));

                    if (is_host) {
                        RunShortCommand("at+msmname=CENTRAL\n");
                    } else {
                        RunShortCommand("at+msmname=REMOTE\n");
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));

                    if (config_ip_radio.empty()) {
                        // no radio IP specified used defaults
                        if (is_host) {
                            configured_ip = microhard_subnet + ".104";
                        } else {
                            configured_ip = microhard_subnet + ".105";
                        }
                    } else {
                        // radio IP specified
                        configured_ip = config_ip_radio;
                    }
                    RunShortCommand("AT+MNLAN=\"lan\",EDIT,0," + configured_ip +
                                    ",255.255.255.0,0\n");
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));

                    RunShortCommand("AT+MNLANDHCP=\"lan\",0\n");
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));

                    if (use_mesh) {
                        RunShortCommand("at+mwvmode=3\n"); // Mesh
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    } else {
                        if (!is_host) {
                            RunShortCommand("at+mwvmode=0\n"); // Master
                            std::this_thread::sleep_for (std::chrono::milliseconds(100));
                        } else {
                            RunShortCommand("at+mwvmode=0\n"); // Slave
                            std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        }
                    }

                    RunShortCommand("at+mwvrate=0\n");
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));

                    RunShortCommand("at+mweminf=0\n");
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));

                    RunShortCommand("at&w\n");
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));

                    password = "microhardsrm";
                    microhard_adapter = "";
                    microhard_address = "";
                    detected_model = RadioModel::UNKNOWN;
                    current_state = RadioState::REMOVED;
                    factory_reset = false;
                    login_attempts = 0;
                    radio_info.channel_freq = -1;
                    radio_info.channel_bw = -1;
                    radio_info.tx_power = -1;
                    radio_info.tx_power = -1;
                    radio_info.tx_modulation = -1;
                    radio_info.network_id = "";
                    radio_info.network_password = "";
                    radio_info.operation_mode = "";
                    radio_info.lan_ok = -1;
                    reset_attempted = true;
                    if (sock) {
                        logged_in = false;
                        connected = false;
                        close(sock);
                        sock = 0;
                    }

                    // Wait for reconfiguration before checking for IP response
                    // from broadcast query.
                    if (microhard_address != configured_ip) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(8000));
                    } else {
                        std::this_thread::sleep_for(std::chrono::milliseconds(3000));
                    }
                    goto handler_top;
                }

                if (config_queue.size() && logged_in &&
                    (current_state != RadioState::BOOTING)) {
                    std::lock_guard<std::mutex> lock(config_queue_mutex);
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
                            int channel = std::get<2>(data);

                            (void)frequency;

                            if (SupportsFrequencyHopping()) {
                                // Disable hopping, back to single frequency
                                radio_info.freq_hop_stat = 0;
                                radio_info.freq_hop_mode = -1;
                                if (!is_host) {
                                    RunShortCommand("AT+MWCHANHOP=0\n");
                                }
                                RunShortCommand("AT+MWFREQLIST=0\n");
                            }

                            int retries = 0;
                            bool success1 = false;
                            bool success2 = false;
                            while (retries < 3) {
                                if (!success1) {
                                    // Old MH versions had a slightly different frequency setting
                                    // command, we use both methods here and expect one to fail.
                                    if (detected_model == RadioModel::PMDDL900) {
                                        success1 = RunShortCommand("AT+MWFREQ900=" + std::to_string(channel) + "\n");
                                    } else if (detected_model == RadioModel::PDDL1800) {
                                        success1 = RunShortCommand("AT+MWFREQ1800=" + std::to_string(channel) + "\n" +
                                                                   "AT+MWFREQ=" + std::to_string(channel) + "\n");
                                    } else if (detected_model == RadioModel::PMDDL2450) {
                                        success1 = RunShortCommand("AT+MWFREQ2400=" + std::to_string(channel) + "\n" +
                                                                   "AT+MWFREQ=" + std::to_string(channel) + "\n");
                                    } else if ((detected_model == RadioModel::PMDDL1624) ||
                                               (detected_model == RadioModel::FDDL1624)) {
                                        // Multi-band radios use the actual frequency, not a "channel" number.
                                        success1 = RunShortCommand("AT+MWFREQ=" + std::to_string(frequency) + "\n");
                                    }
                                }

                                if (!success2) {
                                    if (bandwidth == 8) {
                                        success2 = RunShortCommand("AT+MWBAND=0\n");
                                    } else if (bandwidth == 4) {
                                        success2 = RunShortCommand("AT+MWBAND=1\n");
                                    } else if (bandwidth == 2) {
                                        success2 = RunShortCommand("AT+MWBAND=2\n");
                                    } else if (bandwidth == 1) {
                                        success2 = RunShortCommand("AT+MWBAND=3\n");
                                    }
                                }
                                // Full success, stop trying
                                if (success1 && success2) break;

                                radio_control::vrc_log("Frequency update failed\n");
                                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                                retries++;
                            }
                            if (success1) {
                                radio_info.channel_freq = frequency;
                                radio_info.freq_list.clear();
                            }
                            break;
                        }
                        case radio_control::Action::ACTION_CHANGE_FREQUENCY_LIST: {
                            auto data = any::any_cast<std::tuple<std::vector<int>, float, int>>(cmd.item);
                            auto frequency_list = std::get<0>(data);
                            float bandwidth = std::get<1>(data);
                            auto channel = std::get<2>(data);
                            if ((detected_model == RadioModel::PMDDL2450) ||
                                (detected_model == RadioModel::PMDDL1624) ||
                                (detected_model == RadioModel::FDDL1624)) {
                                // Use frequency list hopping ordered or random
                                std::string frequencies_string;
                                for (auto freq : frequency_list) {
                                    frequencies_string += std::to_string(freq);
                                    frequencies_string += ",";
                                }
                                if (!frequencies_string.empty()) {
                                    frequencies_string.pop_back();  //remove last comma

                                    if (bandwidth == 8) {
                                        RunShortCommand("AT+MWBAND=0\n");
                                    } else if (bandwidth == 4) {
                                        RunShortCommand("AT+MWBAND=1\n");
                                    } else if (bandwidth == 2) {
                                        RunShortCommand("AT+MWBAND=2\n");
                                    } else if (bandwidth == 1) {
                                        RunShortCommand("AT+MWBAND=3\n");
                                    }

                                    if ((detected_model == RadioModel::PMDDL1624) ||
                                        (detected_model == RadioModel::FDDL1624)) {
                                        // Multi-band radios use the actual frequency, not a "channel" number.
                                        RunShortCommand("AT+MWFREQ=" + std::to_string(frequency_list[0]) + "\n");
                                    } else if (detected_model == RadioModel::PMDDL2450) {
                                        RunShortCommand("AT+MWFREQ2400=" + std::to_string(channel) + "\n");
                                    }

                                    // Allow several retries if frequency list command fails
                                    int retries = 0;
                                    while (retries < 3) {
                                        if (RunShortCommand("AT+MWFREQLIST=" + frequencies_string + "\n")) break;
                                        radio_control::vrc_log("Frequency list update failed\n");
                                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                                        retries++;
                                    }

                                    radio_info.freq_list = frequency_list;
                                    radio_info.channel_freq = frequency_list[0];
                                    if (is_host) {
                                        radio_info.freq_hop_stat = 1;
                                        radio_info.freq_hop_mode = 0;
                                    }
                                } else {
                                    RunShortCommand("AT+MWFREQLIST=0\n");
                                }
                            }
                            break;
                        }
                        case radio_control::Action::ACTION_CHANGE_FREQUENCY_HOPPING: {
                            auto data = any::any_cast<std::tuple<uint8_t, uint8_t, uint32_t, uint32_t,
                                    uint32_t, uint32_t, bool>>(cmd.item);
                            uint8_t selection_mode = std::get<0>(data);
                            uint8_t dynamic_mode = std::get<1>(data);
                            uint32_t channel_interval = std::get<2>(data);
                            uint32_t switch_interval = std::get<3>(data);
                            uint32_t announce_times = std::get<4>(data);
                            uint32_t selected_bands = std::get<5>(data);
                            bool attempt_recovery = std::get<6>(data);
                            bool random = (selected_bands != 0) || (selection_mode == 1 || selection_mode == 2);

                            if ((detected_model == RadioModel::PMDDL2450) ||
                                (detected_model == RadioModel::PMDDL1624) ||
                                (detected_model == RadioModel::FDDL1624)) {
                                // NOTE: pMDDL1624 DOCUMENTATION INCORRECT FOR AT+MWCHANHOP
                                // ACTUAL SYNTAX:
                                // AT+MWCHANHOP=<Status>
                                // AT+MWCHANHOP=<Status>,<Mode>                                                       Where <Mode>=3
                                // AT+MWCHANHOP=<Status>,<Mode>,<Channel Interval>,<Switch Interval>,<Announcement>   Where <Mode>=0,1 or 2
                                // AT+MWCHANHOP=<Status>,<Mode>,<Channel Interval>,<Switch Interval>,<Announcement>,
                                //               <BAND1>,<BAND2>,<BAND3>,<BAND4>,<BAND5>,<BAND6>[<Recovery Attempt>]  Where <Mode>=2
                                if (random  && channel_interval == 0) {
                                    channel_interval = (attempt_recovery) ? 10 : 5;
                                }

                                // If band mask not set, then a frequency list must already have
                                // been configured.
                                if (selected_bands) {
                                    // Frequency hop among selected bands
                                    std::string bands_string;
                                    for (uint32_t i=0; i < 6; i++) {
                                        bands_string += ((selected_bands & (1 << i)) != 0) ? "1" : "0";
                                        bands_string += ",";
                                    }

                                    radio_control::vrc_log("Band hopping mode: "
                                                           + std::to_string(selection_mode) + "\n");
                                    radio_info.freq_hop_mode = selection_mode;
                                    radio_info.freq_hop_stat = 1;
                                    RunShortCommand("AT+MWCHANHOP=1,2," +
                                                    std::to_string(channel_interval) + "," +
                                                    std::to_string(switch_interval) + "," +
                                                    std::to_string(announce_times) + "," +
                                                    bands_string +
                                                    std::to_string(attempt_recovery ? 1 : 0) + "\n");
                                } else if (!GetFrequencyList().empty()) {
                                    // mode 0 or 1 (frequency list)
                                    // must set frequency list before setting enabling hopping mode 0/1
                                    radio_info.freq_hop_mode = selection_mode;
                                    radio_info.freq_hop_stat = 1;
                                    radio_control::vrc_log("Frequency hopping mode: " +
                                                           std::to_string(selection_mode) + "\n");
                                    if (selection_mode != 3) {
                                        RunShortCommand("AT+MWCHANHOP=1," +
                                                        std::to_string(selection_mode) + "," +
                                                        std::to_string(channel_interval) + "," +
                                                        std::to_string(switch_interval) + "," +
                                                        std::to_string(announce_times) + "\n");
                                    } else {
                                        RunShortCommand("AT+MWCHANHOP=1,3\n");
                                    }
                                } else {
                                    // Clear frequency list and disable hopping
                                    RunShortCommand("AT+MWFREQLIST=0\n");
                                    RunShortCommand("AT+MWCHANHOP=0\n");
                                    radio_info.freq_hop_mode = -1;
                                    radio_info.freq_hop_stat = 0;
                                }

                                // For dynamic switch also set the dynamic switch mode
                                if (radio_info.freq_hop_mode == 3) {
                                    RunShortCommand("AT+MWDYNSWMODE=" +
                                                    std::to_string(dynamic_mode) + "\n");
                                    radio_control::vrc_log("Dynamic mode: " +
                                                           std::to_string(dynamic_mode) + "\n");
                                }
                            }
                            break;
                        }
                        case radio_control::Action::ACTION_CHANGE_NETWORK_ID: {
                            auto id = any::any_cast<std::string>(cmd.item);
                            bool set_mesh_id = use_mesh;
                            if (topology_change > 0) {
                                set_mesh_id =
                                        (static_cast<Topology>(topology_change) == Topology::MESH);
                            }
                            if (set_mesh_id) {
                                radio_control::vrc_log("Setting mesh id to: " + id + "\n");
                                RunShortCommand("AT+MWMESHID=" + id + "\n");
                            } else {
                                radio_control::vrc_log("Setting network id to: " + id + "\n");
                                RunShortCommand("AT+MWNETWORKID=" + id + "\n");
                            }
                            break;
                        }
                        case radio_control::Action::ACTION_CHANGE_PASSWORD: {
                            // TODO: How do we set this on the fly?
                            auto pw = any::any_cast<std::string>(cmd.item);
                            RunShortCommand("AT+MWVENCRYPT=2," + pw + "\n");
                            break;
                        }
                        case radio_control::Action::ACTION_RESTART_SERVICES: {
                            current_state = RadioState::CONFIGURING;
                            // TODO: Bring MWEMINF out to a separate action.
                            RunShortCommand("at+mweminf=0\n");
                            RunShortCommand("AT&W\n");
                            // Force us to receive a valid status update before switching
                            // back to the default disconnected state.
                            radio_info.operation_mode = "";
                            std::this_thread::sleep_for(std::chrono::milliseconds(5000));
                            break;
                        }
                        case radio_control::Action::ACTION_SET_AIR_RATE_IMMEDIATE: {
                            auto rate = any::any_cast<int>(cmd.item);
                            RunShortCommand("AT+MWVRATEQ=" + std::to_string(rate) + "\n");
                            break;
                        }
                        case radio_control::Action::ACTION_SET_MIMO: {
                            auto active = any::any_cast<bool>(cmd.item);
                            if (active) {
                                RunShortCommand("MWMIMO=1,1\n");
                            } else {
                                RunShortCommand("MWMIMO=0,1\n");
                            }
                            break;
                        }
                        case radio_control::Action::ACTION_SET_AIR_RATE: {
                            auto rate = any::any_cast<int>(cmd.item);
                            RunShortCommand("AT+MWVRATE=" + std::to_string(rate) + "\n");
                            break;
                        }
                        case radio_control::Action::ACTION_SET_OUTPUT_POWER: {
                            auto dbm = any::any_cast<int>(cmd.item);
                            RunShortCommand("AT+MWTXPOWER=" + std::to_string(dbm) + "\n");
                            break;
                        }
                        case radio_control::Action::ACTION_SET_OUTPUT_POWER_IMMEDIATE: {
                            auto dbm = any::any_cast<int>(cmd.item);
                            RunShortCommand("AT+MWTXPOWERQ=" + std::to_string(dbm) + "\n");
                            break;
                        }
                        case radio_control::Action::ACTION_CHANGE_BANDWIDTH_IMMEDIATE: {
                            auto band = any::any_cast<int>(cmd.item);
                            RunShortCommand("AT+MWBANDQ=" + std::to_string(band) + "\n");
                            break;
                        }
                        case radio_control::Action::ACTION_SET_TOPOLOGY: {
                            auto topology = any::any_cast<Topology>(cmd.item);
                            switch (topology) {
                                case Topology::STATION:
                                    if (!is_host) {
                                        RunShortCommand("AT+MWVMODE=0\n"); // Master
                                    } else {
                                        RunShortCommand("AT+MWVMODE=1\n"); // Slave
                                    }
                                    use_mesh = false;
                                    topology_change = 0;
                                    break;
                                case Topology::MESH:
                                    RunShortCommand("AT+MWVMODE=3\n"); // Mesh
                                    use_mesh = true;
                                    topology_change = 0;
                                    break;
                                case Topology::ACCESS_POINT:
                                case Topology::RELAY:
                                default:
                                    break;
                            }
                            break;
                        }
                        case radio_control::Action::ACTION_SET_BUFFER_SIZE: {
                            auto buffer_size = any::any_cast<int>(cmd.item);
                            if (SupportsFeature(radio_control::FEATURE_BUFFER_SIZE)) {
                                constexpr int min_buffer_size =  256 * 1024;
                                constexpr int max_buffer_size = 4096 * 1024;
                                if (buffer_size == 0)
                                    buffer_size = min_buffer_size * 2;
                                if (buffer_size >= min_buffer_size && buffer_size <= max_buffer_size) {
                                    RunShortCommand("AT+MWBUFFSZQ=" +
                                                    std::to_string(buffer_size) + "\n");
                                }
                            }
                            break;
                        }
                        case radio_control::Action::ACTION_SET_RATE_FLOOR: {
                            auto rate_floor = any::any_cast<int>(cmd.item);
                            if (SupportsFeature(radio_control::FEATURE_RATE_FLOOR)) {
                                if (rate_floor > 0) {
                                    RunShortCommand("AT+MWFRATE=1," +
                                                    std::to_string(rate_floor-1) + "\n");
                                } else {
                                    RunShortCommand("AT+MWFRATE=0\n");
                                }
                            }
                            break;
                        }
                        case radio_control::Action::ACTION_SET_RATE_CEILING: {
                            auto rate_ceiling = any::any_cast<int>(cmd.item);
                            if (SupportsFeature(radio_control::FEATURE_RATE_FLOOR)) {
                                if (rate_ceiling > 0) {
                                    RunShortCommand("AT+MWCRATE=1," +
                                                    std::to_string(rate_ceiling-1) + "\n");
                                } else {
                                    RunShortCommand("AT+MWCRATE=0\n");
                                }
                            }
                            break;
                        }
                        case radio_control::Action::ACTION_SET_BEACON_LOSS_COUNT: {
                            auto beacon_loss_count = any::any_cast<int>(cmd.item);
                            if (SupportsFeature(radio_control::FEATURE_FREQUENCY_LIST)) {
                                if (beacon_loss_count == 0)
                                    beacon_loss_count = 300;
                                if (beacon_loss_count >= 7) {
                                    RunShortCommand("AT+MWBEACONLOSS=" +
                                                    std::to_string(beacon_loss_count) + "\n");
                                }
                            }
                            break;
                        }
                        default:
                            break;
                    }
                    config_queue.pop_front();

                    // Kick the return queue:
                    std::string temp("\n");
                    send(sock, temp.c_str(), temp.length(), 0);
                } else if (config_queue.size() && current_state == RadioState::BOOTING) {
                    // Configuration pending but need to move past BOOTING state
                    break;
                }
            }

            // If we haven't yet determined out IP, try to discover neigbors.
            if (!ip_determined) {
                radio_control::vrc_log("Discovering peers.\n");
                int nodes = GetMicrohardCount(microhard_adapter);
                if (nodes > 1) {
                    radio_control::vrc_log(
                            "Nodes on the network: " + std::to_string(nodes) + "\n");
                    // We're going to set our IP based on number of nodes on the network.
                    system_wrap(std::string("ip addr add " + microhard_subnet + "." +
                                            std::to_string(4 + (nodes - 1)) +
                                            "/24 brd 255.255.255.255 dev " +
                                            microhard_adapter));
                    ip_determined = true;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

        } // while (keep_running)

        radio_control::vrc_log("Handler exiting.\n");
        radio_control::vrc_log("TCP disconnect from telnet session.\n");
        if (sock > 0) {
            shutdown(sock, SHUT_RDWR);
            close(sock);
            sock = 0;
        }

    } // namespace radio_control

    bool MicrohardControl::RunShortCommand(std::string c) {
        char buffer[1024];
        uint8_t waits = 10;
        ssize_t n = 0;
        radio_control::vrc_log("Command: " + c + "\n");
        if (send(sock, c.c_str(), c.length(), 0) == -1) {
            return false;
        }

        while (waits > 0 && keep_running) {
            n = read(sock, buffer, sizeof(buffer));
            if (n <= 1) {
                if (n == 0) {
                    radio_control::vrc_log("Orderly TCP disconnect detected.\n");
                    shutdown(sock, SHUT_RDWR);
                    close(sock);
                    sock = 0;
                    return false;
                }
            } else {
                break;
            }
            waits--;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        if (n) {
            std::string output;
            output += buffer;
            if (output.find("OK") != std::string::npos) {
                radio_control::vrc_log(output + "\n");
                return true;
            }
        }
        return false;
    }

    bool MicrohardControl::RunQueryCommand(
            std::string c, std::function<bool(const std::string &response)> parser) {
        char buffer[1024];
        uint8_t waits = 10;
        ssize_t n = 0;
        radio_control::vrc_log("Command: " + c + "\n");
        if (send(sock, c.c_str(), c.length(), 0) == -1) {
            return false;
        }

        while (waits > 0 && keep_running) {
            n = read(sock, buffer, sizeof(buffer));
            if (n <= 1) {
                if (n == 0) {
                    radio_control::vrc_log("Orderly TCP disconnect detected.\n");
                    shutdown(sock, SHUT_RDWR);
                    close(sock);
                    sock = 0;
                    return false;
                }
            } else {
                break;
            }
            waits--;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        if (n) {
            std::string output;
            output += buffer;
            return parser(output);
        }
        return false;
    }

// Function returns likely adapter names for us to work with.
// TODO: Move into RadioControl class as a utility function?
    std::string MicrohardControl::GetAdapterName(std::vector<std::string> &ips) {
        struct ifaddrs *addresses;
        if (getifaddrs(&addresses) == -1) {
            radio_control::vrc_log("getifaddrs call failed\n");
            return "";
        }

        std::vector<std::string> likely_adapter_names;
        likely_adapter_names.push_back("usb");
        likely_adapter_names.push_back("eth");
        likely_adapter_names.push_back("enx");
        likely_adapter_names.push_back("enp");

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

        for (auto ifa_name : adapters) {
            for (auto name : likely_adapter_names) {
                if (ifa_name.find(name) != std::string::npos) {
                    radio_control::vrc_log("Trying " + ifa_name + "\n");
                    // Make sure device is up:
                    system_wrap(std::string("ip link set " + ifa_name + " up"));
                    // Flush ip settings that exist:
                    // system_wrap(std::string("ip addr flush dev " + ifa_name));
                    // We're actually not going to flush it here, we'll let it try to add
                    // the address here, and only flush the device of ips if the discovery
                    // failed. This has the potential to break some configs, and we should
                    // consider just removing the one address that we added. Flushing the
                    // device of addresses helps to ensure that we're setting it up to our
                    // linking, so this is also a potential issue. In general our
                    // if/adapter name filter should help to limit this sort of stomping,
                    // but it's still within the realm of possibilities.
                    // TODO: Save the adapter's ip configuration before we touch it, so we
                    // can put it back We're not flushing here so we never break the flow
                    // of data if the adapter already happens to be in the right
                    // configuration already.

                    // In addition to adding the broadcast ip setup, add the configured
                    // ip in the case that everything is actually already set up and
                    // ready to go. This allows data to flow immediately.
                    // TODO: Ensure no subsequent steps will break the link in this
                    // optimisitc configuration.
                    system_wrap(std::string(
                            "ip addr add 192.168.254.4/16 brd 255.255.255.255 dev " +
                            ifa_name));
                    system_wrap(std::string("ip addr add " + config_ip_network +
                                            "/24 brd 255.255.255.255 dev " + ifa_name));
                    ips = GetMicrohardIPs(name);
                    int count = ips.size();
                    if (count) {
                        radio_control::vrc_log("Found a microhard on: " + ifa_name + "\n");
                        return (ifa_name);
                    } else {
                        // Flush ip addresses that we added to the candidate adapter:
                        system_wrap(
                                std::string("ip addr del 192.168.254.4/16 dev " + ifa_name));
                    }
                }
            }
        }
        return ("");
    }

    int MicrohardControl::GetMicrohardCount(std::string adapter) {
        auto ips = GetMicrohardIPs(adapter);
        return static_cast<int>(ips.size()); // count_lines(std::string(result));
    }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-conversion"
    std::vector<std::string>
    MicrohardControl::GetMicrohardIPs(std::string adapter) {
        (void)adapter;
        std::vector<std::string> outputs;
        std::vector<radio_control::RadioModel> models;

        // RECEIVER:
        struct sockaddr_in broadcastRxAddr;
        int rx_sock;
        int tx_sock;

        bool found_central = false;
        bool found_remote = false;

        if ((rx_sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
            radio_control::vrc_log("Can't open receive socket\n");
            char buffer[256];
            std::string error(strerror_r(errno, buffer, 256));
            radio_control::vrc_log(error + "\n");
        }

        int one = 1;
        if (setsockopt(rx_sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0) {
            radio_control::vrc_log("Can't set sockopt\n");
            char buffer[256];
            std::string error(strerror_r(errno, buffer, 256));
            radio_control::vrc_log(error + "\n");
        }

        // Add a 500mS timeout. Every time we get a response we will try to receive
        // another, just in case we have many microhards on the network.
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 250000;
        setsockopt(rx_sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof tv);

        memset(&broadcastRxAddr, 0, sizeof(broadcastRxAddr));
        broadcastRxAddr.sin_family = AF_INET;
        broadcastRxAddr.sin_addr.s_addr = inet_addr("255.255.255.255");
        broadcastRxAddr.sin_port = htons(13370);

        /* Bind to the broadcast port */
        if (bind(rx_sock, (struct sockaddr *)&broadcastRxAddr,
                 sizeof(broadcastRxAddr)) < 0) {
            radio_control::vrc_log("Can't bind receive socket\n");
            char buffer[256];
            std::string error(strerror_r(errno, buffer, 256));
            radio_control::vrc_log(error + "\n");
        }

        // SENDER:
        struct sockaddr_in broadcastAddr;
        struct sockaddr_in broadcastBindAddr;
        int broadcastPermission; /* Socket opt to set permission to broadcast */

        if ((tx_sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
            radio_control::vrc_log("Couldn't make socket\n");
            char buffer[256];
            std::string error(strerror_r(errno, buffer, 256));
            radio_control::vrc_log(error + "\n");
        }

        broadcastPermission = 1;
        if (setsockopt(tx_sock, SOL_SOCKET, SO_BROADCAST,
                       (void *)&broadcastPermission,
                       sizeof(broadcastPermission)) < 0) {
            radio_control::vrc_log("Can't set sockopt\n");
            char buffer[256];
            std::string error(strerror_r(errno, buffer, 256));
            radio_control::vrc_log(error + "\n");
        }

        // Broadcast details
        memset(&broadcastAddr, 0, sizeof(broadcastAddr));
        broadcastAddr.sin_family = AF_INET;
        broadcastAddr.sin_addr.s_addr = inet_addr("255.255.255.255");
        broadcastAddr.sin_port = htons(20097);

        // Bind details
        memset(&broadcastBindAddr, 0, sizeof(broadcastBindAddr));
        broadcastBindAddr.sin_family = AF_INET;
        broadcastBindAddr.sin_addr.s_addr = inet_addr("192.168.254.4");
        broadcastBindAddr.sin_port = htons(13370);

        if (bind(tx_sock, (sockaddr *)&broadcastBindAddr, sizeof(broadcastBindAddr)) <
            0) {
            radio_control::vrc_log("Can't bind broadcast socket\n");
            char buffer[256];
            std::string error(strerror_r(errno, buffer, 256));
            radio_control::vrc_log(error + "\n");
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        uint8_t sbuf[10] = {0x70, 0x63, 0x00, 0x06, 0xff,
                            0xff, 0xff, 0xff, 0xff, 0xff};
        int ret = sendto(tx_sock, sbuf, 10, 0, (sockaddr *)&broadcastAddr,
                         sizeof(broadcastAddr));
        radio_control::vrc_log("Discovery send response: " + std::to_string(ret) +
                               "\n");

        std::this_thread::sleep_for(std::chrono::milliseconds(250));

        // RX!
        // Do not assume that the first response we get is from our local microhard!
        struct sockaddr_in fromMH;
        socklen_t addrlen = sizeof(struct sockaddr_in);
        for (int i = 0; i < 255; i++) {
            char buffer[1024];
            int received = 0;
            memset(&fromMH, 0, sizeof(fromMH));

            struct timeval tv2;
            tv2.tv_sec = 0;
            tv2.tv_usec = 250000;
            fd_set rset;
            // Needed for select
            FD_ZERO(&rset);
            FD_SET(rx_sock, &rset);

            // select stop us from getting stuck in receive from. The timeout added to
            // the rx_sock doesn't always work.
            if (select(rx_sock + 1, &rset, NULL, NULL, &tv2) == 1) {
                if ((received = recvfrom(rx_sock, buffer, 1024, 0,
                                         (struct sockaddr *)&fromMH, &addrlen)) < 0) {
                    continue;
                }
            } else {
                received = -1;
                break;
            }

            std::string ip(inet_ntoa(fromMH.sin_addr));
            if (BlacklistedIP(ip)) continue;

            auto radio_model = radio_control::RadioModel::UNKNOWN;
            if (find_needle(buffer, received, "1800", 4) != nullptr) {
                radio_model = RadioModel::PDDL1800;
                radio_control::vrc_log("Is pMDDL1800 microhard radio.\n");
            } else if (find_needle(buffer, received, "2450", 4) != nullptr) {
                radio_model = RadioModel::PMDDL2450;
                radio_control::vrc_log("Is pMDDL2450 microhard radio.\n");
            } else if (find_needle(buffer, received, "900", 3) != nullptr) {
                radio_model = RadioModel::PMDDL900;
                radio_control::vrc_log("Is pMDDL900 microhard radio.\n");
            } else if (find_needle(buffer, received, "fDDL1624", 8) != nullptr) {
                // Will determine if it's a 1621, 1622, 1624 later as the
                // actual modle number is not in the discovery response.
                radio_model = RadioModel::FDDL1624;
                radio_control::vrc_log("Is fDDL1624 microhard radio.\n");
            } else if (find_needle(buffer, received, "1624", 4) != nullptr) {
                // Will determine if it's a 1621, 1622, 1624 later as the
                // actual modle number is not in the discovery response.
                radio_model = RadioModel::PMDDL1624;
                radio_control::vrc_log("Is pMDDL1624 microhard radio.\n");
            } else {
                radio_model = RadioModel::UNKNOWN;
                radio_control::vrc_log("Is unknown microhard radio.\n");
            }
            models.push_back(radio_model);

            radio_control::vrc_log("Response from: " + ip + "\n");

            // Prevent duplicate entries
            if (std::find(outputs.begin(), outputs.end(), ip) == outputs.end()) {
                outputs.push_back(ip);
            }

            /*
    radio_control::vrc_log("Response size: " + std::to_string(received) + "\n");
    radio_control::vrc_log("Response data:\n");
    std::string c;
    for (int i=0; i<received; i++) {
      c += std::to_string(buffer[i]);
      c += " (";
      c.push_back(buffer[i]);
      c += ") : ";
      if ((i % 20) == 19) {
        c += "\n";
        radio_control::vrc_log(c);
        c.clear();
      }
    }
    radio_control::vrc_log(c + "\n");
    */

            // Check to see if this is the local or remote microhard based
            // on the NAME string that's in the response.
            // - If the name is neither the CENTRAL or REMOTE, we need to fall
            //   back to other modes of detection
            // - If the name is CENTRAL and we're the cental node, we're good
            // - If the name is REMOTE and we're remote, we're good
            // - If CENTRAL/REMOTE doesn't match the is_host status, we need
            //   fall back to other modes of detection and then potentially
            //   reset the name to what's appropriate for the radio configuration
            if (find_needle(buffer, received, "CENTRAL", 7) != nullptr) {
                found_central = true;
            }
            if (find_needle(buffer, received, "REMOTE", 6) != nullptr) {
                found_remote = true;
            }

            /*
    for (int i=0; i<received; i++) {
         if (buffer[i] < 32) buffer[i] = '_';
    }
    buffer[received] = '\0';
    radio_control::vrc_log(std::string(buffer) + "\n");
    */
        }


        // If we only ever saw one IP and the name matches our central node flag
        // state, use the adapter. Currently only valid if not using mesh mode.
        if (!use_mesh) {
            if (outputs.size() == 1) {
                if ((is_host && found_central) || (!is_host && found_remote)) {
                    microhard_address = outputs.at(0);
                    detected_model = models.at(0);
                    companion_model = RadioModel::UNKNOWN;
                    radio_control::vrc_log(
                            "Only saw one, and is in correct config. Using this adapter.\n");
                } else if ((is_host && found_remote) || (!is_host && found_central)) {
                    radio_control::vrc_log(
                            "Only saw one, and is one of the companion configs. Keep looking.\n");
                    outputs.clear();
                } else {
                    radio_control::vrc_log(
                            "Only saw one, and is not in correct config. Probably needs reset.\n");
                }
            } else if (outputs.size() == 2) {
                // If we found two nodes and the CENTRAL/REMOTE names are present, use
                // this adapter as long as the IPs are sane.
                // TODO: This assumes
                if (found_remote && found_central) {
                    // Just force using .104 for host and .105 for remote if it's present.
                    if ((outputs.at(0) == "192.168.20.104" ||
                         outputs.at(0) == "192.168.20.105") &&
                        (outputs.at(1) == "192.168.20.104" ||
                         outputs.at(1) == "192.168.20.105")) {
                        if (is_host) {
                            microhard_address = "192.168.20.104";
                        } else {
                            microhard_address = "192.168.20.105";
                        }
                        if (outputs.at(0) == microhard_address) {
                            detected_model = models.at(0);
                            companion_model = models.at(1);
                        } else {
                            detected_model = models.at(1);
                            companion_model = models.at(0);
                        }
                        radio_control::vrc_log("Got both the default remote and the central node on "
                                               "the network. Using this adapter.\n");
                    } else {
                        if (is_host) {
                            if (outputs.at(0) == "192.168.20.104") {
                                microhard_address = outputs.at(0);
                                detected_model = models.at(0);
                                companion_model = models.at(1);
                            } else if (outputs.at(1) == "192.168.20.105") {
                                microhard_address = outputs.at(1);
                                detected_model = models.at(1);
                                companion_model = models.at(0);
                            }
                        } else {
                            if (outputs.at(0) != "192.168.20.104") {
                                microhard_address = outputs.at(0);
                                detected_model = models.at(0);
                                companion_model = models.at(1);
                            } else if (outputs.at(1) != "192.168.20.105") {
                                microhard_address = outputs.at(1);
                                detected_model = models.at(1);
                                companion_model = models.at(0);
                            }
                            radio_control::vrc_log("Got both the remote and the central node on "
                                                   "the network. Using this adapter.\n");
                        }
                    }
                }
            } else if (outputs.size() > 2) {
                if (config_ip_radio.length() > 0) {
                    for (size_t i = 0; i < outputs.size(); i++) {
                        if (outputs.at(i) == config_ip_radio) {
                            microhard_address = outputs.at(i);
                            detected_model = models.at(i);
                            break;
                        }
                    }
                }
            } else {
                //TODO support mesh networks with three or more responses
            }
        }

        close(tx_sock);
        close(rx_sock);
        return outputs;
    }

    bool MicrohardControl::BlacklistedIP(const std::string& ip) {
        bool blacklisted = false;
        if (!ip.empty()) {
            if (ip.find("0.0.0") == 0) {
                blacklisted = true;
            }
        }
        return blacklisted;
    }

#pragma GCC diagnostic pop
#pragma GCC diagnostic pop

    static
    std::vector<std::string> splitString(std::string& s, const char* delim) {
        std::vector<std::string> result;
        if (s.length() < 256) {
            char buf[256];
            strcpy(buf, s.c_str());
            const char* token = strtok(buf, delim);
            while (token) {
                result.push_back(token);
                token = strtok(nullptr, delim);
            }
        } else {
            char *buf = strdup(s.c_str());
            const char* token = strtok(buf, delim);
            while (token) {
                result.push_back(token);
                token = strtok(nullptr, delim);
            }
            free(buf);
            radio_control::vrc_log("WARNING! string length error.\n");
        }
        return result;
    }

    void MicrohardControl::ParseSummary(std::string raw) {
        std::istringstream blob(raw);
        std::string line;
        try {
            while (std::getline(blob, line) && keep_running) {
                if (line.find("Device") != std::string::npos) {
                    auto val = get_str_between_two_str(line, "Device      : ", "\r");
                    if (val.size()) {
                        radio_info.radio_name = val;
                        radio_control::vrc_log(std::string("Radio name: ") +
                                               radio_info.radio_name + "\n");
                    }
                }
                else if (line.find("Product") != std::string::npos) {
                    auto val = get_str_between_two_str(line, "Product     : ", "\r");
                    if (val.size()) {
                        radio_info.product_name = val;
                        radio_control::vrc_log(std::string("Product name: ") +
                                               radio_info.product_name + "\n");
                    }
                }
                else if (line.find("Hardware") != std::string::npos) {
                    auto val = get_str_between_two_str(line, "Hardware    : ", "\r");
                    if (val.size()) {
                        radio_info.hardware_version = val;
                        radio_control::vrc_log(std::string("Hardware version: ") +
                                               radio_info.hardware_version + "\n");
                    }
                }
                else if (line.find("Software") != std::string::npos) {
                    auto val = get_str_between_two_str(line, "Software    : ", "\r");
                    if (val.size()) {
                        std::regex number_regex("(\\d+)");
                        auto parse_begin =
                                std::sregex_iterator(val.begin(), val.end(),
                                                     number_regex);
                        auto parse_end = std::sregex_iterator();

                        // parse version string into components (major/minor/rev/build)
                        software_build.clear();
                        for (std::sregex_iterator i = parse_begin; i != parse_end; ++i) {
                            std::smatch match = *i;
                            software_build.push_back(static_cast<uint16_t>(std::atoi(match.str().c_str())));
                        }

                        // Do last as string signals that software version/build is available
                        radio_info.software_version = val;
                        radio_control::vrc_log(std::string("Software version: ") +
                                               radio_info.software_version + "\n");
                    }
                }
            }
        } catch (...) {
            // Make sure this doesn't throw an exception.
        }
    }

    void MicrohardControl::ParseStatus(std::string raw) {
        // Parse periodic responses we're interested in.
        std::istringstream blob(raw);
        std::string line;

        int lan_config_known = 0;
        bool static_ip = false, gateway_ok = false, dhcp_ok = false;
        bool dhcp_disabled = false;

        try {
            while (std::getline(blob, line) && keep_running) {
                if (line.find("General Status") != std::string::npos) {
                    radio_control::vrc_log("Got status message.\n");
                    ParseInfo(raw);
                    break;
                }
                if (line.find("Connection Info") != std::string::npos) {
                    radio_control::vrc_log("Got status message.\n");
                    ParseInfo(raw);
                    break;
                }
                // TODO: Add name detection.
                if (line.find("Traffic Status") != std::string::npos) {
                    radio_control::vrc_log("Got status message.\n");
                    ParseInfo(raw);
                    break;
                }
                if ((line.find(" <2400MHz Channel Index> :") != std::string::npos)) {
                    radio_control::vrc_log("Got pmddl2450 channel index.\n");
                    uint16_t line_count = 0;
                    bool found_ok = false;
                    while (std::getline(blob, line) && keep_running && !found_ok) {
                        if (line.find("OK") != std::string::npos) {
                            found_ok = true;
                        } else {
                            line_count++;
                        }
                    }
                    if (line_count > 11) {
                        radio_control::vrc_log("Too many channels to be JP pmddl2450.\n");
                        radio_variance = RadioVariance::NONE;
                    } else {
                        radio_control::vrc_log("JP pmddl2450 limited channels detected.\n");
                        radio_variance = RadioVariance::PMDDL2450JP;
                    }
                }
                if ((line.find("LAN Configuration") != std::string::npos) ||
                    (line.find("lan Configuration") != std::string::npos)) {
                    constexpr int lan_config_ready = 0x07;
                    radio_control::vrc_log("Got lan config.\n");
                    while (std::getline(blob, line) && keep_running) {
                        if (line.find("Connection Type") != std::string::npos) {
                            if (line.find("Static IP") != std::string::npos) {
                                static_ip = true;
                            } else {
                                static_ip = false;
                            }
                            lan_config_known |= 1;
                        }
                        if (line.find("Default Gateway") != std::string::npos) {
                            if (line.find("192") != std::string::npos) {
                                gateway_ok = false;
                                radio_control::vrc_log("Gateway not ok\n");
                            } else {
                                gateway_ok = true;
                            }
                            lan_config_known |= 2;
                        }
                        if (line.find("DHCP Server") != std::string::npos &&
                            !dhcp_disabled) {
                            if (line.find("Enabled") != std::string::npos) {
                                dhcp_ok = false;
                                radio_control::vrc_log("DHCP not ok\n");
                            } else {
                                dhcp_ok = true;
                            }
                            lan_config_known |= 4;
                        }

                        if ((lan_config_known & 4) && !dhcp_ok) {
                            char buffer[1024] = {};

                            // Before giving up send command to disable DHCP server.
                            lan_config_known &= ~4;
                            ssize_t n = read(sock, buffer,
                                             sizeof(buffer)); // flush any remaining response
                            (void)n;
                            radio_control::vrc_log("Disable DHCP server.\n");
                            RunShortCommand("AT+MNLANDHCP=\"lan\",0\n");
                            dhcp_ok = RunQueryCommand(
                                    "AT+MNLANDHCP=\"lan\"\n",
                                    [&](const std::string &output) -> bool {
                                        std::istringstream blob(output);
                                        std::string line;
                                        while (std::getline(blob, line)) {
                                            if (line.find("Mode") != std::string::npos) {
                                                if (line.find("DHCP Server disabled") !=
                                                    std::string::npos) {
                                                    dhcp_disabled = true;
                                                    radio_control::vrc_log("LAN DHCP disabled.\n");
                                                } else {
                                                    dhcp_disabled = false;
                                                }
                                            }
                                        }
                                        return dhcp_disabled;
                                    });
                            lan_config_known |= 4;
                        }

                        // Evaluate lan configuration state after
                        // all configuration data is parsed.
                        if (lan_config_known == lan_config_ready) {
                            if (static_ip && gateway_ok && dhcp_ok) {
                                radio_info.lan_ok = 1;
                            } else {
                                radio_control::vrc_log("Lan config needs reset.\n");
                                radio_info.lan_ok = 0;
                            }
                        }
                    }
                    break;
                }

                if (line.find("+MWVRATEQ:") != std::string::npos) {
                    auto modulation = get_str_between_two_str(line, "+MWVRATEQ: Virtual Interface TX Rate: ", "\r");
                    ParseRateMod(modulation);
                }

                if (line.find("+MWTXPOWER:") != std::string::npos) {
                    ParseSetting(line);
                }

                if (line.find("Radio is off") != std::string::npos) {
                    radio_control::vrc_log("Radio is off. Attempt factory reset.\n");
                    factory_reset = true;
                }
            }
        } catch (...) {
            std::exception_ptr p = std::current_exception();
            // Doesn't compile on NDK...
            radio_control::vrc_log("Caught exception in microhard handler thread.\n");
            // radio_control::vrc_log((p ? p.__cxa_exception_type()->name() :
            // "null")+
            // "\n");
        }
    }

    void MicrohardControl::ParseSetting(std::string raw) {
        std::istringstream blob(raw);
        std::string line;
        try {
            while (std::getline(blob, line) && keep_running) {
                if (line.find("+MWTXPOWER:") != std::string::npos) {
                    auto val = get_str_between_two_str(line, ": ", "\r");
                    if (val.size()) {
                        auto tokens = splitString(val, " ");
                        if (tokens.size() == 2) {
                            radio_info.tx_power = std::stoi(tokens[0]);
                            radio_control::vrc_log(std::string("Tx Power: ") +
                                                   std::to_string(radio_info.tx_power) + "\n");
                        }
                    }
                }
                else if (line.find("+MWFREQ:") != std::string::npos) {
                    auto val = get_str_between_two_str(line, ": ", "\r");
                    if (val.size()) {
                        auto tokens = splitString(val, " ");
                        if (tokens.size() == 4) {
                            radio_info.channel_freq = std::stoi(tokens[2]);
                            radio_control::vrc_log(std::string("Frequency: ") +
                                                   std::to_string(radio_info.channel_freq) + "\n");
                        } else if (tokens.size() == 2) {
                            radio_info.channel_freq = std::stoi(tokens[0]);
                            radio_control::vrc_log(std::string("Frequency: ") +
                                                   std::to_string(radio_info.channel_freq) + "\n");
                        }
                    }
                }
                else if (line.find("+MWFREQ1800:") != std::string::npos) {
                    auto val = get_str_between_two_str(line, ": ", "\r");
                    if (val.size()) {
                        auto tokens = splitString(val, " ");
                        if (tokens.size() == 4) {
                            radio_info.channel_freq = std::stoi(tokens[2]);
                            radio_control::vrc_log(std::string("Frequency: ") +
                                                   std::to_string(radio_info.channel_freq) + "\n");
                        }
                    }
                }
                else if (line.find("+MWFREQ2400:") != std::string::npos) {
                    auto val = get_str_between_two_str(line, ": ", "\r");
                    if (val.size()) {
                        auto tokens = splitString(val, " ");
                        if (tokens.size() == 4) {
                            radio_info.channel_freq = std::stoi(tokens[2]);
                            radio_control::vrc_log(std::string("Frequency: ") +
                                                   std::to_string(radio_info.channel_freq) + "\n");
                        }
                    }
                }
                else if (line.find("+MWBAND:") != std::string::npos) {
                    auto val = get_str_between_two_str(line, ": ", "\r");
                    if (val.size()) {
                        auto tokens = splitString(val, " ");
                        if (tokens.size() == 4) {
                            radio_info.channel_bw = std::stoi(tokens[2]);
                            radio_control::vrc_log(std::string("Bandwidth: ") +
                                                   std::to_string(radio_info.channel_bw) + "\n");
                        }
                    }
                }
                else if (line.find("Channel-bandwidth") != std::string::npos) {
                    auto val = get_str_between_two_str(line, ": ", "\r");
                    if (val.size()) {
                        auto tokens = splitString(val, " ");
                        if (tokens.size() == 4) {
                            radio_info.channel_bw = std::stoi(tokens[2]);
                            radio_control::vrc_log(std::string("Bandwidth: ") +
                                                   std::to_string(radio_info.channel_bw) + "\n");
                        }
                    }
                }
            }
        } catch (...) {
            // Make sure this doesn't throw an exception.
        }
    }

    MicrohardControl::RFInfo MicrohardControl::ParseInfo(std::string raw) {
        (void)raw;
        MicrohardControl::RFInfo info = {};
        std::istringstream blob(raw);
        std::string line;
        int snr = 0;
        bool got_snr = false;
        bool got_noisefloor = false;
        bool got_rssi = false;
        static int disconnect_timer = 0;
        try {
            while (std::getline(blob, line) && keep_running) {
                if (line.find("Frequency") != std::string::npos) {
                    auto val =
                            get_str_between_two_str(line, "Frequency          : ", " MHz");
                    if (val.size()) {
                        int frequency = std::stoi(val);
                        if (frequency > 0) {
                            radio_info.channel_freq = frequency;
                            radio_control::vrc_log(std::string("Radio Frequency: ") +
                                                   std::to_string(radio_info.channel_freq) +
                                                   "\n");
                        }
                    }
                }
                if (line.find("Operation Mode") != std::string::npos) {
                    auto pos = line.find_first_of(':');
                    if (pos != std::string::npos) {
                        auto mode = line.substr(pos+2);
                        if (mode.size()) {
                            if ((mode.find("Master") != std::string::npos) ||
                                (mode.find("Slave") != std::string::npos) ||
                                (mode.find("Mesh") != std::string::npos)) {
                                radio_info.operation_mode = line;
                                radio_control::vrc_log(line + "\n");
                            }
                        }
                    }
                }

                if (line.find("Network ID") != std::string::npos) {
                    auto val = get_str_between_two_str(line, "Network ID         : ", "\r");
                    // Strange Gen1 bug setting network ID to this string during scan
                    if (val.size() && (val.find("unknown#") == std::string::npos)) {
                        radio_info.network_id = val;
                        use_mesh = false;
                        radio_control::vrc_log(std::string("Network ID: ") +
                                               radio_info.network_id + "\n");
                    }
                }

                if (line.find("Mesh ID") != std::string::npos) {
                    auto val = get_str_between_two_str(line, "Mesh ID            : ", "\r");
                    if (val.size() && (val.find("unknown#") == std::string::npos)) {
                        radio_info.network_id = val;
                        use_mesh = true;
                        radio_control::vrc_log(std::string("Mesh ID: ") +
                                               radio_info.network_id + "\n");
                    } else {
                        // This is actually allowed...
                        radio_info.network_id = "";
                    }
                }

                // Status report the immediate power setting if overridden with AT+MWTXPOWERQ
                if (line.find("Tx Power") != std::string::npos) {
                    auto val =
                            get_str_between_two_str(line, "Tx Power           : ", " dBm");
                    if (val.size()) {
                        radio_info.tx_powerq = std::stoi(val);
                        if (radio_info.tx_powerq == radio_info.tx_power) {
                            radio_control::vrc_log(std::string("Tx Power: ") +
                                                   std::to_string(radio_info.tx_powerq) + "\n");
                        } else {
                            radio_control::vrc_log(std::string("Tx Power Immediate: ") +
                                                   std::to_string(radio_info.tx_powerq) + "\n");
                        }
                    }
                }
                if (line.find("Bandwidth") != std::string::npos) {
                    auto val =
                            get_str_between_two_str(line, "Bandwidth          : ", " MHz");
                    if (val.size()) {
                        radio_info.channel_bw = std::stoi(val);
                        radio_control::vrc_log(std::string("Bandwidth: ") +
                                               std::to_string(radio_info.channel_bw) + "\n");
                    }
                }
                if (line.find("  SNR") != std::string::npos) {
                    auto val = get_str_between_two_str(line, ": ", "\r");
                    if (val.size()) {
                        snr = std::stoi(val);
                        radio_control::vrc_log(std::string("SNR: ") + std::to_string(snr) +
                                               "\n");
                        got_snr = true;
                    }
                }
                if (line.find("  Noise Floor") != std::string::npos) {
                    auto val = get_str_between_two_str(line, ": ", "\r");
                    if (val.size()) {
                        radio_info.noise_floor = std::stoi(val);
                        radio_control::vrc_log(std::string("Noise Floor: ") +
                                               std::to_string(radio_info.noise_floor) + "\n");
                        got_noisefloor = true;
                    }
                }
                if (line.find("  RSSI") != std::string::npos) {
                    auto val = get_str_between_two_str(line, ": ", "\n");
                    if (val.size()) {
                        radio_info.rssi = std::stoi(val);
                        radio_control::vrc_log(std::string("RSSI: ") +
                                               std::to_string(radio_info.rssi) + "\n");
                        got_rssi = true;
                    }
                }
                if (!SupportsFeature(FEATURE_RATE_FLOOR)) {
                    if (line.find("  Tx Mod") != std::string::npos) {
                        auto val = get_str_between_two_str(line, ": ", "\n");
                        if (val.size()) {
                            auto mimo_start = val.find_first_of('(');
                            auto modulation = (mimo_start != std::string::npos) ? val.substr(0, mimo_start-1) : val;
                            ParseRateMod(modulation);
                        }
                    }
                }
            }
        } catch (...) {
            // Make sure this doesn't throw an exception.
        }
        if (got_rssi) {
            radio_control::vrc_log("Modem connected.\n");
            current_state = RadioState::CONNECTED;
            if (got_snr && !got_noisefloor) {
                radio_info.noise_floor = radio_info.rssi - snr;
            }
            disconnect_timer = 0;
        } else if (got_noisefloor && got_snr) {
            radio_control::vrc_log("Modem connected.\n");
            current_state = RadioState::CONNECTED;
            radio_info.rssi = radio_info.noise_floor + snr;
            radio_control::vrc_log(std::string("RSSI: ") +
                                   std::to_string(radio_info.rssi) + "\n");
            disconnect_timer = 0;
        } else {
            if (disconnect_timer > 4) {
                current_state = RadioState::DISCONNECTED;
            } else {
                disconnect_timer++;
            }
        }
        return info;
    }

    void MicrohardControl::ParseHopInfo(std::string raw) {
        std::istringstream blob(raw);
        std::string line;
        try {
            while (std::getline(blob, line) && keep_running) {
                if (!is_host) {
                    // first look for channel hop settings, then frequency list
                    if (radio_info.freq_hop_stat == -1 ||
                        radio_info.freq_hop_mode == -1) {
                        if (line.find("<Status>") != std::string::npos) {
                            auto val = get_str_between_two_str(line, "<Status>                   : ", "\r");
                            if (val.size()) {
                                if (val.find("Disable") != std::string::npos) {
                                    radio_info.freq_hop_stat = 0;
                                } else if (val.find("Enable") != std::string::npos) {
                                    radio_info.freq_hop_stat = 1;
                                }
                                radio_control::vrc_log(std::string("Frequency hopping: ") +
                                                       val + "\n");
                            }
                        } else if (line.find("<Frequency Selection>") != std::string::npos) {
                            auto val = get_str_between_two_str(line, "<Frequency Selection>      : ", "\r");
                            if (val.size()) {
                                radio_info.freq_list.clear();
                                if (val.find("List") != std::string::npos) {
                                    if (val.find("Seq") != std::string::npos) {
                                        radio_info.freq_hop_mode = 0;
                                    } else if (val.find("Random") != std::string::npos) {
                                        radio_info.freq_hop_mode = 1;
                                    }
                                } else if (val.find("Random Generated") != std::string::npos) {
                                    radio_info.freq_hop_mode = 2;
                                } else if (val.find("Dynamic Switch") != std::string::npos) {
                                    radio_info.freq_hop_mode = 3;
                                } else {
                                    radio_info.freq_hop_stat = -1;
                                }
                                if (radio_info.freq_hop_mode != -1)
                                    radio_control::vrc_log(std::string("Frequency Selection: ") +
                                                           val + "\n");
                            }
                        } else if (line[0] == '<') {
                            // Consume any remaining output from AT+MWCHANHOP output after status/mode determined
                        }
                    } else if (radio_info.freq_hop_mode >= 0 &&
                               radio_info.freq_hop_mode <= 3) {
                        if (line.size() && line[0] != '<') {
                            if (line.find("+MWFREQLIST: ") != std::string::npos) {
                                auto frequencies_string = get_str_between_two_str(line, "+MWFREQLIST: ", "\r");
                                if (frequencies_string.size()) {
                                    size_t pos=0;
                                    bool done_parsing = false;
                                    while (!done_parsing) {
                                        size_t end = frequencies_string.find_first_of(' ', pos);
                                        if (end == std::string::npos) {
                                            done_parsing = true;
                                            end = frequencies_string.size();
                                        }
                                        std::string frequency_string = frequencies_string.substr(pos, end-pos);
                                        if (isdigit(frequency_string[0])) {
                                            int frequency = std::atoi(frequency_string.c_str());
                                            radio_info.freq_list.push_back(frequency);
                                        }
                                        pos = end + 1;
                                    }
                                    if (radio_info.freq_list.size()) {
                                        radio_control::vrc_log(std::string("Frequency List: ") +
                                                               frequencies_string + "\n");
                                    }
                                }
                            } else if (line.find("ERROR") != std::string::npos) {
                                // ERROR : Failed to get Frequency List
                                // ERROR: Invalid command "AT+MWFREQLIST"
                                if (line.find("Failed to get") != std::string::npos) {
                                    radio_info.freq_hop_stat = 0;
                                }
                                if (line.find("Invalid command") != std::string::npos) {
                                    radio_info.freq_hop_stat = -1;
                                }
                                if (line.find("Not supported") != std::string::npos) {
                                    radio_info.freq_hop_stat = 0;
                                }
                            }
                        }
                    }
                } else {
                    // slave side only parse frequency list
                    if (radio_info.freq_hop_stat == -1) {
                        if (line.size() && line[0] != '<') {
                            if (line.find("+MWFREQLIST: ") != std::string::npos) {
                                auto frequencies_string = get_str_between_two_str(line, "+MWFREQLIST: ", "\r");
                                if (frequencies_string.size()) {
                                    size_t pos=0;
                                    bool done_parsing = false;
                                    radio_info.freq_list.clear();
                                    while (!done_parsing) {
                                        size_t end = frequencies_string.find_first_of(' ', pos);
                                        if (end == std::string::npos) {
                                            done_parsing = true;
                                            end = frequencies_string.size();
                                        }
                                        std::string frequency_string = frequencies_string.substr(pos, end-pos);
                                        if (isdigit(frequency_string[0])) {
                                            int frequency = std::atoi(frequency_string.c_str());
                                            radio_info.freq_list.push_back(frequency);
                                        }
                                        pos = end + 1;
                                    }
                                    if (radio_info.freq_list.size()) {
                                        radio_control::vrc_log(std::string("Frequency List: ") +
                                                               frequencies_string + "\n");
                                    }
                                }
                            } else if (line.find("ERROR") != std::string::npos) {
                                // ERROR : Failed to get Frequency List
                                // ERROR: Invalid command "AT+MWFREQLIST"
                                if (line.find("Failed to get") != std::string::npos) {
                                    radio_info.freq_hop_stat = 0;
                                }
                                if (line.find("Invalid command") != std::string::npos) {
                                    radio_info.freq_hop_stat = -1;
                                }
                            }
                        }
                    }
                }
            }
        } catch (...) {
            // Make sure this doesn't throw an exception.
        }
    }

    void MicrohardControl::ParseRateMod(std::string modulation) {
        if (modulation == "64-QAM FEC 5/6")
            radio_info.tx_modulation = 1;
        else if (modulation == "64-QAM FEC 3/4")
            radio_info.tx_modulation = 2;
        else if (modulation == "64-QAM FEC 2/3")
            radio_info.tx_modulation = 3;
        else if (modulation == "16-QAM FEC 3/4")
            radio_info.tx_modulation = 4;
        else if (modulation == "16-QAM FEC 1/2")
            radio_info.tx_modulation = 5;
        else if (modulation == "QPSK FEC 3/4")
            radio_info.tx_modulation = 6;
        else if (modulation == "QPSK FEC 1/2")
            radio_info.tx_modulation = 7;
        else if (modulation == "BPSK FEC 1/2")
            radio_info.tx_modulation = 8;
        else if (modulation == "BPSK FEC 1/2 x 2")
            radio_info.tx_modulation = 9;
        else if (modulation == "QPSK FEC 1/2 x 2")
            radio_info.tx_modulation = 10;
        else if (modulation == "QPSK FEC 3/4 x 2")
            radio_info.tx_modulation = 11;
        else if (modulation == "16-QAM FEC 1/2 x 2")
            radio_info.tx_modulation = 12;
        else
            radio_info.tx_modulation = -1;
        radio_control::vrc_log(std::string("Tx Mod: ") +
                               std::to_string(radio_info.tx_modulation) + "\n");
    }

    std::vector<std::tuple<int, float, int>>
    MicrohardControl::GetSupportedFreqAndMaxBWPer(void) {
        std::vector<std::tuple<int, float, int>> options;
        int step_size = 1;
        if (detected_model == RadioModel::PMDDL900) {
            // 901 - 980 MHz
            for (int center_freq = 906; center_freq <= 976; center_freq += step_size) {
                float bw_allowed = 4;
                if (((center_freq - 4) > 901) && ((center_freq + 4) < 980)) {
                    bw_allowed = 8;
                }
                options.push_back(
                        std::make_tuple(center_freq, bw_allowed, (center_freq - 901)));
            }
        } else if (detected_model == RadioModel::PDDL1800) {
            // 1810 - 1870 MHz
            for (int center_freq = 1812; center_freq <= 1870;
                 center_freq += step_size) {
                float bw_allowed = 1;
                if (((center_freq - 1) > 1812) && ((center_freq + 1) < 1870)) {
                    bw_allowed = 2;
                }
                if (((center_freq - 2) > 1812) && ((center_freq + 2) < 1870)) {
                    bw_allowed = 4;
                }
                if (((center_freq - 4) > 1812) && ((center_freq + 4) < 1870)) {
                    bw_allowed = 8;
                }
                options.push_back(
                        std::make_tuple(center_freq, bw_allowed, (center_freq - 1810)));
            }
        } else if (detected_model == RadioModel::PMDDL2450) {
            if (radio_variance == RadioVariance::PMDDL2450JP) {
                options.push_back(std::make_tuple(2486, 4, 1));
                options.push_back(std::make_tuple(2487, 4, 2));
                options.push_back(std::make_tuple(2488, 4, 3));
                options.push_back(std::make_tuple(2489, 8, 4));
                options.push_back(std::make_tuple(2490, 4, 5));
                options.push_back(std::make_tuple(2491, 4, 6));
            } else {
                // 2401 - 2480 MHz
                for (int center_freq = 2405; center_freq <= 2479;
                     center_freq += step_size) {
                    float bw_allowed = 4;
                    if (((center_freq - 4) > 2402) && ((center_freq + 4) < 2482)) {
                        bw_allowed = 8;
                    }
                    options.push_back(
                            std::make_tuple(center_freq, bw_allowed, (center_freq - 2401)));
                }
            }
        } else if ((detected_model == RadioModel::FDDL1624) ||
                   (detected_model == RadioModel::PMDDL1624)) {
            // 1625 - 1725 MHz
            //  Tested setting on FDDL at 1625 and 1725 at 1 MHz
            for (int center_freq = 1625; center_freq <= 1725;
                 center_freq += step_size) {
                float bw_allowed = 1;
                if (((center_freq - 1) >= 1625) && ((center_freq + 1) <= 1725)) {
                    bw_allowed = 2;
                }
                if (((center_freq - 2) >= 1625) && ((center_freq + 2) <= 1725)) {
                    bw_allowed = 4;
                }
                if (((center_freq - 4) >= 1625) && ((center_freq + 4) <= 1725)) {
                    bw_allowed = 8;
                }
                // No notion of a "channel number" on the radio
                options.push_back(std::make_tuple(center_freq, bw_allowed, center_freq));
            }
            // 1780 - 1850 MHz
            //  Tested setting on FDDL at 1780 and 1850 at 1 MHz
            for (int center_freq = 1780; center_freq <= 1850;
                 center_freq += step_size) {
                float bw_allowed = 1;
                if (((center_freq - 1) >= 1780) && ((center_freq + 1) <= 1850)) {
                    bw_allowed = 2;
                }
                if (((center_freq - 2) >= 1780) && ((center_freq + 2) <= 1850)) {
                    bw_allowed = 4;
                }
                if (((center_freq - 4) >= 1780) && ((center_freq + 4) <= 1850)) {
                    bw_allowed = 8;
                }
                // No notion of a "channel number" on the radio
                options.push_back(std::make_tuple(center_freq, bw_allowed, center_freq));
            }
            // 2020 - 2110 MHz
            //  Tested setting on FDDL at 2020 and 2110 at 1 MHz
            for (int center_freq = 2020; center_freq <= 2110;
                 center_freq += step_size) {
                float bw_allowed = 1;
                if (((center_freq - 1) >= 2020) && ((center_freq + 1) <= 2110)) {
                    bw_allowed = 2;
                }
                if (((center_freq - 2) >= 2020) && ((center_freq + 2) <= 2110)) {
                    bw_allowed = 4;
                }
                if (((center_freq - 4) >= 2020) && ((center_freq + 4) <= 2110)) {
                    bw_allowed = 8;
                }
                // No notion of a "channel number" on the radio
                options.push_back(std::make_tuple(center_freq, bw_allowed, center_freq));
            }
            // 2200 - 2300 MHz
            //  Tested setting on FDDL at 2200 and 2300 at 1 MHz
            for (int center_freq = 2200; center_freq <= 2300;
                 center_freq += step_size) {
                float bw_allowed = 1;
                if (((center_freq - 1) >= 2200) && ((center_freq + 1) <= 2300)) {
                    bw_allowed = 2;
                }
                if (((center_freq - 2) >= 2200) && ((center_freq + 2) <= 2300)) {
                    bw_allowed = 4;
                }
                if (((center_freq - 4) >= 2200) && ((center_freq + 4) <= 2300)) {
                    bw_allowed = 8;
                }
                // No notion of a "channel number" on the radio
                options.push_back(std::make_tuple(center_freq, bw_allowed, center_freq));
            }
            // 2301 - 2390 MHz
            //  Tested setting on FDDL at 2301 and 2390 at 1 MHz
            for (int center_freq = 2301; center_freq <= 2390;
                 center_freq += step_size) {
                float bw_allowed = 1;
                if (((center_freq - 1) >= 2301) && ((center_freq + 1) <= 2390)) {
                    bw_allowed = 2;
                }
                if (((center_freq - 2) >= 2301) && ((center_freq + 2) <= 2390)) {
                    bw_allowed = 4;
                }
                if (((center_freq - 4) >= 2301) && ((center_freq + 4) <= 2390)) {
                    bw_allowed = 8;
                }
                // No notion of a "channel number" on the radio
                options.push_back(std::make_tuple(center_freq, bw_allowed, center_freq));
            }
            // 2400 - 2500 MHz
            //  Tested setting on FDDL at 2400 and 2500 at 1 MHz
            for (int center_freq = 2400; center_freq <= 2500;
                 center_freq += step_size) {
                float bw_allowed = 1;
                if (((center_freq - 1) >= 2400) && ((center_freq + 1) <= 2500)) {
                    bw_allowed = 2;
                }
                if (((center_freq - 2) >= 2400) && ((center_freq + 2) <= 2500)) {
                    bw_allowed = 4;
                }
                if (((center_freq - 4) >= 2400) && ((center_freq + 4) <= 2500)) {
                    bw_allowed = 8;
                }
                // No notion of a "channel number" on the radio
                options.push_back(std::make_tuple(center_freq, bw_allowed, center_freq));
            }
        }

        return options;
    }

    std::vector<float> MicrohardControl::GetSupportedBWs(void) {
        std::vector<float> bws;
        if (detected_model == RadioModel::PMDDL1624 ||
            detected_model == RadioModel::FDDL1624 ||
            detected_model == RadioModel::PMDDL1621 ||
            detected_model == RadioModel::PMDDL1622 ||
            detected_model == RadioModel::PDDL1800 ||
            detected_model == RadioModel::PMDDL900) {
            bws.push_back(1);
            bws.push_back(2);
        }
        bws.push_back(4);
        bws.push_back(8);
        return bws;
    }

    int MicrohardControl::SetFrequencyAndBW(int freq, float bw) {
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
            std::lock_guard<std::mutex> lock(config_queue_mutex);
            QueueItem item = {Action::ACTION_CHANGE_FREQUENCY,
                              std::make_tuple(freq, bw, channel_number)};
            config_queue.push_back(item);
            return 0;
        } else {
            if (!freq_ok) {
                radio_control::vrc_log("Invalid frequency\n");
            } else if (!bw_ok) {
                radio_control::vrc_log("Bandwidth is unsupported\n");
            }
            return -1;
        }
    }

    int MicrohardControl::SetFrequencyList(
            const std::vector<int>& freq_list,
            float bw) {
        int results = -1;
        if (SupportsFrequencyHopping()) {
            bool bw_ok, freqs_ok = false;
            int channel_number = 0;

            if (!freq_list.empty()) {
                for (auto freq : freq_list) {
                    freqs_ok = false;
                    for (auto freq_avail : GetSupportedFreqAndMaxBWPer()) {
                        if (freq == std::get<0>(freq_avail) &&
                            bw <= static_cast<float>(std::get<1>(freq_avail))) {
                            freqs_ok = true;
                            channel_number = std::get<2>(freq_avail);
                            break;
                        }
                    }
                    if (!freqs_ok) break;
                }

                for (auto bw_available : GetSupportedBWs()) {
                    if (bw == static_cast<float>(bw_available)) {
                        bw_ok = true;
                        break;
                    }
                }
            } else {
                freqs_ok = true;
                bw_ok = true;
            }

            if (freqs_ok && bw_ok) {
                radio_info.freq_list = freq_list;
                std::lock_guard<std::mutex> lock(config_queue_mutex);
                QueueItem item = {Action::ACTION_CHANGE_FREQUENCY_LIST,
                                  std::make_tuple(freq_list, bw, channel_number)};
                config_queue.push_back(item);
                results = 0;
            } else if (!freqs_ok) {
                radio_control::vrc_log("Frequency list has invalid frequency\n");
            } else if (!bw_ok) {
                radio_control::vrc_log("Frequency list bandwidth is unsupported\n");
            }
        } else {
            radio_control::vrc_log("Frequency hopping unsupported\n");
        }
        return results;
    }

    int MicrohardControl::SetFrequencyHopping(
            uint8_t selection_mode,
            uint8_t dynamic_mode,
            uint32_t channel_interval,
            uint32_t switch_interval,
            uint32_t announce_times,
            uint32_t bands_selected,
            bool recovery) {
        int results = -1;
        if (SupportsFrequencyHopping()) {
            bool freqs_ok, bands_ok = false;

            if ((selection_mode > 3) || (dynamic_mode > 1))
                return results;

            // Bands is bitmask of frequency bands. If zero, then
            // must have set a frequency list.
            if (bands_selected != 0) {
                uint32_t num_bands = 6;   //TODO: parameter based on model
                uint32_t bands_mask = (1 << num_bands) - 1;
                bands_ok = ((bands_selected & ~bands_mask) == 0);
                freqs_ok = bands_ok;
            } else {
                auto& freq_list = GetFrequencyList();
                freqs_ok = !freq_list.empty();
                bands_ok = freqs_ok;
            }

            if (freqs_ok && bands_ok) {
                std::lock_guard<std::mutex> lock(config_queue_mutex);
                QueueItem item = {Action::ACTION_CHANGE_FREQUENCY_HOPPING,
                                  std::make_tuple(selection_mode,
                                                  dynamic_mode,
                                                  channel_interval,
                                                  switch_interval,
                                                  announce_times,
                                                  bands_selected,
                                                  recovery)};
                config_queue.push_back(item);
                results = 0;
            }
        }
        return results;
    }

    void MicrohardControl::SetNetworkID(std::string id) {
        std::lock_guard<std::mutex> lock(config_queue_mutex);
        QueueItem item = {Action::ACTION_CHANGE_NETWORK_ID, id};
        config_queue.push_back(item);
    }

    void MicrohardControl::SetNetworkPassword(std::string pw) {
        std::lock_guard<std::mutex> lock(config_queue_mutex);
        QueueItem item = {Action::ACTION_CHANGE_PASSWORD, pw};
        config_queue.push_back(item);
    }

    void MicrohardControl::SetNetworkIPAddr(std::string ipaddr) {
        if (ipaddr != config_ip_network) {
            // Verify network IP address on same subnet
            struct sockaddr_in sa_radio_ip_config;
            inet_pton(AF_INET, microhard_address.c_str(),
                      &(sa_radio_ip_config.sin_addr));
            sa_radio_ip_config.sin_addr.s_addr &= 0x00FFFFFF;

            // store this IP address in sa:
            struct sockaddr_in sa_local_ip_config;
            inet_pton(AF_INET, ipaddr.c_str(), &(sa_local_ip_config.sin_addr));
            sa_local_ip_config.sin_addr.s_addr &= 0x00FFFFFF;

            // As long as these are on same subnet
            if (sa_local_ip_config.sin_addr.s_addr ==
                sa_radio_ip_config.sin_addr.s_addr) {
                config_ip_network = ipaddr;
            }
        }
    }

    void MicrohardControl::SetRadioIPAddr(std::string ipaddr) {
        if (ipaddr != config_ip_radio) {
            if (microhard_address.empty()) {
                config_ip_radio = ipaddr;
            } else {
                // Verify network IP address on same subnet
                struct sockaddr_in sa_radio_ip_config;
                inet_pton(AF_INET, microhard_address.c_str(),
                          &(sa_radio_ip_config.sin_addr));
                sa_radio_ip_config.sin_addr.s_addr &= 0x00FFFFFF;

                // store this IP address in sa:
                struct sockaddr_in sa_local_ip_config;
                inet_pton(AF_INET, ipaddr.c_str(), &(sa_local_ip_config.sin_addr));
                sa_local_ip_config.sin_addr.s_addr &= 0x00FFFFFF;

                // As long as these are on same subnet
                if (sa_local_ip_config.sin_addr.s_addr ==
                    sa_radio_ip_config.sin_addr.s_addr) {
                    config_ip_radio = ipaddr;
                    factory_reset = true;
                }
            }
        }
    }

    void MicrohardControl::ApplySettings(void) {
        std::lock_guard<std::mutex> lock(config_queue_mutex);
        QueueItem item = {Action::ACTION_RESTART_SERVICES, nullptr};
        config_queue.push_back(item);
    }

    void MicrohardControl::SetOutputPower(int dbm) {
        std::lock_guard<std::mutex> lock(config_queue_mutex);
        QueueItem item = {Action::ACTION_SET_OUTPUT_POWER, dbm};
        config_queue.push_back(item);
    }

    void MicrohardControl::SetRateImmediate(int rate) {
        std::lock_guard<std::mutex> lock(config_queue_mutex);
        QueueItem item = {Action::ACTION_SET_AIR_RATE_IMMEDIATE, rate};
        config_queue.push_back(item);
    }

    void MicrohardControl::SetOutputPowerImmediate(int dbm) {
        std::lock_guard<std::mutex> lock(config_queue_mutex);
        QueueItem item = {Action::ACTION_SET_OUTPUT_POWER_IMMEDIATE, dbm};
        config_queue.push_back(item);
    }

    void MicrohardControl::SetMIMO(bool mimo_active) {
        std::lock_guard<std::mutex> lock(config_queue_mutex);
        QueueItem item = {Action::ACTION_SET_MIMO, mimo_active};
        config_queue.push_back(item);
    }

    void MicrohardControl::SetTopology(Topology topology) {
        std::lock_guard<std::mutex> lock(config_queue_mutex);
        QueueItem item = {Action::ACTION_SET_TOPOLOGY, topology};
        config_queue.push_back(item);
        topology_change = static_cast<int>(topology);
    }

    void MicrohardControl::SetBufferSize(int size) {
        std::lock_guard<std::mutex> lock(config_queue_mutex);
        QueueItem item = {Action::ACTION_SET_BUFFER_SIZE, size};
        config_queue.push_back(item);
    }

    void MicrohardControl::SetRateFloor(int rate) {
        std::lock_guard<std::mutex> lock(config_queue_mutex);
        QueueItem item = {Action::ACTION_SET_RATE_FLOOR, rate};
        config_queue.push_back(item);
    }

    void MicrohardControl::SetRateCeiling(int rate) {
        std::lock_guard<std::mutex> lock(config_queue_mutex);
        QueueItem item = {Action::ACTION_SET_RATE_CEILING, rate};
        config_queue.push_back(item);
    }

    void MicrohardControl::SetBeaconLossCount(int count) {
        std::lock_guard<std::mutex> lock(config_queue_mutex);
        QueueItem item = {Action::ACTION_SET_BEACON_LOSS_COUNT, count};
        config_queue.push_back(item);
    }

    bool MicrohardControl::SocketConnect(const int &sock,
                                         const std::string &air_ip) {
        uint16_t microhard_settings_port = 23;

        fcntl(sock, F_SETFL, O_NONBLOCK);
        const int enable = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int));
        struct sockaddr_in serv_addr;
        memset(&serv_addr, '0', sizeof(serv_addr));
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port = htons(microhard_settings_port);
        if (inet_pton(AF_INET, air_ip.c_str(), &serv_addr.sin_addr) > 0) {
            int retval = connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr));
            if (retval == 0) {
                radio_control::vrc_log("connection completed\n");
                return true;
            } else if (retval < 0) {
                if (errno == EINPROGRESS || errno == EAGAIN) {
                    fd_set fdset;
                    struct timeval tv;
                    FD_ZERO(&fdset);
                    FD_SET(sock, &fdset);
                    tv.tv_sec = 3; /* 3 second timeout */
                    tv.tv_usec = 0;

                    int retries = 0;
                    while (retries < 2) {
                        retval = select(sock + 1, NULL, &fdset, NULL, &tv);
                        if (retval == 1) {
                            int so_error = 0;
                            socklen_t len = sizeof so_error;
                            getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &len);
                            if (so_error == 0) {
                                radio_control::vrc_log("connection completed\n");
                                return true;
                            } else {
                                if (so_error == EHOSTUNREACH) {
                                    radio_control::vrc_log("Host unreachable\n");
                                } else {
                                    radio_control::vrc_log("getsockopt errno: ");
                                    radio_control::vrc_log(std::to_string(so_error) + "\n");
                                }
                                return false;
                            }
                        } else if (retval <= 0) {
                            if (errno == EINPROGRESS || errno == ETIMEDOUT) {
                                radio_control::vrc_log("connection in progress\n");
                                retries++;
                            } else {
                                radio_control::vrc_log("select errno: ");
                                radio_control::vrc_log(std::to_string(errno) + "\n");
                                return false;
                            }
                        }
                    }
                } else {
                    radio_control::vrc_log("connect errno: ");
                    radio_control::vrc_log(std::to_string(errno) + "\n");
                }
            }
        }

        return false;
    }

    std::string
    MicrohardControl::SelectMicrohardIP(const std::vector<std::string> &ips,
                                        const std::string &ipconfig) {

        std::string microhard_address("");

        // Method here only assumes 2 microhards. For mesh we'll need to sort the
        // list and add some more logic here.
        // TODO: This is limited and situationally correct.
        std::string host_radio_ip = microhard_subnet + ".104";
        if (ips.size() == 1) {
            microhard_address = ips.at(0);
        } else if (ips.size() >= 2) {
            // HACK - Just assume we're good if we're already connected...
            // This is a departure from what we've done previously where we
            // would reset it anyway.
            if (is_host) {
                if (ipconfig.empty()) {
                    microhard_address = host_radio_ip;
                } else {
                    microhard_address = ipconfig;
                }
            } else {
                // Prefer match to configured radio IP if discovered
                if (!config_ip_radio.empty()) {
                    for (auto& ip : ips) {
                        if (ip == config_ip_radio) {
                            microhard_address = ip;
                        }
                    }
                }

                // Match IP that is not hosts
                if (microhard_address.empty()) {
                    if (ipconfig.empty()) {
                        if (ips.at(0) != host_radio_ip)
                            microhard_address = ips.at(0);
                        else if (ips.at(1) != host_radio_ip)
                            microhard_address = ips.at(1);
                        else
                            microhard_address = "192.168.20.105";
                    } else {
                        microhard_address = ipconfig;
                    }
                }
            }
        }
        return microhard_address;
    }

    void MicrohardControl::Breadcrumb(bool leave) {
#ifdef BUILD_WITH_RDB
        const char *breadcrumb_file = "/tmp/microhard_discovered_by_rcs";
  std::ifstream ibc(breadcrumb_file, std::ios::in);
  bool exists = ibc.good();
  if (leave && !exists) {
    double uptime_seconds;
    std::ifstream("/proc/uptime", std::ios::in) >> uptime_seconds;

    std::ofstream obc;
    obc.open(breadcrumb_file, std::ios::out);
    obc << uptime_seconds << std::endl;
  } else if (exists && !leave) {
    std::remove(breadcrumb_file);
  }
#else
        (void)leave;
#endif
    }

    std::vector<int> MicrohardControl::ScanChannels(int sort, int count, float bw) {
        std::vector<int> frequency_list;

        if (count > 0 &&
            (current_state == RadioState::CONNECTED ||
             current_state == RadioState::DISCONNECTED)) {

            MonitorMicrohardState state = MonitorMicrohardState::LOGIN;
            std::string command;

            // Connect to microhard with separate connection
            // to run interference scan
            int scan_sock = socket(AF_INET, SOCK_STREAM, 0);
            if (SocketConnect(scan_sock, microhard_address)) {
                std::string output;
                std::string command;
                bool scan_done = false;
                bool scan_requested = false;
                bool scan_started = false;
                char buffer[4096] = {};

                // Filter frequencies to those supported for current bandwidth
                auto supported_freq = GetSupportedFreqAndMaxBWPer();
                auto supported_bw = static_cast<int>(bw);
                auto current_bw = GetCurrentBW();
                int scanning_bw = -1;

                // Bandwidth MHz to argument mapping
                auto bandwidthArg = [](int bandwidth) -> int {
                    int mwband_arg = 0;
                    if (bandwidth == 8) {
                        mwband_arg = 0;
                    } else if (bandwidth == 4) {
                        mwband_arg = 1;
                    } else if (bandwidth == 2) {
                        mwband_arg = 2;
                    } else if (bandwidth == 1) {
                        mwband_arg = 3;
                    }
                    return mwband_arg;
                };

                // Workaround to pMDDL2450 bug hanging on AT+MWINTFSCAN if bandwidth != 4MHz
                // TODO: DOING MORE HARM THAN GOOD
                //if (detected_model == RadioModel::PMDDL2450) {
                //  if (current_bw == 8 && current_state == radio_control::RadioState::CONNECTED) {
                //    scanning_bw = bandwidthArg(4);
                //  }
                //}

                using ScanReturn = std::vector<std::tuple<int,int>>;
                ScanReturn scan_data;

                int wait_for_scan_response = 0;
                int settings_step = 0;
                int retries = 0;

                while (keep_running && !scan_done) {
                    memset(buffer, 0, sizeof(buffer));
                    ssize_t n = read(scan_sock, buffer, sizeof(buffer));
                    if (n < 1) {
                        if (n == 0) {
                            radio_control::vrc_log("Orderly TCP disconnect detected on scan socket.\n");
                            shutdown(scan_sock, SHUT_RDWR);
                            close(scan_sock);
                            scan_sock = 0;
                            break;
                        } else {
                            if (errno == EAGAIN) {
                                constexpr int scan_wait_ms = 500;
                                std::this_thread::sleep_for(std::chrono::milliseconds(scan_wait_ms));
                                if (wait_for_scan_response >= 25000) {
                                    radio_control::vrc_log("Timed out waiting for scan results\n");
                                    scan_done = true;
                                } else {
                                    radio_control::vrc_log("Waiting for channel scan response\n");
                                    std::this_thread::sleep_for(std::chrono::milliseconds(scan_wait_ms));
                                    wait_for_scan_response += scan_wait_ms;
                                }
                            } else {
                                radio_control::vrc_log("read error: " + std::string(strerror(errno)) + "\n");
                                scan_done = true;
                            }
                        }
                        continue;
                    }

                    output += buffer;
                    wait_for_scan_response = 0;
                    switch (state) {
                        case MonitorMicrohardState::LOGIN:
                            if (output.find("login:") != std::string::npos) {
                                output.clear();
                                command = "admin\n";
                                if (send(scan_sock, command.c_str(), command.length(), 0) == -1) {
                                    state = MonitorMicrohardState::LOGIN;
                                    break;
                                }
                                state = MonitorMicrohardState::PASSWORD;
                            }
                            std::this_thread::sleep_for(std::chrono::milliseconds(10));
                            break;

                        case MonitorMicrohardState::PASSWORD:
                            if (output.find("Password:") != std::string::npos) {
                                output.clear();
                                std::string password("microhardsrm");
                                command = password + "\n";
                                if (send(scan_sock, command.c_str(), command.length(), 0) == -1) {
                                    state = MonitorMicrohardState::LOGIN;
                                    break;
                                }
                                scan_requested = false;
                                if (scanning_bw >= 0) {
                                    state = MonitorMicrohardState::MODEM_SETTINGS;
                                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                                } else {
                                    state = MonitorMicrohardState::CHANNEL_SCAN;
                                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                                }
                            }
                            break;

                            // Dealing with 2450 scan issues when connected
                        case MonitorMicrohardState::MODEM_SETTINGS: {
                            if (settings_step == 0 && output.find(">") != std::string::npos) {
                                output.clear();
                                command = "AT+MWBAND=";
                                command += std::to_string(scanning_bw);
                                command += "\n";
                                if (send(scan_sock, command.c_str(), command.length(), 0) == -1) {
                                    radio_control::vrc_log("Failed selecting bandwidth for interference scan\n");
                                    scan_done = true;
                                    break;
                                }
                                settings_step += 1;
                                scan_requested = false;
                                state = MonitorMicrohardState::MODEM_SETTINGS;
                                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                            }

                            if (settings_step == 1 && output.find(">") != std::string::npos) {
                                output.clear();
                                command = "AT+MWBANDQ=";
                                command += std::to_string(scanning_bw);
                                command += "\n";
                                if (send(scan_sock, command.c_str(), command.length(), 0) == -1) {
                                    radio_control::vrc_log("Failed selecting bandwidthq for interference scan\n");
                                    scan_done = true;
                                    break;
                                }
                                settings_step += 1;
                                retries = 0;
                                state = MonitorMicrohardState::MODEM_SETTINGS;
                                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                            }

                            if (settings_step == 2) {
                                output.clear();
                                command = "AT+MWBAND\n";
                                if (send(scan_sock, command.c_str(), command.length(), 0) == -1) {
                                    radio_control::vrc_log("Failed waiting for bandwidth\n");
                                    scan_done = true;
                                    break;
                                }

                                if (GetCurrentBW() == supported_bw) {
                                    settings_step += 1;
                                    scan_requested = false;
                                    state = MonitorMicrohardState::CHANNEL_SCAN;
                                } else if (++retries > 10) {
                                    radio_control::vrc_log("Failed confirming bandwidth for interference scan\n");
                                    scan_done = true;
                                }
                                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                            }
                            break;
                        }
                        case MonitorMicrohardState::CHANNEL_SCAN: {
                            if (!scan_requested) {
                                if (output.find(">") != std::string::npos) {
                                    output.clear();
                                    command = "AT+MWINTFSCAN=";
                                    command += (sort == 1) ? "1" : "0";
                                    command += "\n";
                                    if (send(scan_sock, command.c_str(), command.length(), 0) == -1) {
                                        radio_control::vrc_log("Failed sending interference scan\n");
                                        scan_done = true;
                                        break;
                                    }
                                    scan_requested = true;
                                } else if (output.find("incorrect") != std::string::npos) {
                                    output.clear();
                                    state = MonitorMicrohardState::LOGIN;
                                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                                    continue;
                                }
                            } else {
                                // Parse responses we're interested in.
                                try {
                                    std::istringstream blob(output);
                                    std::string line;
                                    while (std::getline(blob, line) && keep_running && !scan_done) {
                                        if (!scan_started && line.find("FREQ") != std::string::npos) {
                                            radio_control::vrc_log("Got start of channel scan.\n");
                                            scan_started = true;
                                        } else if (scan_started && line.find("OK") != std::string::npos) {
                                            radio_control::vrc_log("Got end of channel scan.\n");
                                            scan_started = false;
                                            scan_done = true;
                                        } else if (scan_started) {
                                            // parse and save in scan data vector
                                            std::stringstream ss(line);
                                            int frequency;
                                            int level_avg;
                                            int level_max;
                                            int activity;
                                            ss >> frequency;
                                            ss >> level_avg;
                                            ss >> level_max;
                                            ss >> activity;
                                            int sort_value = 0;
                                            switch (sort) {
                                                case 0:
                                                    sort_value = frequency;
                                                    break;
                                                case 1:
                                                    sort_value = level_avg;
                                                    break;
                                                case 2:
                                                    sort_value = level_max;
                                                    break;
                                                case 3:
                                                    sort_value = activity;
                                                    break;
                                                default:
                                                    sort_value = level_max;
                                                    break;
                                            }
                                            //radio_control::vrc_log("scan data" + std::to_string(frequency) + " " + std::to_string(sort_value) + "\n");
                                            if (frequency > 100)
                                                scan_data.push_back({ frequency, sort_value });
                                        }
                                    }
                                    if (scan_started)
                                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                                    else
                                        std::this_thread::sleep_for(std::chrono::milliseconds(100));

                                    if (scan_done) {
                                        if (!scan_data.empty()) {
                                            size_t top_count = (count != -1) ? count : scan_data.size();

                                            // Microhard either sorts by LVL_AVG or unsorted
                                            if (sort != 1) {  // not sorted
                                                auto sort_lowest = [](int a, int b, const ScanReturn& value) -> bool {
                                                    return std::get<1>(value[a]) < std::get<1>(value[b]);
                                                };

                                                // Sort scan data
                                                std::vector<int> level_rank(scan_data.size());
                                                std::iota(std::begin(level_rank), std::end(level_rank), 0);
                                                std::sort(std::begin(level_rank), std::end(level_rank), std::bind(sort_lowest, std::placeholders::_1, std::placeholders::_2, scan_data));

                                                // Return top frequencies from scan results
                                                for (auto rank : level_rank) {
                                                    int frequency = std::get<0>(scan_data[rank]);
                                                    for (auto freq_avail : supported_freq) {
                                                        if (frequency == std::get<0>(freq_avail) &&
                                                            supported_bw <= static_cast<int>(std::get<1>(freq_avail))) {
                                                            frequency_list.push_back(frequency);
                                                            break;
                                                        }
                                                    }
                                                    if (frequency_list.size() == top_count)
                                                        break;
                                                }
                                            } else {
                                                // Return top frequencies, presorted by Microhard
                                                for (auto& scan : scan_data) {
                                                    int frequency = std::get<0>(scan);
                                                    for (auto freq_avail : supported_freq) {
                                                        if (frequency == std::get<0>(freq_avail) &&
                                                            supported_bw <= static_cast<int>(std::get<1>(freq_avail))) {
                                                            frequency_list.push_back(frequency);
                                                            break;
                                                        }
                                                    }
                                                    if (frequency_list.size() == top_count)
                                                        break;
                                                }
                                            }
                                        }

                                        // Was bandwidth modified for scan
                                        if (scanning_bw >= 0) {
                                            // Restore current bandwidth
                                            state = MonitorMicrohardState::DONE;
                                            settings_step = 0;
                                            scan_done = false;
                                        }
                                    }
                                } catch (...) {
                                    std::exception_ptr p = std::current_exception();
                                    radio_control::vrc_log("Caught exception in channel scanner thread.\n");
                                    scan_done = true;
                                    scan_requested = false;
                                }
                                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                            }
                            break;
                        }
                        case MonitorMicrohardState::DONE: {
                            // Only falls into this state if bandwidth changed before scan
                            int restore_bw = bandwidthArg(static_cast<int>(current_bw));
                            if (settings_step == 0 && output.find(">") != std::string::npos) {
                                output.clear();
                                command = "AT+MWBAND=";
                                command += std::to_string(restore_bw);
                                command += "\n";
                                if (send(scan_sock, command.c_str(), command.length(), 0) == -1) {
                                    radio_control::vrc_log("Failed restoring bandwidth for interference scan\n");
                                    scan_done = true;
                                    break;
                                }
                                settings_step += 1;
                                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                            }

                            if (settings_step == 1 && output.find(">") != std::string::npos) {
                                output.clear();
                                command = "AT+MWBANDQ=";
                                command += std::to_string(restore_bw);
                                command += "\n";
                                if (send(scan_sock, command.c_str(), command.length(), 0) == -1) {
                                    radio_control::vrc_log("Failed restoring bandwidth for interference scan\n");
                                    scan_done = true;
                                    break;
                                }
                                scan_done = true;
                            }
                            break;
                        }
                        default:
                            scan_done = true;
                            break;
                    }
                }
                close(scan_sock);
            } else {
                std::vector<int> empty;
                radio_control::vrc_log("Failed opening second connection to radio to perform channel scan\n");
            }
        }

        return frequency_list;
    }

    std::vector<int> MicrohardControl::ScanChannels1800(int band_to_scan, int count, float bw) {
        (void)bw;
        std::vector<int> frequency_list;
        constexpr int frequency_bands = 3;

        if (count > 0 &&
            (current_state == RadioState::CONNECTED ||
             current_state == RadioState::DISCONNECTED)) {

            // Connect to microhard with separate connection
            // to run interference scan
            int scan_sock = socket(AF_INET, SOCK_STREAM, 0);
            if (SocketConnect(scan_sock, microhard_address)) {
                MonitorMicrohardState state = MonitorMicrohardState::LOGIN;
                std::string output;
                std::string command;
                bool scan_done = false;
                bool scan_requested = false;
                bool scan_started = false;
                char buffer[4096] = {};

                // Filter frequencies to those supported for current bandwidth
                auto supported_freq = GetSupportedFreqAndMaxBWPer();
                auto supported_bw = static_cast<int>(bw);
                //auto current_bw = GetCurrentBW();

                int wait_for_scan_response = 0;

                // Gen1 pDDL1800 software has its own AT+MWINTFSCAN=<band> format
                // Need to run three scans on each band.  Levels are not reported
                // so we will assume results are pre-ranked best to worst
                while (keep_running && !scan_done) {
                    memset(buffer, 0, sizeof(buffer));
                    ssize_t n = read(scan_sock, buffer, sizeof(buffer));
                    if (n < 1) {
                        if (n == 0) {
                            radio_control::vrc_log("Orderly TCP disconnect detected on scan socket.\n");
                            shutdown(scan_sock, SHUT_RDWR);
                            close(scan_sock);
                            scan_sock = 0;
                            break;
                        } else {
                            if (errno == EAGAIN) {
                                constexpr int scan_wait_ms = 500;
                                if (wait_for_scan_response >= 25000) {
                                    radio_control::vrc_log("Timed out waiting for scan results\n");
                                    scan_done = true;
                                } else {
                                    radio_control::vrc_log("Waiting for channel scan response\n");
                                    std::this_thread::sleep_for(std::chrono::milliseconds(scan_wait_ms));
                                    wait_for_scan_response += scan_wait_ms;
                                }
                            } else {
                                radio_control::vrc_log("read error: " + std::string(strerror(errno)) + "\n");
                                scan_done = true;
                            }
                        }
                        continue;
                    }

                    wait_for_scan_response = 0;
                    output += buffer;
                    switch (state) {
                        case MonitorMicrohardState::LOGIN:
                            if (output.find("login:") != std::string::npos) {
                                command = "admin\n";
                                if (send(scan_sock, command.c_str(), command.length(), 0) == -1) {
                                    state = MonitorMicrohardState::LOGIN;
                                    break;
                                }
                                state = MonitorMicrohardState::PASSWORD;
                            }
                            std::this_thread::sleep_for(std::chrono::milliseconds(10));
                            break;

                        case MonitorMicrohardState::PASSWORD:
                            if (output.find("Password:") != std::string::npos) {
                                std::string password("microhardsrm");
                                command = password + "\n";
                                if (send(scan_sock, command.c_str(), command.length(), 0) == -1) {
                                    state = MonitorMicrohardState::LOGIN;
                                    break;
                                }
                                state = MonitorMicrohardState::CHANNEL_SCAN;
                                scan_requested = false;
                            }
                            std::this_thread::sleep_for(std::chrono::milliseconds(10));
                            break;

                        case MonitorMicrohardState::CHANNEL_SCAN: {
                            if (band_to_scan < frequency_bands) {
                                if (!scan_requested) {
                                    if (output.find(">") != std::string::npos) {
                                        command = "AT+MWINTFSCAN=";
                                        command += std::to_string(band_to_scan);
                                        command += "\n";
                                        if (send(scan_sock, command.c_str(), command.length(), 0) == -1) {
                                            radio_control::vrc_log("Failed sending interference scan\n");
                                            scan_done = true;
                                            break;
                                        }
                                        scan_requested = true;
                                        scan_started = false;
                                    } else if (output.find("ERROR") != std::string::npos) {
                                        state = MonitorMicrohardState::LOGIN;
                                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                                        continue;
                                    }
                                } else {
                                    // Parse responses we're interested in.
                                    try {
                                        std::istringstream blob(output);
                                        std::string line;
                                        while (std::getline(blob, line) && keep_running && !scan_done) {
                                            if (line.find("18") == 0) {
                                                if (!scan_started) {
                                                    radio_control::vrc_log("Got start of band #" +
                                                                           std::to_string(band_to_scan) + " channel scan.\n");
                                                    scan_started = true;
                                                }

                                                if (scan_started && frequency_list.size() < static_cast<size_t>(count)) {
                                                    // parse and add to list if supported frequency/bw pair
                                                    int frequency = std::atoi(line.c_str());
                                                    for (auto freq_avail : supported_freq) {
                                                        if (frequency == std::get<0>(freq_avail) &&
                                                            supported_bw <= static_cast<int>(std::get<1>(freq_avail))) {
                                                            frequency_list.push_back(frequency);
                                                            break;
                                                        }
                                                    }
                                                }
                                            } else if (scan_started && line.find("OK") != std::string::npos) {
                                                radio_control::vrc_log("Got end of band #" +
                                                                       std::to_string(band_to_scan) + " channel scan.\n");
                                                scan_done = true;
                                                scan_requested = false;
                                                scan_started = false;
                                            }

                                            if (scan_started)
                                                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                                            else
                                                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                                        }
                                    } catch (...) {
                                        std::exception_ptr p = std::current_exception();
                                        radio_control::vrc_log("Caught exception in channel scanner thread.\n");
                                        scan_done = true;
                                        scan_requested = false;
                                    }
                                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                                }
                            }
                            break;
                        }
                        default:
                            scan_done = true;
                            break;
                    }
                    output.clear();
                }
                close(scan_sock);
            } else {
                std::vector<int> empty;
                radio_control::vrc_log("Failed opening second connection to radio to perform channel scan\n");
            }
        }

        return frequency_list;
    }

    bool MicrohardControl::NeedsFirmwareUpgrade(const std::string& firmware_filename) {
        if (SupportsFeature(FEATURE_FIRMWARE_UPGRADE_FTP)) {
            // Hardware version
            uint16_t hw_version = 0;
            if (radio_info.hardware_version.empty()) {
                hw_version = 1;
            } else if (radio_info.hardware_version.find("Rev") == 0) {
                hw_version = 1;
            } else {
                hw_version = static_cast<uint16_t>(std::atof(radio_info.hardware_version.c_str()));
            }

            // Firmware differs on AES256 option
            bool aes256_option = (radio_info.product_name.find("AES256") != std::string::npos);

            // Define firmware file prefix bsaed on model, hardware version and AES256 option
            std::string prefix;
            if (detected_model == RadioModel::PMDDL900) {
                prefix = "pDDL900";
            } else if (detected_model == RadioModel::PDDL1800) {
                prefix = (aes256_option) ? "pDDL1800_AES256-crpd-" : "pDDL1800-crpd-";
            } else if (detected_model == RadioModel::PMDDL2450) {
                if (hw_version == 1) {
                    prefix = (aes256_option) ? "pMDDL2450_AES256-crpd-" : "pMDDL2450-crpd-";
                } else {
                    prefix = (aes256_option) ? "pMDDL2450AES256-" : "pMDDL2450-";
                }
            } else if ((detected_model == RadioModel::FDDL1624) ||
                       (detected_model == RadioModel::PMDDL1624)) {
                prefix = "pMDDL1624AES256-";
            }

            // Does firmware file apply to this model?
            auto prefix_pos = firmware_filename.find(prefix);
            if (prefix_pos != std::string::npos) {
                // Parse firmware version and build number
                int major,minor,patch,build;
                std::string version_string = firmware_filename.substr(prefix_pos + prefix.size());

                std::regex rx{ "v(\\d+)_(\\d+)_(\\d+)-r(\\d+)\\.bin" };
                std::smatch match;
                if (std::regex_search(version_string, match, rx) && match.size() > 4) {
                    major = std::atoi(match[1].str().c_str());
                    minor = std::atoi(match[2].str().c_str());
                    patch = std::atoi(match[3].str().c_str());
                    build = std::atoi(match[4].str().c_str());

                    // Is firmware file version upgrade from current version
                    if (software_build.size() >= 4) {
                        if (software_build[0] < major)
                            return true;
                        if (software_build[0] == major && software_build[1] < minor)
                            return true;
                        if (software_build[0] == major && software_build[1] == minor && software_build[2] < patch)
                            return true;
                        if (software_build[0] == major && software_build[1] == minor && software_build[2] == patch && software_build[3] < build)
                            return true;
                    }
                }
            }
        }
        return false;
    }

} // namespace radio_control

