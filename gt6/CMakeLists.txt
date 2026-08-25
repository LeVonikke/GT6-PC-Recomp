cmake_minimum_required(VERSION 3.20)
project(GT6Recomp LANGUAGES C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(PS3RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." CACHE PATH "ps3recomp root")
set(PS3RECOMP_RUNTIME_LIB "${PS3RECOMP_DIR}/build/ps3recomp_runtime.lib" CACHE FILEPATH
    "Pre-built ps3recomp runtime library")
if(NOT EXISTS "${PS3RECOMP_RUNTIME_LIB}")
    message(FATAL_ERROR "Runtime not found: ${PS3RECOMP_RUNTIME_LIB}. Build ${PS3RECOMP_DIR} first.")
endif()

file(GLOB RECOMP_CHUNKS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/recompiled/ppu_recomp_*.cpp")
if(NOT RECOMP_CHUNKS)
    message(FATAL_ERROR "No lifted PPU chunks found. Run tools/ppu_lifter.py first.")
endif()

add_executable(GT6Recomp
    main.cpp
    gen/ppu_hle_nids.cpp
    ${PS3RECOMP_DIR}/runtime/ppu/ppu_loader.cpp
    ${PS3RECOMP_DIR}/runtime/ppu/ppu_hle.cpp
    ${PS3RECOMP_DIR}/runtime/ppu/ppu_sysprx.cpp
    ${RECOMP_CHUNKS}
)

target_include_directories(GT6Recomp PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/recompiled"
    "${PS3RECOMP_DIR}/include"
    "${PS3RECOMP_DIR}/runtime/ppu"
)

target_compile_definitions(GT6Recomp PRIVATE _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    target_compile_options(GT6Recomp PRIVATE
        /bigobj /EHsc /W3 /Zc:__cplusplus
        "/FI${CMAKE_CURRENT_SOURCE_DIR}/msvc_compat.h"
    )
endif()

target_link_libraries(GT6Recomp PRIVATE
    "${PS3RECOMP_RUNTIME_LIB}"
    d3d12 dxgi d3dcompiler uuid dxguid
    ws2_32 xinput ole32 bcrypt
)

# The current ps3recomp runtime archives two compatibility implementations of
# several sceNpCommerce symbols. They are equivalent; accept the archive's
# first definition until the upstream library splits those modules.
if(MSVC)
    target_link_options(GT6Recomp PRIVATE /FORCE:MULTIPLE)
endif()
