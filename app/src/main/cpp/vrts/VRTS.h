#ifndef VRTS_H
#define VRTS_H

#pragma once

#include <atomic>
#include <chrono>
#include <deque>
#include <h265_bitstream_parser.h>
#include <h265_common.h>
#include <map>
#include <mutex>
#include <stdint.h>
#include <thread>
#include <vector>

#include "mpegts/mpegts/mpegts_muxer.h"
//#include <mpegts_muxer.h>

/// @brief Namespace for the vr transport stream layer.
namespace vrts {

typedef enum { VRTS_DATA = 0, VRTS_ACKS, VRTS_NACKS } vrts_packet_type_t;

// Bits to pack into headers of ACKS/NACKS
// to let upstream know that some action
// needs to be taken. Piggyback on top of
// existing comms.
typedef enum { NEW_GOP_NEEDED = 1 } status_bits_t;

// Define clock time point type
#ifdef __ANDROID__
using vrts_clock_time_t = std::chrono::system_clock::time_point;
#else
using vrts_clock_time_t = std::chrono::_V2::system_clock::time_point;
#endif

// TODO: Simplify this, drop bytes.
/// @brief Simple packet header structure
/// @param parent_id_offset Subtract from current packet id to get
///                         the parent packet_id in the fragment chain.
///                         We use an offset to save 3 bytes in the header.
typedef struct {
  uint32_t packet_id;
  uint8_t parent_id_offset;
  uint8_t packet_type;
  uint8_t fragments;
  status_bits_t status_bits;
  uint16_t length;
} vrts_packetheader_t;

/// @brief Structure for local packet trees.
/// @details currently fixed size for initial
typedef struct {
  vrts_packetheader_t header;
  uint8_t data[1472 - sizeof(vrts_packetheader_t)];
} vrts_packet_t;

typedef struct {
  vrts_packet_t ota_packet;
  bool was_sent;
  bool was_acked;
  bool was_nacked;
  uint8_t retx_count;
  vrts_clock_time_t sent_time_local;
  vrts_clock_time_t acked_time_local;
  vrts_clock_time_t nacked_time_local;

} vrts_local_txdata_t;

// TODO: Replace with copy of last slice_segment_header and parser state?
typedef struct {
  h265nal::H265BitstreamParserState bitstream_parser_state;
  bool was_last_slice_first;
  uint32_t last_poc_count;
  uint32_t running_poc;
  uint32_t last_slice_state;
  bool pending_contains_pps;
  uint16_t max_poc;
} parse_tracking_data_t;

typedef struct {
  vrts_packet_t ota_packet;
  vrts_clock_time_t received_time_local;
  bool ack_sent;
  bool in_consumer_queue;
} vrts_local_rxdata_t;

/// @brief Structure for collecting statistics.
typedef struct {
  int64_t timestamp_ms;
  uint64_t send_byte_total;
  uint64_t recv_byte_total;
  uint32_t send_byte_since;
  uint32_t recv_byte_since;
  uint32_t send_pkt_total;
  uint32_t send_pkt_since;
  uint32_t send_pkt_dropped;
  uint32_t send_pkt_loss;
  uint32_t recv_pkt_total;
  uint32_t recv_pkt_since;
  uint32_t ack_total;
  uint32_t ack_not_in_tree;
  uint32_t nack_total;
  uint32_t nack_since;
  uint32_t retx_total;
  uint32_t retx_since;
  uint32_t tx_queued;
  uint32_t rx_queued;
  uint32_t gop_requests;
  uint32_t send_buf_ms;
  uint16_t pending_acks;
  uint16_t rtt_peak;
  uint16_t temporal_filter;
  float rtt_average;
  float rtt_acked;
  float rtt_nacked;
  float tx_in_transit;
  float send_pkt_rate;
  float recv_pkt_rate;
  float send_byte_rate;
  float recv_byte_rate;
  float average_mbps;
  float retx_rate;
  float retx_percent;
  float drop_percent;
  float loss_percent;
  float nack_rate;
} vrts_stat_t;

class VRTS {
public:
  VRTS(uint16_t downstream_port, uint16_t upstream_port,
       std::string downstream_ip, std::string upstream_ip,
       uint16_t sync_rate_hz);
  ~VRTS();

  bool parse(uint8_t *data, size_t len);
  bool dataReady(void);

