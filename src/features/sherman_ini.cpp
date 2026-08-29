#include "stdafx.h"
#include "common.hpp"
#include "feature.hpp"

#include <inipp/inipp.h>

namespace
{
    // Four reader/writer pairs, all __stdcall, and the whole of the engine's registry surface.
    SafetyHookInline shReadString{};
    SafetyHookInline shWriteString{};
    SafetyHookInline shReadInt{};
    SafetyHookInline shWriteInt{};
    SafetyHookInline shReadFloat{};
    SafetyHookInline shWriteFloat{};
    SafetyHookInline shReadGuid{};
    SafetyHookInline shWriteGuid{};

    // Values use the engine's string object: text at +4, capacity at +8.
    // Use its assign function so longer values are reallocated correctly.
    void*(__fastcall* StringAssign)(void*, void*, const char*) = nullptr;

    constexpr const char* RegSubKey = "Software\\Red Storm Entertainment\\Tom Clancy's Rainbow Six";
    constexpr const char* IniSection = "Settings";

    struct CiLess
    {
        using is_transparent = void;
        bool operator()(std::string_view a, std::string_view b) const
        {
            auto n = a.size() < b.size() ? a.size() : b.size();
            auto r = n ? _strnicmp(a.data(), b.data(), n) : 0;
            return r != 0 ? r < 0 : a.size() < b.size();
        }
    };

    std::map<std::string, std::string, CiLess> Values;
    std::filesystem::path IniFile;
    bool bWriteFailed = false;

    // Install paths are derived from the exe directory each boot, so moved installs follow automatically.
    const std::map<std::string_view, std::string_view> DataPaths{
        {"ActionMusicPath",      "sound\\music"},
        {"ActorPath",            "actor"},
        {"BiographyTextPath",    "text\\bios"},
        {"BitmapPath",           "bitmap"},
        {"BriefingBitmapPath",   "shell\\briefing"},
        {"BriefingSoundPath",    "sound"},
        {"BriefingTextPath",     "text\\briefing"},
        {"CharacterPath",        "character"},
        {"CursorPath",           "shell\\cursor"},
        {"DialoguePath",         "dialogue"},
        {"EquipTextPath",        "text\\kit"},
        {"EquipmentPath",        "kit"},
        {"FacesPath",            "shell\\faces"},
        {"FontPath",             "font"},
        {"GamePath",             "save"},
        {"IconPath",             "icon"},
        {"IntelBitmapPath",      "shell\\intel"},
        {"IntelTextPath",        "text\\intel"},
        {"JournalPath",          "journals"},
        {"MapPath",              "map"},
        {"MissionPath",          "mission"},
        {"MissionSplashBMPPath", "Splash"},
        {"MissionSplashPath",    "text\\splash"},
        {"MissionTextPath",      "text\\mission"},
        {"ModelPath",            "model"},
        {"MotionPath",           "motion"},
        {"PlanPath",             "plan"},
        {"PlotPath",             "plot"},
        {"ProfileTextPath",      "text\\profile"},
        {"RosterBitmapPath",     "shell\\roster"},
        {"ShellTextPath",        "text\\interface"},
        {"SoundPath",            "sound"},
        {"TempPath",             "temp"},
        {"TextPath",             "text"},
        {"TexturePath",          "texture"},
        {"VideoPath",            "video"},
        {"InstallationPath",     ""},
    };

    // Install paths are derived from the exe directory, not stored settings.
    bool Synthesised(const char* name, std::string& text)
    {
        auto path = DataPaths.find(name);
        if (path != DataPaths.end())
        {
            text = path->second.empty() ? sExePath.string() + "\\"
                                        : (sExePath / "data" / path->second).string();
            return true;
        }

        // A complete data tree beside the exe means a Full install, which never reads from the CD
        if (_stricmp(name, "InstallType") == 0)
        {
            text = "Full";
            return true;
        }

        if (_stricmp(name, "CDPath") == 0)
        {
            text = sExePath.string() + "\\";
            return true;
        }

        return false;
    }

    bool PassThrough(const char* name) { return !name; }

    const char* StringText(const void* object)
    {
        if (!object)
            return "";
        auto text = *reinterpret_cast<const char* const*>(static_cast<const uint8_t*>(object) + 4);
        return text ? text : "";
    }

    std::string FormatGuid(const GUID& g)
    {
        return std::format("{{{:08X}-{:04X}-{:04X}-{:02X}{:02X}-{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}}}",
                           g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2],
                           g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    }

    bool ParseGuid(const std::string& text, GUID& out)
    {
        unsigned d1 = 0, d2 = 0, d3 = 0, b[8]{};
        auto p = text.c_str();
        if (*p == '{')
            ++p;
        if (sscanf_s(p, "%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x", &d1, &d2, &d3,
                     &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &b[6], &b[7]) != 11)
            return false;

        out.Data1 = d1;
        out.Data2 = static_cast<unsigned short>(d2);
        out.Data3 = static_cast<unsigned short>(d3);
        for (int i = 0; i < 8; ++i)
            out.Data4[i] = static_cast<unsigned char>(b[i]);
        return true;
    }

