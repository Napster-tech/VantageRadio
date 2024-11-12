#include "VRTS.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <iostream>
#include <net/ethernet.h>
#include <net/if.h>
#include <netinet/ether.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <random>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace vrts {

#ifdef __ANDROID__
using chrono_clock = std::chrono::system_clock;
#else
using chrono_clock = std::chrono::high_resolution_clock;
#endif

// TODO: Stop sending packets from the current GOP and issue a new i-frame
// preemptively if we havent' gotten an ACK packet back in some time.
// This would allow us to short-circuit a GOP without needing to get a status
// bit request from the downstream consumer. (Impossible in case of link fully
// jammed). constexpr auto downlink_stall_detection_threshold =
// std::chrono::milliseconds(400);

// Default tunable parameters.
constexpr auto default_removal_age_threshold =
    std::chrono::milliseconds(200);
// Default is to only retransmit when NACK received. This value being greater
// than the removal age threshold stops it from retransmitting just based on
// un-ack period.
constexpr auto default_retransmit_time_threshold =
    std::chrono::milliseconds(240);
// By default we allow a ton of unacked items.
// TODO: When we have tight regulation of data rates and ways to drop GOP /
// layers in place, this value should end up getting tuned.
constexpr uint32_t default_max_unacked_items_allowed = 200;

// How many unacked items are allowed to be in transit before we start to drop
// TRAIL_N frames.
constexpr uint32_t default_max_unacked_full_rate = 16;
constexpr uint32_t default_retransmit_count_limit = 4;
constexpr uint32_t default_temporal_layer_filter_latency = 300;
static constexpr uint16_t MaxUDPPayloadSize = 1472;

// Used to reset the threshold, i.e. in the case of connecting to an in-process
// VRTS downstream where the upstream has been recently restarted from scratch.
static constexpr uint32_t reset_rx_id_threshold = 500;

// H.264 video
#define TYPE_VIDEO_264 0x1b
#define TYPE_VIDEO_265 0x24
// Video PID
#define VIDEO_PID 0x41
// PMT PID
#define PMT_PID 0x20

// Statistical moving averages
constexpr float ma_alpha = 0.9f;
constexpr float ma_beta  = 1.0f - ma_alpha;
template <typename T>
void moving_average(float& average, T value) {
  if (average != 0.0f) {
    average = ma_alpha * average + ma_beta * value;
  } else {
    average = value;
  }
}

// Used for conditional logging flexibility.
constexpr bool debug_to_cout = true;
class vrcout {
public:
  vrcout &operator()(bool indent = false) {
    if (indent)
      std::cout << '\t';
    return *this;
  }

  template <class T> vrcout &operator<<(T t) {
    if (debug_to_cout) {
      std::cout << t;
    }
    return *this;
  }

  vrcout &operator<<(std::ostream &(*f)(std::ostream &o)) {
    if (debug_to_cout) {
      std::cout << f;
    }
    return *this;
  };
};

VRTS::VRTS(uint16_t downstream_port, uint16_t upstream_port,
           std::string downstream_ip, std::string upstream_ip,
           uint16_t sync_rate_hz)
    : sync_hz{sync_rate_hz}, mtu{1472 - sizeof(vrts_packetheader_t)},
      current_packet_id{0}, service_tx_tree{false},
      new_gop_needed{false},
      flush_tx_tree{false}, total_sent{0},
      max_unacked_full_rate{default_max_unacked_full_rate},
      unacked{0},
      global_status_bit_state{status_bits_t::NEW_GOP_NEEDED},
      last_input_status_bit_state{0},
      rx_id_discard_threshold{0},
      removal_age_threshold{default_removal_age_threshold},
      retransmit_time_threshold{default_retransmit_time_threshold},
      max_unacked_items_allowed{default_max_unacked_items_allowed},
      retransmit_count_limit{default_retransmit_count_limit},
      temporal_layer_filter_latency_threshold{default_temporal_layer_filter_latency},
      should_ack{false} {
  keep_running = true;
  resetStatistics();

  // Input h265 state tracking.
  input_state.running_poc = 0;
  input_state.last_slice_state = 0xFFFFFFFF;
  input_state.last_poc_count = 0;
  input_state.was_last_slice_first = false;
  input_state.pending_contains_pps = false;

  // Output h265 state tracking.
  output_state.running_poc = 0;
  output_state.last_slice_state = 0xFFFFFFFF;
  output_state.last_poc_count = 0;
  output_state.was_last_slice_first = false;
  output_state.pending_contains_pps = false;

  streamPidMap[TYPE_VIDEO_265] = VIDEO_PID;
  lMuxer = new MpegTsMuxer(streamPidMap, PMT_PID, VIDEO_PID,
                           MpegTsMuxer::MuxType::segmentType);
  // Our PTS for the mpegts stream currently starts when we start to receive
  // frames.
  mpegtsPtsStart = chrono_clock::now();

  // I don't think that if FakeRadioLoop throws an exception that it will be
  // caught within the scope of the constructor body...
  tx_handler = std::make_shared<std::thread>(
      [this, upstream_ip, upstream_port, downstream_ip, downstream_port] {
        txHandler(upstream_ip, upstream_port, downstream_ip, downstream_port);
      });
  rx_handler = std::make_shared<std::thread>(
      [this, upstream_ip, upstream_port, downstream_ip, downstream_port] {
        rxHandler(upstream_ip, upstream_port, downstream_ip, downstream_port);
      });
}

VRTS::~VRTS() {
  keep_running = false;
  if (tx_handler) {
    if (tx_handler->joinable()) {
      tx_handler->join();
    }
  }
  if (rx_handler) {
    if (rx_handler->joinable()) {
      rx_handler->join();
    }
  }
}

/// @brief Feeds data into the tx tree to be handled by VRTS
/// @details Will break up large input packets into MTU sized chunks
/// @param data pointer to data to put into the tree
/// @param len lenth of data to put into the tree
void VRTS::feedDataH265(uint8_t *data, uint16_t len) {
  std::lock_guard<std::mutex> tx_tree_lock{tx_tree_mutex};
  int chunks = len / mtu;
  int leftovers = len - (chunks * mtu);
  int fragments_total = chunks;
  if (leftovers > 0) {
    fragments_total++;
  }

  // Entry that we'll fill and then put into the tree.
  vrts_local_txdata_t empty_entry = {};

  uint32_t parent_packet_id = current_packet_id;
  for (int i = 0; i < chunks; i++) {
    // Make the map item that we'll copy our data into.
    tx_stream_tree.insert(std::make_pair(current_packet_id, empty_entry));
    // Get a reference to the allocated
    auto &this_chunk = tx_stream_tree[current_packet_id];
    this_chunk.was_sent = false;
    this_chunk.was_acked = false;
    this_chunk.was_nacked = false;
    this_chunk.retx_count = 0;
    this_chunk.ota_packet.header.fragments = fragments_total;
    this_chunk.ota_packet.header.length = mtu;
    this_chunk.ota_packet.header.packet_type = vrts_packet_type_t::VRTS_DATA;
    this_chunk.ota_packet.header.packet_id = current_packet_id;
    this_chunk.ota_packet.header.parent_id_offset =
        static_cast<uint8_t>(current_packet_id - parent_packet_id);
    memcpy(this_chunk.ota_packet.data, &(data[i * mtu]), mtu);
    current_packet_id++;
  }

  // Handle any packets smaller than the mtu size
  if (leftovers) {
    // Make the map item that we'll copy our data into.
    tx_stream_tree.insert(std::make_pair(current_packet_id, empty_entry));
    // Get a reference to the allocated
    auto &this_chunk = tx_stream_tree[current_packet_id];
    this_chunk.was_sent = false;
    this_chunk.was_acked = false;
    this_chunk.was_nacked = false;
    this_chunk.retx_count = 0;
    this_chunk.ota_packet.header.fragments = fragments_total;
    this_chunk.ota_packet.header.length = leftovers;
    this_chunk.ota_packet.header.packet_type = vrts_packet_type_t::VRTS_DATA;
    this_chunk.ota_packet.header.packet_id = current_packet_id;
    this_chunk.ota_packet.header.parent_id_offset =
        static_cast<uint8_t>(current_packet_id - parent_packet_id);
    memcpy(this_chunk.ota_packet.data, &(data[chunks * mtu]), leftovers);
    current_packet_id++;
  }
  service_tx_tree = true;
}

