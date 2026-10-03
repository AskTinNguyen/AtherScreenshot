#include "wgc.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <algorithm>

#pragma comment(lib, "d3d11")
#pragma comment(lib, "dxgi")

namespace ather {

namespace wgcap = winrt::Windows::Graphics::Capture;
namespace wgdx = winrt::Windows::Graphics::DirectX;

struct WgcSource::Impl {
    winrt::com_ptr<ID3D11Device> dev;
    winrt::com_ptr<ID3D11DeviceContext> ctx;
    wgdx::Direct3D11::IDirect3DDevice rtDev{nullptr};
    wgcap::GraphicsCaptureItem item{nullptr};
    wgcap::Direct3D11CaptureFramePool pool{nullptr};
    wgcap::GraphicsCaptureSession session{nullptr};
    winrt::com_ptr<ID3D11Texture2D> staging;
    winrt::Windows::Graphics::SizeInt32 poolSize{};
    bool windowMode = false;
    RECT crop{};  // region mode: crop inside the monitor texture
    int outW = 0, outH = 0;

    bool InitDevice(std::wstring* error) {
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                       nullptr, 0, D3D11_SDK_VERSION, dev.put(), nullptr, ctx.put());
        if (FAILED(hr)) {
            if (error) *error = L"Direct3D 11 is unavailable.";
            return false;
        }
        winrt::com_ptr<IDXGIDevice> dxgi = dev.as<IDXGIDevice>();
        winrt::com_ptr<::IInspectable> insp;
        if (FAILED(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), insp.put()))) return false;
        rtDev = insp.as<wgdx::Direct3D11::IDirect3DDevice>();
        D3D11_TEXTURE2D_DESC d{};
        d.Width = outW;
        d.Height = outH;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_STAGING;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        return SUCCEEDED(dev->CreateTexture2D(&d, nullptr, staging.put()));
    }

    bool StartSession(bool cursor) {
        poolSize = item.Size();
        pool = wgcap::Direct3D11CaptureFramePool::CreateFreeThreaded(rtDev, wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                                                                     2, poolSize);
        session = pool.CreateCaptureSession(item);
        try {
            session.IsCursorCaptureEnabled(cursor);
        } catch (...) {
        }
        try {
            session.IsBorderRequired(false);  // Windows 11; may need user consent, so it's best-effort
        } catch (...) {
        }
        session.StartCapture();
        return true;
    }
};

WgcSource::WgcSource() : impl_(std::make_unique<Impl>()) {}
WgcSource::~WgcSource() { Stop(); }

bool WgcSource::Supported() {
    try {
        return wgcap::GraphicsCaptureSession::IsSupported();
    } catch (...) {
        return false;
    }
}

bool WgcSource::StartWindow(HWND hwnd, int outW, int outH, bool cursor, std::wstring* error) {
    try {
        Impl& m = *impl_;
        m.windowMode = true;
        m.outW = outW;
        m.outH = outH;
        if (!m.InitDevice(error)) return false;
        auto interop = winrt::get_activation_factory<wgcap::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        winrt::check_hresult(interop->CreateForWindow(hwnd, winrt::guid_of<wgcap::GraphicsCaptureItem>(),
                                                      winrt::put_abi(m.item)));
        return m.StartSession(cursor);
    } catch (const winrt::hresult_error& e) {
        if (error) *error = L"Window capture failed: " + std::wstring(e.message().c_str());
        return false;
    }
}

