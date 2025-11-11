#include "video_encoder.h"
#include <spdlog/spdlog.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

VideoEncoder::VideoEncoder()
    : width_(0), height_(0), fps_(60), formatContext_(nullptr), codecContext_(nullptr),
      videoStream_(nullptr), swsContext_(nullptr), yuvFrame_(nullptr), packet_(nullptr),
      shouldStop_(false), finalized_(false), framesEncoded_(0), firstFrameTimestamp_(-1),
      lastFrameTimestamp_(-1) {}

VideoEncoder::~VideoEncoder() {
    if (!finalized_) {
        Finalize();
    }
}

bool VideoEncoder::Initialize(const std::string &outputPath, int width, int height, int fps) {
    outputPath_ = outputPath;
    width_ = width;
    height_ = height;
    fps_ = fps;

    if (!InitializeFFmpeg()) {
        return false;
    }

    // Start encoder thread
    shouldStop_ = false;
    encoderThread_ = std::thread(&VideoEncoder::EncoderThread, this);

    spdlog::info("VideoEncoder initialized: {}", outputPath_);
    spdlog::info("Resolution: {}x{}, Codec: H.264 (variable FPS)", width_, height_);

    return true;
}

bool VideoEncoder::InitializeFFmpeg() {
    try {
        // Allocate output context (MP4 format)
        avformat_alloc_output_context2(&formatContext_, nullptr, nullptr, outputPath_.c_str());
        if (!formatContext_) {
            spdlog::error("Could not create output context");
            return false;
        }

        // Find H.264 codec
        const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_H264);
        if (!codec) {
            spdlog::error("H.264 codec not found. Please install FFmpeg with libx264 support.");
            return false;
        }

        // Create video stream
        videoStream_ = avformat_new_stream(formatContext_, nullptr);
        if (!videoStream_) {
            spdlog::error("Could not create video stream");
            return false;
        }

        // Allocate codec context
        codecContext_ = avcodec_alloc_context3(codec);
        if (!codecContext_) {
            spdlog::error("Could not allocate codec context");
            return false;
        }

        // Configure codec for variable FPS (actual capture rate)
        codecContext_->width = width_;
        codecContext_->height = height_;
        codecContext_->time_base = AVRational{1, 1000000}; // Microsecond time base
        codecContext_->framerate = AVRational{0, 1};       // Variable framerate
        codecContext_->pix_fmt = AV_PIX_FMT_YUV420P;
        codecContext_->bit_rate = 10000000; // 10 Mbps bitrate
        codecContext_->gop_size = 60;       // Keyframe every ~1 second

        // H.264 settings for high quality
        av_opt_set(codecContext_->priv_data, "preset", "medium", 0);
        av_opt_set(codecContext_->priv_data, "crf", "18",
                   0); // High quality (0-51, lower is better)
        av_opt_set(codecContext_->priv_data, "tune", "zerolatency", 0);

        // Some formats require global headers
        if (formatContext_->oformat->flags & AVFMT_GLOBALHEADER) {
            codecContext_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }

        // Open codec
        int ret = avcodec_open2(codecContext_, codec, nullptr);
        if (ret < 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, sizeof(errbuf));
            spdlog::error("Could not open codec: {}", errbuf);
            return false;
        }

        // Copy codec parameters to stream
        avcodec_parameters_from_context(videoStream_->codecpar, codecContext_);
        videoStream_->time_base = codecContext_->time_base;
        videoStream_->avg_frame_rate = AVRational{0, 1}; // Variable frame rate
        videoStream_->r_frame_rate = AVRational{0, 1};   // Variable frame rate

        // Allocate frame for YUV data
        yuvFrame_ = av_frame_alloc();
        if (!yuvFrame_) {
            spdlog::error("Could not allocate frame");
            return false;
        }

        yuvFrame_->format = codecContext_->pix_fmt;
        yuvFrame_->width = width_;
        yuvFrame_->height = height_;

        ret = av_frame_get_buffer(yuvFrame_, 0);
        if (ret < 0) {
            spdlog::error("Could not allocate frame buffer");
            return false;
        }

        // Allocate packet
        packet_ = av_packet_alloc();
        if (!packet_) {
            spdlog::error("Could not allocate packet");
            return false;
        }

        // Initialize swscale context for BGRA -> YUV420P conversion
        swsContext_ = sws_getContext(width_, height_, AV_PIX_FMT_BGRA, width_, height_,
                                     AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!swsContext_) {
            spdlog::error("Could not initialize swscale context");
            return false;
        }

        // Open output file
        if (!(formatContext_->oformat->flags & AVFMT_NOFILE)) {
            ret = avio_open(&formatContext_->pb, outputPath_.c_str(), AVIO_FLAG_WRITE);
            if (ret < 0) {
                char errbuf[AV_ERROR_MAX_STRING_SIZE];
                av_strerror(ret, errbuf, sizeof(errbuf));
                spdlog::error("Could not open output file: {}", errbuf);
                return false;
            }
        }

        // Write header
        ret = avformat_write_header(formatContext_, nullptr);
        if (ret < 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, sizeof(errbuf));
            spdlog::error("Could not write header: {}", errbuf);
            return false;
        }

        spdlog::info("FFmpeg initialized successfully with H.264 codec (variable FPS)");
        return true;

    } catch (const std::exception &e) {
        spdlog::error("Exception during FFmpeg initialization: {}", e.what());
        CleanupFFmpeg();
        return false;
    }
}

