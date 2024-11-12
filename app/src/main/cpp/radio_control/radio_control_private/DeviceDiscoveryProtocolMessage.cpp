#include <cassert>
#include <cstring>
#include "OperatingSystem.h"
#include "DeviceDiscoveryProtocolMessage.h"
#include "Crc32.h"
#include "Encryption.h"
#include "Network.h"
#include "StrUtils.h"
#include "Trace.h"

/////////////////////////////////////////////////////////////////////////////

static const uint8_t BlowfishKey[56] = {
    0x28, 0x30, 0x54, 0x75, 0x6a, 0x52, 0xe9, 0x64,
    0x2d, 0xac, 0x38, 0xb6, 0xe3, 0xd8, 0x12, 0x3c,
    0x3f, 0x6b, 0xff, 0xa6, 0x87, 0xfd, 0x02, 0x09,
    0xe1, 0xed, 0xc5, 0x64, 0xca, 0x7d, 0xbd, 0xb7,
    0x0b, 0x58, 0xfd, 0x25, 0x67, 0x72, 0xa3, 0x0b,
    0x45, 0x8e, 0xfc, 0x88, 0x67, 0xd3, 0x7e, 0x27,
    0xcb, 0x91, 0xfc, 0x5b, 0xcc, 0xd0, 0x04, 0xe8
};

/////////////////////////////////////////////////////////////////////////////

DeviceDiscoveryProtocolMessage::DeviceDiscoveryProtocolMessage()
{
    clear();
}

/////////////////////////////////////////////////////////////////////////////

void DeviceDiscoveryProtocolMessage::clear()
{
    m_localOnly = false;
    m_sourceMacAddress.clear();
    m_targetMacAddress.clear();
    m_messageId = 0;
    m_commandId = 0;
    m_data.clear();
    m_reply.clear();
}

/////////////////////////////////////////////////////////////////////////////

void DeviceDiscoveryProtocolMessage::setLocalOnly(bool localOnly)
{
    m_localOnly = localOnly;
}

/////////////////////////////////////////////////////////////////////////////

void DeviceDiscoveryProtocolMessage::setBroadcastTargetMacAddress()
{
    m_targetMacAddress = MacAddress(ByteArray(MacAddress::NumBytes, 0xFF));
}

/////////////////////////////////////////////////////////////////////////////

void DeviceDiscoveryProtocolMessage::setTargetMacAddress(const MacAddress& address)
{
    m_targetMacAddress = address;
}

/////////////////////////////////////////////////////////////////////////////

void DeviceDiscoveryProtocolMessage::setSourceMacAddress(const MacAddress& address)
{
    m_sourceMacAddress = address;
}

/////////////////////////////////////////////////////////////////////////////

void DeviceDiscoveryProtocolMessage::setMessageId(uint32_t id)
{
    m_messageId = id;
}

/////////////////////////////////////////////////////////////////////////////

void DeviceDiscoveryProtocolMessage::setCommandId(uint8_t id)
{
    m_commandId = id;
}

/////////////////////////////////////////////////////////////////////////////

void DeviceDiscoveryProtocolMessage::appendData(uint8_t type, bool value)
{
    m_data.append(type);
    appendU16(m_data, 1);
    m_data.append(value);
}

/////////////////////////////////////////////////////////////////////////////

void DeviceDiscoveryProtocolMessage::appendData(uint8_t type, const HostAddress& address)
{
    m_data.append(type);
    appendU16(m_data, 4);
    appendU32(m_data, address.data());
}

/////////////////////////////////////////////////////////////////////////////

void DeviceDiscoveryProtocolMessage::appendData(uint8_t type, uint8_t byte)
{
    m_data.append(type);
    appendU16(m_data, 1);
    m_data.append(byte);
}

/////////////////////////////////////////////////////////////////////////////

void DeviceDiscoveryProtocolMessage::appendData(uint8_t type, const std::string& str)
{
    m_data.append(type);
    appendU16(m_data, uint16_t(str.size() + 1));
    for (char c : str)
        m_data.append(c);
    m_data.append(static_cast<char>(0x00));
}

/////////////////////////////////////////////////////////////////////////////

