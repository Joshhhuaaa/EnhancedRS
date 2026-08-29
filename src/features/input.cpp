#include "stdafx.h"
#include "common.hpp"
#include "config.hpp"
#include "feature.hpp"
#include "mouse.hpp"

static Config::Float fLookSensitivity("Input", "LookSensitivity", 1.0f);

namespace
{
    constexpr ptrdiff_t TimingFrameDelta = 0x38;

    // Offsets from esp at the two hook sites.
    constexpr ptrdiff_t DeltaY    = 0x14;
    constexpr ptrdiff_t DeltaX    = 0x18;
    constexpr ptrdiff_t ScaleSlot = 0x10;

    SafetyHookInline shFrameTick{};
    SafetyHookInline shMouseLook{};
    SafetyHookMid    shMouseDelta{};
    SafetyHookMid    shMouseScale{};

    bool bLogged = false;
    bool bResync = false;
    bool bRawRefused = false;

    BOOL(WINAPI* pRegisterRawInputDevices)(PCRAWINPUTDEVICE, UINT, UINT) = nullptr;

    // Under the application-compatibility shim the user32 export resolves into AcGenral.dll, whose
    // stub fails RegisterRawInputDevices unconditionally. The win32u syscall stubs sit below the
    // shim and accept the same call, so both entry points go through them when the shim is present.
    void ResolveRawInput()
    {
        pRegisterRawInputDevices = &RegisterRawInputDevices;

        HMODULE owner = nullptr;
        wchar_t path[MAX_PATH]{};
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(pRegisterRawInputDevices), &owner);
        GetModuleFileNameW(owner, path, MAX_PATH);
        if (_wcsicmp(std::filesystem::path(path).filename().c_str(), L"user32.dll") == 0)
            return;

        HMODULE win32u = GetModuleHandleW(L"win32u.dll");
        auto reg = win32u ? GetProcAddress(win32u, "NtUserRegisterRawInputDevices") : nullptr;
        auto get = win32u ? GetProcAddress(win32u, "NtUserGetRawInputData") : nullptr;
        if (!reg || !get)
        {
            spdlog::warn("MouseLook: RegisterRawInputDevices is shimmed by {} and win32u is unavailable",
                         std::filesystem::path(path).filename().string());
            return;
        }

        pRegisterRawInputDevices = reinterpret_cast<decltype(pRegisterRawInputDevices)>(reg);
        Mouse::pGetRawInputData  = reinterpret_cast<decltype(Mouse::pGetRawInputData)>(get);
        spdlog::info("MouseLook: RegisterRawInputDevices is shimmed by {}, using win32u directly",
                     std::filesystem::path(path).filename().string());
    }

    BOOL Register(const RAWINPUTDEVICE& device)
    {
        return pRegisterRawInputDevices(&device, 1, sizeof(device));
    }

    // Time_UpdateFrameDelta runs every simulation step, which the mouselook path cannot offer - it
    // only runs inside a mission. The step is skipped while paused, so window.cpp drives it too.
    void __fastcall FrameTick(void* timing, void*)
    {
        shFrameTick.thiscall<void>(timing);
        Mouse::UpdateRawInput();
    }

    // Both halves are unconditional on focus - the delta comes from GetCursorPos and
    // Input_CenterCursor warps the pointer back - so left running behind another window this drives
    // the camera from, and then pins, a pointer the player is using elsewhere.
    void __fastcall MouseLookUpdate(void* input, void*)
    {
        if (!Mouse::bActive)
        {
            bResync = true;
            return;
        }

        shMouseLook.thiscall<void>(input);
    }

    // edi and ebp hold the cursor delta with the carry folded in, mirrored to the stack slots the
    // two multiplies read. The frozen cursor makes the engine's own delta zero, so adding the raw
    // counts leaves the carry doing what it did before.
    void MouseLookDelta(SafetyHookContext& ctx)
    {
        // The remembered centre survives the skipped frames, so the first frame back measures from
        // it to wherever the pointer was left. Spending that delta lets the recentre resynchronise.
        if (bResync)
        {
            bResync = false;
            InterlockedExchange(&Mouse::rawX, 0);
            InterlockedExchange(&Mouse::rawY, 0);
            ctx.edi = 0;
            ctx.ebp = 0;
            *reinterpret_cast<int32_t*>(ctx.esp + DeltaX) = 0;
            *reinterpret_cast<int32_t*>(ctx.esp + DeltaY) = 0;
            return;
        }

        if (!Mouse::bRawActive)
            return;

        ctx.edi += InterlockedExchange(&Mouse::rawX, 0);
        ctx.ebp += InterlockedExchange(&Mouse::rawY, 0);

        *reinterpret_cast<int32_t*>(ctx.esp + DeltaX) = static_cast<int32_t>(ctx.edi);
        *reinterpret_cast<int32_t*>(ctx.esp + DeltaY) = static_cast<int32_t>(ctx.ebp);
    }

    // The delta is already per-frame and the engine scales it again by min(frameDelta * 40, 1.0),
    // so the turn rate comes out proportional to the frame rate: 0.40 of intended at 100 FPS, 0.148
    // at 270. Below 40 FPS the clamp hides it, which was every machine of 1998, and 1.0 is the value
    // it produced there.
    void MouseLookScale(SafetyHookContext& ctx)
    {
        float* scale = reinterpret_cast<float*>(ctx.esp + ScaleSlot);

        if (!bLogged)
        {
            uint8_t* app = *reinterpret_cast<uint8_t**>(Mouse::AppObject);
            uint8_t* timing = app ? *reinterpret_cast<uint8_t**>(app + 0x20) : nullptr;
            const float delta = timing ? *reinterpret_cast<float*>(timing + TimingFrameDelta) : 0.0f;

            spdlog::info("MouseLook: stock scale {:.3f} at {:.4f}s ({:.0f} FPS) -> {:.3f}",
                         *scale, delta, delta > 0.0f ? 1.0f / delta : 0.0f,
                         static_cast<float>(fLookSensitivity));
            bLogged = true;
        }

        *scale = fLookSensitivity;
    }
}

