cmake_minimum_required(VERSION 3.20)

project(MindMesh)

set(CMAKE_CXX_STANDARD 20)

# ============================================================
# TOOLCHAIN GUARD
# hook.c uses __try/__except (MSVC SEH) and __declspec().
# MinGW and Clang-cl do not support these — MSVC is required.
# In CLion: Settings -> Toolchains -> set to Visual Studio.
# ============================================================
if (NOT MSVC)
    message(FATAL_ERROR
            "This project requires MSVC.\n"
            "In CLion: Settings -> Build, Execution, Deployment\n"
            "          -> Toolchains -> select Visual Studio.\n"
            "MinGW and Clang are not supported.")
endif()

# /EHa (async SEH) is only needed for hook.c which uses __try/__except.
# Applied per-target to avoid D9025 warning from overriding CMake's
# default /EHs on C++ targets that don't need it.

# ============================================================
# hook.dll
# Core interceptor: IAT patch, process queue, OS filter, logging.
# Compiled as C (not C++) — set explicitly so CLion/MSVC agree.
# ============================================================
add_library(hook SHARED hook.c)

# Force hook.c to be treated as C regardless of file association
# settings in the IDE or toolchain defaults.
set_source_files_properties(hook.c PROPERTIES LANGUAGE C)

# Remove CMake's default /EHs and replace with /EHa for SEH support.
target_compile_options(hook PRIVATE /EHa)

target_link_libraries(hook PRIVATE kernel32 psapi)

# ============================================================
# offload_hook.dll
# Loader: wires AskEngine + TransferEngine stubs into hook.dll.
# ============================================================
add_library(offload_hook SHARED offload_hook.cpp)
target_link_libraries(offload_hook PRIVATE kernel32)

# ============================================================
# install_hook.exe
# Writes/clears AppInit_DLLs registry keys for system-wide
# injection.  Requires Administrator — the UAC manifest below
# causes Windows to prompt for elevation automatically so you
# do not need to launch it manually from an elevated prompt.
# ============================================================
add_executable(install_hook install_hook.cpp)
target_link_libraries(install_hook PRIVATE advapi32)

# Embed a UAC manifest requesting Administrator elevation.
# This triggers the UAC prompt when the exe is launched from
# CLion, Explorer, or the command line.
# UAC elevation via side-by-side manifest file.
# The manifest is embedded into install_hook.exe at link time by mt.exe.
# This avoids the LINK_FLAGS quoting issue that breaks Ninja + MSVC.
set_target_properties(install_hook PROPERTIES
        VS_USER_PROPS ""
)
target_sources(install_hook PRIVATE install_hook.manifest)
set_source_files_properties(install_hook.manifest PROPERTIES
        COMPILE_FLAGS ""          # not compiled
        HEADER_FILE_ONLY TRUE
)
# Embed manifest explicitly for Ninja (non-VS generator)
if (NOT CMAKE_GENERATOR MATCHES "Visual Studio")
    find_program(MT_EXE mt.exe
            HINTS
            "$ENV{WindowsSdkVerBinPath}x64"
            "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64"
            "C:/Program Files (x86)/Windows Kits/10/bin/x64"
    )
    if (MT_EXE)
        add_custom_command(TARGET install_hook POST_BUILD
                COMMAND "${MT_EXE}"
                -manifest "${CMAKE_CURRENT_SOURCE_DIR}/install_hook.manifest"
                "-outputresource:$<TARGET_FILE:install_hook>;1"
                COMMENT "Embedding UAC manifest into install_hook.exe"
        )
    else()
        message(WARNING
                "mt.exe not found — UAC manifest not embedded.\n"
                "install_hook.exe must be run manually as Administrator.")
    endif()
endif()

# ============================================================
# test_runner.exe
# Proof-of-life: loads offload_hook.dll, wires in stubs,
# launches processes, waits, then ejects.
# ============================================================
add_executable(test_runner Hook_Test.cpp)
target_link_libraries(test_runner PRIVATE kernel32)

# ============================================================
# POST-BUILD: copy DLLs next to each executable
#
# Also copies hook.dll to C:\Tools\ so install_hook.exe can
# register it at that stable path.  C:\Tools\ must exist;
# create it once manually or the copy is silently skipped.
#
# To register after build:
#   Run the install_hook target from CLion (UAC prompt appears)
#   — or run from an elevated cmd:
#   install_hook.exe install C:\Tools\hook.dll
# ============================================================
foreach(TARGET_EXE test_runner install_hook)
    add_custom_command(TARGET ${TARGET_EXE} POST_BUILD

            # Copy DLLs next to the executable
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:hook>
            $<TARGET_FILE:offload_hook>
            $<TARGET_FILE_DIR:${TARGET_EXE}>

            # Copy hook.dll to stable registration path.
            # Silent no-op if C:\Tools does not exist.
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:hook>
            "C:/Tools/hook.dll"

            COMMENT "Copying DLLs for ${TARGET_EXE}"
    )
endforeach()

# ============================================================
# MindMesh (teammate target — untouched)
# ============================================================
add_executable(MindMesh
        compute_check.cpp
        matrix_funcs.cpp
        global.cpp
        HiveMindCLI.cpp
        #network.cpp
)

add_executable(matrix_tests
        compute_check.cpp
)