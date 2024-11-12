#ifndef FAKECONTROL_H
#define FAKECONTROL_H

#pragma once

#include "RadioControl.h"
#include <atomic>
#include <chrono>
#include <deque>
#include <iostream>
#include <mutex>
#include <stdlib.h>
#include <string>
#include <thread>
#include <vector>

namespace radio_control {
    class FakeControl : public RadioControl {
    public:
        explicit FakeControl(bool central_node);
        ~FakeControl();
        std::vector<float> GetSupportedBWs(void) override {
            std::vector<float> bws;
            bws.push_back(3);
            bws.push_back(5);
            bws.push_back(10);
            bws.push_back(15);
            bws.push_back(20);
            bws.push_back(26);
            bws.push_back(40);
            return bws;
        }

        std::vector<std::tuple<int, float, int>> GetSupportedFreqAndMaxBWPer(void) override;
        int SetFrequencyAndBW(int freq, float bw) override;
        void SetOutputPower(int dbm) override;
        void SetNetworkID(std::string id) override;
        void SetNetworkPassword(std::string pw) override;
        void SetNetworkIPAddr(std::string ipaddr) override;
        void SetRadioIPAddr(std::string radio_ipaddr) override;
        void SetTopology(Topology topology) override;
        void ApplySettings(void) override;
        int GetRSSI(void) override { return -1; }
        int GetNoiseFloor(void) override { return -1; }
        int GetNeighborCount(void) override { return 0; }
        bool IsConnected(void) override { return false; }
        RadioModel GetModel(void) override { return RadioModel::FAKE; }
        bool SupportsFeature(RadioFeature ) const override { return false; }
        RadioState GetRadioState(void) override { return current_state; }

        int GetCurrentFrequency(void) override { return current_channel; }
        int GetCurrentBW(void) override { return current_bandwidth; }
        int GetCurrentPower(void) override { return 0; }
        Topology GetTopology(void) override { return topology; };
        std::string GetNetworkID(void) override { return network_id;};
        std::string GetNetworkPassword(void) override { return network_password;};
        std::string GetNetworkIPAddr(void) override { return network_ipaddr; };
        std::string GetRadioIPAddr(void) override { return fake_address; };

    private:
        std::string fake_address;
        std::atomic<bool> keep_running;
        bool is_host;
        RadioModel detected_model;
        std::shared_ptr<std::thread> handler;

        void Handler(void);
        std::string GetAdapterName(void);
        int GetFakeCount(std::string adapter);
        std::vector<std::string> GetFakeIPs(std::string adapter);

        std::deque<radio_control::QueueItem> config_queue;
        RadioState current_state;
        int rssi;

        std::string network_id;
        std::string network_password;
        std::string network_ipaddr;

        // ONLY FOR FAKING! //
        int current_channel = 1641;
        int connect_channel = 1640;
        int current_bandwidth = 5;
        Topology topology = Topology::STATION;
        //////////////////////
    };
} // namespace radio_control

#endif
