#include "Ui.h"
#include "Credits.h"
#include "core/Config.h"
#include "game/Game.h"
#include "game/IconCache.h"
#include "game/ItemRender.h"
#include "modules/Stats.h"
#include "modules/Zoom.h"
#include <windows.h>
#include <imgui.h>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace {
    bool g_dragging = false;       // un element du HUD est deplace cette image

    // Compensation de resolution : les HUD sont dimensionnes en pixels, donc a haute resolution
    // ils paraissent plus petits (et inversement). On multiplie l'echelle par la hauteur d'ecran
    // rapportee a une reference 1080p, afin que les HUD gardent la MEME taille relative partout.
    float ResFactor() {
        const float h = ImGui::GetIO().DisplaySize.y;
        return h > 1.f ? h / 1080.f : 1.f;
    }

    const char* kLabels[Game::SlotCount] = { "Casque", "Plastron", "Jambières", "Bottes", "Main" };

    ImVec4 Lerp(const ImVec4& a, const ImVec4& b, float t) {
        return { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t };
    }

    // rouge (0 %) -> orange (50 %) -> vert (100 %)
    ImVec4 DurabilityColor(float ratio) {
        const ImVec4 red(0.95f, 0.25f, 0.25f, 1.f), orange(1.f, 0.70f, 0.15f, 1.f), green(0.35f, 0.90f, 0.40f, 1.f);
        ratio = std::clamp(ratio, 0.f, 1.f);
        return ratio < 0.5f ? Lerp(red, orange, ratio * 2.f) : Lerp(orange, green, (ratio - 0.5f) * 2.f);
    }

    void FormatValue(const Game::ItemInfo& it, int mode, char* buf, size_t size) {
        if (!it.present)        { snprintf(buf, size, "vide"); return; }
        if (it.maxDamage <= 0)  { snprintf(buf, size, "-"); return; }
        const int pct = static_cast<int>(it.Ratio() * 100.f + 0.5f);
        switch (mode) {
            case 0:  snprintf(buf, size, "%d/%d", it.Durability(), it.maxDamage); break;
            case 1:  snprintf(buf, size, "%d%%", pct); break;
            default: snprintf(buf, size, "%d/%d (%d%%)", it.Durability(), it.maxDamage, pct); break;
        }
    }

    void DrawBar(float ratio, ImVec4 color, float width, float height) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float y = p.y + (ImGui::GetTextLineHeight() - height) * 0.5f;
        dl->AddRectFilled({ p.x, y }, { p.x + width, y + height }, IM_COL32(0, 0, 0, 160), height * 0.5f);
        if (ratio > 0.f)
            dl->AddRectFilled({ p.x, y }, { p.x + width * std::clamp(ratio, 0.f, 1.f), y + height },
                              ImGui::ColorConvertFloat4ToU32(color), height * 0.5f);
        ImGui::Dummy({ width, ImGui::GetTextLineHeight() });
    }

    // Placeholder d'icone pour l'apercu (menu) : petit carre teinte diamant + losange.
    // Apercu (hors-partie) : silhouette de la piece d'armure correspondante, pour que le HUD
    // d'apercu ressemble au vrai (casque / plastron / jambieres / bottes / objet en main).
    void DrawIconPlaceholder(float size, float scale, int slot = -1) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 o = ImGui::GetCursorScreenPos();
        const float s = size;
        auto X = [&](float fx) { return o.x + fx * s; };
        auto Y = [&](float fy) { return o.y + fy * s; };
        const ImU32 body = IM_COL32(150, 160, 175, 255);   // metal
        const ImU32 hi   = IM_COL32(205, 214, 226, 255);   // reflet clair
        const ImU32 edge = IM_COL32(60, 66, 78, 255);      // contour sombre
        const float rr = 3.f * scale;

        auto box = [&](float x0, float y0, float x1, float y1, float rad) {
            dl->AddRectFilled({ X(x0), Y(y0) }, { X(x1), Y(y1) }, body, rad);
            dl->AddRectFilled({ X(x0), Y(y0) }, { X(x1), Y(y0 + (y1 - y0) * 0.4f) }, hi, rad);
            dl->AddRect({ X(x0), Y(y0) }, { X(x1), Y(y1) }, edge, rad, 0, 1.3f * scale);
        };

        switch (slot) {
            case Game::Helmet:   // dome + visiere
                box(0.22f, 0.16f, 0.78f, 0.62f, s * 0.14f);
                dl->AddRectFilled({ X(0.30f), Y(0.44f) }, { X(0.70f), Y(0.56f) }, edge, rr);  // fente
                box(0.18f, 0.60f, 0.82f, 0.74f, rr);                                          // bord inferieur
                break;
            case Game::Chestplate:   // torse + epaules
                box(0.16f, 0.20f, 0.84f, 0.34f, rr);                 // epaules
                box(0.24f, 0.30f, 0.76f, 0.80f, s * 0.08f);          // torse
                dl->AddLine({ X(0.5f), Y(0.34f) }, { X(0.5f), Y(0.78f) }, edge, 1.2f * scale);
                break;
            case Game::Leggings:   // ceinture + deux jambes
                box(0.22f, 0.18f, 0.78f, 0.32f, rr);                 // ceinture
                box(0.24f, 0.30f, 0.46f, 0.82f, rr);                 // jambe gauche
                box(0.54f, 0.30f, 0.76f, 0.82f, rr);                 // jambe droite
                break;
            case Game::Boots:   // deux bottes (forme en L)
                box(0.20f, 0.30f, 0.44f, 0.64f, rr); box(0.20f, 0.60f, 0.52f, 0.78f, rr);
                box(0.56f, 0.30f, 0.80f, 0.64f, rr); box(0.56f, 0.60f, 0.88f, 0.78f, rr);
                break;
            case Game::Held: {  // epee generique
                dl->AddLine({ X(0.30f), Y(0.74f) }, { X(0.70f), Y(0.26f) }, hi, 4.f * scale);   // lame
                dl->AddLine({ X(0.28f), Y(0.66f) }, { X(0.40f), Y(0.78f) }, body, 5.f * scale);  // garde
                dl->AddCircleFilled({ X(0.26f), Y(0.76f) }, 2.4f * scale, edge);                 // pommeau
                break;
            }
            default: {          // generique : losange
                const ImVec2 c = { X(0.5f), Y(0.5f) }; const float d = s * 0.22f;
                dl->AddQuadFilled({ c.x, c.y - d }, { c.x + d, c.y }, { c.x, c.y + d }, { c.x - d, c.y }, hi);
                dl->AddRect({ X(0.2f), Y(0.2f) }, { X(0.8f), Y(0.8f) }, edge, rr, 0, 1.3f * scale);
                break;
            }
        }
        ImGui::Dummy({ size, size });
    }

    // Durabilite compacte : une pilule arrondie, remplissage anime, valeur integree.
    // ratio = remplissage (deja lisse) ; pulse 0..1 pour l'effet "bas" ; value optionnel.
    void DrawDuraPill(float ratio, ImVec4 color, float width, float height,
                      const char* value, bool low, float pulse) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float r  = height * 0.5f;
        const ImVec2 a = p, b = { p.x + width, p.y + height };

        // fond de la pilule
        dl->AddRectFilled(a, b, IM_COL32(0, 0, 0, 150), r);

        // remplissage anime (+ eclat pulsant quand la durabilite est basse)
        ImVec4 c = color;
        if (low) { const float k = 0.5f * pulse; c.x += (1.f - c.x) * k; c.y += (1.f - c.y) * k; c.z += (1.f - c.z) * k; }
        const float w = width * std::clamp(ratio, 0.f, 1.f);
        if (w > 1.f) {
            dl->AddRectFilled(a, { p.x + w, b.y }, ImGui::ColorConvertFloat4ToU32(c), r);
            // liseret clair en haut pour le relief
            dl->AddRectFilled({ a.x + r * 0.4f, a.y + height * 0.18f },
                              { p.x + w - r * 0.4f, a.y + height * 0.42f }, IM_COL32(255, 255, 255, 40), height * 0.2f);
        }

        // valeur integree a droite, dans la pilule : contour sombre pour rester
        // lisible sur n'importe quelle couleur de remplissage.
        if (value && value[0]) {
            const ImVec2 ts = ImGui::CalcTextSize(value);
            const ImVec2 tp = { b.x - ts.x - r * 0.9f, p.y + (height - ts.y) * 0.5f };
            const float o = (std::max)(1.f, height * 0.07f);
            const ImU32 outline = IM_COL32(0, 0, 0, 230);
            for (int dx = -1; dx <= 1; ++dx)
                for (int dy = -1; dy <= 1; ++dy)
                    if (dx || dy) dl->AddText({ tp.x + dx * o, tp.y + dy * o }, outline, value);
            dl->AddText(tp, IM_COL32(255, 255, 255, 255), value);
        }
        ImGui::Dummy({ width, height });
    }

    // Texte blanc a contour noir (lisible sur n'importe quel fond), avance le curseur.
    void OutlinedText(const char* txt) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float o = std::max(1.f, ImGui::GetFontSize() * 0.08f);
        const ImU32 outline = IM_COL32(0, 0, 0, 230);
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
                if (dx || dy) dl->AddText({ p.x + dx * o, p.y + dy * o }, outline, txt);
        dl->AddText(p, IM_COL32(255, 255, 255, 255), txt);
        ImGui::Dummy(ImGui::CalcTextSize(txt));
    }

    // Largeur totale d'une ligne de statistique (pour le centrage).
    float StatLineWidth(int value, const char* label, const char* secondary, float scale) {
        char num[16]; snprintf(num, sizeof(num), "%d", value);
        float w = ImGui::CalcTextSize(num).x + 4.f * scale + ImGui::CalcTextSize(label).x;
        if (secondary && secondary[0]) w += 8.f * scale + ImGui::CalcTextSize(secondary).x;
        return w;
    }

    // Affichage commun d'une statistique : grand nombre (accent si actif) + libelle,
    // + une valeur secondaire discrete optionnelle. Utilise par CPS et Combo (meme rendu).
    // centerWidth > 0 : centre la ligne dans cette largeur.
    void DrawStat(int value, bool active, const char* label, const char* secondary, float scale, float centerWidth = 0.f) {
        if (centerWidth > 0.f) {
            const float off = (centerWidth - StatLineWidth(value, label, secondary, scale)) * 0.5f;
            if (off > 0.f) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + off);
        }
        const ImVec4 a_ = Ui::AccentColor(); const float a[3] = { a_.x, a_.y, a_.z };
        ImGui::TextColored(active ? ImVec4(a[0], a[1], a[2], 1.f) : ImVec4(0.6f, 0.6f, 0.6f, 1.f), "%d", value);
        ImGui::SameLine(0.f, 4.f * scale);
        ImGui::TextUnformatted(label);
        if (secondary && secondary[0]) {
            ImGui::SameLine(0.f, 8.f * scale);
            ImGui::TextDisabled("%s", secondary);
        }
    }

    // ---- Systeme de position HUD partage (ancres 1..6, hotbar 7/8, position libre 0) --------
    // anchor : 0 libre, 1..6 coins/bords, 7 gauche hotbar, 8 droite hotbar.
    // margin : marge du bord (1..6) OU ecart horizontal a la hotbar (7/8).
    // hotbarRaise : hauteur depuis le bas pour les positions hotbar.
    // Place la fenetre suivante ; renvoie true si en position libre. L'apercu se met a jour en
    // continu pour les positions ancrees (on ne laisse "Appearing" qu'en position libre).
    // mx = decalage horizontal SIGNE (negatif = gauche, positif = droite) applique a la position
    // de base de l'ancre. my = marge verticale (ecart depuis le bord haut/bas).
    // Derniere taille connue de chaque fenetre de HUD (memorisee apres Begin). Sert a resoudre
    // le pivot et a borner la position AVANT Begin : une fenetre placee entierement hors ecran
    // peut faire court-circuiter Begin (il renvoie false), donc tout clamp post-Begin serait
    // ignore. On clampe donc en amont, ici.
    std::unordered_map<std::string, ImVec2> g_hudSize;

    // Borne un coin haut-gauche pour que la fenetre (taille sz) reste entierement a l'ecran.
    // 'pad' est une marge de securite appliquee UNIQUEMENT au bord extreme (droite/bas) : les
    // HUD a contenu variable (CPS, FPS...) grandissent vers la droite/bas quand un chiffre
    // s'ajoute, et comme on clampe avec la taille de l'image precedente, le fond deborderait
    // d'un cran le temps d'une image. Le pad absorbe cette croissance sans decaler les positions
    // normales (il n'agit que lorsque la fenetre est deja collee au bord).
    ImVec2 ClampTopLeft(ImVec2 tl, ImVec2 sz, ImVec2 disp, float pad) {
        const float maxX = (std::max)(0.f, disp.x - sz.x - pad);
        const float maxY = (std::max)(0.f, disp.y - sz.y - pad);
        tl.x = tl.x < 0.f ? 0.f : (tl.x > maxX ? maxX : tl.x);
        tl.y = tl.y < 0.f ? 0.f : (tl.y > maxY ? maxY : tl.y);
        return tl;
    }

    bool HudPlaceNext(const char* id, int anchor, float mx, float my, float x, float y, bool edit, float pad) {
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        const bool hotbar = (anchor == 7 || anchor == 8);
        const bool freePos = !hotbar && (anchor <= 0 || anchor > 8);
        const ImGuiCond cond = (edit && freePos) ? ImGuiCond_Appearing : ImGuiCond_Always;

        // Taille connue a l'image precedente (0 si la fenetre n'a pas encore ete dessinee).
        ImVec2 sz(0.f, 0.f);
        auto it = g_hudSize.find(id);
        if (it != g_hudSize.end()) sz = it->second;

        // On resout le pivot nous-memes -> coin haut-gauche, puis on borne a l'ecran.
        ImVec2 tl;
        if (hotbar) {
            const float half = 91.f / ItemRender::GuiScaleFrac();   // demi-largeur de la hotbar (px)
            const float cx = display.x * 0.5f, by = display.y - my;
            if (anchor == 7) tl = ImVec2(cx - half + mx - sz.x, by - sz.y);   // base droite (pivot 1,1)
            else             tl = ImVec2(cx + half + mx,        by - sz.y);   // base gauche (pivot 0,1)
        } else if (freePos) {
            tl = ImVec2(x, y);
        } else {
            const int a = anchor - 1, col = a % 3, row = a / 3;
            const float baseX = (col == 0) ? 0.f : (col == 1) ? display.x * 0.5f : display.x;
            const float ax = baseX + mx, ay = (row == 0) ? my : display.y - my;
            tl = ImVec2(ax - col * 0.5f * sz.x, ay - (row == 0 ? 0.f : sz.y));
        }
        tl = ClampTopLeft(tl, sz, display, pad);      // jamais hors ecran, quelles que soient les marges
        ImGui::SetNextWindowPos(tl, cond, ImVec2(0.f, 0.f));
        return freePos;
    }

    // Empeche la fenetre HUD de sortir de l'ecran : on borne sa position pour qu'elle reste
    // entierement visible, quelles que soient l'ancre et les marges choisies.
    void HudClampToScreen() {
        const ImVec2 disp = ImGui::GetIO().DisplaySize;
        const ImVec2 ws = ImGui::GetWindowSize();
        const ImVec2 wp = ImGui::GetWindowPos();
        const float maxX = (std::max)(0.f, disp.x - ws.x);
        const float maxY = (std::max)(0.f, disp.y - ws.y);
        const float nx = wp.x < 0.f ? 0.f : (wp.x > maxX ? maxX : wp.x);
        const float ny = wp.y < 0.f ? 0.f : (wp.y > maxY ? maxY : wp.y);
        if (nx != wp.x || ny != wp.y) ImGui::SetWindowPos(ImVec2(nx, ny));
    }

    // Glisser une fenetre de HUD en edition -> position libre, et memorise sa position.
    void HudDragToFree(int* anchor, float& x, float& y, bool edit) {
        if (!edit) return;
        if (ImGui::IsWindowHovered() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            g_dragging = true; if (anchor) *anchor = 0;
        }
        if (!anchor || *anchor == 0) { const ImVec2 p = ImGui::GetWindowPos(); x = p.x; y = p.y; }
    }

    // Positionne et ouvre un element de HUD (ancres + hotbar, aperçu live). Utilise par FPS, CPS, Zoom.
    bool BeginHudAnchored(const char* id, int& anchor, float mx, float my, float& x, float& y,
                          float scale, bool background, bool edit, float bgAlpha) {
        const bool freePos = HudPlaceNext(id, anchor, mx, my, x, y, edit, 10.f * scale);
        Settings& s = Config::GetSettings();
        // 'bgAlpha' est l'opacite (0..1) qui agit UNIQUEMENT sur le fond et la bordure : le texte
        // et les icones restent pleinement opaques (donc le HUD reste toujours visible/deplacable,
        // meme a 0 %). Aucun plancher en edition -> l'APERCU suit exactement le slider (apercu = jeu).
        const float opacity = bgAlpha;
        // Fond : discret et proportionnel a l'opacite pour les modules a fond. Pour les modules
        // SANS fond, un leger reperage en edition (independant de l'opacite) pour les positionner.
        const float bg = background ? (0.25f * opacity) : (edit ? 0.3f : 0.f);
        const float borderA = background ? (0.9f * opacity) : (edit ? 0.5f : 0.f);
        ImGui::SetNextWindowBgAlpha(bg);

        ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                 ImGuiWindowFlags_NoNav;
        if (!edit) flags |= ImGuiWindowFlags_NoInputs;
        if (!edit && !freePos) flags |= ImGuiWindowFlags_NoMove;   // en jeu : ancre fixe ; en edition : deplacable

        const float borderPx = background ? (std::max)(1.f, 1.5f * scale) : (edit ? 1.f : 0.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.f * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, borderPx);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 8.f * scale, 4.f * scale });
        ImGui::PushStyleColor(ImGuiCol_Border, Ui::AccentColor(borderA));
        const bool open = ImGui::Begin(id, nullptr, flags);
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(3);
        if (open) {
            ImGui::SetWindowFontScale(scale);
            g_hudSize[id] = ImGui::GetWindowSize();   // pour borner la position a l'image suivante
            HudClampToScreen();
            HudDragToFree(&anchor, x, y, edit);
        }
        return open;
    }

    bool BeginHudWindow(const char* id, float& x, float& y, bool edit, float bgAlpha,
                        bool showBorder = true, int* anchor = nullptr, float mx = 10.f, float my = 10.f,
                        float opacity = 1.f) {
        const bool freePos = HudPlaceNext(id, anchor ? *anchor : 0, mx, my, x, y, edit, 10.f * Config::GetSettings().scale);
        // L'opacite agit UNIQUEMENT sur le fond et la bordure (texte/icones restent pleins, donc
        // toujours visibles). Aucun plancher -> l'apercu suit exactement le slider.
        // bgAlpha = base du fond (0 en mode icone, ou l'overlay le dessine), multipliee par l'opacite.
        ImGui::SetNextWindowBgAlpha(bgAlpha * opacity);

        ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                 ImGuiWindowFlags_NoNav;
        if (!edit) flags |= ImGuiWindowFlags_NoInputs;
        if (!edit && !freePos) flags |= ImGuiWindowFlags_NoMove;

        const float scale = Config::GetSettings().scale * ResFactor();
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.f * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, showBorder ? 1.5f * scale : 0.f);
        const ImVec4 ac_ = Ui::AccentColor(); const float ac[3] = { ac_.x, ac_.y, ac_.z };
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(ac[0], ac[1], ac[2], 0.9f * opacity));
        const bool open = ImGui::Begin(id, nullptr, flags);
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);

        if (open) { g_hudSize[id] = ImGui::GetWindowSize(); HudClampToScreen(); HudDragToFree(anchor, x, y, edit); }
        return open;
    }

    // Termine une fenetre de HUD ouverte par BeginHud*. (Wrapper conserve pour uniformite.)
    void HudEnd() { ImGui::End(); }
}

