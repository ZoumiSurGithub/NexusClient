// Nexus Launcher - application graphique (Win32 + DirectX 11 + Dear ImGui).
// Fenetre stylisee facon client Nexus : nouveautes, selection de version (client + Minecraft),
// et lancement (injection si un client existe pour la version, sinon Minecraft seul).
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <dwmapi.h>
#include <thread>
#include <atomic>
#include <cmath>

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"

#include "Launcher.h"
#include "Credits.h"   // credits partages avec le client
#include "Updater.h"
#include "version.h"
#include "stb_image.h" // decodage des logos serveurs (implementation dans StbImageLauncher.cpp)

#include <map>
#include <string>
#include <fstream>
#include <filesystem>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")

// --- DirectX 11 (global, comme l'exemple officiel ImGui) ------------------------
static ID3D11Device*            g_device = nullptr;
static ID3D11DeviceContext*     g_context = nullptr;
static IDXGISwapChain*          g_swap = nullptr;
static ID3D11RenderTargetView*  g_rtv = nullptr;

// --- Logos serveurs : cache de textures D3D11 chargees depuis des fichiers image ----
struct LogoTex { ID3D11ShaderResourceView* srv = nullptr; int w = 0, h = 0; bool tried = false; };
static std::map<std::wstring, LogoTex> g_logos;

static const LogoTex& GetLogo(const std::wstring& path) {
    LogoTex& t = g_logos[path];
    if (t.tried) return t;                 // charge une seule fois (l'UI tourne a chaque frame)
    t.tried = true;
    if (path.empty() || !g_device) return t;
    // Lecture des octets via ifstream (gere les chemins larges), puis decodage en memoire :
    // evite tout probleme d'encodage de chemin de stbi_load/fopen.
    std::ifstream f(std::filesystem::path(path), std::ios::binary);
    std::string bytes;
    if (f) { f.seekg(0, std::ios::end); bytes.resize((size_t)f.tellg()); f.seekg(0);
             if (!bytes.empty()) f.read(&bytes[0], (std::streamsize)bytes.size()); }
    if (bytes.empty()) return t;
    int w = 0, h = 0, ch = 0;
    unsigned char* data = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()),
                                                (int)bytes.size(), &w, &h, &ch, 4);
    if (!data) return t;
    D3D11_TEXTURE2D_DESC d{}; d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sr{}; sr.pSysMem = data; sr.SysMemPitch = w * 4;
    ID3D11Texture2D* tex = nullptr;
    if (SUCCEEDED(g_device->CreateTexture2D(&d, &sr, &tex)) && tex) {
        g_device->CreateShaderResourceView(tex, nullptr, &t.srv);
        tex->Release();
        t.w = w; t.h = h;
    }
    stbi_image_free(data);
    return t;
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static void CreateRenderTarget() {
    ID3D11Texture2D* back = nullptr;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back) { g_device->CreateRenderTargetView(back, nullptr, &g_rtv); back->Release(); }
}
static void CleanupRenderTarget() { if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; } }

static bool CreateDevice(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    UINT flags = 0;
    D3D_FEATURE_LEVEL fl;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        levels, 2, D3D11_SDK_VERSION, &sd, &g_swap, &g_device, &fl, &g_context);
    if (hr == DXGI_ERROR_UNSUPPORTED)
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
            levels, 2, D3D11_SDK_VERSION, &sd, &g_swap, &g_device, &fl, &g_context);
    if (FAILED(hr)) return false;
    CreateRenderTarget();
    return true;
}
static void CleanupDevice() {
    CleanupRenderTarget();
    if (g_swap) { g_swap->Release(); g_swap = nullptr; }
    if (g_context) { g_context->Release(); g_context = nullptr; }
    if (g_device) { g_device->Release(); g_device = nullptr; }
}

static LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return true;
    switch (msg) {
        case WM_SIZE:
            if (g_device && wp != SIZE_MINIMIZED) {
                CleanupRenderTarget();
                g_swap->ResizeBuffers(0, LOWORD(lp), HIWORD(lp), DXGI_FORMAT_UNKNOWN, 0);
                CreateRenderTarget();
            }
            return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// --- Palette Nexus --------------------------------------------------------------
static const ImU32 kAccent   = IM_COL32(139,  92, 246, 255);  // violet Nexus (#8B5CF6)
static const ImU32 kAccentHi = IM_COL32(167, 139, 250, 255);  // #A78BFA
static const ImU32 kBgTop    = IM_COL32( 16,  15,  23, 255);
static const ImU32 kBgBot    = IM_COL32( 10,  10,  16, 255);
static const ImU32 kCard     = IM_COL32( 23,  22,  32, 255);
static const ImU32 kCardBord = IM_COL32(255, 255, 255,  14);

// --- Style de base (couleurs douces, coins arrondis) ----------------------------
static void ApplyNexusStyle() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 0.f; s.FrameRounding = 8.f; s.GrabRounding = 8.f;
    s.ChildRounding = 14.f; s.PopupRounding = 8.f;
    s.WindowPadding = ImVec2(26, 22); s.FramePadding = ImVec2(12, 9); s.ItemSpacing = ImVec2(12, 10);
    s.ScrollbarRounding = 8.f; s.ScrollbarSize = 11.f; s.WindowBorderSize = 0.f; s.ChildBorderSize = 1.f;
    s.PopupBorderSize = 0.f;
    ImVec4* c = s.Colors;
    const ImVec4 accent  = ImGui::ColorConvertU32ToFloat4(kAccent);
    const ImVec4 accentH = ImGui::ColorConvertU32ToFloat4(kAccentHi);
    c[ImGuiCol_WindowBg]      = ImVec4(0.055f,0.050f,0.080f,1.00f);
    c[ImGuiCol_ChildBg]       = ImGui::ColorConvertU32ToFloat4(kCard);
    c[ImGuiCol_PopupBg]       = ImVec4(0.10f, 0.095f,0.14f, 0.98f);
    c[ImGuiCol_Border]        = ImGui::ColorConvertU32ToFloat4(kCardBord);
    c[ImGuiCol_FrameBg]       = ImVec4(0.12f, 0.12f, 0.17f, 1.00f);
    c[ImGuiCol_FrameBgHovered]= ImVec4(0.17f, 0.16f, 0.24f, 1.00f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.20f, 0.18f, 0.30f, 1.00f);
    c[ImGuiCol_Button]        = ImVec4(0.18f, 0.17f, 0.26f, 1.00f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.24f, 0.22f, 0.34f, 1.00f);
    c[ImGuiCol_ButtonActive]  = ImVec4(0.28f, 0.24f, 0.40f, 1.00f);
    c[ImGuiCol_Header]        = ImVec4(0.20f, 0.18f, 0.30f, 1.00f);
    c[ImGuiCol_HeaderHovered] = accentH;
    c[ImGuiCol_HeaderActive]  = accent;
    c[ImGuiCol_CheckMark]     = accent;
    c[ImGuiCol_SliderGrab]    = accent;
    c[ImGuiCol_Separator]     = ImGui::ColorConvertU32ToFloat4(kCardBord);
    c[ImGuiCol_ScrollbarBg]   = ImVec4(0,0,0,0);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(1,1,1,0.10f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(1,1,1,0.18f);
    c[ImGuiCol_Text]          = ImVec4(0.93f, 0.93f, 0.96f, 1.00f);
    c[ImGuiCol_TextDisabled]  = ImVec4(0.52f, 0.52f, 0.62f, 1.00f);
}

// --- Petits composants "modernes" -----------------------------------------------
// Libelle de section : petites majuscules grisees, avec espacement.
static void SectionLabel(const char* txt) {
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(135,133,158,255));
    ImGui::SetWindowFontScale(0.82f);
    ImGui::TextUnformatted(txt);
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, 2));
}

// Valeur animee par widget : avance en douceur (ease exponentiel) vers 'target' a chaque image.
// Rend les survols/clics fluides malgre le mode immediat d'ImGui.
static float Anim(ImGuiID id, float target, float speed = 14.f) {
    static std::map<ImGuiID, float> store;
    float& v = store[id];
    float dt = ImGui::GetIO().DeltaTime; if (dt > 0.1f) dt = 0.1f;
    v += (target - v) * (1.f - std::exp(-speed * dt));
    if (std::fabs(v - target) < 0.0015f) v = target;
    return v;
}
static ImU32 LerpCol(ImU32 a, ImU32 b, float t) {
    const ImVec4 ca = ImGui::ColorConvertU32ToFloat4(a), cb = ImGui::ColorConvertU32ToFloat4(b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(ca.x + (cb.x - ca.x) * t, ca.y + (cb.y - ca.y) * t,
                                                 ca.z + (cb.z - ca.z) * t, ca.w + (cb.w - ca.w) * t));
}

// Logo Nexus facon "hero" : hexagone pointu a contour degrade (violet -> violet fonce),
// un "N", et deux noeuds (violet clair en haut-gauche, rose en bas-droite).
static void DrawNexusMark(ImDrawList* dl, ImVec2 c, float r) {
    const ImU32 cTL = IM_COL32(167,139,250,255), cMid = IM_COL32(139,92,246,255),
                cBR = IM_COL32(109, 40,217,255), cPink = IM_COL32(236, 72,153,255);
    auto grad = [&](float t){ t = t<0?0:(t>1?1:t);
        return t<0.5f ? LerpCol(cTL,cMid,t*2.f) : LerpCol(cMid,cBR,(t-0.5f)*2.f); };
    ImVec2 hx[6]; float tv[6];
    for (int i = 0; i < 6; ++i) { const float a = -1.5707963f + 1.0471976f*i;
        hx[i] = { c.x + std::cos(a)*r, c.y + std::sin(a)*r };
        tv[i] = 0.5f + ((hx[i].x-c.x)+(hx[i].y-c.y))/(2.4f*r); }
    const float hw = std::fmax(1.5f, r*0.11f);
    for (int i = 0; i < 6; ++i) { const int j=(i+1)%6; dl->AddLine(hx[i],hx[j], grad((tv[i]+tv[j])*0.5f), hw); }
    const float s = r/46.f, w = 8.f*s;                              // "N" (repere hero /46)
    const ImVec2 nBL(c.x-16*s,c.y+22*s), nTL(c.x-16*s,c.y-16*s), nBR(c.x+16*s,c.y+16*s), nTR(c.x+16*s,c.y-22*s);
    dl->AddLine(nBL,nTL,cMid,w); dl->AddLine(nTL,nBR,grad(0.62f),w); dl->AddLine(nBR,nTR,grad(0.8f),w);
    dl->AddCircleFilled(nBL,w*0.5f,cMid,12); dl->AddCircleFilled(nTR,w*0.5f,grad(0.8f),12);
    dl->AddCircleFilled(nTL,6.f*s,cTL,16);  dl->AddCircleFilled(nBR,6.f*s,cPink,16);  // noeuds
}

