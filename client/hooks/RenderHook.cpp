#include "RenderHook.h"
#include "InputHook.h"
#include "FocusGrab.h"
#include "core/Logger.h"
#include "core/Config.h"
#include "modules/Screenshot.h"
#include "ui/Ui.h"

#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <MinHook.h>
#include <imgui.h>
#include <cstring>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_dx12.h>
#include <atomic>
#include <vector>

namespace {
    using PresentFn       = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
    using Present1Fn      = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
    using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
    using ExecuteFn       = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
    using SetFullscreenFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, BOOL, IDXGIOutput*);

    PresentFn       o_Present = nullptr;
    Present1Fn      o_Present1 = nullptr;
    ResizeBuffersFn o_ResizeBuffers = nullptr;
    ExecuteFn       o_Execute = nullptr;
    SetFullscreenFn o_SetFullscreen = nullptr;
    void* t_Present = nullptr; void* t_Present1 = nullptr; void* t_Resize = nullptr; void* t_Execute = nullptr;
    void* t_SetFullscreen = nullptr;

    // Pendant une transition du swapchain (deplacement de fenetre, redimensionnement, bascule
    // plein ecran), DXGI/D3D12 reconfigurent les ressources de maniere asynchrone : y soumettre
    // notre rendu provoque un arret brutal (fastfail) non interceptable. On arme ce "cooldown"
    // au debut de chaque transition et l'overlay ne touche plus au swapchain jusqu'a son terme.
    std::atomic<ULONGLONG> g_transitionUntil{ 0 };
    inline void ArmTransition(ULONGLONG ms) {
        const ULONGLONG until = GetTickCount64() + ms;
        ULONGLONG cur = g_transitionUntil.load();
        while (until > cur && !g_transitionUntil.compare_exchange_weak(cur, until)) {}
    }

    enum class Api { None, DX11, DX12 };
    Api  g_api = Api::None;
    bool g_ready = false;
    bool g_initFailed = false;
    HWND g_hwnd = nullptr;
    std::atomic<bool> g_shutdownRequested{ false };
    std::atomic<bool> g_shutdownComplete{ false };

    // ---- DirectX 11 ---------------------------------------------------------
    ID3D11Device*           g_d11Device = nullptr;
    ID3D11DeviceContext*    g_d11Context = nullptr;
    ID3D11RenderTargetView* g_d11Rtv = nullptr;

    // ---- DirectX 12 ---------------------------------------------------------
    struct Frame {
        ID3D12CommandAllocator*     allocator = nullptr;
        ID3D12Resource*             backBuffer = nullptr;
        D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
        UINT64                      fenceValue = 0;
    };
    constexpr UINT kMaxBuffers = 16;
    constexpr UINT kSrvCount = 64;

    ID3D12Device*              g_d12Device = nullptr;
    std::atomic<ID3D12CommandQueue*> g_d12Queue{ nullptr };
    ID3D12DescriptorHeap*      g_rtvHeap = nullptr;
    ID3D12DescriptorHeap*      g_srvHeap = nullptr;
    ID3D12GraphicsCommandList* g_cmdList = nullptr;
    ID3D12Fence*               g_fence = nullptr;
    HANDLE                     g_fenceEvent = nullptr;
    UINT64                     g_fenceCounter = 0;
    std::vector<Frame>         g_frames;
    DXGI_FORMAT                g_rtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

    // Allocateur de descripteurs SRV pour ImGui (police + textures eventuelles)
    std::vector<UINT> g_srvFree;
    void SrvAlloc(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu) {
        const UINT inc = g_d12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        const UINT idx = g_srvFree.back(); g_srvFree.pop_back();
        cpu->ptr = g_srvHeap->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T(idx) * inc;
        gpu->ptr = g_srvHeap->GetGPUDescriptorHandleForHeapStart().ptr + UINT64(idx) * inc;
    }
    void SrvFree(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE) {
        const UINT inc = g_d12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        g_srvFree.push_back(UINT((cpu.ptr - g_srvHeap->GetCPUDescriptorHandleForHeapStart().ptr) / inc));
    }

    template <class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

    void WaitGpuIdle() {
        auto queue = g_d12Queue.load();
        if (!queue || !g_fence) return;
        const UINT64 v = ++g_fenceCounter;
        if (SUCCEEDED(queue->Signal(g_fence, v)) && g_fence->GetCompletedValue() < v) {
            g_fence->SetEventOnCompletion(v, g_fenceEvent);
            WaitForSingleObject(g_fenceEvent, 1000);
        }
    }

    bool CreateDx12Frames(IDXGISwapChain* sc) {
        DXGI_SWAP_CHAIN_DESC desc{};
        sc->GetDesc(&desc);
        const UINT count = desc.BufferCount;
        if (count == 0 || count > kMaxBuffers) return false;

        const UINT rtvInc = g_d12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        auto rtvStart = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();

        g_frames.resize(count);
        for (UINT i = 0; i < count; ++i) {
            Frame& f = g_frames[i];
            if (!f.allocator &&
                FAILED(g_d12Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.allocator))))
                return false;
            // Pas de GetBuffer ici : le back buffer et sa vue (RTV) sont relus a CHAQUE image
            // dans RenderDx12 (robuste aux reconfigurations du swapchain). On ne reserve que
            // l'emplacement de descripteur RTV.
            f.rtv.ptr = rtvStart.ptr + SIZE_T(i) * rtvInc;
        }
        return true;
    }

    void ReleaseDx12BackBuffers() {
        for (Frame& f : g_frames) SafeRelease(f.backBuffer);
    }

    bool InitDx12(IDXGISwapChain* sc) {
        if (!g_d12Queue.load()) return false; // on attend d'avoir capture la file de commandes

        DXGI_SWAP_CHAIN_DESC desc{};
        sc->GetDesc(&desc);
        g_rtvFormat = desc.BufferDesc.Format;

        D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{ D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kMaxBuffers, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
        D3D12_DESCRIPTOR_HEAP_DESC srvDesc{ D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSrvCount, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
        if (FAILED(g_d12Device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&g_rtvHeap))) ||
            FAILED(g_d12Device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&g_srvHeap))))
            return false;
        for (UINT i = kSrvCount; i > 0; --i) g_srvFree.push_back(i - 1);

        if (!CreateDx12Frames(sc)) return false;

        if (FAILED(g_d12Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_frames[0].allocator, nullptr,
                                                  IID_PPV_ARGS(&g_cmdList))))
            return false;
        g_cmdList->Close();

        if (FAILED(g_d12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)))) return false;
        g_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

        ImGui_ImplDX12_InitInfo info;
        info.Device = g_d12Device;
        info.CommandQueue = g_d12Queue.load();
        info.NumFramesInFlight = static_cast<int>(g_frames.size());
        info.RTVFormat = g_rtvFormat;
        info.DSVFormat = DXGI_FORMAT_UNKNOWN;
        info.SrvDescriptorHeap = g_srvHeap;
        info.SrvDescriptorAllocFn = SrvAlloc;
        info.SrvDescriptorFreeFn = SrvFree;
        return ImGui_ImplDX12_Init(&info);
    }

    void RenderDx12(IDXGISwapChain* sc) {
        IDXGISwapChain3* sc3 = nullptr;
        if (FAILED(sc->QueryInterface(IID_PPV_ARGS(&sc3)))) return;
        const UINT idx = sc3->GetCurrentBackBufferIndex();
        sc3->Release();
        if (idx >= g_frames.size()) return;
        Frame& f = g_frames[idx];

        // Back buffer FRAIS a chaque image, directement depuis le swapchain : on ne garde
        // jamais de pointeur en cache (DXGI peut remplacer les buffers sans ResizeBuffers,
        // ex. deplacement de la fenetre sur un autre moniteur -> sinon on dessine sur de la
        // memoire liberee = crash). On recree aussi la vue (RTV) sur ce buffer courant.
        ID3D12Resource* back = nullptr;
        if (FAILED(sc->GetBuffer(idx, IID_PPV_ARGS(&back))) || !back) return;
        g_d12Device->CreateRenderTargetView(back, nullptr, f.rtv);

        if (g_fence->GetCompletedValue() < f.fenceValue) {
            g_fence->SetEventOnCompletion(f.fenceValue, g_fenceEvent);
            WaitForSingleObject(g_fenceEvent, 100);
        }

        f.allocator->Reset();
        g_cmdList->Reset(f.allocator, nullptr);

        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = back;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        g_cmdList->ResourceBarrier(1, &barrier);

        g_cmdList->OMSetRenderTargets(1, &f.rtv, FALSE, nullptr);
        g_cmdList->SetDescriptorHeaps(1, &g_srvHeap);
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_cmdList);

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        g_cmdList->ResourceBarrier(1, &barrier);
        g_cmdList->Close();

        auto queue = g_d12Queue.load();
        ID3D12CommandList* lists[] = { g_cmdList };
        o_Execute(queue, 1, lists); // appel direct a l'original (pas de recursion dans notre hook)
        f.fenceValue = ++g_fenceCounter;
        queue->Signal(g_fence, f.fenceValue);
        back->Release();   // le swapchain garde sa propre reference : on relache la notre
    }

    void RenderDx11(IDXGISwapChain* sc) {
        if (!g_d11Rtv) {
            ID3D11Texture2D* back = nullptr;
            if (FAILED(sc->GetBuffer(0, IID_PPV_ARGS(&back)))) return;
            g_d11Device->CreateRenderTargetView(back, nullptr, &g_d11Rtv);
            back->Release();
            if (!g_d11Rtv) return;
        }
        g_d11Context->OMSetRenderTargets(1, &g_d11Rtv, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }

    // Capture du back buffer (image propre du jeu, sans notre overlay) -> module Screenshot.
    void CaptureDx11(IDXGISwapChain* sc) {
        if (!g_d11Device || !g_d11Context) { LOG_WARN("Capture : device/context DX11 absent."); return; }
        ID3D11Texture2D* back = nullptr;
        HRESULT hr = sc->GetBuffer(0, IID_PPV_ARGS(&back));
        if (FAILED(hr) || !back) { LOG_WARN("Capture : GetBuffer a echoue (0x%08lX).", hr); return; }
        D3D11_TEXTURE2D_DESC d{}; back->GetDesc(&d);

        // Si le back buffer est multi-echantillonne (rare pour un swapchain), on le resout d'abord.
        ID3D11Texture2D* srcTex = back;
        ID3D11Texture2D* resolved = nullptr;
        if (d.SampleDesc.Count > 1) {
            D3D11_TEXTURE2D_DESC rd = d; rd.SampleDesc.Count = 1; rd.SampleDesc.Quality = 0;
            rd.Usage = D3D11_USAGE_DEFAULT; rd.BindFlags = 0; rd.CPUAccessFlags = 0; rd.MiscFlags = 0;
            if (SUCCEEDED(g_d11Device->CreateTexture2D(&rd, nullptr, &resolved)) && resolved) {
                g_d11Context->ResolveSubresource(resolved, 0, back, 0, d.Format);
                srcTex = resolved;
            }
        }

        D3D11_TEXTURE2D_DESC sd = d;
        sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.MiscFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.ArraySize = 1; sd.MipLevels = 1;
        sd.SampleDesc.Count = 1; sd.SampleDesc.Quality = 0;
        ID3D11Texture2D* stag = nullptr;
        hr = g_d11Device->CreateTexture2D(&sd, nullptr, &stag);
        if (SUCCEEDED(hr) && stag) {
            g_d11Context->CopyResource(stag, srcTex);
            D3D11_MAPPED_SUBRESOURCE m{};
            hr = g_d11Context->Map(stag, 0, D3D11_MAP_READ, 0, &m);
            if (SUCCEEDED(hr)) {
                Screenshot::Save(d.Width, d.Height, (uint32_t)d.Format, m.pData, m.RowPitch);
                g_d11Context->Unmap(stag, 0);
                LOG_INFO("Capture : back buffer lu %ux%u (format %d).", d.Width, d.Height, (int)d.Format);
            } else LOG_WARN("Capture : Map a echoue (0x%08lX).", hr);
            stag->Release();
        } else LOG_WARN("Capture : CreateTexture2D staging a echoue (0x%08lX, format %d, %ux%u).", hr, (int)d.Format, d.Width, d.Height);

        if (resolved) resolved->Release();
        back->Release();
    }

    // Capture du back buffer en DirectX 12 : copie vers un buffer "readback" via une command list,
    // puis lecture CPU. Appele avant l'overlay -> image propre du jeu.
    void CaptureDx12(IDXGISwapChain* sc) {
        auto queue = g_d12Queue.load();
        if (!g_d12Device || !queue || !g_cmdList || !g_fence) { LOG_WARN("Capture DX12 : infrastructure absente."); return; }
        IDXGISwapChain3* sc3 = nullptr;
        if (FAILED(sc->QueryInterface(IID_PPV_ARGS(&sc3))) || !sc3) { LOG_WARN("Capture DX12 : SwapChain3 indisponible."); return; }
        const UINT idx = sc3->GetCurrentBackBufferIndex();
        sc3->Release();
        if (idx >= g_frames.size()) return;
        Frame& f = g_frames[idx];

        ID3D12Resource* back = nullptr;
        if (FAILED(sc->GetBuffer(idx, IID_PPV_ARGS(&back))) || !back) { LOG_WARN("Capture DX12 : GetBuffer a echoue."); return; }
        const D3D12_RESOURCE_DESC rd = back->GetDesc();

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT numRows = 0; UINT64 rowBytes = 0, total = 0;
        g_d12Device->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &numRows, &rowBytes, &total);

        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total; bd.Height = 1;
        bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.Format = DXGI_FORMAT_UNKNOWN;
        bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; bd.Flags = D3D12_RESOURCE_FLAG_NONE;
        ID3D12Resource* readback = nullptr;
        HRESULT hr = g_d12Device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
        if (FAILED(hr) || !readback) { LOG_WARN("Capture DX12 : readback impossible (0x%08lX).", hr); back->Release(); return; }

        if (g_fence->GetCompletedValue() < f.fenceValue) {       // allocateur de cette frame libre ?
            g_fence->SetEventOnCompletion(f.fenceValue, g_fenceEvent);
            WaitForSingleObject(g_fenceEvent, 200);
        }
        f.allocator->Reset();
        g_cmdList->Reset(f.allocator, nullptr);

        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = back; b.Transition.Subresource = 0;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        g_cmdList->ResourceBarrier(1, &b);

        D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = readback;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = fp;
        D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = back;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
        g_cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE; b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        g_cmdList->ResourceBarrier(1, &b);
        g_cmdList->Close();

        ID3D12CommandList* lists[] = { g_cmdList };
        o_Execute(queue, 1, lists);
        const UINT64 fv = ++g_fenceCounter;
        queue->Signal(g_fence, fv);
        if (g_fence->GetCompletedValue() < fv) {
            g_fence->SetEventOnCompletion(fv, g_fenceEvent);
            WaitForSingleObject(g_fenceEvent, 1000);
        }
        f.fenceValue = fv;

        D3D12_RANGE rr{ 0, (SIZE_T)total };
        void* p = nullptr;
        if (SUCCEEDED(readback->Map(0, &rr, &p)) && p) {
            Screenshot::Save((unsigned)rd.Width, rd.Height, (uint32_t)rd.Format,
                             (const uint8_t*)p + fp.Offset, fp.Footprint.RowPitch);
            readback->Unmap(0, nullptr);
            LOG_INFO("Capture : back buffer DX12 lu %ux%u (format %d).", (unsigned)rd.Width, rd.Height, (int)rd.Format);
        } else LOG_WARN("Capture DX12 : Map a echoue.");

        readback->Release();
        back->Release();
    }

    HWND FindGameWindow() {
        struct Ctx { DWORD pid; HWND hwnd; } ctx{ GetCurrentProcessId(), nullptr };
        EnumWindows([](HWND h, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
            if (pid == c->pid && IsWindowVisible(h) && !GetWindow(h, GW_OWNER)) { c->hwnd = h; return FALSE; }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&ctx));
        return ctx.hwnd;
    }

    void SetupImGuiStyle() {
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr; // nos reglages sont dans client.ini
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

        // Segoe UI pour les accents francais ; police par defaut en secours.
        // Plage etendue : latin-1 (accents) + ponctuation courbe, tirets, fleches, puces.
        static const ImWchar kRanges[] = {
            0x0020, 0x00FF,
            0x2010, 0x2027,
            0x20AC, 0x20AC,
            0x2190, 0x2193,
            0x25A0, 0x25CF,
            0,
        };
        const char* font = "C:\\Windows\\Fonts\\segoeui.ttf";
        const char* fontBold = "C:\\Windows\\Fonts\\seguisb.ttf"; // Segoe UI Semibold
        if (GetFileAttributesA(font) != INVALID_FILE_ATTRIBUTES)
            io.Fonts->AddFontFromFileTTF(font, 18.f, nullptr, kRanges);
        else
            io.Fonts->AddFontDefault();
        if (GetFileAttributesA(fontBold) != INVALID_FILE_ATTRIBUTES)
            Ui::TitleFont = io.Fonts->AddFontFromFileTTF(fontBold, 24.f, nullptr, kRanges);

        ImGui::StyleColorsDark(); // le theme complet est applique chaque image par Ui::Render()
    }

    bool TryInit(IDXGISwapChain* sc) {
        if (SUCCEEDED(sc->GetDevice(IID_PPV_ARGS(&g_d12Device)))) {
            g_api = Api::DX12;
        } else if (SUCCEEDED(sc->GetDevice(IID_PPV_ARGS(&g_d11Device)))) {
            g_api = Api::DX11;
            g_d11Device->GetImmediateContext(&g_d11Context);
        } else {
            return false;
        }

        if (g_api == Api::DX12 && !g_d12Queue.load()) { SafeRelease(g_d12Device); return false; } // reessaie plus tard

        DXGI_SWAP_CHAIN_DESC desc{};
        sc->GetDesc(&desc);
        g_hwnd = desc.OutputWindow ? desc.OutputWindow : FindGameWindow();
        if (!g_hwnd) { LOG_ERROR("Fenetre du jeu introuvable"); g_initFailed = true; return false; }

        ImGui::CreateContext();
        SetupImGuiStyle();
        ImGui_ImplWin32_Init(g_hwnd);

        const bool ok = g_api == Api::DX12 ? InitDx12(sc) : ImGui_ImplDX11_Init(g_d11Device, g_d11Context);
        if (!ok) {
            LOG_ERROR("Initialisation du rendu ImGui echouee (%s)", g_api == Api::DX12 ? "DX12" : "DX11");
            g_initFailed = true;
            return false;
        }

        InputHook::Init(g_hwnd);
        LOG_INFO("Overlay initialise (%s)", g_api == Api::DX12 ? "DirectX 12" : "DirectX 11");
        return true;
    }

    void Cleanup() {
        InputHook::Shutdown();
        if (g_ready) {
            if (g_api == Api::DX12) { WaitGpuIdle(); ImGui_ImplDX12_Shutdown(); }
            else ImGui_ImplDX11_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
        }
        for (Frame& f : g_frames) { SafeRelease(f.backBuffer); SafeRelease(f.allocator); }
        g_frames.clear();
        SafeRelease(g_cmdList); SafeRelease(g_fence); SafeRelease(g_rtvHeap); SafeRelease(g_srvHeap);
        SafeRelease(g_d12Device);
        if (g_fenceEvent) { CloseHandle(g_fenceEvent); g_fenceEvent = nullptr; }
        SafeRelease(g_d11Rtv); SafeRelease(g_d11Context); SafeRelease(g_d11Device);
        g_ready = false;
        LOG_INFO("Overlay nettoye");
    }

    std::atomic<bool> g_occluded{ false };   // image precedente occultee (autre appli plein ecran)

    void OnPresent(IDXGISwapChain* sc) {
        if (g_shutdownRequested) {
            if (!g_shutdownComplete) { Cleanup(); g_shutdownComplete = true; }
            return;
        }
        if (g_initFailed) return;
        // Fenetre du jeu reduite, masquee, ou occultee par une autre appli en plein ecran
        // exclusif : on ne dessine PAS l'overlay (le swapchain/peripherique peut etre perdu,
        // dessiner dessus ferait planter le jeu). On laisse simplement le jeu presenter.
        if (g_hwnd && (IsIconic(g_hwnd) || g_occluded.load())) return;

        // Alt-Tab vers une AUTRE application : Minecraft passe en arriere-plan et son swapchain
        // peut etre dans un etat instable (surtout si l'autre appli est en plein ecran). On ne
        // dessine pas l'overlay quand notre processus n'est pas au premier plan (de toute facon
        // il n'est pas visible) -> on ne touche pas au swapchain dans ce moment a risque.
        // (Menu ouvert, FocusGrab met SA fenetre au premier plan : meme processus -> on dessine.)
        if (g_hwnd) {
            const HWND fg = GetForegroundWindow();
            if (fg) {
                DWORD fgPid = 0;
                GetWindowThreadProcessId(fg, &fgPid);
                if (fgPid != GetCurrentProcessId()) {
                    // Option dev : garder le menu dessine meme si une AUTRE appli est au premier
                    // plan (utile pour alt-tab vers l'editeur). Le filet __try plus bas absorbe
                    // un eventuel swapchain instable. Sinon : on ne dessine pas (comportement sur).
                    if (!(Config::GetSettings().devMenuOnBlur && Ui::IsMenuOpen())) return;
                }
            }
        }

        // Deplacement de la fenetre (glissement, notamment d'un moniteur a l'autre) : arme le
        // cooldown de transition. Le swapchain se reconfigure et y dessiner ferait planter.
        if (g_hwnd) {
            static RECT lastRect = { LONG_MIN, LONG_MIN, 0, 0 };
            RECT r{};
            if (GetWindowRect(g_hwnd, &r)) {
                if ((r.left != lastRect.left || r.top != lastRect.top || r.right != lastRect.right ||
                     r.bottom != lastRect.bottom) && lastRect.left != LONG_MIN)
                    ArmTransition(400);
                lastRect = r;
            }
        }
        // Pendant une transition (deplacement, resize, plein ecran), on ne touche pas au
        // swapchain : on laisse le jeu presenter seul, sans overlay, jusqu'au retour au calme.
        if (GetTickCount64() < g_transitionUntil.load()) return;

        if (!g_ready) {
            g_ready = TryInit(sc);
            if (!g_ready) return;
        }

        // Filet de securite : un changement d'ecran/resolution, un passage plein ecran ou une
        // perte de peripherique peut rendre le swapchain invalide le temps d'une image. Si notre
        // rendu provoque une exception, on l'absorbe et on saute l'image au lieu de faire planter
        // Minecraft. (OnPresent n'a pas d'objet C++ local a derouler -> __try valide ici.)
        bool frameBegun = false;
        __try {
            // DX11 : on relache la RTV si la fenetre a change de moniteur (elle sera recreee
            // a la volee). En DX12, le back buffer est relu a chaque image (voir RenderDx12),
            // donc aucun cache a rafraichir ici.
            if (g_hwnd && g_api == Api::DX11) {
                static HMONITOR lastMon = nullptr;
                const HMONITOR mon = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST);
                if (mon != lastMon) { lastMon = mon; SafeRelease(g_d11Rtv); }
            }

            if (g_api == Api::DX12) ImGui_ImplDX12_NewFrame(); else ImGui_ImplDX11_NewFrame();
            InputHook::FlushToImGui();   // messages souris/clavier recus depuis la derniere image
            ImGui_ImplWin32_NewFrame();
            // Le hook clavier bas-niveau BLOQUE les touches (dont Ctrl/Maj/Alt) pour le jeu, si
            // bien que le backend ImGui, qui lit les modificateurs via GetKeyState, les croit
            // relaches -> le Ctrl+clic (saisie exacte des sliders) ne s'activait jamais. On
            // resynchronise donc les modificateurs depuis l'etat PHYSIQUE reel (non masque ici).
            if (Ui::IsMenuOpen()) {
                ImGuiIO& io = ImGui::GetIO();
                const bool ctrl  = FocusGrab::IsCtrlDown();
                const bool shift = FocusGrab::IsShiftDown();
                const bool alt   = FocusGrab::IsAltDown();
#if defined(IMGUI_VERSION_NUM) && IMGUI_VERSION_NUM >= 18900
                io.AddKeyEvent(ImGuiMod_Ctrl,  ctrl);
                io.AddKeyEvent(ImGuiMod_Shift, shift);
                io.AddKeyEvent(ImGuiMod_Alt,   alt);
                io.AddKeyEvent(ImGuiKey_LeftCtrl,  ctrl);
                io.AddKeyEvent(ImGuiKey_LeftShift, shift);
                io.AddKeyEvent(ImGuiKey_LeftAlt,   alt);
#else
                io.KeyCtrl = ctrl; io.KeyShift = shift; io.KeyAlt = alt;
#endif
            }
            ImGui::NewFrame();
            frameBegun = true;
            Ui::Render();
            ImGui::Render();
            frameBegun = false;          // frame ImGui fermee par Render()
            if (g_api == Api::DX12) RenderDx12(sc); else RenderDx11(sc);

            // Capture d'ecran : APRES le dessin de l'overlay -> l'image contient tout ce que le
            // joueur voit (HUD Nexus : FPS, armure, barres de vie, CPS, nametags, et le menu s'il est ouvert).
            if (Screenshot::PollRequest()) {
                if (g_api == Api::DX12) CaptureDx12(sc); else CaptureDx11(sc);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            if (frameBegun) { __try { ImGui::EndFrame(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
            { static ULONGLONG t = 0; const ULONGLONG n = GetTickCount64();
              if (n - t > 500) { t = n; LOG_WARN("Rendu : image ignoree (exception absorbee)"); } }
        }
    }

    // ---- Hooks ------------------------------------------------------------------
    // Selon la version de DXGI, Present peut appeler Present1 en interne (ou l'inverse) :
    // ce drapeau evite de dessiner deux fois la meme image.
    thread_local bool t_inPresent = false;

    HRESULT STDMETHODCALLTYPE HkPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
        if (t_inPresent) return o_Present(sc, sync, flags);
        t_inPresent = true;
        if (!(flags & DXGI_PRESENT_TEST)) OnPresent(sc);
        HRESULT hr = o_Present(sc, sync, flags);
        g_occluded = (hr == DXGI_STATUS_OCCLUDED);   // occulte -> on sautera le rendu de l'overlay
        t_inPresent = false;
        return hr;
    }

    HRESULT STDMETHODCALLTYPE HkPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p) {
        if (t_inPresent) return o_Present1(sc, sync, flags, p);
        t_inPresent = true;
        if (!(flags & DXGI_PRESENT_TEST)) OnPresent(sc);
        HRESULT hr = o_Present1(sc, sync, flags, p);
        g_occluded = (hr == DXGI_STATUS_OCCLUDED);
        t_inPresent = false;
        return hr;
    }

    HRESULT STDMETHODCALLTYPE HkSetFullscreenState(IDXGISwapChain* sc, BOOL fs, IDXGIOutput* out) {
        ArmTransition(1200);   // bascule plein ecran <-> fenetre : grosse reconfiguration
        const HRESULT hr = o_SetFullscreen(sc, fs, out);
        ArmTransition(1200);
        return hr;
    }

    HRESULT STDMETHODCALLTYPE HkResizeBuffers(IDXGISwapChain* sc, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) {
        ArmTransition(800);   // le swapchain change : pas de rendu overlay pendant la transition
        // Changement d'ecran / de resolution / bascule plein ecran : on libere nos references
        // aux back buffers AVANT le resize (obligatoire en DX12), puis on les recree. Tout est
        // protege : si une etape echoue, on ne fait pas planter le jeu et on reinitialisera
        // l'overlay a la prochaine image valide.
        if (g_ready) {
            __try {
                if (g_api == Api::DX12) { WaitGpuIdle(); ReleaseDx12BackBuffers(); }
                else SafeRelease(g_d11Rtv);
            } __except (EXCEPTION_EXECUTE_HANDLER) { LOG_WARN("ResizeBuffers: exception a la liberation"); }
        }
        HRESULT hr = o_ResizeBuffers(sc, count, w, h, fmt, flags);
        if (g_ready && g_api == Api::DX12 && SUCCEEDED(hr)) {
            __try { if (!CreateDx12Frames(sc)) { ReleaseDx12BackBuffers(); g_frames.clear(); } }
            __except (EXCEPTION_EXECUTE_HANDLER) { LOG_WARN("ResizeBuffers: exception a la recreation"); }
        }
        return hr;
    }

    void STDMETHODCALLTYPE HkExecute(ID3D12CommandQueue* queue, UINT n, ID3D12CommandList* const* lists) {
        if (!g_d12Queue.load() && queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT)
            g_d12Queue.store(queue);
        o_Execute(queue, n, lists);
    }

    // Cree une swapchain factice pour recuperer les adresses des fonctions a hooker.
    bool GetVTableAddresses() {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"Nexus_Dummy";
        RegisterClassExW(&wc);
        HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100,
                                    nullptr, nullptr, wc.hInstance, nullptr);
        if (!hwnd) return false;

        bool ok = false;
        ID3D12Device* dev = nullptr; ID3D12CommandQueue* queue = nullptr;
        IDXGIFactory2* factory = nullptr; IDXGISwapChain1* sc = nullptr;

        if (SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)))) {
            D3D12_COMMAND_QUEUE_DESC qd{ D3D12_COMMAND_LIST_TYPE_DIRECT };
            DXGI_SWAP_CHAIN_DESC1 sd{};
            sd.Width = 100; sd.Height = 100; sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            sd.SampleDesc.Count = 1; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            sd.BufferCount = 2; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

            if (SUCCEEDED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) &&
                SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) &&
                SUCCEEDED(factory->CreateSwapChainForHwnd(queue, hwnd, &sd, nullptr, nullptr, &sc))) {
                void** scVt = *reinterpret_cast<void***>(sc);
                void** qVt = *reinterpret_cast<void***>(queue);
                t_Present       = scVt[8];
                t_SetFullscreen = scVt[10];
                t_Resize        = scVt[13];
                t_Present1      = scVt[22];
                t_Execute       = qVt[10];
                ok = true;
            }
        }
        if (!ok) LOG_WARN("Swapchain DX12 factice impossible, essai en DX11");

        if (!ok) { // repli DX11 (pas de hook ExecuteCommandLists dans ce cas)
            DXGI_SWAP_CHAIN_DESC sd{};
            sd.BufferCount = 1; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow = hwnd;
            sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
            IDXGISwapChain* sc11 = nullptr; ID3D11Device* d11 = nullptr; ID3D11DeviceContext* c11 = nullptr;
            if (SUCCEEDED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                                        D3D11_SDK_VERSION, &sd, &sc11, &d11, nullptr, &c11))) {
                void** scVt = *reinterpret_cast<void***>(sc11);
                t_Present       = scVt[8];
                t_SetFullscreen = scVt[10];
                t_Resize        = scVt[13];
                ok = true;
            }
            SafeRelease(sc11); SafeRelease(c11); SafeRelease(d11);
        }

        SafeRelease(sc); SafeRelease(factory); SafeRelease(queue); SafeRelease(dev);
        DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return ok;
    }

    bool Hook(void* target, void* detour, void** original, const char* name) {
        if (!target) return false;
        if (MH_CreateHook(target, detour, original) != MH_OK || MH_EnableHook(target) != MH_OK) {
            LOG_ERROR("Hook %s impossible", name);
            return false;
        }
        LOG_INFO("Hook %s installe", name);
        return true;
    }
}

