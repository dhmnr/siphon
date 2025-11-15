#pragma once

#include "process_capture.h"
#include <atomic>
#include <chrono>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#include <windows.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

// Frame data structure shared with subscribers
struct CapturedFrame {
    std::vector<uint8_t> pixels; // BGRA format
    int64_t timestampUs;
    int32_t width;
    int32_t height;
    int32_t frameNumber;
};

// Callback type for frame subscribers
using FrameCallback = std::function<void(const CapturedFrame &)>;

// Thread-safe frame broadcaster that captures once and distributes to multiple consumers
class FrameBroadcaster {
  private:
    // Capture infrastructure
    ProcessCapture *fallbackCapture_; // WGC fallback
    ComPtr<ID3D11Device> d3dDevice_;
    ComPtr<ID3D11DeviceContext> d3dContext_;
    ComPtr<IDXGIOutputDuplication> dxgiDuplication_;
    ComPtr<ID3D11Texture2D> stagingTexture_;
    int captureWidth_;
    int captureHeight_;
    HWND targetWindow_;

    // Broadcasting state
    std::atomic<bool> isRunning_;
    std::atomic<bool> shouldStop_;
    std::thread captureThread_;

    // Subscribers
    std::mutex subscribersMutex_;
    std::unordered_map<uint64_t, FrameCallback> subscribers_;
    std::atomic<uint64_t> nextSubscriberId_;

    // Frame statistics
    std::atomic<int32_t> currentFrame_;
    std::atomic<int64_t> lastFrameTimestampUs_;

    // Private methods
    void CaptureLoop();
    bool InitializeDXGICapture(HWND window);
    void CleanupDXGICapture();
    std::vector<uint8_t> CaptureFrameDXGI();
    void BroadcastFrame(const CapturedFrame &frame);

  public:
    FrameBroadcaster(ProcessCapture *fallbackCapture);
    ~FrameBroadcaster();

    // Start/stop broadcasting
    bool Start(HWND window);
    void Stop();
    bool IsRunning() const { return isRunning_; }

    // Subscribe to frames (returns subscription ID)
    uint64_t Subscribe(FrameCallback callback);
    void Unsubscribe(uint64_t subscriptionId);

    // Get current stats
    int32_t GetCurrentFrame() const { return currentFrame_; }
    int64_t GetLastFrameTimestamp() const { return lastFrameTimestampUs_; }
};

