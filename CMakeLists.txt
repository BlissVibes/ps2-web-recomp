cmake_minimum_required(VERSION 3.20)
project(ps2-web-recomp CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT EMSCRIPTEN)
    add_executable(eerecomp
        recomp/main.cpp
        recomp/r5900.cpp
        recomp/elf.cpp
        recomp/analysis.cpp
        recomp/codegen.cpp)
    target_compile_options(eerecomp PRIVATE -Wall -Wextra)
endif()
