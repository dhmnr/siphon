cmake_minimum_required(VERSION 3.15)
project(Siphon)

set(CMAKE_CXX_STANDARD 17)




add_executable(siphon siphon.cpp)
target_link_libraries(siphon psapi user32)

add_executable(list_windows list_windows.cpp)
target_link_libraries(list_windows psapi user32)

set(INTERCEPTION_SDK_PATH "${CMAKE_SOURCE_DIR}/external/interception")

# Add Interception include directory
include_directories(${INTERCEPTION_SDK_PATH}/include)

# Add your executable
add_executable(elden_ring_controller
    interception.cpp
)

# Link Interception library
# For x64 builds
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    target_link_libraries(elden_ring_controller
        ${INTERCEPTION_SDK_PATH}/library/x64/interception.lib
    )
    # Copy DLL to output directory
    add_custom_command(TARGET elden_ring_controller POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${INTERCEPTION_SDK_PATH}/library/x64/interception.dll"
            $<TARGET_FILE_DIR:elden_ring_controller>
    )
# For x86 builds
else()
    target_link_libraries(elden_ring_controller
        ${INTERCEPTION_SDK_PATH}/library/x86/interception.lib
    )
    add_custom_command(TARGET elden_ring_controller POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${INTERCEPTION_SDK_PATH}/library/x86/interception.dll"
            $<TARGET_FILE_DIR:elden_ring_controller>
    )
endif()
