# Standalone bring-up project for the Chili hardware GE renderer.
#
# It builds the lifted renderer (profiles/chili/host/gpu) and an offline tool
# that replays captured GE draws. Nothing here needs the generated AOT units,
# so a full build takes well under a minute.
#
#   cmake -S profiles/chili/gpu_dev -B out/chili-gpu -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build out/chili-gpu
cmake_minimum_required(VERSION 3.20)
project(ChiliGpuDev LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR "The Chili GE renderer is Direct3D 12 only")
endif()

get_filename_component(PSPRECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
get_filename_component(CHILI_HOST "${CMAKE_CURRENT_SOURCE_DIR}/../host" ABSOLUTE)

# Framework only: no profile, no tests.
set(PSPRECOMP_PROFILE "" CACHE STRING "" FORCE)
set(PSPRECOMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(PSPRECOMP_BUILD_PROFILE_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${PSPRECOMP_ROOT}" psprecomp EXCLUDE_FROM_ALL)

add_library(chili_gpu STATIC
    "${CHILI_HOST}/gpu/ge_renderer.cpp"
    "${CHILI_HOST}/gpu/ge_gpu_backend_dx12.cpp"
    "${CHILI_HOST}/gpu/ge_gpu_support.cpp")
target_include_directories(chili_gpu PUBLIC "${CHILI_HOST}/gpu" "${CHILI_HOST}")
target_link_libraries(chili_gpu PUBLIC psprecomp_core d3d12 dxgi d3dcompiler dxguid)
if(MSVC)
    target_compile_options(chili_gpu PRIVATE /W3 /permissive- /EHsc)
    target_compile_definitions(chili_gpu PRIVATE _CRT_SECURE_NO_WARNINGS)
    psprecomp_enable_host_avx(chili_gpu)
endif()

add_executable(chili_ge_dump_replay dump_replay_main.cpp replay_common.cpp selftest.cpp)
target_link_libraries(chili_ge_dump_replay PRIVATE chili_gpu windowscodecs ole32 user32 gdi32)
if(MSVC)
    target_compile_options(chili_ge_dump_replay PRIVATE /W3 /permissive- /EHsc /bigobj)
    target_compile_definitions(chili_ge_dump_replay PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# The whole-list replay tool (also built with the game by profiles/chili/CMakeLists.txt);
# here it links the same renderer library, so a renderer edit is a 30 s rebuild.
add_executable(chili_ge_replay "${CHILI_HOST}/../tools/chili_ge_replay.cpp" "${CHILI_HOST}/thread_runtime.cpp")
target_link_libraries(chili_ge_replay PRIVATE chili_gpu)
if(MSVC)
    target_compile_options(chili_ge_replay PRIVATE /permissive- /EHsc /bigobj)
    target_compile_definitions(chili_ge_replay PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
