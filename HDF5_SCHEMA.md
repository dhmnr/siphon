# HDF5 Recording Schema

## File Structure

### Dataset: `frames`
- **Shape**: `[N, height, width, 4]`
- **Type**: `uint8`
- **Description**: BGRA pixel data for each frame

### Dataset: `timestamps`
- **Shape**: `[N]`
- **Type**: `int64`
- **Description**: Unix timestamp in milliseconds

### Dataset: `memory_data`
- **Shape**: `[N, num_attributes]`
- **Type**: `float32`
- **Description**: Memory attribute values per frame
- **Attribute**: `attribute_names` (string array) - names of memory attributes in column order

### Dataset: `inputs`
- **Shape**: `[N, 300]`
- **Type**: `uint8` (boolean: 0 or 1)
- **Description**: Binary array indicating which keys/buttons are pressed
- **Attribute**: `key_mapping` (string array) - maps column index to key name

### Dataset: `latencies`
- **Shape**: `[N, 5]`
- **Type**: `float32` (milliseconds)
- **Description**: Per-frame latency breakdown
- **Columns**:
  - `[0]` = frame capture time
  - `[1]` = memory read time
  - `[2]` = keystroke capture time
  - `[3]` = disk write queue time
  - `[4]` = total latency

## How Key Mapping Works

Each recording assigns column indices to keys **as they are pressed**. The first unique key pressed gets column 0, the second gets column 1, etc. The `key_mapping` attribute stores which column corresponds to which key name for that specific recording.

## All Possible Key Names

**Letters**: A, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z

**Numbers**: 0, 1, 2, 3, 4, 5, 6, 7, 8, 9

**Function Keys**: F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12

**Modifiers**: LEFT_SHIFT, RIGHT_SHIFT, LEFT_CTRL, LEFT_ALT

**Special Keys**: ESC, ENTER, SPACE, TAB, BACKSPACE, CAPSLOCK, NUMLOCK, SCROLLLOCK

**Numpad**: KEYPAD_0, KEYPAD_1, KEYPAD_2, KEYPAD_3, KEYPAD_4, KEYPAD_5, KEYPAD_6, KEYPAD_7, KEYPAD_8, KEYPAD_9

**Symbols**: MINUS, EQUALS, LEFT_BRACKET, RIGHT_BRACKET, SEMICOLON, APOSTROPHE, GRAVE, BACKSLASH, COMMA, PERIOD, SLASH

**Mouse Buttons**: MOUSE_LEFT, MOUSE_RIGHT, MOUSE_MIDDLE, MOUSE_BUTTON4, MOUSE_BUTTON5

**Unknown Keys**: UNKNOWN_<vkCode> (for any key not in the above list)