// Bouton principal arrondi, anime : halo + brillance au survol, leger ecrasement au clic.
static bool AccentButton(const char* id, const char* label, const ImVec2& size, bool enabled) {
    ImGui::BeginDisabled(!enabled);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(id, size);
    const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
    const ImGuiID iid = ImGui::GetItemID();
    const float hv = enabled ? Anim(iid, hov ? 1.f : 0.f) : 0.f;
    const float av = enabled ? Anim(iid + 1u, act ? 1.f : 0.f, 24.f) : 0.f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float ins = 1.8f * av;                                   // ecrasement au clic
    const ImVec2 p(p0.x + ins, p0.y + ins), pmax(p0.x + size.x - ins, p0.y + size.y - ins);
    const float r = 11.f;
    if (enabled && hv > 0.01f) {                                   // halo qui respire
        const float e = 5.f * hv;
        dl->AddRectFilled(ImVec2(p.x - e, p.y - e), ImVec2(pmax.x + e, pmax.y + e),
                          IM_COL32(143, 99, 235, (int)(70 * hv)), r + e);
    }
    ImU32 base = !enabled ? IM_COL32(48, 46, 64, 255) : LerpCol(kAccent, kAccentHi, hv);
    if (enabled) base = LerpCol(base, IM_COL32(120, 82, 210, 255), av);
    dl->AddRectFilled(p, pmax, base, r);
    dl->AddRect(p, pmax, IM_COL32(255, 255, 255, (int)(28 + 42 * hv)), r, 0, 1.2f);
    const ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(p0.x + (size.x - ts.x) * 0.5f, p0.y + (size.y - ts.y) * 0.5f),
                IM_COL32(255, 255, 255, enabled ? 255 : 140), label);
    ImGui::EndDisabled();
    return pressed && enabled;
}

// Bouton secondaire arrondi, anime (fond qui s'eclaircit au survol/clic).
static bool GhostButton(const char* label, const ImVec2& size, bool enabled) {
    ImGui::BeginDisabled(!enabled);
    ImVec2 sz = size; if (sz.x <= 0.f) sz.x = ImGui::GetContentRegionAvail().x;
    const bool pressed = ImGui::InvisibleButton(label, sz);
    const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
    const ImGuiID iid = ImGui::GetItemID();
    const float hv = enabled ? Anim(iid, hov ? 1.f : 0.f) : 0.f;
    const float av = enabled ? Anim(iid + 1u, act ? 1.f : 0.f, 24.f) : 0.f;
    const ImVec2 rmin = ImGui::GetItemRectMin(), rmax = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = 9.f;
    dl->AddRectFilled(rmin, rmax, IM_COL32(150, 140, 200, enabled ? (int)(26 + 38 * hv + 22 * av) : 14), r);
    dl->AddRect(rmin, rmax, IM_COL32(255, 255, 255, (int)(16 + 36 * hv)), r, 0, 1.f);
    const ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(rmin.x + (sz.x - ts.x) * 0.5f, rmin.y + (sz.y - ts.y) * 0.5f),
                IM_COL32(230, 230, 245, enabled ? 255 : 140), label);
    ImGui::EndDisabled();
    return pressed && enabled;
}

// --- Etat de l'application -------------------------------------------------------
struct App {
    Launcher::Config                  cfg;
    Launcher::Log                     log;
    std::vector<Launcher::McVersion>  mcVersions;
    std::vector<Launcher::ClientBuild> clients;
    std::vector<Launcher::ChangeEntry> changelog;
    std::vector<Launcher::ServerShortcut> servers;      // connexions rapides (config distante)
    std::vector<std::wstring> clientVersions;           // versions supportees par le client (distante)
    int   mcIndex = 0;
    HWND  hwnd = nullptr;       // fenetre du launcher (pour la cacher/restaurer pendant le jeu)
    std::atomic<bool> busy{ false };
    std::thread worker;        // thread du lancement en cours (injection)
    bool  wantRefresh = false; // demande de rechargement (traitee apres le rendu)
    int   tab = 0;             // onglet actif : 0 = Nouveautes, 1 = Parametres, 2 = Journal
    bool  autoScroll = true;   // journal : defilement automatique vers le bas

