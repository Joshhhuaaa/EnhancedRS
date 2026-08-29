#include "stdafx.h"
#include "common.hpp"
#include "feature.hpp"

namespace
{
    constexpr uint32_t  MaxDimension = 8192;
    constexpr ptrdiff_t RGBBitCount  = 0x54;  // DDSURFACEDESC2::ddpfPixelFormat.dwRGBBitCount
    constexpr uint32_t  MenuBitDepth = 16;

    SafetyHookInline shCollectDisplayMode{};

    // The mode table holds 50 entries and cancels enumeration once full, while the video options
    // list only 16-bit ones. A wrapper reporting each resolution at 32, 16 and 8 bits spends two
    // thirds of the table on modes the menu cannot show, and the highest are what fall off the end.
    BOOL __stdcall CollectDisplayMode(uint8_t* desc, void* context)
    {
        if (*reinterpret_cast<uint32_t*>(desc + RGBBitCount) != MenuBitDepth)
            return TRUE;

        return shCollectDisplayMode.stdcall<BOOL>(desc, context);
    }
}

FEATURE(Game, UnlockResolutions)
{
    // The callback discards anything wider than 1024 or taller than 768 before it reaches the
    // table, which is what holds the stock list to 640x480 through 1024x768.
    auto ceiling = hook::pattern("8B 44 24 04 8B 48 0C 81 F9 00 04 00 00 77 ? 81 78 08 00 03 00 00");
    if (ceiling.empty())
    {
        spdlog::error("UnlockResolutions: display mode ceiling not found");
        return;
    }

    injector::WriteMemory<uint32_t>(ceiling.get_first(9), MaxDimension, true);
    injector::WriteMemory<uint32_t>(ceiling.get_first(18), MaxDimension, true);

    shCollectDisplayMode = Memory::Hook(ceiling.get_first(), CollectDisplayMode);
    if (!shCollectDisplayMode)
    {
        spdlog::error("UnlockResolutions: hook installation failed");
        return;
    }

    spdlog::info("UnlockResolutions: ceiling raised to {}, non-{}-bit modes dropped",
                 MaxDimension, MenuBitDepth);
}
