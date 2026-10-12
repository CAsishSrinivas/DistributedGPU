#pragma once

#include "networking/transport_config.hpp"
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dgpu::networking {

enum class InterfaceType {
    Unknown,
    Ethernet,
    WiFi,
    UsbOrThunderbolt,
    Loopback
};

[[nodiscard]] constexpr std::string_view interfaceTypeToString(InterfaceType t) noexcept {
    switch (t) {
        case InterfaceType::Ethernet:         return "Ethernet";
        case InterfaceType::WiFi:             return "Wi-Fi";
        case InterfaceType::UsbOrThunderbolt: return "USB4/Thunderbolt";
        case InterfaceType::Loopback:         return "Loopback";
        case InterfaceType::Unknown:          return "Unknown";
    }
    return "Unknown";
}

struct NetworkInterfaceInfo {
    std::string   adapterName;      ///< Internal adapter GUID or name
    std::string   friendlyName;     ///< User-visible friendly alias (e.g. "Ethernet", "Wi-Fi", "USB4")
    std::string   description;      ///< Driver/device description
    std::string   ipv4Address;      ///< Primary IPv4 address configured on this interface
    uint64_t      linkSpeedBps{0};  ///< Raw link bandwidth in bits per second (e.g. 1 Gbps, 20 Gbps)
    InterfaceType type{InterfaceType::Unknown};
    bool          isUp{false};
    bool          isPointToPoint{false};
};

class InterfaceDiscovery {
public:
    /// Enumerates all network interfaces visible to Windows
    [[nodiscard]] static std::vector<NetworkInterfaceInfo> enumerateInterfaces();

    /// Finds interfaces matching a query string (case-insensitive substring in alias, description, or IP)
    [[nodiscard]] static std::vector<NetworkInterfaceInfo> findInterfaces(std::string_view query);

    /// Identifies potential high-speed direct USB4 / Thunderbolt / Point-to-Point network adapters
    [[nodiscard]] static std::vector<NetworkInterfaceInfo> findUsb4Interfaces();

    /// Resolves the best local interface matching the requested transport mode
    [[nodiscard]] static NetworkInterfaceInfo resolveBestInterface(TransportType transport);
};

} // namespace dgpu::networking
