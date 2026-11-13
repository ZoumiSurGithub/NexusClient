// Menu Nexus : barre laterale de categories + modules sous forme de cartes
// avec interrupteurs animes. Couleur d'accent personnalisable (Parametres).
#define IMGUI_DEFINE_MATH_OPERATORS
#include "Ui.h"
#include "Client.h"
#include "Credits.h"
#include "core/Config.h"
#include "core/Logger.h"
#include "game/Game.h"
#include "modules/AutoSprint.h"
#include "hooks/MouseBlock.h"
#include "hooks/InputHook.h"
#include "modules/HealthBars.h"
#include "modules/Zoom.h"
#include "modules/OwnNametag.h"
#include "modules/ChatSender.h"
#include "modules/ChatOverlay.h"
#include "modules/DiscordRPC.h"
#include "modules/Network.h"
#include <windows.h>
#include <shellapi.h>   // apres windows.h (ShellExecuteW)
#include <imgui.h>
#include <imgui_stdlib.h>   // apres imgui.h
#include <atomic>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cctype>
#include <vector>

namespace {
    std::atomic<bool> g_menuOpen{ false };
    bool g_wasOpen = false;
    int  g_tab = 0;

    float g_cardX0 = 0.f, g_cardW = 0.f; // geometrie de la carte en cours (pour aligner les lignes)

    // ---- Couleurs ---------------------------------------------------------------
    ImVec4 Accent(float a = 1.f) {
        const auto& c = Config::GetSettings().accent;
        return { c[0], c[1], c[2], a };
    }
    ImVec4 Mix(const ImVec4& a, const ImVec4& b, float t) {
        return { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t };
    }
    ImU32 U32(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }

    // Palette Nexus : fonds sombres legerement teintes de violet
    const ImVec4 kBg       (0.066f, 0.058f, 0.094f, 0.97f);
    const ImVec4 kSidebar  (0.047f, 0.040f, 0.072f, 1.00f);
    const ImVec4 kCard     (0.102f, 0.090f, 0.148f, 1.00f);
    const ImVec4 kFrame    (0.160f, 0.142f, 0.225f, 1.00f);
    const ImVec4 kFrameHov (0.205f, 0.182f, 0.285f, 1.00f);
    const ImVec4 kText     (0.945f, 0.935f, 0.975f, 1.00f);
    const ImVec4 kTextDim  (0.565f, 0.540f, 0.665f, 1.00f);

