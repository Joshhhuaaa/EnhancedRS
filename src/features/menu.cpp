#include "stdafx.h"
#include "common.hpp"
#include "feature.hpp"
#include "hud.hpp"

namespace
{
    using Game::AppObject;

    constexpr ptrdiff_t AppDisplay  = 0x04;
    constexpr ptrdiff_t AppGameMode = 0x08;

    constexpr ptrdiff_t GameModeCurrent = 0xcc;
    constexpr ptrdiff_t GameModeScreen  = 0xd4;
    constexpr int       MissionState    = 5;
    // Multiplayer: the net layer loads the Sim itself and enters 6, not 5.
    constexpr int       NetMissionState = 6;
    constexpr ptrdiff_t ActionScreenViewShown = 0x1ee;

    // Options +0x334 is a string object holding VideoResolution; its text pointer is at +4.
    constexpr ptrdiff_t OptionsResolution = 0x338;

    constexpr ptrdiff_t GameplayWidth  = 0x14;
    constexpr ptrdiff_t GameplayHeight = 0x18;
    constexpr ptrdiff_t CurrentWidth   = 0x1c;
    constexpr ptrdiff_t CurrentHeight  = 0x20;

    // The screen the shell draws into, and the primary the loading screens draw to directly.
    using Game::ScreenSurface;
    using Game::PrimarySurface;
    constexpr ptrdiff_t SurfaceInterface = 0x04;
    constexpr ptrdiff_t SurfaceWidth     = 0x94;
    constexpr ptrdiff_t SurfaceHeight    = 0x98;

    constexpr int DesignWidth  = 640;
    constexpr int DesignHeight = 480;

    constexpr int VersionX = 2;

    constexpr ptrdiff_t CursorBitmaps = 0x0c;
    constexpr ptrdiff_t CursorIndex   = 0x10;
    constexpr ptrdiff_t CursorSaves   = 0x14;
    constexpr ptrdiff_t CursorSaved   = 0x1c;
    constexpr ptrdiff_t CursorSaveX   = 0x20;
    constexpr ptrdiff_t CursorSaveY   = 0x28;
    constexpr ptrdiff_t CursorSaveW   = 0x30;
    constexpr ptrdiff_t CursorSaveH   = 0x38;
    constexpr ptrdiff_t CursorBuffer  = 0x40;

    // Stock save-under region, surfaces are 8x larger for cursor scaling up to 3840px
    constexpr int SaveWidth  = 0x44;
    constexpr int SaveHeight = 0x40;
    constexpr int SaveSurfaceWidth  = SaveWidth * 8;
    constexpr int SaveSurfaceHeight = SaveHeight * 8;

    uint8_t* pOptions = nullptr;
    int lastLogged = 0;
    bool bDrawingCursor = false;
    int scratch[4]{};
    bool bBarsLogged = false;

    SafetyHookInline shReadOptions{};
    SafetyHookInline shInitRenderer{};
    SafetyHookInline shSetCursorPosition{};
    SafetyHookInline shDrawCursor{};
    SafetyHookInline shPresentFrame{};
    SafetyHookInline shCreateViewport{};
    SafetyHookInline shBlitToRect{};
    SafetyHookInline shBlitNoScale{};

    int(__fastcall* BlitToRectFn)(void*, void*, void*, const int*, int) = nullptr;
    int(__fastcall* BlitFromRectToRect)(void*, void*, const int*, void*, const int*, int) = nullptr;

    void(__fastcall* ColorFill)(void*, void*, const int*, uint32_t) = nullptr;
    SafetyHookMid    shBlit[5]{};
    SafetyHookMid    shHaze[5]{};
    SafetyHookMid    shPlanPick{};
    void* hazeInterface = nullptr;

