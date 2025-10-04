#include <spdlog/spdlog.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <windows.h>

#include "process_capture.h"

// Test function to create a simple colored BMP for verification
bool CreateTestBMP(const std::string &filename, int width, int height) {
    spdlog::info("Creating test BMP: {}x{}", width, height);

    // Create test pattern (red, green, blue stripes)
    std::vector<unsigned char> testPixels(width * height * 3);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int pixelIndex = (y * width + x) * 3;
            if (x < width / 3) {
                // Red stripe
                testPixels[pixelIndex] = 0;       // B
                testPixels[pixelIndex + 1] = 0;   // G
                testPixels[pixelIndex + 2] = 255; // R
            } else if (x < 2 * width / 3) {
                // Green stripe
                testPixels[pixelIndex] = 0;       // B
                testPixels[pixelIndex + 1] = 255; // G
                testPixels[pixelIndex + 2] = 0;   // R
            } else {
                // Blue stripe
                testPixels[pixelIndex] = 255;   // B
                testPixels[pixelIndex + 1] = 0; // G
                testPixels[pixelIndex + 2] = 0; // R
            }
        }
    }

    // Calculate BMP parameters
    int bytesPerPixel = 3;
    int rowSize = ((width * bytesPerPixel + 3) / 4) * 4;
    int padding = rowSize - (width * bytesPerPixel);
    int imageSize = rowSize * height;

    // Create headers
    BITMAPFILEHEADER fileHeader;
    memset(&fileHeader, 0, sizeof(fileHeader));
    fileHeader.bfType = 0x4D42;
    fileHeader.bfSize = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + imageSize;
    fileHeader.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);

    BITMAPINFOHEADER infoHeader;
    memset(&infoHeader, 0, sizeof(infoHeader));
    infoHeader.biSize = sizeof(BITMAPINFOHEADER);
    infoHeader.biWidth = width;
    infoHeader.biHeight = -height;
    infoHeader.biPlanes = 1;
    infoHeader.biBitCount = 24;
    infoHeader.biCompression = BI_RGB;
    infoHeader.biSizeImage = imageSize;

    // Write file
    FILE *file = fopen(filename.c_str(), "wb");
    if (!file)
        return false;

    fwrite(&fileHeader, sizeof(fileHeader), 1, file);
    fwrite(&infoHeader, sizeof(infoHeader), 1, file);

    // Write test pixels
    const unsigned char *pixelPtr = testPixels.data();
    for (int y = 0; y < height; y++) {
        fwrite(pixelPtr, width * bytesPerPixel, 1, file);
        if (padding > 0) {
            static const unsigned char paddingBytes[4] = {0, 0, 0, 0};
            fwrite(paddingBytes, padding, 1, file);
        }
        pixelPtr += width * bytesPerPixel;
    }

    fclose(file);
    spdlog::info("Test BMP created: {}", filename);
    return true;
}

// Helper function to try different capture strategies
bool TryCaptureStrategy(HWND hwnd, HDC hdcMem, int width, int height, const char *strategyName) {
    spdlog::info("Trying capture strategy: {}", strategyName);

    if (strcmp(strategyName, "PrintWindow") == 0) {
        // Try different PrintWindow flags for background windows
        if (PrintWindow(hwnd, hdcMem, PW_RENDERFULLCONTENT)) {
            return true;
        }
        // Fallback to basic PrintWindow
        if (PrintWindow(hwnd, hdcMem, 0)) {
            return true;
        }
        // Try with client area only
        return PrintWindow(hwnd, hdcMem, PW_CLIENTONLY);
    } else if (strcmp(strategyName, "BitBlt") == 0) {
        HDC hdcWindow = GetDC(hwnd);
        if (!hdcWindow)
            return false;
        bool result = BitBlt(hdcMem, 0, 0, width, height, hdcWindow, 0, 0, SRCCOPY);
        ReleaseDC(hwnd, hdcWindow);
        return result;
    } else if (strcmp(strategyName, "ScreenCapture") == 0) {
        HDC hdcScreen = GetDC(NULL);
        if (!hdcScreen)
            return false;
        POINT clientTopLeft = {0, 0};
        ClientToScreen(hwnd, &clientTopLeft);
        bool result = BitBlt(hdcMem, 0, 0, width, height, hdcScreen, clientTopLeft.x,
                             clientTopLeft.y, SRCCOPY);
        ReleaseDC(NULL, hdcScreen);
        return result;
    } else if (strcmp(strategyName, "FullScreenCapture") == 0) {
        // Capture entire screen, then crop to window
        HDC hdcScreen = GetDC(NULL);
        if (!hdcScreen)
            return false;

        // Get screen dimensions
        int screenWidth = GetSystemMetrics(SM_CXSCREEN);
        int screenHeight = GetSystemMetrics(SM_CYSCREEN);

        // Create a larger bitmap to hold screen data
        HBITMAP hbmScreen = CreateCompatibleBitmap(hdcScreen, screenWidth, screenHeight);
        HDC hdcScreenMem = CreateCompatibleDC(hdcScreen);
        HGDIOBJ oldScreenBitmap = SelectObject(hdcScreenMem, hbmScreen);

        // Capture entire screen
        bool screenResult =
            BitBlt(hdcScreenMem, 0, 0, screenWidth, screenHeight, hdcScreen, 0, 0, SRCCOPY);

        if (screenResult) {
            // Now copy the window area from screen capture to our target DC
            POINT clientTopLeft = {0, 0};
            ClientToScreen(hwnd, &clientTopLeft);
            bool result = BitBlt(hdcMem, 0, 0, width, height, hdcScreenMem, clientTopLeft.x,
                                 clientTopLeft.y, SRCCOPY);

            SelectObject(hdcScreenMem, oldScreenBitmap);
            DeleteObject(hbmScreen);
            DeleteDC(hdcScreenMem);
            ReleaseDC(NULL, hdcScreen);
            return result;
        }

        SelectObject(hdcScreenMem, oldScreenBitmap);
        DeleteObject(hbmScreen);
        DeleteDC(hdcScreenMem);
        ReleaseDC(NULL, hdcScreen);
        return false;
    }
    return false;
}