    void ApplyTheme() {
        ImGuiStyle& s = ImGui::GetStyle();
        s.WindowRounding = 12.f; s.ChildRounding = 10.f; s.FrameRounding = 6.f;
        s.GrabRounding = 6.f; s.PopupRounding = 8.f; s.ScrollbarRounding = 6.f;
        s.WindowBorderSize = 0.f; s.ChildBorderSize = 0.f; s.FrameBorderSize = 0.f;
        s.ItemSpacing = { 8.f, 8.f }; s.FramePadding = { 10.f, 6.f };
        s.GrabMinSize = 12.f; s.ScrollbarSize = 10.f;

        ImVec4* c = s.Colors;
        c[ImGuiCol_Text]                 = kText;
        c[ImGuiCol_TextDisabled]         = kTextDim;
        c[ImGuiCol_WindowBg]             = kBg;
        c[ImGuiCol_ChildBg]              = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_PopupBg]              = kCard;
        c[ImGuiCol_Border]               = ImVec4(1, 1, 1, 0.06f);
        c[ImGuiCol_FrameBg]              = kFrame;
        c[ImGuiCol_FrameBgHovered]       = kFrameHov;
        c[ImGuiCol_FrameBgActive]        = kFrameHov;
        c[ImGuiCol_SliderGrab]           = Accent();
        c[ImGuiCol_SliderGrabActive]     = Mix(Accent(), ImVec4(1, 1, 1, 1), 0.25f);
        c[ImGuiCol_CheckMark]            = Accent();
        c[ImGuiCol_Button]               = kFrame;
        c[ImGuiCol_ButtonHovered]        = kFrameHov;
        c[ImGuiCol_ButtonActive]         = Accent(0.8f);
        c[ImGuiCol_Header]               = Accent(0.25f);
        c[ImGuiCol_HeaderHovered]        = Accent(0.35f);
        c[ImGuiCol_HeaderActive]         = Accent(0.45f);
        c[ImGuiCol_Separator]            = ImVec4(1, 1, 1, 0.07f);
        c[ImGuiCol_ScrollbarBg]          = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_ScrollbarGrab]        = kFrame;
        c[ImGuiCol_ScrollbarGrabHovered] = kFrameHov;
        c[ImGuiCol_ScrollbarGrabActive]  = Accent(0.8f);
    }

    // ---- Widgets ----------------------------------------------------------------
    // Interrupteur anime facon smartphone
    bool ToggleSwitch(const char* id, bool* v, bool enabled = true) {
        const float h = ImGui::GetFrameHeight() * 0.82f, w = h * 1.8f;
        const ImVec2 p = ImGui::GetCursorScreenPos();

        bool clicked = ImGui::InvisibleButton(id, { w, h }) && enabled;
        if (clicked) *v = !*v;
        const bool hovered = enabled && ImGui::IsItemHovered();

        float* t = ImGui::GetStateStorage()->GetFloatRef(ImGui::GetItemID(), *v ? 1.f : 0.f);
        *t += ((*v ? 1.f : 0.f) - *t) * std::min(1.f, ImGui::GetIO().DeltaTime * 14.f);

        const float a = enabled ? 1.f : 0.35f;
        ImVec4 bg = Mix(ImVec4(0.24f, 0.25f, 0.29f, a), Accent(a), *t);
        if (hovered) bg = Mix(bg, ImVec4(1, 1, 1, a), 0.08f);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, p + ImVec2(w, h), U32(bg), h * 0.5f);
        const float r = h * 0.5f - 3.f;
        dl->AddCircleFilled({ p.x + h * 0.5f + (w - h) * *t, p.y + h * 0.5f }, r, IM_COL32(255, 255, 255, int(255 * a)));
        return clicked;
    }

    // Selecteur a boutons (remplace les listes deroulantes)
    bool Segmented(const char* id, int* v, const char* const* items, int n) {
        ImGui::PushID(id);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, { 4.f, 0.f });
        const float w = (ImGui::GetContentRegionAvail().x - 4.f * (n - 1)) / n;
        bool changed = false;
        for (int i = 0; i < n; ++i) {
            if (i) ImGui::SameLine();
            const bool sel = *v == i;
            ImGui::PushStyleColor(ImGuiCol_Button, sel ? Accent(0.85f) : kFrame);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, sel ? Accent() : kFrameHov);
            ImGui::PushStyleColor(ImGuiCol_Text, sel ? ImVec4(0.05f, 0.05f, 0.06f, 1.f) : kText);
            if (ImGui::Button(items[i], { w, 0.f })) { *v = i; changed = true; }
            ImGui::PopStyleColor(3);
        }
        ImGui::PopStyleVar();
        ImGui::PopID();
        return changed;
    }

    bool SidebarButton(const char* label, bool selected) {
        const ImVec2 size(ImGui::GetContentRegionAvail().x, 36.f);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const bool pressed = ImGui::InvisibleButton(label, size);
        const bool hovered = ImGui::IsItemHovered();

        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (selected)     dl->AddRectFilled(p, p + size, U32(Accent(0.16f)), 8.f);
        else if (hovered) dl->AddRectFilled(p, p + size, IM_COL32(255, 255, 255, 10), 8.f);
        if (selected)     dl->AddRectFilled({ p.x, p.y + 9.f }, { p.x + 3.f, p.y + size.y - 9.f }, U32(Accent()), 2.f);

        const ImVec2 textPos(p.x + 16.f, p.y + (size.y - ImGui::GetTextLineHeight()) * 0.5f);
        dl->AddText(textPos, U32(selected ? kText : (hovered ? Mix(kTextDim, kText, 0.5f) : kTextDim)), label);
        return pressed;
    }

    // Carte de module : titre, description, interrupteur a droite.
    // Renvoie true si le contenu (reglages) doit etre affiche.
    bool BeginCard(const char* id, const char* title, const char* desc, bool* toggle = nullptr, bool available = true) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 16.f, 14.f });
        ImGui::PushStyleColor(ImGuiCol_ChildBg, kCard);
        ImGui::BeginChild(id, { 0.f, 0.f }, ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();

        g_cardX0 = ImGui::GetCursorPosX();
        g_cardW = ImGui::GetContentRegionAvail().x;

        ImGui::BeginGroup();
        ImGui::TextUnformatted(title);
        ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
        ImGui::PushTextWrapPos(g_cardX0 + g_cardW - (toggle ? 70.f : 0.f));
        ImGui::TextUnformatted(desc);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::EndGroup();

        if (toggle) {
            const float groupH = ImGui::GetItemRectSize().y;
            const float h = ImGui::GetFrameHeight() * 0.82f, w = h * 1.8f;
            ImGui::SameLine(g_cardX0 + g_cardW - w);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (groupH - h) * 0.5f);
            ToggleSwitch("##toggle", toggle, available);
        }

        const bool showContent = toggle ? (*toggle && available) : true;
        if (showContent && toggle) {
            ImGui::Dummy({ 0.f, 2.f });
            ImGui::Separator();
            ImGui::Dummy({ 0.f, 2.f });
        }
        return showContent;
    }

    void EndCard() {
        ImGui::EndChild();
        ImGui::Dummy({ 0.f, 4.f });
    }

    // Ligne "libelle .......... widget" dans une carte
    void RowLabel(const char* label) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(g_cardX0 + g_cardW * 0.40f);
        ImGui::SetNextItemWidth(-FLT_MIN);
    }

    void RowToggle(const char* label, bool* v) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        const float w = ImGui::GetFrameHeight() * 0.82f * 1.8f;
        ImGui::SameLine(g_cardX0 + g_cardW - w);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ImGui::GetFrameHeight() * 0.09f);
        ImGui::PushID(label);
        ToggleSwitch("##t", v);
        ImGui::PopID();
    }

    // Mini ecran cliquable : 6 positions predefinies (coins et centres haut/bas)
    void AnchorPicker(int* anchor) {
        const float w = 168.f, h = 94.f;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::Dummy({ w, h });
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, p + ImVec2(w, h), U32(kFrame), 8.f);
        dl->AddRect(p, p + ImVec2(w, h), U32(ImVec4(1, 1, 1, 0.10f)), 8.f, 0, 1.5f);

        const float pad = 14.f, r = 8.f;
        for (int i = 0; i < 6; ++i) {
            const int col = i % 3, row = i / 3;
            const ImVec2 c(p.x + pad + (w - 2 * pad) * 0.5f * col, row == 0 ? p.y + pad : p.y + h - pad);
            ImGui::SetCursorScreenPos(c - ImVec2(r + 4, r + 4));
            ImGui::PushID(i);
            if (ImGui::InvisibleButton("##anchor", { 2 * r + 8, 2 * r + 8 })) *anchor = i + 1;
            const bool hov = ImGui::IsItemHovered();
            ImGui::PopID();
            const bool sel = *anchor == i + 1;
            dl->AddCircleFilled(c, sel ? r : r - 2.f, U32(sel ? Accent() : (hov ? kTextDim : ImVec4(1, 1, 1, 0.18f))));
            if (sel) dl->AddCircle(c, r + 3.f, U32(Accent(0.5f)), 0, 1.5f);
        }
        ImGui::SetCursorScreenPos({ p.x, p.y + h + ImGui::GetStyle().ItemSpacing.y });
    }

    // ---- Choix d'une touche ------------------------------------------------------------
    std::string KeyName(int vk) {
        switch (vk) {
            case 0:          return "Aucune";
            case VK_LBUTTON: return "Clic gauche";
            case VK_RBUTTON: return "Clic droit";
            case VK_MBUTTON: return "Clic molette";
            case VK_XBUTTON1: return "Souris 4";
            case VK_XBUTTON2: return "Souris 5";
        }
        UINT sc = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
        if (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN || vk == VK_INSERT || vk == VK_DELETE ||
            vk == VK_HOME || vk == VK_END || vk == VK_PRIOR || vk == VK_NEXT || vk == VK_RCONTROL || vk == VK_RMENU)
            sc |= 0x100;   // touche etendue
        wchar_t wname[64] = L"";
        if (GetKeyNameTextW(static_cast<LONG>(sc << 16), wname, 64) > 0) {
            char name[128];
            WideCharToMultiByte(CP_UTF8, 0, wname, -1, name, sizeof(name), nullptr, nullptr);
            return name;
        }
        char buf[16];
        snprintf(buf, sizeof(buf), "Touche %d", vk);
        return buf;
    }

    // Bouton "touche" : clic, puis appui sur la touche voulue (Echap = annuler)
    bool KeyBindButton(const char* id, int* vk) {
        static const char* capturing = nullptr;
        static bool waitRelease = false;
        const bool active = capturing == id;
        bool changed = false;

        std::string label = active ? "Appuie sur une touche..." : KeyName(*vk);
        label += "##";
        label += id;
        ImGui::PushStyleColor(ImGuiCol_Button, active ? Accent(0.85f) : kFrame);
        ImGui::PushStyleColor(ImGuiCol_Text, active ? ImVec4(0.05f, 0.04f, 0.08f, 1.f) : kText);
        if (ImGui::Button(label.c_str(), { -FLT_MIN, 0.f }) && !active) { capturing = id; waitRelease = true; }
        ImGui::PopStyleColor(2);

        if (capturing == id) {
            // On attend que le clic qui a lance la capture soit relache, puis on lit la 1re touche appuyee
            bool any = false;
            for (int k = 1; k < 255; ++k) if (GetAsyncKeyState(k) & 0x8000) { any = true; break; }
            if (waitRelease) { if (!any) waitRelease = false; }
            else {
                for (int k = 1; k < 255; ++k) {
                    if (k == VK_SHIFT || k == VK_CONTROL || k == VK_MENU) continue;   // versions G/D utilisees
                    if (!(GetAsyncKeyState(k) & 0x8000)) continue;
                    if (k == VK_ESCAPE) { capturing = nullptr; break; }
                    if (k == VK_INSERT || k == VK_END) continue;                    // touches de Nexus
                    *vk = k == VK_LSHIFT || k == VK_RSHIFT ? VK_SHIFT : k == VK_LCONTROL || k == VK_RCONTROL ? VK_CONTROL
                        : k == VK_LMENU || k == VK_RMENU ? VK_MENU : k;
                    capturing = nullptr;
                    changed = true;
                    break;
                }
            }
        }
        return changed;
    }

    void ColorSwatches(float* accent) {
        static const float presets[][3] = {
            { 0.62f, 0.46f, 1.00f }, { 0.30f, 0.62f, 1.00f }, { 0.30f, 0.82f, 0.55f },
            { 1.00f, 0.42f, 0.64f }, { 1.00f, 0.63f, 0.25f }, { 0.95f, 0.34f, 0.34f },
        };
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float d = ImGui::GetFrameHeight();
        for (int i = 0; i < IM_ARRAYSIZE(presets); ++i) {
            if (i) ImGui::SameLine(0.f, 8.f);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::PushID(i);
            if (ImGui::InvisibleButton("##sw", { d, d })) std::copy(presets[i], presets[i] + 3, accent);
            ImGui::PopID();
            const ImVec2 c = p + ImVec2(d * 0.5f, d * 0.5f);
            const bool sel = std::abs(accent[0] - presets[i][0]) + std::abs(accent[1] - presets[i][1]) +
                             std::abs(accent[2] - presets[i][2]) < 0.01f;
            dl->AddCircleFilled(c, d * 0.5f - 3.f, U32(ImVec4(presets[i][0], presets[i][1], presets[i][2], 1.f)));
            if (sel || ImGui::IsItemHovered())
                dl->AddCircle(c, d * 0.5f - 0.5f, IM_COL32(255, 255, 255, sel ? 230 : 90), 0, 2.f);
        }
        ImGui::SameLine(0.f, 12.f);
        ImGui::ColorEdit3("##custom", accent, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
    }

    // ---- Pages ------------------------------------------------------------------
    void PageVisual(Settings& s) {
        const char* armorDesc = Game::IsConfigured()
            ? "Usure de chaque pièce et de l'objet en main."
            : "Offsets à compléter : visible seulement en mode démo.";
        if (BeginCard("##armor", "Durabilité de l'armure", armorDesc, &s.armorEnabled)) {
            static const char* layouts[] = { "Vertical", "Horizontal" };
            static const char* modes[] = { "Valeur", "%", "Les deux" };
            RowLabel("Disposition");   Segmented("layout", &s.layout, layouts, 2);
            RowLabel("Affichage");     Segmented("mode", &s.valueMode, modes, 3);
            RowLabel("Alerte sous");   ImGui::SliderInt("##low", &s.lowThreshold, 0, 50, "%d %%");
            RowToggle("Objet en main", &s.showHeldItem);
            RowToggle("Afficher les slots vides", &s.showEmptySlots);
        }
        EndCard();

        {
            const bool ok = HealthBars::IsConfigured();
            char desc[320];
            if (ok) snprintf(desc, sizeof(desc), "Barre de vie au-dessus des entités proches que tu vois (rien à travers les murs).");
            else    snprintf(desc, sizeof(desc), "Manque dans offsets.ini [entities] : %s", HealthBars::MissingText());
            if (BeginCard("##health", "Barres de vie", desc, &s.healthEnabled, ok)) {
                ImGui::TextDisabled("Afficher pour");
                RowToggle("Joueurs", &s.hpPlayers);
                RowToggle("Animaux (vaches, cochons, loups...)", &s.hpAnimals);
                RowToggle("Monstres (zombies, squelettes...)", &s.hpMonsters);
                RowToggle("Autres (villageois, golems...)", &s.hpOthers);
                ImGui::Dummy({ 0.f, 2.f });
                ImGui::Separator();
                ImGui::Dummy({ 0.f, 2.f });
                RowLabel("Distance max");  ImGui::SliderFloat("##hpdist", &s.hpMaxDistance, 4.f, 64.f, "%.0f blocs");
                RowLabel("Taille");        ImGui::SliderFloat("##hpscale", &s.hpBarScale, 0.5f, 2.f, "%.2fx");
                RowLabel("Calibrage FOV"); ImGui::SliderFloat("##fov", &s.fov, 30.f, 110.f, "%.0f°");
                RowToggle("Afficher la valeur (15/20)", &s.hpShowValue);
                RowToggle("Afficher le nom des joueurs", &s.hpShowNames);
                RowToggle("Masquer si vie pleine", &s.hpHideFull);
                ImGui::TextDisabled("Si les barres sont décalées, ajuste « Calibrage FOV ».");
            }
            EndCard();
        }

        {
            const bool ok = OwnNametag::IsConfigured();
            BeginCard("##nametag", "Mon nametag en F5",
                      ok ? "Affiche ton pseudo au-dessus de ta tête en vue à la troisième personne."
                         : "Section [nametag] de offsets.ini à compléter (et [player]).",
                      &s.showOwnNametag, ok);
            EndCard();
        }

        {
            const bool ok = Zoom::IsConfigured();
            if (BeginCard("##zoom", "Zoom",
                          ok ? "Maintiens la touche pour zoomer, comme avec une longue-vue."
                             : "Section [zoom] de offsets.ini à compléter.",
                          &s.zoomEnabled, ok)) {
                RowLabel("Touche");  KeyBindButton("zoomkey", &s.zoomKey);
                static const char* modes[] = { "Maintenir", "Basculer" };
                RowLabel("Mode");    Segmented("zoommode", &s.zoomMode, modes, 2);
                RowLabel("Niveau");  ImGui::SliderFloat("##zoomfactor", &s.zoomFactor, 2.f, 10.f, "x%.1f");
                RowToggle("Zoom fluide", &s.zoomSmooth);
                ImGui::TextDisabled("Le zoom est désactivé quand le chat ou ce menu est ouvert.");
            }
            EndCard();
        }

        if (BeginCard("##fps", "Compteur FPS", "Images par seconde, en temps réel.", &s.fpsEnabled)) {
            // Position
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Position");
            ImGui::SameLine(g_cardX0 + g_cardW * 0.40f);
            ImGui::BeginGroup();
            AnchorPicker(&s.fpsAnchor);
            const bool freePos = s.fpsAnchor == 0;
            if (ImGui::RadioButton("Position libre (à faire glisser)", freePos)) s.fpsAnchor = 0;
            ImGui::EndGroup();
            ImGui::Dummy({ 0.f, 2.f });

            RowLabel("Taille");         ImGui::SliderFloat("##fpsscale", &s.fpsScale, 0.6f, 3.f, "%.1fx");
            if (!freePos) { RowLabel("Marge du bord"); ImGui::SliderFloat("##fpsmargin", &s.fpsMargin, 0.f, 80.f, "%.0f px"); }

            static const char* colorModes[] = { "Accent", "Selon perf.", "Blanc" };
            RowLabel("Couleur");        Segmented("fpscolor", &s.fpsColorMode, colorModes, 3);
            if (s.fpsColorMode == 1) {
                RowLabel("FPS visés");  ImGui::SliderInt("##fpstarget", &s.fpsTarget, 30, 240, "%d");
            }
            static const char* rates[] = { "Rapide", "Normal", "Lent" };
            int rate = s.fpsInterval <= 0.15f ? 0 : s.fpsInterval <= 0.4f ? 1 : 2;
            RowLabel("Mise à jour");
            if (Segmented("fpsrate", &rate, rates, 3)) s.fpsInterval = rate == 0 ? 0.1f : rate == 1 ? 0.25f : 1.f;

            RowToggle("Afficher « FPS »", &s.fpsShowLabel);
            RowToggle("Temps par image (ms)", &s.fpsShowMs);
            RowToggle("Fond", &s.fpsBackground);
        }
        EndCard();

        ImGui::TextDisabled("Astuce : menu ouvert, fais glisser les éléments du HUD pour les déplacer.");
    }

    void PageMovement(Settings& s) {
        const bool ok = AutoSprint::IsConfigured();
        BeginCard("##sprint", "Auto-sprint",
                  ok ? "Sprinte automatiquement quand tu avances. Les règles du jeu s'appliquent."
                     : "Section [autosprint] de offsets.ini à compléter.",
                  &s.autoSprint, ok);
        EndCard();
    }

    bool AccentButton(const char* label, const ImVec2& size) {
        ImGui::PushStyleColor(ImGuiCol_Button, Accent(0.85f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Accent());
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, Mix(Accent(), ImVec4(1, 1, 1, 1), 0.2f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.05f, 0.04f, 0.08f, 1.f));
        const bool pressed = ImGui::Button(label, size);
        ImGui::PopStyleColor(4);
        return pressed;
    }

    void PageChat(Settings&) {
        static char input[256] = "";
        const bool busy = ChatSender::Busy();

        if (BeginCard("##send", "Envoyer dans le chat",
                      "Message, ou commande commençant par /. Le menu se ferme le temps de l'envoi.")) {
            ImGui::Dummy({ 0.f, 2.f });
            const float bw = 110.f;
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - bw - 8.f);
            const bool enter = ImGui::InputTextWithHint("##chatinput", "/time set day", input, sizeof(input),
                                                        ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            ImGui::BeginDisabled(busy || !input[0]);
            const bool click = AccentButton(busy ? "Envoi..." : "Envoyer", { bw, 0.f });
            ImGui::EndDisabled();
            if ((enter || click) && input[0] && ChatSender::Send(input)) input[0] = 0;
        }
        EndCard();

        static bool edit = false;
        auto& cmds = Config::Commands();
        if (BeginCard("##quick", "Commandes rapides",
                      edit ? "Modifie le nom et le texte de chaque bouton."
                           : "Un clic pour envoyer. Les commandes demandent que les cheats soient activés dans le monde.")) {
            ImGui::Dummy({ 0.f, 2.f });
            if (!edit) {
                const int cols = 3;
                const float w = (ImGui::GetContentRegionAvail().x - 8.f * (cols - 1)) / cols;
                ImGui::BeginDisabled(busy);
                for (int i = 0; i < (int)cmds.size(); ++i) {
                    if (i % cols) ImGui::SameLine();
                    ImGui::PushID(i);
                    if (ImGui::Button(cmds[i].label.c_str(), { w, 36.f })) ChatSender::Send(cmds[i].text.c_str());
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", cmds[i].text.c_str());
                    ImGui::PopID();
                }
                ImGui::EndDisabled();
                if (cmds.empty()) ImGui::TextDisabled("Aucune commande. Clique sur Modifier pour en ajouter.");
            } else {
                int toDelete = -1;
                const float avail = ImGui::GetContentRegionAvail().x;
                for (int i = 0; i < (int)cmds.size(); ++i) {
                    ImGui::PushID(i);
                    ImGui::SetNextItemWidth(avail * 0.30f);
                    ImGui::InputTextWithHint("##label", "Nom", &cmds[i].label);
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(avail * 0.70f - 8.f - 36.f - 8.f);
                    ImGui::InputTextWithHint("##text", "/commande ou message", &cmds[i].text);
                    ImGui::SameLine();
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.45f, 0.14f, 0.22f, 1.f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.65f, 0.18f, 0.28f, 1.f));
                    if (ImGui::Button("X", { 36.f, 0.f })) toDelete = i;
                    ImGui::PopStyleColor(2);
                    ImGui::PopID();
                }
                if (toDelete >= 0) { cmds.erase(cmds.begin() + toDelete); Config::SaveCommands(); }
                if (ImGui::Button("+ Ajouter une commande", { -FLT_MIN, 0.f })) cmds.push_back({ "Nouvelle", "/" });
            }

            ImGui::Dummy({ 0.f, 2.f });
            if (AccentButton(edit ? "Terminé" : "Modifier", { 120.f, 0.f })) {
                if (edit) Config::SaveCommands();
                edit = !edit;
            }
        }
        EndCard();

        if (BeginCard("##overlay", "Pré-commandes dans le chat",
                      "Quand tu ouvres le chat de Minecraft (T ou /), ces commandes s'affichent à côté et se filtrent pendant que tu tapes.",
                      &Config::GetSettings().chatOverlay)) {
            Settings& st = Config::GetSettings();
            // Boutons dans l'ordre visuel : "Gauche" a gauche, "Droite" a droite
            // (le reglage enregistre reste 0 = droite, 1 = gauche)
            static const char* sides[] = { "Gauche", "Droite" };
            int sideUi = st.chatOverlaySide == 1 ? 0 : 1;
            RowLabel("Côté");
            if (Segmented("side", &sideUi, sides, 2)) st.chatOverlaySide = sideUi == 0 ? 1 : 0;
            RowLabel("Hauteur");  ImGui::SliderFloat("##ovb", &st.chatOverlayBottom, 40.f, 400.f, "%.0f px");
            ImGui::PushTextWrapPos(0.f);
            ImGui::TextDisabled("Un texte qui finit par un espace (ex : « /tp ») est inséré pour que tu le complètes ; "
                                "sinon il est envoyé directement.");
            ImGui::PopTextWrapPos();
            const bool chatOpen = ChatOverlay::IsChatOpen();
            ImGui::TextDisabled("Chat du jeu détecté :");
            ImGui::SameLine();
            ImGui::TextColored(chatOpen ? Accent() : kTextDim, chatOpen ? "ouvert" : "fermé");
        }
        EndCard();

        ImGui::TextDisabled("Si ta touche de chat n'est pas T, change open_key dans client.ini.");
    }

    void PageDiscord(Settings& s) {
        const std::string status = DiscordRPC::Status();
        const bool connected = status.rfind("Connecté", 0) == 0;

        if (BeginCard("##rpc", "Rich Presence", "Affiche sur ton profil Discord ce que tu fais dans Minecraft.", &s.discordEnabled)) {
            RowToggle("Afficher l'adresse du serveur", &s.discordShowIp);
            RowToggle("Afficher les joueurs connectés", &s.discordShowPlayers);
            RowToggle("Afficher le ping (ms)", &s.discordShowPing);
            RowToggle("Message du serveur au survol de l'image", &s.discordShowMotd);
            ImGui::TextDisabled("Joueurs connectés et ping s'affichent au survol de l'image.");

            ImGui::Dummy({ 0.f, 2.f });
            const ImVec4 col = connected ? ImVec4(0.35f, 0.88f, 0.50f, 1.f) : ImVec4(0.95f, 0.70f, 0.25f, 1.f);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddCircleFilled({ p.x + 5.f, p.y + ImGui::GetTextLineHeight() * 0.5f + 1.f }, 4.f, U32(col));
            ImGui::Dummy({ 14.f, ImGui::GetTextLineHeight() });
            ImGui::SameLine();
            ImGui::TextUnformatted(status.c_str());
        }
        EndCard();

        // Apercu facon carte Discord
        if (BeginCard("##rpcpreview", "Aperçu", "Ce que tes amis voient (mis à jour toutes les quelques secondes).")) {
            std::string details, state, hover;
            DiscordRPC::Preview(details, state, hover);
            ImGui::Dummy({ 0.f, 2.f });
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float w = ImGui::GetContentRegionAvail().x, h = 92.f;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(p, p + ImVec2(w, h), IM_COL32(30, 31, 34, 255), 8.f);   // fond Discord
            // Image : logo Nexus tel qu'il apparait sur Discord (carre arrondi + hexagone)
            const ImVec2 c = p + ImVec2(46.f, h * 0.5f);
            dl->AddRectFilled(c - ImVec2(36.f, 36.f), c + ImVec2(36.f, 36.f), IM_COL32(30, 20, 56, 255), 12.f);
            ImVec2 hex[6];
            for (int i = 0; i < 6; ++i) {
                const float a = 3.14159265f / 3.f * i + 3.14159265f / 6.f;
                hex[i] = { c.x + std::cos(a) * 30.f, c.y + std::sin(a) * 30.f };
            }
            dl->AddConvexPolyFilled(hex, 6, U32(Accent()));
            const ImU32 white = IM_COL32(255, 255, 255, 245);
            dl->AddLine(c + ImVec2(-8, 10), c + ImVec2(-8, -10), white, 3.f);
            dl->AddLine(c + ImVec2(-8, -10), c + ImVec2(8, 10), white, 3.f);
            dl->AddLine(c + ImVec2(8, 10), c + ImVec2(8, -10), white, 3.f);

            const float tx = p.x + 92.f, lh = ImGui::GetTextLineHeight();
            dl->AddText({ tx, p.y + 12.f }, IM_COL32(242, 243, 245, 255), "Nexus");
            dl->AddText({ tx, p.y + 14.f + lh }, IM_COL32(219, 222, 225, 255), details.empty() ? "..." : details.c_str());
            if (!state.empty()) dl->AddText({ tx, p.y + 16.f + 2 * lh }, IM_COL32(181, 186, 193, 255), state.c_str());
            ImGui::Dummy({ w, h });
            // Survol de l'image (comme sur Discord)
            if (ImGui::IsMouseHoveringRect(c - ImVec2(36.f, 36.f), c + ImVec2(36.f, 36.f)) && !hover.empty())
                ImGui::SetTooltip("%s", hover.c_str());
            ImGui::TextDisabled("Survol de l'image : %s", hover.c_str());
            ImGui::TextDisabled("%s", Network::DebugText().c_str());
        }
        EndCard();

        ImGui::TextDisabled("Discord doit être ouvert sur ce PC, avec « Partager mon activité » activé.");
    }

    void PageLogs(Settings&) {
        static std::vector<Logger::Entry> entries;
        static size_t version = static_cast<size_t>(-1);
        static int minLevel = 0;          // 0 tout, 1 avertissements et erreurs, 2 erreurs
        static char search[128] = "";
        static bool autoScroll = true;

        if (Logger::Version() != version) { version = Logger::Version(); Logger::Snapshot(entries); }

        int warns = 0, errors = 0;
        for (const auto& e : entries) {
            if (e.level == Logger::Level::Warn) ++warns;
            else if (e.level == Logger::Level::Error) ++errors;
        }

        char desc[160];
        snprintf(desc, sizeof(desc), "%zu lignes · %d avertissement%s · %d erreur%s", entries.size(),
                 warns, warns > 1 ? "s" : "", errors, errors > 1 ? "s" : "");
        if (BeginCard("##logs", "Journal du client", desc)) {
            ImGui::Dummy({ 0.f, 2.f });

            // Filtres
            static const char* levels[] = { "Tout", "Avert. + erreurs", "Erreurs" };
            const float avail = ImGui::GetContentRegionAvail().x;
            ImGui::SetNextItemWidth(avail * 0.45f);
            ImGui::InputTextWithHint("##logsearch", "Rechercher...", search, sizeof(search));
            ImGui::SameLine();
            ImGui::BeginGroup();
            ImGui::PushItemWidth(-FLT_MIN);
            Segmented("loglevel", &minLevel, levels, 3);
            ImGui::PopItemWidth();
            ImGui::EndGroup();

            // Lignes
            const ImVec4 colInfo = kText, colWarn(1.f, 0.72f, 0.25f, 1.f), colErr(0.98f, 0.38f, 0.38f, 1.f);
            std::string filter = search;
            for (auto& ch : filter) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));

            ImGui::PushStyleColor(ImGuiCol_ChildBg, kSidebar);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 10.f, 8.f });
            ImGui::BeginChild("##loglines", { 0.f, 340.f }, ImGuiChildFlags_AlwaysUseWindowPadding);
            ImGui::PopStyleVar();
            std::string all;   // texte visible, pour "Copier"
            for (const auto& e : entries) {
                if (static_cast<int>(e.level) < minLevel) continue;
                if (!filter.empty()) {
                    std::string low = e.message;
                    for (auto& ch : low) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                    if (low.find(filter) == std::string::npos) continue;
                }
                const char* tag = e.level == Logger::Level::Error ? "ERREUR" : e.level == Logger::Level::Warn ? "AVERT." : "INFO";
                const ImVec4 col = e.level == Logger::Level::Error ? colErr : e.level == Logger::Level::Warn ? colWarn : colInfo;
                ImGui::TextColored(kTextDim, "%s", e.time.c_str());
                ImGui::SameLine();
                ImGui::TextColored(e.level == Logger::Level::Info ? Accent(0.9f) : col, "%-6s", tag);
                ImGui::SameLine();
                // Message entier, retour a la ligne automatique (aligne sous le debut du message)
                ImGui::PushStyleColor(ImGuiCol_Text, col);
                ImGui::PushTextWrapPos(0.f);
                ImGui::TextUnformatted(e.message.c_str());
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) {
                    ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin() - ImVec2(4.f, 1.f),
                                                              ImGui::GetItemRectMax() + ImVec2(4.f, 1.f),
                                                              IM_COL32(255, 255, 255, 12), 4.f);
                    ImGui::SetTooltip("Clic : copier cette ligne");
                }
                if (ImGui::IsItemClicked())
                    ImGui::SetClipboardText((e.time + " [" + tag + "] " + e.message).c_str());
                ImGui::Dummy({ 0.f, 1.f });
                all += e.time + " [" + tag + "] " + e.message + "\n";
            }
            if (autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 40.f) ImGui::SetScrollHereY(1.f);
            ImGui::EndChild();
            ImGui::PopStyleColor();

            // Actions
            ImGui::Dummy({ 0.f, 2.f });
            ImGui::Checkbox("Défilement auto", &autoScroll);
            const float bw = 110.f;
            ImGui::SameLine(g_cardX0 + g_cardW - bw * 3 - 16.f);
            if (ImGui::Button("Copier", { bw, 0.f })) ImGui::SetClipboardText(all.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Copie les lignes affichées (pour me les envoyer)");
            ImGui::SameLine();
            if (ImGui::Button("Effacer", { bw, 0.f })) Logger::ClearView();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Vide l'affichage ; le fichier Nexus.log est conservé");
            ImGui::SameLine();
            if (AccentButton("Ouvrir le fichier", { bw, 0.f }))
                ShellExecuteW(nullptr, L"open", Logger::FilePath().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        EndCard();
    }

    void PageSettings(Settings& s) {
        if (BeginCard("##ui", "Interface", "Taille et apparence du HUD et du menu.")) {
            ImGui::Dummy({ 0.f, 2.f });
            RowLabel("Taille du HUD");    ImGui::SliderFloat("##scale", &s.scale, 0.5f, 2.5f, "%.2fx");
            RowLabel("Opacité du fond");  ImGui::SliderFloat("##alpha", &s.bgAlpha, 0.f, 1.f, "%.2f");
            RowLabel("Couleur d'accent"); ColorSwatches(s.accent);
            static const char* blockModes[] = { "Discret", "Alt+Tab", "Désactivé" };
            RowLabel("Blocage du jeu"); Segmented("blockmode", &s.menuBlockMode, blockModes, 3);
            ImGui::PushTextWrapPos(0.f);
            ImGui::TextDisabled(s.menuBlockMode == 0
                ? "Discret : caméra, clics et touches bloqués dans le menu, sans mettre le jeu en pause."
                : s.menuBlockMode == 1
                ? "Alt+Tab : blocage le plus sûr, mais le jeu peut se mettre en pause (solo)."
                : "Désactivé : seul le blocage GameInput / Raw Input est utilisé.");
            ImGui::PopTextWrapPos();
        }
        EndCard();

        if (BeginCard("##about", "À propos de Nexus", "Client Minecraft Bedrock")) {
            ImGui::Dummy({ 0.f, 2.f });
            RowLabel("Version");    ImGui::TextUnformatted(Credits::kVersion);
            RowLabel("Créé par");   ImGui::TextColored(Accent(), "%s", Credits::kCreatedBy);
            RowLabel("Signatures"); ImGui::TextUnformatted(Credits::kSignaturesBy);
            ImGui::PushTextWrapPos(0.f);
            ImGui::TextDisabled("Le client (launcher, DLL, menu et modules) a été écrit par Claude. "
                                "%s a trouvé les signatures et offsets du jeu et testé le client en jeu.",
                                Credits::kSignaturesBy);
            ImGui::PopTextWrapPos();
        }
        EndCard();

        if (BeginCard("##session", "Session", "Les réglages sont aussi sauvegardés à la fermeture du menu.")) {
            ImGui::Dummy({ 0.f, 2.f });
            const float w = (ImGui::GetContentRegionAvail().x - 8.f) * 0.5f;
            ImGui::PushStyleColor(ImGuiCol_Button, Accent(0.85f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Accent());
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.05f, 0.05f, 0.06f, 1.f));
            if (ImGui::Button("Sauvegarder", { w, 0.f })) Config::SaveSettings();
            ImGui::PopStyleColor(3);
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.16f, 0.18f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.70f, 0.20f, 0.22f, 1.f));
            if (ImGui::Button("Décharger (FIN)", { w, 0.f })) Client::RequestUnload();
            ImGui::PopStyleColor(2);
        }
        EndCard();
    }

    void PageDiagnostic(Settings& s) {
        if (BeginCard("##funcs", "Fonctions du jeu", "Signatures trouvées dans Minecraft.Windows.exe.")) {
            ImGui::Dummy({ 0.f, 2.f });
            ImDrawList* dl = ImGui::GetWindowDrawList();
            for (const auto& f : Game::GetFunctionStatus()) {
                const ImVec4 col = f.found ? ImVec4(0.35f, 0.88f, 0.50f, 1.f)
                                           : (f.required ? ImVec4(0.95f, 0.33f, 0.33f, 1.f) : ImVec4(0.95f, 0.70f, 0.25f, 1.f));
                const ImVec2 p = ImGui::GetCursorScreenPos();
                dl->AddCircleFilled({ p.x + 5.f, p.y + ImGui::GetTextLineHeight() * 0.5f + 1.f }, 4.f, U32(col));
                ImGui::Dummy({ 14.f, ImGui::GetTextLineHeight() });
                ImGui::SameLine();
                ImGui::TextUnformatted(f.name);
                ImGui::SameLine(g_cardX0 + g_cardW - 110.f);
                ImGui::TextColored(col, f.found ? "trouvée" : (f.required ? "manquante" : "optionnelle"));
            }
            const bool sprintOk = AutoSprint::IsConfigured();
            const ImVec4 col = sprintOk ? ImVec4(0.35f, 0.88f, 0.50f, 1.f) : ImVec4(0.95f, 0.70f, 0.25f, 1.f);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            dl->AddCircleFilled({ p.x + 5.f, p.y + ImGui::GetTextLineHeight() * 0.5f + 1.f }, 4.f, U32(col));
            ImGui::Dummy({ 14.f, ImGui::GetTextLineHeight() });
            ImGui::SameLine();
            ImGui::TextUnformatted("Auto-sprint (hook + offset)");
            ImGui::SameLine(g_cardX0 + g_cardW - 110.f);
            ImGui::TextColored(col, sprintOk ? "trouvée" : "optionnelle");
        }
        EndCard();

        if (BeginCard("##mouse", "Blocage de la caméra", "Empêche la vue de bouger quand ce menu est ouvert.")) {
            ImGui::Dummy({ 0.f, 2.f });
            const bool ok = MouseBlock::GameInputHooked();
            const ImVec4 col = ok ? ImVec4(0.35f, 0.88f, 0.50f, 1.f) : ImVec4(0.95f, 0.70f, 0.25f, 1.f);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddCircleFilled({ p.x + 5.f, p.y + ImGui::GetTextLineHeight() * 0.5f + 1.f }, 4.f, U32(col));
            ImGui::Dummy({ 14.f, ImGui::GetTextLineHeight() });
            ImGui::SameLine();
            ImGui::TextWrapped("%s", MouseBlock::Status());

            ImGui::Dummy({ 0.f, 2.f });
            ImGui::TextDisabled("Appels par seconde (bouge la souris pour tester) :");
            for (int c = 0; c < MouseBlock::CounterCount; ++c) {
                ImGui::TextUnformatted(MouseBlock::CounterName(c));
                ImGui::SameLine(g_cardX0 + g_cardW - 60.f);
                const unsigned r = MouseBlock::PerSecond(c);
                ImGui::TextColored(r ? Accent() : kTextDim, "%u", r);
            }
        }
        EndCard();

        BeginCard("##demo", "Mode démo", "Affiche de fausses valeurs pour tester le HUD sans offsets.", &s.demoMode);
        EndCard();

        ImGui::TextDisabled("Journal détaillé : Nexus.log, à côté de la DLL.");
    }

    struct Tab { const char* name; const char* subtitle; void (*draw)(Settings&); };
    const Tab kTabs[] = {
        { "Visuel",     "Éléments affichés à l'écran",        PageVisual },
        { "Mouvement",  "Aides au déplacement",               PageMovement },
        { "Chat",       "Messages et commandes rapides",      PageChat },
        { "Discord",    "Statut Rich Presence",               PageDiscord },
        { "Paramètres", "Apparence et session",               PageSettings },
        { "Diagnostic", "État des signatures et des offsets", PageDiagnostic },
        { "Journal",    "Messages du client en direct",       PageLogs },
    };
}