    std::thread fetchThread;                     // recuperation des versions en ligne (tache de fond)
    std::atomic<bool> onlineReady{ false };
    std::atomic<bool> fetching{ false };
    std::atomic<bool> dlOk{ false };             // installation terminee avec succes
    std::vector<Launcher::McVersion> onlinePending;
    std::wstring installedVer;                    // version Minecraft actuellement installee
    Launcher::Featured featuredPending;           // contenu distant en attente de fusion (fetch thread)
    std::vector<Launcher::ChangeEntry> changelogPending;   // changelog distant en attente

    void Refresh() {
        mcVersions = Launcher::EnumerateMcVersions(cfg);   // "Version active"
        clients    = Launcher::LoadClients(cfg, log);
        changelog  = Launcher::LoadChangelog(cfg);
        if (mcIndex >= (int)mcVersions.size()) mcIndex = 0;
        // En tache de fond (ne bloque pas l'UI) : version installee, versions en ligne, config distante.
        if (fetchThread.joinable()) fetchThread.join();
        onlineReady = false; fetching = true;
        fetchThread = std::thread([this] {
            std::wstring iv = Launcher::GetInstalledMcVersion();
            auto v = Launcher::FetchOnlineReleases(cfg, 0, log);   // 0 = toutes les versions
            auto feat = Launcher::FetchFeatured(cfg, log);          // serveurs + versions client (dev)
            auto cl = Launcher::FetchChangelog(cfg, log);           // changelog depuis GitHub
            installedVer = iv;
            onlinePending = std::move(v);
            featuredPending = std::move(feat);
            changelogPending = std::move(cl);
            fetching = false; onlineReady = true;
        });
    }

    // Fusionne les versions en ligne et marque celle qui est actuellement installee.
    void MergeOnline() {
        if (!installedVer.empty() && !mcVersions.empty() && mcVersions[0].active) {
            mcVersions[0].label = L"Version active (" + installedVer + L")";
            mcVersions[0].version = installedVer;        // pour la detection de compatibilite du client
            mcVersions[0].packageVersion = installedVer;
        }
        for (auto& ov : onlinePending) {
            // installedVer est la version de paquet (ex. "1.26.6301.0") -> comparer a packageVersion.
            ov.installed = (!installedVer.empty() && ov.packageVersion == installedVer);
            ov.label = ov.version + (ov.installed ? L" (installée)" : L" (à télécharger)");
            mcVersions.push_back(ov);
        }
        onlinePending.clear();

        // Contenu mis en avant (decide par le developpeur) : serveurs + versions du client.
        servers = std::move(featuredPending.servers);
        if (!featuredPending.clientVersions.empty()) {
            clientVersions = featuredPending.clientVersions;
            for (auto& c : clients) c.mcVersions = clientVersions;  // le dev decide, pas le joueur
        }
        featuredPending = Launcher::Featured{};

        // Changelog recupere depuis GitHub (toujours a jour).
        if (!changelogPending.empty()) changelog = std::move(changelogPending);
        changelogPending.clear();
    }
};

static std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

// Onglet de la barre laterale, anime : fond qui apparait et barre d'accent qui grandit.
static bool SidebarTab(const char* label, bool selected) {
    const ImVec2 sz(ImGui::GetContentRegionAvail().x, 38.f);
    const bool pressed = ImGui::InvisibleButton(label, sz);
    const bool hov = ImGui::IsItemHovered();
    const ImGuiID iid = ImGui::GetItemID();
    const float hv = Anim(iid, hov ? 1.f : 0.f);
    const float sv = Anim(iid + 1u, selected ? 1.f : 0.f, 16.f);
    const ImVec2 rmin = ImGui::GetItemRectMin(), rmax = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const int bgA = (int)(sv * 40 + hv * 16);
    if (bgA > 0) dl->AddRectFilled(rmin, rmax, IM_COL32(150, 120, 230, bgA), 8.f);
    const float barH = (sz.y - 16.f) * sv;                        // barre d'accent qui grandit
    if (barH > 0.5f) {
        const float cy = rmin.y + sz.y * 0.5f;
        dl->AddRectFilled(ImVec2(rmin.x, cy - barH * 0.5f), ImVec2(rmin.x + 3.f, cy + barH * 0.5f), kAccent, 2.f);
    }
    const float slide = 2.f * hv + 3.f * sv;                      // le texte glisse legerement
    ImVec4 tc(0.72f + 0.24f * sv, 0.72f + 0.23f * sv, 0.80f + 0.20f * sv, 1.f);
    const ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(rmin.x + 12.f + slide, rmin.y + (sz.y - ts.y) * 0.5f), ImGui::ColorConvertFloat4ToU32(tc), label);
    return pressed;
}

