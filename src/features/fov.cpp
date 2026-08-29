#include "stdafx.h"
#include "common.hpp"
#include "feature.hpp"

namespace
{
    constexpr ptrdiff_t CameraViewport = 0x90;
    constexpr ptrdiff_t CameraFOV      = 0xb4;
    constexpr ptrdiff_t CameraAspect   = 0xb8;
    constexpr ptrdiff_t ViewportWidth  = 0x10;
    constexpr ptrdiff_t DisplayHeight  = 0x20;

    using Game::AppObject;

    constexpr float ReferenceAspect = 4.0f / 3.0f;
    constexpr float Degrees = 57.2957795f;

    SafetyHookInline shApplyCamera{};

    float lastLogged = 0.0f;

    // The engine keeps the horizontal fixed and derives the vertical by dividing by aspect, so the
    // view loses height as the display widens. Scaling the horizontal against a 4:3 viewport of the
    // same height holds the vertical and adds width instead. The engine's 1.0125 aspect factor and
    // its 16:9 clamp both cancel in the ratio, so the correction follows a view size change itself.
    void __fastcall ApplyCamera(uint8_t* camera, void*)
    {
        float* fov = reinterpret_cast<float*>(camera + CameraFOV);
        const float stock = *fov;
        float corrected = stock;

        uint8_t* app = *reinterpret_cast<uint8_t**>(AppObject);
        uint8_t* display = app ? *reinterpret_cast<uint8_t**>(app + 4) : nullptr;
        uint8_t* viewport = *reinterpret_cast<uint8_t**>(camera + CameraViewport);

        if (display && viewport)
        {
            const int height = *reinterpret_cast<int*>(display + DisplayHeight);
            const float width = static_cast<float>(*reinterpret_cast<int*>(viewport + ViewportWidth));
            const float ratio = height > 0 ? width / (static_cast<float>(height) * ReferenceAspect) : 1.0f;

            if (ratio > 1.0f)
            {
                corrected = 2.0f * std::atan(std::tan(stock * 0.5f) * ratio);
                *fov = corrected;

                // Once: the value now changes every frame of a zoom animation.
                if (lastLogged == 0.0f)
                {
                    const float aspect = *reinterpret_cast<float*>(camera + CameraAspect);
                    spdlog::info("FOV: horizontal {:.2f} -> {:.2f} deg, vertical {:.2f} deg",
                                 stock * Degrees, *fov * Degrees,
                                 2.0f * std::atan(std::tan(*fov * 0.5f) / aspect) * Degrees);
                    lastLogged = *fov;
                }
            }
        }

        shApplyCamera.thiscall<void>(camera);

        // Only the frustum is widened; the camera keeps its stock value. The apply ends by
        // rendering, which is where the weapon zoom is written into this field for the next frame -
        // restoring unconditionally overwrote it, so zoom never reached the renderer.
        if (*fov == corrected)
            *fov = stock;
    }
}

FEATURE(Game, FOVCorrection)
{
    // Per-frame camera apply: SetViewport / SetFOV / SetAspect / SetNear / SetFar.
    auto apply = hook::pattern("56 8B F1 B9 ? ? ? ? 8B 86 90 00 00 00 50 E8 ? ? ? ? "
                               "D9 86 B4 00 00 00");
    if (apply.empty())
    {
        spdlog::error("FOV: camera apply not found");
        return;
    }

    shApplyCamera = Memory::Hook(apply.get_first(), ApplyCamera);
    if (!shApplyCamera)
        spdlog::error("FOV: hook installation failed");
}
