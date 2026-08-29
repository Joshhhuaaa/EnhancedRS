#pragma once

#include "common.hpp"

// Shared mouse state used by window.cpp, input.cpp, and mouse_buttons.cpp
namespace Mouse
{
    using ::Game::AppObject;

    constexpr ptrdiff_t AppDisplay  = 0x04;
    constexpr ptrdiff_t AppGameMode = 0x08;
    constexpr ptrdiff_t AppInput    = 0x0c;
    constexpr ptrdiff_t AppOptions  = 0x10;
    constexpr ptrdiff_t AppRoot     = 0x14;

    constexpr ptrdiff_t DisplayWindow = 0x08;

    constexpr ptrdiff_t GameModeCurrent = 0xcc;
    constexpr ptrdiff_t GameModeScreen  = 0xd4;
    constexpr int       MissionState    = 5;
    constexpr int       NetMissionState = 6;

    // Set while the world view is drawn, menus use legacy mouse messages instead
    constexpr ptrdiff_t ActionScreenViewShown = 0x1ee;

    constexpr ptrdiff_t OptionsControls   = 0x358;
    constexpr ptrdiff_t ControlsMouseLook = 0x4d4;

    inline volatile LONG rawX = 0;
    inline volatile LONG rawY = 0;
    inline bool bRawActive = false;

    // window.cpp keeps this synced with WM_ACTIVATEAPP
    inline bool bActive = true;

    // input.cpp points this at win32u when the compat shim has replaced the user32 export
    inline UINT(WINAPI* pGetRawInputData)(HRAWINPUT, UINT, LPVOID, PUINT, UINT) = &GetRawInputData;

    // Registers or removes RIDEV_NOLEGACY to match OwnsTheCursor(). Defined in input.cpp.
    void UpdateRawInput();

    // Sends mouse 4/5 through the engine's button dispatcher, no-op if unavailable
    void SendExtraButton(int button, bool down);

    // The mouselook flag is a preference, not live state; the shell still needs mouse messages
    // when mouselook is disabled.
    inline bool OwnsTheCursor()
    {
        // RIDEV_NOLEGACY freezes the desktop pointer while registered, regardless of focus
        if (!bActive)
            return false;

        auto app = *reinterpret_cast<uint8_t**>(AppObject);
        if (!app)
            return false;

        auto gameMode = *reinterpret_cast<uint8_t**>(app + AppGameMode);
        if (!gameMode)
            return false;

        auto state = *reinterpret_cast<int*>(gameMode + GameModeCurrent);
        if (state != MissionState && state != NetMissionState)
            return false;

        auto screen = *reinterpret_cast<uint8_t**>(gameMode + GameModeScreen);
        if (!screen || !*(screen + ActionScreenViewShown))
            return false;

        auto options = *reinterpret_cast<uint8_t**>(app + AppOptions);
        auto controls = options ? *reinterpret_cast<uint8_t**>(options + OptionsControls) : nullptr;
        return controls && *(controls + ControlsMouseLook) != 0;
    }

    inline HWND Window()
    {
        auto app = *reinterpret_cast<uint8_t**>(AppObject);
        auto display = app ? *reinterpret_cast<uint8_t**>(app + AppDisplay) : nullptr;
        return display ? *reinterpret_cast<HWND*>(display + DisplayWindow) : nullptr;
    }
}