void DeviceDiscoveryProtocolMessage::appendData(uint8_t type, const char* str)
{
    if (!str)
        return;

    m_data.append(type);
    appendU16(m_data, uint16_t(strlen(str) + 1));
    while (*str)
        m_data.append(*str++);
    m_data.append(static_cast<char>(0x00));
}

/////////////////////////////////////////////////////////////////////////////

ByteArray DeviceDiscoveryProtocolMessage::getMessage() const
{
    if (!m_sourceMacAddress.isValid()) {
        TRACE("Source MAC address not valid");
        return ByteArray();
    }

    if (!m_targetMacAddress.isValid()) {
        TRACE("Target MAC address not valid");
        return ByteArray();
    }

    TRACE("sourceMacAddress: %s", m_sourceMacAddress.toString().c_str());
    TRACE("targetMacAddress: %s", m_targetMacAddress.toString().c_str());

    ByteArray message;

    uint16_t flags = 0; // Request
    if (m_localOnly)
        flags |= 0x02;

    // Header
    appendU32(message, Magic);
    appendU16(message, 0x0001); // version
    appendU16(message, flags);
    message.append(m_targetMacAddress.data());
    message.append(m_sourceMacAddress.data());

    // TRACE("HDR: %s", message.toHexString(' ').c_str());

    // Body
    ByteArray body;
    appendU16(body, uint16_t(m_data.size() + 5)); // Length
    appendU32(body, m_messageId);
    body.append(m_commandId);
    body.append(m_data);

    // CRC32
    Crc32 crc32;
    crc32.append(message);
    crc32.append(body);
    appendU32(body, crc32.getCrc());

    // TRACE("BDY: %s", body.toHexString(' ').c_str());

    // Blowfish encrypt the body (adds padding)
    ByteArray key(BlowfishKey, sizeof(BlowfishKey));
    if (!blowfishEncrypt(key, &body))
        return ByteArray();

    // Append the encrypted body to the message
    message.append(body);

    // TRACE("MSG: %s", message.toHexString(' ').c_str());

    return message;
}

/////////////////////////////////////////////////////////////////////////////

