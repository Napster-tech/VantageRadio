#ifndef MICROHARDCONTROL_H
#define MICROHARDCONTROL_H

#pragma once

#include "RadioControl.h"
#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdlib.h>
#include <string>
#include <thread>
#include <vector>

namespace radio_control {
    enum class MicrohardModulations {
        MH_RATE_AUTO = 0,
        MH_RATE_64_QAM_FEC_5_6,
        MH_RATE_64_QAM_FEC_3_4,
        MH_RATE_64_QAM_FEC_2_3,
        MH_RATE_16_QAM_FEC_3_4,
        MH_RATE_16_QAM_FEC_1_2,
        MH_RATE_QPSK_FEC_3_4,
        MH_RATE_QPSK_FEC_1_2,
        MH_RATE_BPSK_1_2
    };

    enum class ConfigMicrohardState {
        LOGIN,
        PASSWORD,
        SYSTEM_SUMMARY,
        PASSWORD_INCORRECT,
        ENCRYPTION_TYPE,
        CRYPTO_KEY,
        MODEM_IP,
        MODEM_DHCP,
        POWER,
        DEFAULT_FREQUENCY,
        FREQUENCY,
        RATE,
        ROLE,
        BANDWIDTH,
        NETWORK_ID,
        MIMO,
        EMI,
        SAVE,
        WRITE_FLASH,
        DONE
    };

    enum class MonitorMicrohardState {
        LOGIN,
        PASSWORD,
        MODEM_SUMMARY,
        MODEM_MODEL,
        MODEM_CHECK_FOR_VARIANCE,
        MODEM_SETTINGS,
        MODEM_LAN_CHECK,
        MODE_FREQ_HOP_CHECK,
        MODEM_STATUS,
        UPDATE_MODULATION,
        CHANNEL_SCAN,
        DONE
    };

    class MicrohardControl : public RadioControl {
    public:
        MicrohardControl(bool central_node);
        MicrohardControl(bool central_node, std::string adapter,
                         std::string mh_ip, std::string net_ip);
        virtual ~MicrohardControl() override;

        // New configuration functions for drone control


        // Existing functions and properties
        std::vector<float> GetSupportedBWs(void) override;
        std::vector<std::tuple<int, float, int>> GetSupportedFreqAndMaxBWPer(void) override;
        int SetFrequencyAndBW(int freq, float bw) override;
        void SetBandwidth(float bw);
        void SetOutputPower(int dbm) override;
        void SetOutputPowerImmediate(int dbm);
        void SetNetworkID(std::string id) override;
        void SetNetworkPassword(std::string pw) override;
        void SetNetworkIPAddr(std::string ipaddr) override;
        void SetRadioIPAddr(std::string radio_ipaddr) override;
        void SetTopology(Topology topology) override;
        void SetBufferSize(int size) override;
        void SetRateFloor(int rate) override;
        void SetRateCeiling(int rate) override;
        void SetBeaconLossCount(int count) override;
        void SetRateImmediate(int rate);
        void SetMIMO(bool mimo_active);
        void ApplySettings(void) override;
        int GetRSSI(void) override { return radio_info.rssi; }
        int GetNoiseFloor(void) override { return radio_info.noise_floor; }
        int GetNeighborCount(void) override { return 0; }
        bool IsConnected(void) override { return false; }
        int GetMinPower(void) { return 7; }
        int GetMaxPower(void);
        bool SupportsFeature(RadioFeature feature) const override;
        RadioModel GetModel(void) override { return detected_model; }
        RadioState GetRadioState(void) override { return current_state; }
        int GetCurrentFrequency(void) override { return radio_info.channel_freq; }
        int GetCurrentBW(void) override { return radio_info.channel_bw; }
        int GetCurrentPower(void) override { return radio_info.tx_power; }
        int GetOutputPowerImmediate(void) { return radio_info.tx_powerq; }
        int GetCurrentRate(void) { return radio_info.tx_modulation; }
        Topology GetTopology(void) override {
            return (use_mesh) ? Topology::MESH : Topology::STATION; }
        std::string GetNetworkID(void) override { return radio_info.network_id; }
        std::string GetNetworkPassword(void) override { return radio_info.network_password; }
        std::string GetNetworkIPAddr(void) override {
            return config_ip_network.size() ? config_ip_network : network_address; }
        std::string GetRadioIPAddr(void) override { return microhard_address; }
        std::future<std::vector<int>> GetLowInterferenceFrequencyList(int count, float bw) override;

        // Frequency hopping extended support
        bool IsFrequencyHopping(void) override { return (radio_info.freq_hop_stat == 1); }
        int SetFrequencyList(const std::vector<int>& freq_list, float bw) override;
        const std::vector<int>& GetFrequencyList(void) override { return radio_info.freq_list; }
        int SetFrequencyHopping(uint8_t selection_mode, uint8_t dynamic_mode,
                                uint32_t channel_interval, uint32_t switch_interval,
                                uint32_t announce_times, uint32_t bands_selected=0, bool recovery=false);

        virtual std::string GetFirmwareVersionString(void) const override { return radio_info.software_version; }
        virtual bool NeedsFirmwareUpgrade(const std::string& firmware_filename) override;

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
            int tx_powerq;
            int tx_modulation;
            int lan_ok;
            int freq_hop_stat;
            int freq_hop_mode;
            std::vector<int> freq_list;
            std::string network_id;
            std::string network_password;
            std::string operation_mode;
            std::string radio_name;
            std::string product_name;
            std::string hardware_version;
            std::string software_version;
        } RFInfo;

        std::string microhard_address;
        std::string network_address;
        std::atomic<bool> keep_running;
        bool is_host;
        bool use_mesh;
        int topology_change;
        std::string microhard_adapter;
        std::string microhard_subnet;
        RadioModel detected_model;
        RadioModel companion_model;
        RadioVariance radio_variance;
        bool factory_reset;
        std::shared_ptr<std::thread> handler;
        MicrohardControl::RFInfo radio_info;

        std::string config_ip_network;
        std::string config_ip_radio;

        // Helper function to send commands over Telnet
        bool telnetSendCommand(int sock, const std::string& command);

        void Handler(void);
        bool RunShortCommand(std::string c);
        bool RunQueryCommand(std::string c, std::function<bool(const std::string& response)> parser);
        std::string GetAdapterName(std::vector<std::string>& ips);
        int GetMicrohardCount(std::string adapter);
        std::vector<std::string> GetMicrohardIPs(std::string adapter);
        bool BlacklistedIP(const std::string& ip);
        RFInfo ParseInfo(std::string raw);
        void ParseSummary(std::string raw);
        void ParseStatus(std::string raw);
        void ParseSetting(std::string raw);
        void ParseHopInfo(std::string raw);
        void ParseRateMod(std::string modulation);
        bool SocketConnect(const int &sock, const std::string &air_ip);
        std::string SelectMicrohardIP(const std::vector<std::string>& ips, const std::string& ipconfig);
        void Breadcrumb(bool leave);
        bool SupportsFrequencyHopping(void) const;
        std::vector<int> ScanChannels(int sort, int count, float bw);
        std::vector<int> ScanChannels1800(int band, int count, float bw);

        std::deque<radio_control::QueueItem> config_queue;
        std::mutex config_queue_mutex;
        int sock;
        std::string _summary;
        RadioState current_state;
        std::vector<uint16_t> software_build;
    }; // namespace radio_control
} // namespace radio_control

#endif
