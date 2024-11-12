#ifndef HELIXCONTROL_H
#define HELIXCONTROL_H

#pragma once

#include <stdlib.h>
#include <atomic>
#include <chrono>
#include <deque>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "RadioControl.h"

namespace radio_control {
    class HelixControl : public RadioControl {
    public:
        explicit HelixControl(bool central_node);
        ~HelixControl();
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

        std::vector<std::tuple<int, float, int>> GetSupportedFreqAndMaxBWPer(
                void) override;
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
        RadioModel GetModel(void) override { return RadioModel::HELIX2X2; }
        bool SupportsFeature(RadioFeature ) const override { return false; }
        virtual RadioState GetRadioState(void) override { return current_state; };

        // TODO: Implement!
        int GetCurrentFrequency(void) override { return radio_info.channel_freq; }
        int GetCurrentBW(void) override { return radio_info.channel_bw; }
        int GetCurrentPower(void) override { return radio_info.tx_power; }
        Topology GetTopology(void) override { return Topology::MESH; }
        std::string GetNetworkID(void) override { return radio_info.network_id; }
        std::string GetNetworkPassword(void) override { return radio_info.network_password; }
        std::string GetNetworkIPAddr(void) override { return network_ipaddr; };
        std::string GetRadioIPAddr(void) override { return helix_address; };

    private:
        typedef struct {
            int rssi;
            uint64_t tx_retries;
            uint64_t tx_failed;
            uint64_t tx_bytes;
            uint64_t tx_packets;
            uint64_t rx_bytes;
            uint64_t rx_packets;
            float tx_bitrate;
            float rx_bitrate;
            int channel_freq;
            int channel_bw;
            int tx_power;
            std::string network_id;
            std::string network_password;
        } RFInfo;

        std::string helix_address;
        std::atomic<bool> keep_running;
        std::shared_ptr<std::thread> handler;
        bool is_host;
        std::deque<radio_control::QueueItem> config_queue;
        int rssi;
        RadioState current_state;
        RadioModel detected_model;
        RFInfo radio_info;
        std::string network_ipaddr;

        void Handler(void);
        std::string RunCommand(std::string cmd, void *s);
        std::string GetAdapterName(void);
        int GetHelixCount(std::string adapter);
        std::vector<std::string> GetHelixIPs(std::string adapter);
        RFInfo ParseInfo(std::string raw);
    };
}  // namespace radio_control

#endif
