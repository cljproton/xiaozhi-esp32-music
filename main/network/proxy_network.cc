#include "proxy_network.h"
#include "http_client.h"
#include "web_socket.h"
#include "proxy_tcp.h"
#include "proxy_ssl.h"

ProxyNetwork& ProxyNetwork::Instance(NetworkInterface* real) {
    static ProxyNetwork instance(real);
    if (instance.real_ == nullptr) instance.real_ = real;
    return instance;
}

std::unique_ptr<Http> ProxyNetwork::CreateHttp(int connect_id) {
    Refresh();
    return std::make_unique<HttpClient>(this, connect_id);
}

std::unique_ptr<Tcp> ProxyNetwork::CreateTcp(int connect_id) {
    Refresh();
    if (!Enabled()) return real_->CreateTcp(connect_id);
    return std::make_unique<ProxyTcp>(config_, real_);
}

std::unique_ptr<Tcp> ProxyNetwork::CreateSsl(int connect_id) {
    Refresh();
    if (!Enabled()) return real_->CreateSsl(connect_id);
    return std::make_unique<ProxySsl>(config_, real_);
}

std::unique_ptr<Udp> ProxyNetwork::CreateUdp(int connect_id) {
    return real_->CreateUdp(connect_id);
}

std::unique_ptr<Mqtt> ProxyNetwork::CreateMqtt(int connect_id) {
    return real_->CreateMqtt(connect_id);
}

std::unique_ptr<WebSocket> ProxyNetwork::CreateWebSocket(int connect_id) {
    Refresh();
    return std::make_unique<WebSocket>(this, connect_id);
}
