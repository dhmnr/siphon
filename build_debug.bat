@echo off

REM Install dependencies if needed
if not exist "build\generators\conan_toolchain.cmake" (
    echo Installing dependencies...
    if not exist build mkdir build
    cd build
    conan install .. --build=missing -s build_type=Debug || exit /b 1
    cd ..
)

REM Configure and build
cmake --preset debug || exit /b 1
cmake --build --preset debug || exit /b 1

echo Build complete: build\Debug\