bool Hud::IsDragging() { return g_dragging; }
void Hud::ResetDrag() { g_dragging = false; }   // appele en debut d'image

void Hud::RenderArmor(bool edit) {
    Settings& s = Config::GetSettings();
    // Echelle compensee par la resolution (taille constante quelle que soit la definition).
    const float armScale = Config::GetSettings().scale * ResFactor();
    ItemRender::ClearIcons();   // repart d'une file vide chaque image (avant tout retour)
    if (!s.armorEnabled) return;

    Game::Snapshot snap = Game::Read();
    bool preview = false;
    if (!snap.inGame) {
        if (!edit) return;          // dans les menus du jeu : rien a afficher
        snap = Game::Demo();        // menu ouvert hors partie : apercu pour placer le HUD
        preview = true;
    }

    // En jeu (hors edition/apercu/demo) : si aucune piece d'armure n'est portee (et pas
    // d'objet en main a afficher), on n'affiche pas du tout le panneau -> HUD plus propre.
    if (!preview && !snap.demo && !edit) {
        bool any = false;
        for (int k = 0; k < Game::SlotCount; ++k) {
            const Game::ItemInfo& itk = snap.items[k];
            if (k == Game::Held) { if (s.showHeldItem && itk.present) any = true; }
            else if (itk.present) any = true;
        }
        if (!any) return;
    }

    // En mode icone, les icones sont dessinees par le jeu SOUS l'overlay : le fond
    // est donc lui aussi dessine par le jeu (meme rendu en partie ET en apercu),
    // et la fenetre ImGui reste transparente.
    const bool gameBg = ItemRender::Active();   // panneau dessine par le jeu (icone ET texte)
    // Base du fond (constante, identique en edition et en jeu pour que l'apercu corresponde) ;
    // l'opacite (s.armorBgAlpha) la multiplie dans BeginHudWindow.
    const float winBg = gameBg ? 0.f : 0.25f;   // 0 en mode icone (le jeu dessine le fond)
    // Si le jeu dessine le fond+bordure, pas de bordure ImGui (evite la double bordure).
    if (!BeginHudWindow("##armor", s.armorX, s.armorY, edit, winBg, !gameBg, &s.armorAnchor, s.armorMargin, s.armorHotbarY, s.armorBgAlpha)) { HudEnd(); return; }
    ImGui::SetWindowFontScale(armScale);

    if (snap.demo || preview)
    {
        const ImVec4 ac_ = Ui::AccentColor(); const float ac[3] = { ac_.x, ac_.y, ac_.z };
        ImGui::TextColored(ImVec4(ac[0], ac[1], ac[2], 1.f), preview ? "Aperçu" : "DÉMO");
    }

    const float lineH0 = ImGui::GetTextLineHeight();
    const float barW   = 52.f * armScale;
    const float barH   = lineH0 + 4.f * armScale;        // pilule compacte (valeur integree)

    // Largeur de la colonne de gauche en mode texte = plus long nom affiche (aligne
    // les pilules et evite qu'elles chevauchent le texte).
    float labelW = 0.f;
    for (int k = 0; k < Game::SlotCount; ++k) {
        const Game::ItemInfo& itk = snap.items[k];
        if (k == Game::Held) { if (!s.showHeldItem || !itk.present) continue; }
        else if (!itk.present) continue;
        const char* lk = (itk.present && itk.name[0]) ? itk.name : kLabels[k];
        labelW = (std::max)(labelW, ImGui::CalcTextSize(lk).x);
    }
    labelW += 10.f * armScale;

    // Animations : remplissage lisse par piece + pulsation pour les pieces basses.
    const float dt    = ImGui::GetIO().DeltaTime;
    const float pulse = 0.5f + 0.5f * sinf((float)ImGui::GetTime() * 9.4248f);   // ~1.5 Hz
    static float animR[Game::SlotCount];
    static bool  animInit = false;
    if (!animInit) { for (int k = 0; k < Game::SlotCount; ++k) animR[k] = 0.f; animInit = true; }
    bool first = true;

    for (int i = 0; i < Game::SlotCount; ++i) {
        const Game::ItemInfo& it = snap.items[i];
        if (i == Game::Held) {
            if (!s.showHeldItem || !it.present) continue;   // tout objet en main, meme sans durabilite
        } else if (!it.present) {
            continue;                 // slot d'armure vide : jamais affiche
        }

        const float ratio = it.Ratio();
        const bool hasDura = it.present && it.maxDamage > 0;
        const bool low = hasDura && ratio * 100.f < float(s.lowThreshold);
        // remplissage lisse vers la valeur cible
        const float target = hasDura ? ratio : 0.f;
        animR[i] += (target - animR[i]) * (1.f - expf(-dt * 10.f));
        const ImVec4 color = it.present ? DurabilityColor(animR[i]) : ImVec4(0.6f, 0.6f, 0.6f, 1.f);

        // Libelle = nom reel de l'objet (ex. "Casque en diamant") sinon libelle generique.
        const char* lbl = (it.present && it.name[0]) ? it.name : kLabels[i];

        // Objet en main : on affiche TOUJOURS le compte (total du meme item dans l'inventaire),
        // meme s'il n'y en a qu'un. Pour l'armure, le compte ne s'affiche que s'il depasse 1.
        const bool held = (i == Game::Held);
        const bool showCount = it.count > 1 || (held && it.count >= 1);
        char value[48];       // texte de la pilule (durabilite) OU compteur seul si pas de dura
        char countStr[16];    // compteur affiche A COTE de la pilule quand il y a une durabilite
        countStr[0] = '\0';
        if (it.present && it.maxDamage > 0) {
            FormatValue(it, s.valueMode, value, sizeof(value));
            // Le compteur n'est PAS integre a la pilule (il deborderait et serait rogne par le
            // bord de la fenetre) : on le garde a part pour le dessiner a cote, dans la mise en page.
            if (showCount) snprintf(countStr, sizeof(countStr), "x%d", it.count);
        } else if (it.present && showCount) {
            snprintf(value, sizeof(value), "x%d", it.count);
        } else {
            value[0] = '\0';
        }

        // Mode icone : on reserve l'emplacement (Dummy) et on transmet la position
        // + le pointeur d'item a ItemRender, qui dessine la vraie texture via le jeu.
        // Sinon (ou apercu) : le nom en texte.
        const bool canIcon = s.armorDisplayMode == 1 && it.present && !preview && !snap.demo && ItemRender::Active();
        // Apercu (menu, pas de joueur) : pas d'ItemStack a rendre -> placeholder diamant.
        const bool previewIcon = s.armorDisplayMode == 1 && it.present && (preview || snap.demo);
        const float iconPx = 42.f * armScale;
        auto drawLabelOrIcon = [&]() {
            if (canIcon) {
                const ImVec2 p = ImGui::GetCursorScreenPos();
                void* raw = (i == Game::Held) ? Game::HeldStack() : Game::ArmorStack(i);
                ItemRender::QueueIcon(raw, p.x, p.y, iconPx);
                ImGui::Dummy(ImVec2(iconPx, iconPx));
            } else if (previewIcon) {
                DrawIconPlaceholder(iconPx, armScale, i);
            } else {
                ImGui::TextUnformatted(lbl);
            }
        };

        const float x0 = ImGui::GetCursorPosX();
        if (s.layout == 1) {                     // horizontal : une colonne par piece (elements centres)
            if (!first) ImGui::SameLine(0.f, 14.f * armScale);
            const bool  iconElem = canIcon || previewIcon;
            const float topW = iconElem ? iconPx : ImGui::CalcTextSize(lbl).x;
            const float countW = countStr[0] ? (4.f * armScale + ImGui::CalcTextSize(countStr).x) : 0.f;
            const float botW = hasDura ? (barW + countW) : (value[0] ? ImGui::CalcTextSize(value).x : 0.f);
            const float colW = (std::max)(topW, botW);
            const float cx0  = ImGui::GetCursorPosX();
            ImGui::BeginGroup();
            ImGui::SetCursorPosX(cx0 + (colW - topW) * 0.5f);      // element du haut centre
            drawLabelOrIcon();
            if (hasDura) {
                ImGui::SetCursorPosX(cx0 + (colW - botW) * 0.5f);  // pilule (+ compteur) centree sous le nom
                DrawDuraPill(animR[i], color, barW, barH, value, low, pulse);
                if (countStr[0]) {                                 // compteur a droite de la pilule, centre vertical
                    ImGui::SameLine(0.f, 4.f * armScale);
                    const float cy = ImGui::GetCursorPosY();
                    ImGui::SetCursorPosY(cy + (barH - ImGui::GetTextLineHeight()) * 0.5f);
                    OutlinedText(countStr);
                }
            } else if (value[0]) {
                ImGui::SetCursorPosX(cx0 + (colW - botW) * 0.5f);  // valeur centree
                OutlinedText(value);
            }
            ImGui::EndGroup();
        } else {                                 // vertical : une ligne par piece (icone/libelle a gauche, valeur a droite)
            const bool  iconElem = canIcon || previewIcon;               // element gauche = icone/placeholder
            const float elemH  = iconElem ? iconPx : lineH0;             // hauteur de l'element gauche
            const float rowH   = iconElem ? iconPx : (std::max)(lineH0, barH);
            const float y0     = ImGui::GetCursorPosY();

            // colonne gauche : icone ou libelle, centree verticalement dans la ligne
            ImGui::SetCursorPos(ImVec2(x0, y0 + (rowH - elemH) * 0.5f));
            drawLabelOrIcon();

            // colonne droite : pilule de durabilite (ou compteur), alignee pour toutes les
            // lignes (meme x) et centree verticalement sur la ligne.
            const float colW = iconElem ? (iconPx + 8.f * armScale)
                                        : (std::max)(labelW, iconPx + 8.f * armScale);
            // Centre optique de l'icone : le sprite MC occupe surtout le centre de sa boite,
            // on aligne donc la valeur sur ce centre (petit decalage pour les icones).
            const float centerY = y0 + rowH * 0.5f + (iconElem ? 1.5f * armScale : 0.f);
            if (hasDura) {
                ImGui::SetCursorPos(ImVec2(x0 + colW, centerY - barH * 0.5f));
                DrawDuraPill(animR[i], color, barW, barH, value, low, pulse);
                if (countStr[0]) {                                 // compteur a droite de la pilule
                    ImGui::SameLine(0.f, 4.f * armScale);
                    ImGui::SetCursorPosY(centerY - ImGui::GetTextLineHeight() * 0.5f);
                    OutlinedText(countStr);                        // blanc a bordure, jamais rogne
                }
            } else if (value[0]) {
                ImGui::SetCursorPos(ImVec2(x0 + colW, centerY - lineH0 * 0.5f));
                OutlinedText(value);                               // nombre d'item : blanc a bordure
            }

            // ligne suivante (espacement compact)
            ImGui::SetCursorPos(ImVec2(x0, y0 + rowH + 4.f * armScale));
        }
        first = false;
    }

    if (first && (preview || snap.demo || edit)) ImGui::TextDisabled("Aucune armure");

    // Fond dessine par le jeu, sous les icones/placeholders : identique en partie
    // et en apercu (meme teinte, meme taille).
    if (gameBg) {
        const ImVec2 wp = ImGui::GetWindowPos();
        const ImVec2 ws = ImGui::GetWindowSize();
        const float pad = 6.f * armScale;
        const ImVec4 bgc = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);  // teinte du HUD FPS (violet tres sombre)
        const ImVec4 ac_ = Ui::AccentColor(); const float ac[3] = { ac_.x, ac_.y, ac_.z };                                       // bordure violette (accent)
        const float rad = 6.f * armScale;                                 // coins arrondis
        // Fond dessine par le jeu -> non affecte par style.Alpha : on attenue manuellement par
        // l'opacite globale. Fond discret (0.25) + bordure, le tout multiplie par s.armorBgAlpha.
        const float op = s.armorBgAlpha;        // pas de plancher : l'apercu suit le slider
        ItemRender::QueueBackground(wp.x - pad, wp.y - pad, ws.x + 2.f * pad, ws.y + 2.f * pad, rad,
                                    bgc.x, bgc.y, bgc.z, 0.25f * op,        // remplissage discret, attenue
                                    ac[0], ac[1], ac[2], 0.9f * op, 1.5f * armScale);  // bordure attenuee
    }
    HudEnd();
}

