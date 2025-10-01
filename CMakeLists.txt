cmake_minimum_required(VERSION 3.10)
project(Siphon)

set(CMAKE_CXX_STANDARD 17)
# set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Set CMake policies for Conan compatibility
if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

include(${CMAKE_BINARY_DIR}/generators/conan_toolchain.cmake)

# Find required packages
find_package(Protobuf REQUIRED)
find_package(gRPC REQUIRED)
find_package(tomlplusplus CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)


# Generate protobuf and gRPC files
set(PROTO_FILES siphon_service.proto)
set(GENERATED_PROTOBUF_PATH "${CMAKE_BINARY_DIR}/generated")
file(MAKE_DIRECTORY ${GENERATED_PROTOBUF_PATH})

# Generate protobuf files
protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS ${PROTO_FILES})

# Generate gRPC files
set(GRPC_SRCS "${GENERATED_PROTOBUF_PATH}/siphon_service.grpc.pb.cc")
set(GRPC_HDRS "${GENERATED_PROTOBUF_PATH}/siphon_service.grpc.pb.h")

set(INTERCEPTION_SDK_PATH "${CMAKE_SOURCE_DIR}/external/interception")

if(TARGET protobuf::protoc)
    get_target_property(PROTOC_LOCATION protobuf::protoc IMPORTED_LOCATION)
    message(STATUS "protoc location: ${PROTOC_LOCATION}")
    
    # Try to get version
    execute_process(
        COMMAND ${PROTOC_LOCATION} --version
        OUTPUT_VARIABLE PROTOC_VERSION
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    message(STATUS "protoc version: ${PROTOC_VERSION}")
endif()

add_custom_command(
    OUTPUT "${GRPC_SRCS}" "${GRPC_HDRS}"
    COMMAND $<TARGET_FILE:protobuf::protoc>
    ARGS --grpc_out "${GENERATED_PROTOBUF_PATH}"
         --cpp_out "${GENERATED_PROTOBUF_PATH}"
         -I "${CMAKE_CURRENT_SOURCE_DIR}"
         --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
         "${CMAKE_CURRENT_SOURCE_DIR}/${PROTO_FILES}"
    DEPENDS ${PROTO_FILES} protobuf::protoc gRPC::grpc_cpp_plugin
)

# Include directories
message("GENERATED_PROTOBUF_PATH: ${GENERATED_PROTOBUF_PATH}")
message("Protobuf_INCLUDE_DIRS: ${Protobuf_INCLUDE_DIRS}")
message("gRPC_INCLUDE_DIRS: ${gRPC_INCLUDE_DIRS}")

include_directories(${GENERATED_PROTOBUF_PATH})
include_directories(${Protobuf_INCLUDE_DIRS})
include_directories(${gRPC_INCLUDE_DIRS})
include_directories(${tomlplusplus_INCLUDE_DIRS})
include_directories(${spdlog_INCLUDE_DIRS})
include_directories(${INTERCEPTION_SDK_PATH}/include)

# Server executable
add_executable(siphon
    src/main.cpp 
    src/utils.cpp 
    src/process_memory.cpp
    src/process_input.cpp
    src/server.cpp
    ${PROTO_SRCS}
    ${GRPC_SRCS}
)


# Add include directory

target_include_directories(siphon PRIVATE include)

# Link Windows libraries
target_link_libraries(siphon 
    psapi 
    user32 
    gRPC::grpc++ 
    protobuf::libprotobuf
    tomlplusplus::tomlplusplus
    spdlog::spdlog
)

# Link Interception library
# For x64 builds
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    target_link_libraries(siphon
        ${INTERCEPTION_SDK_PATH}/library/x64/interception.lib
    )
    # Copy DLL to output directory
    add_custom_command(TARGET siphon POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${INTERCEPTION_SDK_PATH}/library/x64/interception.dll"
            $<TARGET_FILE_DIR:siphon>
    )
# For x86 builds
else()
    target_link_libraries(siphon
        ${INTERCEPTION_SDK_PATH}/library/x86/interception.lib
    )
    add_custom_command(TARGET siphon POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${INTERCEPTION_SDK_PATH}/library/x86/interception.dll"
            $<TARGET_FILE_DIR:siphon>
    )
endif()


# Client executable
add_executable(siphon_client
    src/client.cpp
    ${PROTO_SRCS}
    ${GRPC_SRCS}
)

target_link_libraries(siphon_client
    gRPC::grpc++
    protobuf::libprotobuf
)