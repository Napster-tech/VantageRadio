#include "FakeControl.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <iostream>
#include <netdb.h>
#include <netinet/in.h>
#include <sstream>
#include <sys/socket.h>

namespace radio_control {

FakeControl::FakeControl(bool central_node)
    : fake_address{""}, keep_running{true}, is_host{central_node}, current_state{RadioState::UNKNOWN},
    network_id{""}, network_password{""}, network_ipaddr{"192.168.20.30"} {
  handler = std::make_shared<std::thread>([this] { Handler(); });
  (void)is_host;
  (void)detected_model;
  (void)rssi;
}

FakeControl::~FakeControl() {
  keep_running = false;
  while (!handler->joinable()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    radio_control::vrc_log("Child thread not joinable.\n");
  }
  handler->join();
}

void FakeControl::Handler(void) {
  radio_control::vrc_log("Handler active.\n");

  // Get the name of the fake's network adapter.
  std::string adapter = GetAdapterName();

  std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  current_state = RadioState::BOOTING;
  std::this_thread::sleep_for(std::chrono::milliseconds(2000));
  current_state = RadioState::CONFIGURING;
  std::this_thread::sleep_for(std::chrono::milliseconds(2000));
  current_state = RadioState::DISCONNECTED;

  while (keep_running) {
    while (config_queue.size()) {
      QueueItem cmd = config_queue.front();
      switch (cmd.action) {
      case radio_control::Action::ACTION_CHANGE_FREQUENCY: {
        auto data = any::any_cast<std::tuple<int, float, int>>(cmd.item);
        int frequency = std::get<0>(data);
        float bandwidth = std::get<1>(data);
        int channel = std::get<2>(data);
        
        (void)channel;
        (void)bandwidth;

        // Simulate changing the channel.
        current_channel = frequency;
        current_bandwidth = bandwidth;
        break;
      }
      case radio_control::Action::ACTION_CHANGE_NETWORK_ID: {
        auto id = any::any_cast<std::string>(cmd.item);
        radio_control::vrc_log("Setting mesh id to: " + id + "\n");
        network_id = id;
        break;
      }
      case radio_control::Action::ACTION_CHANGE_PASSWORD: {
        auto pw = any::any_cast<std::string>(cmd.item);
        network_password = pw;
        break;
      }
      case radio_control::Action::ACTION_RESTART_SERVICES: {
        current_state = RadioState::CONFIGURING;
        std::this_thread::sleep_for(std::chrono::milliseconds(5000));

        // For debugging purposes we have a single channel that we will cause the link to become "connected"
        if (current_channel == connect_channel) {
          current_state = RadioState::CONNECTED;
        } else {
          current_state = RadioState::DISCONNECTED;
        }

        break;
      }
      case radio_control::Action::ACTION_SET_AIR_RATE: {
        break;
      }
      case radio_control::Action::ACTION_SET_OUTPUT_POWER: {
        auto dbm = any::any_cast<int>(cmd.item);
        radio_control::vrc_log("setting output power to: " + std::to_string(dbm)
                 + "\n");
        break;
      }
      case radio_control::Action::ACTION_SET_TOPOLOGY: {
        auto topo = any::any_cast<Topology>(cmd.item);
        topology = topo;
      }
      default:
        break;
      }
      config_queue.pop_front();
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }

  radio_control::vrc_log("Handler exiting.\n");
}

std::string FakeControl::GetAdapterName(void) {
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  return ("fakeadapter");
}

int FakeControl::GetFakeCount(std::string adapter) {
  (void)adapter;
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  return (1);
}

std::vector<std::string> FakeControl::GetFakeIPs(std::string adapter) {
  (void)adapter;
  std::vector<std::string> output;
  output.push_back("192.168.0.0");
  return output;
}

std::vector<std::tuple<int, float, int>>
FakeControl::GetSupportedFreqAndMaxBWPer(void) {
  std::vector<std::tuple<int, float, int>> options;
  // 1625 - 1725 MHz
  // Start Freq: 1630
  // End Freq: 1720
  // Stepsize: 1 MHz
  int step_size = 1;
  for (int center_freq = 1631; center_freq <= 1720; center_freq += step_size) {
    float bw_allowed = 3;
    if (((center_freq - 5) > 1625) && ((center_freq + 5) < 1725)) {
      bw_allowed = 5;
    } else if (((center_freq - 10) > 1625) && ((center_freq + 10) < 1725)) {
      bw_allowed = 10;
    } else if (((center_freq - 15) > 1625) && ((center_freq + 15) < 1725)) {
      bw_allowed = 15;
    } else if (((center_freq - 20) > 1625) && ((center_freq + 20) < 1725)) {
      bw_allowed = 20;
    } else if (((center_freq - 26) > 1625) && ((center_freq + 26) < 1725)) {
      bw_allowed = 26;
    } else if (((center_freq - 40) > 1625) && ((center_freq + 40) < 1725)) {
      bw_allowed = 40;
    }
    options.push_back(
        std::make_tuple(center_freq, bw_allowed, (center_freq - 1625) + 1));
  }

  // 1780 - 1850 MHz
  step_size = 1;
  for (int center_freq = 1785; center_freq <= 1845; center_freq += step_size) {
    float bw_allowed = 3;
    if (((center_freq - 5) > 1780) && ((center_freq + 5) < 1850)) {
      bw_allowed = 5;
    } else if (((center_freq - 10) > 1780) && ((center_freq + 10) < 1850)) {
      bw_allowed = 10;
    } else if (((center_freq - 15) > 1780) && ((center_freq + 15) < 1850)) {
      bw_allowed = 15;
    } else if (((center_freq - 20) > 1780) && ((center_freq + 20) < 1850)) {
      bw_allowed = 20;
    } else if (((center_freq - 26) > 1780) && ((center_freq + 26) < 1850)) {
      bw_allowed = 26;
    } else if (((center_freq - 40) > 1780) && ((center_freq + 40) < 1850)) {
      bw_allowed = 40;
    }
    options.push_back(
        std::make_tuple(center_freq, bw_allowed, (center_freq - 1780) + 1));
  }

  // 2025 - 2110 MHz
  step_size = 1;
  for (int center_freq = 2030; center_freq <= 2105; center_freq += step_size) {
    float bw_allowed = 3;
    if (((center_freq - 5) > 2025) && ((center_freq + 5) < 2110)) {
      bw_allowed = 5;
    } else if (((center_freq - 10) > 2025) && ((center_freq + 10) < 2110)) {
      bw_allowed = 10;
    } else if (((center_freq - 15) > 2025) && ((center_freq + 15) < 2110)) {
      bw_allowed = 15;
    } else if (((center_freq - 20) > 2025) && ((center_freq + 20) < 2110)) {
      bw_allowed = 20;
    } else if (((center_freq - 26) > 2025) && ((center_freq + 26) < 2110)) {
      bw_allowed = 26;
    } else if (((center_freq - 40) > 2025) && ((center_freq + 40) < 2110)) {
      bw_allowed = 40;
    }
    options.push_back(
        std::make_tuple(center_freq, bw_allowed, (center_freq - 2025) + 1));
  }

  // 2200 - 2290 MHz
  step_size = 1;
  for (int center_freq = 2205; center_freq <= 2285; center_freq += step_size) {
    float bw_allowed = 3;
    if (((center_freq - 5) > 2200) && ((center_freq + 5) < 2290)) {
      bw_allowed = 5;
    } else if (((center_freq - 10) > 2200) && ((center_freq + 10) < 2290)) {
      bw_allowed = 10;
    } else if (((center_freq - 15) > 2200) && ((center_freq + 15) < 2290)) {
      bw_allowed = 15;
    } else if (((center_freq - 20) > 2200) && ((center_freq + 20) < 2290)) {
      bw_allowed = 20;
    } else if (((center_freq - 26) > 2200) && ((center_freq + 26) < 2290)) {
      bw_allowed = 26;
    } else if (((center_freq - 40) > 2200) && ((center_freq + 40) < 2290)) {
      bw_allowed = 40;
    }
    options.push_back(
        std::make_tuple(center_freq, bw_allowed, (center_freq - 2200) + 1));
  }

  // 2310 - 2390 MHz
  step_size = 1;
  for (int center_freq = 2315; center_freq <= 2385; center_freq += step_size) {
    float bw_allowed = 3;
    if (((center_freq - 5) > 2310) && ((center_freq + 5) < 2390)) {
      bw_allowed = 5;
    } else if (((center_freq - 10) > 2310) && ((center_freq + 10) < 2390)) {
      bw_allowed = 10;
    } else if (((center_freq - 15) > 2310) && ((center_freq + 15) < 2390)) {
      bw_allowed = 15;
    } else if (((center_freq - 20) > 2310) && ((center_freq + 20) < 2390)) {
      bw_allowed = 20;
    } else if (((center_freq - 26) > 2310) && ((center_freq + 26) < 2390)) {
      bw_allowed = 26;
    } else if (((center_freq - 40) > 2310) && ((center_freq + 40) < 2390)) {
      bw_allowed = 40;
    }
    options.push_back(
        std::make_tuple(center_freq, bw_allowed, (center_freq - 2310) + 1));
  }

  // 2400 - 2500 MHz
  step_size = 1;
  for (int center_freq = 2402; center_freq <= 2498; center_freq += step_size) {
    float bw_allowed = 3;
    if (((center_freq - 5) > 2400) && ((center_freq + 5) < 2500)) {
      bw_allowed = 5;
    } else if (((center_freq - 10) > 2400) && ((center_freq + 10) < 2500)) {
      bw_allowed = 10;
    } else if (((center_freq - 15) > 2400) && ((center_freq + 15) < 2500)) {
      bw_allowed = 15;
    } else if (((center_freq - 20) > 2400) && ((center_freq + 20) < 2500)) {
      bw_allowed = 20;
    } else if (((center_freq - 26) > 2400) && ((center_freq + 26) < 2500)) {
      bw_allowed = 26;
    } else if (((center_freq - 40) > 2400) && ((center_freq + 40) < 2500)) {
      bw_allowed = 40;
    }
    options.push_back(
        std::make_tuple(center_freq, bw_allowed, (center_freq - 2400) + 1));
  }

  return options;
}

int FakeControl::SetFrequencyAndBW(int freq, float bw) {
  bool bw_ok, freq_ok = false;
  int channel_number = 0;
  for (auto bw_available : GetSupportedBWs()) {
    if (bw == static_cast<float>(bw_available)) {
      bw_ok = true;
      break;
    }
  }

  for (auto freq_avail : GetSupportedFreqAndMaxBWPer()) {
    if (freq == std::get<0>(freq_avail) && bw <= static_cast<float>(std::get<1>(freq_avail))) {
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

void FakeControl::SetNetworkID(std::string id) {
  QueueItem item = {Action::ACTION_CHANGE_NETWORK_ID, id};
  config_queue.push_back(item);
}

void FakeControl::SetNetworkPassword(std::string pw) {
  QueueItem item = {Action::ACTION_CHANGE_PASSWORD, pw};
  config_queue.push_back(item);
}

void FakeControl::SetNetworkIPAddr(std::string ipaddr) {
  network_ipaddr = ipaddr;
}

void FakeControl::SetRadioIPAddr(std::string radio_ipaddr) {
  fake_address = radio_ipaddr;
}

void FakeControl::ApplySettings(void) {
  QueueItem item = {Action::ACTION_RESTART_SERVICES, nullptr};
  config_queue.push_back(item);
}

void FakeControl::SetOutputPower(int dbm) {
  QueueItem item = {Action::ACTION_SET_OUTPUT_POWER, dbm};
  config_queue.push_back(item);
}

void FakeControl::SetTopology(Topology topology) {
  QueueItem item = {Action::ACTION_SET_TOPOLOGY, topology};
  config_queue.push_back(item);
}

} // namespace radio_control
