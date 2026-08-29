#include "stdafx.h"
#include "common.hpp"
#include "config.hpp"
#include "feature.hpp"
#include "hud.hpp"

static Config::Float fHudScale("HUD", "Scale", 0.75f);

namespace
{
    using Game::AppObject;

    constexpr ptrdiff_t AppDisplay     = 0x04;
    constexpr ptrdiff_t AppGameMode    = 0x08;
    constexpr ptrdiff_t GameplayWidth  = 0x14;
    constexpr ptrdiff_t GameplayHeight = 0x18;
    constexpr ptrdiff_t CurrentWidth   = 0x1c;
    constexpr ptrdiff_t CurrentHeight  = 0x20;
    constexpr ptrdiff_t GameModeCurrent = 0xcc;
    constexpr ptrdiff_t GameModeScreen  = 0xd4;
    constexpr int       MissionState    = 5;
    constexpr int       NetMissionState = 6;

    constexpr ptrdiff_t WidgetX          = 0x10;
    constexpr ptrdiff_t WidgetY          = 0x14;
    constexpr ptrdiff_t WidgetVisible    = 0x1c;
    constexpr ptrdiff_t ScreenChildCount = 0x20;
    constexpr ptrdiff_t ScreenChildren   = 0x24;
    constexpr ptrdiff_t ScreenView       = 0x28;
    constexpr ptrdiff_t ScreenMap        = 0x34;
    constexpr ptrdiff_t MapUnitsPerPixel = 0x54;
    constexpr ptrdiff_t MapPixelsPerUnit = 0x58;
    constexpr ptrdiff_t ViewProgressBar  = 0x64;
    constexpr ptrdiff_t ProgressBarY     = 0x0c;
    constexpr int       ProgressBarRise  = 30;
    constexpr int       BlockMargin      = 48;

    // The message list is blitted at a literal x of 8 from y 8, a line at a time. Screen-space
    // blits that never touched the HUD block, which is why they alone stayed at 1x.
    constexpr int       MessageX         = 8;
    constexpr int       MessageY         = 8;
    constexpr int       MessageStep      = 0x15;
    constexpr ptrdiff_t ScreenBackdropH  = 0x1dc;
    constexpr ptrdiff_t ScreenBackdropW  = 0x1e0;
    constexpr ptrdiff_t ScreenBackdropX  = 0x1e4;
    constexpr ptrdiff_t ScreenBackdropY  = 0x1e8;
    constexpr ptrdiff_t ScreenBackdrop   = 0x218;
    constexpr ptrdiff_t ScreenFullMap    = 0x1f0;

    using Game::ScreenSurface;

    constexpr int DesignHeight = 480;

    // Below one physical pixel per design pixel there is nothing to gain, so the block stays 1:1.
    float HudFactor(int height)
    {
        const float factor = static_cast<float>(height) / DesignHeight * fHudScale;
        return factor > 1.0f ? factor : 1.0f;
    }

    constexpr size_t VtblSetSize          = 2;
    constexpr size_t VtblInvalidate       = 10;
    constexpr size_t VtblUpdateRenderRect = 15;

    // Tick dimensions and center dot use float constants; the ring radius is angle-based.
    // Repoint the draw's loads instead of changing the shared 1.0 pool value.
    struct ReticleConstant
    {
        ptrdiff_t offset;
        float     stock;
        float     value;
    };

    ReticleConstant ReticleConstants[]
    {
        { 0x00,   1.0f, 0.0f },
        { 0x18, -10.0f, 0.0f },
        { 0x1c,  -1.0f, 0.0f },
        { 0x20,  10.0f, 0.0f },
        { 0x24,   8.0f, 0.0f },
        { 0x28,  -8.0f, 0.0f },
    };

    SafetyHookInline shActionScreenCtor{};
    SafetyHookInline shViewDraw{};
    SafetyHookInline shUpdateRenderRect{};
    SafetyHookInline shToggleFullMap{};
    SafetyHookMid    shDrawChildren{};

    int(__fastcall* BlitNoScale)(void*, void*, void*, int, int, int) = nullptr;
    void(__fastcall* DrawNetPanels)(void*, void*) = nullptr;

