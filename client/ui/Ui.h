#pragma once

struct ImFont;

namespace Ui {
    void Render();          // appele chaque image entre ImGui::NewFrame et ImGui::Render
    void ToggleMenu();
    bool IsMenuOpen();
    inline ImFont* TitleFont = nullptr;   // Segoe UI Semibold, charge par RenderHook
}

namespace Hud {
    void RenderArmor(bool editMode);
    void RenderFps(bool editMode);
}

namespace Menu {
    void Render();
}