bool DeviceDiscoveryProtocolMessage::parse(const ByteArray& message, const MacAddress& ourMacAddress)
{
    m_reply.clear();

    // Minimum size: header (4 + 2 + 2 + 6 + 6) + body (2 + 4 + 1 + 4) + padding (5) == 36
    if (message.size() < 36) {
        TRACE("Too short");
        return false;
    }

    // Header
    if (readU32(message, 0) != Magic)  { TRACE("Magic");   return false; }
    if (readU16(message, 4) != 0x0001) { TRACE("Version"); return false; }

    uint16_t flags = readU16(message, 6);
    if ((flags & 0x0001) != 0x0001) { TRACE("Flags"); return false; }
    m_localOnly = !!(flags & 0x0002);

    m_targetMacAddress.fromByteArray(message.mid(8, 6));
    if (m_targetMacAddress != ourMacAddress) { TRACE("Dest MAC"); return false; }

    m_sourceMacAddress.fromByteArray(message.mid(14, 6));

    // Body
    ByteArray body = message.mid(20);

    // Decrypt
    ByteArray key(BlowfishKey, sizeof(BlowfishKey));
    if (!blowfishDecrypt(key, &body, body.size())) {
        TRACE("Decrypt");
        return false;
    } else {
        TRACE("Dencrypt OK");
    }

    // Remove padding
    uint16_t length = readU16(body, 0);
    if (length > 1500) {
        TRACE("Length larger than a packet");
        return false;
    }
    length += 6; // length (2 bytes) + crc32 (4 bytes)
    if (length > body.size()) {
        TRACE("Length large than data: %d %d", length, body.size());
        return false;
    }
    body.truncate(length);

    // Extract the CRC and truncate
    uint32_t crc = readU32(body, int(body.size()) - 4);
    body.truncate(body.size() - 4);

    // Check the CRC
    Crc32 crc32;
    crc32.append(message.left(20)); // header
    crc32.append(body);
    if (crc32.getCrc() != crc) {
        TRACE("CRC");
        return false;
    }

    // TRACE("%s", body.toHex().constData());

    // Extact the data
    m_messageId = readU32(body, 2);
    m_commandId = body[6];
    m_data = body.mid(7);

    TRACE("MessageId: 0x%08X, CommandId: 0x%02X, Data size: %d", m_messageId, m_commandId, m_data.size());

    for (int n = 0; n < int(m_data.size() - 3);) {
        uint8_t  type = m_data[n++];
        uint16_t length = readU16(m_data, n);
        n += 2;
        ByteArray data = m_data.mid(n, length);
        n += length;

        //TRACE("0x%02X %d '%s'", type, length, data.toHexString().c_str());

        if (length == 1) { // Single byte replies
            m_reply[type] = (uint8_t)data[0];
        }
        else {
            switch (type) {
            case ErrorMessage:
                TRACE("%s", data.constData());
                //FALLTHROUGH();
            case DeviceName:
            case ESN:
            case VersionNumber:
            case UnitName:
            case HttpPorts:
            case HttpsPorts:
            case EthernetInterface:
            case PasswordResetToken:
            case PasswordResetCode:
            case BoardType:
            case OperatingMode:
                {
                    std::string str;
                    if (data.isEmpty()) {
                        TRACE("Warning: Data string for type %d is empty", type);
                    }
                    else if (data.at(data.size() - 1) != 0) {
                        TRACE("Warning: Data string for type %d is not null terminated", type);
                        str = std::string((const char *)data.constData(), data.size());
                    }
                    else {
                        str = std::string((const char *)data.constData(), data.size() - 1);
                    }
                    m_reply[type] = any::any(str);
                }
                break;

            case CurrentIpAddress:
            case StaticIpAddress:
            case StaticNetmask:
            case StaticGateway:
                m_reply[type] = readU32(data, 0);
                break;

            case MeshId:
                m_reply[type] = readU16(data, 0);
                break;

            default:
                m_reply[type] = any::any(data);
                break;
            }
        }

        //TRACE("%s: %s", qPrintable(typeToString(type)), qPrintable(dataToString(type)));
    }

    return true;
}

/////////////////////////////////////////////////////////////////////////////

bool DeviceDiscoveryProtocolMessage::getLocalOnly() const
{
    return m_localOnly;
}

/////////////////////////////////////////////////////////////////////////////

MacAddress DeviceDiscoveryProtocolMessage::getTargetMacAddress() const
{
    return m_targetMacAddress;
}

/////////////////////////////////////////////////////////////////////////////

MacAddress DeviceDiscoveryProtocolMessage::getSourceMacAddress() const
{
    return m_sourceMacAddress;
}

/////////////////////////////////////////////////////////////////////////////

uint32_t DeviceDiscoveryProtocolMessage::getMessageId() const
{
    return m_messageId;
}

/////////////////////////////////////////////////////////////////////////////

uint8_t DeviceDiscoveryProtocolMessage::getCommandId() const
{
    return m_commandId;
}

/////////////////////////////////////////////////////////////////////////////

uint8_t DeviceDiscoveryProtocolMessage::getErrorCode() const
{
    try {
        return any::any_cast<uint8_t>(data(ErrorCode));
    }
    catch (std::exception& e) {
        TRACE("caught: %s", e.what());
        // Don't assert.
        //assert(false);
        return 0;
    }
} 

/////////////////////////////////////////////////////////////////////////////

bool DeviceDiscoveryProtocolMessage::contains(uint8_t type) const
{
    return (m_reply.count(type) > 0);
}

/////////////////////////////////////////////////////////////////////////////

any::any DeviceDiscoveryProtocolMessage::data(uint8_t type) const
{
    try {
        if (m_reply.count(type) > 0)
            return m_reply.at(type);
    }
    catch (std::exception& e) {
        TRACE("caught: %s", e.what());
        // don't assert
        //assert(false);
    }
    return any::any();
}

/////////////////////////////////////////////////////////////////////////////

