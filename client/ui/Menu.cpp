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
#include "hooks/FocusGrab.h"
#include "modules/HealthBars.h"
#include "modules/Stats.h"
#include "modules/Zoom.h"
#include "modules/OwnNametag.h"
#include "modules/ThirdPersonNametag.h"
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
#include <cstring>
#include <vector>
#include <unordered_map>

namespace {
    std::atomic<bool> g_menuOpen{ false };
    bool g_wasOpen = false;
    int  g_tab = 0;

    float g_cardX0 = 0.f, g_cardW = 0.f; // geometrie de la carte en cours (pour aligner les lignes)

    // ---- Couleurs ---------------------------------------------------------------
    // Couleur d'accent : delegue a Ui::AccentColor (partagee avec le HUD, rainbow-aware).
    ImVec4 Accent(float a = 1.f) { return Ui::AccentColor(a); }
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
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const float totalW = ImGui::GetContentRegionAvail().x;
        const float gap = 6.f;
        const float h = ImGui::GetFrameHeight();
        const float cellW = (totalW - gap * (n - 1)) / n;   // chaque bouton + un ecart net entre eux
        bool changed = false;
        for (int i = 0; i < n; ++i) {
            ImGui::SetCursorScreenPos({ start.x + i * (cellW + gap), start.y });   // position absolue : pas de chevauchement
            const bool sel = *v == i;
            ImGui::PushStyleColor(ImGuiCol_Button, sel ? Accent(0.85f) : kFrame);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, sel ? Accent() : kFrameHov);
            ImGui::PushStyleColor(ImGuiCol_Text, sel ? ImVec4(0.05f, 0.05f, 0.06f, 1.f) : kText);
            if (ImGui::Button(items[i], { cellW, h })) { *v = i; changed = true; }
            ImGui::PopStyleColor(3);
        }
        // Reserve la hauteur de la rangee pour que la suite se place en dessous.
        ImGui::SetCursorScreenPos(start);
        ImGui::Dummy({ totalW, h });
        ImGui::PopID();
        return changed;
    }

    bool SidebarButton(const char* label, bool selected, const char* badge = nullptr) {
        const ImVec2 size(ImGui::GetContentRegionAvail().x, 36.f);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const bool pressed = ImGui::InvisibleButton(label, size);
        const bool hovered = ImGui::IsItemHovered();

        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (selected)     dl->AddRectFilled(p, p + size, U32(Accent(0.16f)), 8.f);
        else if (hovered) dl->AddRectFilled(p, p + size, IM_COL32(255, 255, 255, 10), 8.f);
        if (selected)     dl->AddRectFilled({ p.x, p.y + 9.f }, { p.x + 3.f, p.y + size.y - 9.f }, U32(Accent()), 2.f);

        // Onglet dev : libelle en italique simule (legere inclinaison impossible ici) -> couleur attenuee + point
        const ImU32 textCol = badge ? IM_COL32(150, 160, 185, 255)
                                    : U32(selected ? kText : (hovered ? Mix(kTextDim, kText, 0.5f) : kTextDim));
        const ImVec2 textPos(p.x + 16.f, p.y + (size.y - ImGui::GetTextLineHeight()) * 0.5f);
        dl->AddText(textPos, badge && selected ? U32(kText) : textCol, label);

        // Badge "DEV" a droite
        if (badge) {
            const ImVec4 devc(0.55f, 0.75f, 1.0f, 1.f);   // bleu "dev"
            const float fs = ImGui::GetFontSize() * 0.62f;
            const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0.f, badge);
            const float px = 5.f, py = 2.f;
            const ImVec2 bmax(p.x + size.x - 10.f, p.y + (size.y - ts.y) * 0.5f - py + ts.y + py * 2.f);
            const ImVec2 bmin(bmax.x - ts.x - px * 2.f, p.y + (size.y - ts.y) * 0.5f - py);
            dl->AddRect(bmin, bmax, U32(ImVec4(devc.x, devc.y, devc.z, 0.55f)), (ts.y + py * 2.f) * 0.5f, 0, 1.f);
            dl->AddText(ImGui::GetFont(), fs, { bmin.x + px, bmin.y + py }, U32(devc), badge);
        }
        return pressed;
    }

    // Carte de module : titre, description, interrupteur a droite.
    // Renvoie true si le contenu (reglages) doit etre affiche.
    std::unordered_map<std::string, bool> g_expanded;   // etat deplie/replie par carte

    // Carte de module : clic sur l'en-tete = deplier/replier les reglages ; l'interrupteur
    // ne fait qu'activer/desactiver. Les cartes sans interrupteur (reglages generaux) restent
    // toujours ouvertes.
    // perfCost : 0 = aucun badge, 1 = impact moyen, 2 = impact lourd (petit PC).
    bool BeginCard(const char* id, const char* title, const char* desc, bool* toggle = nullptr, bool available = true, bool hasSettings = true, bool collapsibleNoToggle = false, int perfCost = 0) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 16.f, 14.f });
        ImGui::PushStyleColor(ImGuiCol_ChildBg, kCard);
        ImGui::BeginChild(id, { 0.f, 0.f }, ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();

        g_cardX0 = ImGui::GetCursorPosX();
        g_cardW = ImGui::GetContentRegionAvail().x;

        const bool hasSwitch   = (toggle != nullptr);                                         // interrupteur maitre (independant)
        const bool collapsible = hasSettings && (toggle != nullptr || collapsibleNoToggle);   // fleche + corps dépliable
        bool& expanded = g_expanded[id];

        const float switchW = hasSwitch ? ImGui::GetFrameHeight() * 0.82f * 1.8f : 0.f;
        const float chevronW = collapsible ? 20.f : 0.f;
        const ImVec2 headerPos = ImGui::GetCursorScreenPos();

        // Contenu de l'en-tete (titre + description), dessine comme du texte non cliquable.
        ImGui::BeginGroup();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + chevronW);
        ImGui::TextUnformatted(title);
        const ImVec2 titleMin = ImGui::GetItemRectMin(), titleMax = ImGui::GetItemRectMax();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + chevronW);
        ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
        ImGui::PushTextWrapPos(g_cardX0 + g_cardW - switchW - 10.f);
        ImGui::TextUnformatted(desc);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::EndGroup();
        const float headerH = ImGui::GetItemRectSize().y;

        // Badge d'avertissement perf, a droite du titre (ne perturbe pas la mise en page).
        if (perfCost > 0) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const char* txt = perfCost >= 2 ? "LOURD" : "MOYEN";
            const ImU32 bg  = perfCost >= 2 ? IM_COL32(220, 70, 70, 55)  : IM_COL32(230, 170, 60, 50);
            const ImU32 fg  = perfCost >= 2 ? IM_COL32(255, 150, 150, 255) : IM_COL32(255, 212, 130, 255);
            const ImVec2 ts = ImGui::CalcTextSize(txt);
            const float padx = 5.f;
            const ImVec2 p0(titleMax.x + 8.f, titleMin.y - 1.f);
            const ImVec2 p1(p0.x + ts.x + padx * 2.f, titleMax.y + 1.f);
            dl->AddRectFilled(p0, p1, bg, 4.f);
            dl->AddText(ImVec2(p0.x + padx, (p0.y + p1.y - ts.y) * 0.5f), fg, txt);
        }

        // Interrupteur a droite (capture ses clics en premier).
        if (hasSwitch) {
            ImGui::SameLine(g_cardX0 + g_cardW - switchW);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (headerH - ImGui::GetFrameHeight() * 0.82f) * 0.5f);
            ToggleSwitch("##toggle", toggle, available);
        }

        // Zone cliquable de l'en-tete (titre/description) : deplie / replie.
        if (collapsible) {
            ImGui::SetCursorScreenPos(headerPos);
            if (ImGui::InvisibleButton("##hdr", { g_cardW - switchW - 8.f, headerH })) expanded = !expanded;
            const bool hov = ImGui::IsItemHovered();
            if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

            // Chevron
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 c = headerPos + ImVec2(6.f, ImGui::GetTextLineHeight() * 0.5f + 1.f);
            const ImU32 col = U32(hov ? kText : kTextDim);
            if (expanded) {   // v
                dl->AddLine(c + ImVec2(-4, -2), c + ImVec2(0, 2), col, 1.8f);
                dl->AddLine(c + ImVec2(0, 2), c + ImVec2(4, -2), col, 1.8f);
            } else {          // >
                dl->AddLine(c + ImVec2(-2, -4), c + ImVec2(2, 0), col, 1.8f);
                dl->AddLine(c + ImVec2(2, 0), c + ImVec2(-2, 4), col, 1.8f);
            }
        }

        // Dépliable -> suit l'etat ; interrupteur seul -> pas de corps ; carte info -> toujours visible.
        const bool showContent = collapsible ? expanded : (hasSwitch ? false : true);
        if (showContent && collapsible) {
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

    // Petit "!" d'aide : affiche une infobulle au survol.
    void HelpMarker(const char* desc) {
        const auto& ac = Config::GetSettings().accent;
        ImGui::TextColored(ImVec4(ac[0], ac[1], ac[2], 1.f), "(!)");
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 20.f);
            ImGui::TextUnformatted(desc);
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }

    // Comme RowLabel mais avec un "!" d'aide (infobulle) juste apres le libelle.
    void RowLabelHelp(const char* label, const char* help) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(0.f, 5.f);
        HelpMarker(help);
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

    // Repere jaune "recommande petit PC" sur le dernier slider dessine : une marque a la
    // valeur conseillee + une infobulle jaune nommant cette valeur au survol.
    void RecoMarker(float reco, float lo, float hi, const char* valueText) {
        const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
        const ImGuiStyle& st = ImGui::GetStyle();
        const float grab = st.GrabMinSize * 0.5f + st.FramePadding.x;
        float t = (reco - lo) / (hi - lo); t = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
        const float x = mn.x + grab + t * (mx.x - mn.x - 2.f * grab);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 yel = IM_COL32(255, 205, 70, 255);
        dl->AddLine(ImVec2(x, mn.y + 1.f), ImVec2(x, mx.y - 1.f), IM_COL32(255, 205, 70, 150), 2.f);
        dl->AddCircleFilled(ImVec2(x, mn.y - 1.f), 3.f, yel, 12);
        if (ImGui::IsItemHovered()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.80f, 0.27f, 1.f));
            ImGui::SetTooltip("Recommandé petit PC : %s", valueText);
            ImGui::PopStyleColor();
        }
    }
    bool RecoSliderInt(const char* id, int* v, int lo, int hi, const char* fmt, int reco, const char* recoText) {
        const bool r = ImGui::SliderInt(id, v, lo, hi, fmt);
        RecoMarker((float)reco, (float)lo, (float)hi, recoText);
        return r;
    }
    bool RecoSliderFloat(const char* id, float* v, float lo, float hi, const char* fmt, float reco,
                         const char* recoText, ImGuiSliderFlags flags = 0) {
        const bool r = ImGui::SliderFloat(id, v, lo, hi, fmt, flags);
        RecoMarker(reco, lo, hi, recoText);
        return r;
    }

    // Mini ecran cliquable : 6 positions predefinies (coins et centres haut/bas)
    // withHotbar : ajoute deux points en bas (de part et d'autre du centre) representant
    // "a gauche / a droite de la hotbar" (ancres 7 et 8).
    void AnchorPicker(int* anchor, bool withHotbar = false) {
        const float w = 168.f, h = 94.f;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::Dummy({ w, h });
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, p + ImVec2(w, h), U32(kFrame), 8.f);
        dl->AddRect(p, p + ImVec2(w, h), U32(ImVec4(1, 1, 1, 0.10f)), 8.f, 0, 1.5f);

        const float pad = 14.f, r = 8.f;
        const float left = p.x + pad, span = w - 2 * pad;
        const float botY = p.y + h - pad;

        // Petite "hotbar" dessinee en bas-centre pour reperer les positions 7/8.
        if (withHotbar) {
            const float hbw = span * 0.34f, hbh = 7.f;
            dl->AddRectFilled({ p.x + w * 0.5f - hbw * 0.5f, botY - hbh * 0.5f },
                              { p.x + w * 0.5f + hbw * 0.5f, botY + hbh * 0.5f }, U32(ImVec4(1, 1, 1, 0.22f)), 2.f);
        }

        auto point = [&](int id, ImVec2 c, float rad) {
            ImGui::SetCursorScreenPos(c - ImVec2(rad + 4, rad + 4));
            ImGui::PushID(id);
            if (ImGui::InvisibleButton("##a", { 2 * rad + 8, 2 * rad + 8 })) *anchor = id;
            const bool hov = ImGui::IsItemHovered();
            ImGui::PopID();
            const bool sel = *anchor == id;
            dl->AddCircleFilled(c, sel ? rad : rad - 2.f, U32(sel ? Accent() : (hov ? kTextDim : ImVec4(1, 1, 1, 0.18f))));
            if (sel) dl->AddCircle(c, rad + 3.f, U32(Accent(0.5f)), 0, 1.5f);
        };

        for (int i = 0; i < 6; ++i) {
            const int col = i % 3, row = i / 3;
            point(i + 1, ImVec2(left + span * 0.5f * col, row == 0 ? p.y + pad : botY), r);
        }
        if (withHotbar) {                                   // 7 = gauche hotbar, 8 = droite hotbar
            point(7, ImVec2(left + span * 0.30f, botY), r - 1.f);
            point(8, ImVec2(left + span * 0.70f, botY), r - 1.f);
        }
        ImGui::SetCursorScreenPos({ p.x, p.y + h + ImGui::GetStyle().ItemSpacing.y });
    }

    // Bloc de controles de position unifie (tous les HUD) : libelle "Position" + rectangle
    // (picker avec points hotbar) centre a cote, puis position libre, ecart/marge et hauteur.
    // 'tag' unique par module.
    // scale (optionnel) : taille du HUD, heritee du systeme de position. minScale depend du
    // contenu du module (texte seul -> petit mini ; icones -> mini plus grand) pour qu'il reste
    // toujours lisible. Passer scale=nullptr pour un module sans reglage de taille.
    void PosControls(const char* tag, int& anchor, float& marginX, float& marginY,
                     float* scale = nullptr, float minScale = 0.6f, float maxScale = 3.0f,
                     float* bgAlpha = nullptr) {
        char id[48];
        const bool free = (anchor == 0);

        // Libelle a gauche, rectangle centre dans l'espace a droite.
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Position");
        const float availX0 = g_cardX0 + g_cardW * 0.40f;
        const float availW  = g_cardW - g_cardW * 0.40f;
        const float pickerW = 168.f;
        const float off = (availW - pickerW) * 0.5f;
        ImGui::SameLine(availX0 + (off > 0.f ? off : 0.f));
        ImGui::BeginGroup();
        AnchorPicker(&anchor, true);   // 6 ancres + 2 points hotbar (7/8)
        snprintf(id, sizeof(id), "Position libre (à faire glisser)##%s", tag);
        if (ImGui::RadioButton(id, free)) anchor = 0;
        ImGui::EndGroup();

        // Horizontal : decalage SIGNE centre sur 0 (gauche < 0 < droite). Vertical : marge depuis
        // le bord. Slider visible + Ctrl+clic pour saisir une valeur exacte au clavier.
        //
        // IMPORTANT : la saisie clavier native d'ImGui (Ctrl+clic) repose sur l'etat des
        // modificateurs, qu'ImGui lit via GetKeyState -- or notre hook clavier bas-niveau BLOQUE
        // les touches, donc ImGui croit toujours Ctrl relache. On gere donc tout nous-memes :
        // on lit le Ctrl PHYSIQUE (GetAsyncKeyState) et on bascule le slider en InputFloat.
        static char g_editId[64] = "";   // id du slider en cours de saisie (vide = aucun)
        static int  g_editFrames = 0;    // images ecoulees depuis l'ouverture (laisse le focus s'etablir)
        auto marginSlider = [&](const char* label, const char* sid, float* v, float lo, float hi) {
            RowLabel(label);
            snprintf(id, sizeof(id), "%s%s", sid, tag);
            const bool editing = (strcmp(g_editId, id) == 0);
            if (editing) {
                if (g_editFrames == 0) ImGui::SetKeyboardFocusHere();
                ++g_editFrames;
                const bool enter = ImGui::InputFloat(id, v, 0.f, 0.f, "%.0f",
                    ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
                const bool active = ImGui::IsItemActive();
                // Valide sur Entree, ou quand le champ perd le focus (clic ailleurs). Le delai de
                // 2 images evite de fermer avant que le focus clavier ne soit reellement pris.
                if (enter || (g_editFrames > 2 && !active)) {
                    if (*v < lo) *v = lo; else if (*v > hi) *v = hi;   // borne au min/max
                    g_editId[0] = '\0'; g_editFrames = 0;
                }
            } else {
                const float before = *v;
                ImGui::SliderFloat(id, v, lo, hi, "%.0f px",
                    ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput);
                if (*v < lo) *v = lo; else if (*v > hi) *v = hi;
                // Ctrl + clic sur le slider -> bascule en saisie clavier (sans laisser le clic
                // deplacer la valeur : on restaure la valeur d'avant le clic). On lit le Ctrl via
                // notre hook clavier (g_kb), car GetAsyncKeyState ne voit pas une touche bloquee.
                const bool ctrl = FocusGrab::IsCtrlDown();
                if (ctrl && ImGui::IsItemClicked()) {
                    *v = before;
                    strncpy(g_editId, id, sizeof(g_editId) - 1); g_editId[sizeof(g_editId) - 1] = '\0';
                    g_editFrames = 0;
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Ctrl+clic pour saisir une valeur exacte (bornee au min/max)");
            }
        };
        if (!free) {
            marginSlider("Décalage horizontal", "##mgx", &marginX, -2000.f, 2000.f);
            marginSlider("Marge verticale",     "##mgy", &marginY, 0.f, 2000.f);
        }
        // Taille du HUD (heritee, comme la position). Le minimum depend du contenu du module :
        // on borne pour que le HUD ne descende jamais sous une taille lisible.
        if (scale) {
            RowLabel("Taille");
            snprintf(id, sizeof(id), "##size%s", tag);
            ImGui::SliderFloat(id, scale, minScale, maxScale, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
            if (*scale < minScale) *scale = minScale; else if (*scale > maxScale) *scale = maxScale;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Taille du HUD (minimum adapte au contenu pour rester lisible)");
        }
        // Opacite du fond et de la bordure (heritee, comme la position). Le texte et les icones
        // restent pleinement opaques. 0 % = fond/bordure invisibles, 100 % = pleinement visibles.
        if (bgAlpha) {
            RowLabel("Opacité du fond");
            snprintf(id, sizeof(id), "##bga%s", tag);
            float pct = *bgAlpha * 100.f;
            if (ImGui::SliderFloat(id, &pct, 0.f, 100.f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp))
                *bgAlpha = std::clamp(pct / 100.f, 0.f, 1.f);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Opacite du fond et de la bordure (le texte reste opaque)");
        }
        // Barre de separation entre le bloc Position (herite) et les options propres au module.
        ImGui::Dummy({ 0.f, 4.f }); ImGui::Separator(); ImGui::Dummy({ 0.f, 4.f });
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
        ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
        ImGui::TextWrapped("Les badges LOURD / MOYEN signalent les modules les plus gourmands en ressources (à désactiver sur un petit PC).");
        ImGui::PopStyleColor();
        ImGui::Dummy({ 0.f, 6.f });
        const char* armorDesc = Game::IsConfigured()
            ? "Usure de chaque pièce et de l'objet en main."
            : "Offsets à compléter : visible seulement en mode démo.";
        if (BeginCard("##armor", "Durabilité de l'armure", armorDesc, &s.armorEnabled, true, true, false, 1)) {
            static const char* layouts[] = { "Vertical", "Horizontal" };
            static const char* modes[] = { "Valeur", "%", "Les deux" };
            static const char* idmodes[] = { "Nom", "Icône" };
            PosControls("armor", s.armorAnchor, s.armorMargin, s.armorHotbarY, &s.scale, 0.5f, 2.5f, &s.armorBgAlpha);
            ImGui::Dummy({ 0.f, 2.f });
            RowLabel("Disposition");   Segmented("layout", &s.layout, layouts, 2);
            RowLabel("Libellé");       Segmented("armidmode", &s.armorDisplayMode, idmodes, 2);
            RowLabel("Affichage");     Segmented("mode", &s.valueMode, modes, 3);
            RowLabel("Alerte sous");   ImGui::SliderInt("##low", &s.lowThreshold, 0, 50, "%d %%");
            RowToggle("Objet en main", &s.showHeldItem);
        }
        EndCard();

        {
            const bool ok = HealthBars::IsConfigured();
            char desc[320];
            if (ok) snprintf(desc, sizeof(desc), "Barre de vie au-dessus des entités proches.");
            else    snprintf(desc, sizeof(desc), "Manque dans offsets.ini [entities] : %s", HealthBars::MissingText());
            if (BeginCard("##health", "Barres de vie", desc, &s.healthEnabled, ok, true, false, 2)) {
                // Avertissement anticheat (visible, couleur ambre).
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 180, 60, 255));
                ImGui::PushTextWrapPos(0.f);
                ImGui::TextUnformatted("/!\\ Peut être considéré comme un cheat : la plupart des serveurs l'interdisent. À utiliser en solo ou sur un serveur qui l'autorise.");
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
                ImGui::Dummy({ 0.f, 3.f });
                ImGui::TextDisabled("Afficher pour");
                RowToggle("Animaux (vaches, cochons, loups...)", &s.hpAnimals);
                RowToggle("Monstres (zombies, squelettes...)", &s.hpMonsters);
                RowToggle("Autres (villageois, golems...)", &s.hpOthers);
                ImGui::Dummy({ 0.f, 2.f });
                ImGui::Separator();
                ImGui::Dummy({ 0.f, 2.f });
                RowLabel("Distance max");  RecoSliderFloat("##hpdist", &s.hpMaxDistance, 4.f, 64.f, "%.0f blocs", 16.f, "16 blocs");
                RowLabel("Taille");        ImGui::SliderFloat("##hpscale", &s.hpBarScale, 0.5f, 2.f, "%.2fx");
                RowToggle("Afficher la valeur (15/20)", &s.hpShowValue);
                RowToggle("Afficher le nom de l'entité", &s.hpShowName);
                RowToggle("Masquer si vie pleine", &s.hpHideFull);
            }
            EndCard();
        }

        {
            const bool ok = ThirdPersonNametag::IsConfigured();
            BeginCard("##tpnametag", "Nametag en 3e personne",
                      ok ? "Affiche ton propre pseudo au-dessus de ta tête en vue à la troisième personne (F5)."
                         : "Signature introuvable sur cette version.",
                      &s.thirdPersonNametag, ok, false);
            EndCard();
        }

        {
            const bool ok = Zoom::IsConfigured();
            if (BeginCard("##zoom", "Zoom",
                          ok ? "Maintiens la touche pour zoomer, comme avec une longue-vue."
                             : "Section [zoom] de offsets.ini à compléter.",
                          &s.zoomEnabled, ok)) {
                // Position de l'indicateur en premier (systeme herite + separateur), comme CPS.
                PosControls("zoom", s.zoomAnchor, s.zoomHudMargin, s.zoomHotbarY, &s.zoomHudScale, 0.6f, 3.0f, &s.zoomBgAlpha);
                RowLabel("Touche");  KeyBindButton("zoomkey", &s.zoomKey);
                static const char* modes[] = { "Maintenir", "Basculer" };
                RowLabel("Mode");    Segmented("zoommode", &s.zoomMode, modes, 2);
                RowLabel("Niveau");  ImGui::SliderFloat("##zoomfactor", &s.zoomFactor, 2.f, 10.f, "x%.1f");
                RowToggle("Zoom fluide", &s.zoomSmooth);
                RowToggle("Indicateur à l'écran", &s.zoomIndicator);
                ImGui::TextDisabled("Le zoom est désactivé quand le chat ou ce menu est ouvert.");
            }
            EndCard();
        }

        if (BeginCard("##fps", "Compteur FPS", "Images par seconde, en temps réel.", &s.fpsEnabled)) {
            // Position (systeme unifie : ancres + hotbar + ecart + hauteur)
            PosControls("fps", s.fpsAnchor, s.fpsMargin, s.fpsHotbarY, &s.fpsScale, 0.6f, 3.0f, &s.fpsBgAlpha);

            static const char* colorModes[] = { "Accent", "Selon perf.", "Blanc" };
            RowLabel("Couleur");        Segmented("fpscolor", &s.fpsColorMode, colorModes, 3);
            if (s.fpsColorMode == 1) {
                RowLabel("FPS visés");  RecoSliderInt("##fpstarget", &s.fpsTarget, 30, 240, "%d", 60, "60");
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

        if (BeginCard("##cpscombo", "CPS, Combo & Reach", "Clics par seconde, série de coups et portée, dans un même panneau.", &s.cpsComboEnabled)) {
            // Un seul panneau : position + taille partagees (systeme unifie).
            PosControls("cps", s.cpsAnchor, s.cpsMargin, s.cpsHotbarY, &s.cpsScale, 0.6f, 2.5f, &s.cpsBgAlpha);

            ImGui::Dummy({ 0.f, 6.f }); ImGui::Separator(); ImGui::Dummy({ 0.f, 6.f });
            // CPS
            RowToggle("Afficher le CPS", &s.cpsEnabled);
            if (s.cpsEnabled) RowToggle("Inclure le clic droit", &s.cpsShowRight);
            // Combo
            RowToggle("Afficher le combo", &s.comboEnabled);
            if (s.comboEnabled) RowToggle("Afficher le record", &s.comboShowMax);
            // Reach (portee du dernier coup)
            RowToggle("Afficher la portée (reach)", &s.reachEnabled);
            RowToggle("Afficher la portée reçue", &s.reachTakenEnabled);
            if (s.reachTakenEnabled) ImGui::TextDisabled("Portée des coups reçus (surtout fiable en monde local).");
            // Delai d'inactivite commun (combo + reach) : visible si l'un d'eux est actif.
            if (s.comboEnabled || s.reachEnabled || s.reachTakenEnabled) {
                RowLabelHelp("Pause (ms)", "Délai d'inactivité au bout duquel le combo ET les "
                             "valeurs de portée (reach / reach reçue) retombent à zéro. "
                             "Ex. 1000 ms = tout s'efface si rien ne se passe pendant 1 seconde.");
                ImGui::SliderInt("##comboreset", &s.comboResetMs, 250, 3000, "%d ms");
            }
        }
        EndCard();

        ImGui::TextDisabled("Astuce : menu ouvert, fais glisser les éléments du HUD pour les déplacer.");
    }

    void PageMovement(Settings& s) {
        const bool ok = AutoSprint::IsConfigured();
        BeginCard("##sprint", "Auto-sprint",
                  ok ? "Sprinte automatiquement quand tu avances. Les règles du jeu s'appliquent (faim, etc.)."
                     : "Indisponible.",
                  &s.autoSprint, ok, false);
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
        const bool busy = ChatSender::Busy();

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

            // Filtres : recherche a gauche, niveau a droite (largeur fixe, libelles courts)
            static const char* levels[] = { "Tout", "Alertes", "Erreurs" };
            const float segW = 250.f;
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - segW - 8.f);
            ImGui::InputTextWithHint("##logsearch", "Rechercher dans le journal...", search, sizeof(search));
            ImGui::SameLine(0.f, 8.f);
            ImGui::BeginChild("##logseg", { segW, ImGui::GetFrameHeight() }, ImGuiChildFlags_None,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);
            Segmented("loglevel", &minLevel, levels, 3);
            ImGui::EndChild();
            ImGui::Dummy({ 0.f, 2.f });

            // Lignes : heure | badge de niveau | message (retour a la ligne)
            const ImVec4 colWarn(1.f, 0.72f, 0.25f, 1.f), colErr(0.98f, 0.40f, 0.40f, 1.f);
            std::string filter = search;
            for (auto& ch : filter) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));

            ImGui::PushStyleColor(ImGuiCol_ChildBg, kSidebar);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 6.f, 6.f });
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.f);
            ImGui::BeginChild("##loglines", { 0.f, 330.f }, ImGuiChildFlags_AlwaysUseWindowPadding);
            ImGui::PopStyleVar(2);
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, { 8.f, 0.f });

            ImDrawList* dl = ImGui::GetWindowDrawList();
            const float x0 = ImGui::GetCursorPosX();
            const float timeW = ImGui::CalcTextSize("00:00:00").x;
            const float badgeW = ImGui::CalcTextSize("ERREUR").x + 14.f;
            const float msgX = x0 + 8.f + timeW + 10.f + badgeW + 10.f;
            const float rowPadY = 5.f;
            std::string all;   // texte visible, pour "Copier"
            int row = 0;

            for (const auto& e : entries) {
                if (static_cast<int>(e.level) < minLevel) continue;
                if (!filter.empty()) {
                    std::string low = e.message;
                    for (auto& ch : low) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                    if (low.find(filter) == std::string::npos) continue;
                }
                const bool err = e.level == Logger::Level::Error, warn = e.level == Logger::Level::Warn;
                const char* tag = err ? "ERREUR" : warn ? "ALERTE" : "INFO";
                const ImVec4 tagCol = err ? colErr : warn ? colWarn : Accent();
                const ImVec4 msgCol = err ? colErr : warn ? colWarn : kText;

                // Ligne mise en page dans un groupe, puis fond (survol / alternance) dessine dessous
                dl->ChannelsSplit(2);
                dl->ChannelsSetCurrent(1);
                const ImVec2 rowStart = ImGui::GetCursorScreenPos();
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + rowPadY);
                ImGui::BeginGroup();
                ImGui::SetCursorPosX(x0 + 8.f);
                ImGui::TextColored(kTextDim, "%s", e.time.c_str());
                ImGui::SameLine(x0 + 8.f + timeW + 10.f);
                {   // badge
                    const ImVec2 bp = ImGui::GetCursorScreenPos();
                    const ImVec2 ts = ImGui::CalcTextSize(tag);
                    const float bh = ImGui::GetTextLineHeight();
                    dl->AddRectFilled(bp, bp + ImVec2(badgeW, bh), U32(ImVec4(tagCol.x, tagCol.y, tagCol.z, 0.16f)), bh * 0.5f);
                    dl->AddText(bp + ImVec2((badgeW - ts.x) * 0.5f, 0.f), U32(tagCol), tag);
                    ImGui::Dummy({ badgeW, bh });
                }
                ImGui::SameLine(msgX);
                ImGui::PushStyleColor(ImGuiCol_Text, msgCol);
                ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x - 8.f);
                ImGui::TextUnformatted(e.message.c_str());
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
                ImGui::EndGroup();
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + rowPadY);

                const ImVec2 rowEnd(ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x,
                                    ImGui::GetCursorScreenPos().y);
                const bool hovered = ImGui::IsMouseHoveringRect(rowStart, rowEnd) && ImGui::IsWindowHovered();
                dl->ChannelsSetCurrent(0);
                if (hovered)          dl->AddRectFilled(rowStart, rowEnd, U32(Accent(0.14f)), 6.f);
                else if (row % 2)     dl->AddRectFilled(rowStart, rowEnd, IM_COL32(255, 255, 255, 6), 6.f);
                dl->ChannelsMerge();

                if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                    ImGui::SetClipboardText((e.time + " [" + tag + "] " + e.message).c_str());
                all += e.time + " [" + tag + "] " + e.message + "\n";
                ++row;
            }
            if (row == 0) {
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 12.f);
                ImGui::SetCursorPosX(x0 + 8.f);
                ImGui::TextDisabled(entries.empty() ? "Journal vide." : "Aucune ligne ne correspond au filtre.");
            }
            ImGui::PopStyleVar();
            if (autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 40.f) ImGui::SetScrollHereY(1.f);
            ImGui::EndChild();
            ImGui::PopStyleColor();

            // Actions
            ImGui::Dummy({ 0.f, 2.f });
            ImGui::Checkbox("Défilement auto", &autoScroll);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Clic sur une ligne du journal : la copier");
            const float bw = 110.f;
            ImGui::SameLine(g_cardX0 + g_cardW - bw * 3 - 16.f);
            if (ImGui::Button("Copier", { bw, 0.f })) ImGui::SetClipboardText(all.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Copie toutes les lignes affichées\n(clic sur une ligne : copie cette ligne seule)");
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
        // --- Apparence ---------------------------------------------------------------
        if (BeginCard("##appearance", "Apparence", "Taille, transparence et couleur de l'interface.")) {
            ImGui::Dummy({ 0.f, 2.f });
            RowLabel("Taille du HUD");    ImGui::SliderFloat("##scale", &s.scale, 0.5f, 2.5f, "%.2fx");
            RowLabel("Opacité du fond (armure)");
            { int pct = (int)(s.armorBgAlpha * 100.f + 0.5f);
              if (ImGui::SliderInt("##alpha", &pct, 0, 100, "%d%%")) s.armorBgAlpha = pct / 100.f; }
            RowLabel("Couleur d'accent"); ColorSwatches(s.accent);
            RowToggle("Rainbow (accent animé)", &s.accentRainbow);
            if (s.accentRainbow) {
                RowLabel("Vitesse du rainbow");
                int pct = (int)(s.accentRainbowSpeed * 100.f + 0.5f);
                if (ImGui::SliderInt("##rbspeed", &pct, 5, 100, "%d%%")) s.accentRainbowSpeed = pct / 100.f;
            }
        }
        EndCard();

        // --- Comportement du menu ----------------------------------------------------
        if (BeginCard("##behavior", "Comportement du menu", "Comment le jeu est bloqué quand le menu est ouvert.")) {
            ImGui::Dummy({ 0.f, 2.f });
            RowLabel("Blocage du jeu");

            // Deux choix : Discret (recommande, en dore) et Desactive. menuBlockMode : 0 = discret, 2 = desactive.
            const ImVec4 gold(1.00f, 0.78f, 0.28f, 1.f), goldHi(1.00f, 0.85f, 0.42f, 1.f);
            const bool discret = s.menuBlockMode != 2;
            const float totalW = ImGui::GetContentRegionAvail().x;
            const float gap = 6.f, h = ImGui::GetFrameHeight();
            const float cellW = (totalW - gap) / 2.f;
            const ImVec2 start = ImGui::GetCursorScreenPos();

            ImGui::SetCursorScreenPos(start);
            ImGui::PushStyleColor(ImGuiCol_Button, discret ? gold : kFrame);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, discret ? goldHi : kFrameHov);
            ImGui::PushStyleColor(ImGuiCol_Text, discret ? ImVec4(0.12f, 0.08f, 0.02f, 1.f) : kText);
            if (ImGui::Button("Discret", { cellW, h })) s.menuBlockMode = 0;
            ImGui::PopStyleColor(3);
            const ImVec2 dMin = ImGui::GetItemRectMin(), dMax = ImGui::GetItemRectMax();

            ImGui::SetCursorScreenPos({ start.x + cellW + gap, start.y });
            ImGui::PushStyleColor(ImGuiCol_Button, !discret ? ImVec4(0.55f, 0.18f, 0.20f, 1.f) : kFrame);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, !discret ? ImVec4(0.68f, 0.22f, 0.24f, 1.f) : kFrameHov);
            if (ImGui::Button("Désactivé", { cellW, h })) s.menuBlockMode = 2;
            ImGui::PopStyleColor(2);

            ImGui::SetCursorScreenPos(start);
            ImGui::Dummy({ totalW, h });

            // Petit badge "recommandé" en haut a droite du bouton Discret, qui pulse (grandit/retrecit).
            {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const char* tag = "RECOMMANDÉ";
                const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 3.0f);
                const float fs = ImGui::GetFontSize() * 0.58f * (1.0f + 0.14f * pulse);
                const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0.f, tag);
                const float padx = 5.f, pady = 2.f;
                // En haut a gauche, debordant nettement du bouton (coin).
                const ImVec2 bmin(dMin.x - 12.f, dMin.y - 11.f);
                const ImVec2 bmax(bmin.x + ts.x + padx * 2.f, bmin.y + ts.y + pady * 2.f);
                const int bgA = 170 + static_cast<int>(70 * pulse);
                dl->AddRectFilled(bmin, bmax, IM_COL32(28, 18, 4, bgA), (ts.y + pady * 2.f) * 0.5f);
                dl->AddText(ImGui::GetFont(), fs, { bmin.x + padx, bmin.y + pady }, IM_COL32(255, 224, 130, 255), tag);
            }

            ImGui::Dummy({ 0.f, 2.f });
            ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
            ImGui::PushTextWrapPos(0.f);
            ImGui::TextUnformatted(discret
                ? "Caméra, clics et touches bloqués dans le menu, sans mettre le jeu en pause."
                : "Le jeu continue de recevoir la souris et le clavier dans le menu (déconseillé).");
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();

            ImGui::Dummy({ 0.f, 6.f });
            RowToggle("Message d'accueil", &s.welcomeMessage);
            ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
            ImGui::PushTextWrapPos(0.f);
            ImGui::TextUnformatted("À l'arrivée dans un monde ou un serveur, affiche quelques secondes comment ouvrir le menu et la version de Nexus.");
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }
        EndCard();

        // --- Données & session -------------------------------------------------------
        if (BeginCard("##data", "Données & session", "Tes réglages sont dans %APPDATA%\\Nexus (conservés même après désinstallation).")) {
            ImGui::Dummy({ 0.f, 2.f });
            const float w3 = (ImGui::GetContentRegionAvail().x - 16.f) / 3.f;
            if (AccentButton("Sauvegarder", { w3, 0.f })) Config::SaveSettings();
            ImGui::SameLine();
            if (ImGui::Button("Dossier des réglages", { w3, 0.f }))
                ShellExecuteW(nullptr, L"open", Config::DataFolder().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.45f, 0.14f, 0.22f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.65f, 0.18f, 0.28f, 1.f));
            if (ImGui::Button("Réinitialiser", { w3, 0.f })) ImGui::OpenPopup("##resetconfirm");
            ImGui::PopStyleColor(2);

            if (ImGui::BeginPopup("##resetconfirm")) {
                ImGui::TextUnformatted("Remettre tous les réglages par défaut ?");
                ImGui::Dummy({ 0.f, 4.f });
                if (AccentButton("Oui, réinitialiser", { 160.f, 0.f })) { Config::ResetSettings(); ImGui::CloseCurrentPopup(); }
                ImGui::SameLine();
                if (ImGui::Button("Annuler", { 90.f, 0.f })) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
            ImGui::Dummy({ 0.f, 2.f });
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.16f, 0.18f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.70f, 0.20f, 0.22f, 1.f));
            if (ImGui::Button("Décharger Nexus (FIN)", { -FLT_MIN, 0.f })) Client::RequestUnload();
            ImGui::PopStyleColor(2);
        }
        EndCard();

        // --- A propos ----------------------------------------------------------------
        if (BeginCard("##about", "À propos", "Nexus · client Minecraft Bedrock")) {
            ImGui::Dummy({ 0.f, 2.f });
            RowLabel("Version");    ImGui::TextColored(Accent(), "%s", Credits::kVersion);
            RowLabel("Créé par");   ImGui::TextUnformatted(Credits::kCreatedBy);
            RowLabel("Signatures"); ImGui::TextUnformatted(Credits::kSignaturesBy);
            RowLabel("Versions");   ImGui::TextUnformatted(Credits::kVersionsBy);
            ImGui::Dummy({ 0.f, 2.f });
            ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
            ImGui::PushTextWrapPos(0.f);
            ImGui::TextUnformatted("Le client (launcher, DLL, menu et modules) a été écrit par Claude ; "
                                   "Zoumi a trouvé les signatures du jeu et testé le client.");
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
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

        BeginCard("##demo", "Mode démo", "Affiche de fausses valeurs pour tester le HUD sans offsets.", &s.demoMode, true, false);
        EndCard();

        ImGui::TextDisabled("Journal détaillé : Nexus.log, à côté de la DLL.");
    }

    struct Tab { const char* name; const char* subtitle; void (*draw)(Settings&); bool devOnly; };
    const Tab kTabs[] = {
        { "Visuel",     "Éléments affichés à l'écran",        PageVisual,     false },
        { "Mouvement",  "Aides au déplacement",               PageMovement,   false },
        { "Chat",       "Messages et commandes rapides",      PageChat,       false },
        { "Discord",    "Statut Rich Presence",               PageDiscord,    false },
        { "Paramètres", "Apparence et session",               PageSettings,   false },
        { "Diagnostic", "État des signatures et des offsets", PageDiagnostic, true  },
        { "Journal",    "Messages du client en direct",       PageLogs,       false },
    };

    // Build de developpement : la version contient "-dev". Les onglets "devOnly" n'apparaissent
    // que dans ces builds, pas dans les versions publiees.
    bool IsDevBuild() { return std::strstr(Credits::kVersion, "-dev") != nullptr; }
    bool TabVisible(int i) { return !kTabs[i].devOnly || IsDevBuild(); }
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
    // Anti-rebond : evite qu'un seul appui n'ouvre puis referme (deux fenetres recoivent la touche).
    static ULONGLONG lastToggle = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - lastToggle < 180) return;
    lastToggle = now;

    g_menuOpen = !g_menuOpen;
    InputHook::NotifyMenuChanged(g_menuOpen);
    FocusGrab::OnMenuToggled(g_menuOpen);
}
bool Ui::IsMenuOpen() { return g_menuOpen; }

