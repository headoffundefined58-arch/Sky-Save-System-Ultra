cmake_minimum_required(VERSION 3.21)

# SSS-Ultra Save Doctor - standalone .exe, no SKSE, no CommonLibSSE, no third party.
project(SaveDoctor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /utf-8 /EHsc /permissive-")

# Release but keep it static so the exe has no runtime DLL dependency beyond
# the universal CRT (present on any Windows 10+ install).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:DEBUG>:Debug>")

add_executable(SaveDoctor
    main.cpp
)

target_include_directories(SaveDoctor PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

set_target_properties(SaveDoctor PROPERTIES
    OUTPUT_NAME "SSS-Ultra-SaveDoctor"
)
