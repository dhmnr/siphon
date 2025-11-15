#include "frame_broadcaster.h"
#include <spdlog/spdlog.h>
#include <thread>

FrameBroadcaster::FrameBroadcaster(ProcessCapture *fallbackCapture)
    : fallbackCapture_(fallbackCapture), isRunning_(false), shouldStop_(false),
      captureWidth_(0), captureHeight_(0), targetWindow_(nullptr), nextSubscriberId_(1),
      currentFrame_(0), lastFrameTimestampUs_(0) {}

FrameBroadcaster::~FrameBroadcaster() {
    Stop();
}

bool FrameBroadcaster::Start(HWND window) {
    if (isRunning_) {
        spdlog::warn("FrameBroadcaster already running");
        return false;
    }

    targetWindow_ = window;
    shouldStop_ = false;

    // Try to initialize DXGI Desktop Duplication first
    if (!InitializeDXGICapture(window)) {
        spdlog::warn("Failed to initialize DXGI capture, will use WGC fallback");
    }

    // Start capture thread
    captureThread_ = std::thread(&FrameBroadcaster::CaptureLoop, this);
    isRunning_ = true;

    spdlog::info("FrameBroadcaster started");
    return true;
}

void FrameBroadcaster::Stop() {
    if (!isRunning_) {
        return;
    }

    shouldStop_ = true;
    if (captureThread_.joinable()) {
        captureThread_.join();
    }

    CleanupDXGICapture();
    isRunning_ = false;

    spdlog::info("FrameBroadcaster stopped");
}

uint64_t FrameBroadcaster::Subscribe(FrameCallback callback) {
    std::lock_guard<std::mutex> lock(subscribersMutex_);
    uint64_t id = nextSubscriberId_++;
    subscribers_[id] = callback;
    spdlog::info("Frame subscriber added: ID={}", id);
    return id;
}

void FrameBroadcaster::Unsubscribe(uint64_t subscriptionId) {
    std::lock_guard<std::mutex> lock(subscribersMutex_);
    subscribers_.erase(subscriptionId);
    spdlog::info("Frame subscriber removed: ID={}", subscriptionId);
}

void FrameBroadcaster::CaptureLoop() {
    if (dxgiDuplication_) {
        spdlog::info("FrameBroadcaster: Using DXGI Desktop Duplication");
    } else {
        spdlog::info("FrameBroadcaster: Using Windows Graphics Capture fallback");
    }

    auto lastCaptureTime = std::chrono::high_resolution_clock::now();
    std::vector<uint8_t> lastFrame; // Reuse last frame if DXGI has no new frame
    int loopCounter = 0;

    while (!shouldStop_) {
        loopCounter++;

        // Process Windows messages
        MSG msg;
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        // Capture every 4th iteration (~15fps at 60Hz polling)
        if (loopCounter % 4 != 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        auto wallClockTime = std::chrono::system_clock::now();
        int64_t timestampUs =
            std::chrono::duration_cast<std::chrono::microseconds>(wallClockTime.time_since_epoch())
                .count();

        // Capture frame
        std::vector<uint8_t> pixels;
        int32_t width = 0;
        int32_t height = 0;

        if (dxgiDuplication_) {
            pixels = CaptureFrameDXGI();
            width = captureWidth_;
            height = captureHeight_;

            // Keep last frame if DXGI returns empty (no update)
            if (!pixels.empty()) {
                lastFrame = pixels;
            } else if (!lastFrame.empty()) {
                pixels = lastFrame;
            }
        } else if (fallbackCapture_) {
            pixels = fallbackCapture_->GetPixelData();
            width = fallbackCapture_->processWindowWidth;
            height = fallbackCapture_->processWindowHeight;
        }

        // Broadcast to subscribers if we have valid frame data
        if (!pixels.empty() && width > 0 && height > 0) {
            CapturedFrame frame;
            frame.pixels = std::move(pixels);
            frame.timestampUs = timestampUs;
            frame.width = width;
            frame.height = height;
            frame.frameNumber = currentFrame_++;

            lastFrameTimestampUs_ = timestampUs;

            BroadcastFrame(frame);
        }

        lastCaptureTime = std::chrono::high_resolution_clock::now();
    }
}

void FrameBroadcaster::BroadcastFrame(const CapturedFrame &frame) {
    std::lock_guard<std::mutex> lock(subscribersMutex_);

    // Send frame to all subscribers (non-blocking)
    for (const auto &[id, callback] : subscribers_) {
        try {
            callback(frame);
        } catch (const std::exception &e) {
            spdlog::error("Exception in frame subscriber {}: {}", id, e.what());
        }
    }
}

bool FrameBroadcaster::InitializeDXGICapture(HWND window) {
    // Get window rect to determine which monitor
    RECT windowRect;
    if (!GetWindowRect(window, &windowRect)) {
        spdlog::error("Failed to get window rect");
        return false;
    }

    // Find monitor containing window
    HMONITOR hMonitor = MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO monitorInfo = {sizeof(MONITORINFO)};
    if (!GetMonitorInfo(hMonitor, &monitorInfo)) {
        spdlog::error("Failed to get monitor info");
        return false;
    }

    // Create D3D11 device
    D3D_FEATURE_LEVEL featureLevel;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                   D3D11_SDK_VERSION, &d3dDevice_, &featureLevel, &d3dContext_);
    if (FAILED(hr)) {
        spdlog::error("Failed to create D3D11 device: 0x{:X}", static_cast<unsigned int>(hr));
        return false;
    }

    // Get DXGI device and adapter
    ComPtr<IDXGIDevice> dxgiDevice;
    hr = d3dDevice_.As(&dxgiDevice);
    if (FAILED(hr)) {
        spdlog::error("Failed to get DXGI device: 0x{:X}", static_cast<unsigned int>(hr));
        return false;
    }

    ComPtr<IDXGIAdapter> dxgiAdapter;
    hr = dxgiDevice->GetAdapter(&dxgiAdapter);
    if (FAILED(hr)) {
        spdlog::error("Failed to get DXGI adapter: 0x{:X}", static_cast<unsigned int>(hr));
        return false;
    }

    // Find output (monitor) that contains the window
    ComPtr<IDXGIOutput> targetOutput;
    UINT outputIdx = 0;
    ComPtr<IDXGIOutput> output;

    while (dxgiAdapter->EnumOutputs(outputIdx++, &output) != DXGI_ERROR_NOT_FOUND) {
        DXGI_OUTPUT_DESC desc;
        output->GetDesc(&desc);

        // Check if this output's desktop coordinates match our monitor
        if (desc.DesktopCoordinates.left == monitorInfo.rcMonitor.left &&
            desc.DesktopCoordinates.top == monitorInfo.rcMonitor.top &&
            desc.DesktopCoordinates.right == monitorInfo.rcMonitor.right &&
            desc.DesktopCoordinates.bottom == monitorInfo.rcMonitor.bottom) {
            targetOutput = output;
            // Convert WCHAR to char for logging
            char deviceNameMB[128];
            WideCharToMultiByte(CP_UTF8, 0, desc.DeviceName, -1, deviceNameMB, sizeof(deviceNameMB), NULL, NULL);
            spdlog::info("Found target output: {}", deviceNameMB);
            break;
        }
        output.Reset();
    }

    if (!targetOutput) {
        spdlog::error("Could not find output matching window monitor");
        return false;
    }

    // Create desktop duplication
    ComPtr<IDXGIOutput1> output1;
    hr = targetOutput.As(&output1);
    if (FAILED(hr)) {
        spdlog::error("Failed to get IDXGIOutput1: 0x{:X}", static_cast<unsigned int>(hr));
        return false;
    }

    hr = output1->DuplicateOutput(d3dDevice_.Get(), &dxgiDuplication_);
    if (FAILED(hr)) {
        spdlog::error("Failed to duplicate output: 0x{:X}", static_cast<unsigned int>(hr));
        return false;
    }

    // Get duplication description
    DXGI_OUTDUPL_DESC duplDesc;
    dxgiDuplication_->GetDesc(&duplDesc);
    captureWidth_ = duplDesc.ModeDesc.Width;
    captureHeight_ = duplDesc.ModeDesc.Height;

    // Create staging texture
    D3D11_TEXTURE2D_DESC stagingDesc = {};
    stagingDesc.Width = captureWidth_;
    stagingDesc.Height = captureHeight_;
    stagingDesc.MipLevels = 1;
    stagingDesc.ArraySize = 1;
    stagingDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    hr = d3dDevice_->CreateTexture2D(&stagingDesc, nullptr, &stagingTexture_);
    if (FAILED(hr)) {
        spdlog::error("Failed to create staging texture: 0x{:X}", static_cast<unsigned int>(hr));
        return false;
    }

    spdlog::info("DXGI Desktop Duplication initialized: {}x{}", captureWidth_, captureHeight_);
    return true;
}

