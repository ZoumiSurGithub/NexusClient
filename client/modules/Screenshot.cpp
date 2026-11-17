#include "Screenshot.h"
#include "core/Config.h"
#include "core/Logger.h"

#include <windows.h>
#include <wincodec.h>       // WIC : encodage PNG natif Windows
#include <dxgiformat.h>
#include <filesystem>
#include <thread>
#include <vector>
#include <cstring>
#include <atomic>

namespace fs = std::filesystem;

namespace Screenshot {

std::wstring Folder() {
    fs::path d = Config::DataFolder() / L"screenshots";
    std::error_code ec; fs::create_directories(d, ec);
    return d.wstring();
}

static std::atomic<bool> g_requested{ false };
void RequestCapture() { g_requested.store(true); }

static std::atomic<unsigned long long> g_shotTick{ 0 };
unsigned long long LastShotTick() { return g_shotTick.load(); }

bool PollRequest() {
    const auto& s = Config::GetSettings();
    static bool was = false;
    const bool flag = g_requested.exchange(false);          // demande du hook clavier (menu ouvert)
    if (!s.screenshotEnabled || s.screenshotKey <= 0) { was = false; return false; }
    const bool down = (GetAsyncKeyState(s.screenshotKey) & 0x8000) != 0;   // menu ferme
    const bool edge = down && !was;
    was = down || flag;   // si le hook a declenche, on "avale" un eventuel front tardif d'AsyncKeyState
    return edge || flag;
}

// --- Encodage PNG via WIC, sur un thread dedie (COM propre a ce thread) ---------
static void EncodePng(std::wstring path, unsigned w, unsigned h, bool bgra,
                      std::vector<uint8_t> pixels, unsigned stride) {
    const bool comOk = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    IWICImagingFactory* factory = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&factory))) && factory) {
        const WICPixelFormatGUID srcFmt = bgra ? GUID_WICPixelFormat32bppBGRA
                                               : GUID_WICPixelFormat32bppRGBA;
        IWICStream* stream = nullptr;
        if (SUCCEEDED(factory->CreateStream(&stream)) && stream &&
            SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) {
            IWICBitmapEncoder* enc = nullptr;
            if (SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) && enc &&
                SUCCEEDED(enc->Initialize(stream, WICBitmapEncoderNoCache))) {
                IWICBitmapFrameEncode* frame = nullptr; IPropertyBag2* props = nullptr;
                if (SUCCEEDED(enc->CreateNewFrame(&frame, &props)) && frame &&
                    SUCCEEDED(frame->Initialize(props))) {
                    frame->SetSize(w, h);
                    WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;   // PNG supporte le BGRA
                    frame->SetPixelFormat(&pf);
                    IWICBitmap* bmp = nullptr;
                    if (SUCCEEDED(factory->CreateBitmapFromMemory(w, h, srcFmt, stride,
                                   (UINT)pixels.size(), pixels.data(), &bmp)) && bmp) {
                        frame->WriteSource(bmp, nullptr);   // conversion RGBA->BGRA au besoin
                        bmp->Release();
                    }
                    frame->Commit();
                    frame->Release();
                }
                if (props) props->Release();
                enc->Commit();
                enc->Release();
            }
            stream->Release();
        }
        factory->Release();
    }
    if (comOk) CoUninitialize();
    LOG_INFO("Capture d'ecran enregistree : %ls", path.c_str());
}

void Save(unsigned width, unsigned height, uint32_t dxgiFormat, const void* pixels, unsigned rowPitch) {
    if (!pixels || width == 0 || height == 0) return;

    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t name[64];
    swprintf(name, 64, L"Nexus_%04d-%02d-%02d_%02d-%02d-%02d.png",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    const std::wstring path = (fs::path(Folder()) / name).wstring();

    const bool bgra = (dxgiFormat == DXGI_FORMAT_B8G8R8A8_UNORM ||
                       dxgiFormat == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);

    // Copie compacte (le back buffer peut avoir un rowPitch > largeur*4).
    const unsigned dstStride = width * 4;
    std::vector<uint8_t> buf((size_t)dstStride * height);
    const uint8_t* src = static_cast<const uint8_t*>(pixels);
    for (unsigned y = 0; y < height; ++y)
        memcpy(buf.data() + (size_t)y * dstStride, src + (size_t)y * rowPitch, dstStride);

    g_shotTick.store(GetTickCount64());        // declenche la confirmation (flash + toast)
    LOG_INFO("Capture d'ecran : enregistrement de %ls", path.c_str());
    std::thread(EncodePng, path, width, height, bgra, std::move(buf), dstStride).detach();
}

} // namespace Screenshot
