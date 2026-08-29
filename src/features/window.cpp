#include "stdafx.h"
#include "common.hpp"
#include "feature.hpp"
#include "mouse.hpp"

namespace
{
    SafetyHookInline shGameWndProc{};

    // The engine's own way of making every visible screen repaint its background.
    void(__fastcall* InvalidateChildren)(void*, void*) = nullptr;

    bool bEatButtonUp = false;

    // RIDEV_NOLEGACY removes the button messages along with the moves, and the engine fires and
    // zooms from those, so they are rebuilt from the raw packet's transitions.
    struct Button
    {
        USHORT down;
        USHORT up;
        UINT   msgDown;
        UINT   msgUp;
        WPARAM key;
    };

    constexpr Button Buttons[]
    {
        { RI_MOUSE_LEFT_BUTTON_DOWN,   RI_MOUSE_LEFT_BUTTON_UP,   WM_LBUTTONDOWN, WM_LBUTTONUP, MK_LBUTTON },
        { RI_MOUSE_RIGHT_BUTTON_DOWN,  RI_MOUSE_RIGHT_BUTTON_UP,  WM_RBUTTONDOWN, WM_RBUTTONUP, MK_RBUTTON },
        { RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP, WM_MBUTTONDOWN, WM_MBUTTONUP, MK_MBUTTON },
    };

    void RelayButtons(HWND hWnd, const RAWMOUSE& mouse)
    {
        if (!mouse.usButtonFlags)
            return;

        POINT cursor{};
        GetCursorPos(&cursor);
        const LPARAM screen = MAKELPARAM(cursor.x, cursor.y);
        ScreenToClient(hWnd, &cursor);
        const LPARAM client = MAKELPARAM(cursor.x, cursor.y);

        for (auto& button : Buttons)
        {
            if (mouse.usButtonFlags & button.down)
                shGameWndProc.stdcall<LRESULT>(hWnd, button.msgDown, button.key, client);
            if (mouse.usButtonFlags & button.up)
                shGameWndProc.stdcall<LRESULT>(hWnd, button.msgUp, 0, client);
        }

        // No window message for these two; mouse_buttons.cpp dispatches them as key codes.
        if (mouse.usButtonFlags & RI_MOUSE_BUTTON_4_DOWN) Mouse::SendExtraButton(0, true);
        if (mouse.usButtonFlags & RI_MOUSE_BUTTON_4_UP)   Mouse::SendExtraButton(0, false);
        if (mouse.usButtonFlags & RI_MOUSE_BUTTON_5_DOWN) Mouse::SendExtraButton(1, true);
        if (mouse.usButtonFlags & RI_MOUSE_BUTTON_5_UP)   Mouse::SendExtraButton(1, false);

        if (mouse.usButtonFlags & RI_MOUSE_WHEEL)
            shGameWndProc.stdcall<LRESULT>(hWnd, WM_MOUSEWHEEL,
                                           MAKEWPARAM(0, static_cast<SHORT>(mouse.usButtonData)), screen);
    }

    // Pass Alt+F4 to DefWindowProc so it becomes a close command.
    // Hooked because dxwrapper already subclasses the window.
    LRESULT CALLBACK GameWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (msg == WM_SYSKEYDOWN && wParam == VK_F4)
            return DefWindowProcA(hWnd, msg, wParam, lParam);

        // DefWindowProc is required to release the WM_INPUT buffer; RIDEV_NOLEGACY only removes cursor
        // motion and legacy mouse messages. Re-enable the legacy stream here while paused.
        if (msg == WM_INPUT)
        {
            // Raw input is not updated while paused, so restore legacy mouse input for the pause menu.
            Mouse::UpdateRawInput();

            if (Mouse::bRawActive)
            {
                RAWINPUT raw;
                UINT size = sizeof(raw);

                if (Mouse::pGetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, &raw,
                                            &size, sizeof(RAWINPUTHEADER)) != static_cast<UINT>(-1) &&
                    raw.header.dwType == RIM_TYPEMOUSE)
                {
                    if ((raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0)
                    {
                        InterlockedExchangeAdd(&Mouse::rawX, raw.data.mouse.lLastX);
                        InterlockedExchangeAdd(&Mouse::rawY, raw.data.mouse.lLastY);
                    }

                    RelayButtons(hWnd, raw.data.mouse);
                }
            }

            return DefWindowProcA(hWnd, msg, wParam, lParam);
        }

