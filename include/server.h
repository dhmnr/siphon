#pragma once

#include "process_capture.h"
#include "process_input.h"
#include "process_memory.h"
#include <cstdint>
#include <windows.h>

void RunServer(ProcessMemory *memory, ProcessInput *input, ProcessCapture *capture);