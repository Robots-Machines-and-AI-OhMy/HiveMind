cmake_minimum_required(VERSION 3.20)

project(HiveMind)

set(CMAKE_CXX_STANDARD 20)

# ============================================================
# tiny-sha (SHA hashing library)
# ============================================================
set(TINY_SHA_ROOT "${CMAKE_SOURCE_DIR}/tiny-sha")

add_library(tiny_sha STATIC
        "${TINY_SHA_ROOT}/src/tiny_sha.c"
)

target_include_directories(tiny_sha PUBLIC
        "${TINY_SHA_ROOT}/src"
)

# Force C as language (important for MSVC consistency)
set_target_properties(tiny_sha PROPERTIES
        LINKER_LANGUAGE C
)

# ============================================================
# TOOLCHAIN GUARD
# This project requires MSVC (SEH, __declspec, Windows SDK).
# Build from any MSVC environment:
#   - "x64 Native Tools Command Prompt for VS 20xx"
#   - cmake -G Ninja -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl
# ============================================================
if (NOT MSVC)
    message(FATAL_ERROR
            "This project requires MSVC.\n"
            "MinGW and Clang are not supported.\n"
            "Run from an MSVC environment:\n"
            "  cmake -G Ninja -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl ..")
endif()

# ============================================================
# MSQuic — compiled in-repo with build.ps1 -Tls openssl
# Override with: cmake -DMSQUIC_ROOT=<path>
# ============================================================
set(MSQUIC_ROOT "${CMAKE_SOURCE_DIR}/msquic"
        CACHE PATH "Root of the in-repo MSQuic build")

set(MSQUIC_INCLUDE "${MSQUIC_ROOT}/src/inc")
set(MSQUIC_LIB     "${MSQUIC_ROOT}/artifacts/bin/windows/x64_Debug_openssl/msquic.lib")
set(MSQUIC_DLL     "${MSQUIC_ROOT}/artifacts/bin/windows/x64_Debug_openssl/msquic.dll")

if (NOT EXISTS "${MSQUIC_INCLUDE}/msquic.h")
    message(WARNING
            "MSQuic headers not found at ${MSQUIC_INCLUDE}\n"
            "Run: cd msquic && .\\build.ps1 -Tls openssl")
endif()

function(copy_msquic_dll TARGET)
    add_custom_command(TARGET ${TARGET} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${MSQUIC_DLL}" "$<TARGET_FILE_DIR:${TARGET}>"
            COMMENT "Copying msquic.dll for ${TARGET}")
endfunction()

# ============================================================
# NuRaft — compiled in-repo
# Override with: cmake -DNURAFT_ROOT=<path>
# ============================================================
set(NURAFT_ROOT "${CMAKE_SOURCE_DIR}/NuRaft"
        CACHE PATH "Root of the in-repo NuRaft build")

set(NURAFT_INCLUDE "${NURAFT_ROOT}/include")
set(NURAFT_LIB     "${NURAFT_ROOT}/build/nuraft.lib")
set(NURAFT_ASIO    "${NURAFT_ROOT}/asio/include")

if (NOT EXISTS "${NURAFT_INCLUDE}/libnuraft/nuraft.hxx")
    message(WARNING
            "NuRaft headers not found at ${NURAFT_INCLUDE}\n"
            "Run cmake + build inside NuRaft/")
endif()

function(link_nuraft TARGET)
    target_include_directories(${TARGET} PRIVATE
            "${NURAFT_INCLUDE}"
            "${NURAFT_ASIO}")
    target_link_libraries(${TARGET} PRIVATE "${NURAFT_LIB}")
endfunction()

# ============================================================
# Find mt.exe for UAC manifest embedding (install_hook only)
# ============================================================
find_program(MT_EXE mt.exe
        HINTS
        "$ENV{WindowsSdkVerBinPath}x64"
        "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64"
        "C:/Program Files (x86)/Windows Kits/10/bin/x64")

if (NOT MT_EXE)
    message(WARNING "mt.exe not found — UAC manifest will not be embedded in install_hook.exe")
endif()

# ============================================================
# Shared compile options applied to all HiveMind targets.
# WIN32_LEAN_AND_MEAN + NOMINMAX prevent Windows header collisions.
# /FI force-includes the PCH before any source line so winsock2.h
# always precedes windows.h regardless of individual file order.
# ============================================================
set(HiveMind_COMPILE_DEFS WIN32_LEAN_AND_MEAN NOMINMAX)
set(HiveMind_PCH_FLAG /FI${CMAKE_SOURCE_DIR}/HiveMind_pch.hpp)

# ============================================================
# hook.dll
# Core interceptor: IAT patch, process queue, OS filter, logging.
# Compiled as C (not C++) — set explicitly so MSVC agrees.
# ============================================================
add_library(hook SHARED hook.c)
set_source_files_properties(hook.c PROPERTIES LANGUAGE C)
target_compile_options(hook PRIVATE /EHa)   # SEH support for __try/__except
target_link_libraries(hook PRIVATE kernel32 psapi)

