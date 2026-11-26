#include "proxy_ssl.h"
#include "proxy_tcp.h"
#include "network_interface.h"

#include <esp_crt_bundle.h>
#include <esp_log.h>

#include <cstring>

#define TAG "ProxySsl"

ProxySsl::~ProxySsl() { Disconnect(); }

NetworkResult<> ProxySsl::Connect(const std::string& host, int port) {
    if (delegate_ != nullptr && NetworkSettings::IsLocalOrLanHost(host)) {
        direct_ = delegate_->CreateSsl();
        auto r = direct_->Connect(host, port);
        connected_ = direct_->connected();
        direct_->OnStream([this](const std::string& data) {
            if (stream_callback_) stream_callback_(data);
        });
        direct_->OnDisconnected([this]() {
            connected_ = false;
            if (disconnect_callback_) disconnect_callback_();
        });
        return r;
    }
    ProxyTcp tunnel(config_, nullptr, false);
    auto opened = tunnel.OpenTunnel(host, port);
    if (!opened) return opened;

    esp_tls_cfg_t cfg = {};
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 10000;
    tls_ = esp_tls_init();
    if (tls_ == nullptr) return Fail(NetworkError::TlsFailed());

    int fd = tunnel.ReleaseFd();
    esp_tls_set_conn_sockfd(tls_, fd);
    esp_tls_conn_state_t state = ESP_TLS_CONNECTING;  // skip direct tcp_connect
    esp_tls_set_conn_state(tls_, state);
    int ret = esp_tls_conn_new_sync(host.c_str(), static_cast<int>(host.size()), port, &cfg, tls_);
    if (ret != 1) {
        ESP_LOGE(TAG, "TLS handshake through proxy failed for %s", host.c_str());
        esp_tls_conn_destroy(tls_);
        tls_ = nullptr;
        return Fail(NetworkError::TlsFailed(ret));
    }
    if (direct_) return {};
    connected_ = true;
    stopping_ = false;
    if (xTaskCreate([](void* arg) { static_cast<ProxySsl*>(arg)->ReceiveTask(); },
                    "proxy_tls_rx", 6144, this, 5, &task_) != pdPASS) {
        connected_ = false;
        esp_tls_conn_destroy(tls_);
        tls_ = nullptr;
        return Fail(NetworkError::Unknown());
    }
    return {};
}

void ProxySsl::ReceiveTask() {
    std::string buffer;
    buffer.resize(4096);
    while (!stopping_ && tls_ != nullptr) {
        int n = esp_tls_conn_read(tls_, buffer.data(), buffer.size());
        if (n > 0) {
            if (stream_callback_) stream_callback_(buffer.substr(0, n));
        } else if (n == 0 || n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) {
            continue;
        } else {
            break;
        }
    }
    connected_ = false;
    if (disconnect_callback_) disconnect_callback_();
    task_ = nullptr;
    vTaskDelete(nullptr);
}

int ProxySsl::Send(const std::string& data) {
    if (direct_) return direct_->Send(data);
    if (tls_ == nullptr) return -1;
    return esp_tls_conn_write(tls_, data.data(), data.size());
}

void ProxySsl::Disconnect() {
    stopping_ = true;
    if (direct_) {
        direct_->Disconnect();
        direct_.reset();
    }
    if (tls_ != nullptr) {
        esp_tls_conn_destroy(tls_);
        tls_ = nullptr;
    }
    connected_ = false;
}