// Couleur d'accent courante, partagee par le menu et le HUD. Anime (arc-en-ciel) si Rainbow actif.
// La teinte rainbow est calculee une seule fois par frame (mise en cache) : AccentColor est
// appele des dizaines de fois par image, inutile de refaire la conversion HSV a chaque appel.
ImVec4 Ui::AccentColor(float a) {
    const auto& s = Config::GetSettings();
    if (s.accentRainbow) {
        static int   s_frame = -1;
        static float s_r = 1.f, s_g = 1.f, s_b = 1.f;
        const int fr = ImGui::GetFrameCount();
        if (fr != s_frame) {
            s_frame = fr;
            float hue = (float)(ImGui::GetTime() * s.accentRainbowSpeed);
            hue -= std::floor(hue);                  // partie fractionnaire -> teinte 0..1
            ImGui::ColorConvertHSVtoRGB(hue, 0.72f, 1.00f, s_r, s_g, s_b);
        }
        return { s_r, s_g, s_b, a };
    }
    const auto& c = s.accent;
    return { c[0], c[1], c[2], a };
}

void Ui::Render() {
    ApplyTheme();

    // INSER (ouvrir/fermer) et FIN (decharger) detectees ici, quelle que soit la fenetre active.
    {
        static bool insPrev = false, endPrev = false;
        const bool insNow = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
        const bool endNow = (GetAsyncKeyState(VK_END) & 0x8000) != 0;
        if (insNow && !insPrev) Ui::ToggleMenu();
        if (endNow && !endPrev) Client::RequestUnload();
        insPrev = insNow;
        endPrev = endNow;
    }

    const bool open = g_menuOpen;
    ImGui::GetIO().MouseDrawCursor = open;   // le jeu cache le curseur : ImGui dessine le sien
    if (open) ClipCursor(nullptr);

    // Changement de resolution (passage plein ecran, changement d'ecran) : on rescale les
    // elements de HUD en position libre proportionnellement, pour qu'ils gardent la meme
    // place relative a l'ecran. Les elements ancres (coins/bords) restent en place d'eux-memes.
    {
        static ImVec2 lastDisp = { 0.f, 0.f };
        const ImVec2 disp = ImGui::GetIO().DisplaySize;
        if (disp.x > 1.f && disp.y > 1.f) {
            if (lastDisp.x > 1.f && (disp.x != lastDisp.x || disp.y != lastDisp.y)) {
                const float sx = disp.x / lastDisp.x, sy = disp.y / lastDisp.y;
                Settings& st = Config::GetSettings();
                auto reflow = [&](int anchor, float& x, float& y) { if (anchor <= 0) { x *= sx; y *= sy; } };
                reflow(st.fpsAnchor,   st.fpsX,     st.fpsY);
                reflow(st.cpsAnchor,   st.cpsX,     st.cpsY);
                reflow(st.armorAnchor, st.armorX,   st.armorY);
                reflow(st.zoomAnchor,  st.zoomHudX, st.zoomHudY);
            }
            lastDisp = disp;
        }
    }

    Hud::ResetDrag();            // avant de dessiner le HUD
    ChatOverlay::Sample(InputHook::Window());
    HealthBars::Render();        // dessine en arriere-plan, sous le HUD et le menu
    ThirdPersonNametag::Update(); // patch nametag uniquement en 3e personne
    AutoSprint::Update();         // force le sprint si active
    Zoom::Update();
    Hud::RenderArmor(open);
    Hud::RenderFps(open);
    Hud::RenderCpsCombo(open);
    Hud::RenderZoom(open);
    Hud::RenderWelcome();        // message d'accueil a l'arrivee dans un monde/serveur
    if (open) Menu::Render();    // s'efface si on deplace un element du HUD (voir Menu::Render)
    else RenderChatOverlay();

    // Sauvegarde : a la fermeture du menu, et des qu'un deplacement de HUD se termine
    static bool wasDragging = false;
    const bool dragging = Hud::IsDragging();
    if ((g_wasOpen && !open) || (wasDragging && !dragging)) Config::SaveSettings();
    wasDragging = dragging;
    g_wasOpen = open;
}