// RIDEV_NOLEGACY stops Windows applying pointer acceleration and rounding to whole screen pixels
// before GetCursorPos sees the motion, and stops the legacy messages that went with it - 80% of
// wall time inside PeekMessage at 8000 Hz. The shell and a pause menu are driven by those messages,
// so the registration follows the action view.
void Mouse::UpdateRawInput()
{
    if (bRawRefused)
        return;

    const bool wanted = Mouse::OwnsTheCursor();
    if (wanted == Mouse::bRawActive)
        return;

    HWND window = Mouse::Window();
    if (!window)
        return;

    RAWINPUTDEVICE device{};
    device.usUsagePage = 0x01;
    device.usUsage     = 0x02;
    device.dwFlags     = wanted ? RIDEV_NOLEGACY : RIDEV_REMOVE;
    device.hwndTarget  = wanted ? window : nullptr;

    if (!Register(device))
    {
        // Latch, or this repeats every frame for the rest of the mission.
        bRawRefused = true;
        spdlog::error("MouseLook: raw input {} failed ({})", wanted ? "register" : "remove",
                      GetLastError());
        return;
    }

    InterlockedExchange(&Mouse::rawX, 0);
    InterlockedExchange(&Mouse::rawY, 0);
    Mouse::bRawActive = wanted;

    // Once each way: this toggles on every alt-tab and every mission entry.
    static bool logged[2]{};
    if (!logged[wanted])
    {
        logged[wanted] = true;
        spdlog::info("MouseLook: raw input {}", wanted ? "on, legacy mouse messages off" : "off");
    }
}

FEATURE(Game, MouseLook)
{
    auto update = hook::pattern("A1 ? ? ? ? 83 EC 14 53 55 56 8B F1 8B 48 10 57 "
                                "8B 91 58 03 00 00");
    if (update.empty())
    {
        spdlog::error("MouseLook: mouselook update not found");
        return;
    }

    // +0xa5 is the FLD beginning the scale calculation, the last point at which the delta registers
    // and their stack copies are settled and unread; +0xf0 is the FILD before the scale is read.
    // Both are nine bytes clear of any branch.
    shMouseLook  = Memory::Hook(update.get_first(), MouseLookUpdate);
    shMouseDelta = Memory::MidHook(update.get_first(0xa5), MouseLookDelta);
    shMouseScale = Memory::MidHook(update.get_first(0xf0), MouseLookScale);
    if (!shMouseLook || !shMouseDelta || !shMouseScale)
    {
        spdlog::error("MouseLook: hook installation failed");
        return;
    }

    spdlog::info("MouseLook: frame-time scaling removed, sensitivity {:.2f}",
                 static_cast<float>(fLookSensitivity));

    ResolveRawInput();

    auto frame = hook::pattern("A1 ? ? ? ? 8B 15 ? ? ? ? 83 EC 08 8B 40 78 3B C2 76 02 "
                               "8B C2 8B 51 40");
    if (frame.empty())
    {
        spdlog::error("MouseLook: frame tick not found, raw input unavailable");
        return;
    }

    shFrameTick = Memory::Hook(frame.get_first(), FrameTick);
    if (!shFrameTick)
        spdlog::error("MouseLook: frame tick hook failed, raw input unavailable");
}