    // VideoResolution is parsed only on mission entry, after the app constructor stores its globals.
    // At boot the display still holds 640x480, so read the resolution directly from the options object.
    void RefreshGameplayResolution(uint8_t* display)
    {
        if (!pOptions)
            return;

        const char* resolution = *reinterpret_cast<const char**>(pOptions + OptionsResolution);
        int width = 0;
        int height = 0;
        if (!resolution || sscanf_s(resolution, "%dx%d", &width, &height) != 2 || width <= 0 || height <= 0)
            return;

        *reinterpret_cast<int*>(display + GameplayWidth)  = width;
        *reinterpret_cast<int*>(display + GameplayHeight) = height;

        if (width != lastLogged)
        {
            spdlog::info("MenuResolution: VideoResolution \"{}\" -> {}x{}", resolution, width, height);
            lastLogged = width;
        }
    }

    // 640x480 is the shell's design space. Scale by height and center it on wider displays.
    bool MenuTransform(float& factor, int& originX)
    {
        uint8_t* app = *reinterpret_cast<uint8_t**>(AppObject);
        uint8_t* gameMode = app ? *reinterpret_cast<uint8_t**>(app + AppGameMode) : nullptr;
        uint8_t* display  = app ? *reinterpret_cast<uint8_t**>(app + AppDisplay)  : nullptr;
        if (!gameMode || !display)
            return false;

        // The state flips before GameMode_Enter builds the screen, so a mission's loading screen
        // runs under the mission state with no screen object yet. Once there is one, the action
        // screen draws either the world or a sub-screen laid out in the design space like the shell,
        // and ActionScreen+0x1ee tells the two apart.
        const int state = *reinterpret_cast<int*>(gameMode + GameModeCurrent);
        uint8_t* screen = *reinterpret_cast<uint8_t**>(gameMode + GameModeScreen);
        if (screen && (state == MissionState || state == NetMissionState) && screen[ActionScreenViewShown])
            return false;

        const int width  = *reinterpret_cast<int*>(display + CurrentWidth);
        const int height = *reinterpret_cast<int*>(display + CurrentHeight);
        if (height <= DesignHeight)
            return false;

        factor  = static_cast<float>(height) / DesignHeight;
        originX = static_cast<int>((width - DesignWidth * factor) * 0.5f);
        return true;
    }

    // Shell draws end at IDirectDrawSurface::Blt with the destination rect in screen pixels.
    // Skip offscreen surfaces and cursor/save-under blits.
    void ScaleShellBlit(SafetyHookContext& ctx)
    {
        // Glyphs and cursor save-under use offscreen surfaces with widget-like rectangles.
        if (bDrawingCursor ||
            *reinterpret_cast<void**>(ctx.esp) != *reinterpret_cast<void**>(ScreenSurface + SurfaceInterface))
            return;

        // Dialog haze tiles use widget-like rectangles; HazeLoop records the source.
        if (hazeInterface && *reinterpret_cast<void**>(ctx.esp + 8) == hazeInterface)
            return;

        const int* rect = *reinterpret_cast<int**>(ctx.esp + 4);
        if (!rect)
            return;

        // Check the HUD first since it can overlap the design space at lower resolutions.
        if (HudScaleRect(rect, scratch))
        {
            *reinterpret_cast<int**>(ctx.esp + 4) = scratch;
            return;
        }

        float factor = 1.0f;
        int originX = 0;
        if (!MenuTransform(factor, originX))
            return;

        // Version text is positioned in screen pixels and anchored to the bottom.
        if (rect[0] == VersionX && rect[3] > DesignHeight)
        {
            scratch[0] = static_cast<int>(std::floor(rect[0] * factor)) + originX;
            scratch[1] = rect[3] - static_cast<int>(std::ceil((rect[3] - rect[1]) * factor));
            scratch[2] = scratch[0] + static_cast<int>(std::ceil((rect[2] - rect[0]) * factor));
            scratch[3] = rect[3];

            *reinterpret_cast<int**>(ctx.esp + 4) = scratch;
            return;
        }

        if (rect[0] < 0 || rect[1] < 0 || rect[2] > DesignWidth || rect[3] > DesignHeight)
            return;

        // Repoint instead of scaling in place; widgets reuse the rect for repeated draws.
        // Round outward to avoid seams.
        scratch[0] = static_cast<int>(std::floor(rect[0] * factor)) + originX;
        scratch[1] = static_cast<int>(std::floor(rect[1] * factor));
        scratch[2] = static_cast<int>(std::ceil(rect[2] * factor)) + originX;
        scratch[3] = static_cast<int>(std::ceil(rect[3] * factor));

        *reinterpret_cast<int**>(ctx.esp + 4) = scratch;
    }

