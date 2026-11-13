#include "Config.h"
#include "Logger.h"
#include "embedded_offsets.h"   // genere par CMake (signatures integrees)
#include <windows.h>
#include <sstream>
#include <cctype>
#include <cstring>
#include <string>
#include <fstream>
#include <cstdio>

namespace {
    std::filesystem::path g_folder;
    Offsets g_offsets;
    Settings g_settings;
    std::vector<QuickCommand> g_commands;

    std::wstring IniPath(const wchar_t* name) { return (g_folder / name).wstring(); }

    // Les signatures sont integrees dans la DLL sous forme obfusquee (XOR). On les dechiffre,
    // on les ecrit dans un fichier temporaire unique juste le temps de les lire, puis on
    // l'ecrase et on le supprime. Aucun offsets.ini en clair n'est livre aux joueurs.
    std::string DecodeEmbeddedOffsets() {
        std::string out(kEmbeddedOffsetsLen, '\0');
        for (unsigned i = 0; i < kEmbeddedOffsetsLen; ++i)
            out[i] = static_cast<char>(kEmbeddedOffsets[i] ^ kEmbeddedOffsetsKey[i % sizeof(kEmbeddedOffsetsKey)]);
        return out;
    }

    // Renvoie le chemin d'un fichier offsets a lire, et met cleanup=true si c'est un temporaire a effacer.
    std::wstring ResolveOffsetsFile(bool& cleanup) {
        cleanup = false;
        // 1) offsets.ini a cote de la DLL : prioritaire (confort de developpement, edition a chaud)
        const std::wstring external = IniPath(L"offsets.ini");
        if (GetFileAttributesW(external.c_str()) != INVALID_FILE_ATTRIBUTES) {
            LOG_INFO("offsets.ini externe detecte, utilise a la place des signatures integrees");
            return external;
        }
        // 2) sinon, les signatures integrees dans la DLL
        if (kEmbeddedOffsetsLen == 0) {
            LOG_WARN("Aucun offsets.ini et aucune signature integree : les modules resteront inactifs");
            return external;   // n'existe pas : les lectures renverront les valeurs par defaut
        }
        wchar_t dir[MAX_PATH], file[MAX_PATH];
        if (GetTempPathW(MAX_PATH, dir) == 0 || GetTempFileNameW(dir, L"nx", 0, file) == 0) {
            LOG_ERROR("Fichier temporaire impossible : signatures integrees illisibles");
            return external;
        }
        const std::string data = DecodeEmbeddedOffsets();
        HANDLE h = CreateFileW(file, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
            CloseHandle(h);
        }
        cleanup = true;
        return file;
    }

    void ShredAndDelete(const std::wstring& file) {
        HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER size{};
            if (GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart < 1 << 20) {
                std::string zeros(static_cast<size_t>(size.QuadPart), '\0');
                DWORD w = 0;
                WriteFile(h, zeros.data(), static_cast<DWORD>(zeros.size()), &w, nullptr);
            }
            CloseHandle(h);
        }
        DeleteFileW(file.c_str());
    }

    std::string ReadStr(const wchar_t* file, const wchar_t* section, const wchar_t* key, const char* def = "") {
        char buf[2048];
        std::wstring wdef(def, def + strlen(def));
        wchar_t wbuf[2048];
        GetPrivateProfileStringW(section, key, wdef.c_str(), wbuf, 2048, file);
        WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, buf, sizeof(buf), nullptr, nullptr);
        // retire les commentaires en fin de ligne
        std::string s = buf;
        if (auto p = s.find(';'); p != std::string::npos) s.erase(p);
        while (!s.empty() && isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
        return s;
    }

    long long ReadInt(const wchar_t* file, const wchar_t* section, const wchar_t* key, long long def) {
        std::string s = ReadStr(file, section, key);
        if (s.empty()) return def;
        try { return std::stoll(s, nullptr, 0); } // accepte 0x...
        catch (...) { LOG_WARN("Valeur invalide pour %ls/%ls : '%s'", section, key, s.c_str()); return def; }
    }

    float ReadFloat(const wchar_t* file, const wchar_t* section, const wchar_t* key, float def) {
        std::string s = ReadStr(file, section, key);
        if (s.empty()) return def;
        try { return std::stof(s); } catch (...) { return def; }
    }

    void WriteVal(const wchar_t* file, const wchar_t* section, const wchar_t* key, const std::wstring& v) {
        WritePrivateProfileStringW(section, key, v.c_str(), file);
    }
}