bool WgcSource::StartRegion(const RECT& region, int outW, int outH, bool cursor, std::wstring* error) {
    try {
        Impl& m = *impl_;
        HMONITOR mon = MonitorFromRect(&region, MONITOR_DEFAULTTONULL);
        MONITORINFO mi{sizeof(mi)};
        RECT inter;
        if (!mon || !GetMonitorInfoW(mon, &mi) || !IntersectRect(&inter, &region, &mi.rcMonitor) ||
            !EqualRect(&inter, &region)) {
            if (error) *error = L"Region spans monitors.";
            return false;
        }
        m.windowMode = false;
        m.outW = outW;
        m.outH = outH;
        m.crop = region;
        OffsetRect(&m.crop, -mi.rcMonitor.left, -mi.rcMonitor.top);
        if (!m.InitDevice(error)) return false;
        auto interop = winrt::get_activation_factory<wgcap::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        winrt::check_hresult(interop->CreateForMonitor(mon, winrt::guid_of<wgcap::GraphicsCaptureItem>(),
                                                       winrt::put_abi(m.item)));
        return m.StartSession(cursor);
    } catch (const winrt::hresult_error& e) {
        if (error) *error = L"GPU capture failed: " + std::wstring(e.message().c_str());
        return false;
    }
}

bool WgcSource::Grab(uint32_t* dst) {
    Impl& m = *impl_;
    if (!m.pool) return false;
    try {
        wgcap::Direct3D11CaptureFrame frame{nullptr};
        while (auto f = m.pool.TryGetNextFrame()) frame = f;  // keep only the newest
        if (!frame) return false;
        const auto content = frame.ContentSize();
        auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        winrt::com_ptr<ID3D11Texture2D> tex;
        winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(tex.put())));
        D3D11_TEXTURE2D_DESC td{};
        tex->GetDesc(&td);

        D3D11_BOX box{0, 0, 0, 0, 0, 1};
        if (m.windowMode) {
            box.right = (UINT)std::min({content.Width, (int)td.Width, m.outW});
            box.bottom = (UINT)std::min({content.Height, (int)td.Height, m.outH});
        } else {
            box.left = (UINT)std::max(0L, m.crop.left);
            box.top = (UINT)std::max(0L, m.crop.top);
            box.right = (UINT)std::min<LONG>(m.crop.right, (LONG)td.Width);
            box.bottom = (UINT)std::min<LONG>(m.crop.bottom, (LONG)td.Height);
        }
        if (box.right > box.left && box.bottom > box.top) {
            m.ctx->CopySubresourceRegion(m.staging.get(), 0, 0, 0, 0, tex.get(), 0, &box);
            D3D11_MAPPED_SUBRESOURCE map{};
            if (SUCCEEDED(m.ctx->Map(m.staging.get(), 0, D3D11_MAP_READ, 0, &map))) {
                const int cw = (int)(box.right - box.left), ch = (int)(box.bottom - box.top);
                if (cw < m.outW || ch < m.outH) std::fill_n(dst, (size_t)m.outW * m.outH, 0xFF000000u);
                for (int y = 0; y < ch; ++y)
                    memcpy(dst + (size_t)y * m.outW, static_cast<const BYTE*>(map.pData) + (size_t)y * map.RowPitch,
                           (size_t)cw * 4);
                m.ctx->Unmap(m.staging.get(), 0);
            }
        }
        // A resized window needs a pool that matches its new size, or frames get stretched.
        if (m.windowMode && (content.Width != m.poolSize.Width || content.Height != m.poolSize.Height)) {
            m.poolSize = content;
            m.pool.Recreate(m.rtDev, wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, content);
        }
        return true;
    } catch (...) {
        return false;  // e.g. the window was closed: keep the last frame
    }
}

void WgcSource::Stop() {
    Impl& m = *impl_;
    try {
        if (m.session) m.session.Close();
        if (m.pool) m.pool.Close();
    } catch (...) {
    }
    m.session = nullptr;
    m.pool = nullptr;
    m.item = nullptr;
}

SIZE WgcWindowSize(HWND hwnd) {
    try {
        auto interop = winrt::get_activation_factory<wgcap::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        wgcap::GraphicsCaptureItem item{nullptr};
        winrt::check_hresult(
            interop->CreateForWindow(hwnd, winrt::guid_of<wgcap::GraphicsCaptureItem>(), winrt::put_abi(item)));
        auto s = item.Size();
        return {s.Width, s.Height};
    } catch (...) {
        return {0, 0};
    }
}

}  // namespace ather
