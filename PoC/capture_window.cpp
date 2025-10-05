#include <condition_variable>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <iostream>
#include <mutex>
#include <windows.h>
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

// Helper to find window
struct WindowSearchData {
    const wchar_t *partial_title;
    HWND found_hwnd;
};

BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    auto *data = (WindowSearchData *)lParam;
    wchar_t title[256];

    if (GetWindowTextW(hwnd, title, 256) > 0) {
        if (wcsstr(title, data->partial_title) != nullptr && IsWindowVisible(hwnd)) {
            data->found_hwnd = hwnd;
            std::wcout << L"Found window: " << title << std::endl;
            return FALSE;
        }
    }
    return TRUE;
}

// Create D3D11 device
ComPtr<ID3D11Device> CreateD3DDevice() {
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
D3D::IDirect3DDevice CreateDirect3DDevice(ComPtr<ID3D11Device> d3dDevice) {
    ComPtr<IDXGIDevice> dxgiDevice;
    d3dDevice.As(&dxgiDevice);

    winrt::com_ptr<::IInspectable> inspectable;
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(), inspectable.put()));

    return inspectable.as<D3D::IDirect3DDevice>();
}

// Get texture from surface
ComPtr<ID3D11Texture2D> GetTextureFromSurface(D3D::IDirect3DSurface surface) {
    auto access =
        surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    ComPtr<ID3D11Texture2D> texture;
    winrt::check_hresult(
        access->GetInterface(__uuidof(ID3D11Texture2D), (void **)texture.GetAddressOf()));
    return texture;
}

// Save texture to BMP
void SaveTextureToBMP(ComPtr<ID3D11Device> device, ComPtr<ID3D11Texture2D> texture,
                      const wchar_t *filename) {
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);

    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);

    // Create staging texture
    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> stagingTexture;
    device->CreateTexture2D(&stagingDesc, nullptr, &stagingTexture);

    context->CopyResource(stagingTexture.Get(), texture.Get());

    D3D11_MAPPED_SUBRESOURCE mapped;
    context->Map(stagingTexture.Get(), 0, D3D11_MAP_READ, 0, &mapped);

    BITMAPFILEHEADER fileHeader = {};
    BITMAPINFOHEADER infoHeader = {};

    infoHeader.biSize = sizeof(BITMAPINFOHEADER);
    infoHeader.biWidth = desc.Width;
    infoHeader.biHeight = -(LONG)desc.Height;
    infoHeader.biPlanes = 1;
    infoHeader.biBitCount = 32;
    infoHeader.biCompression = BI_RGB;

    DWORD imageSize = desc.Width * desc.Height * 4;
    fileHeader.bfType = 0x4D42;
    fileHeader.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    fileHeader.bfSize = fileHeader.bfOffBits + imageSize;

    HANDLE hFile = CreateFileW(filename, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(hFile, &fileHeader, sizeof(fileHeader), &written, nullptr);
        WriteFile(hFile, &infoHeader, sizeof(infoHeader), &written, nullptr);

        for (UINT y = 0; y < desc.Height; y++) {
            BYTE *row = (BYTE *)mapped.pData + y * mapped.RowPitch;
            WriteFile(hFile, row, desc.Width * 4, &written, nullptr);
        }

        CloseHandle(hFile);
        std::wcout << L"Saved: " << filename << std::endl;
    }

    context->Unmap(stagingTexture.Get(), 0);
}

int main() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    // Find window
    WindowSearchData search_data = {L"ELDEN RING", nullptr};
    EnumWindows(EnumWindowsProc, (LPARAM)&search_data);

    HWND game_window = search_data.found_hwnd;
    if (!game_window) {
        std::cerr << "Window not found!" << std::endl;
        return 1;
    }

    // Bring to foreground
    std::cout << "Bringing to foreground..." << std::endl;
    // SetForegroundWindow(game_window);
    // Sleep(100);

    try {
        // Create devices
        auto d3dDevice = CreateD3DDevice();
        auto device = CreateDirect3DDevice(d3dDevice);

        // Create capture item
        auto interop =
            winrt::get_activation_factory<WGC::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        WGC::GraphicsCaptureItem item{nullptr};
        winrt::check_hresult(interop->CreateForWindow(
            game_window, winrt::guid_of<WGC::GraphicsCaptureItem>(), winrt::put_abi(item)));

        auto size = item.Size();
        std::cout << "Capture size: " << size.Width << "x" << size.Height << std::endl;

        // Create frame pool
        auto framePool = WGC::Direct3D11CaptureFramePool::CreateFreeThreaded(
            device, DX::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);

        // Create session
        auto session = framePool.CreateCaptureSession(item);

        // Set cursor capture off for cleaner capture
        session.IsCursorCaptureEnabled(false);

        // Capture frame
        bool captured = false;
        ComPtr<ID3D11Texture2D> capturedTexture;
        std::mutex mtx;
        std::condition_variable cv;

        auto token = framePool.FrameArrived([&](auto &&sender, auto &&) {
            std::lock_guard<std::mutex> lock(mtx);
            if (!captured) {
                auto frame = sender.TryGetNextFrame();
                if (frame) {
                    auto surface = frame.Surface();
                    capturedTexture = GetTextureFromSurface(surface);
                    captured = true;
                    cv.notify_one();
                }
            }
        });

        // Start capture
        session.StartCapture();
        std::cout << "Capturing..." << std::endl;

        // Wait for frame with timeout
        {
            std::unique_lock<std::mutex> lock(mtx);
            if (cv.wait_for(lock, std::chrono::seconds(5), [&] { return captured; })) {
                SaveTextureToBMP(d3dDevice, capturedTexture, L"elden_ring_capture.bmp");
                std::cout << "Success!" << std::endl;
            } else {
                std::cout << "Timeout - no frame captured" << std::endl;
            }
        }

        framePool.FrameArrived(token);
        session.Close();
        framePool.Close();

    } catch (winrt::hresult_error const &ex) {
        std::wcerr << L"Error: " << ex.message().c_str() << std::endl;
        return 1;
    }

    return 0;
}