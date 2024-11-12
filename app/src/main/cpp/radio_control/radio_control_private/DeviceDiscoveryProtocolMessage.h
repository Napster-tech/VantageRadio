#pragma once

#include <map>
#include "../RadioControl.h"
#include "MacAddress.h"
#include "HostAddress.h"

class DeviceDiscoveryProtocolMessage
{
public:
    enum { Magic = 0xA1D030DD };

    enum CommandId {
        DeviceQuery = 0x00,
        ConfigureNetwork = 0x01,
        FlashLed = 0x03,
        UnlockEthernet = 0x04,
        RequestPasswordResetToken = 0x05,
        ResetPassword = 0x06
    };

    enum DataType {
        ErrorCode = 0x00,
        ErrorMessage = 0x01,
        DeviceType = 0x02,
        DeviceName = 0x03,
        ESN = 0x04,
        VersionNumber = 0x05,
        UnitName = 0x06,
        CurrentIpAddress = 0x07,
        DhcpEnabled = 0x08,
        StaticIpAddress = 0x09,
        StaticNetmask = 0x0A,
        StaticGateway = 0x0B,
        HttpPorts = 0x0C,
        HttpsPorts = 0x0D,
        SupportedCommands = 0x0E,
        EthernetInterface = 0x0F,
        MeshId = 0x10,
        LedEnable = 0x11,
        UnlockPort = 0x12,
        PasswordResetToken = 0x13,
        PasswordResetCode = 0x14,
        BoardType = 0x15,
        OperatingMode = 0x16
    };

    enum ErrorCode {
        NoError = 0,
        UnknownCommand = 1,
        IllegalParameter = 2
    };

    enum DeviceType {
        Generic = 0x00,
        Mesh = 0x01,
        Duo = 0x02,
        Solo4Rx = 0x03,
        CRx = 0x04,
        ProRx = 0x05,
        NanoVue = 0x06,
        IpEncoder = 0x07,
        AntennaSwitchUnit = 0x08,
        NanoVueHd = 0x09,
        ProRx2 = 0x0A,
        Eastwood = 0x0B,
        NetWorker = 0x0C,
        AntennaSwitchUnit2 = 0x0D,
        Encipher = 0x0E,
        Solo7HdIfRx = 0x0F,
        Decipher = 0x10,
        Eclipse = 0x11
    };

    enum ConnectionType {
        Connection_Invalid,
        Connection_Wired,
        Connection_WiFi,
        Connection_Mesh
    };

    DeviceDiscoveryProtocolMessage();

    void clear();

    void setLocalOnly(bool localOnly);
    void setBroadcastTargetMacAddress();
    void setTargetMacAddress(const MacAddress& address);
    void setSourceMacAddress(const MacAddress& address);
    void setMessageId(uint32_t id);
    void setCommandId(uint8_t id);
    void appendData(uint8_t type, bool value);
    void appendData(uint8_t type, const HostAddress& address);
    void appendData(uint8_t type, uint8_t byte);
    void appendData(uint8_t type, const std::string& str);
    void appendData(uint8_t type, const char *str);

    ByteArray getMessage() const;

    bool parse(const ByteArray& message, const MacAddress& ourMacAddress);

    bool getLocalOnly() const;
    MacAddress getTargetMacAddress() const;
    MacAddress getSourceMacAddress() const;
    uint32_t getMessageId() const;
    uint8_t getCommandId() const;

    uint8_t getErrorCode() const;

    bool contains(uint8_t type) const;

    any::any data(uint8_t type) const;
    std::string dataToString(uint8_t type) const;

    bool supportsCommand(uint8_t id) const;

    ConnectionType getConnectionType() const;

    static std::string typeToString(uint8_t type);
    static std::string dataToString(uint8_t type, const any::any& data);

private:
    bool isConnectionMesh(const std::string &iface) const;

    void appendU16(ByteArray& array, uint16_t value) const;
    void appendU32(ByteArray& array, uint32_t value) const;
    uint16_t readU16(const ByteArray& array, int offset) const;
    uint32_t readU32(const ByteArray& array, int offset) const;

    bool m_localOnly;
    MacAddress m_sourceMacAddress;
    MacAddress m_targetMacAddress;
    uint32_t m_messageId;
    uint8_t m_commandId;
    ByteArray m_data;

    std::map<uint8_t, any::any> m_reply;
};
