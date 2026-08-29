#include "stdafx.h"
#include "common.hpp"
#include "config.hpp"
#include "feature.hpp"
#include "logging.hpp"

void Init()
{
    InitPaths();

    // Launchers, updaters and editors sitting in the same folder load the asi too, so bail before
    // touching the log, or whichever ran last owns it.
    if (!Game::SelectTarget())
        return;

    Logging::Initialize();
    Logging::LogSystemInfo();
    Config::Read();
    Memory::ReserveTrampolines();
    RegisterFeatures();
}

CEXP void InitializeASI()
{
    std::call_once(CallbackHandler::flag, []()
    {
        CallbackHandler::RegisterCallback(Init);
    });
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        baseModule = hModule;
        InitializeASI();
    }
    return TRUE;
}