    // Convert mouse coordinates from screen space to the 640x480 design space.
    void __fastcall SetCursorPosition(void* input, void*, const int* point)
    {
        float factor = 1.0f;
        int originX = 0;
        if (!MenuTransform(factor, originX))
            return shSetCursorPosition.thiscall<void>(input, point);

        const int design[2]{ static_cast<int>((point[0] - originX) / factor),
                             static_cast<int>(point[1] / factor) };

        return shSetCursorPosition.thiscall<void>(input, design);
    }

    // Cursor coordinates use the same design-space transform as the menu.
    // Scale the save-under region with the display; scaling only the cursor blit leaves too little
    // area to restore at higher resolutions, so the save surfaces and restore rect must scale too.
    void __fastcall DrawCursor(uint8_t* cursor, void*, uint8_t* dest, int x, int y)
    {
        float factor = 1.0f;
        int originX = 0;
        if (MenuTransform(factor, originX))
        {
            x = static_cast<int>(x * factor) + originX;
            y = static_cast<int>(y * factor);
        }

        const int width  = *reinterpret_cast<int*>(dest + SurfaceWidth);
        const int height = *reinterpret_cast<int*>(dest + SurfaceHeight);
        const int index  = *reinterpret_cast<int*>(cursor + CursorIndex);
        const float scale = static_cast<float>(height) / DesignHeight;
        if (scale <= 1.0f || index < 0 || x < 0 || y < 0 || x >= width || y >= height)
            return shDrawCursor.thiscall<void>(cursor, dest, x, y);

        uint8_t* bitmap = *reinterpret_cast<uint8_t**>(cursor + CursorBitmaps + index * 4);
        const int buffer = *reinterpret_cast<int*>(cursor + CursorBuffer);
        void* save = *reinterpret_cast<void**>(cursor + CursorSaves + buffer * 4);

        const int saveX = x & ~3;
        const int saveW = std::min({ (static_cast<int>(SaveWidth * scale) + 3) & ~3, width - saveX, SaveSurfaceWidth });
        const int saveH = std::min({ static_cast<int>(SaveHeight * scale) + 1, height - y, SaveSurfaceHeight });
        const int screenRect[4]{ saveX, y, saveX + saveW, y + saveH };
        const int saveRect[4]{ 0, 0, saveW, saveH };
        BlitFromRectToRect(dest, nullptr, screenRect, save, saveRect, 0);

        const int drawW = std::min(static_cast<int>(*reinterpret_cast<int*>(bitmap + SurfaceWidth) * scale), width - x);
        const int drawH = std::min(static_cast<int>(*reinterpret_cast<int*>(bitmap + SurfaceHeight) * scale), height - y);
        const int bitmapRect[4]{ 0, 0, static_cast<int>(drawW / scale), static_cast<int>(drawH / scale) };
        const int drawRect[4]{ x, y, x + drawW, y + drawH };
        BlitFromRectToRect(bitmap, nullptr, bitmapRect, dest, drawRect, 0);

        *reinterpret_cast<int*>(cursor + CursorSaveX + buffer * 4) = saveX;
        *reinterpret_cast<int*>(cursor + CursorSaveY + buffer * 4) = y;
        *reinterpret_cast<int*>(cursor + CursorSaveW + buffer * 4) = saveW;
        *reinterpret_cast<int*>(cursor + CursorSaveH + buffer * 4) = saveH;
        cursor[CursorSaved + buffer] = 1;
    }

