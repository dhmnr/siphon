#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

// Forward declarations for FFmpeg types
struct AVCodec;
struct AVCodecContext;
struct AVFormatContext;
struct AVFrame;
struct AVPacket;
struct AVStream;
struct SwsContext;

// Frame data for encoding
struct EncoderFrame {
    std::vector<uint8_t> pixels; // BGRA pixel data
    int64_t timestampUs;         // Microseconds since epoch
    int width;
    int height;
};

// Video encoder using FFmpeg with lossless compression
class VideoEncoder {
  public:
    VideoEncoder();
    ~VideoEncoder();

    // Initialize encoder with lossless FFV1 codec
    bool Initialize(const std::string &outputPath, int width, int height, int fps = 60);

    // Queue frame for encoding (non-blocking)
    void EncodeFrame(EncoderFrame frame);

    // Stop encoding and finalize video file
    void Finalize();

    // Get current queue size
    size_t GetQueueSize() const;

    // Get total frames encoded
    int GetFramesEncoded() const { return framesEncoded_.load(); }

  private:
    void EncoderThread();
    bool InitializeFFmpeg();
    void CleanupFFmpeg();
    bool EncodeFrameInternal(const EncoderFrame &frame);

    // Configuration
    std::string outputPath_;
    int width_;
    int height_;
    int fps_;

    // FFmpeg context
    AVFormatContext *formatContext_;
    AVCodecContext *codecContext_;
    AVStream *videoStream_;
    SwsContext *swsContext_;
    AVFrame *yuvFrame_;
    AVPacket *packet_;

    // Threading
    std::thread encoderThread_;
    std::queue<EncoderFrame> frameQueue_;
    std::mutex queueMutex_;
    std::condition_variable queueCV_;
    std::atomic<bool> shouldStop_;
    std::atomic<bool> finalized_;

    // Statistics
    std::atomic<int> framesEncoded_;
    int64_t firstFrameTimestamp_;
    int64_t lastFrameTimestamp_;
};
