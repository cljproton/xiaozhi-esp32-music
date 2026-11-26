#include "network_config.h"
#include "settings.h"

#include <lwip/sockets.h>
#include <lwip/inet.h>
#include <algorithm>
#include <cctype>
#include <esp_log.h>
#include <esp_netif.h>

#define TAG "NetworkConfig"

namespace NetworkSettings {

StaticNetworkConfig LoadStatic() {
    Settings s("network", false);
    StaticNetworkConfig c;
    c.enabled = s.GetBool("static_enabled", false);
    c.ip = s.GetString("ip", "");
    c.netmask = s.GetString("netmask", "");
    c.gateway = s.GetString("gateway", "");
    c.dns1 = s.GetString("dns1", "");
    c.dns2 = s.GetString("dns2", "");
    return c;
}

ProxyConfig LoadProxy() {
    Settings s("proxy", false);
    ProxyConfig c;
    c.enabled = s.GetBool("enabled", false);
    c.type = s.GetString("type", "http");
    c.host = s.GetString("host", "");
    c.port = s.GetInt("port", 0);
    c.username = s.GetString("username", "");
    c.password = s.GetString("password", "");
    return c;
}

void ApplyStaticIp() {
    auto c = LoadStatic();
    if (!c.enabled) return;
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == nullptr) {
        ESP_LOGW(TAG, "station netif not found, static IP not applied");
        return;
    }
    esp_netif_ip_info_t info = {};
    if (inet_pton(AF_INET, c.ip.c_str(), &info.ip) != 1 ||
        inet_pton(AF_INET, c.netmask.c_str(), &info.netmask) != 1 ||
        inet_pton(AF_INET, c.gateway.c_str(), &info.gw) != 1) {
        ESP_LOGW(TAG, "invalid static IP settings, using DHCP");
        return;
    }
    esp_netif_dhcpc_stop(netif);
    esp_netif_set_ip_info(netif, &info);
    auto set_dns = [netif](esp_netif_dns_type_t type, const std::string& value) {
        if (value.empty()) return;
        esp_netif_dns_info_t dns = {};
        if (inet_pton(AF_INET, value.c_str(), &dns.ip.u_addr.ip4) == 1) {
            esp_netif_set_dns_info(netif, type, &dns);
        }
    };
    set_dns(ESP_NETIF_DNS_MAIN, c.dns1);
    set_dns(ESP_NETIF_DNS_BACKUP, c.dns2);
    ESP_LOGI(TAG, "static IP applied: %s", c.ip.c_str());
}

bool IsLocalOrLanHost(const std::string& host) {
    std::string lower = host;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if ((lower == "localhost" || (lower.size() >= 6 &&
        lower.compare(lower.size() - 6, 6, ".local") == 0))) return true;
    struct in_addr addr;
    if (inet_pton(AF_INET, host.c_str(), &addr) == 1) {
        auto* b = reinterpret_cast<const uint8_t*>(&addr);
        return b[0] == 127 || b[0] == 10 || b[0] == 0 ||
               (b[0] == 192 && b[1] == 168) ||
               (b[0] == 172 && b[1] >= 16 && b[1] <= 31) ||
               (b[0] == 169 && b[1] == 254);
    }
    return lower.rfind("::1", 0) == 0 || lower.rfind("fe80", 0) == 0 ||
           lower.rfind("fc", 0) == 0 || lower.rfind("fd", 0) == 0;
}

}  // namespace NetworkSettings
