#include "stdafx.h"
#include "common.hpp"

void InitPaths()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(GetModuleHandleW(nullptr), path, MAX_PATH);

    auto exe = std::filesystem::path(path);
    sExePath = exe.parent_path();
    sExeName = exe.filename().string();
    if (GetModuleFileNameW(baseModule, path, MAX_PATH))
        sAsiPath = std::filesystem::path(path).parent_path();
    else
        sAsiPath = sExePath;
}

namespace
{
    // Neither exe has relocations; both load at their preferred base.
    struct Target
    {
        const char* exe;
        uintptr_t   appObject;
        uintptr_t   renderer;
        uintptr_t   screenSurface;
        uintptr_t   primarySurface;
        uintptr_t   reticlePool;
    };

    constexpr Target Targets[]
    {
        { "RainbowSix.exe",   0x008aebdc, 0x008bb690, 0x008bc4d8, 0x008bc42c, 0x0084691c },
        { "RainbowSixMP.exe", 0x008b1dac, 0x008be820, 0x008bf668, 0x008bf5bc, 0x008498dc },
    };
}

namespace
{
    std::string TargetBytes(void* target)
    {
        std::string text;
        for (int i = 0; i < 16; ++i)
            text += std::format("{:02X} ", static_cast<uint8_t*>(target)[i]);

        return text;
    }

    const char* AllocatorError(safetyhook::Allocator::Error error)
    {
        return error == safetyhook::Allocator::Error::BAD_VIRTUAL_ALLOC ? "VirtualAlloc refused"
                                                                       : "no free memory in range";
    }

    const char* InlineError(const safetyhook::InlineHook::Error& error)
    {
        switch (error.type)
        {
        case safetyhook::InlineHook::Error::BAD_ALLOCATION:                        return AllocatorError(error.allocator_error);
        case safetyhook::InlineHook::Error::FAILED_TO_DECODE_INSTRUCTION:          return "could not decode an instruction";
        case safetyhook::InlineHook::Error::SHORT_JUMP_IN_TRAMPOLINE:              return "short jump in the trampoline";
        case safetyhook::InlineHook::Error::IP_RELATIVE_INSTRUCTION_OUT_OF_RANGE:  return "ip-relative instruction out of range";
        case safetyhook::InlineHook::Error::UNSUPPORTED_INSTRUCTION_IN_TRAMPOLINE: return "unsupported instruction in the trampoline";
        case safetyhook::InlineHook::Error::FAILED_TO_UNPROTECT:                   return "could not unprotect the page";
        case safetyhook::InlineHook::Error::NOT_ENOUGH_SPACE:                      return "not enough space at the entry";
        default:                                                                   return "unknown";
        }
    }
}

// Only safetyhook's first allocation can fail, and this path is the one that cannot, so taking it
// here makes the 64k block exist before any feature hooks. The allocation is held because a live
// one cannot be handed back.
void Memory::ReserveTrampolines()
{
    static safetyhook::Allocation pool;

    auto reserved = safetyhook::Allocator::global()->allocate(1);
    if (!reserved)
    {
        spdlog::warn("Trampoline block: {}", AllocatorError(reserved.error()));
        return;
    }

    pool = std::move(*reserved);
}

SafetyHookInline Memory::Hook(void* target, void* destination)
{
    auto hook = safetyhook::InlineHook::create(target, destination);
    if (hook)
        return std::move(*hook);

    spdlog::error("Hook at 0x{:08x} refused: {} [{}]", reinterpret_cast<uintptr_t>(target),
                  InlineError(hook.error()), TargetBytes(target));
    return {};
}

