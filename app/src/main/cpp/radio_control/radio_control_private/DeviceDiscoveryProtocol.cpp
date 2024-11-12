#include "DeviceDiscoveryProtocol.h"
#include "Network.h"
#include "RandomNumber.h"
#include "StrUtils.h"
#include "Trace.h"

#include <iostream>

extern void die(const char *msg);

/////////////////////////////////////////////////////////////////////////////

DeviceDiscoveryProtocol::DeviceDiscoveryProtocol()
    : m_udpBroadcaster(UDP_PORT) {
  //  VR: We don't use this
  //  std::vector<MacAddress> macAddresses = Network::getMacAddresses();
  //if (macAddresses.empty())
  //  die("getMacAddresses() failed");

  //  VR: WE DON'T USE THIS
  //  m_ourMacAddress = macAddresses.at(0);
  //  for (auto a : macAddresses) {
  //    std::cout << a.toString() << std::endl;
  //  }
  //  TRACE("ourMacAddress: %s", m_ourMacAddress.toString().c_str());
  //
  //  if (!m_recvSock.isValid())
  //    die("UdpSocket() failed");
  //
  //  if (!m_recvSock.enableReuseAddr())
  //    die("enableReuseAddr() failed");
  //
  //  if (!m_recvSock.enableNonBlocking())
  //    die("enableNonBlocking() failed");
  //
  //  if (!m_recvSock.bind(INADDR_ANY, UDP_PORT))
  //    die("bind() failed");
}

/////////////////////////////////////////////////////////////////////////////

void DeviceDiscoveryProtocol::setLocalOnly(bool localOnly) {
  m_localOnly = localOnly;
}

/////////////////////////////////////////////////////////////////////////////

void DeviceDiscoveryProtocol::setInterfaces(
    const std::vector<HostAddress> &interfaces) {
  m_udpBroadcaster.setInterfaces(interfaces);
}

/////////////////////////////////////////////////////////////////////////////

bool DeviceDiscoveryProtocol::discover(bool listAll) {
  m_messagesReceived.clear();

  uint32_t messageId = sendDeviceQuery();

  std::string json = "[\n";
  std::string indent = "    ";
  int count = 0;

  uint64_t endTime = OperatingSystem::getCurrentTime() + 2'000'000;
  while (1) {
    DeviceDiscoveryProtocolMessage message;
    if (!waitForReply(message, DeviceDiscoveryProtocolMessage::DeviceQuery,
                      messageId, endTime))
      break;

    if (count++)
      json += ",\n";

    json += toJson(message, indent, listAll);
  }

  json += "\n]";

  printf("%s\n", json.c_str());

  return true;
}

ByteArray
DeviceDiscoveryProtocol::discoverVantageArray(const MacAddress &ourMac,
                                              bool listAll) {
  (void)listAll;
  return prepareDeviceQuery(ourMac, MacAddress(""));
}

/////////////////////////////////////////////////////////////////////////////

bool DeviceDiscoveryProtocol::query(const MacAddress &macAddress,
                                    bool listAll) {
  m_messagesReceived.clear();

  uint32_t messageId = sendDeviceQuery(macAddress);

  std::string json;

  uint64_t endTime = OperatingSystem::getCurrentTime() + 1'000'000;
  while (1) {
    DeviceDiscoveryProtocolMessage message;
    if (!waitForReply(message, DeviceDiscoveryProtocolMessage::DeviceQuery,
                      messageId, endTime))
      break;

    if (macAddress != message.getSourceMacAddress())
      continue;

    json = toJson(message, "", listAll);

    break;
  }

  if (json.empty())
    return false;

  printf("%s\n", json.c_str());

  return true;
}

/////////////////////////////////////////////////////////////////////////////

bool DeviceDiscoveryProtocol::configureNetwork(const MacAddress &macAddress,
                                               bool dhcpEnabled,
                                               const HostAddress &ipAddress,
                                               const HostAddress &netmask,
                                               const HostAddress &gateway) {
  m_messagesReceived.clear();

  uint32_t messageId = sendNetworkConfiguration(macAddress, dhcpEnabled,
                                                ipAddress, netmask, gateway);

  DeviceDiscoveryProtocolMessage message;
  uint64_t endTime = OperatingSystem::getCurrentTime() + 1'000'000;
  return waitForReply(message, DeviceDiscoveryProtocolMessage::ConfigureNetwork,
                      messageId, endTime);
}