std::string DeviceDiscoveryProtocolMessage::dataToString(uint8_t type) const
{
    try {
        if (m_reply.count(type) > 0)
            return dataToString(type, m_reply.at(type));
    }
    catch (std::exception& e) {
        TRACE("caught: %s", e.what());
        // don't assert
        //assert(false);
    }
    return std::string();
}

/////////////////////////////////////////////////////////////////////////////

bool DeviceDiscoveryProtocolMessage::supportsCommand(uint8_t id) const
{
    try {
        ByteArray bytes = any::any_cast<ByteArray>(data(SupportedCommands));

        size_t numBytes = bytes.size();
        size_t numBits = numBytes * 8;

        if (id >= numBits)
            return false;

        auto byteIdx = numBytes - (id / 8) - 1;

        auto byte = bytes.at(byteIdx);

        return !!(byte & (1 << (id % 8)));
    }
    catch (std::exception& e) {
        TRACE("caught: %s", e.what());
        // don't assert
        //assert(false);
        return false;
    }
}

/////////////////////////////////////////////////////////////////////////////

DeviceDiscoveryProtocolMessage::ConnectionType DeviceDiscoveryProtocolMessage::getConnectionType() const
{
    std::string iface = dataToString(DeviceDiscoveryProtocolMessage::EthernetInterface);
    iface = lower(trim(iface));

    if (startsWith(iface, "eth"))  return DeviceDiscoveryProtocolMessage::Connection_Wired;
    if (startsWith(iface, "wlan")) return DeviceDiscoveryProtocolMessage::Connection_WiFi;
    if (isConnectionMesh(iface))   return DeviceDiscoveryProtocolMessage::Connection_Mesh;

    int deviceType = -1;
    try {
        deviceType = any::any_cast<uint8_t>(m_reply.at(DeviceDiscoveryProtocolMessage::DeviceType));
    }
    catch (std::exception& e) {
        TRACE("caught: %s", e.what());
        // don't assert
        //assert(false);
    }

    switch (deviceType) {
    case DeviceDiscoveryProtocolMessage::Eastwood:
        //
        // If board type is defined, it's v5.3.0 and above
        //
        if (m_reply.count(DeviceDiscoveryProtocolMessage::BoardType) > 0) {
            //
            // Empty iface can occur when the node IP address is invalid
            // Report invalid (unknown)
            //
            if (iface.empty())
                return Connection_Invalid;

            // Assume it's wired since mesh is handled above
            //
            return Connection_Wired;
        }

        // Pre v5.3.0, so we don't have enough information
        // Report invalid (unknown)
        //
        return Connection_Invalid;

    case DeviceDiscoveryProtocolMessage::Mesh:
    case DeviceDiscoveryProtocolMessage::Duo:
        //
        // Old Mesh/Duo devices which haven't given us enough information
        // Report invalid (unknown)
        //
        return Connection_Invalid;

    default:
        //
        // Anything else, including an empty iface, assume it's wired
        //
        return Connection_Wired;
    }
}

/////////////////////////////////////////////////////////////////////////////

std::string DeviceDiscoveryProtocolMessage::typeToString(uint8_t type)
{
    switch (type) {
    case ErrorCode:          return "Error Code";
    case ErrorMessage:       return "Error Message";
    case DeviceType:         return "Device Type";
    case DeviceName:         return "Device Name";
    case ESN:                return "ESN";
    case VersionNumber:      return "Version Number";
    case UnitName:           return "Unit Name";
    case CurrentIpAddress:   return "Current IP Address";
    case DhcpEnabled:        return "DHCP Enabled";
    case StaticIpAddress:    return "Static IP Address";
    case StaticNetmask:      return "Static Netmask";
    case StaticGateway:      return "Static Gateway";
    case HttpPorts:          return "HTTP Ports";
    case HttpsPorts:         return "HTTPS Ports";
    case SupportedCommands:  return "Supported Commands";
    case EthernetInterface:  return "Ethernet Interface";
    case MeshId:             return "Mesh ID";
    case LedEnable:          return "LED Enable";
    case UnlockPort:         return "Unlock Port";
    case PasswordResetToken: return "Password Reset Token";
    case PasswordResetCode:  return "Password Reset Code";
    case BoardType:          return "Board Type";
    case OperatingMode:      return "Operating Mode";
    default:                 return "Unknown Type";
    }
}

