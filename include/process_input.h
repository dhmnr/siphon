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

  public:
    ProcessInput();
    ~ProcessInput();
    bool Initialize(HWND processWindow);
    void PressKey(std::string key);
    void ReleaseKey(std::string key);
    bool TapKey(std::vector<std::string> keys, int holdMs = 100, int delayMs = 0);
};