        // Clicking an inactive window activates it and then delivers the click, so taking focus by
        // clicking fires the weapon or presses whatever the pointer landed on.
        if (msg == WM_MOUSEACTIVATE)
        {
            bEatButtonUp = true;
            return MA_ACTIVATEANDEAT;
        }

        // MA_ACTIVATEANDEAT discards the button-down but still delivers the up. The engine pairs them,
        // so an unmatched up reaches the dispatcher and dereferences null.
        if (bEatButtonUp && (msg == WM_LBUTTONUP || msg == WM_RBUTTONUP || msg == WM_MBUTTONUP ||
                             msg == WM_XBUTTONUP))
        {
            bEatButtonUp = false;
            return 0;
        }

        // The procedure predates XBUTTON, so these messages fall through to DefWindowProc and are ignored.
        // Forward them to the game as extra mouse buttons.
        if (msg == WM_XBUTTONDOWN || msg == WM_XBUTTONUP)
        {
            Mouse::SendExtraButton(GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? 0 : 1,
                                   msg == WM_XBUTTONDOWN);
            return TRUE;
        }

        if (msg == WM_ACTIVATEAPP)
        {
            Mouse::bActive = wParam != FALSE;
            SetCursor(Mouse::bActive ? nullptr : LoadCursorW(nullptr, IDC_ARROW));

            // Coming back under exclusive fullscreen the device was reset and the screen surface is
            // blank. Nothing sets a screen's dirty counter on activation, so nothing repaints it.
            // Missions redraw every frame and need none of this.
            auto app = *reinterpret_cast<uint8_t**>(Mouse::AppObject);
            auto root = app ? *reinterpret_cast<void**>(app + Mouse::AppRoot) : nullptr;
            if (Mouse::bActive && root && InvalidateChildren)
                InvalidateChildren(root, nullptr);
        }

        // WM_SETCURSOR is unhandled by the game, so it reaches DefWindowProc and the wrapper's cursor path.
        // The engine draws its own cursor, so return TRUE without setting anything.
        if (msg == WM_SETCURSOR)
        {
            // Answering without setting anything leaves the window the pointer came from in charge.
            if (!Mouse::bActive)
                SetCursor(LoadCursorW(nullptr, IDC_ARROW));

            return TRUE;
        }

        // Mouselook polls GetCursorPos directly, so these messages are unused. Skip them when inactive
        // or when the game owns the cursor.
        if (msg == WM_MOUSEMOVE && (!Mouse::bActive || Mouse::OwnsTheCursor()))
            return 0;

        return shGameWndProc.stdcall<LRESULT>(hWnd, msg, wParam, lParam);
    }
}

FEATURE(Game, Window)
{
    auto wndProc = hook::pattern("83 EC 0C 53 8B 5C 24 14 55 8B 6C 24 20 56 8B 74 24 28 57 8B 7C 24 24");
    if (wndProc.empty())
    {
        spdlog::error("Window: game window procedure not found");
        return;
    }

    shGameWndProc = Memory::Hook(wndProc.get_first(), GameWndProc);
    if (!shGameWndProc)
    {
        spdlog::error("Window: hook installation failed");
        return;
    }

    auto invalidate = hook::pattern("56 57 8B F9 33 F6 8B 47 20 85 C0 7E 1E 8B 47 24 8B 0C B0 85 C9 74 0C "
                                    "8A 41 1C 84 C0 74 05 8B 11 FF 52 28 8B 47 20 46 3B F0 7C E2 5F 5E C3");
    if (invalidate.empty())
        spdlog::error("Window: WidgetContainer_InvalidateChildren not found, shell will not repaint after a switch");
    else
        InvalidateChildren = reinterpret_cast<decltype(InvalidateChildren)>(invalidate.get_first());

    spdlog::info("Window: game window procedure hooked");
}
