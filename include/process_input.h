#pragma once

#include "interception.h"
#include <cstdio>
#include <map>
#include <string>
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
    bool TapKey(std::string key, int holdMs = 5000);
};