    void Save()
    {
        std::ofstream file(IniFile, std::ios::trunc);
        if (!file)
        {
            // Read-only install directory. Say so once rather than on every settings change.
            if (!bWriteFailed)
                spdlog::error("ShermanIni: cannot write {}, settings will not persist", IniFile.string());
            bWriteFailed = true;
            return;
        }

        file << "[" << IniSection << "]\n";
        for (const auto& [key, value] : Values)
            file << key << " = " << value << "\n";
    }

    const std::string& Get(const char* name, std::string fallback)
    {
        auto it = Values.find(name);
        if (it != Values.end())
            return it->second;

        it = Values.emplace(name, std::move(fallback)).first;
        Save();
        return it->second;
    }

    void Set(const char* name, std::string value)
    {
        auto& slot = Values[name];
        if (slot == value)
            return;

        slot = std::move(value);
        Save();
    }

    // A failed read aborts Options_ReadFromRegistry. The engine expects the output to already hold the 
    // default, so return success and preserve it when the value is missing. This lets a bare install boot.
    char __stdcall ReadString(const char* section, const char* name, void* out, void* def)
    {
        std::string synthesised;
        if (name && Synthesised(name, synthesised))
        {
            StringAssign(out, nullptr, synthesised.c_str());
            return 1;
        }

        if (PassThrough(name))
        {
            if (!shReadString.stdcall<char>(section, name, out, def) && out != def)
                StringAssign(out, nullptr, StringText(def));
            return 1;
        }

        StringAssign(out, nullptr, Get(name, StringText(def)).c_str());
        return 1;
    }

    char __stdcall WriteString(const char* section, const char* name, void* value)
    {
        std::string ignored;
        if (PassThrough(name) || Synthesised(name, ignored))
            return shWriteString.stdcall<char>(section, name, value);

        Set(name, StringText(value));
        return 1;
    }

    char __stdcall ReadInt(const char* section, const char* name, int* out, int def)
    {
        if (PassThrough(name))
        {
            if (!shReadInt.stdcall<char>(section, name, out, def))
                *out = def;
            return 1;
        }

        int parsed = def;
        std::istringstream(Get(name, std::to_string(def))) >> parsed;
        *out = parsed;
        return 1;
    }

    char __stdcall WriteInt(const char* section, const char* name, int value)
    {
        if (PassThrough(name))
            return shWriteInt.stdcall<char>(section, name, value);

        Set(name, std::to_string(value));
        return 1;
    }

    char __stdcall ReadFloat(const char* section, const char* name, float* out, float def)
    {
        if (PassThrough(name))
        {
            if (!shReadFloat.stdcall<char>(section, name, out, def))
                *out = def;
            return 1;
        }

        float parsed = def;
        std::istringstream(Get(name, std::format("{}", def))) >> parsed;
        *out = parsed;
        return 1;
    }

    char __stdcall WriteFloat(const char* section, const char* name, float value)
    {
        if (PassThrough(name))
            return shWriteFloat.stdcall<char>(section, name, value);

        Set(name, std::format("{}", value));
        return 1;
    }

    char __stdcall ReadGuid(const char* section, const char* name, GUID* out, GUID def)
    {
        if (PassThrough(name))
        {
            if (!shReadGuid.stdcall<char>(section, name, out, def))
                *out = def;
            return 1;
        }

        if (!ParseGuid(Get(name, FormatGuid(def)), *out))
            *out = def;
        return 1;
    }

    char __stdcall WriteGuid(const char* section, const char* name, GUID value)
    {
        if (PassThrough(name))
            return shWriteGuid.stdcall<char>(section, name, value);

        Set(name, FormatGuid(value));
        return 1;
    }

    bool stale = false;

    bool Load()
    {
        std::ifstream file(IniFile);
        if (!file)
            return false;

        inipp::Ini<char> parsed;
        parsed.parse(file);

        auto section = parsed.sections.find(IniSection);
        if (section != parsed.sections.end())
            for (const auto& [key, value] : section->second)
            {
                // Remove old synthesised entries so the next save cleans them from the file
                std::string ignored;
                if (Synthesised(key.c_str(), ignored))
                    stale = true;
                else
                    Values[key] = value;
            }

        return true;
    }