    // The fourth loadscreen.bmp blit has no size test, so route it through the rect blit to apply
    // the same pillarbox transform as the other three.
    int __fastcall BlitNoScale(uint8_t* source, void*, void* dest, int x, int y, int mode)
    {
        float factor = 1.0f;
        int originX = 0;
        if (dest == reinterpret_cast<void*>(PrimarySurface) && MenuTransform(factor, originX))
        {
            const int width  = *reinterpret_cast<int*>(PrimarySurface + SurfaceWidth);
            const int height = *reinterpret_cast<int*>(PrimarySurface + SurfaceHeight);
            if (*reinterpret_cast<int*>(source + SurfaceWidth) >= DesignWidth / 2 &&
                *reinterpret_cast<int*>(source + SurfaceWidth) < width)
            {
                const int full[4]{ x, y, x + width, y + height };
                return BlitToRectFn(source, nullptr, dest, full, mode);
            }
        }

        return shBlitNoScale.thiscall<int>(source, dest, x, y, mode);
    }

    // Loading screens blit 4:3 art to the primary in full-display coordinates, so the rect is not
    // design-space scaled. Inset and clear it first to pillarbox the image safely.
    int __fastcall BlitToRect(void* source, void*, void* dest, int* rect, int mode)
    {
        int pillarboxed[4]{};
        float factor = 1.0f;
        int originX = 0;
        if (dest == reinterpret_cast<void*>(PrimarySurface) && MenuTransform(factor, originX))
        {
            const int width  = rect[2] - rect[0];
            const int inset  = (width - (rect[3] - rect[1]) * DesignWidth / DesignHeight) / 2;
            if (inset > 0)
            {
                ColorFill(dest, nullptr, rect, 0);

                pillarboxed[0] = rect[0] + inset;
                pillarboxed[1] = rect[1];
                pillarboxed[2] = rect[2] - inset;
                pillarboxed[3] = rect[3];
                rect = pillarboxed;
            }
        }

        return shBlitToRect.thiscall<int>(source, dest, rect, mode);
    }

    // The planning map builds its Direct3D viewport from the widget rect, bypassing the blit hooks.
    // Fullscreen viewports exceed the design space and are left unscaled, like oversized blit rects.
    void* __fastcall CreateViewport(void* renderer, void*, int x, int y, int width, int height)
    {
        float factor = 1.0f;
        int originX = 0;
        if (MenuTransform(factor, originX) &&
            x >= 0 && y >= 0 && x + width <= DesignWidth && y + height <= DesignHeight)
        {
            x      = static_cast<int>(x * factor) + originX;
            y      = static_cast<int>(y * factor);
            width  = static_cast<int>(width * factor);
            height = static_cast<int>(height * factor);
        }


        return shCreateViewport.thiscall<void*>(renderer, x, y, width, height);
    }

    // The pick unprojects the cursor - stored in the design space - through the viewport, which
    // CreateViewport has put in screen pixels. Renderer_ScreenToRay(out, viewport, point).
    void MapPlanPick(SafetyHookContext& ctx)
    {
        float factor = 1.0f;
        int originX = 0;
        if (!MenuTransform(factor, originX))
            return;

        float* point = *reinterpret_cast<float**>(ctx.esp + 8);
        point[0] = point[0] * factor + originX;
        point[1] = point[1] * factor;
    }

    // The pick point is stored in design space, but the viewport uses screen pixels.
    // Transform it to match the viewport before unprojecting.
    void __fastcall PresentFrame(void* present, void*)
    {
        // In a mission the bars would land on the world behind a pause menu or dialog.
        uint8_t* app = *reinterpret_cast<uint8_t**>(AppObject);
        uint8_t* gameMode = app ? *reinterpret_cast<uint8_t**>(app + AppGameMode) : nullptr;
        float factor = 1.0f;
        int originX = 0;
        const int state = gameMode ? *reinterpret_cast<int*>(gameMode + GameModeCurrent) : MissionState;
        if (MenuTransform(factor, originX) && originX > 0 &&
            state != MissionState && state != NetMissionState)
        {
            const int width  = *reinterpret_cast<int*>(ScreenSurface + SurfaceWidth);
            const int height = *reinterpret_cast<int*>(ScreenSurface + SurfaceHeight);
            const int left[4]{ 0, 0, originX, height };
            const int right[4]{ width - originX, 0, width, height };

            ColorFill(reinterpret_cast<void*>(ScreenSurface), nullptr, left, 0);
            ColorFill(reinterpret_cast<void*>(ScreenSurface), nullptr, right, 0);

            if (!bBarsLogged)
            {
                spdlog::info("MenuResolution: clearing {}px pillarbox bars on a {}x{} surface",
                             originX, width, height);
                bBarsLogged = true;
            }
        }

        bDrawingCursor = true;
        shPresentFrame.thiscall<void>(present);
        bDrawingCursor = false;
    }