void Config::Init(const std::filesystem::path& folder) {
    g_folder = folder;
    LoadOffsets();
    LoadSettings();
    LoadCommands();
}

void Config::LoadOffsets() {
    bool cleanup = false;
    const std::wstring f = ResolveOffsetsFile(cleanup);
    Offsets o;
    o.playerHookSig       = ReadStr(f.c_str(), L"player", L"hook_signature");
    o.playerHookArg       = static_cast<int>(ReadInt(f.c_str(), L"player", L"hook_argument", 0));
    std::stringstream chain(ReadStr(f.c_str(), L"player", L"pointer_chain"));
    for (std::string part; std::getline(chain, part, ',');) {
        try { if (!part.empty()) o.playerChain.push_back(static_cast<ptrdiff_t>(std::stoll(part, nullptr, 0))); }
        catch (...) { LOG_WARN("pointer_chain invalide : '%s'", part.c_str()); }
    }

    o.getArmorSig        = ReadStr(f.c_str(), L"functions", L"Actor_getArmor");
    o.getSelectedItemSig = ReadStr(f.c_str(), L"functions", L"Player_getSelectedItem");
    o.getDamageValueSig  = ReadStr(f.c_str(), L"functions", L"ItemStackBase_getDamageValue");

    o.itemStackItem         = static_cast<ptrdiff_t>(ReadInt(f.c_str(), L"offsets", L"ItemStackBase_item", 0x8));
    o.itemStackCount        = static_cast<ptrdiff_t>(ReadInt(f.c_str(), L"offsets", L"ItemStackBase_count", -1));
    o.itemGetMaxDamageIndex = static_cast<int>(ReadInt(f.c_str(), L"vtable", L"Item_getMaxDamage", -1));

    auto readChain = [&](const wchar_t* key) {
        std::vector<ptrdiff_t> out;
        std::stringstream ss(ReadStr(f.c_str(), L"entities", key));
        for (std::string part; std::getline(ss, part, ',');) {
            try { if (!part.empty()) out.push_back(static_cast<ptrdiff_t>(std::stoll(part, nullptr, 0))); }
            catch (...) { LOG_WARN("entities.%ls invalide : '%s'", key, part.c_str()); }
        }
        return out;
    };
    o.entityHookSig        = ReadStr(f.c_str(), L"entities", L"hook_signature");
    o.entityHookArg        = static_cast<int>(ReadInt(f.c_str(), L"entities", L"hook_argument", 0));
    o.positionChain        = readChain(L"position_chain");
    o.rotationChain        = readChain(L"rotation_chain");
    o.aabbChain            = readChain(L"aabb_chain");
    o.getHealthSig         = ReadStr(f.c_str(), L"entities", L"Actor_getHealth");
    o.getMaxHealthSig      = ReadStr(f.c_str(), L"entities", L"Actor_getMaxHealth");
    o.getEntityTypeIdIndex = static_cast<int>(ReadInt(f.c_str(), L"entities", L"vtable_getEntityTypeId", -1));
    o.getNameTagIndex      = static_cast<int>(ReadInt(f.c_str(), L"entities", L"vtable_getNameTag", -1));
    o.canSeeSig            = ReadStr(f.c_str(), L"entities", L"Actor_canSee");
    o.isInvisibleIndex     = static_cast<int>(ReadInt(f.c_str(), L"entities", L"vtable_isInvisible", -1));

    o.zoomFovSig      = ReadStr(f.c_str(), L"zoom", L"getFov_signature");
    o.nametagSig      = ReadStr(f.c_str(), L"nametag", L"hook_signature");
    o.nametagActorArg = static_cast<int>(ReadInt(f.c_str(), L"nametag", L"actor_argument", 0));

    o.sprintHookSig = ReadStr(f.c_str(), L"autosprint", L"hook_signature");
    o.sprintHookArg = static_cast<int>(ReadInt(f.c_str(), L"autosprint", L"hook_argument", 0));
    o.sprintOffset  = static_cast<ptrdiff_t>(ReadInt(f.c_str(), L"autosprint", L"sprint_offset", -1));

    g_offsets = std::move(o);
    if (cleanup) ShredAndDelete(f);
    LOG_INFO("Signatures chargees (%zu niveaux dans la chaine joueur)", g_offsets.playerChain.size());
}

