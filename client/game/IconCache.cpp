#include "IconCache.h"
#include "core/Logger.h"
#include "hooks/RenderHook.h"
#include <stb_image.h>
#include <windows.h>
#include <unordered_map>
#include <string>
#include <vector>
#include <fstream>
#include <filesystem>
#include <algorithm>

namespace {
    struct Entry { ImTextureID tex = 0; bool tried = false; };
    std::unordered_map<std::string, Entry> g_cache;

    // Index : "diamond_helmet.png" -> chemin complet. Construit une fois.
    std::unordered_map<std::string, std::filesystem::path> g_index;
    bool g_indexReady = false;

    std::string lower(std::string s) {
        for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        return s;
    }
    // "Diamond Helmet" / "diamond helmet" -> "diamond_helmet"
    std::string toKey(const char* id) {
        std::string s;
        for (const char* p = id; *p; ++p) s.push_back(*p == ' ' ? '_' : *p);
        return lower(s);
    }

    void buildIndex() {
        g_indexReady = true;
        std::error_code ec;
        wchar_t exe[MAX_PATH] = {};
        if (!GetModuleFileNameW(GetModuleHandleW(nullptr), exe, MAX_PATH)) return;
        const std::filesystem::path install = std::filesystem::path(exe).parent_path();
        const std::filesystem::path root = std::filesystem::exists(install / L"data", ec) ? (install / L"data") : install;

        namespace fs = std::filesystem;
        std::vector<fs::path> stack{ root };
        size_t files = 0, samples = 0;
        while (!stack.empty() && files < 60000) {
            const fs::path dir = stack.back(); stack.pop_back();
            std::error_code e;
            for (fs::directory_iterator di(dir, fs::directory_options::skip_permission_denied, e), end; di != end; di.increment(e)) {
                if (e) break;
                std::error_code e2;
                if (di->is_directory(e2)) { stack.push_back(di->path()); continue; }
                if (!di->is_regular_file(e2)) continue;
                const fs::path& p = di->path();
                if (lower(p.extension().string()) != ".png") continue;
                const std::string fn = lower(p.filename().string());
                g_index.emplace(fn, p);
                ++files;
                // 20 premiers noms + tout ce qui contient "diamond" (pour comprendre le nommage)
                if (samples < 20) { LOG_INFO("  png[%zu] : %s", files, p.string().c_str()); ++samples; }
                else if (fn.find("diamond") != std::string::npos || fn.find("helmet") != std::string::npos)
                    LOG_INFO("  diamond/helmet : %s", p.string().c_str());
            }
        }
        LOG_INFO("Icones : index de %zu PNG construit (racine : %s)", g_index.size(), root.string().c_str());
    }

    ImTextureID loadFrom(const std::filesystem::path& file) {
        std::ifstream in(file, std::ios::binary);
        if (!in) return 0;
        std::vector<unsigned char> buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (buf.empty()) return 0;
        int w = 0, h = 0, ch = 0;
        unsigned char* px = stbi_load_from_memory(buf.data(), static_cast<int>(buf.size()), &w, &h, &ch, 4);
        if (!px) { LOG_WARN("Icone illisible : %s", file.string().c_str()); return 0; }
        const unsigned long long id = RenderHook::LoadTexture(px, w, h);
        stbi_image_free(px);
        LOG_INFO("Icone chargee : %s (%dx%d)", file.filename().string().c_str(), w, h);
        return static_cast<ImTextureID>(id);
    }
}

ImTextureID IconFor(const char* itemId) {
    if (!itemId || !itemId[0]) return 0;
    if (!g_indexReady) buildIndex();

    const std::string key = toKey(itemId);
    Entry& e = g_cache[key];
    if (e.tried) return e.tex;
    e.tried = true;

    auto it = g_index.find(key + ".png");
    if (it == g_index.end()) {
        LOG_INFO("Icone introuvable dans l'index pour '%s' (%s.png)", itemId, key.c_str());
        return 0;
    }
    e.tex = loadFrom(it->second);
    return e.tex;
}
