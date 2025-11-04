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
                                 ProcessInput *input)
    : capture_(capture), memory_(memory), input_(input), isRecording_(false), shouldStop_(false),
      currentFrame_(0), droppedFrames_(0), currentLatencyMs_(0.0), maxDurationSeconds_(0),
      keystrokeContext_(nullptr) {

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
}

ProcessRecorder::~ProcessRecorder() {
    if (isRecording_) {
        shouldStop_ = true;
        if (recordingThread_.joinable()) {
            recordingThread_.join();
        }
        if (keystrokeThread_.joinable()) {
            keystrokeThread_.join();
        }
    }

    if (keystrokeContext_) {
        interception_destroy_context(keystrokeContext_);
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
    frameDataBuffer_.clear();

    stats_.totalFrames = 0;
    stats_.droppedFrames = 0;
    stats_.averageLatencyMs = 0.0;
    stats_.maxLatencyMs = 0.0;
    stats_.minLatencyMs = 999999.0;
    stats_.startTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();

    // Initialize keystroke monitoring
    keystrokeContext_ = interception_create_context();
    if (!keystrokeContext_) {
        spdlog::error("Failed to create Interception context for keystroke monitoring");
        return false;
    }

    // Set filter to capture all keyboard events
    interception_set_filter(keystrokeContext_, interception_is_keyboard,
                            INTERCEPTION_FILTER_KEY_DOWN | INTERCEPTION_FILTER_KEY_UP);

    // Start threads
    shouldStop_ = false;
    isRecording_ = true;

    keystrokeThread_ = std::thread(&ProcessRecorder::KeystrokeMonitorLoop, this);
    recordingThread_ = std::thread(&ProcessRecorder::RecordingLoop, this);

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

    // Wait for threads to finish
    if (recordingThread_.joinable()) {
        recordingThread_.join();
    }
    if (keystrokeThread_.joinable()) {
        keystrokeThread_.join();
    }

    isRecording_ = false;

    // Write metadata to file
    WriteMetadataToFile();

    // Update final statistics
    stats_.endTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    stats_.totalFrames = currentFrame_;
    stats_.droppedFrames = droppedFrames_;

    // Calculate actual duration and FPS
    stats_.actualDurationSeconds = (stats_.endTimeMs - stats_.startTimeMs) / 1000.0;
    stats_.actualFps = stats_.totalFrames / stats_.actualDurationSeconds;

    // Calculate average latency
    if (!frameDataBuffer_.empty()) {
        double totalLatency = 0.0;
        for (const auto &frame : frameDataBuffer_) {
            totalLatency += frame.totalMs;
        }
        stats_.averageLatencyMs = totalLatency / frameDataBuffer_.size();
    }

    stats = stats_;

    spdlog::info("Recording stopped - Total frames: {}, Dropped: {}, Avg latency: {:.2f}ms",
                 stats.totalFrames, stats.droppedFrames, stats.averageLatencyMs);

    // Cleanup
    if (keystrokeContext_) {
        interception_destroy_context(keystrokeContext_);
        keystrokeContext_ = nullptr;
    }

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

void ProcessRecorder::KeystrokeMonitorLoop() {
    spdlog::info("Keystroke monitor thread started");

    std::map<unsigned short, std::string> scancodeToString;

    // Build reverse map from scancode to key name
    for (const auto &pair : scancodeMap) {
        scancodeToString[pair.second] = pair.first;
    }

    while (!shouldStop_) {
        InterceptionDevice device;
        InterceptionStroke stroke;

        // Non-blocking receive with timeout
        if (interception_receive(keystrokeContext_,
                                 device = interception_wait_with_timeout(keystrokeContext_, 10),
                                 &stroke, 1) > 0) {

            if (interception_is_keyboard(device)) {
                InterceptionKeyStroke *keyStroke = (InterceptionKeyStroke *)&stroke;

                std::string keyName = "UNKNOWN";
                if (scancodeToString.find(keyStroke->code) != scancodeToString.end()) {
                    keyName = scancodeToString[keyStroke->code];
                }

                std::lock_guard<std::mutex> lock(keystrokeMutex_);

                if (keyStroke->state & INTERCEPTION_KEY_DOWN) {
                    // Key pressed
                    if (std::find(currentKeysPressed_.begin(), currentKeysPressed_.end(),
                                  keyName) == currentKeysPressed_.end()) {
                        currentKeysPressed_.push_back(keyName);
                    }
                } else if (keyStroke->state & INTERCEPTION_KEY_UP) {
                    // Key released
                    currentKeysPressed_.erase(std::remove(currentKeysPressed_.begin(),
                                                          currentKeysPressed_.end(), keyName),
                                              currentKeysPressed_.end());
                }
            }

            // Pass through the keystroke
            interception_send(keystrokeContext_, device, &stroke, 1);
        }
    }

    spdlog::info("Keystroke monitor thread stopped");
}

std::vector<std::string> ProcessRecorder::GetCurrentKeysPressed() {
    std::lock_guard<std::mutex> lock(keystrokeMutex_);
    return currentKeysPressed_;
}

void ProcessRecorder::RecordingLoop() {
    spdlog::info("Recording loop started - Target: 60fps (16.67ms per frame)");

    const auto targetFrameTime = std::chrono::microseconds(16667); // 60fps = 16.67ms
    auto lastFrameTime = std::chrono::high_resolution_clock::now();

    while (!shouldStop_) {
        auto frameStartTime = std::chrono::high_resolution_clock::now();
        auto wallClockTime = std::chrono::system_clock::now();

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

        // Check if a new frame is available from Windows Graphics Capture
        if (!capture_->IsNewFrameAvailable()) {
            // No new frame yet, sleep briefly and continue
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        FrameData frameData;
        frameData.frameNumber = currentFrame_;
        frameData.timestampMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(wallClockTime.time_since_epoch())
                .count();

        // Capture frame
        auto captureStart = std::chrono::high_resolution_clock::now();
        std::vector<uint8_t> pixels = capture_->GetPixelData();
        auto captureEnd = std::chrono::high_resolution_clock::now();
        frameData.frameCaptureMs =
            std::chrono::duration<double, std::milli>(captureEnd - captureStart).count();

        // Read memory attributes
        auto memoryStart = std::chrono::high_resolution_clock::now();
        ReadMemoryAttributes(frameData);
        auto memoryEnd = std::chrono::high_resolution_clock::now();
        frameData.memoryReadMs =
            std::chrono::duration<double, std::milli>(memoryEnd - memoryStart).count();

        // Capture keystrokes
        auto keystrokeStart = std::chrono::high_resolution_clock::now();
        CaptureKeystrokes(frameData);
        auto keystrokeEnd = std::chrono::high_resolution_clock::now();
        frameData.keystrokeCaptureMs =
            std::chrono::duration<double, std::milli>(keystrokeEnd - keystrokeStart).count();

        // Write frame to disk
        auto diskStart = std::chrono::high_resolution_clock::now();
        WriteFrameToDisk(pixels, frameData.frameNumber);
        auto diskEnd = std::chrono::high_resolution_clock::now();
        frameData.diskWriteMs =
            std::chrono::duration<double, std::milli>(diskEnd - diskStart).count();

        auto frameEndTime = std::chrono::high_resolution_clock::now();
        frameData.totalMs =
            std::chrono::duration<double, std::milli>(frameEndTime - frameStartTime).count();

        // Update statistics
        currentLatencyMs_ = frameData.totalMs;
        if (frameData.totalMs > stats_.maxLatencyMs) {
            stats_.maxLatencyMs = frameData.totalMs;
        }
        if (frameData.totalMs < stats_.minLatencyMs) {
            stats_.minLatencyMs = frameData.totalMs;
        }

        // Store frame data for metadata
        {
            std::lock_guard<std::mutex> lock(bufferMutex_);
            frameDataBuffer_.push_back(frameData);
        }

        // Check if we exceeded frame time budget
        if (frameData.totalMs > 16.67) {
            droppedFrames_++;
            spdlog::warn("Frame {} exceeded time budget: {:.2f}ms", frameData.frameNumber,
                         frameData.totalMs);
        }

        currentFrame_++;

        // Log progress every 60 frames
        if (currentFrame_ % 60 == 0 && currentFrame_ > 0) {
            auto currentTime = std::chrono::system_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                               currentTime.time_since_epoch())
                               .count() -
                           stats_.startTimeMs;
            double actualFps = (currentFrame_ * 1000.0) / elapsed;
            spdlog::info("Frame {}: {:.2f}ms (capture: {:.2f}ms, memory: {:.2f}ms, keys: "
                         "{:.2f}ms, disk: {:.2f}ms) - FPS: {:.1f}",
                         frameData.frameNumber, frameData.totalMs, frameData.frameCaptureMs,
                         frameData.memoryReadMs, frameData.keystrokeCaptureMs,
                         frameData.diskWriteMs, actualFps);
        }

        lastFrameTime = frameEndTime;
    }

    spdlog::info("Recording loop stopped");
}

bool ProcessRecorder::ReadMemoryAttributes(FrameData &frameData) {
    if (!memory_) {
        return false;
    }

    for (const auto &attrName : attributeNames_) {
        try {
            ProcessAttribute attr = memory_->GetAttribute(attrName);

            if (attr.AttributeType == "int") {
                int32_t value = 0;
                if (memory_->ExtractAttributeInt(attrName, value)) {
                    frameData.memoryData[attrName] = std::to_string(value);
                }
            } else if (attr.AttributeType == "float") {
                float value = 0.0f;
                if (memory_->ExtractAttributeFloat(attrName, value)) {
                    frameData.memoryData[attrName] = std::to_string(value);
                }
            } else if (attr.AttributeType == "array") {
                std::vector<uint8_t> value(attr.AttributeLength);
                if (memory_->ExtractAttributeArray(attrName, value)) {
                    // Convert array to hex string
                    std::stringstream ss;
                    ss << "[";
                    for (size_t i = 0; i < value.size(); ++i) {
                        if (i > 0)
                            ss << ",";
                        ss << static_cast<int>(value[i]);
                    }
                    ss << "]";
                    frameData.memoryData[attrName] = ss.str();
                }
            } else if (attr.AttributeType == "bool") {
                std::vector<uint8_t> value(1);
                if (memory_->ExtractAttributeArray(attrName, value)) {
                    frameData.memoryData[attrName] = value[0] ? "true" : "false";
                }
            }
        } catch (const std::exception &e) {
            spdlog::error("Failed to read attribute {}: {}", attrName, e.what());
        }
    }

    return true;
}

bool ProcessRecorder::CaptureKeystrokes(FrameData &frameData) {
    frameData.keysPressed = GetCurrentKeysPressed();
    return true;
}

bool ProcessRecorder::WriteFrameToDisk(const std::vector<uint8_t> &pixels, int frameNumber) {
    if (pixels.empty()) {
        spdlog::error("Cannot write empty frame {}", frameNumber);
        return false;
    }

    try {
        // Build path: outputDirectory/sessionId/frames/frame_NNNNNN.bmp
        fs::path framesPath = fs::path(outputDirectory_) / sessionId_ / "frames";

        std::stringstream filename;
        filename << "frame_" << std::setw(6) << std::setfill('0') << frameNumber << ".bmp";
        fs::path framePath = framesPath / filename.str();

        // Use ProcessCapture's SaveBMP function
        return capture_->SaveBMP(pixels, framePath.string().c_str());

    } catch (const std::exception &e) {
        spdlog::error("Failed to write frame {} to disk: {}", frameNumber, e.what());
        return false;
    }
}

bool ProcessRecorder::WriteMetadataToFile() {
    try {
        // Build path: outputDirectory/sessionId/metadata.json
        fs::path metadataPath = fs::path(outputDirectory_) / sessionId_ / "metadata.json";
        std::ofstream outFile(metadataPath);

        if (!outFile.is_open()) {
            spdlog::error("Failed to open metadata file for writing");
            return false;
        }

        outFile << "{\n";
        outFile << "  \"session_id\": \"" << sessionId_ << "\",\n";
        outFile << "  \"start_time_ms\": " << stats_.startTimeMs << ",\n";
        outFile << "  \"end_time_ms\": " << stats_.endTimeMs << ",\n";
        outFile << "  \"total_frames\": " << stats_.totalFrames << ",\n";
        outFile << "  \"dropped_frames\": " << stats_.droppedFrames << ",\n";
        outFile << "  \"actual_duration_seconds\": " << stats_.actualDurationSeconds << ",\n";
        outFile << "  \"actual_fps\": " << stats_.actualFps << ",\n";
        outFile << "  \"average_latency_ms\": " << stats_.averageLatencyMs << ",\n";
        outFile << "  \"max_latency_ms\": " << stats_.maxLatencyMs << ",\n";
        outFile << "  \"min_latency_ms\": " << stats_.minLatencyMs << ",\n";
        outFile << "  \"frames\": [\n";

        std::lock_guard<std::mutex> lock(bufferMutex_);
        for (size_t i = 0; i < frameDataBuffer_.size(); ++i) {
            const auto &frame = frameDataBuffer_[i];

            outFile << "    {\n";
            outFile << "      \"frame\": " << frame.frameNumber << ",\n";
            outFile << "      \"timestamp_ms\": " << frame.timestampMs << ",\n";

            // Memory data
            outFile << "      \"memory_data\": {\n";
            size_t memIdx = 0;
            for (const auto &pair : frame.memoryData) {
                outFile << "        \"" << pair.first << "\": \"" << pair.second << "\"";
                if (memIdx < frame.memoryData.size() - 1)
                    outFile << ",";
                outFile << "\n";
                memIdx++;
            }
            outFile << "      },\n";

            // Keys pressed
            outFile << "      \"keys_pressed\": [";
            for (size_t j = 0; j < frame.keysPressed.size(); ++j) {
                outFile << "\"" << frame.keysPressed[j] << "\"";
                if (j < frame.keysPressed.size() - 1)
                    outFile << ", ";
            }
            outFile << "],\n";

            // Latency
            outFile << "      \"latency_ms\": {\n";
            outFile << "        \"frame_capture\": " << frame.frameCaptureMs << ",\n";
            outFile << "        \"memory_read\": " << frame.memoryReadMs << ",\n";
            outFile << "        \"keystroke_capture\": " << frame.keystrokeCaptureMs << ",\n";
            outFile << "        \"disk_write\": " << frame.diskWriteMs << ",\n";
            outFile << "        \"total\": " << frame.totalMs << "\n";
            outFile << "      }\n";

            outFile << "    }";
            if (i < frameDataBuffer_.size() - 1)
                outFile << ",";
            outFile << "\n";
        }

        outFile << "  ]\n";
        outFile << "}\n";

        outFile.close();

        spdlog::info("Metadata written to: {}", metadataPath.string());
        return true;

    } catch (const std::exception &e) {
        spdlog::error("Failed to write metadata file: {}", e.what());
        return false;
    }
}
