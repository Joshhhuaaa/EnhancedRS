#include "stdafx.h"
#include "common.hpp"
#include "feature.hpp"
#include "mouse.hpp"

namespace
{
    // Mouse buttons use the same key-code space: left=0xbc, right=0xbd, middle=0xbe.
    // The key handler stops at 0xbb, leaving 0xbf/0xc0 free. sherman.kmp is a 256-entry table
    // indexed by these codes, so the bindings need no format change.
    constexpr uint8_t ExtraCodes[]{ 0xbf, 0xc0 };

    constexpr int EventKeyDown = 2;
    constexpr int EventKeyUp   = 3;

    constexpr ptrdiff_t InputKeyCode = 0x0c;

    void(__fastcall* DispatchInput)(void*, void*, int, int, int) = nullptr;
    void(__fastcall* AssignString)(void*, void*, const char*) = nullptr;

    SafetyHookInline shKeyName{};

    // The remap lookup covers 0x80..0xbe plus digits and letters, codes outside it keep "-----".
    // remap_controls.txt has no slots past middle mouse, so set names for the added buttons here.
    void __fastcall KeyName(void* names, void*, int code, void* destination)
    {
        if (code == ExtraCodes[0] || code == ExtraCodes[1])
        {
            AssignString(destination, nullptr, code == ExtraCodes[0] ? "MOUSE 4" : "MOUSE 5");
            return;
        }

        shKeyName.thiscall<void>(names, code, destination);
    }
}

void Mouse::SendExtraButton(int button, bool down)
{
    if (!DispatchInput)
        return;

    auto app = *reinterpret_cast<uint8_t**>(Mouse::AppObject);
    auto input = app ? *reinterpret_cast<uint8_t**>(app + Mouse::AppInput) : nullptr;
    if (!input)
        return;

    input[InputKeyCode] = ExtraCodes[button];
    DispatchInput(input, nullptr, down ? EventKeyDown : EventKeyUp, 0, 0);
}

FEATURE(Game, ExtraMouseButtons)
{
    auto thunk = hook::pattern("6A 00 6A 00 6A 02 C6 41 0C BC E8");
    if (thunk.empty())
    {
        spdlog::error("ExtraMouseButtons: left button thunk not found");
        return;
    }

    auto call = thunk.get_first<uint8_t>(10);
    DispatchInput = reinterpret_cast<decltype(DispatchInput)>(call + 5 + *reinterpret_cast<int32_t*>(call + 1));

    auto keyName = hook::pattern("56 57 8B 7C 24 10 8B F1 68 ? ? ? ? 8B CF E8");
    if (keyName.empty())
    {
        spdlog::warn("ExtraMouseButtons: key name lookup not found, the two will show as \"-----\"");
    }
    else
    {
        auto assign = keyName.get_first<uint8_t>(15);
        AssignString = reinterpret_cast<decltype(AssignString)>(assign + 5 + *reinterpret_cast<int32_t*>(assign + 1));

        shKeyName = Memory::Hook(keyName.get_first(), KeyName);
        if (!shKeyName)
            spdlog::error("ExtraMouseButtons: key name hook failed");
    }

    spdlog::info("ExtraMouseButtons: mouse 4/5 send 0x{:02x}/0x{:02x}, {}",
                ExtraCodes[0], ExtraCodes[1], shKeyName ? "remap" : "nowhere");
}