// Capture window frame as RGB bytes
std::vector<unsigned char> CaptureFrameInternal(HWND processWindow, int &width, int &height) {
    if (!processWindow || !IsWindow(processWindow)) {
        spdlog::error("Invalid window handle");
        return {};
    }

    // Check if window is visible
    if (!IsWindowVisible(processWindow)) {
        spdlog::error("Window is not visible");
        return {};
    }

    // Get window information for debugging
    char windowTitle[256] = {0};
    char className[256] = {0};
    GetWindowTextA(processWindow, windowTitle, sizeof(windowTitle));
    GetClassNameA(processWindow, className, sizeof(className));
    spdlog::info("Capturing window - Title: '{}', Class: '{}', HWND: 0x{:p}", windowTitle,
                 className, (void *)processWindow);

    // Get window dimensions - try multiple approaches
    RECT clientRect, windowRect;
    if (!GetClientRect(processWindow, &clientRect)) {
        spdlog::error("Failed to get client rect");
        return {};
    }

    if (!GetWindowRect(processWindow, &windowRect)) {
        spdlog::error("Failed to get window rect");
        return {};
    }

    // Try to get the actual content area
    int clientWidth = clientRect.right - clientRect.left;
    int clientHeight = clientRect.bottom - clientRect.top;
    int windowWidth = windowRect.right - windowRect.left;
    int windowHeight = windowRect.bottom - windowRect.top;

    spdlog::info("Window rect: {}x{}, Client rect: {}x{}", windowWidth, windowHeight, clientWidth,
                 clientHeight);

    // Use client area for capture
    width = clientWidth;
    height = clientHeight;

    if (width <= 0 || height <= 0) {
        spdlog::error("Invalid window dimensions: {}x{}", width, height);
        return {};
    }

    spdlog::info("Capturing client area: {}x{}", width, height);

    // Get device contexts
    HDC hdcWindow = GetDC(processWindow);
    if (!hdcWindow) {
        spdlog::error("Failed to get window DC");
        return {};
    }

    HDC hdcMem = CreateCompatibleDC(hdcWindow);
    if (!hdcMem) {
        spdlog::error("Failed to create compatible DC");
        ReleaseDC(processWindow, hdcWindow);
        return {};
    }

    HBITMAP hbmScreen = CreateCompatibleBitmap(hdcWindow, width, height);
    if (!hbmScreen) {
        spdlog::error("Failed to create compatible bitmap");
        DeleteDC(hdcMem);
        ReleaseDC(processWindow, hdcWindow);
        return {};
    }

    HGDIOBJ oldBitmap = SelectObject(hdcMem, hbmScreen);

    // Fill with black background first to avoid white background
    RECT fillRect = {0, 0, width, height};
    HBRUSH blackBrush = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(hdcMem, &fillRect, blackBrush);
    DeleteObject(blackBrush);

    // Check if window is minimized or in background
    bool isMinimized = IsIconic(processWindow);
    HWND foregroundWindow = GetForegroundWindow();
    bool isInBackground = (foregroundWindow != processWindow);

    spdlog::info("Window state - Minimized: {}, In background: {}", isMinimized, isInBackground);

    // For background/minimized windows, we need special handling
    HWND originalForeground = nullptr;
    if (isMinimized || isInBackground) {
        spdlog::info("Window is minimized or in background - using special capture method");

        // Store original foreground window
        originalForeground = GetForegroundWindow();

        // Try to restore and bring window to foreground temporarily
        if (isMinimized) {
            ShowWindow(processWindow, SW_RESTORE);
            Sleep(200); // Give it time to restore
        }

        // Bring to foreground
        SetForegroundWindow(processWindow);
        Sleep(200); // Give it time to render

        spdlog::info("Temporarily brought window to foreground for capture");
    }

    // Try different capture strategies in order of preference
    const char *strategies[] = {"PrintWindow", "BitBlt", "ScreenCapture", "FullScreenCapture"};
    bool captureSuccess = false;

    for (const char *strategy : strategies) {
        if (TryCaptureStrategy(processWindow, hdcMem, width, height, strategy)) {
            spdlog::info("Capture successful with strategy: {}", strategy);
            captureSuccess = true;
            break;
        } else {
            spdlog::warn("Strategy {} failed", strategy);
        }
    }

    if (!captureSuccess) {
        spdlog::error("All capture strategies failed");
    }

    // Restore original window state if we changed it
    if (originalForeground && originalForeground != processWindow) {
        spdlog::info("Restoring original foreground window");
        SetForegroundWindow(originalForeground);
    }

    // Setup bitmap info for RGB format
    BITMAPINFOHEADER bi = {0};
    bi.biSize = sizeof(BITMAPINFOHEADER);
    bi.biWidth = width;
    bi.biHeight = -height; // Negative for top-down bitmap
    bi.biPlanes = 1;
    bi.biBitCount = 24; // 24-bit RGB
    bi.biCompression = BI_RGB;

    // Allocate buffer for pixel data (3 bytes per pixel: BGR)
    std::vector<unsigned char> pixels(width * height * 3);

    // Get bitmap bits
    int lines =
        GetDIBits(hdcMem, hbmScreen, 0, height, pixels.data(), (BITMAPINFO *)&bi, DIB_RGB_COLORS);
    if (lines == 0) {
        DWORD error = GetLastError();
        spdlog::error("GetDIBits failed with error: {}", error);
        pixels.clear();
    } else {
        spdlog::info("Successfully captured {} lines", lines);

        // Check if we got actual content (not just white/black)
        bool hasContent = false;
        int nonWhitePixels = 0;
        int nonBlackPixels = 0;

        for (size_t i = 0; i < pixels.size(); i += 3) {
            // Check if pixel is not pure white (255,255,255)
            if (!(pixels[i] == 255 && pixels[i + 1] == 255 && pixels[i + 2] == 255)) {
                nonWhitePixels++;
            }
            // Check if pixel is not pure black (0,0,0)
            if (!(pixels[i] == 0 && pixels[i + 1] == 0 && pixels[i + 2] == 0)) {
                nonBlackPixels++;
            }
        }

        spdlog::info("Pixel analysis - Non-white: {}, Non-black: {}, Total pixels: {}",
                     nonWhitePixels, nonBlackPixels, pixels.size() / 3);

        if (nonWhitePixels < 100 && nonBlackPixels < 100) {
            spdlog::warn("Captured image appears to be solid color - may indicate capture failure");
        }
    }

    // Cleanup
    SelectObject(hdcMem, oldBitmap);
    DeleteObject(hbmScreen);
    DeleteDC(hdcMem);
    ReleaseDC(processWindow, hdcWindow);

    return pixels; // Returns BGR format
}