    template <typename Fn>
    Fn Method(void* object, size_t slot)
    {
        return reinterpret_cast<Fn>((*reinterpret_cast<void***>(object))[slot]);
    }

    uint8_t* ActionScreen()
    {
        uint8_t* app = *reinterpret_cast<uint8_t**>(AppObject);
        uint8_t* gameMode = app ? *reinterpret_cast<uint8_t**>(app + AppGameMode) : nullptr;
        if (!gameMode)
            return nullptr;

        const int state = *reinterpret_cast<int*>(gameMode + GameModeCurrent);
        if (state != MissionState && state != NetMissionState)
            return nullptr;

        return *reinterpret_cast<uint8_t**>(gameMode + GameModeScreen);
    }

    // The interaction bar is pinned to the render rect's bottom minus 30, which stock was the HUD's
    // top. With the view the whole display it lands over the HUD, so it is pinned to the block's top
    // instead and the transform's margin above the block carries it up with the HUD.
    void PinProgressBar(uint8_t* screen, uint8_t* view)
    {
        uint8_t* bar = *reinterpret_cast<uint8_t**>(view + ViewProgressBar);
        if (bar)
            *reinterpret_cast<int*>(bar + ProgressBarY) =
                *reinterpret_cast<int*>(screen + ScreenBackdropY) + *reinterpret_cast<int*>(screen + WidgetY) - ProgressBarRise;
    }

    // Stock subtracts the 120-pixel HUD strip from the display, then clamps the view to 16:9 within it.
    // This produces the black band and side bars. The + key already expands the view to the full display,
    // do the same while keeping the HUD.
    void* __fastcall ActionScreenCtor(uint8_t* self, void*, void* a, void* b)
    {
        void* screen = shActionScreenCtor.thiscall<void*>(self, a, b);

        uint8_t* app = *reinterpret_cast<uint8_t**>(AppObject);
        uint8_t* display = app ? *reinterpret_cast<uint8_t**>(app + AppDisplay) : nullptr;
        void* view = *reinterpret_cast<void**>(self + ScreenView);
        if (!display || !view)
            return screen;

        const int width  = *reinterpret_cast<int*>(display + GameplayWidth);
        const int height = *reinterpret_cast<int*>(display + GameplayHeight);

        Method<void(__fastcall*)(void*, void*, int, int)>(view, VtblSetSize)(view, nullptr, width, height);
        Method<void(__fastcall*)(void*, void*)>(view, VtblUpdateRenderRect)(view, nullptr);
        PinProgressBar(self, reinterpret_cast<uint8_t*>(view));

        for (auto& constant : ReticleConstants)
            constant.value = constant.stock * HudFactor(height);

        // The action screen is rebuilt on every mission and every return from the pause menu.
        static int lastLogged = 0;
        if (lastLogged != height)
        {
            lastLogged = height;
            spdlog::info("Hud: 3D view {}x{}, HUD drawn over it at {:.2f}x", width, height, HudFactor(height));
        }

        return screen;
    }

    // The map uses a fixed 10 world units per pixel. In the panel, blits scale with the HUD, fullscreen
    // maps use physical pixels, so the map shrinks at higher resolutions. Zoom and marker radius derive
    // from this value, so scale it only when toggling fullscreen.
    void __fastcall ToggleFullMap(uint8_t* self, void*)
    {
        const bool wasFull = *(self + ScreenFullMap) != 0;
        shToggleFullMap.thiscall<void>(self);
        if (wasFull == (*(self + ScreenFullMap) != 0))
            return;

        uint8_t* app = *reinterpret_cast<uint8_t**>(AppObject);
        uint8_t* display = app ? *reinterpret_cast<uint8_t**>(app + AppDisplay) : nullptr;
        uint8_t* map = *reinterpret_cast<uint8_t**>(self + ScreenMap);
        if (!display || !map)
            return;

        const float height = static_cast<float>(*reinterpret_cast<int*>(display + GameplayHeight));
        float& unitsPerPixel = *reinterpret_cast<float*>(map + MapUnitsPerPixel);
        unitsPerPixel *= wasFull ? height / DesignHeight : DesignHeight / height;
        *reinterpret_cast<float*>(map + MapPixelsPerUnit) = 1.0f / unitsPerPixel;
    }

