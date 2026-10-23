cmake_minimum_required(VERSION 3.22)
project(NFSU2AndroidBootstrap C CXX)
if(NOT ANDROID OR NOT (ANDROID_ABI STREQUAL "arm64-v8a" OR ANDROID_ABI STREQUAL "armeabi-v7a"))
    message(FATAL_ERROR "This target requires Android arm64-v8a or armeabi-v7a")
endif()

# libadrenotools (custom Vulkan drivers) exists for arm64 only.
if(ANDROID_ABI STREQUAL "arm64-v8a")
    set(NFS_CUSTOM_DRIVERS ON)
else()
    set(NFS_CUSTOM_DRIVERS OFF)
endif()

add_library(native_app_glue STATIC "${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c")
target_include_directories(native_app_glue PUBLIC "${ANDROID_NDK}/sources/android/native_app_glue")
get_filename_component(NFS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
set(PCRECOMP "${NFS_ROOT}/../pcrecomp")
set(DXVK_SOURCE "${NFS_ROOT}/build/dxvk-source" CACHE PATH "Prepared DXVK native source")

set(DXVK_ANDROID_STATIC ON CACHE BOOL "Link into application" FORCE)
add_subdirectory("${NFS_ROOT}/ports/dxvk_android" "${CMAKE_CURRENT_BINARY_DIR}/dxvk")

set(GEN_DIR "${NFS_ROOT}/src/recomp/gen")
file(GLOB GEN_SOURCES CONFIGURE_DEPENDS "${GEN_DIR}/recomp_*.c")
if(NOT GEN_SOURCES)
    message(FATAL_ERROR "Generate the local game C first; see the project README")
endif()

# Very large translation units leave one compiler working long after all other
# jobs finish. Split only those units, preserving complete functions and headers.
set(ANDROID_GEN_SOURCES)
foreach(source IN LISTS GEN_SOURCES)
    file(SIZE "${source}" source_bytes)
    if(source_bytes GREATER 67108864)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${source}" "${NFS_ROOT}/scripts/split_android_sources.py")
        execute_process(COMMAND "${Python3_EXECUTABLE}" "${NFS_ROOT}/scripts/split_android_sources.py"
            "${source}" "${CMAKE_CURRENT_BINARY_DIR}/split_guest"
            RESULT_VARIABLE split_status OUTPUT_VARIABLE split_sources OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(NOT split_status EQUAL 0)
            message(FATAL_ERROR "Failed to split lifted Android source: ${source}")
        endif()
        string(REPLACE "\n" ";" split_sources "${split_sources}")
        list(APPEND ANDROID_GEN_SOURCES ${split_sources})
    else()
        list(APPEND ANDROID_GEN_SOURCES "${source}")
    endif()
endforeach()

# server.dll (EA's LAN game server) lifted from a copy relocated to its guest
# base: scripts/prepare_module.py + run_lift.py --prefix server_. Optional: a
# build without it reports LAN hosting as unavailable instead of failing.
# Native replacements of hot lifted functions (the R36S's src/runtime/native_math.c, bit-identical, and the
# D3DX preshader cache of fx_cache.c): their lifted definitions become <name>_orig in a copy of whichever
# source (or split part) defines them, so every caller reaches the native version. scripts/wrap_lifted.py
# fails the configure step if a name is not defined exactly once. NFS_ANDROID_NATIVE=OFF builds without them.
option(NFS_ANDROID_NATIVE "Native math helpers and the preshader cache (as on the R36S)" ON)
option(NFS_ANDROID_CODEGEN "R36S code generation for the lifted code (CPU state struct, local x87 window)" ON)
if(NFS_ANDROID_NATIVE AND NOT NFS_ANDROID_CODEGEN)
    # native_math.c works on the CPU state struct (g_cpu) that NFS_ANDROID_CODEGEN introduces
    message(WARNING "NFS_ANDROID_NATIVE needs NFS_ANDROID_CODEGEN: building without the native helpers")
    set(NFS_ANDROID_NATIVE OFF)
endif()
if(NFS_ANDROID_NATIVE)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(NFS_WRAPPED_FUNCTIONS
        # native_math.c: _ftol2, point x matrix, cross product, matrix copy/multiply/transpose, sine polynomials,
        # interpolated table lookup, quad edge test, AABB transform and frustum test, the game's QPC timer
        sub_0075BC00 sub_00440FF0 sub_0043CA20 sub_0043D250 sub_0043CA30 sub_00401310 sub_00401430 sub_00689A17
        sub_0043C310 sub_0043C3B0 sub_0043BE50 sub_00576170 sub_005D3080 sub_005BBF20 sub_005BC010
        # fx_cache.c: D3DX preshader evaluation (skipped when its inputs did not change)
        sub_00664743)
    list(JOIN NFS_WRAPPED_FUNCTIONS "," wrapped_list)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${ANDROID_GEN_SOURCES} "${NFS_ROOT}/scripts/wrap_lifted.py")
    execute_process(COMMAND "${Python3_EXECUTABLE}" "${NFS_ROOT}/scripts/wrap_lifted.py"
        "${CMAKE_CURRENT_BINARY_DIR}/wrapped_guest" "${wrapped_list}" ${ANDROID_GEN_SOURCES}
        RESULT_VARIABLE wrap_status OUTPUT_VARIABLE wrap_map ERROR_VARIABLE wrap_error OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT wrap_status EQUAL 0)
        message(FATAL_ERROR "wrap_lifted.py failed: ${wrap_error}")
    endif()
    # One "<source>;<copy>" line per wrapped source: a flat source/copy list once the newlines are ";".
    string(REPLACE "\n" ";" wrap_pairs "${wrap_map}")
    list(LENGTH wrap_pairs wrap_len)
    if(wrap_len GREATER 0)
        math(EXPR wrap_last "${wrap_len} - 1")
        foreach(i RANGE 0 ${wrap_last} 2)
            math(EXPR j "${i} + 1")
            list(GET wrap_pairs ${i} wrap_src)
            list(GET wrap_pairs ${j} wrap_dst)
            list(FIND ANDROID_GEN_SOURCES "${wrap_src}" wrap_index)
            if(wrap_index GREATER -1)
                list(REMOVE_AT ANDROID_GEN_SOURCES ${wrap_index})
                list(INSERT ANDROID_GEN_SOURCES ${wrap_index} "${wrap_dst}")
            endif()
        endforeach()
        message(STATUS "Native replacements: ${wrap_len} entries (source/copy) for ${wrapped_list}")
    endif()
endif()

# The game's own lifted sources (and their split parts / wrapped copies), without server.dll's below.
set(NFS_MAIN_GEN_SOURCES ${ANDROID_GEN_SOURCES})

set(SERVER_GEN_DIR "${NFS_ROOT}/src/recomp/gen_server")
file(GLOB SERVER_GEN_SOURCES CONFIGURE_DEPENDS "${SERVER_GEN_DIR}/server_recomp_*.c")
list(APPEND ANDROID_GEN_SOURCES ${SERVER_GEN_SOURCES})

set_property(GLOBAL PROPERTY JOB_POOLS guest_compile=4)
add_library(nfs_guest OBJECT ${ANDROID_GEN_SOURCES})
set_property(TARGET nfs_guest PROPERTY JOB_POOL_COMPILE guest_compile)
target_include_directories(nfs_guest PRIVATE "${PCRECOMP}/runtime/recomp32" "${GEN_DIR}" "${SERVER_GEN_DIR}")
# RECOMP_LOCAL_REGS: guest registers live in locals of each lifted function
# (written back at every call and return) so they stay in ARM64 registers.
# RECOMP_MEM_QUAL=: guest memory accesses are not volatile; -fno-strict-aliasing
# keeps the untyped guest memory correct, and lifted loops still make an opaque
# call per back-edge. -fwrapv: x86 integer arithmetic wraps. Both validated by
# scripts/difftest_real.py --cc clang-o2 --local-regs (same result as MSVC).
# RECOMP_X87_WINDOW: the x87 stack is a sliding window (push/pop move one
# index instead of shifting 16 values). Every unit of the library must agree,
# so nfsu2_android (runtime, overrides, widescreen) defines it too.
target_compile_definitions(nfs_guest PRIVATE RECOMP_STRICT_ICALL RECOMP_LOCAL_REGS RECOMP_X87_WINDOW "RECOMP_MEM_QUAL=")
# Code generation from the R36S port (arm64_guest.h), same lifted sources:
# RECOMP_TLS_STRUCT: the guest CPU state is one struct (g_cpu) addressed from one base, instead of ~60
# separate globals (on the R36S: lifted code 201 -> 166 MB). It stays a plain global here (RECOMP_TLS is
# empty: guest threads take turns on the machine and switch through State save/load, as before).
# NFS_X87_LOCAL: the x87 window position, TOP and control word are locals of each function.
# NFS_X87_LW: per-function local x87 window: functions whose stack depth is statically known
# (src/recomp/gen/nfs_lw.h, ports/r36s/lw/lw.sh) keep their x87 values in locals = registers
# (R36S: game thread -12% and -22% in two steps, code 157 -> 92 MB). The runtime units must agree on the
# layout, so nfsu2_android defines the same three.
set(NFS_X87_LW_DEFS)
if(NFS_ANDROID_CODEGEN)
    # The local x87 window is only correct for the sources nfs_lw.h was computed from: after a new lift,
    # run ports/r36s/lw/lw.sh (it annotates the sources and writes nfs_lw.h last). Without
    # an up-to-date header the build keeps the struct and x87 locals and leaves the window off.
    set(NFS_LW_HEADER "${GEN_DIR}/nfs_lw.h")
    set(NFS_LW_OK TRUE)
    if(NOT EXISTS "${NFS_LW_HEADER}")
        set(NFS_LW_OK FALSE)
    else()
        foreach(source IN LISTS GEN_SOURCES)
            if("${source}" IS_NEWER_THAN "${NFS_LW_HEADER}" AND NOT "${source}" STREQUAL "${NFS_LW_HEADER}")
                file(TIMESTAMP "${source}" t1 "%s")
                file(TIMESTAMP "${NFS_LW_HEADER}" t2 "%s")
                if(t1 GREATER t2)
                    set(NFS_LW_OK FALSE)
                endif()
            endif()
        endforeach()
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${NFS_LW_HEADER}")
    if(NFS_LW_OK)
        set(NFS_X87_LW_DEFS NFS_X87_LW)
    else()
        message(WARNING "src/recomp/gen/nfs_lw.h is missing or older than the lifted sources: building without "
                        "the local x87 window (run ports/r36s/lw/lw.sh to enable it)")
    endif()
    target_compile_definitions(nfs_guest PRIVATE RECOMP_TLS_STRUCT NFS_X87_LOCAL)
    # Only the game's own code: nfs_lw.h describes SPEED2.EXE's functions, not server.dll's (gen_server).
    if(NFS_X87_LW_DEFS)
        set_source_files_properties(${NFS_MAIN_GEN_SOURCES} PROPERTIES COMPILE_DEFINITIONS NFS_X87_LW)
    endif()
endif()
# Optimize the ARM64 game while avoiding aliasing assumptions in x86 accesses;
# retain unwind tables so unsupported HLE paths can exit to the host cleanly.
target_compile_options(nfs_guest PRIVATE -O2 -w -fwrapv -fno-strict-aliasing -fexceptions -ffp-contract=off
    -include "${CMAKE_CURRENT_SOURCE_DIR}/arm64_guest.h")
# The ~27,700 lifted functions are internal to the library: hidden visibility
# turns their cross-file calls from PLT stubs into direct calls, and ThinLTO
# (as in the MW port) can inline small game functions across the generated
# files, which also removes their guest register save/reload. No debug info:
# it only slows the build of 9M generated lines.
option(NFS_GUEST_LTO "ThinLTO over the lifted game code" ON)
target_compile_options(nfs_guest PRIVATE -g0 -fvisibility=hidden)
# ARMv7: guest data is only as aligned as x86 needed, but the lifted code reads
# floats and doubles through plain pointers, and the compiler would use
# LDRD/STRD/VLDR, which fault on such addresses (SIGBUS BUS_ADRALN). ARM64
# loads tolerate it; on 32-bit assume no alignment for unannotated pointers.
if(ANDROID_ABI STREQUAL "armeabi-v7a")
    target_compile_options(nfs_guest PRIVATE -fmax-type-align=1)
endif()
if(NFS_GUEST_LTO)
    target_compile_options(nfs_guest PRIVATE -flto=thin)
endif()

set(MOJOSHADER_DIR "${NFS_ROOT}/third_party/mojoshader")
set(GLES_SOURCES
    "${NFS_ROOT}/ports/r36s/d3d9_gles/d3d9_gles.cpp"
    "${MOJOSHADER_DIR}/mojoshader.c"
    "${MOJOSHADER_DIR}/mojoshader_common.c"
    "${MOJOSHADER_DIR}/profiles/mojoshader_profile_common.c"
    "${MOJOSHADER_DIR}/profiles/mojoshader_profile_glsl.c"
)

if(NFS_CUSTOM_DRIVERS)
    add_subdirectory("${NFS_ROOT}/third_party/libadrenotools" "${CMAKE_CURRENT_BINARY_DIR}/adrenotools")
endif()

add_library(nfsu2_diagnostics SHARED gpu_probe.cpp vulkan_driver.cpp)
target_link_libraries(nfsu2_diagnostics PRIVATE android log dl)
target_compile_options(nfsu2_diagnostics PRIVATE -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers)

add_library(nfsu2_android SHARED
    main.cpp
    guest_runtime.cpp
    d3d9_bridge.cpp
    vulkan_driver.cpp
    ${GLES_SOURCES}
    "${NFS_ROOT}/src/runtime/overrides.c"
    "${NFS_ROOT}/src/runtime/widescreen.c"
    $<TARGET_OBJECTS:nfs_guest>
)
if(NFS_ANDROID_NATIVE)
    target_sources(nfsu2_android PRIVATE "${NFS_ROOT}/src/runtime/native_math.c" "${NFS_ROOT}/src/runtime/fx_cache.c")
    # fx_cache.c without its device-call replay (that needs the R36S render thread's queue).
    target_compile_definitions(nfsu2_android PRIVATE NFS_FX_PRESHADER_ONLY)
endif()
if(NFS_ANDROID_CODEGEN)
    target_compile_definitions(nfsu2_android PRIVATE RECOMP_TLS_STRUCT NFS_X87_LOCAL ${NFS_X87_LW_DEFS})
endif()

target_include_directories(nfsu2_android SYSTEM PRIVATE
    "${DXVK_SOURCE}/include/native"
    "${DXVK_SOURCE}/include/native/windows"
    "${DXVK_SOURCE}/include/native/directx"
)
target_include_directories(nfsu2_android PRIVATE
    "${NFS_ROOT}/ports/r36s/d3d9_gles"
    "${MOJOSHADER_DIR}"
    "${PCRECOMP}/runtime/recomp32"
    "${PCRECOMP}/runtime/win32hle"
    "${NFS_ROOT}/src/runtime"
)

target_compile_definitions(nfsu2_android PRIVATE
    RECOMP_STRICT_ICALL
    RECOMP_X87_WINDOW
    MOJOSHADER_DEPTH_CLIPPING
    MOJOSHADER_FLIP_RENDERTARGET
    SUPPORT_PROFILE_ARB1=0
    SUPPORT_PROFILE_ARB1_NV=0
    SUPPORT_PROFILE_HLSL=0
    SUPPORT_PROFILE_METAL=0
    SUPPORT_PROFILE_D3D=0
    SUPPORT_PROFILE_BYTECODE=0
    SUPPORT_PROFILE_SPIRV=0
    SUPPORT_PROFILE_GLSPIRV=0
    VK_USE_PLATFORM_ANDROID_KHR
)

if(SERVER_GEN_SOURCES)
    target_compile_definitions(nfsu2_android PRIVATE NFS_SERVER_DLL)
endif()

target_compile_features(nfsu2_android PRIVATE cxx_std_17)
target_compile_options(nfsu2_android PRIVATE -O2 -fno-strict-aliasing -ffp-contract=off -Wall -Wextra -Wno-missing-field-initializers)
target_link_options(nfsu2_android PRIVATE "-uANativeActivity_onCreate" "-Wl,-z,max-page-size=16384")
target_link_libraries(nfsu2_android PRIVATE
    native_app_glue
    android
    log
    vulkan
    dxvk_android
    EGL
    GLESv2
    GLESv3
    aaudio
    dl
)

if(NFS_GUEST_LTO)
    target_link_options(nfsu2_android PRIVATE -flto=thin "-Wl,--thinlto-jobs=4"
        "-Wl,--thinlto-cache-dir=${CMAKE_BINARY_DIR}/thinlto-cache")
endif()

# linkernsbypass has data symbols named like libdl's functions; exporting them breaks
# the system loader when it opens the vendor namespace (MW port note).
# Only adrenotools' archives: excluding ALL would also hide native_app_glue's
# ANativeActivity_onCreate, which NativeActivity looks up by name.
foreach(target nfsu2_android nfsu2_diagnostics)
    target_link_options(${target} PRIVATE "-Wl,--exclude-libs,libadrenotools.a:liblinkernsbypass.a" "-Wl,-z,max-page-size=16384")
endforeach()

if(NFS_CUSTOM_DRIVERS)
    foreach(target nfsu2_android nfsu2_diagnostics)
        target_link_libraries(${target} PRIVATE adrenotools)
        target_compile_definitions(${target} PRIVATE NFS_CUSTOM_DRIVERS)
    endforeach()
    target_link_options(hook_impl PRIVATE "-Wl,--exclude-libs,ALL")
    foreach(target adrenotools main_hook file_redirect_hook gsl_alloc_hook hook_impl)
        target_link_options(${target} PRIVATE "-Wl,-z,max-page-size=16384")
    endforeach()
    # adrenotools opens these from nativeLibraryDir at run time.
    add_dependencies(nfsu2_android main_hook file_redirect_hook gsl_alloc_hook hook_impl)
endif()