/// @brief set new MTU length
/// @param new_mtu new mtu size in bytes
bool VRTS::setMtu(uint16_t new_mtu) {
  // VRTS payload must be able to fit into the maximum UDP payload size that can
  // fit into a single L2 ethernet frame
  if (mtu - sizeof(vrts_packetheader_t) <= MaxUDPPayloadSize) {
    mtu = new_mtu;
    return true;
  }
  return false;
}

/// @brief rxHandler thread, consider combining with txHandler.
void VRTS::rxHandler(std::string upstream_ip, uint16_t upstream_port,
                     std::string downstream_ip, uint16_t downstream_port) {
  vrts_local_rxdata_t rx_entry = {};
  while (keep_running) {
    std::vector<uint32_t> chunks_to_be_removed;
    while (udpRecv(rx_entry, upstream_ip, upstream_port)) {
      // ACKs of any type are never put into the tree.
      // All ACK messages are created on-the-fly based on
      // the messages in the tree and immediately sent out.
      vrcout() << "[vrts] rx type = " << (int)rx_entry.ota_packet.header.packet_type << std::endl;

      if (rx_entry.ota_packet.header.packet_type ==
          vrts_packet_type_t::VRTS_ACKS) {
        // Check for any piggy-backed status updates:
        requestNewGOPHandler(rx_entry.ota_packet.header.status_bits);

        uint16_t ack_count = rx_entry.ota_packet.header.length / 4;
        vrcout() << "[vrts] got ack count: " << ack_count << std::endl;

        for (int i = 0; i < ack_count; i++) {
          uint32_t acked_packet_id =
              reinterpret_cast<uint32_t *>(&(rx_entry.ota_packet.data))[i];
          try {
            std::lock_guard<std::mutex> tx_tree_lock{tx_tree_mutex};
            // Make sure it's actually in the tree before using the overloaded
            // [] access type.
            if (tx_stream_tree.find(acked_packet_id) != tx_stream_tree.end()) {
              tx_stream_tree[acked_packet_id].was_acked = true;
              tx_stream_tree[acked_packet_id].acked_time_local =
                  chrono_clock::now();
              statistics.ack_total++;
            } else {
              vrcout() << "[vrts] acked packet id: " << acked_packet_id
                       << " NOT in tree" << std::endl;
              statistics.ack_total++;
              statistics.ack_not_in_tree++;
            }
          } catch (...) {
            vrcout() << "[vrts] caught exception accesing map" << std::endl;
          }
        }
      } else if (rx_entry.ota_packet.header.packet_type ==
                 vrts_packet_type_t::VRTS_NACKS) {
        // Check for any piggy-backed status updates:
        requestNewGOPHandler(rx_entry.ota_packet.header.status_bits);

        auto nack_received_time = chrono_clock::now();
        uint16_t nack_count = rx_entry.ota_packet.header.length / 4;
        vrcout() << "[vrts] got nack count: " << nack_count << std::endl;
        for (int i = 0; i < nack_count; i++) {
          uint32_t nacked_packet_id =
              reinterpret_cast<uint32_t *>(&(rx_entry.ota_packet.data))[i];
          try {
            std::lock_guard<std::mutex> tx_tree_lock{tx_tree_mutex};
            // Make sure it's actually in the tree before using the overloaded
            // [] access type.
            if (tx_stream_tree.find(nacked_packet_id) != tx_stream_tree.end()) {
              if (tx_stream_tree[nacked_packet_id].was_acked) {
                vrcout()
                    << "[vrts] == WARNING: Got NACK for a packet marked ACKed."
                    << std::endl;
              }
              vrcout() << "[vrts] == Got NACK for a packet: "
                       << nacked_packet_id << std::endl;
              if (!tx_stream_tree[nacked_packet_id].was_nacked) {
                statistics.nack_total++;
                statistics.nack_since++;

                auto& chunk = tx_stream_tree[nacked_packet_id];
                auto chunk_rtt =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        nack_received_time - chunk.sent_time_local);

                moving_average(statistics.rtt_average, chunk_rtt.count());
                moving_average(statistics.rtt_nacked, chunk_rtt.count());
                if (chunk_rtt.count() > statistics.rtt_peak)
                  statistics.rtt_peak = chunk_rtt.count();
              }

              tx_stream_tree[nacked_packet_id].was_nacked = true;
              tx_stream_tree[nacked_packet_id].nacked_time_local =
                  chrono_clock::now();
            } else {
              vrcout() << "[vrts] nacked packet id: " << nacked_packet_id
                       << " NOT in tree" << std::endl;
            }
          } catch (...) {
            vrcout() << "[vrts] caught exception accesing map" << std::endl;
          }
        }
      } else if (rx_entry.ota_packet.header.packet_type ==
                 vrts_packet_type_t::VRTS_DATA) {
        // The insert() method won't overwrite an entry.
        // rx_stream_tree.insert(
        //    std::make_pair(rx_entry.ota_packet.header.packet_id, rx_entry));
        // Use overloaded [] for insertion and overwriting of an entry such
        // that if we receive a packet twice that has already been ACK'd, we
        // will ACK the packet again. Receiving the packet twice implies the
        // upstream has not yet gotten an ACK for this packet.
        // TODO: Overwriting an entry and updating its received time could
        // mess up timing statistics...
        std::lock_guard<std::mutex> rx_tree_lock{rx_tree_mutex};
        rx_entry.received_time_local = chrono_clock::now();
        rx_entry.ack_sent = false;
        // Make sure it's not already in the tree before using the overloaded
        // [] access type.
        if (rx_id_discard_threshold == 0 ||
            (rx_entry.ota_packet.header.packet_id > rx_id_discard_threshold)) {
          if (rx_stream_tree.find(rx_entry.ota_packet.header.packet_id) ==
              rx_stream_tree.end()) {
            rx_stream_tree[rx_entry.ota_packet.header.packet_id] = rx_entry;
            vrcout() << "[vrts] == got packet_id: "
                     << std::to_string(rx_entry.ota_packet.header.packet_id)
                     << " offset: "
                     << std::to_string(
                            rx_entry.ota_packet.header.parent_id_offset)
                     << " fragments: "
                     << std::to_string(rx_entry.ota_packet.header.fragments)
                     << std::endl;
          } else {
            // If we get a lot of these, then we need to adjust ACK/NACK logic
            vrcout() << "[vrts] == received non-flushed duplicate packet_id: "
                     << std::to_string(rx_entry.ota_packet.header.packet_id)
                     << std::endl;
          }
        } else {
          // Hacky way to reset the discard threshold if we're reconnecting.
          // TODO: Deal wth this more gracefully.
          if ((((int64_t)rx_id_discard_threshold) -
               ((int64_t)rx_entry.ota_packet.header.packet_id)) >
              reset_rx_id_threshold) {
            vrcout() << "[vrts] == resetting rx_id_discard_threshold to 0"
                     << std::endl;
            rx_id_discard_threshold = 0;
          } else {
            vrcout() << "[vrts] == rx_id_discard_threshold >= id: "
                     << std::to_string(rx_entry.ota_packet.header.packet_id)
                     << std::endl;
          }
        }
      } else {
        vrcout() << "[vrts] got unhandled packet type." << std::endl;
      }
    }

    // Criteria for periodic removal from the rx_stream_tree:
    // - Data is ready to be enqueued into output buffer
    // - Data is too old and should be removed, e.g. from old GOP
    // - TBD

    // Empty out the buffer.
    // Rules for this are pretty simple:
    // - Don't flush any fragmented packets.
    // - Only flush packets oldest to newest.
    // - Stop flushing packets once we hit a fragmented packet.
    //
    // The map is a sorted tree by key value
    // First value in the map is smallest, last is largest

    // TODO: keep going until we hit stop condition.
    // Only check this if the size of the map has changed since the last time
    // we did this. This scope touches the rx_stream_tree state.
    {
      std::lock_guard<std::mutex> rx_tree_lock{rx_tree_mutex};
      std::vector<uint32_t> nack_ids;
      // Check may not be needed:
      if (rx_stream_tree.size()) {
        uint32_t oldest_id = rx_stream_tree.begin()->first;
        auto &chunk = rx_stream_tree[oldest_id];
        bool complete_chain = false;
        if (!chunk.in_consumer_queue) {
          // Let's see if we have all the data we need
          // to reconstruct the original un-fragmented data.
          // Should never receive an ota data packet with fragment count 0.
          vrcout() << "[vrts] == attempting flush" << std::endl;
          if (chunk.ota_packet.header.fragments >= 1 &&
              chunk.ota_packet.header.parent_id_offset == 0) {
            // If this chunk is part of a fragment chain and the
            // offset isn't zero, we lost preceeding packets and
            // must wait for them to arrive.
            vrcout() << "[vrts]     fragment count "
                    << std::to_string(chunk.ota_packet.header.fragments)
                    << std::endl;
            bool keep_checking = true;
            for (int i = 0;
                (i < chunk.ota_packet.header.fragments && keep_checking); i++) {
              try {
                // - If we look ahead and the parent id_offset of subsequent
                // packets doesn't match this iterator value as we access the
                // map, then the chain is broken and we must wait for
                // retransmissions or subsequent transmissions to occur.
                // - First check to see if we even have the keys we would need.
                if (rx_stream_tree.find(oldest_id + i) == rx_stream_tree.end()) {
                  vrcout() << "[vrts]     chain links unavailable " << std::endl;
                  complete_chain = false;
                  keep_checking = false;
                } else {
                  vrcout() << "[vrts]      link available" << std::endl;
                  auto &link = rx_stream_tree[oldest_id + i];
                  if (link.ota_packet.header.parent_id_offset == i) {
                    vrcout() << "[vrts]     chain unbroken count " << i
                            << std::endl;
                    complete_chain = true;
                  } else {
                    complete_chain = false;
                    keep_checking = false;
                    vrcout() << "[vrts]     chain broken" << std::endl;
                  }
                }
              } catch (...) {
                vrcout() << "[vrts] caught exception looking for complete chain"
                        << std::endl;
                complete_chain = false;
              }
            }
          } // If the first packet in our tree does not have an offset id of 0,
            // we're missing preceeding packets.
          else if (chunk.ota_packet.header.parent_id_offset != 0) {
            complete_chain = false;
            uint32_t this_id = chunk.ota_packet.header.packet_id;
            uint32_t chain_parent_id =
                this_id - chunk.ota_packet.header.parent_id_offset;
            vrcout() << "[vrts] == this_id: " << std::to_string(this_id)
                    << " parent-id: " << std::to_string(chain_parent_id)
                    << std::endl;
            vrcout() << "[vrts] == missing leading packets: ";
            // Enqueue nacks for the missing preceeding items.
            for (uint32_t nack_id = chain_parent_id; nack_id < this_id;
                nack_id++) {
              vrcout() << std::to_string(nack_id) << " ";
              nack_ids.emplace_back(nack_id);
            }
            vrcout() << std::endl;
          }
        }

        // TODO: Add rejection criteria:
        // - Don't enqueue a NAL that we've already enqueued within the GOP
        // - Don't enqueue a NAL if we're waiting on a NACKd chunk that should
        // be coming before this full NAL. I.e. if NACKd packet ID is at all
        // before any packet IDs in this chunk, we gotta wait!
        // (duplicate)
        if (complete_chain) {
          std::vector<uint8_t> nal;
          for (int i = 0; i < chunk.ota_packet.header.fragments; i++) {
            try {
              auto &link = rx_stream_tree[oldest_id + i];
              nal.insert(nal.end(), link.ota_packet.data,
                         link.ota_packet.data + link.ota_packet.header.length);
              link.in_consumer_queue = true;
              // Defer tree removal until packet acked AND in consumer queue

              // Update our up-front filter for dropping ids that come in that
              // are too old:
              rx_id_discard_threshold = oldest_id + i;
            } catch (...) {
              vrcout() << "[vrts] output: caught exception at rx nal enqueue"
                       << std::endl;
            }
          }

          if (trackOutputStream(nal.data(), nal.size())) {
            // parse output stream, check for errors.
            output_queue.emplace_back(nal);
          }
          vrcout() << "[vrts] nal emplaced" << std::endl;
        }

        // Schedule packets for removal once ACK sent and in consumer queue
        for (auto &entry : rx_stream_tree) {
          auto &chunk = entry.second;
          if (chunk.ack_sent && chunk.in_consumer_queue) {
            uint32_t this_id = chunk.ota_packet.header.packet_id;
            vrcout() << "[vrts] output: erasing id: " << this_id
                      << std::endl;
              chunks_to_be_removed.emplace_back(this_id);
          }
        }
      }

      // Now look for any discontinuities in the keys we've received
      // These are missing packets that we need to NACK.
      uint32_t last_id = 0;
      for (auto &entry : rx_stream_tree) {
        uint32_t id = entry.second.ota_packet.header.packet_id;
        if (last_id == 0) {
          last_id = id;
        } else {
          uint8_t difference = labs((long int)id - (long int)last_id);
          if (difference > 1) {
            vrcout() << "[vrts] detected missing id id/last" << id << "/"
                     << last_id << std::endl;
            for (uint32_t missing_id = last_id + 1; missing_id < id;
                 missing_id++) {
              vrcout() << "[vrts] " << missing_id << std::endl;
              nack_ids.emplace_back(missing_id);
            }
          }
          last_id = id;
        }
      }

      if (nack_ids.size()) {
        vrts_local_txdata_t nack = {};
        // Put the chunk's packet ID into the ack packet's payload.
        // TODO: Handle this case...
        if (nack_ids.size() > (1472 - sizeof(vrts_packetheader_t)) / 4) {
          vrcout() << "[vrts] hit nack payload size limit - nacks backing up "
                      "/ dropped"
                   << std::endl;
        } else {
          uint32_t *indexes =
              reinterpret_cast<uint32_t *>(&(nack.ota_packet.data[0]));
          for (int i = 0; i < nack_ids.size(); i++) {
            indexes[i] = nack_ids.at(i);
          }

          nack.was_sent = true;
          nack.was_acked = true;
          nack.was_nacked = true;

          // Set flags
          nack.ota_packet.header.status_bits =
              static_cast<status_bits_t>(global_status_bit_state.load());

          nack.sent_time_local = chrono_clock::now();
          nack.ota_packet.header.fragments = 0;
          nack.ota_packet.header.packet_id = 0;
          nack.ota_packet.header.parent_id_offset = 0;
          nack.ota_packet.header.packet_type = vrts_packet_type_t::VRTS_NACKS;
          // Specify how much ack data there is in this ota_packet.
          nack.ota_packet.header.length = nack_ids.size() * sizeof(uint32_t);

          // Don't put the ack packet into the tx tree. But do update the
          // packet id.
          // tx_stream_tree.insert(std::make_pair(current_packet_id, ack));
          udpSend(reinterpret_cast<uint8_t *>(&(nack.ota_packet)),
                  sizeof(vrts_packetheader_t) + nack.ota_packet.header.length,
                  downstream_ip, downstream_port);
          vrcout() << "[vrts] == sending nack chunk of size: "
                   << nack.ota_packet.header.length << std::endl;
        }
      }
    }

    // Scope touches rx_stream_tree state
    // In this scope we remove entries from the tree and queue entries against
    // age criteria for removal
    {
      std::lock_guard<std::mutex> rx_tree_lock{rx_tree_mutex};
      auto now = chrono_clock::now();
      for (auto &entry : rx_stream_tree) {
        auto &chunk = entry.second;
        auto chunk_age = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - chunk.received_time_local);
        if ((chunk_age > removal_age_threshold.load()) &&
            !chunk.in_consumer_queue) {
          vrcout() << "[vrts] == OLD " << chunk_age.count()
                   << " [ms] | erasing id: "
                   << chunk.ota_packet.header.packet_id << std::endl;
          chunks_to_be_removed.emplace_back(chunk.ota_packet.header.packet_id);
        }
      }

      // Remove chunks that have been slated for removal from our receive
      // tree.
      for (auto id : chunks_to_be_removed) {
        try {
          auto& chunk = rx_stream_tree[id];
          if (!chunk.ack_sent) {
            vrcout() << "[vrts] removing unacked packet " << chunk.ota_packet.header.packet_id << std::endl;
          }
          rx_stream_tree.erase(id);
        } catch (...) {
          vrcout()
              << "[vrts] caught exception releasing items in the rx_stream_tree"
              << std::endl;
        }
      }
    }

    // In the rxHandler thread we should have decision thresholds for
    // allowing data to go to the output queue. E.g. are we skipping the next
    // P-frame chunk to catch up on missed packets?
    // Delay should be a setting.
    std::this_thread::sleep_for(std::chrono::microseconds(250));
  }

  vrcout() << "[vrts] rxHandler exiting" << std::endl;
}