void Menu::Render() {
    Settings& s = Config::GetSettings();
    const ImVec2 display = ImGui::GetIO().DisplaySize;

    ImGui::SetNextWindowSize({ 940.f, 640.f }, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(display * 0.5f, ImGuiCond_FirstUseEver, { 0.5f, 0.5f });
    // Pendant qu'on deplace un element du HUD, le menu s'efface presque entierement
    // pour voir ou on le pose.
    const bool draggingHud = Hud::IsDragging();
    ImGui::SetNextWindowBgAlpha(draggingHud ? 0.05f : 1.f);
    if (draggingHud) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.15f);
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
    ImGui::BeginChild("##sidebar", { 190.f, 0.f }, ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
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

        for (int i = 0; i < IM_ARRAYSIZE(kTabs); ++i) {
            if (!TabVisible(i)) continue;
            if (SidebarButton(kTabs[i].name, g_tab == i, kTabs[i].devOnly ? "DEV" : nullptr)) g_tab = i;
        }
        if (!TabVisible(g_tab)) g_tab = 0;   // au cas ou l'onglet actif vient d'etre masque

        // Raccourcis en bas
        // Raccourcis en bas, alignes avec le texte des onglets
        {
            const float indent = 16.f;
            const float lh = ImGui::GetTextLineHeightWithSpacing();
            ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 18.f - lh * 3.f - 6.f);
            auto keyLine = [&](const char* key, const char* what) {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent);
                const ImVec2 p = ImGui::GetCursorScreenPos();
                const ImVec2 ks = ImGui::CalcTextSize(key);
                ImGui::GetWindowDrawList()->AddRectFilled(p - ImVec2(4.f, 1.f), p + ks + ImVec2(4.f, 1.f), U32(kFrame), 4.f);
                ImGui::TextUnformatted(key);
                ImGui::SameLine(indent + 12.f + 56.f);
                ImGui::TextDisabled("%s", what);
            };
            keyLine("INSER", "menu");
            keyLine("FIN", "décharger");
            ImGui::Dummy({ 0.f, 6.f });
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent);
            ImGui::TextColored(Accent(0.75f), "par Claude");
        }
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
    if (draggingHud) ImGui::PopStyleVar();   // Alpha
}
