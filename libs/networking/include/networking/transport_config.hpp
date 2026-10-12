#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace dgpu::networking {

/// Direct link or standard network transport selection
enum class TransportType {
    Auto,      ///< Automatically resolve (use explicit interface if given, else default)
    TcpLan,    ///< Standard Ethernet / Wi-Fi TCP transport (existing default)
    TcpUsb4    ///< Direct USB4 / USB-C / Thunderbolt high-speed point-to-point TCP transport
};

[[nodiscard]] constexpr std::string_view transportTypeToString(TransportType t) noexcept {
    switch (t) {
        case TransportType::Auto:    return "auto";
        case TransportType::TcpLan:  return "tcp";
        case TransportType::TcpUsb4: return "usb4";
    }
    return "unknown";
}

[[nodiscard]] inline TransportType stringToTransportType(std::string_view str) noexcept {
    if (str == "usb4" || str == "usb-c" || str == "thunderbolt") return TransportType::TcpUsb4;
    if (str == "tcp" || str == "lan" || str == "ethernet" || str == "wifi") return TransportType::TcpLan;
    return TransportType::Auto;
}

/// Fully configurable endpoint and transport specification
struct EndpointConfig {
    std::string   host{"127.0.0.1"};
    uint16_t      port{7777};
    TransportType transport{TransportType::Auto};
    std::string   interfaceName; ///< Optional interface alias/description to bind or query (e.g. "USB4", "Ethernet")
    std::string   localBindIp;   ///< Optional local IP to bind before connecting

    /// Parse "host:port", "host", "usb4://host:port", or "tcp://host:port"
    static EndpointConfig parse(std::string_view endpointStr, uint16_t defaultPort = 7777) {
        EndpointConfig cfg;
        cfg.port = defaultPort;
        if (endpointStr.empty()) return cfg;

        std::string_view str = endpointStr;
        if (str.starts_with("usb4://")) {
            cfg.transport = TransportType::TcpUsb4;
            str.remove_prefix(7);
        } else if (str.starts_with("tcp://")) {
            cfg.transport = TransportType::TcpLan;
            str.remove_prefix(6);
        }

        size_t colon = str.rfind(':');
        if (colon != std::string_view::npos) {
            cfg.host = std::string(str.substr(0, colon));
            try {
                cfg.port = static_cast<uint16_t>(std::stoul(std::string(str.substr(colon + 1))));
            } catch (...) {
                cfg.port = defaultPort;
            }
        } else {
            cfg.host = std::string(str);
        }
        return cfg;
    }
};

} // namespace dgpu::networking
