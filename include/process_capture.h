#pragma once

#include <string>
#include <vector>
#include <windows.h>

std::vector<unsigned char> CaptureFrameInternal(HWND processWindow, int &width, int &height);
bool SaveFrameToBMP(HWND processWindow, const std::string &filename);