void Hud::RenderFps(bool edit) {
    Settings& s = Config::GetSettings();
    if (!s.fpsEnabled) return;
    const float fpsScale = Config::GetSettings().fpsScale * ResFactor();  // echelle compensee resolution

    // Valeur rafraichie a intervalle regulier (plus lisible qu'un chiffre qui change a chaque image)
    static float shownFps = 0.f;
    static double lastUpdate = -1.0;
    const double now = ImGui::GetTime();
    if (lastUpdate < 0.0 || now - lastUpdate >= std::max(0.05f, s.fpsInterval)) {
        shownFps = ImGui::GetIO().Framerate;
        lastUpdate = now;
    }

    // Position unifiee (ancres + hotbar + ecart + hauteur + apercu live), comme l'armure.
    if (!BeginHudAnchored("##fps", s.fpsAnchor, s.fpsMargin, s.fpsHotbarY, s.fpsX, s.fpsY, fpsScale,
                          s.fpsBackground, edit, s.fpsBgAlpha)) { HudEnd(); return; }

    // Couleur
    ImVec4 valueColor(1.f, 1.f, 1.f, 1.f);
    const ImVec4 accent = Ui::AccentColor();
    if (s.fpsColorMode == 0) valueColor = accent;
    else if (s.fpsColorMode == 1) {
        const float target = float(std::max(1, s.fpsTarget));
        valueColor = shownFps >= target ? ImVec4(0.35f, 0.90f, 0.40f, 1.f)
                   : shownFps >= target * 0.5f ? ImVec4(1.f, 0.70f, 0.15f, 1.f)
                   : ImVec4(0.95f, 0.30f, 0.30f, 1.f);
    }
    const ImVec4 labelColor = s.fpsColorMode == 0 ? ImVec4(1.f, 1.f, 1.f, 0.85f) : ImVec4(1.f, 1.f, 1.f, 0.6f);

    ImGui::TextColored(valueColor, "%.0f", shownFps);
    if (s.fpsShowLabel) {
        ImGui::SameLine(0.f, 4.f * fpsScale);
        ImGui::TextColored(labelColor, "FPS");
    }
    if (s.fpsShowMs) {
        ImGui::SameLine(0.f, 8.f * fpsScale);
        ImGui::TextColored(ImVec4(1.f, 1.f, 1.f, 0.5f), "%.1f ms", shownFps > 0.f ? 1000.f / shownFps : 0.f);
    }
    HudEnd();
}

