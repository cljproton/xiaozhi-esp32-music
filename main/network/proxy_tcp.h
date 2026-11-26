#pragma once

#include "network_config.h"
#include "tcp.h"

#include <atomic>
#include <memory>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class NetworkInterface;

// Raw TCP tunnel: connects to the configured proxy, performs the SOCKS5 or
// HTTP CONNECT handshake, then exposes the tunnel as a Tcp for the existing
// HttpClient / WebSocket code. Local and LAN destinations bypass the proxy.
class ProxyTcp : public Tcp {
public:
    explicit ProxyTcp(const ProxyConfig& config, NetworkInterface* delegate,
                      bool start_reader = true);
    ~ProxyTcp() override;

    NetworkResult<> Connect(const std::string& host, int port) override;
    void Disconnect() override;
    int Send(const std::string& data) override;

    // Handshake without starting a reader task. Used by ProxySsl, which takes
    // ownership of the socket and runs TLS on it.
    NetworkResult<> OpenTunnel(const std::string& host, int port) { return ConnectTunnel(host, port, false); }
    int ReleaseFd();

private:
    NetworkResult<> ConnectTunnel(const std::string& host, int port, bool start_reader);
    void ReceiveTask();

    ProxyConfig config_;
    NetworkInterface* delegate_ = nullptr;
    std::unique_ptr<Tcp> direct_;
    bool reader_ = true;
    int fd_ = -1;
    TaskHandle_t task_ = nullptr;
    std::atomic<bool> stopping_{false};
};
