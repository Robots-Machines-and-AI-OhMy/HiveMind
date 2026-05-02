cmake_minimum_required(VERSION 3.20)

project(MindMesh)

set(CMAKE_CXX_STANDARD 20)

# --- hook.dll ---
add_library(hook SHARED hook.c)
target_link_libraries(hook PRIVATE kernel32)

# --- injector.dll ---
add_library(injector SHARED Offload_Hook.cpp)
target_link_libraries(injector PRIVATE kernel32)

# --- test executable ---
add_executable(test_runner Hook_Test.cpp)
target_link_libraries(test_runner PRIVATE kernel32)

# Ensure DLLs are next to exe after build
add_custom_command(TARGET test_runner POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:hook>
        $<TARGET_FILE:injector>
        $<TARGET_FILE_DIR:test_runner>
)

add_executable(MindMesh
        HiveMindCLI.cpp
        network.cpp
)