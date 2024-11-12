#ifndef VRTS_VIDEO_SERVER_H
#define VRTS_VIDEO_SERVER_H

#include <arpa/inet.h>
#include <sys/socket.h>
#include <atomic>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "VRTS.h"
#include <sstream>

using VRTSSp = std::shared_ptr<vrts::VRTS>;

class VrtsVideoServer
{private:
    std::ostringstream logs;
    std::vector<std::string> _vrts_log_buffer;  // Buffer to store recent VRTS log entries
    const size_t _vrts_log_buffer_size = 1000;  // Maximum size for the VRTS log buffer



public:
    std::string getVRTSLogs();

    // Add this method declaration in the public section

    VrtsVideoServer(void) = delete;
    VrtsVideoServer(const std::string& upstream_addr, int upstream_port = 30000);
    ~VrtsVideoServer(void);

    bool setDownstreamUrl(const std::string& url);

    bool startup(int fwdport);
    void shutdown(void);

    bool running() const { return (_keep_running && _tx_socket > 0); }
    int packetsReceived() const { return _packets_received; }
    int forwardingErrors() const { return _forward_errors; }

    void enableLogging(const std::string& path = "");

protected:
    struct StatData
    {
        int connections;
        int disconnects;
        int packets_received;
        int forward_errors;
    };

    void collectStatistics(StatData& statData);
    void run(void);
    int udpRecv(uint8_t *data, uint16_t len, uint16_t port);
    int udpSend(uint8_t *data, uint16_t len, uint16_t port);
    void vrc_log(const std::string& message);  // New method for unified logging

protected:
    VRTSSp _vrts;
    std::string _dnstream_ip;
    std::string _upstream_ip;
    int _dnstream_port;
    int _upstream_port;

    std::atomic<bool> _keep_running;
    std::thread run_thread;
    std::mutex _forward_mutex;
    struct sockaddr_in _forward_addr;
    uint16_t _forward_port;
    int _tx_socket = -1;

    int _connections = 0;
    int _disconnects = 0;
    int _packets_received = 0;
    int _forward_errors = 0;

    // Logging variables
    std::ofstream _log_file;                 // Log file stream
    std::mutex _log_mutex;                   // Mutex for thread-safe logging
    bool _logging_enabled = false;           // Flag to check if logging is enabled
};

#endif // VRTS_VIDEO_SERVER_H