void VRTS::txHandler(std::string upstream_ip, uint16_t upstream_port,
                     std::string downstream_ip, uint16_t downstream_port) {
  auto last_ack_check = chrono_clock::now();
  // TODO: Limit how many items can be outstanding
  while (keep_running) {
    // Every time we service / access the tree, we should take a look at the
    // conditons that might require a chunk to be removed from memory.
    // Conditions:
    // - Packet is too old (based on wall time threshold)
    // - Packet is from a GOP that we wont be decoding
    // - Packet has been acked successfully
    // - TBD
    std::vector<uint32_t> chunks_to_be_removed;
    if (service_tx_tree) {
      std::lock_guard<std::mutex> tx_tree_lock{tx_tree_mutex};
      unacked = 0;
      int nacked = 0;

      // Handle NACK'd packets first.
      for (auto &entry : tx_stream_tree) {
        auto &chunk = entry.second;
        auto now = chrono_clock::now();
        auto last_send_period =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - chunk.sent_time_local);
        // TODO: Make nack transmission rate limit a param / constexpr.
        if (chunk.was_nacked == true &&
            last_send_period > std::chrono::milliseconds(10) &&
            chunk.retx_count < retransmit_count_limit) {
          chunk.sent_time_local = now;
          chunk.retx_count++;
          udpSend(reinterpret_cast<uint8_t *>(&(chunk.ota_packet)),
                  sizeof(vrts_packetheader_t) + chunk.ota_packet.header.length,
                  downstream_ip, downstream_port);
          vrcout() << "[vrts] sent nacked packet id: "
                   << chunk.ota_packet.header.packet_id << std::endl;

          statistics.retx_total++;
          statistics.retx_since++;
        }

        if (chunk.was_nacked) {
          nacked++;
         }

        // While we're iterating, keep track of how many items have been
        // unacked.
        if (!chunk.was_acked && chunk.was_sent) {
          unacked++;
          statistics.pending_acks = unacked;
          moving_average(statistics.tx_in_transit, unacked.load());
        }
      }

      if (nacked > 0) {
        vrcout() << "[vrts] nacked in tree: " << nacked << std::endl;
      }

      // If we have too many packets in transit, don't transmit more data.
      // TODO: If the backlog of data grows long enough that a new GOP has
      // entered the TX tree, clear all the data up to that point? Perhaps that
      // should happen before the data enters the tree at all?
      // TODO: If the unack count is starting to grow, trim the data that's
      // supposed to be sent before it's sent, e.g. drop a temporal layer.
      if (unacked < max_unacked_items_allowed) {
        for (auto &entry : tx_stream_tree) {
          auto &chunk = entry.second;
          if (chunk.was_sent == false) {
            udpSend(reinterpret_cast<uint8_t *>(&(chunk.ota_packet)),
                    sizeof(vrts_packetheader_t) +
                        chunk.ota_packet.header.length,
                    downstream_ip, downstream_port);
            chunk.was_sent = true;
            chunk.sent_time_local = chrono_clock::now();
            vrcout() << "[vrts] sending chunk of size: "
                     << chunk.ota_packet.header.length << std::endl;
            total_sent++;
          } else if (chunk.was_acked == false) {
            //   TODO: This timing theshold needs to be a setting.
            //   Since we have 1-way ACK currently do we want to limit
            //   how many retransmits we allow per packet?
            auto unack_period =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    chrono_clock::now() - chunk.sent_time_local);
            if (unack_period > removal_age_threshold.load()) {
              // Queue to remove because too old.
              vrcout() << "[vrts] == id : " << chunk.ota_packet.header.packet_id
                       << " queued for removal due to age" << std::endl;
              chunks_to_be_removed.emplace_back(
                  chunk.ota_packet.header.packet_id);
              if (statistics.ack_total > 0) {
                statistics.send_pkt_loss++;
              }
              else {
                // no losses until after first ack, backout send
                --statistics.send_pkt_total;
                --statistics.send_pkt_since;
                statistics.send_byte_total -= chunk.ota_packet.header.length;
                statistics.send_byte_since -= chunk.ota_packet.header.length;
                vrcout() << "[vrts] NO CONNECTION:  send total " << statistics.send_pkt_total << " " << statistics.send_byte_total << std::endl;
              }
            } else if (unack_period > retransmit_time_threshold.load()) {
              udpSend(reinterpret_cast<uint8_t *>(&(chunk.ota_packet)),
                      sizeof(vrts_packetheader_t) +
                          chunk.ota_packet.header.length,
                      downstream_ip, downstream_port);
              chunk.sent_time_local = chrono_clock::now();
              vrcout() << "[vrts] re-tx unack period: " << unack_period.count()
                       << " [ms], chunk of size : "
                       << chunk.ota_packet.header.length << std::endl;
              chunk.retx_count++;
              statistics.retx_total++;
              statistics.retx_since++;
              if (chunk.retx_count > retransmit_count_limit) {
                vrcout() << "[vrts] == id : "
                         << chunk.ota_packet.header.packet_id
                         << " queued for removal due to retx limit"
                         << std::endl;
                chunks_to_be_removed.emplace_back(
                    chunk.ota_packet.header.packet_id);
              }
            }
          } else if (chunk.was_acked && chunk.was_sent) {
            auto chunk_rtt =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    chrono_clock::now() - chunk.sent_time_local);
            moving_average(statistics.rtt_average, chunk_rtt.count());
            moving_average(statistics.rtt_acked, chunk_rtt.count());
            if (chunk_rtt.count() > statistics.rtt_peak)
              statistics.rtt_peak = chunk_rtt.count();
            vrcout() << "[vrts] rtt : " << chunk_rtt.count() << " avg "
                                        << statistics.rtt_average << std::endl;

            // If the chunk has been sent and acked, we don't need it any
            // longer.
            vrcout() << "[vrts] == id : " << chunk.ota_packet.header.packet_id
                     << " acked/sent will remove" << std::endl;
            chunks_to_be_removed.emplace_back(
                chunk.ota_packet.header.packet_id);
          }
        }
      } else {
        statistics.send_pkt_dropped++;
      }

      if (unacked) {
        vrcout() << "[vrts] unacked in tree: " << unacked.load() << std::endl;
        vrcout() << "[vrts] total_sent: " << total_sent.load() << std::endl;
      }

      // Remove chunks slated for removal.
      for (auto id : chunks_to_be_removed) {
        vrcout() << "[vrts] == removing packet from tx: " << id << std::endl;
        tx_stream_tree.erase(id);
      }
      chunks_to_be_removed.clear();
      service_tx_tree = false;
    }

    auto ack_check_wait = std::chrono::duration_cast<std::chrono::milliseconds>(
        chrono_clock::now() - last_ack_check);

    // ACK Handling.
    if ((ack_check_wait >= std::chrono::milliseconds(1000 / sync_hz)) ||
        should_ack) {
      std::lock_guard<std::mutex> tx_tree_lock{tx_tree_mutex};
      last_ack_check = chrono_clock::now();
      should_ack = false;

      // ACK scope:
      // Here we handle sending out acks based on the data we've received.
      {
        std::lock_guard<std::mutex> rx_tree_lock{rx_tree_mutex};
        vrts_local_txdata_t ack = {};

        // Find out if we need to ack anything new.
        int ack_count = 0;
        for (auto &entry : rx_stream_tree) {
          auto &chunk = entry.second;
          // If we haven't sent out an ack for this packet we received, or we
          // haven't gotten confirmation that our ack was received, send
          // another ack out in the next ACK packet for a given chunk id.
          if (chunk.ack_sent == false) {
            chunk.ack_sent = true;
            // Put the chunk's packet ID into the ack packet's payload.
            if (ack_count > (1472 - sizeof(vrts_packetheader_t)) / 4) {
              vrcout() << "[vrts] hit ack payload size limit - acks backing up "
                          "/ dropped"
                       << std::endl;
              continue;
            }
            uint32_t *indexes =
                reinterpret_cast<uint32_t *>(&(ack.ota_packet.data[0]));
            indexes[ack_count] = chunk.ota_packet.header.packet_id;
            auto ack_delay =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    chrono_clock::now() - chunk.received_time_local);
            vrcout() << "[vrts] packet " << chunk.ota_packet.header.packet_id
                     << " ack delay: " << ack_delay.count()
                     << " [ms]" << std::endl;
            ack_count++;
          }
        }

        // ACKs and NACK packets are special in the sense that we
        // don't try to resend them. As such we just say upfront that
        // they need no further acking/retransmission.
        if (ack_count) {
          ack.was_sent = true;
          ack.was_acked = true;
          ack.was_nacked = false;

          // Set flags
          ack.ota_packet.header.status_bits =
              static_cast<status_bits_t>(global_status_bit_state.load());

          ack.sent_time_local = chrono_clock::now();
          ack.ota_packet.header.fragments = 0;
          ack.ota_packet.header.packet_id = 0;
          ack.ota_packet.header.parent_id_offset = 0;
          ack.ota_packet.header.packet_type = vrts_packet_type_t::VRTS_ACKS;
          // Specify how much ack data there is in this ota_packet.
          ack.ota_packet.header.length = ack_count * sizeof(uint32_t);

          // Don't put the ack packet into the tx tree. But do update the
          // packet id.
          // tx_stream_tree.insert(std::make_pair(current_packet_id, ack));
          udpSend(reinterpret_cast<uint8_t *>(&(ack.ota_packet)),
                  sizeof(vrts_packetheader_t) + ack.ota_packet.header.length,
                  downstream_ip, downstream_port);
          vrcout() << "[vrts] sending ack chunk of size: "
                   << ack.ota_packet.header.length << std::endl;
        }
      }

      // Remove chunks that have been slated for removal from our transmit
      // tree.
      for (auto id : chunks_to_be_removed) {
        tx_stream_tree.erase(id);
      }

      // On the next loop service transmit/retransmit scope
      service_tx_tree = true;
    }

    // Delay should be a setting.
    std::this_thread::sleep_for(std::chrono::microseconds(500));
  }
}

