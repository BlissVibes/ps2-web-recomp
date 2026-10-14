cmake_minimum_required(VERSION 3.20)
project(ps2-web-recomp CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(PS2_GENERATED "" CACHE PATH "Folder with the C++ generated for a game")

# A multiply is never fused into an add: ps2/vu_exact.hpp needs every float operation rounded on its own.
add_compile_options(-ffp-contract=off)

if(EMSCRIPTEN)
    add_compile_options(-msimd128 -mtail-call -fwasm-exceptions -sWASM_LEGACY_EXCEPTIONS=0 -pthread)
    add_link_options(-msimd128 -mtail-call -fwasm-exceptions -sWASM_LEGACY_EXCEPTIONS=0 -pthread)
endif()

if(NOT EMSCRIPTEN)
    add_executable(eerecomp
        recomp/main.cpp
        recomp/r5900.cpp
        recomp/elf.cpp
        recomp/analysis.cpp
        recomp/codegen.cpp
        recomp/snr2.cpp)
    target_compile_options(eerecomp PRIVATE -Wall -Wextra)

    add_executable(vujit_dump recomp/vujit_dump.cpp runtime/src/vu_jit.cpp)
    target_include_directories(vujit_dump PRIVATE runtime/include runtime/src)
    add_executable(vurecomp recomp/vurecomp.cpp)
    target_include_directories(vurecomp PRIVATE runtime/include)
    target_compile_options(vurecomp PRIVATE -Wall -Wextra)
endif()

add_library(ps2runtime STATIC
    runtime/src/memory.cpp
    runtime/src/dispatch.cpp
    runtime/src/system.cpp
    runtime/src/kernel.cpp
    runtime/src/harness.cpp
    runtime/src/hooks.cpp
    runtime/src/hw.cpp
    runtime/src/cop.cpp
    runtime/src/disc.cpp
    runtime/src/dmac.cpp
    runtime/src/vif.cpp
    runtime/src/vu_micro.cpp
    runtime/src/vu_jit.cpp
    runtime/src/vu_jit_web.cpp
    runtime/src/ipu/ipu.cpp
    runtime/src/gs/gs.cpp
    runtime/src/gs/gif.cpp
    runtime/src/gs/queue.cpp
    runtime/src/gs/raster.cpp
    runtime/src/iop/cpu.cpp
    runtime/src/iop/kernel.cpp
    runtime/src/iop/libs.cpp
    runtime/src/iop/hw.cpp
    runtime/src/iop/spu2.cpp
    runtime/src/iop/sio2.cpp
    runtime/src/iop/io.cpp
    runtime/src/iop/cdvd.cpp
    runtime/src/iop/sif.cpp
    runtime/src/iop/cdvdfsv.cpp)
target_include_directories(ps2runtime PUBLIC runtime/include)
target_compile_options(ps2runtime PRIVATE -Wall -Wextra)

set(PS2_WGPU_DIR "${CMAKE_SOURCE_DIR}/third_party/wgpu-native" CACHE PATH "wgpu-native release folder")
if(EMSCRIPTEN OR EXISTS "${PS2_WGPU_DIR}/lib/libwgpu_native.a")
    set(PS2_GS_WGSL "${CMAKE_BINARY_DIR}/gs_wgsl.cpp")
    add_custom_command(OUTPUT "${PS2_GS_WGSL}"
        COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR}/runtime/src/gs/gpu/gs.wgsl -DOUT=${PS2_GS_WGSL} -DNAME=kGsShader -P ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
        DEPENDS runtime/src/gs/gpu/gs.wgsl cmake/embed.cmake)
    set(PS2_BLIT_WGSL "${CMAKE_BINARY_DIR}/blit_wgsl.cpp")
    add_custom_command(OUTPUT "${PS2_BLIT_WGSL}"
        COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR}/runtime/src/gs/gpu/blit.wgsl -DOUT=${PS2_BLIT_WGSL} -DNAME=kBlitShader -P ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
        DEPENDS runtime/src/gs/gpu/blit.wgsl cmake/embed.cmake)
    target_sources(ps2runtime PRIVATE runtime/src/gs/gpu/gpu.cpp "${PS2_GS_WGSL}" "${PS2_BLIT_WGSL}")
    target_compile_definitions(ps2runtime PUBLIC PS2_GPU)
    if(EMSCRIPTEN)
        target_compile_options(ps2runtime PUBLIC --use-port=emdawnwebgpu)
        target_link_options(ps2runtime PUBLIC --use-port=emdawnwebgpu)
    else()
        target_include_directories(ps2runtime PRIVATE "${PS2_WGPU_DIR}/include")
        target_link_libraries(ps2runtime PUBLIC "${PS2_WGPU_DIR}/lib/libwgpu_native.a" dl pthread m)
    endif()
endif()