// Pastille "connexion rapide" compacte (logo seul, nom en infobulle). Renvoie true au clic.
static bool ServerTile(const Launcher::ServerShortcut& sv, float tile) {
    const LogoTex& logo = GetLogo(sv.logoPath);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool clicked = ImGui::InvisibleButton("##logo", ImVec2(tile, tile));
    const bool hov = ImGui::IsItemHovered();
    const float hv = Anim(ImGui::GetItemID(), hov ? 1.f : 0.f);
    const float grow = 2.f * hv;                                   // leger soulevement au survol
    const ImVec2 q(p.x - grow, p.y - grow), pmax(p.x + tile + grow, p.y + tile + grow);
    const float r = 9.f;
    if (hv > 0.01f)
        dl->AddRectFilled(ImVec2(q.x - 2, q.y + 2), ImVec2(pmax.x + 2, pmax.y + 3), IM_COL32(143, 99, 235, (int)(45 * hv)), r + 2);
    dl->AddRectFilled(q, pmax, LerpCol(IM_COL32(40, 35, 66, 255), IM_COL32(64, 55, 100, 255), hv), r);
    if (logo.srv) {
        // "Cover" : remplit la pastille carree sans deformer le logo (recadrage centre).
        ImVec2 uv0(0, 0), uv1(1, 1);
        if (logo.w > 0 && logo.h > 0 && logo.w != logo.h) {
            const float ar = (float)logo.w / (float)logo.h;
            if (ar > 1.f) { const float crop = (1.f - 1.f / ar) * 0.5f; uv0.x = crop; uv1.x = 1.f - crop; }
            else          { const float crop = (1.f - ar)       * 0.5f; uv0.y = crop; uv1.y = 1.f - crop; }
        }
        dl->AddImageRounded((ImTextureID)logo.srv, q, pmax, uv0, uv1, IM_COL32_WHITE, r);
    } else {
        dl->AddRect(q, pmax, kAccent, r, 0, 1.4f);
        const float cxp = (q.x + pmax.x) * 0.5f, cyp = (q.y + pmax.y) * 0.5f;
        const float uw = tile * 0.52f, uh = tile * 0.17f, ug = tile * 0.08f;
        for (int k = 0; k < 2; ++k) {
            const float y0 = cyp - uh - ug * 0.5f + k * (uh + ug);
            dl->AddRectFilled(ImVec2(cxp - uw * 0.5f, y0), ImVec2(cxp + uw * 0.5f, y0 + uh), IM_COL32(150, 140, 200, 255), 2.f);
            dl->AddCircleFilled(ImVec2(cxp - uw * 0.5f + 4.f, y0 + uh * 0.5f), 1.6f, kAccent, 8);
        }
    }
    if (hv > 0.01f) dl->AddRect(q, pmax, IM_COL32(255, 255, 255, (int)(90 * hv)), r, 0, 1.4f);
    if (hov) ImGui::SetTooltip("%s\n%s:%d", Narrow(sv.name).c_str(), Narrow(sv.address).c_str(), sv.port);
    return clicked;
}