/////////////////////////////////////////////////////////////////////////////

bool DeviceDiscoveryProtocol::flashLed(const MacAddress &macAddress) {
  m_messagesReceived.clear();

  uint32_t messageId = sendFlashLed(macAddress);

  DeviceDiscoveryProtocolMessage message;
  uint64_t endTime = OperatingSystem::getCurrentTime() + 1'000'000;
  return waitForReply(message, DeviceDiscoveryProtocolMessage::FlashLed,
                      messageId, endTime);
}

/////////////////////////////////////////////////////////////////////////////

bool DeviceDiscoveryProtocol::unlockEthernetPort(const MacAddress &macAddress) {
  m_messagesReceived.clear();

  uint32_t messageId = sendUnlockEthernetPort(macAddress);

  DeviceDiscoveryProtocolMessage message;
  uint64_t endTime = OperatingSystem::getCurrentTime() + 1'000'000;
  return waitForReply(message, DeviceDiscoveryProtocolMessage::UnlockEthernet,
                      messageId, endTime);
}

/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////

uint32_t
DeviceDiscoveryProtocol::sendDeviceQuery(const MacAddress &targetMacAddress) {
  // Form the command
  DeviceDiscoveryProtocolMessage command;

  command.setLocalOnly(m_localOnly);

  if (targetMacAddress.isValid())
    command.setTargetMacAddress(targetMacAddress);
  else
    command.setBroadcastTargetMacAddress();

  command.setSourceMacAddress(m_ourMacAddress);
  command.setMessageId(getNextMessageId());
  command.setCommandId(DeviceDiscoveryProtocolMessage::DeviceQuery);

  // Broadcast it on each network
  m_udpBroadcaster.broadcast(command.getMessage());

  return command.getMessageId();
}

ByteArray DeviceDiscoveryProtocol::prepareDeviceQuery(
    const MacAddress &ourMacAddress, const MacAddress &targetMacAddress) {
  // Form the command
  DeviceDiscoveryProtocolMessage command;

  command.setLocalOnly(m_localOnly);

  if (targetMacAddress.isValid())
    command.setTargetMacAddress(targetMacAddress);
  else
    command.setBroadcastTargetMacAddress();

  command.setSourceMacAddress(ourMacAddress);
  command.setMessageId(getNextMessageId());
  command.setCommandId(DeviceDiscoveryProtocolMessage::DeviceQuery);

  // Broadcast it on each network
  return command.getMessage();
}

/////////////////////////////////////////////////////////////////////////////

uint32_t DeviceDiscoveryProtocol::sendNetworkConfiguration(
    const MacAddress &targetMacAddress, bool dhcpEnabled,
    const HostAddress &ipAddress, const HostAddress &netmask,
    const HostAddress &gateway) {
  DeviceDiscoveryProtocolMessage command;

  command.setLocalOnly(m_localOnly);
  command.setTargetMacAddress(targetMacAddress);
  command.setSourceMacAddress(m_ourMacAddress);
  command.setMessageId(getNextMessageId());
  command.setCommandId(DeviceDiscoveryProtocolMessage::ConfigureNetwork);

  command.appendData(DeviceDiscoveryProtocolMessage::DhcpEnabled, dhcpEnabled);
  if (!dhcpEnabled) {
    command.appendData(DeviceDiscoveryProtocolMessage::StaticIpAddress,
                       ipAddress);
    command.appendData(DeviceDiscoveryProtocolMessage::StaticNetmask, netmask);
    command.appendData(DeviceDiscoveryProtocolMessage::StaticGateway, gateway);
  }

  // Broadcast it on each network
  m_udpBroadcaster.broadcast(command.getMessage());

  return command.getMessageId();
}

/////////////////////////////////////////////////////////////////////////////

