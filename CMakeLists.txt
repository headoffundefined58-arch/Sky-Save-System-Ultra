cmake_minimum_required(VERSION 3.21)

# Runtime library must be decided before any target exists.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:DEBUG>:Debug>DLL")

# SkySaveSystemUltra.  Name carries no spaces: SKSE module names, MO2 virtual
# paths and every third-party log parser handle them more predictably this way.
project(
    SkySaveSystemUltra
    VERSION 2.0.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# /utf-8: spdlog's bundled fmt refuses to compile without it.
# /EHsc:  CommonLibSSE does not set it and MSVC warns C4530 otherwise.
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /utf-8 /EHsc /permissive-")

# Release keeps full optimisation but also emits a .pdb (CodeView RSDS record)
# so CrashLoggerSSE frames stay symbolised.
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /Zi /FS")
set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} /DEBUG:FULL")

# Header-only spdlog assembled under D:/dev/deps (vcpkg is unusable on this
# machine: CreateFileW on named pipes is blocked by the sandbox).
list(APPEND CMAKE_PREFIX_PATH "D:/dev/deps")

set(SKSE_SUPPORT_XBYAK OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)

# Version selection is driven by the build script via -DENABLE_SKYRIM_* on the
# cmake command line.  The defaults below are intentionally NON-FORCE so the
# script's -D values win (a FORCE here would crush the -D override).
#   SE build : -DENABLE_SKYRIM_SE=ON  -DENABLE_SKYRIM_AE=OFF
#   AE build : -DENABLE_SKYRIM_SE=ON  -DENABLE_SKYRIM_AE=ON
set(ENABLE_SKYRIM_SE ON CACHE BOOL "" )
set(ENABLE_SKYRIM_AE ON CACHE BOOL "" )
set(ENABLE_SKYRIM_VR OFF CACHE BOOL "" )

# CommonLibSSE-NG still has a handful of RE headers (TES, BGSSaveLoadManager,
# CombatController, InterfaceStrings, TESObjectCELL ...) that pick their member
# layout from the legacy SKYRIM_SUPPORT_AE macro instead of ENABLE_SKYRIM_AE.
# Building for AE MUST define it; building for SE MUST NOT, or every engine
# struct (TES::gridCells, GetCell, ForEachReference ...) silently reads the wrong
# field.  The two layouts are compile-time, so SE and AE need separate binaries
# -- a single DLL cannot be struct-correct for both runtimes.
if(ENABLE_SKYRIM_AE)
    add_compile_definitions(SKYRIM_SUPPORT_AE=1)
endif()

add_subdirectory("D:/dev/CommonLibSSE-NG" "${CMAKE_CURRENT_BINARY_DIR}/CommonLibSSE-NG")

add_library(${PROJECT_NAME} SHARED
    src/main.cpp
    src/Ini.cpp
    src/Audit.cpp
    src/SaveGuard.cpp
    src/Trend.cpp
    src/FormRef.cpp
    # Vault.cpp is intentionally NOT built.  Its "persist any key forever"
    # implementation has been replaced by the classified StateStore below;
    # Vault.h survives only as a namespace alias for the consumer modules.
    src/StateStore.cpp
    src/StateStoreEngine.cpp
    src/LoadOrder.cpp
    src/Migrate.cpp
    src/Notify.cpp
    src/NotifyHud.cpp
    src/SavePathScan.cpp
    src/InitEngine.cpp
    src/Repair.cpp
    src/SaveDoctor.cpp
)

target_include_directories(${PROJECT_NAME} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/doctor"
)

target_link_libraries(${PROJECT_NAME}
    PRIVATE
        CommonLibSSE::CommonLibSSE
        # SHGetFolderPathW is declared by hand in SaveGuard.cpp so that no
        # translation unit has to include <windows.h> or <shlobj.h>.
        shell32
)

target_precompile_headers(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/PCH.h")

set_target_properties(${PROJECT_NAME} PROPERTIES
    # The artefact carries the full product name; the CMake target cannot
    # contain spaces, so only the output file name is spelled out.
    OUTPUT_NAME "SkySaveSystemUltra"
    PREFIX ""
)

# A plain console program that exercises the byte layer on its own.  It needs
# neither Skyrim nor SKSE, so the failure handling of the reader can be proven
# instead of asserted.  Build target: CodecTest.
add_executable(CodecTest tools/codec_test.cpp)
target_include_directories(CodecTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
set_target_properties(CodecTest PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tools"
)

# Proves the save-directory discovery against the real disk.  Build: PathTest.
add_executable(PathTest tools/path_test.cpp src/SavePathScan.cpp)
target_include_directories(PathTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
set_target_properties(PathTest PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tools"
)

# Proves the notification wrapper: it links the engine-free part of the
# wrapper only, so it runs anywhere.  Build target: NotifyTest.
add_executable(NotifyTest tools/notify_test.cpp src/Notify.cpp)
target_include_directories(NotifyTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
set_target_properties(NotifyTest PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tools"
)

# Proves the .ess header parser against a real save's first 32 bytes, so a
# healthy save can never again be reported as "version 1447121759".
add_executable(HeaderTest tools/header_test.cpp)
target_include_directories(HeaderTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
set_target_properties(HeaderTest PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tools"
)

# Proves the append-only init journal: hash chain, torn-tail repair and the
# write-ahead receipt that keeps load-time initialisation idempotent without
# requiring the player to save.  Build target: JournalTest.
add_executable(JournalTest tools/journal_test.cpp)
target_include_directories(JournalTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
set_target_properties(JournalTest PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tools"
)

# Proves the complete reconstructed save/load lifecycle without Skyrim:
# classification, Create / Load / Update / Commit, field-level damage
# isolation, graded fault recovery, and the post-commit read-back comparison.
# StateStore.cpp is engine-free, so it links directly here.  Build: StateTest.
add_executable(StateTest tools/state_test.cpp src/StateStore.cpp)
target_include_directories(StateTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
set_target_properties(StateTest PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tools"
)

# Proves the load-time repair policy and the one-txt/one-log retention rule
# without Skyrim: no engine, no SKSE, just a real temporary directory.
add_executable(RepairTest tools/repair_test.cpp)
target_include_directories(RepairTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
set_target_properties(RepairTest PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tools"
)

# ---------------------------------------------------------------------------
# SSS-Ultra Save Doctor - standalone .exe that repairs a broken .ess on the file
# layer before the engine parses it.  No SKSE, no CommonLibSSE, no third
# party: only doctor/lz4.hpp + doctor/ess.hpp + doctor/main.cpp.  It is
# launched silently by the DLL at pre-load when a save references removed
# plugins.  Build target: SaveDoctor.
# ---------------------------------------------------------------------------
add_executable(SaveDoctor
    doctor/main.cpp
)
target_include_directories(SaveDoctor PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/doctor")
set_target_properties(SaveDoctor PROPERTIES
    OUTPUT_NAME "SSS-Ultra-SaveDoctor"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/doctor"
)
