#pragma once

#include "process_capture.h"
#include "process_input.h"
#include "process_memory.h"
#include <cstdint>
#include <windows.h>

// New server API - starts without pre-initialized components
void RunServer();