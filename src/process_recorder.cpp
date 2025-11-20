#include "process_recorder.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <random>
#include <spdlog/spdlog.h>
#include <sstream>

namespace fs = std::filesystem;

ProcessRecorder::ProcessRecorder(ProcessCapture *capture, ProcessMemory *memory,
                                 ProcessInput *input, FrameBroadcaster *frameBroadcaster)
    : capture_(capture), memory_(memory), input_(input), frameBroadcaster_(frameBroadcaster),
      isRecording_(false), shouldStop_(false), currentFrame_(0), droppedFrames_(0),
      currentLatencyMs_(0.0), maxDurationSeconds_(0), frameSubscriptionId_(0), hasNewFrame_(false) {

    // Initialize stats
    stats_.totalFrames = 0;
    stats_.droppedFrames = 0;
    stats_.averageLatencyMs = 0.0;
    stats_.maxLatencyMs = 0.0;
    stats_.minLatencyMs = 999999.0;
    stats_.startTimeMs = 0;
    stats_.endTimeMs = 0;
    stats_.actualDurationSeconds = 0.0;
    stats_.actualFps = 0.0;

    // Create input event logger
    inputLogger_ = std::make_unique<InputEventLogger>();

    // Create video encoder
    videoEncoder_ = std::make_unique<VideoEncoder>();
}

ProcessRecorder::~ProcessRecorder() {
    if (isRecording_) {
        shouldStop_ = true;
        if (recordingThread_.joinable()) {
            recordingThread_.join();
        }
    }

    // Unsubscribe from frame broadcaster
    if (frameBroadcaster_ && frameSubscriptionId_ != 0) {
        frameBroadcaster_->Unsubscribe(frameSubscriptionId_);
    }

    // Stop input logger if running
    if (inputLogger_ && inputLogger_->IsLogging()) {
        inputLogger_->StopLogging();
    }

    // Stop video encoder if running
    if (videoEncoder_) {
        videoEncoder_->Finalize();
    }

    // Close memory file if open
    if (memoryFile_.is_open()) {
        memoryFile_.close();
    }
}

std::string ProcessRecorder::GenerateSessionId() {
    auto now = std::chrono::system_clock::now();
    auto timestamp =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(1000, 9999);

    std::stringstream ss;
    ss << "rec_" << timestamp << "_" << dis(gen);
    return ss.str();
}

bool ProcessRecorder::CreateOutputDirectories() {
    try {
        // Create base output directory if it doesn't exist
        fs::path basePath(outputDirectory_);
        if (!fs::exists(basePath)) {
            fs::create_directories(basePath);
        }

        // Create session-specific subdirectory
        fs::path sessionPath = basePath / sessionId_;
        if (!fs::exists(sessionPath)) {
            fs::create_directories(sessionPath);
        }

        // Create frames subdirectory within session folder
        fs::path framesPath = sessionPath / "frames";
        if (!fs::exists(framesPath)) {
            fs::create_directories(framesPath);
        }

        spdlog::info("Created output directories at: {}", sessionPath.string());
        return true;
    } catch (const std::exception &e) {
        spdlog::error("Failed to create output directories: {}", e.what());
        return false;
    }
}