SafetyHookMid Memory::MidHook(void* target, safetyhook::MidHookFn destination)
{
    auto hook = safetyhook::MidHook::create(target, destination);
    if (hook)
        return std::move(*hook);

    const auto& error = hook.error();
    const char* reason = error.type == safetyhook::MidHook::Error::BAD_ALLOCATION
                       ? AllocatorError(error.allocator_error)
                       : InlineError(error.inline_hook_error);

    spdlog::error("Mid hook at 0x{:08x} refused: {} [{}]", reinterpret_cast<uintptr_t>(target),
                  reason, TargetBytes(target));
    return {};
}

bool Game::SelectTarget()
{
    for (const auto& target : Targets)
    {
        if (_stricmp(sExeName.c_str(), target.exe) != 0)
            continue;

        AppObject      = target.appObject;
        Renderer       = target.renderer;
        ScreenSurface  = target.screenSurface;
        PrimarySurface = target.primarySurface;
        ReticlePool    = target.reticlePool;
        return true;
    }

    return false;
}

namespace
{
    // The whole of WSOCK32 is imported by ordinal, so a name is not always available to match on.
    IMAGE_THUNK_DATA* FindIATEntry(uint8_t* base, const char* targetModule, const char* targetFunction, uint16_t targetOrdinal)
    {
        auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        auto dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!dir.VirtualAddress)
            return nullptr;

        auto imports = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);

        for (int i = 0; imports[i].Characteristics; ++i)
        {
            if (_stricmp(reinterpret_cast<const char*>(base + imports[i].Name), targetModule) != 0)
                continue;

            auto orig = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imports[i].OriginalFirstThunk);
            auto thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imports[i].FirstThunk);

            for (; orig->u1.AddressOfData; ++orig, ++thunk)
            {
                if (orig->u1.Ordinal & IMAGE_ORDINAL_FLAG)
                {
                    if (targetOrdinal && IMAGE_ORDINAL(orig->u1.Ordinal) == targetOrdinal)
                        return thunk;

                    continue;
                }

                if (!targetFunction)
                    continue;

                auto byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + orig->u1.AddressOfData);
                if (strcmp(reinterpret_cast<const char*>(byName->Name), targetFunction) == 0)
                    return thunk;
            }
        }

        return nullptr;
    }
}

void* Memory::ReadIAT(HMODULE callerModule, const char* targetModule, const char* targetFunction)
{
    auto thunk = FindIATEntry(reinterpret_cast<uint8_t*>(callerModule), targetModule, targetFunction, 0);
    return thunk ? reinterpret_cast<void*>(thunk->u1.Function) : nullptr;
}

void* Memory::ReadIAT(HMODULE callerModule, const char* targetModule, uint16_t targetOrdinal)
{
    auto thunk = FindIATEntry(reinterpret_cast<uint8_t*>(callerModule), targetModule, nullptr, targetOrdinal);
    return thunk ? reinterpret_cast<void*>(thunk->u1.Function) : nullptr;
}

bool Memory::WriteIAT(HMODULE callerModule, const char* targetModule, const char* targetFunction, void* detour)
{
    auto thunk = FindIATEntry(reinterpret_cast<uint8_t*>(callerModule), targetModule, targetFunction, 0);
    if (!thunk)
        return false;

    DWORD oldProtect;
    if (!VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_READWRITE, &oldProtect))
        return false;

    thunk->u1.Function = reinterpret_cast<ULONG_PTR>(detour);
    VirtualProtect(&thunk->u1.Function, sizeof(void*), oldProtect, &oldProtect);
    return true;
}

bool Memory::WriteIAT(HMODULE callerModule, const char* targetModule, uint16_t targetOrdinal, void* detour)
{
    auto thunk = FindIATEntry(reinterpret_cast<uint8_t*>(callerModule), targetModule, nullptr, targetOrdinal);
    if (!thunk)
        return false;

    DWORD oldProtect;
    if (!VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_READWRITE, &oldProtect))
        return false;

    thunk->u1.Function = reinterpret_cast<ULONG_PTR>(detour);
    VirtualProtect(&thunk->u1.Function, sizeof(void*), oldProtect, &oldProtect);
    return true;
}