void VideoEncoder::EncodeFrame(EncoderFrame frame) {
    if (finalized_) {
        spdlog::warn("Cannot encode frame - encoder already finalized");
        return;
    }

    std::lock_guard<std::mutex> lock(queueMutex_);
    frameQueue_.push(std::move(frame));
    queueCV_.notify_one();
}

void VideoEncoder::EncoderThread() {
    spdlog::info("Video encoder thread started");

    while (true) {
        EncoderFrame frame;

        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            queueCV_.wait(lock, [this] { return !frameQueue_.empty() || shouldStop_; });

            if (shouldStop_ && frameQueue_.empty()) {
                break;
            }

            if (!frameQueue_.empty()) {
                frame = std::move(frameQueue_.front());
                frameQueue_.pop();
            } else {
                continue;
            }
        }

        if (!EncodeFrameInternal(frame)) {
            spdlog::error("Failed to encode frame {}", framesEncoded_.load());
        }

        framesEncoded_++;

        // Log progress every 60 frames
        if (framesEncoded_ % 60 == 0) {
            spdlog::info("Encoded {} frames, queue size: {}", framesEncoded_.load(),
                         GetQueueSize());
        }
    }

    spdlog::info("Video encoder thread stopped - {} frames encoded", framesEncoded_.load());
}

bool VideoEncoder::EncodeFrameInternal(const EncoderFrame &frame) {
    try {
        // Make frame writable
        int ret = av_frame_make_writable(yuvFrame_);
        if (ret < 0) {
            spdlog::error("Could not make frame writable");
            return false;
        }

        // Convert BGRA to YUV420P
        const uint8_t *srcData[1] = {frame.pixels.data()};
        int srcLinesize[1] = {frame.width * 4}; // BGRA = 4 bytes per pixel

        sws_scale(swsContext_, srcData, srcLinesize, 0, frame.height, yuvFrame_->data,
                  yuvFrame_->linesize);

        // Set presentation timestamp using actual capture time for variable FPS
        if (firstFrameTimestamp_ < 0) {
            firstFrameTimestamp_ = frame.timestampUs;
        }
        lastFrameTimestamp_ = frame.timestampUs;

        // PTS in microseconds from first frame
        yuvFrame_->pts = frame.timestampUs - firstFrameTimestamp_;

        // Send frame to encoder
        ret = avcodec_send_frame(codecContext_, yuvFrame_);
        if (ret < 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, sizeof(errbuf));
            spdlog::error("Error sending frame to encoder: {}", errbuf);
            return false;
        }

        // Receive encoded packets
        while (ret >= 0) {
            ret = avcodec_receive_packet(codecContext_, packet_);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            } else if (ret < 0) {
                char errbuf[AV_ERROR_MAX_STRING_SIZE];
                av_strerror(ret, errbuf, sizeof(errbuf));
                spdlog::error("Error receiving packet from encoder: {}", errbuf);
                return false;
            }

            // Rescale packet timestamps
            av_packet_rescale_ts(packet_, codecContext_->time_base, videoStream_->time_base);
            packet_->stream_index = videoStream_->index;

            // Write packet to file
            ret = av_interleaved_write_frame(formatContext_, packet_);
            av_packet_unref(packet_);

            if (ret < 0) {
                char errbuf[AV_ERROR_MAX_STRING_SIZE];
                av_strerror(ret, errbuf, sizeof(errbuf));
                spdlog::error("Error writing packet: {}", errbuf);
                return false;
            }
        }

        return true;

    } catch (const std::exception &e) {
        spdlog::error("Exception during frame encoding: {}", e.what());
        return false;
    }
}

void VideoEncoder::Finalize() {
    if (finalized_) {
        return;
    }

    spdlog::info("Finalizing video encoder - queue size: {}", GetQueueSize());

    // Signal encoder thread to stop
    shouldStop_ = true;
    queueCV_.notify_all();

    // Wait for encoder thread
    if (encoderThread_.joinable()) {
        encoderThread_.join();
    }

    // Flush encoder
    if (codecContext_) {
        avcodec_send_frame(codecContext_, nullptr);

        int ret;
        while (ret >= 0) {
            ret = avcodec_receive_packet(codecContext_, packet_);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            } else if (ret >= 0) {
                av_packet_rescale_ts(packet_, codecContext_->time_base, videoStream_->time_base);
                packet_->stream_index = videoStream_->index;
                av_interleaved_write_frame(formatContext_, packet_);
                av_packet_unref(packet_);
            }
        }
    }

    // Write trailer
    if (formatContext_) {
        av_write_trailer(formatContext_);
    }

    CleanupFFmpeg();

    finalized_ = true;

    double durationSec = (lastFrameTimestamp_ - firstFrameTimestamp_) / 1000000.0;
    double actualFps = framesEncoded_.load() / durationSec;

    spdlog::info("Video encoder finalized");
    spdlog::info("  Total frames: {}", framesEncoded_.load());
    spdlog::info("  Duration: {:.2f}s", durationSec);
    spdlog::info("  Actual FPS: {:.2f}", actualFps);
}

void VideoEncoder::CleanupFFmpeg() {
    if (swsContext_) {
        sws_freeContext(swsContext_);
        swsContext_ = nullptr;
    }

    if (yuvFrame_) {
        av_frame_free(&yuvFrame_);
    }

    if (packet_) {
        av_packet_free(&packet_);
    }

    if (codecContext_) {
        avcodec_free_context(&codecContext_);
    }

    if (formatContext_) {
        if (!(formatContext_->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&formatContext_->pb);
        }
        avformat_free_context(formatContext_);
        formatContext_ = nullptr;
    }
}

size_t VideoEncoder::GetQueueSize() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex &>(queueMutex_));
    return frameQueue_.size();
}