// Optional: Save to BMP for debugging
bool SaveFrameToBMP(HWND processWindow, const std::string &filename) {
    int width, height;

    auto pixels = CaptureFrameInternal(processWindow, width, height);
    spdlog::info("Captured frame - width: {}, height: {}", width, height);

    if (pixels.empty() || width <= 0 || height <= 0) {
        spdlog::error("Invalid frame data - width: {}, height: {}, pixels size: {}", width, height,
                      pixels.size());
        return false;
    }

    // Create a test BMP first to verify our BMP format is correct
    std::string testFilename = filename + "_test.bmp";
    if (CreateTestBMP(testFilename, width, height)) {
        spdlog::info("Created test BMP for verification: {}", testFilename);
    }

    // Calculate row padding (BMP requires 4-byte alignment)
    int bytesPerPixel = 3;                               // 24-bit RGB
    int rowSize = ((width * bytesPerPixel + 3) / 4) * 4; // Round up to 4-byte boundary
    int padding = rowSize - (width * bytesPerPixel);
    int imageSize = rowSize * height;

    spdlog::info(
        "BMP calculations - Width: {}, Height: {}, RowSize: {}, Padding: {}, ImageSize: {}", width,
        height, rowSize, padding, imageSize);

    // Validate pixel data size
    int expectedPixelSize = width * height * bytesPerPixel;
    if (pixels.size() != expectedPixelSize) {
        spdlog::error("Pixel data size mismatch - expected: {}, actual: {}", expectedPixelSize,
                      pixels.size());
        return false;
    }

    // Create BMP headers with proper initialization
    BITMAPFILEHEADER fileHeader;
    memset(&fileHeader, 0, sizeof(fileHeader));
    fileHeader.bfType = 0x4D42; // "BM"
    fileHeader.bfSize = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + imageSize;
    fileHeader.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    fileHeader.bfReserved1 = 0;
    fileHeader.bfReserved2 = 0;

    BITMAPINFOHEADER infoHeader;
    memset(&infoHeader, 0, sizeof(infoHeader));
    infoHeader.biSize = sizeof(BITMAPINFOHEADER);
    infoHeader.biWidth = width;
    infoHeader.biHeight = -height; // Negative for top-down bitmap
    infoHeader.biPlanes = 1;
    infoHeader.biBitCount = 24;
    infoHeader.biCompression = BI_RGB;
    infoHeader.biSizeImage = imageSize;
    infoHeader.biXPelsPerMeter = 0;
    infoHeader.biYPelsPerMeter = 0;
    infoHeader.biClrUsed = 0;
    infoHeader.biClrImportant = 0;

    spdlog::info("BMP Headers - FileSize: {}, OffBits: {}, Width: {}, Height: {}, SizeImage: {}",
                 fileHeader.bfSize, fileHeader.bfOffBits, infoHeader.biWidth, infoHeader.biHeight,
                 infoHeader.biSizeImage);

    FILE *file = fopen(filename.c_str(), "wb");
    if (!file) {
        spdlog::error("Failed to open file: {}", filename);
        return false;
    }

    // Write headers
    if (fwrite(&fileHeader, sizeof(fileHeader), 1, file) != 1) {
        spdlog::error("Failed to write file header");
        fclose(file);
        return false;
    }

    if (fwrite(&infoHeader, sizeof(infoHeader), 1, file) != 1) {
        spdlog::error("Failed to write info header");
        fclose(file);
        return false;
    }

    // Write pixel data with proper row padding
    const unsigned char *pixelPtr = pixels.data();
    size_t totalBytesWritten = 0;

    for (int y = 0; y < height; y++) {
        // Write row data (BGR format)
        size_t rowBytes = width * bytesPerPixel;
        if (fwrite(pixelPtr, rowBytes, 1, file) != 1) {
            spdlog::error("Failed to write pixel row {}", y);
            fclose(file);
            return false;
        }
        totalBytesWritten += rowBytes;

        // Write padding if needed (BMP requires 4-byte row alignment)
        if (padding > 0) {
            static const unsigned char paddingBytes[4] = {0, 0, 0, 0};
            if (fwrite(paddingBytes, padding, 1, file) != 1) {
                spdlog::error("Failed to write row padding for row {}", y);
                fclose(file);
                return false;
            }
            totalBytesWritten += padding;
        }

        pixelPtr += width * bytesPerPixel;
    }

    spdlog::info("Wrote {} bytes of pixel data (expected: {})", totalBytesWritten, imageSize);

    // Verify we wrote the correct amount
    if (totalBytesWritten != imageSize) {
        spdlog::error("Pixel data write mismatch - wrote: {}, expected: {}", totalBytesWritten,
                      imageSize);
        fclose(file);
        return false;
    }

    fclose(file);

    // Verify file was created and has correct size
    FILE *verifyFile = fopen(filename.c_str(), "rb");
    if (verifyFile) {
        fseek(verifyFile, 0, SEEK_END);
        long fileSize = ftell(verifyFile);
        fclose(verifyFile);

        long expectedFileSize = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + imageSize;
        spdlog::info("BMP file verification - Expected size: {} bytes, Actual size: {} bytes",
                     expectedFileSize, fileSize);

        if (fileSize == expectedFileSize) {
            spdlog::info("Successfully saved BMP file: {} ({}x{} pixels, {} bytes)", filename,
                         width, height, fileSize);
        } else {
            spdlog::error("BMP file size mismatch - file may be corrupted");
            return false;
        }
    } else {
        spdlog::error("Failed to verify BMP file after creation");
        return false;
    }

    return true;
}