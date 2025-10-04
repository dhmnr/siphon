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
    ProcessInput(HWND processWindow);
    ~ProcessInput();
    bool IsInitialized() const;
    bool BringToFocus();
    void PressKey(std::string key);
    void ReleaseKey(std::string key);
    void TapKey(std::string key, int holdMs = 5000);
};