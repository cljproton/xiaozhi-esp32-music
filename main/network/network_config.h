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

namespace NetworkSettings {
StaticNetworkConfig LoadStatic();
// Apply the saved static IP to the station interface. Called after Wi-Fi
// connects, so saving settings never drops the current web session.
void ApplyStaticIp();
}  // namespace NetworkSettings