bool ProcessRecorder::StartRecording(const std::vector<std::string> &attributeNames,
                                     const std::string &outputDirectory, int maxDurationSeconds) {
    if (isRecording_) {
        spdlog::warn("Recording already in progress");
        return false;
    }

    if (!capture_ || !memory_) {
        spdlog::error("Capture or Memory subsystem not initialized");
        return false;
    }

    // Store configuration
    attributeNames_ = attributeNames;
    outputDirectory_ = outputDirectory;
    maxDurationSeconds_ = maxDurationSeconds;
    sessionId_ = GenerateSessionId();

    // Create output directories
    if (!CreateOutputDirectories()) {
        return false;
    }

    // Reset statistics
    currentFrame_ = 0;
    droppedFrames_ = 0;
    currentLatencyMs_ = 0.0;

    stats_.totalFrames = 0;
    stats_.droppedFrames = 0;
    stats_.averageLatencyMs = 0.0;
    stats_.maxLatencyMs = 0.0;
    stats_.minLatencyMs = 999999.0;
    stats_.startTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();

    // Recreate video encoder if it was used before (to allow multiple recording sessions)
    if (videoEncoder_) {
        videoEncoder_.reset();
    }
    videoEncoder_ = std::make_unique<VideoEncoder>();

    // Initialize video encoder using capture dimensions
    try {
        std::string videoPath = (fs::path(outputDirectory_) / sessionId_ / "video.mp4").string();
        int width = capture_->processWindowWidth;
        int height = capture_->processWindowHeight;

        if (!videoEncoder_->Initialize(videoPath, width, height, 60)) {
            spdlog::error("Failed to initialize video encoder");
            return false;
        }
        spdlog::info("Initialized video encoder (H.264 CRF-20): {}", videoPath);
    } catch (const std::exception &e) {
        spdlog::error("Failed to initialize video encoder: {}", e.what());
        return false;
    }

    // Initialize memory data CSV file
    if (!attributeNames_.empty()) {
        std::string memoryPath =
            (fs::path(outputDirectory_) / sessionId_ / "memory_data.csv").string();
        memoryFile_.open(memoryPath, std::ios::out | std::ios::trunc);
        if (!memoryFile_.is_open()) {
            spdlog::error("Failed to open memory data file: {}", memoryPath);
            return false;
        }
        WriteMemoryHeader();
        spdlog::info("Initialized memory data CSV: {}", memoryPath);
    }

    // Initialize performance data CSV file
    std::string perfPath = (fs::path(outputDirectory_) / sessionId_ / "perf_data.csv").string();
    perfFile_.open(perfPath, std::ios::out | std::ios::trunc);
    if (!perfFile_.is_open()) {
        spdlog::error("Failed to open perf data file: {}", perfPath);
        return false;
    }
    WritePerfHeader();
    spdlog::info("Initialized perf data CSV: {}", perfPath);

    // Recreate input logger if it was used before (to allow multiple recording sessions)
    if (inputLogger_) {
        if (inputLogger_->IsLogging()) {
            inputLogger_->StopLogging();
        }
        inputLogger_.reset();
    }
    inputLogger_ = std::make_unique<InputEventLogger>();

    // Start input event logger (independent of video recording)
    std::string inputLogPath = (fs::path(outputDirectory_) / sessionId_ / "inputs.csv").string();
    if (!inputLogger_->StartLogging(inputLogPath)) {
        spdlog::error("Failed to start input event logger");
        return false;
    }

    // Unsubscribe from previous frame broadcaster subscription if any
    if (frameBroadcaster_ && frameSubscriptionId_ != 0) {
        frameBroadcaster_->Unsubscribe(frameSubscriptionId_);
        frameSubscriptionId_ = 0;
        spdlog::info("Unsubscribed from previous frame broadcaster session");
    }

    // Start recording threads
    shouldStop_ = false;
    isRecording_ = true;

    recordingThread_ = std::thread(&ProcessRecorder::RecordingLoop, this);

    // Start memory reading thread (runs independently)
    if (!attributeNames_.empty()) {
        memoryThread_ = std::thread(&ProcessRecorder::MemoryReadingLoop, this);
    }

    spdlog::info("Recording started - Session ID: {}", sessionId_);
    spdlog::info("Output directory: {}", outputDirectory_);
    spdlog::info("Attributes to record: {}", attributeNames_.size());

    return true;
}

bool ProcessRecorder::StopRecording(RecordingStats &stats) {
    if (!isRecording_) {
        spdlog::warn("No recording in progress");
        return false;
    }

    spdlog::info("Stopping recording...");
    shouldStop_ = true;

    // Wait for recording threads to finish
    if (recordingThread_.joinable()) {
        recordingThread_.join();
    }
    if (memoryThread_.joinable()) {
        memoryThread_.join();
    }

    isRecording_ = false;

    // Stop input logger
    if (inputLogger_ && inputLogger_->IsLogging()) {
        inputLogger_->StopLogging();
    }

    // Finalize video encoder (waits for queue to drain)
    if (videoEncoder_) {
        spdlog::info("Finalizing video encoder - queue size: {}", videoEncoder_->GetQueueSize());
        videoEncoder_->Finalize();
        spdlog::info("Video finalized - frames encoded: {}", videoEncoder_->GetFramesEncoded());
    }

    // Close memory file
    if (memoryFile_.is_open()) {
        memoryFile_.close();
    }

    // Close perf file
    if (perfFile_.is_open()) {
        perfFile_.close();
    }

    // Update final statistics
    stats_.endTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    stats_.totalFrames = currentFrame_;
    stats_.droppedFrames = droppedFrames_;

    // Calculate actual duration and FPS
    stats_.actualDurationSeconds = (stats_.endTimeMs - stats_.startTimeMs) / 1000.0;
    stats_.actualFps = stats_.totalFrames / stats_.actualDurationSeconds;

    stats = stats_;

    spdlog::info("Recording stopped - Total frames: {}, Dropped: {}, Avg latency: {:.2f}ms",
                 stats.totalFrames, stats.droppedFrames, stats.averageLatencyMs);

    return true;
}

