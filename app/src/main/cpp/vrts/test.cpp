#include "argparse.h"
#include <chrono>
#include <cstdlib>
#include <h265_bitstream_parser.h>
#include <h265_common.h>
#include <iostream>
#include <map>
#include <signal.h>
#include <thread>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "FakeRadioLink.h"
#include "VRTS.h"

// Set up a second loopback: ifconfig lo:2 128.0.0.1 netmask 255.0.0.0 up

// Test source: gst-launch-1.0 -v videotestsrc ! videoconvert ! videoscale ! video/x-raw,width=1280,height=720 ! x265enc bitrate=1000 ! option-string="bframes=0:intra-refresh=1:keyint=60:no-open-gop=1:repeat-headers=1" ! udpsink host=127.0.0.1 port=10000
// Test sink: GST_DEBUG=4 gst-launch-1.0 udpsrc address=127.0.0.1 port=10001 ! h265parse ! avdec_h265 ! autovideosink sync=false
bool keep_running = true;
FakeRadioLink *fake_link = nullptr;
vrts::VRTS *ts1 = nullptr;

typedef struct {
  uint8_t *data;
  uint16_t len;
  uint32_t gop_number;
} Entry;

std::map<uint16_t, Entry> nal_tree;

void signalCallbackHandler(int signum) {
  std::cout << "Caught signal " << signum << std::endl;
  keep_running = false;
}

int udpRecv(uint8_t *data, uint16_t len, uint16_t port) {
  int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0) {
    std::cout << "cannot open socket" << std::endl;
    return -1;
  }

  struct timeval tv;
  tv.tv_sec = 0;
  tv.tv_usec = 500000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof tv);

  struct sockaddr_in broadcastRxAddr;
  memset(&broadcastRxAddr, 0, sizeof(broadcastRxAddr));
  broadcastRxAddr.sin_family = AF_INET;
  broadcastRxAddr.sin_addr.s_addr = inet_addr("127.0.0.1");
  broadcastRxAddr.sin_port = htons(port);

  /* Bind to the broadcast port */
  bind(fd, (struct sockaddr *)&broadcastRxAddr, sizeof(broadcastRxAddr));
  struct sockaddr_in from;
  socklen_t addrlen = sizeof(struct sockaddr_in);
  int received = recvfrom(fd, data, len, 0, (struct sockaddr *)&from, &addrlen);
  close(fd);
  return received;
}

void udpSend(uint8_t *data, uint16_t len, uint16_t port) {
  static int tx_sock = -1;
  static struct sockaddr_in bindaddr;
  static struct sockaddr_in destaddr;
  if (tx_sock < 0) {
    if ((tx_sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
    }

    int one = 1;
    if (setsockopt(tx_sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0) {
    }

    if (setsockopt(tx_sock, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one)) < 0) {
    }

    // To
    memset(&destaddr, 0, sizeof(destaddr));
    destaddr.sin_family = AF_INET;
    destaddr.sin_addr.s_addr = inet_addr("127.0.0.1");
    destaddr.sin_port = htons(port);
  }
  sendto(tx_sock, data, len, 0, (sockaddr *)&destaddr, sizeof(destaddr));
}

void sendFunction(h265nal::NalUnitType type, uint8_t *data,
                  size_t len) { //, uint32_t gop_count) {
  // Keep track of what we've sent out - in some ADT that we can sync
  // over the air. It should keep track of the relationship between different
  // NALs that we send over the network so that once we can purge an entire
  // chain, we do so i.e. no need to retransmit anything that won't be
  // referenced in the decode pipeline after some point in time
  // Entry test;
  // nal_tree.insert(std::make_pair(gop_count, test));

  udpSend(data, len, 10001);
}

// One of the things we can do here is
// immediately drop non-reference slice segments if the ack rate
// starts to drop. Don't even need to issue a new i-frame.

int main(int argc, const char *argv[]) {
  argparse::ArgumentParser parser("vrts-test",
                                  "Vantage elementary stream test app.");
  parser.add_argument("-p", "--port", "port", true)
      .description("port to listen to for elementary stream input");
  parser.add_argument("-g", "--gcs", "gcs", false).description("act as host/gcs");

  parser.enable_help();
  auto err = parser.parse(argc, argv);
  if (err) {
    std::cout << err << std::endl;
    return -1;
  }

  if (parser.exists("help")) {
    parser.print_help();
    return 0;
  }

  signal(SIGINT, signalCallbackHandler);

  if (parser.exists("gcs")) {
    fake_link =
        new FakeRadioLink("127.0.0.1", 30001, "192.168.1.66", 30000);
    fake_link->setLossProbability(.05); // 5% packet loss probability
    fake_link->setMinMaxLatency(0, 0); // 0 - x mS latency per packet burst - needs work (not accurate at all)
    ts1 = new vrts::VRTS(20000, 30000, "192.168.20.30", "192.168.20.4", 100);
  } else {
    fake_link =
        new FakeRadioLink("127.0.0.1", 20001, "192.168.1.236", 20000);
    fake_link->setLossProbability(.05); // 5% packet loss probability
    fake_link->setMinMaxLatency(0, 0); // 0 - x mS latency per packet burst - needs work (not accurate at all)
    ts1 = new vrts::VRTS(30000, 20000, "192.168.20.4", "192.168.20.30", 100);
  }

  uint16_t port = 0;
  if (parser.exists("port")) {
    port = std::stoi(parser.get<std::string>("port"));
  }

  // Receive packets sent to 10000 on localhost.
  uint8_t data[65535];
  while (keep_running) {
    if (parser.exists("gcs")) {
      if (ts1->dataReady()) {
        //const auto nal_in = ts1->getData();
        //std::cout << "Got NAL of size: " << nal_in.size() << std::endl;
        // Send data.
        //udpSend(const_cast<uint8_t *>(nal_in.data()), nal_in.size(), port);
        const auto ts_in = ts1->getDataAsMPEGTS();
        std::cout << "Got TS of size: " << ts_in.size() << std::endl;
        // Send data.
        udpSend(const_cast<uint8_t *>(ts_in.data()), ts_in.size(), port);
      }
    } else {
      int len = udpRecv(data, 65535, port);
      if (len > 0) {
        std::cout << "got: " << len << std::endl;
        ts1->parse(data, len);
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  if (ts1) {
    delete (ts1);
  }
  return 0;
}