static void DrawUi(App& app) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::GetBackgroundDrawList()->AddRectFilledMultiColor(
        vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y), kBgTop, kBgTop, kBgBot, kBgBot);
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::Begin("##nexus", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleColor();

    const bool busy = app.busy.load();
    const Launcher::McVersion* selMc = app.mcVersions.empty() ? nullptr : &app.mcVersions[app.mcIndex];
    const bool needInstall = selMc && !selMc->active && !selMc->installed;
    const Launcher::ClientBuild* client = selMc ? Launcher::FindClientForMc(app.clients, *selMc) : nullptr;
    const bool willInject = client && app.cfg.autoInject;

    auto startLaunch = [&](const std::wstring& serverUri) {
        if (busy || !selMc) return;
        Launcher::LaunchRequest req;
        req.cfg = app.cfg; req.mc = *selMc; req.window = app.hwnd; req.needInstall = needInstall;
        if (willInject) { req.client = *client; req.hasClient = true; }
        req.serverUri = serverUri;
        app.busy = true; app.dlOk = false;
        if (app.worker.joinable()) app.worker.join();
        app.worker = std::thread([&app, req] { Launcher::RunFlow(req, app.log, app.busy, app.dlOk); });
    };

    // ===================== Barre laterale =====================
    const float sideW = 288.f;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 18));
    ImGui::BeginChild("##sidebar", ImVec2(sideW, 0), ImGuiChildFlags_Border,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        // Logo + nom + version
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const ImVec2 bc(p.x + 18.f, p.y + 17.f);
        const float r = 18.f;
        DrawNexusMark(dl, bc, r);
        ImGui::SetCursorScreenPos(ImVec2(p.x + 42.f, p.y - 2.f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.96f, 0.95f, 1.f, 1.f));
        ImGui::SetWindowFontScale(1.35f); ImGui::TextUnformatted("Nexus"); ImGui::SetWindowFontScale(1.f);
        ImGui::PopStyleColor();
        ImGui::SetCursorScreenPos(ImVec2(p.x + 43.f, p.y + 19.f));
        ImGui::TextDisabled("Launcher v%s", Narrow(NEXUS_VERSION_W).c_str());
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + 44.f));

        ImGui::Dummy(ImVec2(0, 8));
        if (SidebarTab("Nouveautés", app.tab == 0)) app.tab = 0;
        if (SidebarTab("Paramètres", app.tab == 1)) app.tab = 1;
        if (SidebarTab("Journal",    app.tab == 2)) app.tab = 2;

        // ---- Bas : statut client + version + bouton lancer + fleche version ----
        const float bottomH = 126.f;
        if (ImGui::GetWindowHeight() - bottomH > ImGui::GetCursorPosY())
            ImGui::SetCursorPosY(ImGui::GetWindowHeight() - bottomH);

        {   // statut client
            const ImU32 dot = needInstall ? IM_COL32(120, 170, 240, 255) : willInject ? IM_COL32(90, 210, 120, 255) : IM_COL32(240, 185, 80, 255);
            const ImVec4 col = needInstall ? ImVec4(0.55f, 0.72f, 0.96f, 1.f) : willInject ? ImVec4(0.60f, 0.88f, 0.62f, 1.f) : ImVec4(0.96f, 0.76f, 0.36f, 1.f);
            std::string txt = needInstall ? "Version a installer"
                            : willInject  ? (client ? Narrow(client->name) : "Client actuel")
                                          : "Minecraft seul";
            if (txt.size() > 30) txt = txt.substr(0, 29) + "\xE2\x80\xA6";
            const ImVec2 cp = ImGui::GetCursorScreenPos();
            dl->AddCircleFilled(ImVec2(cp.x + 5.f, cp.y + ImGui::GetTextLineHeight() * 0.5f), 5.f, dot, 16);
            ImGui::SetCursorScreenPos(ImVec2(cp.x + 16.f, cp.y));
            ImGui::PushStyleColor(ImGuiCol_Text, col); ImGui::TextUnformatted(txt.c_str()); ImGui::PopStyleColor();
        }
        {   // version courante
            std::string vlabel;
            if (!selMc) vlabel = "(aucune version)";
            else if (selMc->active) {
                std::string vnum = !selMc->packageVersion.empty() ? Narrow(selMc->packageVersion)
                                 : !selMc->version.empty()        ? Narrow(selMc->version)
                                                                  : Narrow(app.installedVer);
                vlabel = vnum.empty() ? "Version active" : "Version active (" + vnum + ")";
            } else vlabel = Narrow(selMc->label);
            if (vlabel.size() > 34) vlabel = vlabel.substr(0, 33) + "\xE2\x80\xA6";
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.60f, 0.60f, 0.70f, 1.f));
            ImGui::TextUnformatted(vlabel.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::Dummy(ImVec2(0, 4));

        std::string busyLabel;
        if (busy) { busyLabel = Narrow(app.log.Status()); if (busyLabel.empty()) busyLabel = "Opération\xE2\x80\xA6"; if (busyLabel.size() > 22) busyLabel = busyLabel.substr(0, 21) + "\xE2\x80\xA6"; }
        const char* btn = busy ? busyLabel.c_str() : needInstall ? "Installer et jouer" : willInject ? "Lancer avec Nexus" : "Lancer Minecraft seul";
        const float arrowW = 34.f, lgap = 6.f;
        const float lbw = ImGui::GetContentRegionAvail().x - arrowW - lgap;
        if (AccentButton("##launch", btn, ImVec2(lbw, 44), !busy && selMc)) startLaunch(L"");
        ImGui::SameLine(0, lgap);
        if (GhostButton("\xE2\x96\xBC", ImVec2(arrowW, 44), !busy)) ImGui::OpenPopup("##verspick");
        if (ImGui::BeginPopup("##verspick")) {
            ImGui::TextDisabled("Version de Minecraft%s", app.fetching ? "  (chargement\xE2\x80\xA6)" : "");
            ImGui::Separator();
            ImGui::BeginChild("##verslist", ImVec2(320, 340), false);
            for (int i = 0; i < (int)app.mcVersions.size(); ++i) {
                const bool sel = (i == app.mcIndex);
                if (ImGui::Selectable(Narrow(app.mcVersions[i].label).c_str(), sel)) { app.mcIndex = i; ImGui::CloseCurrentPopup(); }
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndChild();
            ImGui::Separator();
            if (ImGui::SmallButton("Actualiser la liste")) { app.wantRefresh = true; ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();

    // ===================== Zone principale =====================
    ImGui::SameLine(0, 14);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22, 20));
    ImGui::BeginChild("##main", ImVec2(0, 0), ImGuiChildFlags_Border);
    {
        ImDrawList* d2 = ImGui::GetWindowDrawList();
        auto title = [&](const char* t) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.96f, 0.95f, 1.f, 1.f));
            ImGui::SetWindowFontScale(1.2f); ImGui::TextUnformatted(t); ImGui::SetWindowFontScale(1.f);
            ImGui::PopStyleColor();
            ImGui::Dummy(ImVec2(0, 6));
        };

        // Connexions rapides : bande discrete (petites pastilles logo) centree, quel que soit l'onglet.
        if (!app.servers.empty()) {
            const float tile = 34.f, pad = 16.f;
            const float avail = ImGui::GetContentRegionAvail().x;
            const int   n = (int)app.servers.size();
            int perRow = (int)((avail + pad) / (tile + pad));
            if (perRow < 1) perRow = 1; if (perRow > n) perRow = n;
            const float baseX = ImGui::GetCursorPosX();
            for (int idx = 0; idx < n; ) {
                const int cnt = (n - idx < perRow) ? (n - idx) : perRow;   // pastilles sur cette ligne
                const float rowW = cnt * tile + (cnt - 1) * pad;
                const float sx = baseX + (avail - rowW) * 0.5f;            // centrage horizontal
                for (int c = 0; c < cnt; ++c, ++idx) {
                    if (c == 0) ImGui::SetCursorPosX(sx > baseX ? sx : baseX);
                    else        ImGui::SameLine(0, pad);
                    ImGui::PushID(idx);
                    if (ServerTile(app.servers[idx], tile) && !busy && selMc) startLaunch(Launcher::ConnectUri(app.servers[idx]));
                    ImGui::PopID();
                }
            }
            ImGui::Dummy(ImVec2(0, 8));
            ImGui::Separator();
            ImGui::Dummy(ImVec2(0, 8));
        }

        if (app.tab == 0) {
            // -------- Nouveautes --------
            title("Nouveautés");
            if (app.changelog.empty())
                ImGui::TextDisabled("Aucune note de version (CHANGELOG.md introuvable).");
            for (size_t i = 0; i < app.changelog.size(); ++i) {
                const auto& e = app.changelog[i];
                if (i) ImGui::Dummy(ImVec2(0, 10));
                const std::string ver = Narrow(e.version);
                const ImVec2 tp = ImGui::GetCursorScreenPos();
                const ImVec2 ts = ImGui::CalcTextSize(ver.c_str());
                const ImVec2 pad(10, 4);
                d2->AddRectFilled(tp, ImVec2(tp.x + ts.x + pad.x * 2, tp.y + ts.y + pad.y * 2), IM_COL32(143, 99, 235, 45), 6.f);
                d2->AddText(ImVec2(tp.x + pad.x, tp.y + pad.y), IM_COL32(200, 180, 255, 255), ver.c_str());
                ImGui::Dummy(ImVec2(ts.x + pad.x * 2, ts.y + pad.y * 2));
                if (!e.date.empty()) { ImGui::SameLine(0, 10); ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("%s", Narrow(e.date).c_str()); }
                ImGui::Dummy(ImVec2(0, 4));
                for (const auto& n : e.notes) {
                    const ImVec2 bp = ImGui::GetCursorScreenPos();
                    d2->AddCircleFilled(ImVec2(bp.x + 4, bp.y + ImGui::GetTextLineHeight() * 0.5f), 2.5f, kAccent, 12);
                    ImGui::Indent(14.f); ImGui::TextWrapped("%s", Narrow(n).c_str()); ImGui::Unindent(14.f);
                }
            }
        }
        else if (app.tab == 1) {
            // -------- Parametres du launcher --------
            title("Paramètres du launcher");
            bool ch = false;
            ch |= ImGui::Checkbox("Cacher le launcher pendant le jeu", &app.cfg.hideWhilePlaying);
            ch |= ImGui::Checkbox("Injecter le client Nexus quand il est compatible", &app.cfg.autoInject);
            ch |= ImGui::Checkbox("Centrer la fenêtre au démarrage", &app.cfg.centerOnStart);
            if (ch) Launcher::SaveLauncherSettings(app.cfg);
            ImGui::Dummy(ImVec2(0, 10));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.60f, 0.60f, 0.70f, 1.f));
            ImGui::TextWrapped("Les serveurs mis en avant et les versions de Minecraft prises en charge par le client sont definis par le developpeur et recuperes automatiquement au lancement.");
            ImGui::PopStyleColor();

            // Credits en bas
            const float footerH = 94.f;
            if (ImGui::GetCursorPosY() < ImGui::GetWindowHeight() - footerH) ImGui::SetCursorPosY(ImGui::GetWindowHeight() - footerH);
            ImGui::Separator(); ImGui::Dummy(ImVec2(0, 4));
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(135, 133, 158, 255));
            ImGui::SetWindowFontScale(0.82f); ImGui::TextUnformatted("CREDITS"); ImGui::SetWindowFontScale(1.f);
            ImGui::PopStyleColor();
            ImGui::Dummy(ImVec2(0, 2));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.72f, 0.72f, 0.80f, 1.f));
            ImGui::Text("Créé par  %s", Credits::kCreatedBy);
            ImGui::Text("Signatures  %s", Credits::kSignaturesBy);
            ImGui::Text("Versions  %s", Credits::kVersionsBy);
            ImGui::PopStyleColor();
        }
        else {
            // -------- Journal --------
            title("Journal");
            const auto logLines = app.log.Snapshot();
            ImGui::Checkbox("Défilement automatique", &app.autoScroll);
            ImGui::SameLine();
            ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - 150.f);
            if (ImGui::SmallButton("Copier")) {
                std::string all; for (const auto& ln : logLines) { all += Narrow(ln.text); all += "\n"; }
                ImGui::SetClipboardText(all.c_str());
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Effacer")) app.log.Clear();
            ImGui::Dummy(ImVec2(0, 4));
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0.25f));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 10));
            ImGui::BeginChild("##log", ImVec2(0, 0), ImGuiChildFlags_Border, ImGuiWindowFlags_HorizontalScrollbar);
            for (const auto& ln : logLines) {
                ImVec4 col(0.80f, 0.80f, 0.88f, 1.f);
                switch (ln.level) {
                    case Launcher::Log::Level::Good:  col = ImVec4(0.60f, 0.88f, 0.62f, 1.f); break;
                    case Launcher::Log::Level::Warn:  col = ImVec4(0.96f, 0.76f, 0.36f, 1.f); break;
                    case Launcher::Log::Level::Error: col = ImVec4(0.96f, 0.48f, 0.48f, 1.f); break;
                    default: break;
                }
                ImGui::PushStyleColor(ImGuiCol_Text, col);
                ImGui::TextWrapped("%s", Narrow(ln.text).c_str());
                ImGui::PopStyleColor();
            }
            if (app.autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 2.f) ImGui::SetScrollHereY(1.f);
            ImGui::EndChild();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::End();
}