bool ProcessRecorder::GetStatus(bool &isRecording, int &currentFrame, double &elapsedTime,
                                double &currentLatency, int &droppedFrames) {
    isRecording = isRecording_;
    currentFrame = currentFrame_;
    droppedFrames = droppedFrames_;
    currentLatency = currentLatencyMs_;

    if (isRecording_) {
        auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
        elapsedTime = (now - stats_.startTimeMs) / 1000.0;
    } else {
        elapsedTime = 0.0;
    }

    return true;
}

void ProcessRecorder::WriteMemoryHeader() {
    if (!memoryFile_.is_open())
        return;

    std::lock_guard<std::mutex> lock(memoryMutex_);
    memoryFile_ << "timestamp_us";
    for (const auto &attrName : attributeNames_) {
        memoryFile_ << "," << attrName;
    }
    memoryFile_ << "\n";
    memoryFile_.flush();
}

void ProcessRecorder::WriteMemoryFrame(const MemoryFrameData &data) {
    if (!memoryFile_.is_open())
        return;

    std::lock_guard<std::mutex> lock(memoryMutex_);
    memoryFile_ << data.timestampUs;
    for (const auto &attrName : attributeNames_) {
        auto it = data.memoryData.find(attrName);
        if (it != data.memoryData.end()) {
            memoryFile_ << "," << it->second;
        } else {
            memoryFile_ << ",0";
        }
    }
    memoryFile_ << "\n";

    // Flush every 60 frames
    if (currentFrame_ % 60 == 0) {
        memoryFile_.flush();
    }
}

void ProcessRecorder::WritePerfHeader() {
    if (!perfFile_.is_open())
        return;

    std::lock_guard<std::mutex> lock(perfMutex_);
    perfFile_ << "frame,timestamp_us,total_ms,capture_ms,fps,queue_size,dropped_frames\n";
    perfFile_.flush();
}

void ProcessRecorder::WritePerfData(int frame, int64_t timestampUs, double totalMs,
                                    double captureMs, double fps, size_t queueSize, int dropped) {
    if (!perfFile_.is_open())
        return;

    std::lock_guard<std::mutex> lock(perfMutex_);
    perfFile_ << frame << "," << timestampUs << "," << totalMs << "," << captureMs << "," << fps
              << "," << queueSize << "," << dropped << "\n";

    // Flush every 60 frames
    if (frame % 60 == 0) {
        perfFile_.flush();
    }
}