// CPS et Combo reunis dans UN seul panneau (meme position, meme taille, meme style).
void Hud::RenderCpsCombo(bool edit) {
    Settings& s = Config::GetSettings();
    const float cpsScale = Config::GetSettings().cpsScale * ResFactor();  // echelle compensee resolution
    if (!s.cpsComboEnabled) return;              // interrupteur maitre du module
    if (!s.cpsEnabled && !s.comboEnabled && !s.reachEnabled && !s.reachTakenEnabled) return;
    if (!edit && !Game::LocalPlayer()) return;   // seulement en jeu (hors edition)
    if (!BeginHudAnchored("##cpscombo", s.cpsAnchor, s.cpsMargin, s.cpsHotbarY, s.cpsX, s.cpsY, cpsScale, true, edit, s.cpsBgAlpha)) { HudEnd(); return; }

    // Prepare les deux lignes pour pouvoir les centrer sur la plus large.
    char cpsSec[24] = "";
    if (s.cpsEnabled && s.cpsShowRight) snprintf(cpsSec, sizeof(cpsSec), "| %d D", Stats::RightCps());
    const int combo = edit && Stats::Combo() == 0 ? 3 : Stats::Combo();   // apercu en edition
    char cboSec[24] = "";
    if (s.comboEnabled && s.comboShowMax) snprintf(cboSec, sizeof(cboSec), "(max %d)", Stats::MaxCombo());

    // Valeurs de reach (avec apercu en edition).
    const float reachV  = (edit && Stats::Reach() == 0.f) ? 3.14f : Stats::Reach();
    const float reachTV = (edit && Stats::ReachTaken() == 0.f) ? 3.14f : Stats::ReachTaken();
    auto reachLineW = [&](float v, const char* label) {
        char num[16]; snprintf(num, sizeof(num), "%.2f", v);
        return ImGui::CalcTextSize(num).x + 4.f * cpsScale + ImGui::CalcTextSize(label).x;
    };

    // Largeur cible = la plus large des lignes affichees (toutes centrees dessus).
    float targetW = 0.f;
    if (s.cpsEnabled)        targetW = (std::max)(targetW, StatLineWidth(Stats::LeftCps(), "CPS", cpsSec, cpsScale));
    if (s.comboEnabled)      targetW = (std::max)(targetW, StatLineWidth(combo, combo > 1 ? "combo" : "coup", cboSec, cpsScale));
    if (s.reachEnabled)      targetW = (std::max)(targetW, reachLineW(reachV, "blocs"));
    if (s.reachTakenEnabled) targetW = (std::max)(targetW, reachLineW(reachTV, "blocs reçus"));

    if (s.cpsEnabled)
        DrawStat(Stats::LeftCps(), true, "CPS", cpsSec, cpsScale, targetW);
    if (s.comboEnabled)
        DrawStat(combo, true, combo > 1 ? "combo" : "coup", cboSec, cpsScale, targetW);
    auto drawReach = [&](float v, const char* label) {
        const float off = (targetW - reachLineW(v, label)) * 0.5f;     // centrage sur targetW
        if (off > 0.f) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + off);
        const ImVec4 a_ = Ui::AccentColor(); const float a[3] = { a_.x, a_.y, a_.z };
        ImGui::TextColored(ImVec4(a[0], a[1], a[2], 1.f), "%.2f", v);   // nombre en accent (comme le CPS)
        ImGui::SameLine(0.f, 4.f * cpsScale);
        ImGui::TextColored(ImVec4(1.f, 1.f, 1.f, 1.f), "%s", label);    // libelle en blanc
    };
    if (s.reachEnabled)      drawReach(reachV, "blocs");
    if (s.reachTakenEnabled) drawReach(reachTV, "blocs reçus");
    HudEnd();
}

