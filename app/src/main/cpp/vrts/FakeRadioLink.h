#ifndef FAKERADIOLINK_H
#define FAKERADIOLINK_H

#pragma once

#include <atomic>
#include <deque>
#include <stdint.h>
#include <thread>
#include <vector>

/// @brief Localhost loopback radio link with emulated packet loss, latency, and
/// internal buffer size
class FakeRadioLink {
public:
  FakeRadioLink(std::string input_ip, uint16_t input_port,
                std::string output_ip, uint16_t output_port);
  ~FakeRadioLink();

  /// @brief Sets the min and max random latency that will be added to the
  /// transmission.
  /// @param min_ms min latency bound in mS
  /// @param max_ms max latency bound in mS
  void setMinMaxLatency(uint16_t min_ms, uint16_t max_ms);

  /// @brief Sets the min and max random packet loss probability for a given packet.
  /// @param loss_probability Probability between 0.0 and 1.0 of loss of an input packet.
  void setLossProbability(float loss_probability);

  /// @brief Set how many internal buffer slots there are. FIFO
  /// @param num_bufs Number of max L2 MTU buffer slots exist.
  /// @details Useful for emulating a small radio buffer and testing how spikes
  /// in bitrates can cause end-to-end packet loss.
  void setBufferSize(uint64_t num_bufs);

private:
  std::deque<std::vector<uint8_t>> buf_deque;
  std::atomic<float> loss_probability;
  std::atomic<uint16_t> min_ms_delay;
  std::atomic<uint16_t> max_ms_delay;
  std::atomic<uint16_t> buffer_limit;
  std::atomic<bool> keep_running;
  std::shared_ptr<std::thread> radio_loop;

  void FakeRadioLoop(std::string input_ip, uint16_t input_port,
                     std::string output_ip, uint16_t output_port);
  void udpSend(std::vector<uint8_t> &packet, std::string ip, uint16_t port);
  std::vector<uint8_t> udpRecv(std::string ip, uint16_t port);
};

#endif