#include "RenderHook.h"
#include "InputHook.h"
#include "core/Logger.h"
#include "ui/Ui.h"

#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <MinHook.h>
#include <imgui.h>
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

    PresentFn       o_Present = nullptr;
    Present1Fn      o_Present1 = nullptr;
    ResizeBuffersFn o_ResizeBuffers = nullptr;
    ExecuteFn       o_Execute = nullptr;
    void* t_Present = nullptr; void* t_Present1 = nullptr; void* t_Resize = nullptr; void* t_Execute = nullptr;

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
            if (FAILED(sc->GetBuffer(i, IID_PPV_ARGS(&f.backBuffer)))) return false;
            f.rtv.ptr = rtvStart.ptr + SIZE_T(i) * rtvInc;
            g_d12Device->CreateRenderTargetView(f.backBuffer, nullptr, f.rtv);
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
        if (idx >= g_frames.size() || !g_frames[idx].backBuffer) return;

        Frame& f = g_frames[idx];
        if (g_fence->GetCompletedValue() < f.fenceValue) {
            g_fence->SetEventOnCompletion(f.fenceValue, g_fenceEvent);
            WaitForSingleObject(g_fenceEvent, 100);
        }

        f.allocator->Reset();
        g_cmdList->Reset(f.allocator, nullptr);

        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = f.backBuffer;
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
        const char* font = "C:\\Windows\\Fonts\\segoeui.ttf";
        const char* fontBold = "C:\\Windows\\Fonts\\seguisb.ttf"; // Segoe UI Semibold
        if (GetFileAttributesA(font) != INVALID_FILE_ATTRIBUTES)
            io.Fonts->AddFontFromFileTTF(font, 18.f, nullptr, io.Fonts->GetGlyphRangesDefault());
        else
            io.Fonts->AddFontDefault();
        if (GetFileAttributesA(fontBold) != INVALID_FILE_ATTRIBUTES)
            Ui::TitleFont = io.Fonts->AddFontFromFileTTF(fontBold, 24.f, nullptr, io.Fonts->GetGlyphRangesDefault());

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

    void OnPresent(IDXGISwapChain* sc) {
        if (g_shutdownRequested) {
            if (!g_shutdownComplete) { Cleanup(); g_shutdownComplete = true; }
            return;
        }
        if (g_initFailed) return;
        if (!g_ready) {
            g_ready = TryInit(sc);
            if (!g_ready) return;
        }

        if (g_api == Api::DX12) ImGui_ImplDX12_NewFrame(); else ImGui_ImplDX11_NewFrame();
        InputHook::FlushToImGui();   // messages souris/clavier recus depuis la derniere image
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        Ui::Render();
        ImGui::Render();

        if (g_api == Api::DX12) RenderDx12(sc); else RenderDx11(sc);
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
        t_inPresent = false;
        return hr;
    }

    HRESULT STDMETHODCALLTYPE HkPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p) {
        if (t_inPresent) return o_Present1(sc, sync, flags, p);
        t_inPresent = true;
        if (!(flags & DXGI_PRESENT_TEST)) OnPresent(sc);
        HRESULT hr = o_Present1(sc, sync, flags, p);
        t_inPresent = false;
        return hr;
    }

    HRESULT STDMETHODCALLTYPE HkResizeBuffers(IDXGISwapChain* sc, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) {
        if (g_ready) {
            if (g_api == Api::DX12) { WaitGpuIdle(); ReleaseDx12BackBuffers(); }
            else SafeRelease(g_d11Rtv);
        }
        HRESULT hr = o_ResizeBuffers(sc, count, w, h, fmt, flags);
        if (g_ready && g_api == Api::DX12 && SUCCEEDED(hr)) CreateDx12Frames(sc);
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
                t_Present  = scVt[8];
                t_Resize   = scVt[13];
                t_Present1 = scVt[22];
                t_Execute  = qVt[10];
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
                t_Present = scVt[8];
                t_Resize = scVt[13];
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
    return ok;
}

void RenderHook::RequestShutdown() {
    if (!g_ready && !g_d12Device && !g_d11Device) g_shutdownComplete = true; // rien a nettoyer
    g_shutdownRequested = true;
}

bool RenderHook::IsShutdownComplete() { return g_shutdownComplete; }