// --- Mises a jour obligatoires au demarrage (silencieux : pas de console) --------
static bool RunUpdater(const fs::path& exeDir, const wchar_t* selfExe) {
    Updater::CleanupOldFiles(exeDir);
    switch (Updater::Run(exeDir, selfExe)) {
        case Updater::Result::Updated: return false;  // le nouveau launcher a pris le relais : quitter
        case Updater::Result::Failed:
            MessageBoxW(nullptr, L"Nexus ne peut pas démarrer sans la dernière mise à jour.",
                        L"Nexus Launcher", MB_ICONERROR | MB_OK);
            return false;
        default: return true;  // a jour / hors ligne / version de dev : on continue
    }
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    const fs::path exeDir = fs::path(exePath).parent_path();

    if (!RunUpdater(exeDir, exePath)) return 0;

    // Fenetre
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"NexusLauncherWnd";
    RegisterClassExW(&wc);
    // Fenetre centree sur la zone de travail (sauf si l'option est desactivee dans launcher.ini).
    const int winW = 980, winH = 700;
    const fs::path lini = exeDir / L"launcher.ini";
    const bool center = GetPrivateProfileIntW(L"launcher", L"center_on_start", 1, lini.c_str()) != 0;
    int winX = CW_USEDEFAULT, winY = CW_USEDEFAULT;
    if (center) {
        RECT wa{}; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        winX = wa.left + ((wa.right - wa.left) - winW) / 2;
        winY = wa.top  + ((wa.bottom - wa.top) - winH) / 2;
    }
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Nexus Launcher",
        WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX, winX, winY, winW, winH,
        nullptr, nullptr, hInst, nullptr);

    if (!CreateDevice(hwnd)) { CleanupDevice(); DestroyWindow(hwnd); UnregisterClassW(wc.lpszClassName, hInst); return 1; }

    // Barre de titre sombre (Windows 10/11)
    BOOL dark = TRUE; DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // Police : Segoe UI si disponible (accents corrects), sinon police par defaut.
    {
        static const ImWchar kRanges[] = {
            0x0020, 0x00FF, // latin + latin-1 (accents francais)
            0x2010, 0x2027, // tirets, apostrophes/guillemets courbes, puce, points de suspension
            0x20AC, 0x20AC, // euro
            0x2190, 0x2193, // fleches
            0x25A0, 0x25CF, // carres / ronds pleins
            0,
        };
        wchar_t win[MAX_PATH]; GetWindowsDirectoryW(win, MAX_PATH);
        const std::string seg = Narrow(std::wstring(win) + L"\\Fonts\\segoeui.ttf");
        if (GetFileAttributesA(seg.c_str()) != INVALID_FILE_ATTRIBUTES)
            io.Fonts->AddFontFromFileTTF(seg.c_str(), 19.f, nullptr, kRanges);
    }
    ApplyNexusStyle();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    App app;
    app.hwnd = hwnd;
    app.cfg = Launcher::LoadConfig(exeDir);
    app.Refresh();
    app.log.Info(L"Pret. Choisis une version et clique sur Lancer.");

    bool running = true;
    while (running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) running = false;
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        if (!running) break;

        // Cachee (Minecraft en cours) : on ne rend rien pour economiser les ressources.
        if (!IsWindowVisible(hwnd)) { Sleep(100); continue; }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        DrawUi(app);
        ImGui::Render();

        if (app.wantRefresh && !app.busy) { app.Refresh(); app.wantRefresh = false; }
        if (app.onlineReady) {                       // versions en ligne pretes : on les fusionne
            app.MergeOnline();
            app.onlineReady = false;
            if (app.fetchThread.joinable()) app.fetchThread.join();
        }
        if (app.dlOk && !app.busy) { app.dlOk = false; app.wantRefresh = true; }  // version installee -> recharger

        const float clear[4] = { 0.05f, 0.05f, 0.07f, 1.0f };
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_context->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swap->Present(1, 0);  // vsync
    }

    if (app.worker.joinable()) app.worker.join();         // attend la fin d'un lancement en cours
    if (app.fetchThread.joinable()) app.fetchThread.join();

    Launcher::ClearCache(app.cfg);                        // efface les caches temporaires a la fermeture

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDevice();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, hInst);
    return 0;
}
