rmdir /s /q build
mkdir build && cd build
conan install .. --build=missing -s build_type=Release

cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release