    // One-time migration preserves settings from existing installs. Read with KEY_READ because the
    // engine requests KEY_QUERY_VALUE and then writes through the same handle.
    void Migrate()
    {
        HKEY key{};
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, RegSubKey, 0, KEY_READ, &key) != ERROR_SUCCESS)
        {
            spdlog::info("ShermanIni: no registry key to migrate, starting from the engine's defaults");
            return;
        }

        char name[256];
        BYTE data[2048];
        int taken = 0;

        for (DWORD i = 0;; ++i)
        {
            DWORD nameLen = static_cast<DWORD>(std::size(name));
            DWORD dataLen = static_cast<DWORD>(std::size(data));
            DWORD type = 0;

            auto status = RegEnumValueA(key, i, name, &nameLen, nullptr, &type, data, &dataLen);
            if (status == ERROR_NO_MORE_ITEMS)
                break;
            if (status != ERROR_SUCCESS)
                continue;

            std::string ignored;
            if (Synthesised(name, ignored) || PassThrough(name))
                continue;

            std::string text;
            if ((type == REG_SZ || type == REG_EXPAND_SZ) && dataLen > 0)
                text.assign(reinterpret_cast<char*>(data), strnlen(reinterpret_cast<char*>(data), dataLen));
            else if (type == REG_DWORD && dataLen == sizeof(DWORD))
                text = std::to_string(*reinterpret_cast<DWORD*>(data));
            else if (type == REG_BINARY && dataLen == sizeof(GUID))
                text = FormatGuid(*reinterpret_cast<GUID*>(data));
            else if (dataLen > 0)
                // The float writer tags its text REG_DWORD, so what is left is taken as text
                text.assign(reinterpret_cast<char*>(data), strnlen(reinterpret_cast<char*>(data), dataLen));
            else
                continue;

            Values[name] = std::move(text);
            ++taken;
        }

        RegCloseKey(key);
        spdlog::info("ShermanIni: migrated {} values out of the registry", taken);
    }
}

FEATURE(Game, ShermanIni)
{
    auto readString  = hook::pattern("81 EC 10 01 00 00 8B 15 ? ? ? ? 53 56");
    auto writeString = hook::pattern("51 8B 15 ? ? ? ? 8D 44 24 00 56 8D 4C 24 0C 50");
    auto readInt     = hook::pattern("83 EC 10 8B 15 ? ? ? ? 8D 44 24 08 56 8D 4C 24 18 50");
    auto readFloat   = hook::pattern("81 EC 10 01 00 00 8B 15 ? ? ? ? 56 8D 44 24 0C 57");
    auto writeFloat  = hook::pattern("D9 44 24 0C 81 EC 08 01 00 00 8D 44 24 08 57 83 EC 08 DD 1C");
    auto readGuid    = hook::pattern("83 EC 1C 8B 15 ? ? ? ? 8D 44 24 04 56 8D 4C 24 24 50");
    auto assign      = hook::pattern("8B 44 24 04 56 8B F1 3B 46 04 74 08 50 8B CE E8");

    // The int and GUID writers are byte-identical for 50 bytes and diverge only at the RET
    // immediate, so both patterns have to run that far to be unique
    constexpr const char* WriterHead =
        "51 8B 15 ? ? ? ? 8D 44 24 00 8D 4C 24 08 50 8B 44 24 0C 51 6A 00 6A 01 6A 00 52 "
        "6A 00 50 68 02 00 00 80 FF 15 ? ? ? ? 85 C0 74 06 32 C0 59 C2 ";
    const std::string writeIntBytes  = std::string(WriterHead) + "0C 00";
    const std::string writeGuidBytes = std::string(WriterHead) + "18 00";
    auto writeInt  = hook::pattern(writeIntBytes);
    auto writeGuid = hook::pattern(writeGuidBytes);

    if (readString.empty() || writeString.empty() || readInt.empty() || writeInt.empty() ||
        readFloat.empty() || writeFloat.empty() || readGuid.empty() || writeGuid.empty() ||
        assign.empty())
    {
        spdlog::error("ShermanIni: settings store not found");
        return;
    }

    StringAssign = reinterpret_cast<decltype(StringAssign)>(assign.get_first());

    std::error_code ec;
    std::filesystem::create_directories(sExePath / "data" / "temp", ec);
    std::filesystem::create_directories(sExePath / "data" / "save", ec);

    IniFile = sExePath / "Sherman.ini";
    if (Load())
    {
        spdlog::info("ShermanIni: {} values from {}", Values.size(), IniFile.string());

        // Load() dropped the keys that became synthesised, but only a change rewrites the file
        if (stale)
        {
            Save();
            spdlog::info("ShermanIni: dropped keys now derived from the install");
        }
    }
    else
    {
        Migrate();
    }

    shReadString  = Memory::Hook(readString.get_first(), ReadString);
    shWriteString = Memory::Hook(writeString.get_first(), WriteString);
    shReadInt     = Memory::Hook(readInt.get_first(), ReadInt);
    shWriteInt    = Memory::Hook(writeInt.get_first(), WriteInt);
    shReadFloat   = Memory::Hook(readFloat.get_first(), ReadFloat);
    shWriteFloat  = Memory::Hook(writeFloat.get_first(), WriteFloat);
    shReadGuid    = Memory::Hook(readGuid.get_first(), ReadGuid);
    shWriteGuid   = Memory::Hook(writeGuid.get_first(), WriteGuid);

    if (!shReadString || !shWriteString || !shReadInt || !shWriteInt ||
        !shReadFloat || !shWriteFloat || !shReadGuid || !shWriteGuid)
    {
        spdlog::error("ShermanIni: hook installation failed");
        return;
    }

    spdlog::info("ShermanIni: settings in {}, {} data paths synthesised, registry untouched",
                 IniFile.filename().string(), DataPaths.size());
}