void Config::LoadSettings() {
    const std::wstring f = IniPath(L"client.ini");
    const wchar_t* f_ = f.c_str();
    Settings& s = g_settings;
    s.armorEnabled   = ReadInt(f_, L"armor", L"enabled", s.armorEnabled) != 0;
    s.armorX         = ReadFloat(f_, L"armor", L"x", s.armorX);
    s.armorY         = ReadFloat(f_, L"armor", L"y", s.armorY);
    s.scale          = ReadFloat(f_, L"armor", L"scale", s.scale);
    s.layout         = static_cast<int>(ReadInt(f_, L"armor", L"layout", s.layout));
    s.valueMode      = static_cast<int>(ReadInt(f_, L"armor", L"value_mode", s.valueMode));
    s.showHeldItem   = ReadInt(f_, L"armor", L"show_held_item", s.showHeldItem) != 0;
    s.showEmptySlots = ReadInt(f_, L"armor", L"show_empty_slots", s.showEmptySlots) != 0;
    s.lowThreshold   = static_cast<int>(ReadInt(f_, L"armor", L"low_threshold", s.lowThreshold));
    s.bgAlpha        = ReadFloat(f_, L"armor", L"background_alpha", s.bgAlpha);
    s.fpsEnabled     = ReadInt(f_, L"fps", L"enabled", s.fpsEnabled) != 0;
    s.fpsX           = ReadFloat(f_, L"fps", L"x", s.fpsX);
    s.fpsY           = ReadFloat(f_, L"fps", L"y", s.fpsY);
    s.fpsAnchor      = static_cast<int>(ReadInt(f_, L"fps", L"anchor", s.fpsAnchor));
    s.fpsMargin      = ReadFloat(f_, L"fps", L"margin", s.fpsMargin);
    s.fpsScale       = ReadFloat(f_, L"fps", L"scale", s.fpsScale);
    s.fpsColorMode   = static_cast<int>(ReadInt(f_, L"fps", L"color_mode", s.fpsColorMode));
    s.fpsBackground  = ReadInt(f_, L"fps", L"background", s.fpsBackground) != 0;
    s.fpsShowMs      = ReadInt(f_, L"fps", L"show_ms", s.fpsShowMs) != 0;
    s.fpsShowLabel   = ReadInt(f_, L"fps", L"show_label", s.fpsShowLabel) != 0;
    s.fpsInterval    = ReadFloat(f_, L"fps", L"interval", s.fpsInterval);
    s.fpsTarget      = static_cast<int>(ReadInt(f_, L"fps", L"target", s.fpsTarget));
    s.autoSprint     = ReadInt(f_, L"movement", L"auto_sprint", s.autoSprint) != 0;
    s.showOwnNametag = ReadInt(f_, L"nametag", L"show_own", s.showOwnNametag) != 0;
    s.zoomEnabled    = ReadInt(f_, L"zoom", L"enabled", s.zoomEnabled) != 0;
    s.zoomKey        = static_cast<int>(ReadInt(f_, L"zoom", L"key", s.zoomKey));
    s.zoomMode       = static_cast<int>(ReadInt(f_, L"zoom", L"mode", s.zoomMode));
    s.zoomFactor     = ReadFloat(f_, L"zoom", L"factor", s.zoomFactor);
    s.zoomSmooth     = ReadInt(f_, L"zoom", L"smooth", s.zoomSmooth) != 0;
    s.healthEnabled  = ReadInt(f_, L"health", L"enabled", s.healthEnabled) != 0;
    s.hpPlayers      = ReadInt(f_, L"health", L"players", s.hpPlayers) != 0;
    s.hpAnimals      = ReadInt(f_, L"health", L"animals", s.hpAnimals) != 0;
    s.hpMonsters     = ReadInt(f_, L"health", L"monsters", s.hpMonsters) != 0;
    s.hpOthers       = ReadInt(f_, L"health", L"others", s.hpOthers) != 0;
    s.hpMaxDistance  = ReadFloat(f_, L"health", L"max_distance", s.hpMaxDistance);
    s.hpHideFull     = ReadInt(f_, L"health", L"hide_full", s.hpHideFull) != 0;
    s.hpShowValue    = ReadInt(f_, L"health", L"show_value", s.hpShowValue) != 0;
    s.hpShowNames    = ReadInt(f_, L"health", L"show_names", s.hpShowNames) != 0;
    s.hpBarScale     = ReadFloat(f_, L"health", L"bar_scale", s.hpBarScale);
    s.fov            = ReadFloat(f_, L"health", L"fov", s.fov);
    s.demoMode       = ReadInt(f_, L"general", L"demo_mode", s.demoMode) != 0;
    s.discordEnabled = ReadInt(f_, L"discord", L"enabled", s.discordEnabled) != 0;
    s.discordShowIp      = ReadInt(f_, L"discord", L"show_ip", s.discordShowIp) != 0;
    s.discordShowPlayers = ReadInt(f_, L"discord", L"show_players", s.discordShowPlayers) != 0;
    s.discordShowPing    = ReadInt(f_, L"discord", L"show_ping", s.discordShowPing) != 0;
    s.discordShowMotd    = ReadInt(f_, L"discord", L"show_motd", s.discordShowMotd) != 0;
    s.chatKey        = static_cast<int>(ReadInt(f_, L"chat", L"open_key", s.chatKey));
    s.chatOverlay    = ReadInt(f_, L"chat", L"overlay", s.chatOverlay) != 0;
    s.chatOverlaySide = static_cast<int>(ReadInt(f_, L"chat", L"overlay_side", s.chatOverlaySide));
    s.chatOverlayBottom = ReadFloat(f_, L"chat", L"overlay_bottom", s.chatOverlayBottom);
    s.menuBlockMode  = static_cast<int>(ReadInt(f_, L"ui", L"menu_block_mode", s.menuBlockMode));
    s.accent[0]      = ReadFloat(f_, L"ui", L"accent_r", s.accent[0]);
    s.accent[1]      = ReadFloat(f_, L"ui", L"accent_g", s.accent[1]);
    s.accent[2]      = ReadFloat(f_, L"ui", L"accent_b", s.accent[2]);
}