uint32_t
DeviceDiscoveryProtocol::sendFlashLed(const MacAddress &targetMacAddress) {
  // Form the command
  DeviceDiscoveryProtocolMessage command;

  command.setLocalOnly(m_localOnly);
  command.setTargetMacAddress(targetMacAddress);
  command.setSourceMacAddress(m_ourMacAddress);
  command.setMessageId(getNextMessageId());
  command.setCommandId(DeviceDiscoveryProtocolMessage::FlashLed);
  command.appendData(DeviceDiscoveryProtocolMessage::LedEnable, (uint8_t)1);

  // Broadcast it on each network
  m_udpBroadcaster.broadcast(command.getMessage());

  return command.getMessageId();
}

/////////////////////////////////////////////////////////////////////////////

uint32_t DeviceDiscoveryProtocol::sendUnlockEthernetPort(
    const MacAddress &targetMacAddress) {
  // Form the command
  DeviceDiscoveryProtocolMessage command;

  command.setLocalOnly(m_localOnly);
  command.setTargetMacAddress(targetMacAddress);
  command.setSourceMacAddress(m_ourMacAddress);
  command.setMessageId(getNextMessageId());
  command.setCommandId(DeviceDiscoveryProtocolMessage::UnlockEthernet);
  command.appendData(DeviceDiscoveryProtocolMessage::UnlockPort, (uint8_t)1);

  // Broadcast it on each network
  m_udpBroadcaster.broadcast(command.getMessage());

  return command.getMessageId();
}

/////////////////////////////////////////////////////////////////////////////

uint32_t DeviceDiscoveryProtocol::getNextMessageId() {
  return RandomNumber::randU32();
}

/////////////////////////////////////////////////////////////////////////////

bool DeviceDiscoveryProtocol::waitForReply(
    DeviceDiscoveryProtocolMessage &message, uint8_t commandId,
    uint32_t messageId, uint64_t endTime) {
  SOCKET sock = m_recvSock.socket();

  fd_set readSet;
  FD_ZERO(&readSet);
  FD_SET(sock, &readSet);

  while (1) {
    uint64_t now = OperatingSystem::getCurrentTime();
    int64_t timeout = int64_t(endTime) - int64_t(now);

    // TRACE("now: %llu, endTime: %llu, timeout: %lld", now, endTime, timeout);
    if (timeout <= 0) {
      TRACE("timeout");
      return false;
    }

    timeval tv;
    tv.tv_sec = long(timeout / 1'000'000);
    tv.tv_usec = long(timeout % 1'000'000);
    // TRACE("sec: %ld, usec: %ld", tv.tv_sec, tv.tv_usec);

    int result = select(int(sock + 1), &readSet, NULL, NULL, &tv);
    if (result == SOCKET_ERROR)
      die("select() failed");

    if (result == 0) {
      TRACE("timeout");
      return false;
    }

    if (FD_ISSET(sock, &readSet)) {
      ByteArray bytes = m_recvSock.read(1024);

      if (!message.parse(bytes, m_ourMacAddress))
        continue;

      if (message.getCommandId() != commandId ||
          message.getMessageId() != messageId)
        continue;

      // Already seen this one?
      if (std::find(m_messagesReceived.begin(), m_messagesReceived.end(),
                    bytes) != m_messagesReceived.end()) {
        TRACE("Ignoring duplicate");
        continue;
      }

      m_messagesReceived.push_back(bytes);

      uint8_t errorCode = message.getErrorCode();
      if (errorCode != DeviceDiscoveryProtocolMessage::NoError)
        TRACE("error: %u %s", errorCode,
              message.dataToString(DeviceDiscoveryProtocolMessage::ErrorMessage)
                  .c_str());

      switch (commandId) {
      case DeviceDiscoveryProtocolMessage::DeviceQuery:
        if (errorCode == DeviceDiscoveryProtocolMessage::NoError)
          return true;
        break;

      case DeviceDiscoveryProtocolMessage::ConfigureNetwork:
        return (errorCode == DeviceDiscoveryProtocolMessage::NoError);

      default:
        return true;
      }
    }
  }
}

/////////////////////////////////////////////////////////////////////////////

