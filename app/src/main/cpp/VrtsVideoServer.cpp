#include "VrtsVideoServer.h"
#include <arpa/inet.h>
#include <iostream>
#include <string.h>
#include <unistd.h>




VrtsVideoServer::VrtsVideoServer(const std::string& upstream_ip, int upstream_port)
        : _dnstream_ip("192.168.20.30")
        , _upstream_ip(upstream_ip)
        , _dnstream_port(11000)
        , _upstream_port(upstream_port)
        , _keep_running(false)
        , _forward_port(0)
        , _tx_socket(-1)
        , _logging_enabled(false)
{
}

VrtsVideoServer::~VrtsVideoServer()
{
    shutdown();
    if (_log_file.is_open()) {
        _log_file.close();
    }
}

std::string VrtsVideoServer::getVRTSLogs() {
    std::lock_guard<std::mutex> lock(_log_mutex);

    std::ostringstream logs;  // Declare logs as a local variable here
    for (const auto& message : _vrts_log_buffer) {
        logs << message << "\n";
    }
    return logs.str();  // Return the collected log messages as a single string
}




bool VrtsVideoServer::setDownstreamUrl(const std::string& url)
{
    auto pos = url.find("//");
    if (pos != std::string::npos) {
        std::string scheme = url.substr(0, pos - 1);
        if (scheme == "vrts") {
            std::string tmp = url.substr(pos + 2);
            pos = tmp.find(":");
            if (pos != std::string::npos) {
                _dnstream_ip = tmp.substr(0, pos);
                _dnstream_port = std::stoi(tmp.substr(pos + 1));

                if (_keep_running) {
                    shutdown();
                    startup(_forward_port);
                }
                return true;
            }
        }
    }
    return false;
}

bool VrtsVideoServer::startup(int fwdport)
{
    uint16_t forward_port = static_cast<uint16_t>(fwdport);

    if (_tx_socket >= 0 && _keep_running) {
        if (htons(forward_port) != _forward_addr.sin_port) {
            vrc_log("Switch Forwarding port from " + std::to_string(htons(_forward_addr.sin_port)) +
                    " to " + std::to_string(fwdport));
            std::lock_guard<std::mutex> lock(_forward_mutex);
            _forward_addr.sin_port = htons(forward_port);
            _packets_received = 0;
            _forward_errors = 0;
        }
        return true;
    }

    if (!_keep_running) {
        _connections = 0;
        _disconnects = 0;
        _packets_received = 0;
        _forward_errors = 0;

        if (udpSend(nullptr, 0, forward_port) < 0) {
            shutdown();
            return false;
        }

        run_thread = std::thread([this]() { run(); });
    }

    vrc_log("VRTS video server started. Forwarding to port " + std::to_string(fwdport));
    return true;
}

void VrtsVideoServer::shutdown()
{
    if (_vrts) {
        _keep_running = false;
        if (run_thread.joinable()) {
            run_thread.join();
        }
        _vrts.reset();
        _connections = 0;
        _disconnects = 0;
        _packets_received = 0;
        _forward_errors = 0;
        vrc_log("VRTS video server shutdown.");
    }
}

void VrtsVideoServer::enableLogging(const std::string& path)
{
    std::lock_guard<std::mutex> lock(_log_mutex);
    _log_file.open(path, std::ios::out | std::ios::app);
    if (_log_file.is_open()) {
        _logging_enabled = true;
        _log_file << "Logging started.\n";
        std::cout << "Logging enabled at: " << path << std::endl;
    } else {
        std::cerr << "Failed to open log file: " << path << std::endl;
    }
}

void VrtsVideoServer::vrc_log(const std::string& message) {
    std::lock_guard<std::mutex> lock(_log_mutex);

    // Write to file if logging is enabled
    if (_logging_enabled && _log_file.is_open()) {
        _log_file << message << std::endl;
    }

    // Store in the VRTS log buffer
    _vrts_log_buffer.push_back(message);
    if (_vrts_log_buffer.size() > _vrts_log_buffer_size) {
        _vrts_log_buffer.erase(_vrts_log_buffer.begin());  // Remove the oldest log if buffer is full
    }

    // Also output to console
    std::cout << message << std::endl;
}


void VrtsVideoServer::run()
{
    _keep_running = true;
    _connections = 0;
    _vrts = std::make_shared<vrts::VRTS>(_dnstream_port, _upstream_port, _dnstream_ip, _upstream_ip, 100);
    vrc_log("VRTS up stream IP: " + _upstream_ip + " port: " + std::to_string(_upstream_port));
    vrc_log("VRTS dn stream IP: " + _dnstream_ip + " port: " + std::to_string(_dnstream_port));

    int time_nodata_received = 0;
    while (_keep_running) {
        if (_vrts->dataReady()) {
            const auto nal_in = _vrts->getDataAsMPEGTS();
            _packets_received++;
            if (_connections == 0) {
                _connections = 1;
                _disconnects = 0;
                vrc_log("VRTS client connected");
            }
            time_nodata_received = 0;

            udpSend(const_cast<uint8_t*>(nal_in.data()),
                    static_cast<uint16_t>(nal_in.size()), _forward_port);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            time_nodata_received += 2;
            if (_connections && time_nodata_received > 1000) {
                _disconnects += 1;
                _connections = 0;
                vrc_log("VRTS client disconnected");
            }
        }
    }

    if (_tx_socket >= 0) {
        close(_tx_socket);
        _tx_socket = -1;
    }
    _forward_port = 0;
}

int VrtsVideoServer::udpRecv(uint8_t* data, uint16_t len, uint16_t port)
{
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        vrc_log("VRTS cannot open receive socket");
        return -1;
    }

    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 500000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);

    struct sockaddr_in broadcast_rx_addr;
    memset(&broadcast_rx_addr, 0, sizeof(broadcast_rx_addr));
    broadcast_rx_addr.sin_family = AF_INET;
    broadcast_rx_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    broadcast_rx_addr.sin_port = htons(port);

    bind(fd, reinterpret_cast<sockaddr*>(&broadcast_rx_addr), sizeof(broadcast_rx_addr));
    struct sockaddr_in from;
    socklen_t addrlen = sizeof(struct sockaddr_in);
    ssize_t received = recvfrom(fd, data, static_cast<size_t>(len), 0, reinterpret_cast<sockaddr*>(&from), &addrlen);
    close(fd);
    return static_cast<int>(received);
}

int VrtsVideoServer::udpSend(uint8_t* data, uint16_t len, uint16_t port)
{
    ssize_t result = 0;

    if (_tx_socket < 0) {
        _tx_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (_tx_socket < 0) {
            vrc_log("VRTS server failed to open forwarding socket");
            return -1;
        }

        const int enable = 1;
        setsockopt(_tx_socket, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
        setsockopt(_tx_socket, SOL_SOCKET, SO_REUSEPORT, &enable, sizeof(enable));

        std::lock_guard<std::mutex> lock(_forward_mutex);
        memset(&_forward_addr, 0, sizeof(_forward_addr));
        _forward_addr.sin_family = AF_INET;
        _forward_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        _forward_addr.sin_port = htons(port);
        _forward_port = port;
    }

    if (data != nullptr && len > 0) {
        result = sendto(_tx_socket, data, len, 0, reinterpret_cast<sockaddr*>(&_forward_addr), sizeof(_forward_addr));
        if (result < 0) {
            _forward_errors++;
            vrc_log("Error: UDP send failed");
        }
    }
    return static_cast<int>(result);
}
