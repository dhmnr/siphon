#pragma once

#include <vector>
#include <cstdint>

// Simple JPEG encoder using FFmpeg
class JpegEncoder {
  public:
    // Encode BGRA pixels to JPEG
    // Returns empty vector on failure
    static std::vector<uint8_t> EncodeBGRA(const uint8_t* pixels, int width, int height, int quality = 85);
};

