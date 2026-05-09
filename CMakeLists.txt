cmake_minimum_required(VERSION 3.20)

project(MindMesh)

set(CMAKE_CXX_STANDARD 20)

# ============================================================
# TOOLCHAIN GUARD
# ============================================================
if (NOT MSVC)
    message(FATAL_ERROR
            "This project requires MSVC.\n"
            "In CLion: Settings -> Build, Execution, Deployment\n"
            "          -> Toolchains -> select Visual Studio.\n"
            "MinGW and Clang are not supported.")
endif()

# ============================================================
# MSQuic — compiled in-repo using build.ps1 with -Tls openssl.
# Headers and lib are relative to the project root.
# Adjust MSQUIC_ROOT if your repo layout differs.
# ============================================================
set(MSQUIC_ROOT "${CMAKE_SOURCE_DIR}/msquic"
        CACHE PATH "Root of the in-repo MSQuic build")

# Headers ship with the MSQuic source.
set(MSQUIC_INCLUDE "${MSQUIC_ROOT}/src/inc")

# The Ninja/MSVC build outputs to artifacts/bin/... — pick the
# config that matches CMAKE_BUILD_TYPE.
set(MSQUIC_LIB
        "${MSQUIC_ROOT}/artifacts/bin/windows/x64_Debug_openssl/msquic.lib")

# Also need the DLL next to the exe at runtime.
set(MSQUIC_DLL
        "${MSQUIC_ROOT}/artifacts/bin/windows/x64/$<IF:$<CONFIG:Debug>,debug,release>/msquic.dll")

if (NOT EXISTS "${MSQUIC_INCLUDE}/msquic.h")
    message(WARNING
            "MSQuic headers not found at ${MSQUIC_INCLUDE}.\n"
            "Run build.ps1 -Tls openssl inside the msquic/ subdirectory first.")
endif()

# Copy msquic.dll next to every executable that needs it.
function(copy_msquic_dll TARGET)
    add_custom_command(TARGET ${TARGET} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${MSQUIC_DLL}" "$<TARGET_FILE_DIR:${TARGET}>"
            COMMENT "Copying msquic.dll for ${TARGET}")
endfunction()

# ============================================================
# NuRaft — pre-installed or fetched.
# Uncomment FetchContent block when ready to pull automatically.
# ============================================================
# include(FetchContent)
# FetchContent_Declare(NuRaft
#     GIT_REPOSITORY https://github.com/eBay/NuRaft.git
#     GIT_TAG        master)
# FetchContent_MakeAvailable(NuRaft)

# NuRaft — in-repo build (sibling directory to msquic).
set(NURAFT_ROOT "${CMAKE_SOURCE_DIR}/NuRaft"
        CACHE PATH "Root of the in-repo NuRaft build")
# Headers: NuRaft keeps them under include/libnuraft/
set(NURAFT_INCLUDE "${NURAFT_ROOT}/include")
# Lib: produced as nuraft.lib (not libnuraft.lib) by the MSVC build.
set(NURAFT_LIB    "${NURAFT_ROOT}/build/nuraft.lib")
# Asio ships bundled with NuRaft — header-only, no separate build needed.
set(NURAFT_ASIO   "${NURAFT_ROOT}/asio/include")

if (NOT EXISTS "${NURAFT_INCLUDE}/libnuraft/nuraft.hxx")
    message(WARNING
            "NuRaft headers not found at ${NURAFT_INCLUDE}.\n"
            "Run cmake + build inside NuRaft/ first.")
endif()

# Helper: apply NuRaft includes/libs to a target.
function(link_nuraft TARGET)
    target_include_directories(${TARGET} PRIVATE
            "${NURAFT_INCLUDE}"
            "${NURAFT_ASIO}")
    target_link_libraries(${TARGET} PRIVATE "${NURAFT_LIB}")
endfunction()

# ============================================================
# /EHa — async SEH for hook.c; applied per-target only.
# ============================================================