// TODO: Speed up udp recv
bool VRTS::udpRecv(vrts_local_rxdata_t &entry, std::string ip, uint16_t port) {
  // TODO: ADD SYNC TYPE THAT ALLOWS US TO WIPE THE TRANSMISSION TREE LIKE IN
  // THE CASE OF STARTING A NEW SESSION ONLY ON ONE END
  static int fd = -1;
  if (fd < 0) {
    if ((fd = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
      vrcout() << "[vrts] failed to get socket udpRecv" << std::endl;
      return false;
    }

    int one = 1;
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof tv);
    // //fcntl(fd, F_SETFL, O_NONBLOCK);
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));

    struct sockaddr_in rx_addr;
    memset(&rx_addr, 0, sizeof(rx_addr));
    rx_addr.sin_family = AF_INET;
    rx_addr.sin_addr.s_addr = inet_addr(ip.c_str());
    rx_addr.sin_port = htons(port);

    /* Bind to the broadcast port */
    if (bind(fd, (struct sockaddr *)&rx_addr, sizeof(rx_addr)) < 0) {
      vrcout() << "[vrts] bind failed " << ip << " udpRecv: " << errno << std::endl;
      return false;
    }
  }

  //struct sockaddr_in from;
  int received = read(fd, reinterpret_cast<uint8_t *>(&(entry.ota_packet)),
                      sizeof(vrts_packet_t));
  // socklen_t addrlen = sizeof(struct sockaddr_in);
  //    recvfrom(fd, reinterpret_cast<uint8_t *>(&(entry.ota_packet)),
  //             sizeof(vrts_packet_t), 0, (struct sockaddr *)&from,
  //             &addrlen);

  if (received > 0) {
    entry.ack_sent = false;
    vrcout() << "[vrts] received: " << received << std::endl;
    statistics.recv_pkt_total++;
    statistics.recv_pkt_since++;
    statistics.recv_byte_total += received;
    statistics.recv_byte_since += received;
    return true;
  }
  if (received < 0) {
    int err = errno;
    if (err != EAGAIN) {
      close(fd);
      fd = -1;
      vrcout() << "[vrts] recvfrom failed udpRecv: " << err << std::endl;
    }
  }
  return false;
}

