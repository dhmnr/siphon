#pragma once

#include "interception.h"
#include <cstdio>
#include <map>
#include <string>
#include <vector>
#include <windows.h>

extern std::map<std::string, unsigned short> scancodeMap;

class ProcessInput {
  private:
    HWND processWindow;
    InterceptionContext context;
    InterceptionDevice keyboard;
    InterceptionDevice mouse;

  public:
    ProcessInput();
    ~ProcessInput();
    bool Initialize(HWND processWindow);
    void PressKey(std::string key);
    void ReleaseKey(std::string key);
    bool TapKey(std::vector<std::string> keys, int holdMs = 100, int delayMs = 0);
    void PressMouseButton(std::string button);
    void ReleaseMouseButton(std::string button);
    bool ClickMouseButton(std::string button, int delayMs = 0);
    bool MoveMouse(int deltaX, int deltaY);
    bool MoveMouseSmooth(int targetX, int targetY, int steps = 10);
    bool ScrollWheel(int amount);
};