#include "ThirdPersonNametag.h"
#include "core/Config.h"
#include "core/SigScan.h"
#include "core/Logger.h"
#include "game/ItemRender.h"
#include <windows.h>
#include <cstring>

// La signature pointe sur un saut conditionnel "74 ??" (JE, 2 octets) qui saute
// le rendu du nametag du joueur local. On remplace ces 2 octets par deux NOP (90 90)
// pour que le nametag s'affiche. Mais UNIQUEMENT en 3e personne : on lit la
// perspective chaque frame (Options::getPerspective, 0 = 1ere personne) et on
// applique / retire le patch en consequence.
namespace {
    constexpr size_t kPatchSize = 2;
    void*    g_addr = nullptr;
    uint8_t  g_orig[kPatchSize] = {};
    bool     g_patched = false;
    using PerspFn = int (__fastcall*)(void*);
    PerspFn  g_getPersp = nullptr;

    void WriteBytes(void* addr, const void* src, size_t n) {
        DWORD prot = 0;
        if (VirtualProtect(addr, n, PAGE_EXECUTE_READWRITE, &prot)) {
            std::memcpy(addr, src, n);
            VirtualProtect(addr, n, prot, &prot);
            FlushInstructionCache(GetCurrentProcess(), addr, n);
        }
    }
    void SetPatched(bool on) {
        if (!g_addr || on == g_patched) return;
        if (on) { const uint8_t nops[kPatchSize] = { 0x90, 0x90 }; WriteBytes(g_addr, nops, kPatchSize); }
        else    { WriteBytes(g_addr, g_orig, kPatchSize); }
        g_patched = on;
    }
    // Perspective : 0 = 1ere personne, 1/2 = 3e personne, -1 = inconnu.
    int ReadPerspective() {
        if (!g_getPersp) return -1;
        void* ci = ItemRender::ClientInstance();
        if (!ci) return -1;
        __try {
            void* opts = *reinterpret_cast<void**>(reinterpret_cast<char*>(ci) + Config::GetOffsets().optionsOffset);
            if (!opts) return -1;
            return g_getPersp(opts);
        } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    }
}

bool ThirdPersonNametag::Init() {
    g_addr = reinterpret_cast<void*>(
        SigScan::Find(Config::GetOffsets().tpNametagSig, "ThirdPersonNametag"));
    if (!g_addr) { LOG_WARN("Nametag 3e personne : signature introuvable"); return false; }
    std::memcpy(g_orig, g_addr, kPatchSize);
    g_getPersp = reinterpret_cast<PerspFn>(
        SigScan::Find(Config::GetOffsets().getPerspectiveSig, "Options::getPerspective"));
    if (!g_getPersp) LOG_WARN("Nametag 3e personne : getPerspective introuvable (patch sans filtre de vue)");
    LOG_INFO("Nametag 3e personne : pret (0x%llX)", reinterpret_cast<unsigned long long>(g_addr));
    return true;
}

void ThirdPersonNametag::Update() {
    if (!g_addr) return;
    if (!Config::GetSettings().thirdPersonNametag) { SetPatched(false); return; }
    const int persp = ReadPerspective();
    // Patch seulement en 3e personne (persp >= 1). Repli : patch si vue illisible.
    SetPatched(g_getPersp ? (persp >= 1) : true);
}

void ThirdPersonNametag::Shutdown() {
    SetPatched(false);
}

bool ThirdPersonNametag::IsConfigured() { return g_addr != nullptr; }
