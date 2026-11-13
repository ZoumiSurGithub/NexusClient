#pragma once

// Hook de IDXGISwapChain::Present (+ Present1, ResizeBuffers) et de
// ID3D12CommandQueue::ExecuteCommandLists. Gere DirectX 12 (cas normal de
// Bedrock) et DirectX 11 (repli). Dessine ImGui par-dessus l'image du jeu.
namespace RenderHook {
    bool Init();                // MinHook doit deja etre initialise
    void RequestShutdown();     // le nettoyage se fait sur le thread de rendu
    bool IsShutdownComplete();
}
