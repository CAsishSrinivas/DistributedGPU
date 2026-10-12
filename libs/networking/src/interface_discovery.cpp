#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "networking/interface_discovery.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <iostream>
#include <memory>
#include <vector>

#pragma comment(lib, "IPHLPAPI.lib")
#pragma comment(lib, "Ws2_32.lib")

namespace dgpu::networking {

namespace {

std::string wideToUtf8(const wchar_t* wstr) {
    if (!wstr || *wstr == L'\0') return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, result.data(), size, nullptr, nullptr);
    return result;
}

std::string toLower(std::string_view str) {
    std::string lower;
    lower.reserve(str.size());
    for (char c : str) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return lower;
}

bool containsCaseInsensitive(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return true;
    if (haystack.empty()) return false;
    std::string h = toLower(haystack);
    std::string n = toLower(needle);
    return h.find(n) != std::string::npos;
}

InterfaceType classifyInterface(const IP_ADAPTER_ADDRESSES* adapter, std::string_view friendly, std::string_view desc) {
    if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
        return InterfaceType::Loopback;
    }

    std::string combined = toLower(friendly);
    combined += " ";
    combined += toLower(desc);

    // Detect direct high-speed interconnects (USB4, Thunderbolt, USB-C NCM/RNDIS)
    if (combined.find("usb4") != std::string::npos ||
        combined.find("thunderbolt") != std::string::npos ||
        combined.find("rndis") != std::string::npos ||
        combined.find("ncm") != std::string::npos ||
        combined.find("cdc") != std::string::npos ||
        combined.find("usb ethernet") != std::string::npos ||
        combined.find("type-c") != std::string::npos) {
        return InterfaceType::UsbOrThunderbolt;
    }

    if (adapter->IfType == IF_TYPE_IEEE80211) {
        return InterfaceType::WiFi;
    }

    if (adapter->IfType == IF_TYPE_ETHERNET_CSMACD) {
        return InterfaceType::Ethernet;
    }

    return InterfaceType::Unknown;
}

} // namespace

std::vector<NetworkInterfaceInfo> InterfaceDiscovery::enumerateInterfaces() {
    std::vector<NetworkInterfaceInfo> interfaces;

    ULONG flags = GAA_FLAG_INCLUDE_PREFIX | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG outBufLen = 15000;
    std::vector<BYTE> buffer(outBufLen);

    DWORD ret = GetAdaptersAddresses(AF_INET, flags, nullptr,
                                     reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &outBufLen);
    if (ret == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(outBufLen);
        ret = GetAdaptersAddresses(AF_INET, flags, nullptr,
                                   reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &outBufLen);
    }

    if (ret != NO_ERROR) {
        return interfaces;
    }

    for (auto* curr = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); curr != nullptr; curr = curr->Next) {
        NetworkInterfaceInfo info;
        info.adapterName  = curr->AdapterName ? curr->AdapterName : "";
        info.friendlyName = wideToUtf8(curr->FriendlyName);
        info.description  = wideToUtf8(curr->Description);
        info.linkSpeedBps = curr->TransmitLinkSpeed;
        info.isUp         = (curr->OperStatus == IfOperStatusUp);
        info.type         = classifyInterface(curr, info.friendlyName, info.description);
        info.isPointToPoint = (curr->IfType == IF_TYPE_PPP || curr->IfType == IF_TYPE_TUNNEL ||
                               info.type == InterfaceType::UsbOrThunderbolt);

        for (auto* unicast = curr->FirstUnicastAddress; unicast != nullptr; unicast = unicast->Next) {
            if (unicast->Address.lpSockaddr && unicast->Address.lpSockaddr->sa_family == AF_INET) {
                char ipBuf[INET_ADDRSTRLEN] = {0};
                auto* sa = reinterpret_cast<sockaddr_in*>(unicast->Address.lpSockaddr);
                if (inet_ntop(AF_INET, &(sa->sin_addr), ipBuf, sizeof(ipBuf))) {
                    info.ipv4Address = ipBuf;
                    break;
                }
            }
        }

        interfaces.push_back(std::move(info));
    }

    return interfaces;
}

std::vector<NetworkInterfaceInfo> InterfaceDiscovery::findInterfaces(std::string_view query) {
    std::vector<NetworkInterfaceInfo> matches;
    auto all = enumerateInterfaces();
    for (auto& iface : all) {
        if (containsCaseInsensitive(iface.friendlyName, query) ||
            containsCaseInsensitive(iface.description, query) ||
            containsCaseInsensitive(iface.ipv4Address, query)) {
            matches.push_back(std::move(iface));
        }
    }
    return matches;
}

std::vector<NetworkInterfaceInfo> InterfaceDiscovery::findUsb4Interfaces() {
    std::vector<NetworkInterfaceInfo> usb4;
    auto all = enumerateInterfaces();
    for (auto& iface : all) {
        if (iface.type == InterfaceType::UsbOrThunderbolt) {
            usb4.push_back(std::move(iface));
        }
    }
    return usb4;
}

NetworkInterfaceInfo InterfaceDiscovery::resolveBestInterface(TransportType transport) {
    auto all = enumerateInterfaces();

    if (transport == TransportType::TcpUsb4) {
        for (const auto& iface : all) {
            if (iface.type == InterfaceType::UsbOrThunderbolt && iface.isUp && !iface.ipv4Address.empty()) {
                return iface;
            }
        }
        for (const auto& iface : all) {
            if (iface.type == InterfaceType::UsbOrThunderbolt) {
                return iface;
            }
        }
    }

    if (transport == TransportType::TcpLan) {
        for (const auto& iface : all) {
            if (iface.type == InterfaceType::Ethernet && iface.isUp && !iface.ipv4Address.empty()) {
                return iface;
            }
        }
        for (const auto& iface : all) {
            if (iface.type == InterfaceType::WiFi && iface.isUp && !iface.ipv4Address.empty()) {
                return iface;
            }
        }
    }

    // Auto or fallback: prefer high-speed direct, then active Ethernet, then active Wi-Fi, then any up
    for (const auto& iface : all) {
        if (iface.type == InterfaceType::UsbOrThunderbolt && iface.isUp && !iface.ipv4Address.empty()) {
            return iface;
        }
    }
    for (const auto& iface : all) {
        if (iface.type == InterfaceType::Ethernet && iface.isUp && !iface.ipv4Address.empty()) {
            return iface;
        }
    }
    for (const auto& iface : all) {
        if (iface.type == InterfaceType::WiFi && iface.isUp && !iface.ipv4Address.empty()) {
            return iface;
        }
    }
    for (const auto& iface : all) {
        if (iface.isUp && !iface.ipv4Address.empty() && iface.type != InterfaceType::Loopback) {
            return iface;
        }
    }

    return {};
}

} // namespace dgpu::networking
