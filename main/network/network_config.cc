#include "network_config.h"
#include "settings.h"

#include <lwip/sockets.h>
#include <lwip/inet.h>
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

}  // namespace NetworkSettings