bool RenderHook::Init() {
    if (!GetVTableAddresses()) { LOG_ERROR("Impossible de recuperer les fonctions DXGI"); return false; }

    bool ok = Hook(t_Present, &HkPresent, reinterpret_cast<void**>(&o_Present), "Present");
    Hook(t_Present1, &HkPresent1, reinterpret_cast<void**>(&o_Present1), "Present1");
    ok &= Hook(t_Resize, &HkResizeBuffers, reinterpret_cast<void**>(&o_ResizeBuffers), "ResizeBuffers");
    Hook(t_Execute, &HkExecute, reinterpret_cast<void**>(&o_Execute), "ExecuteCommandLists");
    Hook(t_SetFullscreen, &HkSetFullscreenState, reinterpret_cast<void**>(&o_SetFullscreen), "SetFullscreenState");
    return ok;
}

void RenderHook::RequestShutdown() {
    if (!g_ready && !g_d12Device && !g_d11Device) g_shutdownComplete = true; // rien a nettoyer
    g_shutdownRequested = true;
}

bool RenderHook::IsShutdownComplete() { return g_shutdownComplete; }

// Cree une texture ImGui depuis des pixels RGBA8. Thread de rendu uniquement.
unsigned long long RenderHook::LoadTexture(const unsigned char* rgba, int w, int h) {
    if (!rgba || w <= 0 || h <= 0 || w > 1024 || h > 1024) return 0;

    // ---- DirectX 11 : texture + SRV (ImTextureID = pointeur du SRV) ----
    if (g_api == Api::DX11 && g_d11Device) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA srd{}; srd.pSysMem = rgba; srd.SysMemPitch = UINT(w) * 4;
        ID3D11Texture2D* tex = nullptr;
        if (FAILED(g_d11Device->CreateTexture2D(&td, &srd, &tex)) || !tex) return 0;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = td.Format; sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
        ID3D11ShaderResourceView* srv = nullptr;
        const HRESULT hr = g_d11Device->CreateShaderResourceView(tex, &sd, &srv);
        tex->Release();
        if (FAILED(hr) || !srv) return 0;
        return (unsigned long long)(uintptr_t)srv;
    }

    // ---- DirectX 12 : texture GPU + upload + SRV (ImTextureID = handle GPU) ----
    auto queue = g_d12Queue.load();
    if (g_api == Api::DX12 && g_d12Device && queue && !g_srvFree.empty()) {
        D3D12_HEAP_PROPERTIES hpDef{}; hpDef.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = UINT(w); rd.Height = UINT(h);
        rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        ID3D12Resource* tex = nullptr;
        if (FAILED(g_d12Device->CreateCommittedResource(&hpDef, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex))) || !tex) return 0;

        UINT64 uploadSize = 0; UINT rows = 0; UINT64 rowBytes = 0; D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        g_d12Device->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &rows, &rowBytes, &uploadSize);

        D3D12_HEAP_PROPERTIES hpUp{}; hpUp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = uploadSize; bd.Height = 1;
        bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.Format = DXGI_FORMAT_UNKNOWN;
        bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource* upload = nullptr;
        if (FAILED(g_d12Device->CreateCommittedResource(&hpUp, D3D12_HEAP_FLAG_NONE, &bd,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload))) || !upload) {
            tex->Release(); return 0;
        }

        uint8_t* mapped = nullptr; D3D12_RANGE empty{ 0, 0 };
        if (FAILED(upload->Map(0, &empty, reinterpret_cast<void**>(&mapped)))) { upload->Release(); tex->Release(); return 0; }
        for (UINT y = 0; y < UINT(h); ++y)
            memcpy(mapped + fp.Offset + size_t(y) * fp.Footprint.RowPitch, rgba + size_t(y) * w * 4, size_t(w) * 4);
        upload->Unmap(0, nullptr);

        ID3D12CommandAllocator* alloc = nullptr; ID3D12GraphicsCommandList* cl = nullptr;
        if (FAILED(g_d12Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
            FAILED(g_d12Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&cl)))) {
            SafeRelease(alloc); upload->Release(); tex->Release(); return 0;
        }
        D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = tex; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = upload; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = fp;
        cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = tex;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cl->ResourceBarrier(1, &b);
        cl->Close();
        ID3D12CommandList* lists[] = { cl };
        queue->ExecuteCommandLists(1, lists);
        WaitGpuIdle();                         // attend la fin de l'upload
        cl->Release(); alloc->Release(); upload->Release();

        const UINT inc = g_d12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        const UINT idx = g_srvFree.back(); g_srvFree.pop_back();
        D3D12_CPU_DESCRIPTOR_HANDLE cpu{ g_srvHeap->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T(idx) * inc };
        D3D12_GPU_DESCRIPTOR_HANDLE gpu{ g_srvHeap->GetGPUDescriptorHandleForHeapStart().ptr + UINT64(idx) * inc };
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1; sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        g_d12Device->CreateShaderResourceView(tex, &sd, cpu);
        // tex volontairement conserve (icone persistante pour la session).
        return (unsigned long long)gpu.ptr;
    }
    return 0;
}