void ProcessRecorder::RecordingLoop() {
    spdlog::info("Recording loop started - receiving frames from FrameBroadcaster (~15fps)");

    // Subscribe to frame broadcaster
    if (!frameBroadcaster_) {
        spdlog::error("FrameBroadcaster not available!");
        return;
    }

    auto frameCallback = [this](const CapturedFrame &frame) {
        std::lock_guard<std::mutex> lock(frameMutex_);
        latestFrame_ = frame;
        hasNewFrame_ = true;
    };

    frameSubscriptionId_ = frameBroadcaster_->Subscribe(frameCallback);
    spdlog::info("Subscribed to FrameBroadcaster with ID: {}", frameSubscriptionId_);

    while (!shouldStop_) {
        auto frameStartTime = std::chrono::high_resolution_clock::now();

        // Check max duration
        if (maxDurationSeconds_ > 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                               frameStartTime - std::chrono::high_resolution_clock::time_point(
                                                    std::chrono::milliseconds(stats_.startTimeMs)))
                               .count();
            if (elapsed >= maxDurationSeconds_) {
                spdlog::info("Max duration reached, stopping recording");
                break;
            }
        }

        // Process Windows messages for keyboard hook to work
        MSG msg;
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        // Wait for new frame from broadcaster
        if (!hasNewFrame_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        // Get frame data
        frameMutex_.lock();
        CapturedFrame frame = latestFrame_;
        hasNewFrame_ = false;
        frameMutex_.unlock();

        auto captureStart = std::chrono::high_resolution_clock::now();

        // Queue frame for video encoding
        EncoderFrame videoFrame;
        videoFrame.pixels = std::move(frame.pixels);
        videoFrame.timestampUs = frame.timestampUs;
        videoFrame.width = frame.width;
        videoFrame.height = frame.height;
        videoEncoder_->EncodeFrame(std::move(videoFrame));

        auto captureEnd = std::chrono::high_resolution_clock::now();
        double frameCaptureMs =
            std::chrono::duration<double, std::milli>(captureEnd - captureStart).count();

        // Calculate total frame time
        auto frameEndTime = std::chrono::high_resolution_clock::now();
        double totalMs =
            std::chrono::duration<double, std::milli>(frameEndTime - frameStartTime).count();

        // Update statistics
        currentLatencyMs_ = totalMs;
        if (totalMs > stats_.maxLatencyMs) {
            stats_.maxLatencyMs = totalMs;
        }
        if (totalMs < stats_.minLatencyMs) {
            stats_.minLatencyMs = totalMs;
        }

        // Accumulate average latency
        stats_.averageLatencyMs =
            (stats_.averageLatencyMs * currentFrame_ + totalMs) / (currentFrame_ + 1);

        // Check if we exceeded frame time budget (15fps = 66.67ms)
        if (totalMs > 66.67) {
            droppedFrames_++;
        }

        currentFrame_++;

        // Write performance data to CSV
        auto currentTime = std::chrono::system_clock::now();
        auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(currentTime.time_since_epoch())
                .count() -
            stats_.startTimeMs;
        double actualFps = elapsed > 0 ? (currentFrame_ * 1000.0) / elapsed : 0.0;
        size_t videoQueueSize = videoEncoder_->GetQueueSize();

        WritePerfData(currentFrame_, frame.timestampUs, totalMs, frameCaptureMs, actualFps,
                      videoQueueSize, droppedFrames_);
    }

    // Unsubscribe from frame broadcaster
    if (frameBroadcaster_ && frameSubscriptionId_ != 0) {
        frameBroadcaster_->Unsubscribe(frameSubscriptionId_);
        frameSubscriptionId_ = 0;
    }

    spdlog::info("Recording loop stopped");
}

void ProcessRecorder::MemoryReadingLoop() {
    spdlog::info("Memory reading thread started");

    while (!shouldStop_) {
        auto wallClockTime = std::chrono::high_resolution_clock::now();
        int64_t timestampUs =
            std::chrono::duration_cast<std::chrono::microseconds>(wallClockTime.time_since_epoch())
                .count();

        // Read memory attributes
        MemoryFrameData memoryData;
        memoryData.timestampUs = timestampUs;

        for (const auto &attrName : attributeNames_) {
            try {
                ProcessAttribute attr = memory_->GetAttribute(attrName);
                std::string value;

                if (attr.AttributeType == "int") {
                    int32_t intVal = 0;
                    if (memory_->ExtractAttributeInt(attrName, intVal)) {
                        value = std::to_string(intVal);
                    }
                } else if (attr.AttributeType == "float") {
                    float floatVal = 0.0f;
                    if (memory_->ExtractAttributeFloat(attrName, floatVal)) {
                        value = std::to_string(floatVal);
                    }
                } else {
                    value = "0";
                }

                memoryData.memoryData[attrName] = value;
            } catch (const std::exception &e) {
                memoryData.memoryData[attrName] = "0";
            }
        }

        // Write memory data to CSV
        WriteMemoryFrame(memoryData);

        // Sleep for a bit to avoid hammering the process memory
        // Aiming for ~30 Hz memory sampling (independent of video capture)
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }

    spdlog::info("Memory reading thread stopped");
}