    char __fastcall ReadOptions(uint8_t* options, void*)
    {
        pOptions = options;

        return shReadOptions.thiscall<char>(options);
    }

    int __fastcall InitRenderer(uint8_t* display, void*, int mode)
    {
        RefreshGameplayResolution(display);

        return shInitRenderer.thiscall<int>(display, mode);
    }

    // Every haze loop starts with the haze surface object still in EAX from the null test.
    void HazeLoop(SafetyHookContext& ctx)
    {
        if (auto haze = reinterpret_cast<uint8_t*>(ctx.eax))
            hazeInterface = *reinterpret_cast<void**>(haze + SurfaceInterface);
    }

    struct BlitSite
    {
        const char* pattern;
        ptrdiff_t   call;
    };

    constexpr BlitSite BlitSites[]
    {
        { "8D 4C 24 18 56 51 52 FF 50 14",              7 },  // Surface::BlitNoScale
        { "8B 56 04 52 8D 54 24 20 52 50 FF 51 14",    10 },  // Surface::BlitToRect
        { "8B 4B 04 51 8D 4C 24 20 51 50 FF 52 14",    10 },  // Surface::BlitFromRectToRect
        { "68 00 0C 00 01 6A 00 6A 00 52 50 FF 51 14", 11 },  // the color fill
        { "8B 49 04 51 8D 4C 24 34 51 50 FF 52 14",    10 },  // Font::DrawText
    };
}

