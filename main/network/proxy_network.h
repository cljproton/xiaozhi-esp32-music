#pragma once

#include "network_config.h"
#include "network_interface.h"

// NetworkInterface wrapper: keeps the real implementation for everything that
// cannot be tunneled, and routes TCP/HTTP/WS through the configured proxy.
class ProxyNetwork : public NetworkInterface {
public:
    static ProxyNetwork& Instance(NetworkInterface* real);

    void Init(NetworkInterface* real) { real_ = real; }
    void Refresh() { config_ = NetworkSettings::LoadProxy(); }
    bool Enabled() const { return config_.enabled && !config_.host.empty() && config_.port > 0; }
    const ProxyConfig& Config() const { return config_; }

    std::unique_ptr<Http> CreateHttp(int connect_id = -1) override;
    std::unique_ptr<Tcp> CreateTcp(int connect_id = -1) override;
    std::unique_ptr<Tcp> CreateSsl(int connect_id = -1) override;
    std::unique_ptr<Udp> CreateUdp(int connect_id = -1) override;
    std::unique_ptr<Mqtt> CreateMqtt(int connect_id = -1) override;
    std::unique_ptr<WebSocket> CreateWebSocket(int connect_id = -1) override;

private:
    explicit ProxyNetwork(NetworkInterface* real) : real_(real) {}

    NetworkInterface* real_ = nullptr;
    ProxyConfig config_ = NetworkSettings::LoadProxy();
};
