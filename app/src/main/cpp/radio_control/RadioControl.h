#ifndef RADIOCONTROL_H
#define RADIOCONTROL_H

#if __cplusplus == 201703L
#include <any>
namespace any = std;
#else
#if __GNUC__ > 7 ||                                                            \
    (__GNUC__ == 6 &&                                                          \
     (__GNUC_MINOR__ > 8 || (__GNUC_MINOR__ == 0 && __GNUC_PATCHLEVEL__ > 0)))
#include <any>
namespace any = std;
#else
#include <experimental/any>
namespace any = std::experimental;
#endif
#endif
#include <deque>
#include <iostream>
#include <future>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#pragma once

namespace radio_control {
    enum class Action {
        ACTION_CHANGE_FREQUENCY,
        ACTION_CHANGE_FREQUENCY_LIST,
        ACTION_CHANGE_FREQUENCY_HOPPING,
        ACTION_CHANGE_BANDWIDTH_IMMEDIATE,
        ACTION_CHANGE_PASSWORD,
        ACTION_CHANGE_NETWORK_ID,
        ACTION_SET_AIR_RATE,
        ACTION_SET_AIR_RATE_IMMEDIATE,
        ACTION_SET_OUTPUT_POWER,
        ACTION_SET_OUTPUT_POWER_IMMEDIATE,
        ACTION_SET_MIMO,
        ACTION_SET_TOPOLOGY,
        ACTION_SET_BUFFER_SIZE,
        ACTION_SET_RATE_FLOOR,
        ACTION_SET_RATE_CEILING,
        ACTION_SET_BEACON_LOSS_COUNT,
        ACTION_RESTART_SERVICES,
        ACTION_SET_RF_TEST_MODE
    };

    enum class RadioModel {
        UNKNOWN,
        FAKE,
        PMDDL900,
        PDDL1800,
        PMDDL2450,
        PMDDLMULTI,
        PMDDL1621,
        PMDDL1622,
        PMDDL1624,
        FDDL1624,
        HELIX2X2,
        HELIX1X1,
        VR_POPLAR,
        RPX,
        DTC
    };

// Some microhards have hacky variances for
// region-specific changes and it isn't captured
// in the radio model...
    enum class RadioVariance {
        NONE,
        PMDDL2450JP
    };

    enum class RadioState {
        UNKNOWN,
        BOOTING,
        CONFIGURING,
        SCANNING,
        CONNECTED,
        DISCONNECTED,
        REMOVED
    };

    enum class Topology { ACCESS_POINT, STATION, MESH, RELAY };

    struct QueueItem {
        Action action;
        any::any item;
    };

    enum RadioFeature {
        FEATURE_CORE,
        FEATURE_BUFFER_SIZE,
        FEATURE_CHANNEL_HOP,
        FEATURE_CHANNEL_SCAN,
        FEATURE_FREQUENCY_LIST,
        FEATURE_MESH,
        FEATURE_RATE_FLOOR,
        FEATURE_FIRMWARE_UPGRADE_FTP,
        FEATURE_FIRMWARE_UPGRADE_SFTP,
    };

// Do we want to create a public method to determine what kind of radio is
// attached? Or should that be up to the configuration manager to determine
// that and specify up-front via selection of a child class explicitly?
    class RadioControl {
    public:
        RadioControl(){}
        virtual ~RadioControl(){}

        virtual std::vector<float> GetSupportedBWs(void) = 0;
        virtual std::vector<std::tuple<int, float, int>>
        GetSupportedFreqAndMaxBWPer(void) = 0;
        virtual int GetRSSI(void) = 0;
        virtual int GetNoiseFloor(void) = 0;
        virtual int GetNeighborCount(void) = 0;
        virtual bool IsConnected(void) = 0;
        virtual void SetNetworkID(std::string id) = 0;
        virtual void SetNetworkPassword(std::string pw) = 0;
        virtual void SetNetworkIPAddr(std::string ipaddr) = 0;
        virtual void SetRadioIPAddr(std::string radio_ipaddr) = 0;
        virtual void SetOutputPower(int dbm) = 0;
        virtual void SetTopology(Topology topology) = 0;
        virtual void SetBufferSize(int size) { (void)size; }
        virtual void SetRateFloor(int rate) {  (void)rate; }
        virtual void SetRateCeiling(int rate) {  (void)rate; }
        virtual void SetBeaconLossCount(int count) {  (void)count; }
        virtual void ApplySettings(void) = 0;

        //  We only allow explicitly setting BW and Frequency at the same time, as
        //  some center frequencies are invalid depending on the channel BW selected
        //  or peculiarities of the radio we're controlling.
        virtual int SetFrequencyAndBW(int freq, float bw) = 0;
        virtual bool SupportsFeature(RadioFeature feature) const = 0;
        virtual RadioModel GetModel(void) = 0;
        virtual RadioState GetRadioState(void) = 0;
        virtual int GetCurrentFrequency(void) = 0;
        virtual int GetCurrentBW(void) = 0;
        virtual int GetCurrentPower(void) = 0;
        virtual Topology GetTopology(void) = 0;
        virtual std::string GetNetworkID(void) = 0;
        virtual std::string GetNetworkPassword(void) = 0;
        virtual std::string GetNetworkIPAddr(void) = 0;
        virtual std::string GetRadioIPAddr(void) = 0;
        virtual std::future<std::vector<int>> GetLowInterferenceFrequencyList(int count, float bw) {
            (void)count;
            (void)bw;
            std::promise<std::vector<int>> promise;
            std::vector<int> empty;
            promise.set_value(empty);
            return promise.get_future();
        }

        // If frequency hopping supported (TODO override in other radio classes)
        virtual bool IsFrequencyHopping(void) { return false; }
        virtual int SetFrequencyList(const std::vector<int>& freq_list, float bw) {
            (void)freq_list; (void)bw;
            return -1;
        }
        virtual const std::vector<int>& GetFrequencyList(void) {
            static std::vector<int> empty;
            return empty;
        }

        // Firmware version management
        virtual std::string GetFirmwareVersionString(void) const { return std::string("Unknown version"); }
        virtual bool NeedsFirmwareUpgrade(const std::string& firmware_filename) {
            (void)firmware_filename; return false; }
    };

// Utility functions.
    int count_lines(std::string);
    std::string get_str_between_two_str(const std::string &s,
                                        const std::string &start_delim,
                                        const std::string &stop_delim);
    void system_wrap(std::string);
    std::string system_call(std::string);
    int get_vrc_log_len(void);
    void vrc_log(std::string);
    std::string get_vrc_log(void);
    bool send_udp(const std::string adapter, const std::string ip, const int port,
                  std::vector<uint8_t> payload);
    bool is_adapter_present(std::string adapter);
    std::string get_subnet(std::string);
    std::vector<uint8_t>
    broadcast_and_listen(std::vector<uint8_t> data, uint16_t broadcast_port,
                         uint16_t listen_port, uint16_t from_port,
                         std::string adapter_ip, uint16_t msec_wait,
                         std::string *detected_ip);
} // namespace radio_control

#endif