/////////////////////////////////////////////////////////////////////////////

std::string DeviceDiscoveryProtocolMessage::dataToString(uint8_t type, const any::any& data)
{
    try {
        // TRACE("%d %s", type, data.type().name());

        // if (!data.has_value())
        //     return std::string();
        if (data.type() == typeid(const char*)) {
            return std::string();
        }

        switch (type) {
        case CurrentIpAddress:
        case StaticIpAddress:
        case StaticNetmask:
        case StaticGateway:
            return HostAddress(any::any_cast<uint32_t>(data)).toString();

        case DhcpEnabled:
            return any::any_cast<uint8_t>(data) ? "true" : "false";

        default:
            break;
        }

        if (data.type() == typeid(std::string)) return any::any_cast<std::string>(data);
        if (data.type() == typeid(ByteArray))   return any::any_cast<ByteArray>(data).toHexString();
        if (data.type() == typeid(uint8_t))     return asprintf("%u", any::any_cast<uint8_t>(data));
        if (data.type() == typeid(uint16_t))    return asprintf("%u", any::any_cast<uint16_t>(data));
        if (data.type() == typeid(uint32_t))    return asprintf("%u", any::any_cast<uint32_t>(data));
    }
    catch (std::exception& e) {
        TRACE("caught: %s", e.what());
        // don't assert
        // assert(false);
    }

    return std::string();
}

/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////

bool DeviceDiscoveryProtocolMessage::isConnectionMesh(const std::string &iface) const
{
    if (!contains(MeshId))
        return false;

    if (iface.size() < 2)
        return false;

    if (iface.at(0) != 'm')
        return false;

    size_t n;
    for (n = 1; n < iface.size(); n++) {
        if (isdigit(iface.at(n)))
            break;
    }

    if (n >= iface.size())
        return false;

    const char *pStr = iface.c_str();
    char *pEnd;
    long nodeId = strtol(pStr + n, &pEnd, 10);
    TRACE("%s -> %ld (%s)", iface.c_str(), nodeId, (pEnd == pStr + iface.size()) ? "ok" : "error");
    if (pEnd != pStr + iface.size())
        return false;

    switch (n) {
    case 1:  return true; // mXXX
    case 2:  return (iface.at(1) == 's'); // msXX
    case 3:  return (iface.at(1) == 's' && iface.at(2) == 'h'); // mshX
    default: return false;
    }
}

/////////////////////////////////////////////////////////////////////////////

void DeviceDiscoveryProtocolMessage::appendU16(ByteArray& array, uint16_t value) const
{
    array.append((uint8_t)(0xFF & (value >> 8)));
    array.append((uint8_t)(0xFF & (value)));
}

/////////////////////////////////////////////////////////////////////////////

void DeviceDiscoveryProtocolMessage::appendU32(ByteArray& array, uint32_t value) const
{
    array.append((uint8_t)(0xFF & (value >> 24)));
    array.append((uint8_t)(0xFF & (value >> 16)));
    array.append((uint8_t)(0xFF & (value >> 8)));
    array.append((uint8_t)(0xFF & (value)));
}

/////////////////////////////////////////////////////////////////////////////

uint16_t DeviceDiscoveryProtocolMessage::readU16(const ByteArray& array, int offset) const
{
    uint16_t u;

    u =  ((uint16_t)array[offset++] << 8) & 0xFF00;
    u |= ((uint16_t)array[offset++])      & 0x00FF;

    return u;
}

/////////////////////////////////////////////////////////////////////////////

uint32_t DeviceDiscoveryProtocolMessage::readU32(const ByteArray& array, int offset) const
{
    uint32_t u;

    u =  ((uint32_t)array[offset++] << 24) & 0xFF000000;
    u |= ((uint32_t)array[offset++] << 16) & 0x00FF0000;
    u |= ((uint32_t)array[offset++] << 8)  & 0x0000FF00;
    u |= ((uint32_t)array[offset++])       & 0x000000FF;

    return u;
}

/////////////////////////////////////////////////////////////////////////////
