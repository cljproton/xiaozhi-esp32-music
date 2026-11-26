#pragma once

#include <string>

struct StaticNetworkConfig {
    bool enabled = false;
    std::string ip;
    std::string netmask;
    std::string gateway;
    std::string dns1;
    std::string dns2;
};

struct ProxyConfig {
    bool enabled = false;
    std::string type = "http";   // "http" or "socks5"
    std::string host;
    int port = 0;
    std::string username;
    std::string password;
};

namespace NetworkSettings {
StaticNetworkConfig LoadStatic();
ProxyConfig LoadProxy();
// Apply the saved static IP to the station interface. Called after Wi-Fi
// connects, so saving settings never drops the current web session.
void ApplyStaticIp();
// Localhost / RFC1918 / link-local destinations connect directly even when a
// proxy is enabled (LAN WebDAV, device Web UI, etc.).
bool IsLocalOrLanHost(const std::string& host);
}  // namespace NetworkSettings
