#include "stdafx.h"
#include "common.hpp"
#include "feature.hpp"

namespace
{
    using Game::Renderer;
    constexpr ptrdiff_t RendererDDraw = 0x240;

    SafetyHookInline shResetMode{};

    ULONG Call(void* object, size_t slot)  // IUnknown: 1 = AddRef, 2 = Release
    {
        return (*reinterpret_cast<ULONG(__stdcall***)(void*)>(object))[slot](object);
    }

    // Under Dd7to9, the Direct3D9 device dies with the last DirectDraw object. Hold the outgoing
    // object across Display_ResetMode so the second CreateDevice becomes a Reset. Overlays such as RTSS
    // hook the first device and can crash if a second is created. The surfaces still get torn down.
    char __fastcall ResetMode(void* display, void*, int mode)
    {
        void* ddraw = *reinterpret_cast<void**>(Renderer + RendererDDraw);
        if (ddraw)
            Call(ddraw, 1);

        const char result = shResetMode.thiscall<char>(display, mode);

        if (ddraw)
            Call(ddraw, 2);

        return result;
    }
}

FEATURE(Game, DeviceReuse)
{
    auto reset = hook::pattern("A1 ? ? ? ? 56 8B F1 8B 48 24 E8 ? ? ? ? 8B 4E 38 85 C9");
    // Renderer_Shutdown's fullscreen branch calls RestoreDisplayMode and SetCooperativeLevel(NORMAL),
    // which the wrapper handles by destroying the device. Neither can run here.
    auto fullscreen = hook::pattern("38 9D C0 0F 00 00 74 13 8B CD E8 ? ? ? ? 8B 55 24 53 52");
    if (reset.empty() || fullscreen.empty())
    {
        spdlog::error("DeviceReuse: Display_ResetMode {}, Renderer_Shutdown fullscreen branch {}",
                      reset.empty() ? "not found" : "found", fullscreen.empty() ? "not found" : "found");
        return;
    }

    injector::WriteMemory<uint8_t>(fullscreen.get_first(6), 0xEB, true);

    shResetMode = Memory::Hook(reset.get_first(), ResetMode);
    if (!shResetMode)
    {
        spdlog::error("DeviceReuse: hook installation failed");
        return;
    }

    spdlog::info("DeviceReuse: Direct3D9 device kept across renderer rebuilds");
}
