#include "desktop_capture.h"
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <chrono>
#include <stdexcept>
#include <cstring>
#include <mutex>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

using Microsoft::WRL::ComPtr;
namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgdx = winrt::Windows::Graphics::DirectX;
namespace wgdx11 = winrt::Windows::Graphics::DirectX::Direct3D11;

std::vector<DesktopOutput> DesktopCapture::enumerate() {
  std::vector<DesktopOutput> out;
  ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return out;

  for (UINT ai = 0;; ++ai) {
    ComPtr<IDXGIAdapter1> adapter;
    if (factory->EnumAdapters1(ai, &adapter) == DXGI_ERROR_NOT_FOUND) break;

    for (UINT oi = 0;; ++oi) {
      ComPtr<IDXGIOutput> output;
      if (adapter->EnumOutputs(oi, &output) == DXGI_ERROR_NOT_FOUND) break;

      DXGI_OUTPUT_DESC d{};
      if (FAILED(output->GetDesc(&d))) continue;

      DesktopOutput item;
      item.adapter_index = ai;
      item.output_index = oi;
      item.name = d.DeviceName;
      item.width = static_cast<unsigned>(d.DesktopCoordinates.right - d.DesktopCoordinates.left);
      item.height = static_cast<unsigned>(d.DesktopCoordinates.bottom - d.DesktopCoordinates.top);
      item.left = d.DesktopCoordinates.left;
      item.top = d.DesktopCoordinates.top;
      item.right = d.DesktopCoordinates.right;
      item.bottom = d.DesktopCoordinates.bottom;
      out.push_back(std::move(item));
    }
  }
  return out;
}

DesktopCapture::~DesktopCapture() { stop(); }

