#include "SigScan.h"
#include "Logger.h"
#include <windows.h>
#include <vector>
#include <optional>
#include <sstream>

namespace {
    struct Section { uintptr_t start; size_t size; };

    Section GetTextSection() {
        auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)); // Minecraft.Windows.exe
        auto dos = reinterpret_cast<PIMAGE_DOS_HEADER>(base);
        auto nt = reinterpret_cast<PIMAGE_NT_HEADERS>(base + dos->e_lfanew);
        auto sec = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
            if (memcmp(sec->Name, ".text", 5) == 0)
                return { base + sec->VirtualAddress, sec->Misc.VirtualSize };
        }
        return { base, nt->OptionalHeader.SizeOfImage };
    }

    std::vector<std::optional<uint8_t>> Parse(const std::string& sig) {
        std::vector<std::optional<uint8_t>> bytes;
        std::istringstream ss(sig);
        for (std::string tok; ss >> tok;) {
            if (tok[0] == '?') bytes.emplace_back(std::nullopt);
            else bytes.emplace_back(static_cast<uint8_t>(std::stoul(tok, nullptr, 16)));
        }
        return bytes;
    }

    uintptr_t Scan(const std::vector<std::optional<uint8_t>>& pattern) {
        static const Section text = GetTextSection();
        const auto* data = reinterpret_cast<const uint8_t*>(text.start);
        const size_t n = pattern.size();
        if (n == 0 || n > text.size) return 0;

        for (size_t i = 0; i <= text.size - n; ++i) {
            size_t j = 0;
            for (; j < n; ++j) {
                if (pattern[j] && data[i + j] != *pattern[j]) break;
            }
            if (j == n) return text.start + i;
        }
        return 0;
    }

    uintptr_t ResolveRel(uintptr_t addr, int relOffset, int instrLen) {
        int32_t rel = *reinterpret_cast<int32_t*>(addr + relOffset);
        return addr + instrLen + rel;
    }
}

uintptr_t SigScan::Find(const std::string& signature, const char* name) {
    if (signature.empty()) { LOG_WARN("Signature '%s' non renseignee dans offsets.ini", name); return 0; }

    std::string sig = signature;
    enum class Mode { Direct, Call, Ref } mode = Mode::Direct;
    int relOff = 0, instrLen = 0;

    try {
        if (sig.rfind("call:", 0) == 0) { mode = Mode::Call; sig.erase(0, 5); }
        else if (sig.rfind("ref:", 0) == 0) {
            mode = Mode::Ref;
            sig.erase(0, 4);
            size_t p1 = sig.find(':'); relOff = std::stoi(sig.substr(0, p1)); sig.erase(0, p1 + 1);
            size_t p2 = sig.find(':'); instrLen = std::stoi(sig.substr(0, p2)); sig.erase(0, p2 + 1);
        }

        uintptr_t addr = Scan(Parse(sig));
        if (!addr) { LOG_ERROR("Signature '%s' introuvable (version du jeu differente ?)", name); return 0; }

        if (mode == Mode::Call) addr = ResolveRel(addr, 1, 5);
        else if (mode == Mode::Ref) addr = ResolveRel(addr, relOff, instrLen);

        LOG_INFO("Signature '%s' -> Minecraft.Windows.exe+0x%llX", name,
                 static_cast<unsigned long long>(addr - reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))));
        return addr;
    } catch (...) {
        LOG_ERROR("Signature '%s' mal formee : '%s'", name, signature.c_str());
        return 0;
    }
}
