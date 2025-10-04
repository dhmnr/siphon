#include <d3d11.h>
#include <dxgi1_2.h>
#include <iostream>
#include <vector>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>


#pragma comment(lib, "windowsapp")
#pragma comment(lib, "d3d11")
#pragma comment(lib, "dxgi")

using namespace winrt;
using namespace Windows::Graphics::Capture;
using namespace Windows::Graphics::DirectX;
using namespace Windows::Graphics::DirectX::Direct3D11;

// Create D3D11 device
com_ptr<ID3D11Device> CreateD3DDevice() {
    com_ptr<ID3D11Device> device;
    D3D_FEATURE_LEVEL featureLevels[] = {D3D_FEATURE_LEVEL_11_0};

    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, featureLevels,
        ARRAYSIZE(featureLevels), D3D11_SDK_VERSION, device.put(), nullptr, nullptr);

    if (FAILED(hr)) {
        std::cerr << "Failed to create D3D11 device" << std::endl;
    }

    return device;
}

// Create WinRT Direct3D11 device from D3D11 device
IDirect3DDevice CreateDirect3DDevice(com_ptr<ID3D11Device> const &d3dDevice) {
    com_ptr<IDXGIDevice> dxgiDevice = d3dDevice.as<IDXGIDevice>();
    com_ptr<IInspectable> inspectable;

    check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), inspectable.put()));

    return inspectable.as<IDirect3DDevice>();
}

// Save texture to BMP file
void SaveTextureToBMP(com_ptr<ID3D11Device> device, com_ptr<ID3D11Texture2D> texture,
                      const wchar_t *filename) {
    com_ptr<ID3D11DeviceContext> context;
    device->GetImmediateContext(context.put());

    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);

    // Create staging texture
    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;

    com_ptr<ID3D11Texture2D> stagingTexture;
    check_hresult(device->CreateTexture2D(&stagingDesc, nullptr, stagingTexture.put()));

    // Copy texture to staging
    context->CopyResource(stagingTexture.get(), texture.get());

    // Map the texture
    D3D11_MAPPED_SUBRESOURCE mapped;
    check_hresult(context->Map(stagingTexture.get(), 0, D3D11_MAP_READ, 0, &mapped));

    // Prepare BMP headers
    BITMAPFILEHEADER fileHeader = {};
    BITMAPINFOHEADER infoHeader = {};

    infoHeader.biSize = sizeof(BITMAPINFOHEADER);
    infoHeader.biWidth = desc.Width;
    infoHeader.biHeight = -static_cast<LONG>(desc.Height); // Top-down
    infoHeader.biPlanes = 1;
    infoHeader.biBitCount = 32;
    infoHeader.biCompression = BI_RGB;

    DWORD imageSize = desc.Width * desc.Height * 4;
    fileHeader.bfType = 0x4D42; // "BM"
    fileHeader.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    fileHeader.bfSize = fileHeader.bfOffBits + imageSize;

    // Write to file
    HANDLE hFile = CreateFileW(filename, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(hFile, &fileHeader, sizeof(fileHeader), &written, nullptr);
        WriteFile(hFile, &infoHeader, sizeof(infoHeader), &written, nullptr);

        // Write pixel data row by row
        for (UINT y = 0; y < desc.Height; y++) {
            BYTE *row = (BYTE *)mapped.pData + y * mapped.RowPitch;
            WriteFile(hFile, row, desc.Width * 4, &written, nullptr);
        }

        CloseHandle(hFile);
        std::cout << "Saved to " << (char *)filename << std::endl;
    }

    context->Unmap(stagingTexture.get(), 0);
}

// Get ID3D11Texture2D from Direct3DSurface
com_ptr<ID3D11Texture2D> GetTextureFromSurface(Direct3D11::IDirect3DSurface const &surface) {
    auto access =
        surface.as<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    com_ptr<ID3D11Texture2D> texture;
    check_hresult(access->GetInterface(guid_of<ID3D11Texture2D>(), texture.put_void()));
    return texture;
}

int main() {
    init_apartment();

    try {
        // Find window (example: Notepad)
        HWND hwnd = FindWindowW(nullptr, L"Untitled - Notepad");
        if (!hwnd) {
            std::cerr << "Window not found! Open Notepad first." << std::endl;
            return 1;
        }

        // Create D3D device
        auto d3dDevice = CreateD3DDevice();
        auto device = CreateDirect3DDevice(d3dDevice);

        // Create capture item for window
        auto interop = get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        GraphicsCaptureItem item{nullptr};
        check_hresult(
            interop->CreateForWindow(hwnd, guid_of<GraphicsCaptureItem>(), put_abi(item)));

        // Create frame pool
        auto framePool = Direct3D11CaptureFramePool::Create(
            device, DirectXPixelFormat::B8G8R8A8UIntNormalized, 1, item.Size());

        // Create capture session
        auto session = framePool.CreateCaptureSession(item);

        // Capture a single frame
        bool frameCaptured = false;
        com_ptr<ID3D11Texture2D> capturedTexture;

        framePool.FrameArrived([&](auto &&sender, auto &&) {
            if (!frameCaptured) {
                auto frame = sender.TryGetNextFrame();
                if (frame) {
                    auto surface = frame.Surface();
                    capturedTexture = GetTextureFromSurface(surface);
                    frameCaptured = true;
                }
            }
        });

        // Start capture
        session.StartCapture();

        // Wait for frame
        std::cout << "Capturing window..." << std::endl;
        Sleep(100); // Give it time to capture

        // Process messages to trigger FrameArrived
        MSG msg;
        while (!frameCaptured && GetMessage(&msg, nullptr, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (frameCaptured)
                break;
        }

        if (capturedTexture) {
            SaveTextureToBMP(d3dDevice, capturedTexture, L"window_capture.bmp");
            std::cout << "Window captured successfully!" << std::endl;
        }

        session.Close();
        framePool.Close();

    } catch (hresult_error const &ex) {
        std::wcerr << L"Error: " << ex.message().c_str() << std::endl;
        return 1;
    }

    return 0;
}