    // The screen blits its backdrop before child draws, so the full-height view paints over it.
    // Blit it again after the view to restore it under the panels, which draw next in child order.
    // Network panels are drawn onto the backdrop instead of as children, so redraw them after the blit.
    void __fastcall ViewDraw(uint8_t* view, void*)
    {
        shViewDraw.thiscall<void>(view);

        uint8_t* screen = ActionScreen();
        if (!screen || *reinterpret_cast<void**>(screen + ScreenView) != view)
            return;

        void* backdrop = *reinterpret_cast<void**>(screen + ScreenBackdrop);
        if (!backdrop)
            return;

        BlitNoScale(backdrop, nullptr, reinterpret_cast<void*>(ScreenSurface),
                    *reinterpret_cast<int*>(screen + ScreenBackdropX) + *reinterpret_cast<int*>(screen + WidgetX),
                    *reinterpret_cast<int*>(screen + ScreenBackdropY) + *reinterpret_cast<int*>(screen + WidgetY),
                    0);
        DrawNetPanels(screen, nullptr);
    }

    void __fastcall UpdateRenderRect(uint8_t* view, void*)
    {
        shUpdateRenderRect.thiscall<void>(view);

        // Null while the constructor runs, the constructor hook pins the bar itself.
        uint8_t* screen = ActionScreen();
        if (screen && *reinterpret_cast<void**>(screen + ScreenView) == view)
            PinProgressBar(screen, view);
    }

    // Widgets blit only while their repaint count is above zero, which the engine can afford
    // because nothing else touches their pixels. The view now renders under them every frame.
    void InvalidateChildren(SafetyHookContext& ctx)
    {
        uint8_t* screen = reinterpret_cast<uint8_t*>(ctx.esi);
        void** children = *reinterpret_cast<void***>(screen + ScreenChildren);
        const int count = *reinterpret_cast<int*>(screen + ScreenChildCount);

        for (int i = 0; i < count; ++i)
        {
            void* child = children[i];
            if (child && *(reinterpret_cast<uint8_t*>(child) + WidgetVisible))
                Method<void(__fastcall*)(void*, void*)>(child, VtblInvalidate)(child, nullptr);
        }
    }
}

// The HUD uses physical pixels around the backdrop origin, so the block stays 640x120 at any resolution.
// Blits inside it scale by height / 480 around a bottom-centred origin, matching the shell's scale.
// This keeps the HUD at its intended 640x480 size.
bool HudScaleRect(const int* rect, int* scaled)
{
    uint8_t* screen = ActionScreen();
    uint8_t* app = *reinterpret_cast<uint8_t**>(AppObject);
    uint8_t* display = app ? *reinterpret_cast<uint8_t**>(app + AppDisplay) : nullptr;
    if (!screen || !display)
        return false;

    const int height = *reinterpret_cast<int*>(display + CurrentHeight);
    const float factor = HudFactor(height);
    if (factor == 1.0f)
        return false;

    if (rect[0] == MessageX && rect[1] >= MessageY && (rect[1] - MessageY) % MessageStep == 0)
    {
        scaled[0] = static_cast<int>(std::floor(rect[0] * factor));
        scaled[1] = static_cast<int>(std::floor(rect[1] * factor));
        scaled[2] = static_cast<int>(std::ceil(rect[2] * factor));
        scaled[3] = static_cast<int>(std::ceil(rect[3] * factor));
        return true;
    }

    const int blockX = *reinterpret_cast<int*>(screen + ScreenBackdropX) + *reinterpret_cast<int*>(screen + WidgetX);
    const int blockY = *reinterpret_cast<int*>(screen + ScreenBackdropY) + *reinterpret_cast<int*>(screen + WidgetY);
    const int blockW = *reinterpret_cast<int*>(screen + ScreenBackdropW);
    const int blockH = *reinterpret_cast<int*>(screen + ScreenBackdropH);
    if (rect[0] < blockX || rect[1] < blockY - BlockMargin || rect[2] > blockX + blockW || rect[3] > blockY + blockH)
        return false;

    const int originX = (*reinterpret_cast<int*>(display + CurrentWidth) - static_cast<int>(blockW * factor)) / 2;
    const int originY = height - static_cast<int>(blockH * factor);

    scaled[0] = static_cast<int>(std::floor((rect[0] - blockX) * factor)) + originX;
    scaled[1] = static_cast<int>(std::floor((rect[1] - blockY) * factor)) + originY;
    scaled[2] = static_cast<int>(std::ceil((rect[2] - blockX) * factor)) + originX;
    scaled[3] = static_cast<int>(std::ceil((rect[3] - blockY) * factor)) + originY;
    return true;
}