FEATURE(Game, MenuResolution)
{
    // The display carries a hardcoded 640x480 at +0xc/+0x10 and the gameplay resolution at
    // +0x14/+0x18, and the renderer init picks between them on a mode argument that is 5 only while
    // a mission is loaded. Dropping the two tests leaves the gameplay pair selected everywhere.
    auto fullscreen = hook::pattern("83 F8 05 6A 10 75");
    auto windowed   = hook::pattern("83 F8 05 75 28 8B 56 18 8B 46 14");
    auto options    = hook::pattern("53 56 8B F1 8B 15 ? ? ? ? 8A 8E 08 02 00 00");
    auto init       = hook::pattern("64 A1 00 00 00 00 6A FF 68 ? ? ? ? 50 A1 ? ? ? ? "
                                    "64 89 25 00 00 00 00 83 EC 24 85 C0");
    auto cursorPos  = hook::pattern("A1 ? ? ? ? 8B 50 04 8A 42 24 84 C0 8B 44 24 04 74 0E");
    auto drawCursor = hook::pattern("81 EC 9C 00 00 00 53 55 56 8B F1 57 83 7E 10 FF");
    auto present    = hook::pattern("A1 ? ? ? ? 56 8B F1 8B 48 0C 8B 41 20 8B 49 24");
    auto fill       = hook::pattern("83 EC 64 8B D1 57 B9 19 00 00 00 33 C0 8D 7C 24 04 F3 AB");
    auto viewport   = hook::pattern("6A FF 68 ? ? ? ? 64 A1 00 00 00 00 50 64 89 25 00 00 00 00 "
                                    "51 53 56 8B D9 68 BC 00 00 00");
    auto blitNoScale = hook::pattern("83 EC 74 53 8B D9 56 57 8B 73 04 85 F6");
    auto blitToRect = hook::pattern("81 EC 84 00 00 00 53 55 56 8B F1 8B 9C 24 94 00 00 00 57");
    auto blitFromTo = hook::pattern("81 EC 84 00 00 00 53 8B D9 55 56 8B 43 04 57 85 C0 0F 84 ? ? ? ? "
                                    "8B 84 24 98 00 00 00");
    // Cursor ctor, the DDSURFACEDESC for the two save-under surfaces: flags, caps, width, height.
    auto cursorSave = hook::pattern("C7 44 24 40 07 00 00 00 C7 84 24 A4 00 00 00 40 08 00 00 "
                                    "C7 44 24 48 44 00 00 00 C7 44 24 44 40 00 00 00");
    // The planning map's pick, at its call into Renderer_ScreenToRay.
    auto planPick   = hook::pattern("53 B9 ? ? ? ? D9 5C 24 34 E8");
    // The three dialog classes centre their bitmap on the display's live size in both constructor
    // and Draw, so above 640x480 they land outside the design space, unscaled and out of reach of
    // the mapped cursor. Pointing the four reads at the design pair at +0xc/+0x10 puts them back.
    // The Draw code is byte-identical in all three.
    auto dialogDraw  = hook::pattern("8B 73 34 8B 51 04 8B 42 1C 8B AE 94 00 00 00 2B C5 99 2B C2 D1 F8 "
                                     "89 44 24 10 8B 41 04 8B AE 98 00 00 00 8D 4C 24 10 8B 40 20");
    auto dialogCtor  = hook::pattern("8B A9 94 00 00 00 8B 40 1C 2B C5 99 2B C2 D1 F8 89 44 24 18 "
                                     "8B 57 04 8B A9 98 00 00 00 8B CE 8B 42 20");
    auto yesNoCtor   = hook::pattern("8B 4E 34 8B 57 04 8B 42 1C 8B 91 94 00 00 00 2B C2 99 2B C2 D1 F8 "
                                     "89 44 24 20 8B 47 04 8B A9 98 00 00 00 8D 4C 24 20 8B 40 20");
    auto okayCtor    = hook::pattern("8B 95 94 00 00 00 8B 41 1C 2B C2 99 2B C2 8B F8 8B 41 20 "
                                     "8B 8D 98 00 00 00");
    // The in-mission quick menu is a fifth class centring on the gameplay pair at +0x14/+0x18 -
    // identical arithmetic, one Draw and two constructors, its multiplayer twin sharing the bytes.
    // Every one of these dialogs tiles a haze over the display.
    auto quickCtor   = hook::pattern("8B 41 14 2B C2 99 2B C2 D1 F8 89 44 24 20 8B 41 18 2B C3 BB ? ? ? ? "
                                     "99 2B C2 8D 56 4C");
    auto quickDraw   = hook::pattern("8B 42 14 2B C3 99 2B C2 D1 F8 89 44 24 10 8B 41 04 8B BE 98 00 00 00 "
                                     "8D 4C 24 10 8B 40 18");
    auto hazeDialogs = hook::pattern("8B 0D ? ? ? ? 33 FF 8B 41 04 8B 50 1C 85 D2 0F 8E");
    auto hazeQuick   = hook::pattern("8B 0D ? ? ? ? 33 FF 8B 41 04 8B 50 14 85 D2 0F 8E");
    if (dialogDraw.size() != 3 || dialogCtor.empty() || yesNoCtor.empty() || okayCtor.empty() ||
        quickCtor.size() != 2 || quickDraw.empty() || hazeDialogs.size() != 4 || hazeQuick.empty())
    {
        spdlog::error("MenuResolution: dialog centring code not found");
        return;
    }

    if (fullscreen.empty() || windowed.empty() || options.empty() || init.empty() ||
        cursorPos.empty() || drawCursor.empty() || present.empty() || fill.empty() ||
        viewport.empty() || blitNoScale.empty() || blitToRect.empty() || blitFromTo.empty() ||
        cursorSave.empty() || planPick.empty())
    {
        spdlog::error("MenuResolution: display mode code not found");
        return;
    }

    injector::MakeNOP(fullscreen.get_first(5), 2, true);
    injector::MakeNOP(windowed.get_first(3), 2, true);
    for (size_t i = 0; i < 3; ++i)
    {
        injector::WriteMemory<uint8_t>(dialogDraw.get(i).get<void>(8),  0x0c, true);
        injector::WriteMemory<uint8_t>(dialogDraw.get(i).get<void>(41), 0x10, true);
    }
    injector::WriteMemory<uint8_t>(dialogCtor.get_first(8),  0x0c, true);
    injector::WriteMemory<uint8_t>(dialogCtor.get_first(33), 0x10, true);
    injector::WriteMemory<uint8_t>(yesNoCtor.get_first(8),   0x0c, true);
    injector::WriteMemory<uint8_t>(yesNoCtor.get_first(41),  0x10, true);
    injector::WriteMemory<uint8_t>(okayCtor.get_first(8),    0x0c, true);
    injector::WriteMemory<uint8_t>(okayCtor.get_first(18),   0x10, true);
    for (size_t i = 0; i < 2; ++i)
    {
        injector::WriteMemory<uint8_t>(quickCtor.get(i).get<void>(2),  0x0c, true);
        injector::WriteMemory<uint8_t>(quickCtor.get(i).get<void>(16), 0x10, true);
    }
    injector::WriteMemory<uint8_t>(quickDraw.get_first(2),  0x0c, true);
    injector::WriteMemory<uint8_t>(quickDraw.get_first(29), 0x10, true);

    for (size_t i = 0; i < 5; ++i)
    {
        shHaze[i] = Memory::MidHook(i < 4 ? hazeDialogs.get(i).get<void>() : hazeQuick.get_first(), HazeLoop);
        if (!shHaze[i])
        {
            spdlog::error("MenuResolution: haze hook {} failed", i);
            return;
        }
    }

    injector::WriteMemory<uint32_t>(cursorSave.get_first(23), SaveSurfaceWidth, true);
    injector::WriteMemory<uint32_t>(cursorSave.get_first(31), SaveSurfaceHeight, true);

    shReadOptions  = Memory::Hook(options.get_first(), ReadOptions);
    shInitRenderer = Memory::Hook(init.get_first(), InitRenderer);
    shSetCursorPosition = Memory::Hook(cursorPos.get_first(), SetCursorPosition);
    shDrawCursor        = Memory::Hook(drawCursor.get_first(), DrawCursor);
    shPresentFrame = Memory::Hook(present.get_first(), PresentFrame);
    shCreateViewport = Memory::Hook(viewport.get_first(), CreateViewport);
    shBlitToRect   = Memory::Hook(blitToRect.get_first(), BlitToRect);
    shBlitNoScale  = Memory::Hook(blitNoScale.get_first(), BlitNoScale);
    shPlanPick     = Memory::MidHook(planPick.get_first(10), MapPlanPick);
    BlitToRectFn   = reinterpret_cast<decltype(BlitToRectFn)>(blitToRect.get_first());
    BlitFromRectToRect = reinterpret_cast<decltype(BlitFromRectToRect)>(blitFromTo.get_first());
    ColorFill     = reinterpret_cast<decltype(ColorFill)>(fill.get_first());
    if (!shReadOptions || !shInitRenderer || !shSetCursorPosition || !shDrawCursor ||
        !shPresentFrame || !shCreateViewport || !shBlitToRect || !shBlitNoScale || !shPlanPick)
    {
        spdlog::error("MenuResolution: hook installation failed");
        return;
    }

    for (size_t i = 0; i < std::size(BlitSites); ++i)
    {
        auto site = hook::pattern(BlitSites[i].pattern);
        if (site.empty())
        {
            spdlog::error("MenuResolution: blit site {} not found", i);
            return;
        }

        shBlit[i] = Memory::MidHook(site.get_first(BlitSites[i].call), ScaleShellBlit);
        if (!shBlit[i])
        {
            spdlog::error("MenuResolution: blit site {} hook failed", i);
            return;
        }
    }

    spdlog::info("MenuResolution: menu follows the gameplay resolution, shell scaled and centred");
}
