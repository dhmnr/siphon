#include "jpeg_encoder.h"
#include <spdlog/spdlog.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

std::vector<uint8_t> JpegEncoder::EncodeBGRA(const uint8_t *pixels, int width, int height,
                                              int quality) {
    // Find MJPEG encoder
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (!codec) {
        spdlog::error("MJPEG codec not found");
        return {};
    }

    AVCodecContext *codecCtx = avcodec_alloc_context3(codec);
    if (!codecCtx) {
        spdlog::error("Failed to allocate codec context");
        return {};
    }

    // Configure JPEG encoder
    codecCtx->width = width;
    codecCtx->height = height;
    codecCtx->pix_fmt = AV_PIX_FMT_YUVJ420P; // JPEG uses full-range YUV
    codecCtx->time_base = AVRational{1, 1};

    // Quality: 2-31 (lower = better), convert from 1-100 scale
    int qscale = 31 - ((quality * 29) / 100);
    codecCtx->qmin = codecCtx->qmax = qscale;
    codecCtx->flags |= AV_CODEC_FLAG_QSCALE;

    if (avcodec_open2(codecCtx, codec, nullptr) < 0) {
        spdlog::error("Failed to open MJPEG codec");
        avcodec_free_context(&codecCtx);
        return {};
    }

    // Allocate frame for YUV data
    AVFrame *yuvFrame = av_frame_alloc();
    if (!yuvFrame) {
        avcodec_free_context(&codecCtx);
        return {};
    }

    yuvFrame->format = codecCtx->pix_fmt;
    yuvFrame->width = width;
    yuvFrame->height = height;

    if (av_frame_get_buffer(yuvFrame, 0) < 0) {
        av_frame_free(&yuvFrame);
        avcodec_free_context(&codecCtx);
        return {};
    }

    // Convert BGRA to YUV420
    SwsContext *swsCtx = sws_getContext(width, height, AV_PIX_FMT_BGRA, width, height,
                                        AV_PIX_FMT_YUVJ420P, SWS_BILINEAR, nullptr, nullptr,
                                        nullptr);
    if (!swsCtx) {
        av_frame_free(&yuvFrame);
        avcodec_free_context(&codecCtx);
        return {};
    }

    const uint8_t *srcData[1] = {pixels};
    int srcLinesize[1] = {width * 4};
    sws_scale(swsCtx, srcData, srcLinesize, 0, height, yuvFrame->data, yuvFrame->linesize);
    sws_freeContext(swsCtx);

    // Encode frame
    AVPacket *pkt = av_packet_alloc();
    if (!pkt) {
        av_frame_free(&yuvFrame);
        avcodec_free_context(&codecCtx);
        return {};
    }

    int ret = avcodec_send_frame(codecCtx, yuvFrame);
    std::vector<uint8_t> jpegData;

    if (ret >= 0) {
        ret = avcodec_receive_packet(codecCtx, pkt);
        if (ret >= 0) {
            jpegData.assign(pkt->data, pkt->data + pkt->size);
        }
    }

    // Cleanup
    av_packet_free(&pkt);
    av_frame_free(&yuvFrame);
    avcodec_free_context(&codecCtx);

    return jpegData;
}

