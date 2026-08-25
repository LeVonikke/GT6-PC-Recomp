cmake_minimum_required(VERSION 3.20)
project(GT6MainRecompHook LANGUAGES C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
string(REPLACE "/EHsc" "/EHa" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
string(REPLACE "/EHsc" "/EHa" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")

get_filename_component(PS3RECOMP_DIR "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
if(MSVC)
    set(PS3RECOMP_RUNTIME_LIB_DEFAULT "${PS3RECOMP_DIR}/build/ps3recomp_runtime.lib")
else()
    set(PS3RECOMP_RUNTIME_LIB_DEFAULT "${PS3RECOMP_DIR}/build/libps3recomp_runtime.a")
endif()
set(PS3RECOMP_RUNTIME_LIB "${PS3RECOMP_RUNTIME_LIB_DEFAULT}" CACHE FILEPATH
    "Pre-built ps3recomp runtime library")
if(NOT EXISTS "${PS3RECOMP_RUNTIME_LIB}")
    message(FATAL_ERROR "Runtime not found: ${PS3RECOMP_RUNTIME_LIB}. Build ${PS3RECOMP_DIR} first.")
endif()

set(GT6_DIR "${PS3RECOMP_DIR}/gt6")
file(GLOB RECOMP_CHUNKS CONFIGURE_DEPENDS
    "${GT6_DIR}/emain_recompiled/ppu_recomp_*.cpp")
if(NOT RECOMP_CHUNKS)
    message(FATAL_ERROR "No EMAIN lifted PPU chunks found. Run ppu_lifter.py first.")
endif()

add_executable(GT6MainRecompHook
    "${GT6_DIR}/main.cpp"
    "${GT6_DIR}/gt6_hle.cpp"
    "${GT6_DIR}/spu_job_executor.cpp"
    "${GT6_DIR}/spu_policy_runtime.c"
    "${GT6_DIR}/emain_project/ppu_mid_entries.cpp"
    # Locally generated from the user's decrypted firmware libsre.prx.  This
    # is the actual SPURS kernel-side selector/yield path for PDI, kept out of
    # source control together with the firmware-derived input.
    "${GT6_DIR}/spu_kernel/spu_recomp.c"
    "${GT6_DIR}/spu_policy/spu_recomp.c"
    # all_jobs: a single lifted SPU module covering the full 256KB LS address space,
    # including shared runtime functions (e.g. 0x11498) that individual job modules miss.
    # Replaces the 16 individual job0-job15 modules that only covered ~57 functions each.
    "${GT6_DIR}/spu_jobs/all_jobs/spu_recomp.c"
    "${GT6_DIR}/emain_gen/ppu_hle_nids.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_loader.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_hle.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_sysprx.cpp"
    ${RECOMP_CHUNKS}
)

# all_jobs/spu_recomp.c is ~8.5 MB of generated C code (143k lines).
# MSVC runs out of internal heap with /O2 on files this large.
# Compile it with /Od (no optimization) + /W0 (suppress label warnings) to avoid crash.
if(MSVC)
    set_source_files_properties(
        "${GT6_DIR}/spu_jobs/all_jobs/spu_recomp.c"
        PROPERTIES COMPILE_FLAGS "/Od /W0"
    )

    # MSVC chokes on the giant recomp C files with optimizations enabled (O2),
    # but /Od creates massive stack frames that blow the 1GB limit. Try /O1.
    set_source_files_properties(
        ../emain_recompiled/ppu_recomp_000.cpp
        ../emain_recompiled/ppu_recomp_001.cpp
        ../emain_recompiled/ppu_recomp_002.cpp
        ../emain_recompiled/ppu_recomp_003.cpp
        ../emain_recompiled/ppu_recomp_004.cpp
        PROPERTIES COMPILE_FLAGS "/Od /W0"
    )
endif()

target_include_directories(GT6MainRecompHook PRIVATE
    "${GT6_DIR}"
    "${GT6_DIR}/emain_recompiled"
    "${PS3RECOMP_DIR}/include"
    "${PS3RECOMP_DIR}/libs/filesystem"
    "${PS3RECOMP_DIR}/libs/system"
    "${PS3RECOMP_DIR}/runtime/ppu"
    "${PS3RECOMP_DIR}/runtime/spu"
)

target_compile_definitions(GT6MainRecompHook PRIVATE _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    target_compile_options(GT6MainRecompHook PRIVATE
        /bigobj /EHa /W3 /Zc:__cplusplus
        "/FI${GT6_DIR}/msvc_compat.h"
    )
    # Lifted guest calls are ordinary nested host calls.  Loading the large
    # ADCH main script exceeds the PE default 1 MiB main-thread stack long
    # before ppu_loader's recursion guard can observe an indirect-call loop.
    # Match the 256 MiB reserve already used for spawned PPU host threads;
    # Windows commits it on demand, so the physical cost remains small.
    target_link_options(GT6MainRecompHook PRIVATE
        /FORCE:MULTIPLE /MAP /STACK:1073741824,1048576)
endif()

target_link_libraries(GT6MainRecompHook PRIVATE "${PS3RECOMP_RUNTIME_LIB}")
if(WIN32)
    target_link_libraries(GT6MainRecompHook PRIVATE
        d3d12 dxgi d3dcompiler uuid dxguid
        ws2_32 xinput ole32 bcrypt
    )
else()
    find_package(Threads REQUIRED)
    target_link_libraries(GT6MainRecompHook PRIVATE Threads::Threads)
endif()