# ============================================================
# offload_hook.dll
# Loader: wires AskEngine + TransferEngine into hook.dll.
# AskEngine is live (calls NetworkManager::requestOffload).
# TransferEngine is NULL for demo — see Offload_Hook.cpp LINK comment.
# ============================================================
add_library(offload_hook SHARED
        Offload_Hook.cpp
        network.cpp
        Raft_Engine.cpp
        compute_check.cpp
        Calculate_Performance.cpp
        global.cpp
)
target_include_directories(offload_hook PRIVATE
        "${MSQUIC_INCLUDE}"
        "${NURAFT_INCLUDE}"
        "${NURAFT_ASIO}")
target_compile_definitions(offload_hook PRIVATE ${HiveMind_COMPILE_DEFS})
target_compile_options(offload_hook PRIVATE ${HiveMind_PCH_FLAG})
target_link_libraries(offload_hook PRIVATE kernel32 ws2_32 advapi32 pdh "${MSQUIC_LIB}" tiny_sha)
copy_msquic_dll(offload_hook)
link_nuraft(offload_hook)

# ============================================================
# install_hook.exe
# Writes/clears AppInit_DLLs registry keys (requires Administrator).
# UAC elevation requested via embedded manifest.
# ============================================================
add_executable(install_hook install_hook.cpp)
target_link_libraries(install_hook PRIVATE advapi32)
add_dependencies(install_hook hook offload_hook)

# Manifest is embedded post-build via mt.exe (works with Ninja + MSVC).
target_sources(install_hook PRIVATE install_hook.manifest)
set_source_files_properties(install_hook.manifest PROPERTIES
        COMPILE_FLAGS "" HEADER_FILE_ONLY TRUE)

if (MT_EXE)
    add_custom_command(TARGET install_hook POST_BUILD
            COMMAND "${MT_EXE}"
            -manifest "${CMAKE_CURRENT_SOURCE_DIR}/install_hook.manifest"
            "-outputresource:$<TARGET_FILE:install_hook>;1"
            COMMENT "Embedding UAC manifest into install_hook.exe")
endif()

# ============================================================
# test_runner.exe
# Loads offload_hook.dll, wires stub engines, launches test processes.
# ============================================================
add_executable(test_runner Hook_Test.cpp)
target_link_libraries(test_runner PRIVATE kernel32)
add_dependencies(test_runner hook offload_hook)

# Copy hook + offload_hook DLLs next to each exe that needs them.
foreach(TARGET_EXE test_runner install_hook)
    add_custom_command(TARGET ${TARGET_EXE} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:hook>
            $<TARGET_FILE:offload_hook>
            $<TARGET_FILE_DIR:${TARGET_EXE}>
            COMMENT "Copying DLLs for ${TARGET_EXE}")
endforeach()

# ============================================================
# remote_agent.exe
# Receives offloaded process bundles on target nodes.
# Uncomment when remote_agent.cpp is added to the project.
# ============================================================
# add_executable(remote_agent remote_agent.cpp)
# target_link_libraries(remote_agent PRIVATE ws2_32 kernel32)

# ============================================================
# HiveMind.exe — main CLI application
# ============================================================
add_executable(HiveMind
        HiveMindCLI.cpp
        compute_check.cpp
        Calculate_Performance.cpp
        global.cpp
        network.cpp
        Raft_Engine.cpp
)
target_include_directories(HiveMind PRIVATE
        "${MSQUIC_INCLUDE}"
        "${NURAFT_INCLUDE}"
        "${NURAFT_ASIO}")
target_compile_definitions(HiveMind PRIVATE ${HiveMind_COMPILE_DEFS})
target_compile_options(HiveMind PRIVATE ${HiveMind_PCH_FLAG})
target_link_libraries(HiveMind PRIVATE
        kernel32
        ws2_32
        advapi32
        pdh
        "${MSQUIC_LIB}"
        tiny_sha
)
copy_msquic_dll(HiveMind)
link_nuraft(HiveMind)

# ============================================================
# matrix_tests — standalone benchmark runner
# Requires a trivial main() in matrix_test_main.cpp:
#   #include "compute_check.hpp"
#   #include "global.hpp"
#   #include <cstdio>
#   double metric = 0.0; int LOCAL_RAFT_NODE_ID = 1;
#   double NODE_CAPACITY_WEIGHT = 1.0;
#   const char* LOCAL_RAFT_ENDPOINT_C = "";
#   const char* LOCAL_NODE_IP_C = "";
#   int main() { printf("metric = %f\n", metric_calculation()); }
# ============================================================
# add_executable(matrix_tests compute_check.cpp matrix_test_main.cpp)