void FrameBroadcaster::CleanupDXGICapture() {
    if (dxgiDuplication_) {
        dxgiDuplication_->ReleaseFrame();
    }
    stagingTexture_.Reset();
    dxgiDuplication_.Reset();
    d3dContext_.Reset();
    d3dDevice_.Reset();
}

std::vector<uint8_t> FrameBroadcaster::CaptureFrameDXGI() {
    if (!dxgiDuplication_) {
        return {};
    }

    DXGI_OUTDUPL_FRAME_INFO frameInfo;
    ComPtr<IDXGIResource> desktopResource;

    // Try to acquire next frame (0ms timeout for polling)
    HRESULT hr = dxgiDuplication_->AcquireNextFrame(0, &frameInfo, &desktopResource);

    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        // No new frame available
        return {};
    }

    if (FAILED(hr)) {
        if (hr == DXGI_ERROR_ACCESS_LOST) {
            spdlog::warn("DXGI access lost, attempting to reinitialize");
            CleanupDXGICapture();
            InitializeDXGICapture(targetWindow_);
        }
        return {};
    }

    // Get texture from resource
    ComPtr<ID3D11Texture2D> frameTexture;
    hr = desktopResource.As(&frameTexture);
    if (FAILED(hr)) {
        dxgiDuplication_->ReleaseFrame();
        return {};
    }

    // Copy to staging texture
    d3dContext_->CopyResource(stagingTexture_.Get(), frameTexture.Get());

    // Map staging texture to read pixels
    D3D11_MAPPED_SUBRESOURCE mappedResource;
    hr = d3dContext_->Map(stagingTexture_.Get(), 0, D3D11_MAP_READ, 0, &mappedResource);
    if (FAILED(hr)) {
        dxgiDuplication_->ReleaseFrame();
        return {};
    }

    // Copy pixel data
    std::vector<uint8_t> pixels(captureWidth_ * captureHeight_ * 4);
    uint8_t *src = static_cast<uint8_t *>(mappedResource.pData);
    uint8_t *dst = pixels.data();

    for (int y = 0; y < captureHeight_; ++y) {
        memcpy(dst + y * captureWidth_ * 4, src + y * mappedResource.RowPitch, captureWidth_ * 4);
    }

    d3dContext_->Unmap(stagingTexture_.Get(), 0);
    dxgiDuplication_->ReleaseFrame();

    return pixels;
}