// ---- Panneau de pre-commandes a cote du chat du jeu ------------------------------------
namespace {
    bool StartsWithNoCase(const std::string& s, const std::string& prefix) {
        if (prefix.size() > s.size()) return false;
        for (size_t i = 0; i < prefix.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(s[i])) != std::tolower(static_cast<unsigned char>(prefix[i])))
                return false;
        return true;
    }

    void RenderChatOverlay() {
        const Settings& s = Config::GetSettings();
        ChatOverlay::SetVisible(false);
        if (!s.chatOverlay || !ChatOverlay::IsChatOpen()) return;

        bool known = true;
        const std::string typed = ChatOverlay::Typed(known);

        // Filtre : commandes dont le texte (ou le nom) commence par ce qui est tape
        std::vector<const QuickCommand*> list;
        for (const auto& c : Config::Commands()) {
            if (!known || typed.empty() || StartsWithNoCase(c.text, typed) || StartsWithNoCase(c.label, typed))
                list.push_back(&c);
            if (list.size() >= 8) break;
        }
        if (list.empty()) return;

        const ImVec2 display = ImGui::GetIO().DisplaySize;
        const bool right = s.chatOverlaySide == 0;
        ImGui::SetNextWindowPos({ right ? display.x - 16.f : 16.f, display.y - s.chatOverlayBottom },
                                ImGuiCond_Always, { right ? 1.f : 0.f, 1.f });
        ImGui::SetNextWindowBgAlpha(0.92f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 10.f, 10.f });
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, kSidebar);
        const bool shown = ImGui::Begin("##nexus_chat", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoMove);
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
        if (!shown) { ImGui::End(); return; }
        ChatOverlay::SetVisible(true);
        ChatOverlay::SetHovered(ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem));
        {
            std::string texts[8];
            for (size_t i = 0; i < list.size() && i < 8; ++i) texts[i] = list[i]->text;
            ChatOverlay::SetShortcutTexts(texts, static_cast<int>(std::min<size_t>(list.size(), 8)));
        }

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float w = 300.f;

        // En-tete
        {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            dl->AddCircleFilled(p + ImVec2(5.f, ImGui::GetTextLineHeight() * 0.5f), 4.f, U32(Accent()));
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 14.f);
            ImGui::TextColored(Accent(), "Nexus");
            ImGui::SameLine(0.f, 6.f);
            ImGui::TextDisabled("pré-commandes");
            ImGui::Dummy({ w, 2.f });
        }

        for (size_t i = 0; i < list.size(); ++i) {
            const QuickCommand& c = *list[i];
            const bool complete = !c.text.empty() && c.text.back() == ' ';
            const float h = ImGui::GetTextLineHeight() * 2.f + 12.f;
            const ImVec2 p = ImGui::GetCursorScreenPos();

            ImGui::PushID(static_cast<int>(i));
            const bool clicked = ImGui::InvisibleButton("##cmd", { w, h });
            const bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();

            if (hovered) dl->AddRectFilled(p, p + ImVec2(w, h), U32(Accent(0.18f)), 7.f);
            if (hovered) dl->AddRectFilled({ p.x, p.y + 8.f }, { p.x + 3.f, p.y + h - 8.f }, U32(Accent()), 2.f);
            // Badge du raccourci Alt+N
            char key[4];
            snprintf(key, sizeof(key), "%d", static_cast<int>(i) + 1);
            const float bs = ImGui::GetTextLineHeight() + 4.f;
            const ImVec2 bp(p.x + 10.f, p.y + (h - bs) * 0.5f);
            dl->AddRectFilled(bp, bp + ImVec2(bs, bs), U32(hovered ? Accent() : kFrame), 5.f);
            const ImVec2 ks = ImGui::CalcTextSize(key);
            dl->AddText(bp + ImVec2((bs - ks.x) * 0.5f, (bs - ks.y) * 0.5f), U32(hovered ? ImVec4(0.05f, 0.04f, 0.08f, 1.f) : kText), key);

            const float tx = bp.x + bs + 10.f;
            dl->AddText({ tx, p.y + 5.f }, U32(kText), c.label.c_str());
            dl->AddText({ tx, p.y + 7.f + ImGui::GetTextLineHeight() }, U32(kTextDim), c.text.c_str());

            const char* tag = complete ? "compléter" : "envoyer";
            const ImVec2 ts = ImGui::CalcTextSize(tag);
            const ImVec2 tp(p.x + w - ts.x - 18.f, p.y + (h - ts.y) * 0.5f);
            dl->AddRectFilled(tp - ImVec2(6.f, 2.f), tp + ts + ImVec2(6.f, 2.f),
                              U32(complete ? kFrame : Accent(0.30f)), 6.f);
            dl->AddText(tp, U32(complete ? kTextDim : kText), tag);

            if (clicked) ChatOverlay::Use(c.text);
        }

        ImGui::Dummy({ w, 2.f });
        ImGui::TextDisabled("Clic ou Alt+chiffre  ·  INSER : gérer la liste");
        ImGui::End();
    }
}

