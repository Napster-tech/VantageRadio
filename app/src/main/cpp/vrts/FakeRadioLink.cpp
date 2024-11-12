#include "FakeRadioLink.h"
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

void FakeRadioLink::FakeRadioLoop(std::string input_ip, uint16_t input_port,
                                  std::string output_ip, uint16_t output_port) {
  float current_delay = 0;
  std::random_device rd;  // obtain a random number from hardware
  std::mt19937 gen(rd()); // seed the generator

  std::knuth_b rand_engine;
  std::uniform_real_distribution<> uniform_zero_to_one(0.0, 1.0);

  while (keep_running) {
    auto packet_in = udpRecv(input_ip, input_port);
    if (packet_in.size() > 0) {
      if (((buf_deque.size() <= buffer_limit) && (buffer_limit > 0)) ||
          (buffer_limit == 0)) {
        // Decide if this packet gets dropped.
        if (uniform_zero_to_one(rand_engine) >= loss_probability) {
          buf_deque.push_back(packet_in);
          // If there's more data to send out, initialize the delay again.
          if (current_delay <= .1) {
            std::uniform_int_distribution<uint16_t> distr(min_ms_delay,
                                                          max_ms_delay);
            current_delay = static_cast<float>(distr(gen));
#ifdef DEBUG
            std::cout << "New Delay: " << current_delay << std::endl;
#endif
          }
        } else {
          std::cout << "========================= Randomly dropping packet." << std::endl;
        }
      }
    }

    if (current_delay <= .1 && buf_deque.size()) {
      // Push out a burset of packets
      while (buf_deque.size()) {
        std::cout << "Sending packet of size " << buf_deque.front().size()
                  << std::endl;
        udpSend(buf_deque.front(), output_ip, output_port);
        buf_deque.pop_front();
      }
    }

    // If there's more data to send out, initialize the delay again.
    if (buf_deque.size() && current_delay <= .1) {
      std::uniform_int_distribution<uint16_t> distr(min_ms_delay, max_ms_delay);
      current_delay = distr(gen);
#ifdef DEBUG
      std::cout << "New Delay: " << current_delay << std::endl;
#endif
    }

    if (current_delay > .1) {
      current_delay -= .1;
#ifdef DEBUG
      std::cout << "Current delay: " << current_delay << std::endl;
#endif
    }
    // Tight, dumb loop.
    // TODO: This loop is taking way too long, likely recv blocking
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
}

FakeRadioLink::FakeRadioLink(std::string input_ip, uint16_t input_port,
                             std::string output_ip, uint16_t output_port)
    : loss_probability{0}, min_ms_delay{0}, buffer_limit{0}, max_ms_delay{0} {
  keep_running = true;
  // I don't think that if FakeRadioLoop throws an exception that it will be
  // caught within the scope of the constructor body...
  radio_loop = std::make_shared<std::thread>(
      [this, input_ip, input_port, output_ip, output_port] {
        FakeRadioLoop(input_ip, input_port, output_ip, output_port);
      });
}

FakeRadioLink::~FakeRadioLink() {
  keep_running = false;
  if (radio_loop) {
    if (radio_loop->joinable()) {
      radio_loop->join();
    }
  }
}

void FakeRadioLink::setMinMaxLatency(uint16_t min_ms, uint16_t max_ms) {
  min_ms_delay = min_ms;
  max_ms_delay = max_ms;
}

void FakeRadioLink::setLossProbability(float probability) {
  loss_probability = probability;
}

void FakeRadioLink::setBufferSize(uint64_t num_bufs) {
  buffer_limit = num_bufs;
}

std::vector<uint8_t> FakeRadioLink::udpRecv(std::string ip, uint16_t port) {
  std::vector<uint8_t> recv;
  static uint8_t buf[30000];
  static int fd = -1;
  if (fd < 0) {
    fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 1;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof tv);

    struct sockaddr_in broadcastRxAddr;
    memset(&broadcastRxAddr, 0, sizeof(broadcastRxAddr));
    broadcastRxAddr.sin_family = AF_INET;
    broadcastRxAddr.sin_addr.s_addr = inet_addr(ip.c_str());
    broadcastRxAddr.sin_port = htons(port);

    /* Bind to the broadcast port */
    bind(fd, (struct sockaddr *)&broadcastRxAddr, sizeof(broadcastRxAddr));
  }

  struct sockaddr_in from;
  socklen_t addrlen = sizeof(struct sockaddr_in);
  int received = recvfrom(fd, buf, 30000, 0, (struct sockaddr *)&from, &addrlen);
  if (received > 0) {
    recv.insert(recv.end(), buf, buf + received);
  }
  return recv;
}

void FakeRadioLink::udpSend(std::vector<uint8_t> &packet, std::string ip,
                            uint16_t port) {
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

    // From
    memset(&bindaddr, 0, sizeof(bindaddr));
    bindaddr.sin_family = AF_INET;
    bindaddr.sin_addr.s_addr = inet_addr(ip.c_str());
    bindaddr.sin_port = 0;//htons(port);
    if (bind(tx_sock, (sockaddr *)&bindaddr, sizeof(bindaddr)) < 0) {
    }

    // Broadcast details
    memset(&destaddr, 0, sizeof(destaddr));
    destaddr.sin_family = AF_INET;
    destaddr.sin_addr.s_addr = inet_addr(ip.c_str());
    destaddr.sin_port = htons(port);
  }

  sendto(tx_sock, packet.data(), packet.size(), 0, (sockaddr *)&destaddr,
         sizeof(destaddr));
}