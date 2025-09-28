cmake_minimum_required(VERSION 3.10)
project(Siphon)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(siphon src/main.cpp src/utils.cpp src/process_memory.cpp)

# Add include directory
target_include_directories(siphon PRIVATE include)

# Link Windows libraries
target_link_libraries(siphon psapi user32)