void VRTS::udpSend(uint8_t *data, uint16_t len, std::string ip, uint16_t port) {
  int tx_sock = -1;
  struct sockaddr_in destaddr;
  if (tx_sock < 0) {
    if ((tx_sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
    }

    int one = 1;
    if (setsockopt(tx_sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0) {
    }

    if (setsockopt(tx_sock, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one)) < 0) {
    }

    // Broadcast details
    memset(&destaddr, 0, sizeof(destaddr));
    destaddr.sin_family = AF_INET;
    destaddr.sin_addr.s_addr = inet_addr(ip.c_str());
    destaddr.sin_port = htons(port);
  }

  if (sendto(tx_sock, data, len, 0, (sockaddr *)&destaddr, sizeof(destaddr)) <=
      0) {
    vrcout() << "[vrts] send failed" << std::endl;
  } else {
    statistics.send_pkt_total++;
    statistics.send_pkt_since++;
    statistics.send_byte_total += len;
    statistics.send_byte_since += len;
  }


  close(tx_sock);
}

// This function will go through the tree
void VRTS::flushUpToNalType(h265nal::NalUnitType) {
  std::lock_guard<std::mutex> tx_tree_lock{tx_tree_mutex};
  // Iterate through the tx_tree and flush all packets from oldest up to the
  // first packet with the nal type passed into this function Could be used to
  // flush our internal streaming buffer after we've received a new IDR /
  // CRA_NUT / etc...
  // - This is currently very dumb and will just drop all items in the tree.
  if (tx_stream_tree.size()) {
    // vrcout() << "[vrts] new gop - clearing tree of item count: "
    //          << tx_stream_tree.size() << std::endl;
    // tx_stream_tree.clear();
  }
}

bool VRTS::dataReady(void) { return (output_queue.size() > 0); }

const std::vector<uint8_t> VRTS::getData(void) {
  const auto vec = output_queue.front();
  output_queue.pop_front();
  return vec;
}

const std::vector<uint8_t> VRTS::getDataAsMPEGTS(void) {
  vrcout() << "Getting mpegts" << std::endl;
  // Get our NAL blocks out.
  const auto vec = output_queue.front();
  output_queue.pop_front();

  // Build a frame of data (ES)
  EsFrame esFrame;
  esFrame.mData = std::make_shared<SimpleBuffer>();
  // Append your ES-Data
  esFrame.mData->append(vec.data(), vec.size());

  // We set both the pts and dts to the current local time.
  uint64_t pts = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(chrono_clock::now() - mpegtsPtsStart)
          .count());

  vrcout() << "PTS/DTS: " << pts << std::endl;

  esFrame.mPts = pts;
  esFrame.mDts = pts;
  esFrame.mPcr = pts; // This isn't correct, but tsparse should handle it.
  esFrame.mStreamType = TYPE_VIDEO_265;
  esFrame.mStreamId = 224;
  esFrame.mPid = VIDEO_PID;
  esFrame.mExpectedPesPacketLength = 0;
  esFrame.mCompleted = true;

  // TODO: Rework mpegts lib to hand out vector directly.
  auto ts_out = lMuxer->encode(esFrame);
  std::vector<uint8_t> vec_out;
  vec_out.assign(ts_out.data(), ts_out.data() + ts_out.size());
  return vec_out;
}

