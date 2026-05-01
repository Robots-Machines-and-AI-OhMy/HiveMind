cmake_minimum_required(VERSION 4.1)
project(MindMesh)

set(CMAKE_CXX_STANDARD 14)

include_directories(.)
include_directories(Winforms/MMWinforms)

add_executable(MindMesh
        Winforms/MMWinforms/AssemblyInfo.cpp
        Winforms/MMWinforms/CppCLR_WinFormsProject.cpp
        Winforms/MMWinforms/Form1.cpp
        Winforms/MMWinforms/Form1.h
        Winforms/MMWinforms/pch.cpp
        Winforms/MMWinforms/pch.h
        Winforms/MMWinforms/Resource.h
        containerization.cpp
        containerization.hpp
        HiveMindCLI.cpp
        network.cpp
        network.hpp
        Offload_Hook.cpp)