// Kept as a fallback for diagnostics/older code paths.
void DesktopCapture::start(const DesktopOutput& sel, FrameCallback cb) {
  stop();
  running_ = true;
  frames_ = 0;
  fps_ = 0.0;

  thread_ = std::thread([this, sel, cb = std::move(cb)]() mutable {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    try {
      ComPtr<IDXGIFactory1> factory;
      if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        throw std::runtime_error("DXGI Factory fehlgeschlagen.");

      ComPtr<IDXGIAdapter1> adapter;
      if (FAILED(factory->EnumAdapters1(sel.adapter_index, &adapter)))
        throw std::runtime_error("Grafikadapter nicht gefunden.");

      ComPtr<IDXGIOutput> output;
      if (FAILED(adapter->EnumOutputs(sel.output_index, &output)))
        throw std::runtime_error("Bildschirm nicht gefunden.");

      ComPtr<ID3D11Device> device;
      ComPtr<ID3D11DeviceContext> context;
      D3D_FEATURE_LEVEL fl{};
      UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
      HRESULT hr = D3D11CreateDevice(
          adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags,
          nullptr, 0, D3D11_SDK_VERSION, &device, &fl, &context);
      if (FAILED(hr)) throw std::runtime_error("D3D11 Device fehlgeschlagen.");

      ComPtr<IDXGIOutput1> output1;
      if (FAILED(output.As(&output1)))
        throw std::runtime_error("IDXGIOutput1 fehlt.");

      ComPtr<IDXGIOutputDuplication> dupl;
      hr = output1->DuplicateOutput(device.Get(), &dupl);
      if (FAILED(hr)) throw std::runtime_error("Desktop Duplication fehlgeschlagen.");

      DXGI_OUTDUPL_DESC dd{};
      dupl->GetDesc(&dd);
      const UINT width = dd.ModeDesc.Width;
      const UINT height = dd.ModeDesc.Height;

      D3D11_TEXTURE2D_DESC stageDesc{};
      stageDesc.Width = width;
      stageDesc.Height = height;
      stageDesc.MipLevels = 1;
      stageDesc.ArraySize = 1;
      stageDesc.Format = dd.ModeDesc.Format;
      stageDesc.SampleDesc.Count = 1;
      stageDesc.Usage = D3D11_USAGE_STAGING;
      stageDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

      ComPtr<ID3D11Texture2D> staging;
      if (FAILED(device->CreateTexture2D(&stageDesc, nullptr, &staging)))
        throw std::runtime_error("Staging Texture fehlgeschlagen.");

      auto secondStart = std::chrono::steady_clock::now();
      std::uint64_t secondFrames = 0;

      while (running_) {
        DXGI_OUTDUPL_FRAME_INFO info{};
        ComPtr<IDXGIResource> resource;
        hr = dupl->AcquireNextFrame(20, &info, &resource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
        if (hr == DXGI_ERROR_ACCESS_LOST) break;
        if (FAILED(hr)) continue;

        ComPtr<ID3D11Texture2D> tex;
        if (SUCCEEDED(resource.As(&tex))) {
          context->CopyResource(staging.Get(), tex.Get());
          D3D11_MAPPED_SUBRESOURCE mapped{};
          if (SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            std::vector<std::uint8_t> bgra(static_cast<size_t>(width) * height * 4);
            for (UINT y = 0; y < height; ++y) {
              std::memcpy(bgra.data() + static_cast<size_t>(y) * width * 4,
                          static_cast<const std::uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch,
                          static_cast<size_t>(width) * 4);
            }
            context->Unmap(staging.Get(), 0);
            const auto now = std::chrono::steady_clock::now();
            const auto us = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
            cb(width, height, std::move(bgra), us);
            ++frames_;
            ++secondFrames;
            const auto elapsed = std::chrono::duration<double>(now - secondStart).count();
            if (elapsed >= 1.0) {
              fps_ = secondFrames / elapsed;
              secondFrames = 0;
              secondStart = now;
            }
          }
        }
        dupl->ReleaseFrame();
      }
    } catch (...) {
      running_ = false;
    }
    CoUninitialize();
  });
}

void DesktopCapture::start_window(HWND hwnd, FrameCallback cb) {
  stop();
  if (!hwnd || !IsWindow(hwnd)) throw std::runtime_error("WoW-Fenster ist nicht verfuegbar.");

  running_ = true;
  frames_ = 0;
  fps_ = 0.0;

  thread_ = std::thread([this, hwnd, cb = std::move(cb)]() mutable {
    try {
      winrt::init_apartment(winrt::apartment_type::multi_threaded);
      if (!wgc::GraphicsCaptureSession::IsSupported()) {
        throw std::runtime_error("Windows Graphics Capture wird auf diesem System nicht unterstuetzt.");
      }

      ComPtr<ID3D11Device> d3dDevice;
      ComPtr<ID3D11DeviceContext> context;
      D3D_FEATURE_LEVEL featureLevel{};
      UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
      HRESULT hr = D3D11CreateDevice(
          nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
          nullptr, 0, D3D11_SDK_VERSION,
          &d3dDevice, &featureLevel, &context);
      if (FAILED(hr)) throw std::runtime_error("D3D11 Device fuer WoW-Fensteraufnahme fehlgeschlagen.");

      ComPtr<IDXGIDevice> dxgiDevice;
      if (FAILED(d3dDevice.As(&dxgiDevice))) throw std::runtime_error("DXGI Device fehlt.");

      winrt::com_ptr<IInspectable> inspectable;
      winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(), inspectable.put()));
      auto winrtDevice = inspectable.as<wgdx11::IDirect3DDevice>();

      auto interopFactory = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
      wgc::GraphicsCaptureItem item{nullptr};
      winrt::check_hresult(interopFactory->CreateForWindow(
          hwnd, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(item)));

      auto size = item.Size();
      if (size.Width <= 0 || size.Height <= 0) throw std::runtime_error("WoW-Fenster hat keine gueltige Groesse.");

      auto framePool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
          winrtDevice, wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 3, size);
      auto session = framePool.CreateCaptureSession(item);

      std::mutex captureMutex;
      ComPtr<ID3D11Texture2D> staging;
      UINT stageWidth = 0;
      UINT stageHeight = 0;
      auto secondStart = std::chrono::steady_clock::now();
      std::uint64_t secondFrames = 0;

      auto token = framePool.FrameArrived([&](wgc::Direct3D11CaptureFramePool const& sender, winrt::Windows::Foundation::IInspectable const&) {
        if (!running_) return;
        std::lock_guard<std::mutex> lock(captureMutex);

        auto frame = sender.TryGetNextFrame();
        if (!frame) return;
        auto contentSize = frame.ContentSize();
        if (contentSize.Width <= 0 || contentSize.Height <= 0) return;

        auto surface = frame.Surface();
        auto access = surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        winrt::com_ptr<ID3D11Texture2D> texture;
        winrt::check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D), texture.put_void()));

        D3D11_TEXTURE2D_DESC texDesc{};
        texture->GetDesc(&texDesc);
        const UINT width = static_cast<UINT>(contentSize.Width);
        const UINT height = static_cast<UINT>(contentSize.Height);

        if (!staging || stageWidth != width || stageHeight != height) {
          D3D11_TEXTURE2D_DESC stageDesc{};
          stageDesc.Width = width;
          stageDesc.Height = height;
          stageDesc.MipLevels = 1;
          stageDesc.ArraySize = 1;
          stageDesc.Format = texDesc.Format;
          stageDesc.SampleDesc.Count = 1;
          stageDesc.Usage = D3D11_USAGE_STAGING;
          stageDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
          staging.Reset();
          if (FAILED(d3dDevice->CreateTexture2D(&stageDesc, nullptr, &staging))) return;
          stageWidth = width;
          stageHeight = height;
        }

        D3D11_BOX srcBox{0, 0, 0, width, height, 1};
        context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, texture.get(), 0, &srcBox);

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return;

        std::vector<std::uint8_t> bgra(static_cast<size_t>(width) * height * 4);
        for (UINT y = 0; y < height; ++y) {
          std::memcpy(bgra.data() + static_cast<size_t>(y) * width * 4,
                      static_cast<const std::uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch,
                      static_cast<size_t>(width) * 4);
        }
        context->Unmap(staging.Get(), 0);

        const auto now = std::chrono::steady_clock::now();
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
        cb(width, height, std::move(bgra), us);

        ++frames_;
        ++secondFrames;
        const auto elapsed = std::chrono::duration<double>(now - secondStart).count();
        if (elapsed >= 1.0) {
          fps_ = secondFrames / elapsed;
          secondFrames = 0;
          secondStart = now;
        }
      });

      session.StartCapture();
      while (running_ && IsWindow(hwnd)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }

      framePool.FrameArrived(token);
      session.Close();
      framePool.Close();
    } catch (...) {
      running_ = false;
    }
  });
}

void DesktopCapture::stop() {
  running_ = false;
  if (thread_.joinable()) thread_.join();
}