void VRTS::getStatistics(vrts_stat_t& stats) {
  static vrts_clock_time_t time_start = chrono_clock::now();
  static vrts_clock_time_t time_local = time_start;

  std::lock_guard<std::mutex> stats_lock(statistics_mutex);
  if (statistics.send_pkt_total < 5) return;

  vrts_clock_time_t statistics_now = chrono_clock::now();
  statistics.timestamp_ms =
    std::chrono::duration_cast<std::chrono::milliseconds>(
      statistics_now - time_start).count();

  // queue sizes
  statistics.tx_queued = tx_stream_tree.size();
  statistics.rx_queued = rx_stream_tree.size();

  auto age_since_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    statistics_now - time_local).count();
  float delta =  age_since_ms / 1000.0f;
  if (delta > 1.0f) delta = 1.0f;

  if (statistics.send_pkt_since > 0) {
    uint32_t send_pkt_unique = statistics.send_pkt_since - statistics.retx_since;
    statistics.retx_percent = static_cast<float>(statistics.retx_since) /
                              static_cast<float>(send_pkt_unique);
  }
  if (statistics.send_pkt_total) {
    uint32_t send_pkt_unique = statistics.send_pkt_total - statistics.retx_total;
    statistics.drop_percent = 100.0f * static_cast<float>(statistics.send_pkt_dropped) /
                              static_cast<float>(send_pkt_unique);
    statistics.loss_percent = 100.0f * static_cast<float>(statistics.send_pkt_loss) /
                              static_cast<float>(send_pkt_unique);
  }
  uint32_t total_bytes = statistics.send_byte_since + statistics.recv_byte_since;

  // Determine oldest unacked tx packet in transit
  {
    statistics.send_buf_ms = 0;
    std::lock_guard<std::mutex> tx_tree_lock{tx_tree_mutex};
    auto now = chrono_clock::now();
    for (auto &entry : tx_stream_tree) {
      auto &chunk = entry.second;
      if (chunk.was_sent && !chunk.was_acked) {
        auto last_send_period =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - chunk.sent_time_local).count();
        if (last_send_period > statistics.send_buf_ms) {
          statistics.send_buf_ms = last_send_period;
        }
      }
    }
  }

  // update moving averages for rates
  if (delta >= 0.0001f) {
    float send_byte_rate = statistics.send_byte_since / delta;
    float recv_byte_rate = statistics.recv_byte_since / delta;
    float send_pkt_rate = statistics.send_pkt_since / delta;
    float recv_pkt_rate = statistics.recv_pkt_since / delta;
    float retx_rate = statistics.retx_since / delta;
    float nack_rate = statistics.nack_since / delta;
    float total_mbps = (total_bytes / delta) * (8.0f / 1000000.0f);
    moving_average(statistics.send_byte_rate, send_byte_rate);
    moving_average(statistics.recv_byte_rate, recv_byte_rate);
    moving_average(statistics.send_pkt_rate, send_pkt_rate);
    moving_average(statistics.recv_pkt_rate, recv_pkt_rate);
    moving_average(statistics.retx_rate, retx_rate);
    moving_average(statistics.nack_rate, nack_rate);
    moving_average(statistics.average_mbps, total_mbps);
  }

  stats = statistics;

  // Reset rate counters every 1/10th of a second
  if (delta >= 0.1f) {
    statistics.send_byte_since = 0;
    statistics.recv_byte_since = 0;
    statistics.send_pkt_since = 0;
    statistics.recv_pkt_since = 0;
    statistics.retx_since = 0;
    statistics.nack_since = 0;
    time_local = statistics_now;
  }
}

void VRTS::resetStatistics(void) {
  std::lock_guard<std::mutex> stats_lock(statistics_mutex);
  memset(&statistics, 0, sizeof(statistics));
}

// For details on the NAL unit types, see the H265 spec
// https://www.itu.int/rec/dologin_pub.asp?lang=e&id=T-REC-H.265-201802-S!!PDF-E&type=items
// TSA: Temporal Sublayer Access
// STSA: Stepwise Temporal Sublayer Access
// _R: Reference (i)
// _N: Non-reference (p/b)
// More useful details:
// https://datatracker.ietf.org/doc/html/draft-schierl-payload-rtp-h265-01
std::string VRTS::nalTypeToString(h265nal::NalUnitType nalType) {
  std::string type;
  switch (nalType) {
  case h265nal::NalUnitType::TRAIL_N:
    type = "TRAIL_N: non-reference slice segment, non (S)TSA";
    break;
  case h265nal::NalUnitType::TRAIL_R:
    type = "TRAIL_R: reference slice segment, non (S)TSA";
    break;
  case h265nal::NalUnitType::TSA_N:
    type = "TSA_N: non-reference slice segment temporal sublayer access";
    break;
  case h265nal::NalUnitType::TSA_R:
    type = "TSA_R: reference slice segment temporal sublayer access";
    break;
  case h265nal::NalUnitType::STSA_N:
    type = "STSA_N: non-reference slice segment stepwise temporal sublayer "
           "access";
    break;
  case h265nal::NalUnitType::STSA_R:
    type = "STSA_R: reference slice segment stepwise temporaal sublayer access";
    break;
  case h265nal::NalUnitType::RADL_N:
    type = "ADL_R: non-reference slice segment random access decodable leading";
    break;
  case h265nal::NalUnitType::RADL_R:
    type = "RADL_R: reference slice segment random access decodable leading";
    break;
  case h265nal::NalUnitType::RASL_N:
    type = "RASL_N: non-reference slice segment random access skipped leading";
    break;
  case h265nal::NalUnitType::RASL_R:
    type = "RASL_R: reference slice segment random access skipped leading";
    break;
  case h265nal::NalUnitType::IDR_W_RADL:
    type = "IDR_W_RADL: instantaneous decoding refresh picture / leading RADL "
           "slices";
    break;
  case h265nal::NalUnitType::IDR_N_LP:
    type = "IDR_N_LP: instantaneous decoding refresh picture / no leading "
           "pictures";
    break;
  case h265nal::NalUnitType::CRA_NUT:
    // A CRA picture does not refer to any pictures other than itself for
    // inter prediction in its decoding process, and
    // may be the first picture in the bitstream in decoding order, or may
    // appear later in the bitstream. A CRA picture may have associated RADL
    // or RASL pictures. As with a BLA picture, a CRA picture may contain
    // syntax elements that specify a non-empty RPS. When a CRA picture has
    // NoRaslOutputFlag equal to 1, the associated RASL pictures are not
    // output by the decoder, because they may not be decodable, as they may
    // contain references to pictures that are not present in the bitstream.
    type = "CRA_NUT: clean random access picture (like gop start)";
    break;
  case h265nal::NalUnitType::VPS_NUT:
    type = "VPS_NUT: video parameter set";
    break;
  case h265nal::NalUnitType::SPS_NUT:
    type = "SPS_NUT: sequence parameter set";
    break;
  case h265nal::NalUnitType::PPS_NUT:
    type = "PPS_NUT: picture parameter set";
    break;
  case h265nal::NalUnitType::AUD_NUT:
    type = "AUD_NUT: access unit delimiter";
    break;
  case h265nal::NalUnitType::EOS_NUT:
    type = "EOS_NUT: end of sequence";
    break;
  case h265nal::NalUnitType::EOB_NUT:
    type = "EOB_NUT: end of bitstream";
    break;
  case h265nal::NalUnitType::FD_NUT:
    type = "FD_NUT: filler data!";
    break;
  case h265nal::NalUnitType::PREFIX_SEI_NUT:
    type = "PREFIX_SEI_NUT: prefix supplemental enhancement information";
    break;
  default:
    type = std::to_string(nalType);
    break;
  }
  return type;
}