FEATURE(Game, HudOverlay)
{
    auto ctor = hook::pattern("6A FF 68 ? ? ? ? 64 A1 00 00 00 00 50 64 89 25 00 00 00 00 83 EC 20 "
                              "8B 44 24 34 53 55 56 8B F1 57 8B 4C 24 40 50 51 8B CE 89 74 24 18 "
                              "E8 ? ? ? ? C7 06 ? ? ? ?");

    auto draw = hook::pattern("B9 ? ? ? ? E8 ? ? ? ? 8B CE E8 ? ? ? ? 8B CE E8 ? ? ? ? "
                              "8B 8E C4 01 00 00");
    auto viewDraw = hook::pattern("6A FF 68 ? ? ? ? 64 A1 00 00 00 00 50 64 89 25 00 00 00 00 83 EC 2C "
                                  "53 55 56 57 8B F1 FF 15 ? ? ? ? 8D 7E 50");

    auto renderRect = hook::pattern("53 55 56 57 8B F9 B8 B7 60 0B B6 8B 5F 2C 8B 77 08");
    auto blit = hook::pattern("56 57 8B 73 04 85 F6 0F 84 ? ? ? ? 8B 94 24 84 00 00 00 "
                              "8B 8C 24 88 00 00 00 8B 82 8C 00 00 00");

    auto netPanels = hook::pattern("81 EC E0 02 00 00 53 55 8B E9 56 57 8A 85 F3 01 00 00 84 C0 75 0A");
    auto fullMap = hook::pattern("53 56 8B F1 57 8A 86 F0 01 00 00 84 C0 0F 84 ? ? ? ? 8A 86 F3 01 00 00");
    if (ctor.empty() || draw.empty() || viewDraw.empty() || renderRect.empty() || blit.empty() ||
        netPanels.empty() || fullMap.empty())
    {
        spdlog::error("Hud: action screen code not found");
        return;
    }

    shActionScreenCtor = Memory::Hook(ctor.get_first(), ActionScreenCtor);
    shViewDraw         = Memory::Hook(viewDraw.get_first(), ViewDraw);
    shUpdateRenderRect = Memory::Hook(renderRect.get_first(), UpdateRenderRect);
    shToggleFullMap    = Memory::Hook(fullMap.get_first(), ToggleFullMap);
    shDrawChildren     = Memory::MidHook(draw.get_first(10), InvalidateChildren);
    BlitNoScale        = reinterpret_cast<decltype(BlitNoScale)>(blit.get_first(-6));
    DrawNetPanels      = reinterpret_cast<decltype(DrawNetPanels)>(netPanels.get_first(-21));
    if (!shActionScreenCtor || !shViewDraw || !shUpdateRenderRect || !shToggleFullMap || !shDrawChildren)
    {
        spdlog::error("Hud: hook installation failed");
        return;
    }

    for (auto& constant : ReticleConstants)
    {
        const uintptr_t pooled = Game::ReticlePool + constant.offset;
        const std::string load = std::format("D8 25 {:02X} {:02X} {:02X} {:02X}",
                                             pooled & 0xff, (pooled >> 8) & 0xff,
                                             (pooled >> 16) & 0xff, (pooled >> 24) & 0xff);

        auto sites = hook::pattern(load);
        if (sites.empty())
        {
            spdlog::error("Hud: reticle constant {} not found", constant.stock);
            return;
        }

        constant.value = constant.stock;
        sites.for_each_result([&](hook::pattern_match match)
        {
            injector::WriteMemory<uint32_t>(match.get<void>(2), reinterpret_cast<uint32_t>(&constant.value), true);
        });
    }

    spdlog::info("Hud: active");
}
