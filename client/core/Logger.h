#pragma once
#include <string>
#include <vector>
#include <filesystem>

namespace Logger {
    enum class Level { Info, Warn, Error };

    struct Entry {
        std::string time;     // "23:20:44"
        Level       level;
        std::string message;
    };

    void Init(const std::filesystem::path& file);
    void Shutdown();
    void Write(const char* level, const char* fmt, ...);

    // Journal en memoire (1000 dernieres lignes) pour l'onglet "Journal" du menu
    void Snapshot(std::vector<Entry>& out);   // copie thread-safe
    size_t Version();                         // change a chaque nouvelle ligne
    void ClearView();                         // vide l'affichage (le fichier est conserve)
    const std::filesystem::path& FilePath();
}

#define LOG_INFO(...)  ::Logger::Write("INFO", __VA_ARGS__)
#define LOG_WARN(...)  ::Logger::Write("WARN", __VA_ARGS__)
#define LOG_ERROR(...) ::Logger::Write("ERREUR", __VA_ARGS__)