// Indicateur de zoom : element HUD au style unifie, deplacable comme le FPS.
// En jeu : visible uniquement pendant le zoom. Menu ouvert : apercu pour le placer.
void Hud::RenderZoom(bool edit) {
    Settings& s = Config::GetSettings();
    const float zoomHudScale = Config::GetSettings().zoomHudScale * ResFactor();  // echelle compensee resolution
    if (!s.zoomIndicator) return;
    const bool active = Zoom::CurrentFactor() > 1.05f;
    if (!edit && !active) return;
    if (!BeginHudAnchored("##zoomind", s.zoomAnchor, s.zoomHudMargin, s.zoomHotbarY, s.zoomHudX, s.zoomHudY,
                          zoomHudScale, true, edit, s.zoomBgAlpha)) { HudEnd(); return; }

    const float f = active ? Zoom::CurrentFactor() : std::max(1.f, s.zoomFactor);
    const ImVec4 ac_ = Ui::AccentColor(); const float ac[3] = { ac_.x, ac_.y, ac_.z };
    ImGui::TextColored(ImVec4(ac[0], ac[1], ac[2], 1.f), "ZOOM");
    ImGui::SameLine(0.f, 6.f * zoomHudScale);
    ImGui::Text("x%.1f", f);
    HudEnd();
}