if(PS2_GENERATED)
    file(GLOB PS2_UNITS CONFIGURE_DEPENDS "${PS2_GENERATED}/*.cpp")
    add_library(ps2game STATIC ${PS2_UNITS})
    target_include_directories(ps2game PRIVATE "${PS2_GENERATED}")
    target_link_libraries(ps2game PUBLIC ps2runtime)
    target_compile_options(ps2game PRIVATE -Wno-unused-label -Wno-unused-variable)

    set(PS2_HOOKS "" CACHE PATH "Folder with decompiled functions that replace recompiled ones (hooks_table.cpp defines register_hooks())")
    if(PS2_HOOKS)
        file(GLOB_RECURSE PS2_HOOK_SOURCES CONFIGURE_DEPENDS "${PS2_HOOKS}/*.cpp")
        add_library(ps2hooks STATIC ${PS2_HOOK_SOURCES})
        # The parent too, so sources can include "hooks/<subsystem>/<name>.hpp".
        cmake_path(GET PS2_HOOKS PARENT_PATH PS2_HOOKS_PARENT)
        target_include_directories(ps2hooks PRIVATE "${PS2_HOOKS}" "${PS2_HOOKS_PARENT}")
        target_link_libraries(ps2hooks PUBLIC ps2runtime)
        target_compile_definitions(ps2runtime PUBLIC PS2_HOOKS)
    endif()

    if(EMSCRIPTEN)
        set(PS2_WEB_ELF "" CACHE FILEPATH "Game executable preloaded as /game/core.elf")
        set(PS2_WEB_BOOT_FILE "" CACHE FILEPATH "Text file holding the argv[0] the game sees, preloaded as /game/boot.txt")
        set(PS2_WEB_SHELL "${CMAKE_SOURCE_DIR}/web/shell.html" CACHE FILEPATH "Emscripten shell HTML for the page")
        # ON: the executable is left out of the page's data. The page reads it from the user's disc (the path in
        # /game/boot.txt) and boots only if it has the size and SHA-256 of PS2_WEB_ELF. For a game whose
        # executable is a file on its disc.
        option(PS2_WEB_ELF_FROM_DISC "Read the executable from the user's disc instead of preloading PS2_WEB_ELF" OFF)
        add_executable(ps2web host/web.cpp host/disc_cache.cpp host/pace.cpp host/sha256.cpp)
        if(PS2_WEB_BOOT_FILE)
            target_link_options(ps2web PRIVATE "SHELL:--preload-file ${PS2_WEB_BOOT_FILE}@/game/boot.txt")
        endif()
        target_link_libraries(ps2web PRIVATE $<$<BOOL:${PS2_HOOKS}>:ps2hooks> ps2game ps2runtime)
        # Memory: a Burnout 3 race ended with the heap top at 248.8 MB with 16 MB stacks and a 64 MB disc cache;
        # without the 78 MB those cuts give back it is near 171 MB, so 192 MB holds it with room. Growth stays
        # on in case another route needs more.
        # STACK_SIZE is the main thread's stack (2 KB used); the other threads' are set in host/web.cpp.
        target_link_options(ps2web PRIVATE --profiling-funcs -sJSPI -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=201326592 -sPTHREAD_POOL_SIZE=2 -sALLOW_TABLE_GROWTH=1 -lidbfs.js -sSTACK_SIZE=1048576 -sENVIRONMENT=web
            -sEXPORTED_FUNCTIONS=_main,_ps2web_pad,_ps2web_resize,_ps2web_gpu_ms,_ps2web_frames,_ps2web_primitives,_ps2web_audio_ring,_ps2web_audio_queued_ms,_ps2web_idle_ms,_ps2web_vu_interpreted,_ps2web_stack_used,_ps2web_heap_top
            -sEXPORTED_RUNTIME_METHODS=callMain,addRunDependency,removeRunDependency,HEAPU8,ENV
            --shell-file ${PS2_WEB_SHELL})
        if(PS2_WEB_ELF_FROM_DISC)
            if(NOT PS2_WEB_ELF)
                message(FATAL_ERROR "PS2_WEB_ELF_FROM_DISC needs PS2_WEB_ELF, the executable the disc's copy is checked against")
            endif()
            file(SHA256 "${PS2_WEB_ELF}" PS2_WEB_ELF_SHA256)
            file(SIZE "${PS2_WEB_ELF}" PS2_WEB_ELF_SIZE)
            target_compile_definitions(ps2web PRIVATE "PS2_WEB_ELF_SHA256=\"${PS2_WEB_ELF_SHA256}\"" PS2_WEB_ELF_SIZE=${PS2_WEB_ELF_SIZE})
        elseif(PS2_WEB_ELF)
            target_link_options(ps2web PRIVATE --preload-file ${PS2_WEB_ELF}@/game/core.elf)
        endif()
        set_target_properties(ps2web PROPERTIES SUFFIX ".html" LINK_DEPENDS ${PS2_WEB_SHELL})
        return()
    endif()

    add_executable(ps2run host/main.cpp)
    target_link_libraries(ps2run PRIVATE $<$<BOOL:${PS2_HOOKS}>:ps2hooks> ps2game ps2runtime)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
        target_sources(ps2run PRIVATE host/profile.cpp)
        target_compile_definitions(ps2run PRIVATE PS2_PROFILE)
    endif()
    find_package(SDL2 QUIET)
    if(SDL2_FOUND AND NOT EMSCRIPTEN)
        target_sources(ps2run PRIVATE host/window.cpp)
        target_compile_definitions(ps2run PRIVATE PS2_WINDOW)
        target_link_libraries(ps2run PRIVATE SDL2::SDL2)
    endif()
endif()
