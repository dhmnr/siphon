cmake_minimum_required(VERSION 3.15)
project(Siphon)

set(CMAKE_CXX_STANDARD 17)

add_executable(simple_siphon simple_siphon.cpp)

# Link Windows libraries
target_link_libraries(simple_siphon psapi user32)

add_executable(siphon siphon.cpp)

# Link Windows libraries
target_link_libraries(siphon psapi user32)