void Ui::ToggleMenu() {
    g_menuOpen = !g_menuOpen;
    InputHook::NotifyMenuChanged(g_menuOpen);
}
bool Ui::IsMenuOpen() { return g_menuOpen; }

void Ui::Render() {
    ApplyTheme();

    const bool open = g_menuOpen;
    ImGui::GetIO().MouseDrawCursor = open;   // le jeu cache le curseur : ImGui dessine le sien
    if (open) ClipCursor(nullptr);

    ChatOverlay::Sample(InputHook::Window());
    HealthBars::Render();        // dessine en arriere-plan, sous le HUD et le menu
    Zoom::Update();
    Hud::RenderArmor(open);
    Hud::RenderFps(open);
    if (open) Menu::Render();
    else RenderChatOverlay();

    if (g_wasOpen && !open) Config::SaveSettings(); // sauvegarde a la fermeture du menu
    g_wasOpen = open;
}

void Menu::Render() {
    Settings& s = Config::GetSettings();
    const ImVec2 display = ImGui::GetIO().DisplaySize;

    ImGui::SetNextWindowSize({ 760.f, 500.f }, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(display * 0.5f, ImGuiCond_FirstUseEver, { 0.5f, 0.5f });
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 0.f, 0.f });
    const bool visible = ImGui::Begin("##NexusMenu", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar();
    if (!visible) { ImGui::End(); return; }

    // ---- Barre laterale ---------------------------------------------------------
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kSidebar);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 12.f, 18.f });
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.f);
    ImGui::BeginChild("##sidebar", { 190.f, 0.f }, ImGuiChildFlags_AlwaysUseWindowPadding);
    {
        const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
        ImGui::GetWindowDrawList()->AddRectFilledMultiColor(
            wp, { wp.x + ws.x, wp.y + 140.f },
            U32(Accent(0.14f)), U32(Accent(0.06f)), U32(Accent(0.f)), U32(Accent(0.f)));

        // Logo : hexagone a l'accent, avec un N
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 c = p + ImVec2(14.f, 15.f);
        const float r = 13.f;
        ImVec2 hex[6];
        for (int i = 0; i < 6; ++i) {
            const float a = 3.14159265f / 3.f * i + 3.14159265f / 6.f;
            hex[i] = { c.x + std::cos(a) * r, c.y + std::sin(a) * r };
        }
        dl->AddConvexPolyFilled(hex, 6, U32(Accent()));
        dl->AddPolyline(hex, 6, U32(Mix(Accent(), ImVec4(1, 1, 1, 1), 0.35f)), ImDrawFlags_Closed, 1.5f);
        const ImU32 white = IM_COL32(255, 255, 255, 245);
        dl->AddLine(c + ImVec2(-4.5f, 5.5f), c + ImVec2(-4.5f, -5.5f), white, 2.2f);
        dl->AddLine(c + ImVec2(-4.5f, -5.5f), c + ImVec2(4.5f, 5.5f), white, 2.2f);
        dl->AddLine(c + ImVec2(4.5f, 5.5f), c + ImVec2(4.5f, -5.5f), white, 2.2f);

        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 36.f);
        ImGui::BeginGroup();
        if (Ui::TitleFont) ImGui::PushFont(Ui::TitleFont);
        ImGui::TextUnformatted("Nexus");
        if (Ui::TitleFont) ImGui::PopFont();
        ImGui::EndGroup();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 6.f);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 36.f);
        ImGui::TextDisabled("Client Bedrock");
        ImGui::Dummy({ 0.f, 10.f });

        for (int i = 0; i < IM_ARRAYSIZE(kTabs); ++i)
            if (SidebarButton(kTabs[i].name, g_tab == i)) g_tab = i;

        // Raccourcis en bas
        ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 92.f);
        ImGui::TextDisabled("INSER   menu");
        ImGui::TextDisabled("FIN       décharger");
        ImGui::Dummy({ 0.f, 2.f });
        ImGui::TextColored(Accent(0.75f), "par %s", "Claude");
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    ImGui::SameLine(0.f, 0.f);

    // ---- Contenu ------------------------------------------------------------------
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 22.f, 20.f });
    ImGui::BeginChild("##content", { 0.f, 0.f }, ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    {
        const Tab& tab = kTabs[g_tab];
        const float right = ImGui::GetContentRegionAvail().x;
        const float x0 = ImGui::GetCursorPosX();

        if (Ui::TitleFont) ImGui::PushFont(Ui::TitleFont);
        ImGui::TextUnformatted(tab.name);
        if (Ui::TitleFont) ImGui::PopFont();

        // Bouton fermer
        const float btn = 28.f;
        ImGui::SameLine(x0 + right - btn);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##close", { btn, btn })) Ui::ToggleMenu();
        const bool hov = ImGui::IsItemHovered();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (hov) dl->AddRectFilled(p, p + ImVec2(btn, btn), IM_COL32(255, 255, 255, 18), 6.f);
        const ImVec2 c = p + ImVec2(btn * 0.5f, btn * 0.5f);
        const ImU32 xc = U32(hov ? kText : kTextDim);
        dl->AddLine(c - ImVec2(5, 5), c + ImVec2(5, 5), xc, 2.f);
        dl->AddLine(c + ImVec2(-5, 5), c + ImVec2(5, -5), xc, 2.f);

        ImGui::TextDisabled("%s", tab.subtitle);
        ImGui::Dummy({ 0.f, 8.f });

        tab.draw(s);
    }
    ImGui::EndChild();

    ImGui::End();
}
