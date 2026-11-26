#include "proxy_tcp.h"
#include "network_interface.h"

#include <esp_log.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <mbedtls/base64.h>

#include <cerrno>
#include <cstring>
#include <string>

#define TAG "ProxyTcp"

namespace {

bool RecvExact(int fd, void* data, size_t len) {
    auto* p = static_cast<uint8_t*>(data);
    size_t got = 0;
    while (got < len) {
        int n = recv(fd, p + got, len - got, 0);
        if (n <= 0) return false;
        got += n;
    }
    return true;
}

bool SendAll(int fd, const void* data, size_t len) {
    auto* p = static_cast<const uint8_t*>(data);
    size_t sent = 0;
    while (sent < len) {
        int n = send(fd, p + sent, len - sent, 0);
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}

}  // namespace

ProxyTcp::ProxyTcp(const ProxyConfig& config, NetworkInterface* delegate, bool start_reader)
    : config_(config), delegate_(delegate), reader_(start_reader) {}

ProxyTcp::~ProxyTcp() { Disconnect(); }

int ProxyTcp::ReleaseFd() {
    int fd = fd_;
    fd_ = -1;
    task_ = nullptr;
    connected_ = false;
    return fd;
}

NetworkResult<> ProxyTcp::Connect(const std::string& host, int port) {
    if (delegate_ != nullptr && NetworkSettings::IsLocalOrLanHost(host)) {
        direct_ = delegate_->CreateTcp();
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
    return ConnectTunnel(host, port, reader_);
}

NetworkResult<> ProxyTcp::ConnectTunnel(const std::string& host, int port, bool start_reader) {
    if (config_.host.empty() || config_.port <= 0 || host.empty()) {
        return Fail(NetworkError::InvalidArgument());
    }
    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* result = nullptr;
    std::string port_str = std::to_string(config_.port);
    if (getaddrinfo(config_.host.c_str(), port_str.c_str(), &hints, &result) != 0 ||
        result == nullptr) {
        return Fail(NetworkError::DnsFailed());
    }
    fd_ = socket(result->ai_family, SOCK_STREAM, 0);
    if (fd_ < 0) {
        freeaddrinfo(result);
        return Fail(NetworkError::ConnectFailed(errno));
    }
    int timeout_ms = 10000;
    setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout_ms, sizeof(timeout_ms));
    setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &timeout_ms, sizeof(timeout_ms));
    if (connect(fd_, result->ai_addr, result->ai_addrlen) != 0) {
        int err = errno;
        close(fd_);
        fd_ = -1;
        freeaddrinfo(result);
        return Fail(NetworkError::ConnectFailed(err));
    }
    freeaddrinfo(result);

