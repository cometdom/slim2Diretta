cmake_minimum_required(VERSION 3.10)
project(slim2diretta VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)

# ============================================
# Architecture Detection
# ============================================

message(STATUS "")
message(STATUS "═══════════════════════════════════════════════════════")
message(STATUS "  slim2diretta - Build Configuration")
message(STATUS "═══════════════════════════════════════════════════════")

execute_process(
    COMMAND uname -m
    OUTPUT_VARIABLE UNAME_M
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

# Detect base architecture
if(UNAME_M STREQUAL "x86_64")
    set(BASE_ARCH "x64")
    set(ARCH_DESC_BASE "x86_64 (Intel/AMD 64-bit)")
elseif(UNAME_M STREQUAL "aarch64" OR UNAME_M STREQUAL "arm64")
    set(BASE_ARCH "aarch64")
    set(ARCH_DESC_BASE "ARM64 (aarch64)")
elseif(UNAME_M STREQUAL "armv7l")
    set(BASE_ARCH "arm")
    set(ARCH_DESC_BASE "ARM 32-bit (armv7l)")
    message(WARNING "ARM 32-bit detected but not officially supported by Diretta SDK")
elseif(UNAME_M STREQUAL "riscv64")
    set(BASE_ARCH "riscv64")
    set(ARCH_DESC_BASE "RISC-V 64-bit")
else()
    set(BASE_ARCH "unknown")
    set(ARCH_DESC_BASE "Unknown: ${UNAME_M}")
endif()

message(STATUS "Architecture: ${ARCH_DESC_BASE}")

# ============================================
# Architecture-Specific Variant Detection
# ============================================

if(BASE_ARCH STREQUAL "x64")
    # x64: Auto-detect CPU capabilities
    execute_process(
        COMMAND grep -q avx2 /proc/cpuinfo
        RESULT_VARIABLE HAS_AVX2_RESULT
    )
    execute_process(
        COMMAND grep -q avx512 /proc/cpuinfo
        RESULT_VARIABLE HAS_AVX512_RESULT
    )
    execute_process(
        COMMAND sh -c "lscpu 2>/dev/null | grep -qi 'AMD.*Zen 4'"
        RESULT_VARIABLE IS_ZEN4_RESULT
    )

    if(IS_ZEN4_RESULT EQUAL 0)
        set(DEFAULT_VARIANT "x64-linux-15zen4")
        set(CPU_DESC "AMD Zen 4 detected")
    elseif(HAS_AVX512_RESULT EQUAL 0)
        set(DEFAULT_VARIANT "x64-linux-15v4")
        set(CPU_DESC "AVX512 detected (x86-64-v4)")
    elseif(HAS_AVX2_RESULT EQUAL 0)
        set(DEFAULT_VARIANT "x64-linux-15v3")
        set(CPU_DESC "AVX2 detected (x86-64-v3)")
    else()
        set(DEFAULT_VARIANT "x64-linux-15v2")
        set(CPU_DESC "Basic x64 (x86-64-v2)")
    endif()

elseif(BASE_ARCH STREQUAL "aarch64")
    # aarch64: Prefer 16K page-size detection (k16), fallback to model (RPi5/CM5)
    set(IS_K16 FALSE)
    set(PAGE_SIZE "")
    set(DT_MODEL "")

    execute_process(
        COMMAND getconf PAGESIZE
        OUTPUT_VARIABLE PAGE_SIZE
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )

    if(PAGE_SIZE STREQUAL "16384")
        set(IS_K16 TRUE)
        set(CPU_DESC "ARM64 with 16KB pages detected via getconf PAGESIZE")
    else()
        if(EXISTS "/proc/device-tree/model")
            execute_process(
                COMMAND sh -c "tr -d '\\0' < /proc/device-tree/model"
                OUTPUT_VARIABLE DT_MODEL
                OUTPUT_STRIP_TRAILING_WHITESPACE
            )
        elseif(EXISTS "/sys/firmware/devicetree/base/model")
            execute_process(
                COMMAND sh -c "tr -d '\\0' < /sys/firmware/devicetree/base/model"
                OUTPUT_VARIABLE DT_MODEL
                OUTPUT_STRIP_TRAILING_WHITESPACE
            )
        endif()

        if(DT_MODEL MATCHES "Raspberry Pi 5" OR DT_MODEL MATCHES "Raspberry Pi Compute Module 5")
            set(IS_K16 TRUE)
            set(CPU_DESC "${DT_MODEL} detected (using k16 variant)")
        endif()
    endif()

    if(IS_K16)
        set(DEFAULT_VARIANT "aarch64-linux-15k16")
        if(CPU_DESC STREQUAL "")
            set(CPU_DESC "ARM64 16KB pages (k16 variant)")
        endif()
    else()
        set(DEFAULT_VARIANT "aarch64-linux-15")
        if(DT_MODEL STREQUAL "")
            set(CPU_DESC "ARM64 with 4KB pages (standard variant)")
        else()
            set(CPU_DESC "ARM64 (${DT_MODEL}) with 4KB pages (standard variant)")
        endif()
    endif()

elseif(BASE_ARCH STREQUAL "riscv64")
    set(DEFAULT_VARIANT "riscv64-linux-15")
    set(CPU_DESC "RISC-V 64-bit")

else()
    set(DEFAULT_VARIANT "unknown")
    set(CPU_DESC "Unknown architecture")
endif()

# Allow manual override via -DARCH_NAME=...
if(NOT DEFINED ARCH_NAME)
    set(FULL_VARIANT ${DEFAULT_VARIANT})
else()
    set(FULL_VARIANT ${ARCH_NAME})
endif()

message(STATUS "Variant: ${FULL_VARIANT}")
message(STATUS "CPU: ${CPU_DESC}")

# ============================================
# Diretta SDK Auto-Detection
# ============================================

if(DEFINED ENV{DIRETTA_SDK_PATH})
    set(SDK_PATH $ENV{DIRETTA_SDK_PATH})
    message(STATUS "Using SDK from environment: ${SDK_PATH}")
else()
    # Search in common locations (SDK 148 preferred, then 147)
    set(SDK_SEARCH_PATHS
        "$ENV{HOME}/DirettaHostSDK_148"
        "${CMAKE_SOURCE_DIR}/DirettaHostSDK_148"
        "${CMAKE_SOURCE_DIR}/../DirettaHostSDK_148"
        "/opt/DirettaHostSDK_148"
        "$ENV{HOME}/audio/DirettaHostSDK_148"
        "/usr/local/DirettaHostSDK_148"
        "$ENV{HOME}/DirettaHostSDK_147"
        "${CMAKE_SOURCE_DIR}/DirettaHostSDK_147"
        "${CMAKE_SOURCE_DIR}/../DirettaHostSDK_147"
        "/opt/DirettaHostSDK_147"
        "$ENV{HOME}/audio/DirettaHostSDK_147"
        "/usr/local/DirettaHostSDK_147"
    )

    foreach(path ${SDK_SEARCH_PATHS})
        if(EXISTS "${path}")
            set(SDK_PATH "${path}")
            break()
        endif()
    endforeach()

    if(NOT DEFINED SDK_PATH)
        message(FATAL_ERROR
            "\n"
            "═══════════════════════════════════════════════════════\n"
            "  Diretta SDK not found!\n"
            "═══════════════════════════════════════════════════════\n"
            "\n"
            "Searched in:\n"
            "  ${SDK_SEARCH_PATHS}\n"
            "\n"
            "Please:\n"
            "  1. Download SDK from: https://www.diretta.link/hostsdk.html\n"
            "  2. Extract to one of the above locations, OR\n"
            "  3. Set environment variable: export DIRETTA_SDK_PATH=/path/to/sdk\n"
        )
    else()
        message(STATUS "SDK auto-detected: ${SDK_PATH}")
    endif()
endif()

# ============================================
# Construct Library Names
# ============================================

set(DIRETTA_LIB_NAME "libDirettaHost_${FULL_VARIANT}.a")
set(ACQUA_LIB_NAME "libACQUA_${FULL_VARIANT}.a")

set(SDK_LIB_DIRETTA "${SDK_PATH}/lib/${DIRETTA_LIB_NAME}")
set(SDK_LIB_ACQUA "${SDK_PATH}/lib/${ACQUA_LIB_NAME}")

# ============================================
# Verify SDK Installation
# ============================================

if(NOT EXISTS "${SDK_LIB_DIRETTA}")
    file(GLOB AVAILABLE_LIBS "${SDK_PATH}/lib/libDirettaHost_*.a")
    set(AVAILABLE_VARIANTS "")
    foreach(lib ${AVAILABLE_LIBS})
        get_filename_component(lib_name ${lib} NAME)
        string(REGEX REPLACE "libDirettaHost_(.*)\\.a" "\\1" variant ${lib_name})
        list(APPEND AVAILABLE_VARIANTS "  - ${variant}")
    endforeach()
    string(REPLACE ";" "\n" AVAILABLE_VARIANTS_STR "${AVAILABLE_VARIANTS}")

    message(FATAL_ERROR
        "\n"
        "═══════════════════════════════════════════════════════\n"
        "  Required library not found: ${DIRETTA_LIB_NAME}\n"
        "═══════════════════════════════════════════════════════\n"
        "\n"
        "Path checked: ${SDK_LIB_DIRETTA}\n"
        "\n"
        "Available libraries in SDK:\n"
        "${AVAILABLE_VARIANTS_STR}\n"
        "\n"
        "Common solutions:\n"
        "\n"
        "  For Raspberry Pi:\n"
        "    cmake -DARCH_NAME=aarch64-linux-15 ..\n"
        "    cmake -DARCH_NAME=aarch64-linux-15k16 ..\n"
        "\n"
        "  For x64 systems:\n"
        "    cmake -DARCH_NAME=x64-linux-15v2 ..       # Baseline\n"
        "    cmake -DARCH_NAME=x64-linux-15v3 ..       # AVX2 (most common)\n"
        "    cmake -DARCH_NAME=x64-linux-15v4 ..       # AVX512\n"
        "    cmake -DARCH_NAME=x64-linux-15zen4 ..     # AMD Ryzen 7000+\n"
        "\n"
        "  For RISC-V:\n"
        "    cmake -DARCH_NAME=riscv64-linux-15 ..\n"
    )
endif()

if(NOT EXISTS "${SDK_LIB_ACQUA}")
    message(WARNING "ACQUA library not found: ${ACQUA_LIB_NAME}")
endif()

# Check for SDK headers (SDK 148 uses different path than 147)
if(EXISTS "${SDK_PATH}/Host/Sync.hpp")
    message(STATUS "SDK 148 headers detected")
    set(SDK_VERSION "148")
elseif(EXISTS "${SDK_PATH}/Host/Diretta/SyncBuffer")
    message(STATUS "SDK 147 headers detected")
    set(SDK_VERSION "147")
else()
    message(FATAL_ERROR "SDK headers not found at: ${SDK_PATH}/Host/")
endif()

message(STATUS "SDK validation passed")
message(STATUS "Library: ${DIRETTA_LIB_NAME}")
message(STATUS "")
message(STATUS "═══════════════════════════════════════════════════════")
message(STATUS "")

# ============================================
# Find Required Packages
# ============================================

find_package(Threads REQUIRED)

# Find libFLAC (prefer pkg-config, fallback to find_library)
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(FLAC flac)
endif()
if(NOT FLAC_FOUND)
    find_library(FLAC_LIB NAMES FLAC)
    find_path(FLAC_INC NAMES FLAC/stream_decoder.h)
    if(FLAC_LIB AND FLAC_INC)
        set(FLAC_LIBRARIES ${FLAC_LIB})
        set(FLAC_INCLUDE_DIRS ${FLAC_INC})
        set(FLAC_FOUND TRUE)
        message(STATUS "libFLAC found via find_library: ${FLAC_LIB}")
    else()
        message(FATAL_ERROR
            "libFLAC not found! Install with:\n"
            "  Ubuntu/Debian: sudo apt-get install libflac-dev\n"
            "  Fedora: sudo dnf install flac-devel\n"
            "  Arch: sudo pacman -S flac\n"
        )
    endif()
endif()

# ============================================
# Include Directories
# ============================================

include_directories(
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/diretta
    ${SDK_PATH}/Host
    ${FLAC_INCLUDE_DIRS}
)

# ============================================
# Link Directories
# ============================================

link_directories(
    ${SDK_PATH}/lib
    ${FLAC_LIBRARY_DIRS}
)

# ============================================
# Sources
# ============================================

set(SLIM2DIRETTA_SOURCES
    src/main.cpp
    src/SlimprotoClient.cpp
    src/HttpStreamClient.cpp
    src/Decoder.cpp
    src/FlacDecoder.cpp
    src/PcmDecoder.cpp
    src/DsdProcessor.cpp
    src/DsdStreamReader.cpp
    diretta/DirettaSync.cpp
    diretta/globals.cpp
)

# ============================================
# Create Executable
# ============================================

add_executable(slim2diretta
    ${SLIM2DIRETTA_SOURCES}
)

# ============================================
# Link Libraries
# ============================================

target_link_libraries(slim2diretta
    ${CMAKE_THREAD_LIBS_INIT}
    ${SDK_LIB_DIRETTA}
    ${FLAC_LIBRARIES}
    dl
)

# Link ACQUA if available
if(EXISTS "${SDK_LIB_ACQUA}")
    target_link_libraries(slim2diretta ${SDK_LIB_ACQUA})
    message(STATUS "ACQUA library will be linked")
endif()

# ============================================
# Compiler Flags
# ============================================

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -O2")
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Architecture-specific SIMD flags
# TARGET_MARCH allows cross-compilation: cmake -DTARGET_MARCH=v3 ..
# Values: v2, v3, v4, zen4, native (default: auto-detect from ARCH_NAME or CPU)
if(BASE_ARCH STREQUAL "x64")
    if(DEFINED TARGET_MARCH)
        set(MARCH_LEVEL ${TARGET_MARCH})
        message(STATUS "SIMD: Using TARGET_MARCH=${TARGET_MARCH}")
    elseif(DEFINED ARCH_NAME)
        if(ARCH_NAME MATCHES "zen4$")
            set(MARCH_LEVEL "zen4")
        elseif(ARCH_NAME MATCHES "v4$")
            set(MARCH_LEVEL "v4")
        elseif(ARCH_NAME MATCHES "v3$")
            set(MARCH_LEVEL "v3")
        elseif(ARCH_NAME MATCHES "v2$")
            set(MARCH_LEVEL "v2")
        else()
            set(MARCH_LEVEL "native")
        endif()
        message(STATUS "SIMD: Derived from ARCH_NAME=${ARCH_NAME} -> ${MARCH_LEVEL}")
    else()
        if(IS_ZEN4_RESULT EQUAL 0)
            set(MARCH_LEVEL "zen4")
        elseif(HAS_AVX512_RESULT EQUAL 0)
            set(MARCH_LEVEL "v4")
        elseif(HAS_AVX2_RESULT EQUAL 0)
            set(MARCH_LEVEL "v3")
        else()
            set(MARCH_LEVEL "v2")
        endif()
        message(STATUS "SIMD: Auto-detected from CPU -> ${MARCH_LEVEL}")
    endif()

    if(MARCH_LEVEL STREQUAL "zen4")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=znver4 -mtune=znver4")
        message(STATUS "SIMD: AMD Zen 4 optimizations enabled")
    elseif(MARCH_LEVEL STREQUAL "v4")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=x86-64-v4 -mavx512f -mavx512bw -mavx512vl -mavx512dq")
        message(STATUS "SIMD: AVX-512 optimizations enabled")
    elseif(MARCH_LEVEL STREQUAL "v3")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=x86-64-v3 -mavx2 -mfma")
        message(STATUS "SIMD: AVX2 optimizations enabled")
    elseif(MARCH_LEVEL STREQUAL "v2")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=x86-64-v2")
        message(STATUS "SIMD: x86-64-v2 baseline")
    elseif(MARCH_LEVEL STREQUAL "native")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=native")
        message(STATUS "SIMD: Native CPU optimizations")
    endif()
elseif(BASE_ARCH STREQUAL "aarch64")
    if(DEFINED TARGET_MARCH AND NOT TARGET_MARCH STREQUAL "native")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mcpu=${TARGET_MARCH}")
        message(STATUS "SIMD: ARM64 -mcpu=${TARGET_MARCH}")
    else()
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mcpu=native")
        message(STATUS "SIMD: ARM64 native optimizations enabled")
    endif()
endif()

# ============================================
# Production Build (NOLOG)
# ============================================

option(NOLOG "Disable SDK internal logging for production builds" OFF)
if(NOLOG)
    target_compile_definitions(slim2diretta PRIVATE NOLOG)
    message(STATUS "NOLOG: SDK logging disabled (production build)")
endif()

# ============================================
# Install
# ============================================

install(TARGETS slim2diretta DESTINATION bin)

# Systemd service files: install manually with
#   sudo cp slim2diretta@.service /etc/systemd/system/
#   sudo cp slim2diretta.default /etc/default/slim2diretta
#   sudo systemctl daemon-reload

# ============================================
# Print Summary
# ============================================

message(STATUS "")
message(STATUS "═══════════════════════════════════════════════════════")
message(STATUS "  Build Configuration Summary")
message(STATUS "═══════════════════════════════════════════════════════")
message(STATUS "")
message(STATUS "System:")
message(STATUS "  uname -m:       ${UNAME_M}")
message(STATUS "  Base Arch:      ${BASE_ARCH}")
message(STATUS "")
message(STATUS "Selected:")
message(STATUS "  Variant:        ${FULL_VARIANT}")
message(STATUS "  Library:        ${DIRETTA_LIB_NAME}")
message(STATUS "")
message(STATUS "SDK:")
message(STATUS "  Path:           ${SDK_PATH}")
message(STATUS "  Diretta Lib:    ${SDK_LIB_DIRETTA}")
message(STATUS "  ACQUA Lib:      ${SDK_LIB_ACQUA}")
message(STATUS "")
message(STATUS "Dependencies:")
message(STATUS "  libFLAC:        ${FLAC_LIBRARIES}")
message(STATUS "")
message(STATUS "Build:")
message(STATUS "  Compiler:       ${CMAKE_CXX_COMPILER}")
message(STATUS "  Target:         slim2diretta")
message(STATUS "")
message(STATUS "═══════════════════════════════════════════════════════")
message(STATUS "")
