#include "Ui.h"
#include "core/Config.h"
#include "game/Game.h"
#include <windows.h>
#include <imgui.h>
#include <cstdio>
#include <algorithm>

namespace {
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

    bool BeginHudWindow(const char* id, float& x, float& y, bool edit, float bgAlpha) {
        if (!edit) ImGui::SetNextWindowPos({ x, y }, ImGuiCond_Always);
        else       ImGui::SetNextWindowPos({ x, y }, ImGuiCond_Appearing);
        ImGui::SetNextWindowBgAlpha(edit ? std::max(bgAlpha, 0.55f) : bgAlpha);

        ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                 ImGuiWindowFlags_NoNav;
        if (!edit) flags |= ImGuiWindowFlags_NoInputs;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, edit ? 1.f : 0.f);
        const auto& ac = Config::GetSettings().accent;
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(ac[0], ac[1], ac[2], 0.9f));
        const bool open = ImGui::Begin(id, nullptr, flags);
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();

        if (edit) { const ImVec2 pos = ImGui::GetWindowPos(); x = pos.x; y = pos.y; }
        return open;
    }
}

void Hud::RenderArmor(bool edit) {
    Settings& s = Config::GetSettings();
    if (!s.armorEnabled) return;

    Game::Snapshot snap = Game::Read();
    bool preview = false;
    if (!snap.inGame) {
        if (!edit) return;          // dans les menus du jeu : rien a afficher
        snap = Game::Demo();        // menu ouvert hors partie : apercu pour placer le HUD
        preview = true;
    }

    if (!BeginHudWindow("##armor", s.armorX, s.armorY, edit, s.bgAlpha)) { ImGui::End(); return; }
    ImGui::SetWindowFontScale(s.scale);

    if (snap.demo || preview)
    {
        const auto& ac = s.accent;
        ImGui::TextColored(ImVec4(ac[0], ac[1], ac[2], 1.f), preview ? "Aperçu" : "DÉMO");
    }

    const float barW = 56.f * s.scale, barH = 5.f * s.scale;
    const float labelW = ImGui::CalcTextSize("Jambières").x + 8.f * s.scale;
    const bool blinkOn = (GetTickCount64() / 400) % 2 == 0;
    bool first = true;

    for (int i = 0; i < Game::SlotCount; ++i) {
        const Game::ItemInfo& it = snap.items[i];
        if (i == Game::Held) {
            if (!s.showHeldItem || !it.present || it.maxDamage <= 0) continue;
        } else if (!it.present && !s.showEmptySlots) {
            continue;
        }

        const float ratio = it.Ratio();
        const bool low = it.present && it.maxDamage > 0 && ratio * 100.f < float(s.lowThreshold);
        ImVec4 color = it.present ? DurabilityColor(ratio) : ImVec4(0.6f, 0.6f, 0.6f, 1.f);
        if (low && blinkOn) color = ImVec4(1.f, 1.f, 1.f, 1.f);

        char value[48];
        FormatValue(it, s.valueMode, value, sizeof(value));

        if (s.layout == 1) {                     // horizontal : une colonne par piece
            if (!first) ImGui::SameLine(0.f, 14.f * s.scale);
            ImGui::BeginGroup();
            ImGui::TextUnformatted(kLabels[i]);
            DrawBar(it.maxDamage > 0 ? ratio : 0.f, color, barW, barH);
            ImGui::TextColored(color, "%s", value);
            ImGui::EndGroup();
        } else {                                 // vertical : une ligne par piece
            const float x0 = ImGui::GetCursorPosX();
            ImGui::TextUnformatted(kLabels[i]);
            ImGui::SameLine(x0 + labelW);
            DrawBar(it.maxDamage > 0 ? ratio : 0.f, color, barW, barH);
            ImGui::SameLine();
            ImGui::TextColored(color, "%s", value);
        }
        first = false;
    }

    if (first) ImGui::TextDisabled("Aucune armure");
    ImGui::End();
}

void Hud::RenderFps(bool edit) {
    Settings& s = Config::GetSettings();
    if (!s.fpsEnabled) return;

    // Valeur rafraichie a intervalle regulier (plus lisible qu'un chiffre qui change a chaque image)
    static float shownFps = 0.f;
    static double lastUpdate = -1.0;
    const double now = ImGui::GetTime();
    if (lastUpdate < 0.0 || now - lastUpdate >= std::max(0.05f, s.fpsInterval)) {
        shownFps = ImGui::GetIO().Framerate;
        lastUpdate = now;
    }

    // Position : coin / bord predefini, ou position libre (deplacable menu ouvert)
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const bool freePos = s.fpsAnchor <= 0 || s.fpsAnchor > 6;
    if (freePos) {
        if (!edit) ImGui::SetNextWindowPos({ s.fpsX, s.fpsY }, ImGuiCond_Always);
        else       ImGui::SetNextWindowPos({ s.fpsX, s.fpsY }, ImGuiCond_Appearing);
    } else {
        const int a = s.fpsAnchor - 1;               // 0..5
        const int col = a % 3, row = a / 3;          // colonne : gauche/centre/droite ; ligne : haut/bas
        const float m = s.fpsMargin;
        const ImVec2 pos(col == 0 ? m : col == 1 ? display.x * 0.5f : display.x - m,
                         row == 0 ? m : display.y - m);
        const ImVec2 pivot(col * 0.5f, row == 0 ? 0.f : 1.f);
        ImGui::SetNextWindowPos(pos, ImGuiCond_Always, pivot);
    }
    ImGui::SetNextWindowBgAlpha(s.fpsBackground ? (edit ? std::max(s.bgAlpha, 0.55f) : s.bgAlpha) : (edit ? 0.3f : 0.f));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoNav;
    if (!edit) flags |= ImGuiWindowFlags_NoInputs;
    if (!freePos) flags |= ImGuiWindowFlags_NoMove;   // position predefinie : fixe

    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, edit ? 1.f : 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 8.f * s.fpsScale, 4.f * s.fpsScale });
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(s.accent[0], s.accent[1], s.accent[2], 0.9f));
    const bool open = ImGui::Begin("##fps", nullptr, flags);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    if (!open) { ImGui::End(); return; }

    if (edit && freePos) { const ImVec2 p = ImGui::GetWindowPos(); s.fpsX = p.x; s.fpsY = p.y; }
    ImGui::SetWindowFontScale(s.fpsScale);

    // Couleur
    ImVec4 valueColor(1.f, 1.f, 1.f, 1.f);
    const ImVec4 accent(s.accent[0], s.accent[1], s.accent[2], 1.f);
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
        ImGui::SameLine(0.f, 4.f * s.fpsScale);
        ImGui::TextColored(labelColor, "FPS");
    }
    if (s.fpsShowMs) {
        ImGui::SameLine(0.f, 8.f * s.fpsScale);
        ImGui::TextColored(ImVec4(1.f, 1.f, 1.f, 0.5f), "%.1f ms", shownFps > 0.f ? 1000.f / shownFps : 0.f);
    }
    ImGui::End();
}