    bool ok = false;
    if (config_.type == "socks5") {
        // Greeting: no-auth, or username/password when configured.
        bool auth = !config_.username.empty();
        uint8_t hello[] = {0x05, 0x01, static_cast<uint8_t>(auth ? 0x02 : 0x00)};
        uint8_t reply[2];
        if (!SendAll(fd_, hello, sizeof(hello)) || !RecvExact(fd_, reply, 2) ||
            reply[0] != 0x05 || reply[1] != (auth ? 0x02 : 0x00)) {
            ESP_LOGE(TAG, "SOCKS5 greeting failed");
        } else if (auth) {
            size_t ulen = config_.username.size(), plen = config_.password.size();
            std::string auth_msg;
            auth_msg.reserve(3 + ulen + plen);
            auth_msg.push_back(0x01);
            auth_msg.push_back(static_cast<char>(ulen));
            auth_msg += config_.username;
            auth_msg.push_back(static_cast<char>(plen));
            auth_msg += config_.password;
            if (!SendAll(fd_, auth_msg.data(), auth_msg.size()) || !RecvExact(fd_, reply, 2) ||
                reply[1] != 0x00) {
                ESP_LOGE(TAG, "SOCKS5 auth rejected");
            } else {
                ok = true;
            }
        } else {
            ok = true;
        }
        if (ok) {
            std::string req;
            req.reserve(host.size() + 10);
            req.push_back(0x05);
            req.push_back(0x01);
            req.push_back(0x00);
            req.push_back(0x03);  // domain name: let the proxy resolve it
            req.push_back(static_cast<char>(host.size()));
            req += host;
            req.push_back(static_cast<char>(port >> 8));
            req.push_back(static_cast<char>(port & 0xff));
            uint8_t head[4];
            if (!SendAll(fd_, req.data(), req.size()) || !RecvExact(fd_, head, 4) ||
                head[1] != 0x00) {
                ESP_LOGE(TAG, "SOCKS5 connect failed (rep=%d)", head[1]);
                ok = false;
            } else {
                size_t skip = head[3] == 0x01 ? 4 : (head[3] == 0x04 ? 16 : 0);
                if (head[3] == 0x03) {
                    uint8_t len = 0;
                    ok = RecvExact(fd_, &len, 1) && RecvExact(fd_, head, 2 + len);
                } else {
                    uint8_t rest[18];
                    ok = RecvExact(fd_, rest, skip + 2);
                }
            }
        }
    } else {
        std::string req = "CONNECT " + host + ":" + std::to_string(port) +
                          " HTTP/1.1\r\nHost: " + host + ":" + std::to_string(port) + "\r\n";
        if (!config_.username.empty()) {
            std::string cred = config_.username + ":" + config_.password;
            size_t olen = 0;
            mbedtls_base64_encode(nullptr, 0, &olen,
                                  reinterpret_cast<const unsigned char*>(cred.data()), cred.size());
            std::string b64(olen, '\0');
            mbedtls_base64_encode(reinterpret_cast<unsigned char*>(b64.data()), b64.size(), &olen,
                                  reinterpret_cast<const unsigned char*>(cred.data()), cred.size());
            req += "Proxy-Authorization: Basic " + b64 + "\r\n";
        }
        req += "\r\n";
        std::string response;
        while (response.find("\r\n\r\n") == std::string::npos && response.size() < 8192) {
            char buf[512];
            int n = recv(fd_, buf, sizeof(buf), 0);
            if (n <= 0) break;
            response.append(buf, n);
        }
        ok = response.find(" 200 ") != std::string::npos;
        if (!ok) ESP_LOGE(TAG, "HTTP CONNECT failed: %s", response.c_str());
    }
    if (!ok) {
        close(fd_);
        fd_ = -1;
        return Fail(NetworkError::ProtocolError());
    }

    connected_ = true;
    if (start_reader) {
        stopping_ = false;
        if (xTaskCreate([](void* arg) { static_cast<ProxyTcp*>(arg)->ReceiveTask(); },
                        "proxy_tcp_rx", 4096, this, 5, &task_) != pdPASS) {
            connected_ = false;
            close(fd_);
            fd_ = -1;
            return Fail(NetworkError::Unknown());
        }
    }
    ESP_LOGI(TAG, "tunnel ready %s:%d via %s (%s)", host.c_str(), port,
             config_.host.c_str(), config_.type.c_str());
    return {};
}

void ProxyTcp::ReceiveTask() {
    std::string buffer;
    buffer.resize(2048);
    while (!stopping_) {
        int n = recv(fd_, buffer.data(), buffer.size(), 0);
        if (n > 0) {
            if (stream_callback_) stream_callback_(buffer.substr(0, n));
        } else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            break;
        }
    }
    connected_ = false;
    if (disconnect_callback_) disconnect_callback_();
    task_ = nullptr;
    vTaskDelete(nullptr);
}

int ProxyTcp::Send(const std::string& data) {
    if (direct_) return direct_->Send(data);
    if (fd_ < 0) return -1;
    return send(fd_, data.data(), data.size(), 0);
}

void ProxyTcp::Disconnect() {
    stopping_ = true;
    if (direct_) {
        direct_->Disconnect();
        direct_.reset();
    }
    if (fd_ >= 0) {
        shutdown(fd_, SHUT_RDWR);
        close(fd_);
        fd_ = -1;
    }
    connected_ = false;
}
