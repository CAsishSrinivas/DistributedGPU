cmake_minimum_required(VERSION 3.25)
project(DistributedGPU VERSION 0.1.0 LANGUAGES CXX)

# ──────────────────────────────────────────────────────────────────────────────
# Global settings
# ──────────────────────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Put all output binaries in the same place so tests can find them easily.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# Treat warnings as errors in CI; override locally with -DWARNINGS_AS_ERRORS=OFF
option(WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)

if(MSVC)
    add_compile_options(/W4 /permissive-)
    if(WARNINGS_AS_ERRORS)
        add_compile_options(/WX)
    endif()
    # Suppress noisy MSVC warnings that are false positives with standard C++20
    add_compile_options(/wd4100)   # unreferenced formal parameter
    add_compile_options(/wd4324)   # structure was padded due to alignment specifier
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
    if(WARNINGS_AS_ERRORS)
        add_compile_options(-Werror)
    endif()
endif()

# ──────────────────────────────────────────────────────────────────────────────
# Find system dependencies
# ──────────────────────────────────────────────────────────────────────────────

# DirectX 12 — part of the Windows SDK; locate via environment variable or
# the default Windows SDK installation path.
if(WIN32)
    # Windows SDK provides d3d12.lib, dxgi.lib, d3dcompiler.lib.
    # CMake's FindDirectX module is not always present, so we locate manually.
    set(WIN_SDK_LIB_HINTS
        "$ENV{WindowsSdkDir}/Lib/$ENV{WindowsSDKVersion}/um/x64"
        "C:/Program Files (x86)/Windows Kits/10/Lib/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/um/x64"
        "C:/Program Files (x86)/Windows Kits/10/Lib/10.0.26100.0/um/x64"
        "C:/Program Files (x86)/Windows Kits/10/Lib/10.0.22621.0/um/x64"
    )
    find_library(D3D12_LIB   d3d12       HINTS ${WIN_SDK_LIB_HINTS})
    find_library(DXGI_LIB    dxgi        HINTS ${WIN_SDK_LIB_HINTS})
    find_library(D3DCOMP_LIB d3dcompiler HINTS ${WIN_SDK_LIB_HINTS})
    find_library(DXGUID_LIB  dxguid      HINTS ${WIN_SDK_LIB_HINTS})

    if(NOT D3D12_LIB)
        message(WARNING "d3d12.lib not found -- GPU modules will not be available. "
                        "Ensure the Windows SDK is installed.")
    endif()
endif()

# ──────────────────────────────────────────────────────────────────────────────
# Shared compile definitions
# ──────────────────────────────────────────────────────────────────────────────
add_library(dgpu_compile_options INTERFACE)
target_compile_definitions(dgpu_compile_options INTERFACE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    $<$<CONFIG:Debug>:DGPU_DEBUG>
)

# ──────────────────────────────────────────────────────────────────────────────
# Sub-projects (order matters for dependencies)
# ──────────────────────────────────────────────────────────────────────────────
add_subdirectory(libs/protocol)
add_subdirectory(libs/networking)
add_subdirectory(libs/telemetry)
add_subdirectory(libs/task_runtime)

if(WIN32 AND D3D12_LIB)
    add_subdirectory(libs/d3d12)
    set(D3D12_AVAILABLE TRUE)
else()
    set(D3D12_AVAILABLE FALSE)
    message(STATUS "Skipping d3d12 library (Windows + d3d12.lib required).")
endif()

add_subdirectory(apps/host)
add_subdirectory(apps/worker)
add_subdirectory(apps/benchmark)
add_subdirectory(apps/game_adapter)

# ──────────────────────────────────────────────────────────────────────────────
# Tests
# ──────────────────────────────────────────────────────────────────────────────
enable_testing()
add_subdirectory(tests/protocol_tests)
add_subdirectory(tests/networking_tests)
if(D3D12_AVAILABLE)
    add_subdirectory(tests/gpu_tests)
endif()
add_subdirectory(tests/benchmark_tests)