  bool setMtu(uint16_t new_mtu);
  const std::vector<uint8_t> getData(void);
  const std::vector<uint8_t> getDataAsMPEGTS(void);
  void getStatistics(vrts_stat_t& stats);
  void resetStatistics(void);
  void updateAgeRemovalThreshold(uint32_t age_threshold_ms);
  void updateUnACKedRetransmitTimeThreshold(uint32_t retx_threshold_ms);
  void updateMaxUnACKedPacketsInTransit(uint32_t max_unack_count);
  void updateReTXLimitPerPacket(uint32_t retx_count_max);
  void updateTemporalFilterLatencyThreshold(uint32_t in_transit_latency_ms);
  bool newGOPRequested(void);

private:
  std::shared_ptr<std::thread> tx_handler;
  std::shared_ptr<std::thread> rx_handler;
  uint16_t sync_hz;
  uint16_t mtu;
  std::map<uint32_t, vrts_local_txdata_t> tx_stream_tree;
  std::map<uint32_t, vrts_local_rxdata_t> rx_stream_tree;
  std::mutex tx_tree_mutex;
  std::mutex rx_tree_mutex;
  uint32_t current_packet_id;
  std::atomic<bool> keep_running;
  std::atomic<bool> service_tx_tree;
  std::atomic<bool> new_gop_needed;
  std::atomic<bool> flush_tx_tree;
  std::atomic<uint32_t> total_sent;
  std::atomic<uint16_t> max_unacked_full_rate;
  std::atomic<uint16_t> unacked;

  // Input/output parsing
  std::vector<uint8_t> pending_input_entry;
  // Experimental option to drop NALs by type.
  std::vector<h265nal::NalUnitType> filter_list;
  std::vector<h265nal::NalUnitType> threshold_list;
  parse_tracking_data_t input_state;
  parse_tracking_data_t output_state;
  // What should be going out in nacks / acks
  std::atomic<uint8_t> global_status_bit_state;
  // Tracking input status bit state over time
  std::atomic<uint8_t> last_input_status_bit_state;

  // Tunables
  std::atomic<uint32_t> rx_id_discard_threshold;
  std::atomic<std::chrono::milliseconds> removal_age_threshold;
  std::atomic<std::chrono::milliseconds> retransmit_time_threshold;
  std::atomic<uint32_t> max_unacked_items_allowed;
  std::atomic<uint32_t> retransmit_count_limit;
  std::atomic<uint32_t> temporal_layer_filter_latency_threshold;

  std::atomic<bool> should_ack;
  std::deque<std::vector<uint8_t>> output_queue;

  // mpegts related
  std::map<uint8_t, int> streamPidMap;
  vrts_clock_time_t mpegtsPtsStart;
  // Leave as void pointer so we don't have to add includes in this header.
  MpegTsMuxer* lMuxer;

  // Statistics collection
  vrts_stat_t statistics;
  std::mutex statistics_mutex;

  void rxHandler(std::string upstream_ip, uint16_t upstream_port,
                 std::string downstream_ip, uint16_t downstream_port);
  void txHandler(std::string upstream_ip, uint16_t upstream_port,
                 std::string downstream_ip, uint16_t downstream_port);

  std::string nalTypeToString(h265nal::NalUnitType nalType);
  void flushUpToNalType(h265nal::NalUnitType nal_type);

  void udpSend(uint8_t *data, uint16_t len, std::string ip, uint16_t port);
  bool udpRecv(vrts_local_rxdata_t &entry, std::string ip, uint16_t port);

  /// @brief parse output stream and return if stream is ok
  /// @todo At some point this shold specify what upstream needs to happen
  /// @returns true if stream OK, false if stream not OK and upstream shoul
  /// issue new iframe/drop pending frames.
  bool trackOutputStream(uint8_t *data, uint16_t len);

  /// @brief Consumes an elementary h265 stream.
  /// @details Assumes each chunk contains unfragmented NAL(s)
  /// @param data pointer to data chunk
  /// @param len length of data chunk
  void feedDataH265(uint8_t *data, uint16_t len);

  /// @brief Handle the request for a new GOP from downstream.
  void requestNewGOPHandler(uint8_t downstream_state);

  /// @brief Deletes all entries in the tx-tree.
  void flushTXTree(void);

  /// @brief Deletes all entries in the rx-tree.
  void flushRXTree(void);
};

} // namespace vrts

#endif