// Currently our logic for handling errors in the stream is extremely simplistic
// where our only actions are to...
// - Ask upstream for new GOP AND
// - Tell upstream to drop packets AND
// - Don't push data to the decoder until new GOP arrives
// ...IF
// - POC jump is detected with missing slices
// - POC jump detected without temporal layers / short term references enabled
// - We haven't ever received a PPS (connecting mid-GOP)
bool VRTS::trackOutputStream(uint8_t *data, uint16_t len) {
  // What do we do with this?
  if (data[0] != 0x00 || data[1] != 0x00 || data[2] != 0x00 ||
      data[3] != 0x01) {
    vrcout()
        << "[vrts] parse-out: buffer doesn't start with start code 0x00000001"
        << std::endl;
    return false;
  }

  h265nal::ParsingOptions parsing_options;
  parsing_options.add_offset = true;
  auto stream = h265nal::H265BitstreamParser::ParseBitstream(
      data, len, &output_state.bitstream_parser_state, parsing_options);
  if (stream == nullptr) {
    // did not work
    vrcout() << " [vrcout] parse-out: Couldn't determine NAL indices..."
             << std::endl;
    return false;
  }

  uint32_t highest_tid = 0;
  for (auto &nalu : stream->nal_units) {
    uint8_t *offset = &(data[nalu->offset]);
    if (nalu->nal_unit_payload->slice_segment_layer != nullptr) {
      auto &ssh =
          nalu->nal_unit_payload->slice_segment_layer->slice_segment_header;

      highest_tid = std::max(highest_tid,
                            nalu->nal_unit_header->nuh_temporal_id_plus1);

      // Look for input discontinuties in POC count:
      long poc_jump = labs((long int)output_state.running_poc -
                (long int)ssh->slice_pic_order_cnt_lsb);
      if (poc_jump > 1 &&
          (ssh->slice_pic_order_cnt_lsb != 255) &&
          ssh->slice_pic_order_cnt_lsb != 0) {
        // Carve out exception for 50% temporal filtering
        //   - POC gap is one picture
        //   - Running POC was even number
        //   - Highest temporal id indicates filtered by sender
        if (poc_jump == 1 && highest_tid <= 2 &&
            (output_state.running_poc & 1 == 0)) {
          // highest_temporal_layer<=2 assumes 50% temporal layer filter
          // Skip odd POC ok when temporal filter active
          // TODO: pass bitmask to indicate percent of filtering
        } else {
          vrcout() << "[vrts] parse-out: POC jump detected: "
                  << output_state.running_poc << " / "
                  << ssh->slice_pic_order_cnt_lsb << std::endl;
          global_status_bit_state |= ((uint8_t)status_bits_t::NEW_GOP_NEEDED);
        }

        // TODO: We may want to force sending out a packet here with the
        // NEW_GOP_NEEDED bit set.

        // At this point we should clear the pending rx tree?
        // In the future we might do something smarter than this.
        // flushRXTree();
      }

      // If our last POC count doesn't match the current POC, but the current
      // slice isn't the fist in the POC, that's bad!
      if (output_state.last_poc_count != ssh->slice_pic_order_cnt_lsb &&
          !ssh->first_slice_segment_in_pic_flag) {
        vrcout() << "[vrts] parse-out: Unexpected POC jump across slices. "
                    "Requesting New GOP."
                 << std::endl;
        global_status_bit_state |= ((uint8_t)status_bits_t::NEW_GOP_NEEDED);

        // TODO: We may want to force sending out a packet here with the
        // NEW_GOP_NEEDED bit set.

        // At this point we should clear the pending rx tree?
        // In the future we might do something smarter than this.
        // flushRXTree();
      }

      vrcout() << "[vrts] parse-out: - POC: " << ssh->slice_pic_order_cnt_lsb
               << " first slice: " << ssh->first_slice_segment_in_pic_flag
               << std::endl;

      // Update last state tracking vars.
      // TODO: Replace with copy of last slice_segment_header?
      output_state.was_last_slice_first = ssh->first_slice_segment_in_pic_flag;
      output_state.last_poc_count = ssh->slice_pic_order_cnt_lsb;
      output_state.running_poc = ssh->slice_pic_order_cnt_lsb;
    }

    // If we've gotten a PPS_NUT, an IDR frame is surely to follow.
    // TODO: This is an assumption based on our encoder configuration.
    if (nalu->nal_unit_header->nal_unit_type == h265nal::PPS_NUT) {
      global_status_bit_state &= ~((uint8_t)status_bits_t::NEW_GOP_NEEDED);
    }
  }

  if (global_status_bit_state & status_bits_t::NEW_GOP_NEEDED) {
    vrcout() << "[vrts] parse-out: Dropping input data, waiting for new GOP."
             << std::endl;
    return false;
  }

  return true;
}

bool VRTS::parse(uint8_t *data, size_t len) {
  static bool inited = false;
  if (!inited) {
    inited = true;
    // We don't want to pass fillers through.
    filter_list.push_back(h265nal::NalUnitType::FD_NUT);
    // filter_list.push_back(h265nal::AUD_NUT); // Don't strictly need this
    filter_list.push_back(h265nal::PREFIX_SEI_NUT);
    // Experimentation list:
    // See what happens if we only ever let trailing reference updates
    // through.
    // filter_list.push_back(h265nal::NalUnitType::TRAIL_N);
    // filter_list.push_back(h265nal::NalUnitType::TRAIL_R);

    // Don't currently drop the PREFIX_SEI_NUT as at the moment
    // it is included in the "first GOP" NAL aggregated chunk
    // and would drop the whole first big NAL.
    // filter_list.push_back(h265nal::NalUnitType::PREFIX_SEI_NUT);

    // The types that denote a "threshold" between a group of NALS
    // that we might want to align our decoder on or skip to reduce
    // birate.
    threshold_list.push_back(h265nal::NalUnitType::IDR_N_LP);
    threshold_list.push_back(h265nal::NalUnitType::IDR_W_RADL);
    threshold_list.push_back(h265nal::NalUnitType::CRA_NUT);
  }

  if (data[0] != 0x00 || data[1] != 0x00 || data[2] != 0x00 ||
      data[3] != 0x01) {
    vrcout() << "[vrts] buffer doesn't start with start code 0x00000001"
             << std::endl;
  }

  h265nal::ParsingOptions parsing_options;
  parsing_options.add_offset = true;
  auto stream = h265nal::H265BitstreamParser::ParseBitstream(
      data, len, &input_state.bitstream_parser_state, parsing_options);
  if (stream == nullptr) {
    // did not work
    vrcout() << " [vrcout] Couldn't determine NAL indices..." << std::endl;
    return false;
  }
  stream->parsing_options = parsing_options;

  if (stream->nal_units.size() == 0) {
    vrcout() << " [vrcout] Couldn't determine NAL indices..." << std::endl;
    return false;
  }

  bool smart_fill = true;
  if (smart_fill) {
    uint8_t temporal_filter = 0;
    for (auto &nalu : stream->nal_units) {
      bool drop_nal = false;
      // Use UNSPEC63 as the default value denoting that no NAL was present
      h265nal::NalUnitType nal_type = h265nal::NalUnitType::UNSPEC63;
      uint8_t *offset = &(data[nalu->offset]);
      // if (h265nal::H265NalUnitHeaderParser::GetNalUnitType(
      //         offset, nalu->length, nal_type)) {
      nal_type = static_cast<h265nal::NalUnitType>(
          nalu->nal_unit_header->nal_unit_type);
      if (nal_type != h265nal::NalUnitType::UNSPEC63) {
        int current_poc = -1;
        if (nalu->nal_unit_payload && nalu->nal_unit_payload->slice_segment_layer) {
          auto &ssh =
              nalu->nal_unit_payload->slice_segment_layer->slice_segment_header;
          if (ssh) current_poc = ssh->slice_pic_order_cnt_lsb;
        }
        vrcout() << "   [vrts] GOT NAL: " << nalTypeToString(nal_type)
                //  << " size: " << nalu->length << std::endl;
                 << " size: " << nalu->length
                 << " LID: " << nalu->nal_unit_header->nuh_layer_id
                 << " TID: " << nalu->nal_unit_header->nuh_temporal_id_plus1 - 1
                 << " POC: " << current_poc
                 << std::endl;
        // TODO: Use std::any_of()?
        for (auto type : filter_list) {
          if (type == nal_type) {
            drop_nal = true;
          }
        }

        // Reset running POC count on transition between GOPs.
        for (auto type : threshold_list) {
          if (type == nal_type) {
            input_state.running_poc = 0;
          }
        }

        // Our encoder currently is set up to output AUD_NUTs, so use them to
        // flush pending data chunks.
        if (pending_input_entry.size() &&
            nal_type == h265nal::NalUnitType::AUD_NUT) {
          vrcout() << "[vrts] Feeding h265 ES block of size: "
                   << pending_input_entry.size() << std::endl;
          if (new_gop_needed && !input_state.pending_contains_pps) {
            // Don't put this into the tree, just clear it.
            vrcout() << "[vrts] Dropping input until PPS arives." << std::endl;
          } else if (new_gop_needed && input_state.pending_contains_pps) {
            this->feedDataH265(pending_input_entry.data(),
                               pending_input_entry.size());
            // Assumes that we're fed blocks of NALS that contain both
            // PPS and gop transition NALS like IDR NAL(s) between AUD_NUTs
            vrcout() << "[vrts] new GOP request cleared." << std::endl;
            new_gop_needed = false;
            input_state.pending_contains_pps = false;
          } else {
            this->feedDataH265(pending_input_entry.data(),
                               pending_input_entry.size());
            input_state.pending_contains_pps = false;
          }

          pending_input_entry.clear();
        }

        // If we care about this NAL type, parse it further.
        if (!drop_nal) {
          if (nalu->nal_unit_payload->slice_segment_layer != nullptr) {
            auto &ssh = nalu->nal_unit_payload->slice_segment_layer
                            ->slice_segment_header;

            // Look for input discontinuties in POC count:
            if ((labs((long int)input_state.running_poc -
                      (long int)ssh->slice_pic_order_cnt_lsb) > 1) &&
                (ssh->slice_pic_order_cnt_lsb != 255)) {
              vrcout() << "[vrts] parse: POC jump detected: "
                       << input_state.running_poc << " / "
                       << ssh->slice_pic_order_cnt_lsb << std::endl;
            }

            if (input_state.last_poc_count != ssh->slice_pic_order_cnt_lsb &&
                !ssh->first_slice_segment_in_pic_flag) {
              vrcout() << "[vrts] parse: Unexpected POC jump across slices."
                       << std::endl;
            }

            vrcout() << "[vrts] parse - POC: " << ssh->slice_pic_order_cnt_lsb
                     << " first slice: " << ssh->first_slice_segment_in_pic_flag
                     << std::endl;

            // Update last state tracking vars.
            // TODO: Replace with copy of last slice_segment_header?
            input_state.was_last_slice_first =
                ssh->first_slice_segment_in_pic_flag;
            input_state.last_poc_count = ssh->slice_pic_order_cnt_lsb;
            input_state.running_poc = ssh->slice_pic_order_cnt_lsb;

            // Not all encoders are configured to put AUD_NUTs in the stream, so
            // don't expect to see them. Every time we see this flag, we can
            // flush our current slice to the tx tree depending on if we're
            // accepting the temporal layer or not / other preconditions.
            if (ssh->first_slice_segment_in_pic_flag) {
              // if (pending_entry.size()) {
              //   this->feedDataH265(data, len);
              //   pending_entry.clear();
              // }
            } else {
              // Subsequent slice in x-Frame
            }

            // We should flush the last chunk of data to the tx_tree once
            // slice_pic_order_cnt_lsb is incremented.
            if (ssh->short_term_ref_pic_set_sps_flag &&
                ssh->num_short_term_ref_pic_sets > 0) {
              // These indexes represent the temporal layers that we can drop if
              // we need to reduce bitrate.
              // TODO: Control the ability to drop from public interface.
              // TODO: Automatically track the number of temporal layers?
              // uint8_t temporal_layer_cutoff = 1;
              // TODO: This needs to be sync'ed to POC blocks correctly before
              // we can use it. AS OF THIS MOMENT WE CAN DROP TRAIL_N's TO ~
              // halve the bitrate.
              // if (ssh->short_term_ref_pic_set_idx == 3) {
              //   drop_nal = true;
              // }
              vrcout() << " [vrts] temporal layer: "
                       << ssh->short_term_ref_pic_set_idx << std::endl;
            }
          } else {
            // Couldn't parse header, either not an image slice or PPS hasn't
            // arrived yet.
            if (nal_type != h265nal::NalUnitType::AUD_NUT) {
              vrcout()
                  << " [vrts] couldn't parse header, not image, or no PPS yet. "
                  << std::endl;
            }
          }
        }

        // If link is starting to get congested, drop our non-reference P-frames
        // to reduce bitrate. As of now there are two temporal layers, so this
        // is a binary 50% drop in configured bitrate.
        {
          // Lock the mutex briefly such that we don't get an in-process update
          // of the unacked count.
          std::lock_guard<std::mutex> tx_tree_lock{tx_tree_mutex};
          if (nal_type == h265nal::NalUnitType::TRAIL_N || nal_type == h265nal::NalUnitType::TRAIL_R) {
            uint32_t temporal_id = nalu->nal_unit_header->nuh_temporal_id_plus1;
            // OMX encoder will output either TID 3 (one layer) or 4 (4 layer hibrid)
            // Anything above 2 will achive 50% drop of temporal backs
            if (temporal_id > 2) {
              if (statistics.send_buf_ms > temporal_layer_filter_latency_threshold) {
                  vrcout() << "[vrts] dropping temporal layer due to congestion"
                           << " in transit " << unacked.load() << ", " << statistics.tx_in_transit << " (" << statistics.send_buf_ms << " ms)"
                           << std::endl;
                  temporal_filter |= 1 << (temporal_id - 3); // TODO: test with hybrid 4 layer encoding
                  drop_nal = true;
              } else {
                vrcout() << "[vrts] not dropping temporal layer"
                         << " in transit " << unacked.load() << ", " << statistics.tx_in_transit << " (" << statistics.send_buf_ms << " ms)"
                         << std::endl;
                drop_nal = false;

              }
            }
          }
        }

        // We can decide to drop this nal in this scope as well.
        if (!drop_nal) {
          // Add the NAL delimiter back to the stream.
          pending_input_entry.emplace_back(0);
          pending_input_entry.emplace_back(0);
          pending_input_entry.emplace_back(0);
          pending_input_entry.emplace_back(1);
          pending_input_entry.insert(pending_input_entry.end(), offset,
                                     &offset[nalu->length]);

          if (nal_type == h265nal::PPS_NUT) {
            input_state.pending_contains_pps = true;
          }
        }

        if (drop_nal) {
          vrcout() << "    [vrts] Dropping NAL" << std::endl;
        }
      }
    }
    statistics.temporal_filter = temporal_filter;
  } else {
    vrcout() << "[vrts] directly feeding block size: " << len << std::endl;
    this->feedDataH265(data, len);
  }

  return true;
  // TODO: Parse P frames based on their short_term_ref_pic_set_idx and keep
  // track of this in the tree, as we'll drop short term reference to reduce
  // bitrate.
  // TODO: Can we trust AUD_NUTs? Or do we have to track indexes and overall
  // sequence within the IDR?

  // TODO: Do we know how many slices there will be per AUD_NUT? - useful in the
  // case of not getting all of the slices between AUD_NUTs passed into parse in
  // one-go.
  // TODO: Do we want a single NAL per output buffer for the output of the
  // encoder?
  // TODO: Make this input function add an entry to the tree on a per-AUD_NUT
  // transition
  // TODO: The tree should treat many NALs between AUD_NUTs as a chain.

  // - Right now we will send groups of NALS out in one tx_tree chunk.
  // So, a PPS/VPS/CRA_NUT/Etc. that all came in as one out of the
  // encoder will be fed to the decoder on the other side in one go
  // after the chunks are recreated.
  // - We will internally track this chunk as CRA_NUT, or IDR / etc
  // nal type, even though this includes more than one NAL. We could make
  // the entry in the tree for the nal type a vector with all the NALS
  // included in the entry if we wish.
  // - This should also be where we up-front drop packets if we're skipping
  // the next GOP
}

