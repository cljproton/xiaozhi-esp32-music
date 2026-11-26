#pragma once

#include "network_config.h"
#include "tcp.h"

#include <atomic>
#include <memory>
#include <esp_tls.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// TLS over an already established proxy tunnel. esp_tls can handshake on a
// socket we hand it via esp_tls_set_conn_sockfd(), so no vendored code changes.
class NetworkInterface;

class ProxySsl : public Tcp {
public:
    explicit ProxySsl(const ProxyConfig& config, NetworkInterface* delegate)
        : config_(config), delegate_(delegate) {}
    ~ProxySsl() override;

    NetworkResult<> Connect(const std::string& host, int port) override;
    void Disconnect() override;
    int Send(const std::string& data) override;

private:
    void ReceiveTask();

    ProxyConfig config_;
    NetworkInterface* delegate_ = nullptr;
    std::unique_ptr<Tcp> direct_;
    esp_tls_t* tls_ = nullptr;
    TaskHandle_t task_ = nullptr;
    std::atomic<bool> stopping_{false};
};