std::string
DeviceDiscoveryProtocol::toJson(const DeviceDiscoveryProtocolMessage &message,
                                const std::string &indent, bool listAll) {
  std::map<std::string, std::string> map;

  map["localOnly"] = message.getLocalOnly() ? "true" : "false";
  map["ipAddress"] = quote(
      message.dataToString(DeviceDiscoveryProtocolMessage::CurrentIpAddress));
  map["deviceType"] =
      message.dataToString(DeviceDiscoveryProtocolMessage::DeviceType);
  map["deviceName"] =
      quote(message.dataToString(DeviceDiscoveryProtocolMessage::DeviceName));
  map["unitName"] =
      quote(message.dataToString(DeviceDiscoveryProtocolMessage::UnitName));
  map["esn"] = quote(message.dataToString(DeviceDiscoveryProtocolMessage::ESN));
  map["version"] = quote(
      message.dataToString(DeviceDiscoveryProtocolMessage::VersionNumber));
  map["macAddress"] = quote(message.getSourceMacAddress().toString());
  map["dhcpEnabled"] =
      message.dataToString(DeviceDiscoveryProtocolMessage::DhcpEnabled);
  map["staticIpAddress"] = quote(
      message.dataToString(DeviceDiscoveryProtocolMessage::StaticIpAddress));
  map["staticNetmask"] = quote(
      message.dataToString(DeviceDiscoveryProtocolMessage::StaticNetmask));
  map["staticGateway"] = quote(
      message.dataToString(DeviceDiscoveryProtocolMessage::StaticGateway));

  if (listAll) {
    std::vector<std::string> supportedCommands;
    if (message.supportsCommand(
            DeviceDiscoveryProtocolMessage::ConfigureNetwork))
      supportedCommands.push_back("configureNetwork");
    if (message.supportsCommand(DeviceDiscoveryProtocolMessage::FlashLed))
      supportedCommands.push_back("flashLed");
    if (message.supportsCommand(DeviceDiscoveryProtocolMessage::UnlockEthernet))
      supportedCommands.push_back("unlockEthernet");
    if (message.supportsCommand(
            DeviceDiscoveryProtocolMessage::RequestPasswordResetToken))
      supportedCommands.push_back("requestPasswordResetToken");
    if (message.supportsCommand(DeviceDiscoveryProtocolMessage::ResetPassword))
      supportedCommands.push_back("resetPassword");
    if (!supportedCommands.empty()) {
      std::string list;
      for (auto const &str : supportedCommands) {
        if (!list.empty())
          list += ", ";
        list += quote(str);
      }

      map["supportedCommands"] = "[" + list + "]";
    }

    addString(map, "ethernetInterface", message,
              DeviceDiscoveryProtocolMessage::EthernetInterface);
    addString(map, "httpPorts", message,
              DeviceDiscoveryProtocolMessage::HttpPorts);
    addString(map, "httpsPorts", message,
              DeviceDiscoveryProtocolMessage::HttpsPorts);
    addInt(map, "meshId", message, DeviceDiscoveryProtocolMessage::MeshId);
    addString(map, "boardType", message,
              DeviceDiscoveryProtocolMessage::BoardType);
    addString(map, "operatingMode", message,
              DeviceDiscoveryProtocolMessage::OperatingMode);

    std::string connectionType;
    switch (message.getConnectionType()) {
    case DeviceDiscoveryProtocolMessage::Connection_Wired:
      connectionType = "wired";
      break;
    case DeviceDiscoveryProtocolMessage::Connection_WiFi:
      connectionType = "wifi";
      break;
    case DeviceDiscoveryProtocolMessage::Connection_Mesh:
      connectionType = "mesh";
      break;
    default:
      break;
    }
    if (!connectionType.empty())
      map["connectionType"] = quote(connectionType);
  }

  std::string json;
  json.reserve(16 * 1024);
  json += indent + "{\n";

  int count = 0;
  // C++17 version that has issues on old GCC used on 845
  // for (auto const &[key, val] : map) {
  //   if (count++)
  //     json += ",\n";

  //   json += indent + "    " + quote(key) + ": " + val;
  // }

  for (auto const& x : map)
  {
    if (count++)
      json += ",\n";

    json += indent + "    " + quote(x.first) + ": " + x.second;
  }
  if (count)
    json += "\n";
  json += indent + "}";

  return json;
}