/// @brief Handles the input status state through time
/// We can't just look at the request for a new GOP and then issue a new GOP
/// as there is pipeline delay. Here we track state changes and make changes
/// accordingly.
void VRTS::requestNewGOPHandler(uint8_t downstream_state) {
  // If we transitioned from a state of not needing a new GOP, to needing a new
  // GOP set the global atomic flag.
  if ((downstream_state & status_bits_t::NEW_GOP_NEEDED) &&
      !(last_input_status_bit_state & status_bits_t::NEW_GOP_NEEDED)) {
    // This will be cleared by the input parser.
    vrcout() << "[vrts] downstream requested new GOP" << std::endl;
    new_gop_needed = true;
    statistics.gop_requests++;

    // Flush tx tree here.
    flushTXTree();
  }

  last_input_status_bit_state = downstream_state;
}

void VRTS::flushTXTree(void) {
  std::lock_guard<std::mutex> tx_tree_lock{tx_tree_mutex};
  tx_stream_tree.clear();
  // We don't handle any outstanding IDs.
  // The downstream side that requested the
  // new GOP will purge its RX tree.
  // If we wanted to be smart about what exactly we clear,
  // we could iterate through the items like so:
  // for (auto &entry : tx_stream_tree) {
  //  auto &chunk = entry.second;
  //}
}

void VRTS::flushRXTree(void) {
  std::lock_guard<std::mutex> rx_tree_lock{rx_tree_mutex};
  rx_stream_tree.clear();
  // If we wanted to be smart about what exactly we clear,
  // we could iterate through the items like so:
  // for (auto &entry : rx_stream_tree) {
  //  auto &chunk = entry.second;
  //}
}

bool VRTS::newGOPRequested(void) { return new_gop_needed; }

void VRTS::updateAgeRemovalThreshold(uint32_t age_threshold_ms) {
  removal_age_threshold = std::chrono::milliseconds(age_threshold_ms);
}
void VRTS::updateUnACKedRetransmitTimeThreshold(uint32_t retx_threshold_ms) {
  retransmit_time_threshold = std::chrono::milliseconds(retx_threshold_ms);
}
void VRTS::updateMaxUnACKedPacketsInTransit(uint32_t max_unack_count) {
  max_unacked_items_allowed = max_unack_count;
}
void VRTS::updateReTXLimitPerPacket(uint32_t retx_count_max) {
  retransmit_count_limit = retx_count_max;
}

void VRTS::updateTemporalFilterLatencyThreshold(uint32_t in_transit_latency_ms) {
  temporal_layer_filter_latency_threshold = in_transit_latency_ms;
}

} // namespace vrts