void Config::SaveSettings() {
    const std::wstring f = IniPath(L"client.ini");
    const wchar_t* f_ = f.c_str();
    const Settings& s = g_settings;
    auto b = [](bool v) { return std::wstring(v ? L"1" : L"0"); };
    auto n = [](auto v) { return std::to_wstring(v); };
    WriteVal(f_, L"armor", L"enabled", b(s.armorEnabled));
    WriteVal(f_, L"armor", L"x", n(s.armorX));
    WriteVal(f_, L"armor", L"y", n(s.armorY));
    WriteVal(f_, L"armor", L"scale", n(s.scale));
    WriteVal(f_, L"armor", L"layout", n(s.layout));
    WriteVal(f_, L"armor", L"value_mode", n(s.valueMode));
    WriteVal(f_, L"armor", L"show_held_item", b(s.showHeldItem));
    WriteVal(f_, L"armor", L"show_empty_slots", b(s.showEmptySlots));
    WriteVal(f_, L"armor", L"low_threshold", n(s.lowThreshold));
    WriteVal(f_, L"armor", L"background_alpha", n(s.bgAlpha));
    WriteVal(f_, L"fps", L"enabled", b(s.fpsEnabled));
    WriteVal(f_, L"fps", L"x", n(s.fpsX));
    WriteVal(f_, L"fps", L"y", n(s.fpsY));
    WriteVal(f_, L"fps", L"anchor", n(s.fpsAnchor));
    WriteVal(f_, L"fps", L"margin", n(s.fpsMargin));
    WriteVal(f_, L"fps", L"scale", n(s.fpsScale));
    WriteVal(f_, L"fps", L"color_mode", n(s.fpsColorMode));
    WriteVal(f_, L"fps", L"background", b(s.fpsBackground));
    WriteVal(f_, L"fps", L"show_ms", b(s.fpsShowMs));
    WriteVal(f_, L"fps", L"show_label", b(s.fpsShowLabel));
    WriteVal(f_, L"fps", L"interval", n(s.fpsInterval));
    WriteVal(f_, L"fps", L"target", n(s.fpsTarget));
    WriteVal(f_, L"movement", L"auto_sprint", b(s.autoSprint));
    WriteVal(f_, L"nametag", L"show_own", b(s.showOwnNametag));
    WriteVal(f_, L"zoom", L"enabled", b(s.zoomEnabled));
    WriteVal(f_, L"zoom", L"key", n(s.zoomKey));
    WriteVal(f_, L"zoom", L"mode", n(s.zoomMode));
    WriteVal(f_, L"zoom", L"factor", n(s.zoomFactor));
    WriteVal(f_, L"zoom", L"smooth", b(s.zoomSmooth));
    WriteVal(f_, L"health", L"enabled", b(s.healthEnabled));
    WriteVal(f_, L"health", L"players", b(s.hpPlayers));
    WriteVal(f_, L"health", L"animals", b(s.hpAnimals));
    WriteVal(f_, L"health", L"monsters", b(s.hpMonsters));
    WriteVal(f_, L"health", L"others", b(s.hpOthers));
    WriteVal(f_, L"health", L"max_distance", n(s.hpMaxDistance));
    WriteVal(f_, L"health", L"hide_full", b(s.hpHideFull));
    WriteVal(f_, L"health", L"show_value", b(s.hpShowValue));
    WriteVal(f_, L"health", L"show_names", b(s.hpShowNames));
    WriteVal(f_, L"health", L"bar_scale", n(s.hpBarScale));
    WriteVal(f_, L"health", L"fov", n(s.fov));
    WriteVal(f_, L"general", L"demo_mode", b(s.demoMode));
    WriteVal(f_, L"chat", L"open_key", n(s.chatKey));
    WriteVal(f_, L"discord", L"enabled", b(s.discordEnabled));
    WriteVal(f_, L"discord", L"show_ip", b(s.discordShowIp));
    WriteVal(f_, L"discord", L"show_players", b(s.discordShowPlayers));
    WriteVal(f_, L"discord", L"show_ping", b(s.discordShowPing));
    WriteVal(f_, L"discord", L"show_motd", b(s.discordShowMotd));
    WriteVal(f_, L"chat", L"overlay", b(s.chatOverlay));
    WriteVal(f_, L"chat", L"overlay_side", n(s.chatOverlaySide));
    WriteVal(f_, L"chat", L"overlay_bottom", n(s.chatOverlayBottom));
    WriteVal(f_, L"ui", L"menu_block_mode", n(s.menuBlockMode));
    WriteVal(f_, L"ui", L"accent_r", n(s.accent[0]));
    WriteVal(f_, L"ui", L"accent_g", n(s.accent[1]));
    WriteVal(f_, L"ui", L"accent_b", n(s.accent[2]));
}

