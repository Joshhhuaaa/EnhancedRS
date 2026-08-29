#include "stdafx.h"
#include "common.hpp"
#include "feature.hpp"

#include <winsock2.h>

#pragma comment(lib, "ws2_32.lib")

namespace
{
    constexpr uint16_t WsockGetHostName = 57;

    int (__stdcall* pGetHostName)(char*, int) = nullptr;

    void Format(const uint8_t* octets, char* out, size_t size)
    {
        _snprintf_s(out, size, _TRUNCATE, "%u.%u.%u.%u", octets[0], octets[1], octets[2], octets[3]);
    }

    // UDP connect sends nothing; it only selects the interface that would route the datagram.
    // Adapters without a default route cannot be selected.
    bool RoutableAddress(char* out, size_t size)
    {
        SOCKET probe = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (probe == INVALID_SOCKET)
            return false;

        sockaddr_in remote{};
        remote.sin_family = AF_INET;
        remote.sin_port = htons(53);
        remote.sin_addr.s_addr = htonl(0x08080808);

        sockaddr_in local{};
        int length = sizeof(local);

        bool routed = connect(probe, reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) == 0
                   && getsockname(probe, reinterpret_cast<sockaddr*>(&local), &length) == 0
                   && local.sin_addr.s_addr != INADDR_ANY;

        closesocket(probe);

        if (routed)
            Format(reinterpret_cast<const uint8_t*>(&local.sin_addr), out, size);

        return routed;
    }

    void LogChoice(const char* chosen)
    {
        char host[256]{};
        std::string offered;

        if (pGetHostName(host, sizeof(host)) == 0)
            if (auto* entry = gethostbyname(host))
                for (int i = 0; entry->h_addr_list[i]; ++i)
                {
                    char text[16];
                    Format(reinterpret_cast<const uint8_t*>(entry->h_addr_list[i]), text, sizeof(text));
                    offered += (i ? ", " : "") + std::string(text);
                }

        spdlog::info("LocalAddress: using {}, gethostname offered {}", chosen, offered);
    }

    int __stdcall GetHostName(char* name, int length)
    {
        if (!RoutableAddress(name, length))
            return pGetHostName(name, length);

        static bool logged = false;
        if (!logged)
        {
            logged = true;
            LogChoice(name);
        }

        return 0;
    }
}

// The engine uses gethostname's first result for socket binding and LAN advertisements.
// If it resolves to an unreachable adapter, clients cannot connect.
FEATURE(Game, LocalAddress)
{
    auto game = GetModuleHandleW(nullptr);

    pGetHostName = reinterpret_cast<decltype(pGetHostName)>(Memory::ReadIAT(game, "WSOCK32.dll", WsockGetHostName));
    if (!pGetHostName || !Memory::WriteIAT(game, "WSOCK32.dll", WsockGetHostName, GetHostName))
    {
        spdlog::error("LocalAddress: WSOCK32 gethostname import not found");
        return;
    }

    spdlog::info("LocalAddress: sockets will bind the default-route adapter");
}
