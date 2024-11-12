#ifndef DTCCONTROL_H
#define DTCCONTROL_H

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

// TODO
    enum class DTCModulations {
        DTC_RATE = 1000000,
    };

    enum class DTCMGMTActions {
        MGMT_INVALID = 1,
        MGMT_SET_KEY,
        MGMT_SET_BW,
        MGMT_SET_FREQ
    };

    class DTCControl : public RadioControl {
    public:
        DTCControl(bool central_node);
        DTCControl(bool central_node, std::string adapter, std::string dtc_ip,
                   std::string net_ip);
        virtual ~DTCControl() override;
        std::vector<float> GetSupportedBWs(void) override {
            // Bandwidth indexes are as follows:
            // 0 = 2.5MHz
            // 1 = 3.0MHz
            // 2 = 3.5MHz
            // 3 = 5.0MHz
            // 4 = 6.0MHz
            // 5 = 7.0MHz
            // 6 = 8.0MHz
            // 7 = 10.0MHz
            // 8 = 12.0MHz
            // 9 = 14.0MHz
            // 10 = 16.0MHz
            // 11 = 20.0MHz
            // 13 = 1.25MHz
            // 14 = 1.5MHz
            // 15 = 1.75MHz

            std::vector<float> bws;
            bws.push_back(2.5);
            bws.push_back(3.0);
            bws.push_back(3.5);
            bws.push_back(5.0);
            bws.push_back(6.0);
            bws.push_back(7.0);
            bws.push_back(8.0);
            bws.push_back(10.0);
            bws.push_back(12.0);
            bws.push_back(14.0);
            bws.push_back(16.0);
            bws.push_back(1.25);
            bws.push_back(1.5);
            bws.push_back(1.75);
            return bws;
        }

        std::vector<std::tuple<int, float, int>>
        GetSupportedFreqAndMaxBWPer(void) override;
        int SetFrequencyAndBW(int freq, float bw) override;
        void SetNetworkID(std::string id) override;
        void SetNetworkPassword(std::string pw) override;
        void SetOutputPower(int dbm) override;
        void ApplySettings(void) override;
        int GetRSSI(void) override { return radio_info.rssi; }
        int GetNoiseFloor(void) override { return radio_info.noise_floor; }
        int GetNeighborCount(void) override {
            if (IsConnected()) {
                return 1;
            } else {
                return 0;
            }
        }
        bool IsConnected(void) override;
        int GetMinPower(void) { return 26; }
        int GetMaxPower(void) { return 26; }
        std::string GetESN(void) { return dtc_device_esn; }
        bool SupportsFeature(RadioFeature ) const override { return false; }
        RadioModel GetModel(void) override { return detected_model; }
        RadioState GetRadioState(void) override { return current_state; }
        int GetCurrentFrequency(void) override { return radio_info.channel_freq; }
        int GetCurrentBW(void) override { return radio_info.channel_bw; }
        int GetCurrentPower(void) override { return 30; }
        std::string GetNetworkID(void) override { return radio_info.network_id; }
        std::string GetNetworkPassword(void) override {
            return radio_info.network_password;
        }

        // TODO: Implement these.
        void SetRadioIPAddr(std::string ip) override { (void)ip; }
        void SetNetworkIPAddr(std::string ip) override { (void)ip; }

        // TODO! I think only the ras_a pairing interface uses this in axiom at the moment.
        std::string GetRadioIPAddr(void) override { return "192.168.20.105"; }
        std::string GetNetworkIPAddr(void) override { return "192.168.20.30"; }
        Topology GetTopology(void) override { return Topology::MESH; }
        void SetTopology(Topology topology) override { (void)topology; }

    private:
        typedef struct {
            int rssi;
            int noise_floor;
            int node_id;
            float channel_freq;
            float channel_bw;
            int min_freq;
            int max_freq;
            std::string network_id;
            std::string network_password;
        } RFInfo;

        std::string address;
        std::atomic<bool> keep_running;
        std::string dtc_adapter;
        std::string dtc_adapter_mac;
        std::string dtc_device_mac;
        std::string dtc_device_ip;
        std::string dtc_device_esn;
        std::string dtc_subnet;
        std::string pw_reset_token;
        std::string config_ip_network;
        std::string config_ip_radio;
        bool is_host;
        bool pw_reset;
        bool dtc_dhcp_enabled;
        RadioModel detected_model;
        std::shared_ptr<std::thread> handler;
        DTCControl::RFInfo radio_info;

        void Handler(void);
        bool RunShortCommand(std::string c);
        std::string GetAdapterName(void);
        int DetectDTC(std::string adapter, std::string adapter_mac,
                      std::string &detected_ip);
        RFInfo ParseInfo(std::string raw);
        bool SocketConnect(const int &sock, const std::string &air_ip);

        std::deque<radio_control::QueueItem> config_queue;
        RadioState current_state;
    }; // namespace radio_control
} // namespace radio_control

#endif