// Message d'accueil : toast affiche quelques secondes a l'arrivee dans un monde/serveur
// (transition "pas de joueur local" -> "joueur local present"). Explique comment ouvrir le
// menu et rappelle la version. Dessine par-dessus tout, independamment du HUD.
void Hud::RenderWelcome() {
    const Settings& s = Config::GetSettings();
    // On suit l'IDENTITE du joueur (adresse du LocalPlayer, stable tant qu'on reste dans le
    // meme monde/serveur). LocalPlayer() peut renvoyer nullptr par intermittence pendant les
    // chargements : en comparant le pointeur, on n'affiche qu'une fois par monde, sans spam.
    static void*  shownFor = nullptr;  // joueur pour lequel le message a deja ete declenche
    static double showAt = -1.0;

    void* lp = Game::LocalPlayer();
    if (lp && lp != shownFor) {         // nouveau LocalPlayer -> nouveau monde/serveur
        shownFor = lp;
        if (s.welcomeMessage) showAt = ImGui::GetTime();
    }
    if (showAt < 0.0) return;

    const double dur = 6.0;
    const double t = ImGui::GetTime() - showAt;
    if (t > dur) { showAt = -1.0; return; }
    float alpha = 1.f;                                                 // fondu entree/sortie
    if (t < 0.3)            alpha = (float)(t / 0.3);
    else if (t > dur - 0.7) alpha = (float)((dur - t) / 0.7);
    alpha = alpha < 0.f ? 0.f : (alpha > 1.f ? 1.f : alpha);

    char line1[160];
    std::snprintf(line1, sizeof(line1), "Bienvenue sur Nexus v%s", Credits::kVersion);
    const char* line2 = "Appuie sur INSER pour ouvrir le menu";

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    ImFont* tf = Ui::TitleFont;
    const ImVec2 s1 = tf ? tf->CalcTextSizeA(tf->FontSize, FLT_MAX, 0.f, line1)
                         : ImGui::CalcTextSize(line1);
    const ImVec2 s2 = ImGui::CalcTextSize(line2);

    const float pad = 16.f, gap = 6.f;
    const float boxW = (std::max)(s1.x, s2.x) + pad * 2.f;
    const float boxH = s1.y + gap + s2.y + pad * 2.f;
    const float cx = disp.x * 0.5f;
    const float top = disp.y * 0.11f;
    const ImVec2 bmin(cx - boxW * 0.5f, top), bmax(cx + boxW * 0.5f, top + boxH);

    const ImVec4 ac = Ui::AccentColor(alpha);
    const ImU32  acU = ImGui::ColorConvertFloat4ToU32(ac);
    dl->AddRectFilled(bmin, bmax, IM_COL32(14, 12, 22, (int)(225 * alpha)), 10.f);
    dl->AddRect(bmin, bmax, acU, 10.f, 0, 2.f);

    float ty = top + pad;
    if (tf) dl->AddText(tf, tf->FontSize, ImVec2(cx - s1.x * 0.5f, ty), acU, line1);
    else    dl->AddText(ImVec2(cx - s1.x * 0.5f, ty), acU, line1);
    ty += s1.y + gap;
    dl->AddText(ImVec2(cx - s2.x * 0.5f, ty), IM_COL32(255, 255, 255, (int)(255 * alpha)), line2);
}
