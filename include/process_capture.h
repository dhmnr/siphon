#pragma once

#include <d3d11.h>
#include <dxgi1_2.h>
#include <mutex>
#include <string>
#include <vector>
#include <windows.h>

#include <condition_variable>
#include <iostream>
#include <wrl/client.h>

// Include WinRT headers AFTER windows.h
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>

#pragma comment(lib, "windowsapp")
#pragma comment(lib, "d3d11")

using Microsoft::WRL::ComPtr;
namespace WGC = winrt::Windows::Graphics::Capture;
namespace WF = winrt::Windows::Foundation;
namespace DX = winrt::Windows::Graphics::DirectX;
namespace D3D = winrt::Windows::Graphics::DirectX::Direct3D11;

class ProcessCapture {
  private:
    ComPtr<ID3D11Device> d3dDevice;
    D3D::IDirect3DDevice device{nullptr};
    WGC::GraphicsCaptureItem item{nullptr};
    WGC::Direct3D11CaptureFramePool framePool{nullptr};
    WGC::GraphicsCaptureSession session{nullptr};

    ComPtr<ID3D11Texture2D> latestFrame;
    std::mutex frameMutex;
    HWND processWindow;
    uint64_t frameCounter;
    uint64_t lastReadFrameCounter;

  public:
    int processWindowWidth;
    int processWindowHeight;
    ProcessCapture();
    ~ProcessCapture();
    ComPtr<ID3D11Device> CreateD3DDevice();
    D3D::IDirect3DDevice CreateDirect3DDevice(ComPtr<ID3D11Device> d3dDevice);
    ComPtr<ID3D11Texture2D> GetTextureFromSurface(D3D::IDirect3DSurface surface);
    bool Initialize(HWND processWindow);
    std::vector<uint8_t> GetPixelData();
    bool IsNewFrameAvailable();
    bool SaveBMP(const std::vector<uint8_t> &pixels, const char *filename);
};
