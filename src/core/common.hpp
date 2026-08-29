#pragma once

inline HMODULE baseModule = nullptr;
inline std::filesystem::path sExePath;
inline std::string sExeName;
inline std::filesystem::path sAsiPath;

void InitPaths();

// RainbowSixMP.exe (Eagle Watch) is the same code with its data segment shifted, so every pattern
// matches both once an embedded address is wildcarded. Only these globals differ.
namespace Game
{
    inline uintptr_t AppObject      = 0;
    inline uintptr_t Renderer       = 0;
    inline uintptr_t ScreenSurface  = 0;
    inline uintptr_t PrimarySurface = 0;

    // Base of the six-float pool the reticle draw loads from; its members sit at fixed offsets in it.
    inline uintptr_t ReticlePool    = 0;

    // False when the host is not one of the game executables, which is the signal to do nothing.
    bool SelectTarget();
}

namespace Memory
{
    // Call once before any hook.
    void ReserveTrampolines();

    // safetyhook's easy API discards the failure reason; these log it with the target's bytes.
    SafetyHookInline Hook(void* target, void* destination);
    SafetyHookMid    MidHook(void* target, safetyhook::MidHookFn destination);

    void* ReadIAT(HMODULE callerModule, const char* targetModule, const char* targetFunction);
    void* ReadIAT(HMODULE callerModule, const char* targetModule, uint16_t targetOrdinal);
    bool  WriteIAT(HMODULE callerModule, const char* targetModule, const char* targetFunction, void* detour);
    bool  WriteIAT(HMODULE callerModule, const char* targetModule, uint16_t targetOrdinal, void* detour);
}
