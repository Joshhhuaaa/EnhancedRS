#include "stdafx.h"
#include "common.hpp"
#include "feature.hpp"

namespace
{
    // SmackOpen handle, with the frame width and height at +4 and +8.
    constexpr ptrdiff_t MovieSmack = 0x20;

    constexpr int DesignWidth  = 640;
    constexpr int DesignHeight = 480;

    SafetyHookInline shSetRect{};
    bool bLogged = false;

    // Startup movies use the display size, stretching their 4:3 frames to the screen aspect.
    // Render them at 640x480 instead so the menu scaling handles the aspect ratio and placement.
    void __fastcall SetMovieRect(uint8_t* movie, void*, int left, int top, int right, int bottom)
    {
        if (!bLogged)
        {
            const int* smack = *reinterpret_cast<int**>(movie + MovieSmack);
            spdlog::info("MovieAspect: {}x{} frame asked for {}x{}, drawn as {}x{}",
                         smack ? smack[1] : 0, smack ? smack[2] : 0,
                         right - left, bottom - top, DesignWidth, DesignHeight);
            bLogged = true;
        }

        return shSetRect.thiscall<void>(movie, 0, 0, DesignWidth, DesignHeight);
    }
}

FEATURE(Game, MovieAspect)
{
    auto setRect = hook::pattern("53 55 8B 6C 24 0C 56 8B F1 8B 5C 24 1C 57 8B 7C 24 1C 8D 46 28");
    if (setRect.empty())
    {
        spdlog::error("MovieAspect: Movie::SetRect not found");
        return;
    }

    shSetRect = Memory::Hook(setRect.get_first(), SetMovieRect);
    if (!shSetRect)
        return;

    spdlog::info("MovieAspect: movies scaled from 640x480");
}
