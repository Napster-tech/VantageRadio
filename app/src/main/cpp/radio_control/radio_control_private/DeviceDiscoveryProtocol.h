#pragma once

#include "DeviceDiscoveryProtocolMessage.h"
#include "HostAddress.h"
#include "MacAddress.h"
#include "UdpBroadcaster.h"

class DeviceDiscoveryProtocol {
public:
  enum { UDP_PORT = 22484 };

  explicit DeviceDiscoveryProtocol();

  void setLocalOnly(bool localOnly);
  void setInterfaces(const std::vector<HostAddress> &interfaces);

  bool discover(bool listAll = false);
  bool query(const MacAddress &macAddress, bool listAll = false);

  bool configureNetwork(const MacAddress &macAddress, bool dhcpEnabled,
                        const HostAddress &ipAddress,
                        const HostAddress &netmask, const HostAddress &gateway);

  bool flashLed(const MacAddress &macAddress);
  bool unlockEthernetPort(const MacAddress &macAddress);

  // VR:
  ByteArray prepareDeviceQuery(const MacAddress &ourMacAddress,
                               const MacAddress &targetMacAddress);
  ByteArray discoverVantageArray(const MacAddress &ourMac, bool listAll);
  std::string toJson(const DeviceDiscoveryProtocolMessage &message,
                     const std::string &indent, bool listAll);
  std::map<std::string, std::string>
  toMap(const DeviceDiscoveryProtocolMessage &message, bool listAll);

  // END VR

  uint32_t sendDeviceQuery(const MacAddress &targetMacAddress = MacAddress());
  uint32_t sendNetworkConfiguration(const MacAddress &targetMacAddress,
                                    bool dhcpEnabled,
                                    const HostAddress &ipAddress,
                                    const HostAddress &netmask,
                                    const HostAddress &gateway);
  uint32_t sendFlashLed(const MacAddress &targetMacAddress);
  uint32_t sendUnlockEthernetPort(const MacAddress &targetMacAddress);

  uint32_t getNextMessageId();

  bool waitForReply(DeviceDiscoveryProtocolMessage &message, uint8_t commandId,
                    uint32_t messageId, uint64_t endTime);

  void addString(std::map<std::string, std::string> &map,
                 const std::string &key,
                 const DeviceDiscoveryProtocolMessage &message,
                 DeviceDiscoveryProtocolMessage::DataType type);

  void addInt(std::map<std::string, std::string> &map, const std::string &key,
              const DeviceDiscoveryProtocolMessage &message,
              DeviceDiscoveryProtocolMessage::DataType type);

  MacAddress m_ourMacAddress;
  UdpBroadcaster m_udpBroadcaster;
  UdpSocket m_recvSock;
  std::vector<ByteArray> m_messagesReceived;
  bool m_localOnly = false;
};
