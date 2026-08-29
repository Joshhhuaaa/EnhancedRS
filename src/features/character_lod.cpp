#include "stdafx.h"
#include "common.hpp"
#include "feature.hpp"

namespace
{
    constexpr ptrdiff_t LodSwitch1 = 0x8c;
    constexpr ptrdiff_t LodSwitch2 = 0x90;

    // Well beyond any map range and safe for squared distance calculations.
    // Kept separate in case the difference is used as a divisor.
    constexpr float Switch1 = 1.0e12f;
    constexpr float Switch2 = 2.0e12f;

    SafetyHookInline shLoadConstants{};

    int __fastcall LoadConstants(uint8_t* constants, void*, void* source)
    {
        const int result = shLoadConstants.thiscall<int>(constants, source);

        auto& first  = *reinterpret_cast<float*>(constants + LodSwitch1);
        auto& second = *reinterpret_cast<float*>(constants + LodSwitch2);

        static bool logged = false;
        if (!logged)
        {
            logged = true;
            spdlog::info("CharacterLod: stock switches at {:.0f} and {:.0f}, both pushed out of range",
                         first, second);
        }

        first  = Switch1;
        second = Switch2;

        return result;
    }
}

// Constants.txt sets character LOD meshes to switch at 325 and 725 units.
// Hook the loader to handle loadconstants reloads.
FEATURE(Game, CharacterLod)
{
    auto load = hook::pattern("55 8B EC 6A FF 68 ? ? ? ? 64 A1 00 00 00 00 50 64 89 25 "
                              "00 00 00 00 51 81 EC 60 09 00 00");
    if (load.empty())
    {
        spdlog::error("CharacterLod: constants loader not found");
        return;
    }

    shLoadConstants = Memory::Hook(load.get_first(), LoadConstants);
    if (!shLoadConstants)
    {
        spdlog::error("CharacterLod: hook installation failed");
        return;
    }

    spdlog::info("CharacterLod: characters keep their full mesh at every range");
}
