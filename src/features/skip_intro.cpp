#include "stdafx.h"
#include "common.hpp"
#include "config.hpp"
#include "feature.hpp"

static Config::Value bSkipIntro("General", "SkipIntro", false);

FEATURE(Game, SkipIntro)
{
    if (!bSkipIntro)
        return;

    // WinMain skips the movie state when MovieOn is clear, so forcing that branch is the engine's
    // own no-movies route rather than a state it can be left waiting in.
    auto movieOn = hook::pattern("8A 82 08 02 00 00 84 C0 74 1D");
    if (movieOn.empty())
    {
        spdlog::error("SkipIntro: MovieOn branch not found");
        return;
    }

    injector::WriteMemory<uint8_t>(movieOn.get_first(8), 0xEB, true);

    spdlog::info("SkipIntro: startup movies disabled");
}