# ============================================================
# hook.dll
# ============================================================
add_library(hook SHARED hook.c)
set_source_files_properties(hook.c PROPERTIES LANGUAGE C)
target_compile_options(hook PRIVATE /EHa)
target_link_libraries(hook PRIVATE kernel32 psapi)

# ============================================================
# offload_hook.dll
# Loader: wires AskEngine (live) + TransferEngine (stub) into hook.dll.
# Now includes Network.cpp + supporting files so AskEngine can
# call NetworkManager::requestOffload().
#
# LINK TransferEngine: when bundle transfer is ready, also add
#   transfer_engine.cpp and link ws2_32 psapi.
# ============================================================
add_library(offload_hook SHARED
        Offload_Hook.cpp
        network.cpp
        Raft_Engine.cpp
        compute_check.cpp
        global.cpp
)
target_include_directories(offload_hook PRIVATE "${MSQUIC_INCLUDE}" "${NURAFT_INCLUDE}" "${NURAFT_ASIO}")
target_link_libraries(offload_hook PRIVATE
        kernel32
        ws2_32
        advapi32
        "${MSQUIC_LIB}"
)
copy_msquic_dll(offload_hook)
link_nuraft(offload_hook)

# ============================================================
# install_hook.exe
# ============================================================
add_executable(install_hook install_hook.cpp)
target_link_libraries(install_hook PRIVATE advapi32)
add_dependencies(install_hook hook offload_hook)

set_target_properties(install_hook PROPERTIES VS_USER_PROPS "")
target_sources(install_hook PRIVATE install_hook.manifest)
set_source_files_properties(install_hook.manifest PROPERTIES
        COMPILE_FLAGS "" HEADER_FILE_ONLY TRUE)

if (NOT CMAKE_GENERATOR MATCHES "Visual Studio")
    find_program(MT_EXE mt.exe
            HINTS
            "$ENV{WindowsSdkVerBinPath}x64"
            "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64"
            "C:/Program Files (x86)/Windows Kits/10/bin/x64")
    if (MT_EXE)
        add_custom_command(TARGET install_hook POST_BUILD
                COMMAND "${MT_EXE}"
                -manifest "${CMAKE_CURRENT_SOURCE_DIR}/install_hook.manifest"
                "-outputresource:$<TARGET_FILE:install_hook>;1"
                COMMENT "Embedding UAC manifest into install_hook.exe")
    else()
        message(WARNING "mt.exe not found — UAC manifest not embedded.")
    endif()
endif()

# ============================================================
# test_runner.exe
# ============================================================
add_executable(test_runner Hook_Test.cpp)
target_link_libraries(test_runner PRIVATE kernel32)
add_dependencies(test_runner hook offload_hook)

# ============================================================
# remote_agent.exe  (uncomment when remote_agent.cpp is added)
# ============================================================
# add_executable(remote_agent remote_agent.cpp)
# target_link_libraries(remote_agent PRIVATE ws2_32 kernel32)

# ============================================================
# POST-BUILD: copy DLLs next to executables
# ============================================================
foreach(TARGET_EXE test_runner install_hook)
    add_custom_command(TARGET ${TARGET_EXE} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:hook>
            $<TARGET_FILE:offload_hook>
            $<TARGET_FILE_DIR:${TARGET_EXE}>
            COMMENT "Copying DLLs for ${TARGET_EXE}")
endforeach()

# ============================================================
# MindMesh — main application (CLI + full network stack)
# ============================================================
add_executable(MindMesh
        HiveMindCLI.cpp
        compute_check.cpp
        global.cpp
        Raft_Engine.cpp
        network.cpp
)
target_include_directories(MindMesh PRIVATE "${MSQUIC_INCLUDE}" "${NURAFT_INCLUDE}" "${NURAFT_ASIO}")
target_link_libraries(MindMesh PRIVATE
        kernel32
        ws2_32
        advapi32
        "${MSQUIC_LIB}"
)
copy_msquic_dll(MindMesh)
link_nuraft(MindMesh)