#include <condition_variable>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <iostream>
#include <mutex>
#include <windows.h>
#include <wrl/client.h>

// Include WinRT headers AFTER windows.h
#include <spdlog/spdlog.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>

#pragma comment(lib, "windowsapp")
#pragma comment(lib, "d3d11")

#include "process_capture.h"

ProcessCapture::ProcessCapture() : frameCounter(0), lastReadFrameCounter(0) {}

ProcessCapture::~ProcessCapture() {}

using Microsoft::WRL::ComPtr;
namespace WGC = winrt::Windows::Graphics::Capture;
namespace WF = winrt::Windows::Foundation;
namespace DX = winrt::Windows::Graphics::DirectX;
namespace D3D = winrt::Windows::Graphics::DirectX::Direct3D11;

// Create D3D11 device
ComPtr<ID3D11Device> ProcessCapture::CreateD3DDevice() {
    ComPtr<ID3D11Device> device;
    D3D_FEATURE_LEVEL featureLevels[] = {D3D_FEATURE_LEVEL_11_0};

    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, featureLevels,
        ARRAYSIZE(featureLevels), D3D11_SDK_VERSION, device.GetAddressOf(), nullptr, nullptr);

    if (FAILED(hr)) {
        std::cerr << "Failed to create D3D11 device" << std::endl;
    }

    return device;
}

// Create WinRT device from D3D11
D3D::IDirect3DDevice ProcessCapture::CreateDirect3DDevice(ComPtr<ID3D11Device> d3dDevice) {
    ComPtr<IDXGIDevice> dxgiDevice;
    d3dDevice.As(&dxgiDevice);

    winrt::com_ptr<::IInspectable> inspectable;
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(), inspectable.put()));

    return inspectable.as<D3D::IDirect3DDevice>();
}

// Get texture from surface
ComPtr<ID3D11Texture2D> ProcessCapture::GetTextureFromSurface(D3D::IDirect3DSurface surface) {
    auto access =
        surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    ComPtr<ID3D11Texture2D> texture;
    winrt::check_hresult(
        access->GetInterface(__uuidof(ID3D11Texture2D), (void **)texture.GetAddressOf()));
    return texture;
}

bool ProcessCapture::Initialize(HWND processWindow) {
    this->processWindow = processWindow;
    d3dDevice = CreateD3DDevice();
    device = CreateDirect3DDevice(d3dDevice);

    auto interop =
        winrt::get_activation_factory<WGC::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    winrt::check_hresult(interop->CreateForWindow(
        processWindow, winrt::guid_of<WGC::GraphicsCaptureItem>(), winrt::put_abi(item)));

    auto size = item.Size();
    spdlog::info("Capture size: {}x{}", size.Width, size.Height);
    this->processWindowWidth = size.Width;
    this->processWindowHeight = size.Height;
    framePool = WGC::Direct3D11CaptureFramePool::CreateFreeThreaded(
        device, DX::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);

    this->session = framePool.CreateCaptureSession(item);

    // Keep updating latest frame in background
    this->framePool.FrameArrived([&](auto &&sender, auto &&) {
        auto frame = sender.TryGetNextFrame();
        if (frame) {
            std::lock_guard<std::mutex> lock(frameMutex);
            this->latestFrame = GetTextureFromSurface(frame.Surface());
            this->frameCounter++;
        }
    });

    session.StartCapture();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return true;
}

// Check if a new frame is available (not previously read)
bool ProcessCapture::IsNewFrameAvailable() {
    std::lock_guard<std::mutex> lock(frameMutex);
    return frameCounter > lastReadFrameCounter;
}

// Call this from gRPC handler
std::vector<uint8_t> ProcessCapture::GetPixelData() {
    std::lock_guard<std::mutex> lock(frameMutex);

    if (!this->latestFrame)
        return {};

    // Mark this frame as read
    lastReadFrameCounter = frameCounter;

    // Copy texture to CPU readable format
    D3D11_TEXTURE2D_DESC desc;
    this->latestFrame->GetDesc(&desc);

    ComPtr<ID3D11Texture2D> staging;
    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;

    this->d3dDevice->CreateTexture2D(&stagingDesc, nullptr, &staging);

    ComPtr<ID3D11DeviceContext> context;
    this->d3dDevice->GetImmediateContext(&context);
    context->CopyResource(staging.Get(), this->latestFrame.Get());

    D3D11_MAPPED_SUBRESOURCE mapped;
    context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);

    // Copy to vector
    std::vector<uint8_t> pixels(desc.Width * desc.Height * 4);
    for (UINT y = 0; y < desc.Height; y++) {
        memcpy(pixels.data() + y * desc.Width * 4, (uint8_t *)mapped.pData + y * mapped.RowPitch,
               desc.Width * 4);
    }

    context->Unmap(staging.Get(), 0);
    return pixels;
}

bool ProcessCapture::SaveBMP(const std::vector<uint8_t> &pixels, const char *filename) {
    spdlog::info("Saving BMP to {}, pixels size: {}", filename, pixels.size());
    if (pixels.size() != this->processWindowWidth * this->processWindowHeight * 4) {
        std::cerr << "Invalid pixel data size" << std::endl;
        return false;
    }

    BITMAPFILEHEADER fileHeader = {};
    BITMAPINFOHEADER infoHeader = {};

    infoHeader.biSize = sizeof(BITMAPINFOHEADER);
    infoHeader.biWidth = this->processWindowWidth;
    infoHeader.biHeight = -this->processWindowHeight; // Negative for top-down bitmap
    infoHeader.biPlanes = 1;
    infoHeader.biBitCount = 32; // BGRA format
    infoHeader.biCompression = BI_RGB;

    fileHeader.bfType = 0x4D42; // "BM"
    fileHeader.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    fileHeader.bfSize =
        fileHeader.bfOffBits + (this->processWindowWidth * this->processWindowHeight * 4);

    HANDLE hFile = CreateFileA(filename, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        std::cerr << "Failed to create file" << std::endl;
        return false;
    }

    DWORD written;
    WriteFile(hFile, &fileHeader, sizeof(fileHeader), &written, nullptr);
    WriteFile(hFile, &infoHeader, sizeof(infoHeader), &written, nullptr);
    WriteFile(hFile, pixels.data(), pixels.size(), &written, nullptr);

    CloseHandle(hFile);
    return true;
}
