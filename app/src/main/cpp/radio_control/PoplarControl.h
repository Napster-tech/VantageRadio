#ifndef POPLARCONTROL_H
#define POPLARCONTROL_H

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

    enum class PoplarModulations {
        VR_POPLAR_RATE_2M = 0,
        VR_POPLAR_RATE_1M,
        VR_POPLAR_RATE_1M_S2,
        VR_POPLAR_RATE_1M_S8
    };


#define MGMT_PORT 18888
#define MGMT_IP "192.168.20.254"

    enum class PoplarMGMTActions {
        MGMT_INVALID = 1,
        MGMT_GET_RADIO_NAME,
        MGMT_SET_RADIO_NAME,
        MGMT_GET_ROLE,
        MGMT_SET_ROLE,
        MGMT_REBOOT,
        MGMT_CONNECT,
        MGMT_DISCONNECT,
        MGMT_SET_PASSWORD,
        MGMT_SCAN,
        MGMT_SET_BW,
        MGMT_SET_RF_TEST
    };

    class PoplarControl : public RadioControl {
    public:
        PoplarControl(bool central_node);
        PoplarControl(bool central_node, std::string adapter,
                      std::string net_ip);
        virtual ~PoplarControl() override;
        std::vector<float> GetSupportedBWs(void) override {
            std::vector<float> bws;
            bws.push_back(4);
            bws.push_back(8);

            return bws;
        }

        std::vector<std::tuple<int, float, int>>
        GetSupportedFreqAndMaxBWPer(void) override;
        int SetFrequencyAndBW(int freq, float bw) override;
        void SetOutputPower(int dbm) override;
        void SetNetworkID(std::string id) override;
        void SetNetworkPassword(std::string pw) override;
        void SetNetworkIPAddr(std::string ipaddr) override;
        void SetRadioIPAddr(std::string radio_ipaddr) override;
        void SetTopology(Topology topology) override;
        void ApplySettings(void) override;
        void SetRateImmediate(int rate);
        void SetMIMO(bool mimo_active);
        int GetRSSI(void) override { return radio_info.rssi; }
        int GetNoiseFloor(void) override { return radio_info.noise_floor; }
        void EnableRFTestMode(int enable, int power, int channel);
        int GetNeighborCount(void) override {
            if (IsConnected()) {
                return 1;
            } else {
                return 0;
            }
        }
        bool IsConnected(void) override;
        int GetMinPower(void) { return 7; }
        int GetMaxPower(void) { return 30; }
        bool SupportsFeature(RadioFeature ) const override { return false; }
        RadioModel GetModel(void) override { return detected_model; }
        RadioState GetRadioState(void) override { return current_state; }
        int GetCurrentFrequency(void) override { return radio_info.channel_freq; }
        int GetCurrentBW(void) override { return radio_info.channel_bw; }
        int GetCurrentPower(void) override { return 30; }
        Topology GetTopology(void) override { return Topology::STATION; }
        std::string GetNetworkID(void) override { return radio_info.network_id; }
        std::string GetNetworkPassword(void) override {
            return radio_info.network_password;
        }
        std::string GetNetworkIPAddr(void) override;
        std::string GetRadioIPAddr(void) override { return GetNetworkIPAddr(); };

    private:
        typedef struct {
            int rssi;
            int noise_floor;
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
            int lan_ok;
            std::string network_id;
            std::string network_password;
            std::string operation_mode;
        } RFInfo;

        std::string Poplar_address;
        std::atomic<bool> keep_running;
        bool is_host;
        std::string Poplar_adapter;
        RadioModel detected_model;
        bool factory_reset;
        std::shared_ptr<std::thread> handler;
        PoplarControl::RFInfo radio_info;
        std::string config_ip_network;

        void Handler(void);
        bool RunShortCommand(std::string c);
        std::string GetAdapterName(void);
        int GetPoplarRSSI(std::string adapter);
        RFInfo ParseInfo(std::string raw);
        bool SocketConnect(const int &sock, const std::string &air_ip);

        std::deque<radio_control::QueueItem> config_queue;
        int rssi;
        int sock;
        std::string _summary;
        RadioState current_state;
    }; // namespace radio_control
} // namespace radio_control

#endif