std::map<std::string, std::string>
DeviceDiscoveryProtocol::toMap(const DeviceDiscoveryProtocolMessage &message,
                               bool listAll) {
  std::map<std::string, std::string> map;

  map["localOnly"] = message.getLocalOnly() ? "true" : "false";
  map["ipAddress"] = quote(
      message.dataToString(DeviceDiscoveryProtocolMessage::CurrentIpAddress));
  map["deviceType"] =
      message.dataToString(DeviceDiscoveryProtocolMessage::DeviceType);
  map["deviceName"] =
      quote(message.dataToString(DeviceDiscoveryProtocolMessage::DeviceName));
  map["unitName"] =
      quote(message.dataToString(DeviceDiscoveryProtocolMessage::UnitName));
  map["esn"] = quote(message.dataToString(DeviceDiscoveryProtocolMessage::ESN));
  map["version"] = quote(
      message.dataToString(DeviceDiscoveryProtocolMessage::VersionNumber));
  map["macAddress"] = quote(message.getSourceMacAddress().toString());
  map["dhcpEnabled"] =
      message.dataToString(DeviceDiscoveryProtocolMessage::DhcpEnabled);
  map["staticIpAddress"] = quote(
      message.dataToString(DeviceDiscoveryProtocolMessage::StaticIpAddress));
  map["staticNetmask"] = quote(
      message.dataToString(DeviceDiscoveryProtocolMessage::StaticNetmask));
  map["staticGateway"] = quote(
      message.dataToString(DeviceDiscoveryProtocolMessage::StaticGateway));

  if (listAll) {
    std::vector<std::string> supportedCommands;
    if (message.supportsCommand(
            DeviceDiscoveryProtocolMessage::ConfigureNetwork))
      supportedCommands.push_back("configureNetwork");
    if (message.supportsCommand(DeviceDiscoveryProtocolMessage::FlashLed))
      supportedCommands.push_back("flashLed");
    if (message.supportsCommand(DeviceDiscoveryProtocolMessage::UnlockEthernet))
      supportedCommands.push_back("unlockEthernet");
    if (message.supportsCommand(
            DeviceDiscoveryProtocolMessage::RequestPasswordResetToken))
      supportedCommands.push_back("requestPasswordResetToken");
    if (message.supportsCommand(DeviceDiscoveryProtocolMessage::ResetPassword))
      supportedCommands.push_back("resetPassword");
    if (!supportedCommands.empty()) {
      std::string list;
      for (auto const &str : supportedCommands) {
        if (!list.empty())
          list += ", ";
        list += quote(str);
      }

      map["supportedCommands"] = "[" + list + "]";
    }

    addString(map, "ethernetInterface", message,
              DeviceDiscoveryProtocolMessage::EthernetInterface);
    addString(map, "httpPorts", message,
              DeviceDiscoveryProtocolMessage::HttpPorts);
    addString(map, "httpsPorts", message,
              DeviceDiscoveryProtocolMessage::HttpsPorts);
    addInt(map, "meshId", message, DeviceDiscoveryProtocolMessage::MeshId);
    addString(map, "boardType", message,
              DeviceDiscoveryProtocolMessage::BoardType);
    addString(map, "operatingMode", message,
              DeviceDiscoveryProtocolMessage::OperatingMode);

    std::string connectionType;
    switch (message.getConnectionType()) {
    case DeviceDiscoveryProtocolMessage::Connection_Wired:
      connectionType = "wired";
      break;
    case DeviceDiscoveryProtocolMessage::Connection_WiFi:
      connectionType = "wifi";
      break;
    case DeviceDiscoveryProtocolMessage::Connection_Mesh:
      connectionType = "mesh";
      break;
    default:
      break;
    }
    if (!connectionType.empty())
      map["connectionType"] = quote(connectionType);
  }

  return map;
}

/////////////////////////////////////////////////////////////////////////////

void DeviceDiscoveryProtocol::addString(
    std::map<std::string, std::string> &map, const std::string &key,
    const DeviceDiscoveryProtocolMessage &message,
    DeviceDiscoveryProtocolMessage::DataType type) {
  if (message.contains(type))
    map[key] = quote(message.dataToString(type));
}

/////////////////////////////////////////////////////////////////////////////

void DeviceDiscoveryProtocol::addInt(
    std::map<std::string, std::string> &map, const std::string &key,
    const DeviceDiscoveryProtocolMessage &message,
    DeviceDiscoveryProtocolMessage::DataType type) {
  if (message.contains(type))
    map[key] = message.dataToString(type);
}

/////////////////////////////////////////////////////////////////////////////