std::vector<QuickCommand>& Config::Commands() { return g_commands; }

void Config::LoadCommands() {
    g_commands.clear();
    std::ifstream in(g_folder / L"commands.txt", std::ios::binary);
    if (!in) {
        g_commands = {
            { "Jour",          "/time set day" },
            { "Nuit",          "/time set night" },
            { "Beau temps",    "/weather clear" },
            { "Créatif",       "/gamemode creative" },
            { "Survie",        "/gamemode survival" },
            { "Coucou",        "Salut tout le monde !" },
        };
        SaveCommands();
        return;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() >= 3 && (unsigned char)line[0] == 0xEF) line.erase(0, 3);   // BOM UTF-8
        if (line.empty() || line[0] == '#') continue;
        const size_t bar = line.find('|');
        if (bar == std::string::npos) g_commands.push_back({ line, line });
        else g_commands.push_back({ line.substr(0, bar), line.substr(bar + 1) });
    }
    LOG_INFO("%zu commandes rapides chargees", g_commands.size());
}

void Config::SaveCommands() {
    std::ofstream out(g_folder / L"commands.txt", std::ios::binary | std::ios::trunc);
    out << "# Commandes rapides Nexus : une par ligne, \"Nom du bouton|texte envoye\"\n";
    for (const auto& c : g_commands) out << c.label << '|' << c.text << '\n';
}

Offsets& Config::GetOffsets() { return g_offsets; }
Settings& Config::GetSettings() { return g_settings; }
const std::filesystem::path& Config::Folder() { return g_folder; }
