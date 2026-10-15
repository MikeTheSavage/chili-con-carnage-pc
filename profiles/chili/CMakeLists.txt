include("${CMAKE_CURRENT_SOURCE_DIR}/media_backend.cmake")

set(CHILI_GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/analysis/generated")
file(GLOB CHILI_GENERATED CONFIGURE_DEPENDS "${CHILI_GENERATED_DIR}/*.cpp")
if(NOT EXISTS "${CHILI_GENERATED_DIR}/generated_registry.cpp")
    # The corpus is C++ generated from the game's executable. It is not in the repository: each builder makes
    # it from their own copy of the game (BUILDING.md, scripts/analyze.ps1 -Iso <your .iso>).
    message(FATAL_ERROR "Generate Chili AOT code before configuring this profile: ${CHILI_GENERATED_DIR} is missing. Run profiles/chili/scripts/analyze.ps1 -Iso <your .iso> (see BUILDING.md).")
endif()
list(SORT CHILI_GENERATED)

# ---------------------------------------------------------------------------
# Generated AOT corpus.
#
# It is its own object library so that nothing done to the host target (new
# sources, include directories, definitions, libraries) changes a generated
# unit's command line. A full corpus build is about half an hour; a host edit
# must never trigger one. Generated units include only framework headers.
# ---------------------------------------------------------------------------
set(CHILI_HOT_UNITS "" CACHE STRING
    "Generated units that also get /d2OptimizeHugeFunctions, as numbers or ranges (0001;0242;0100-0103)")
# The first PT_LOAD of BOOT.BIN is executable and spans .rodata, .data and
# .eh_frame, so the generator emitted units for them. .text and the import
# stubs end inside unit 0357; units 0358-0378 are data decoded as code and are
# never entered. They are small enough that MSVC really optimizes them, which
# took 72-925 s each (a third of the corpus compile time) for nothing.
set(CHILI_COLD_UNITS "0358-0378" CACHE STRING
    "Generated units always compiled /Od /Ob0, as numbers or ranges")
# Unit 0357 holds the tail of .text plus rodata decoded as code and takes about
# ten minutes at /O2. Units listed here (and the hot units) are compiled first
# so they do not become the single-threaded tail of a parallel build.
set(CHILI_SLOW_UNITS "0357" CACHE STRING
    "Generated units compiled first because they take minutes, as numbers or ranges")

# Turns unit numbers and ranges into generated source paths.
function(chili_unit_sources out_var)
    set(sources)
    foreach(entry IN LISTS ARGN)
        if(entry MATCHES "^([0-9]+)-([0-9]+)$")
            set(first "${CMAKE_MATCH_1}")
            set(last "${CMAKE_MATCH_2}")
        elseif(entry MATCHES "([0-9]+)")
            set(first "${CMAKE_MATCH_1}")
            set(last "${CMAKE_MATCH_1}")
        else()
            message(FATAL_ERROR "Chili: '${entry}' is not a generated unit number or range")
        endif()
        # Drop leading zeros. (REGEX REPLACE re-anchors ^ after every match, so
        # it would turn 0100 into 10.)
        foreach(bound first last)
            string(REGEX MATCH "[1-9][0-9]*$" trimmed "${${bound}}")
            if(trimmed STREQUAL "")
                set(trimmed 0)
            endif()
            set(${bound} "${trimmed}")
        endforeach()
        if(last LESS first)
            message(FATAL_ERROR "Chili: empty generated unit range '${entry}'")
        endif()
        foreach(number RANGE ${first} ${last})
            set(id "000${number}")
            string(LENGTH "${id}" length)
            math(EXPR start "${length} - 4")
            string(SUBSTRING "${id}" ${start} 4 id)
            set(source "${CHILI_GENERATED_DIR}/generated_unit_${id}.cpp")
            if(NOT EXISTS "${source}")
                message(FATAL_ERROR "Chili: '${entry}' names a missing generated unit: ${source}")
            endif()
            list(APPEND sources "${source}")
        endforeach()
    endforeach()
    list(REMOVE_DUPLICATES sources)
    set(${out_var} "${sources}" PARENT_SCOPE)
endfunction()

chili_unit_sources(CHILI_HOT_SOURCES ${CHILI_HOT_UNITS})
chili_unit_sources(CHILI_COLD_SOURCES ${CHILI_COLD_UNITS})
chili_unit_sources(CHILI_SLOW_SOURCES ${CHILI_SLOW_UNITS})
if(PSPRECOMP_GENERATED_OPT_LEVEL STREQUAL "0")
    if(CHILI_HOT_SOURCES)
        message(STATUS "Chili: CHILI_HOT_UNITS ignored at generated optimization level 0")
    endif()
    set(CHILI_HOT_SOURCES)
endif()
if(CHILI_HOT_SOURCES)
    list(REMOVE_ITEM CHILI_COLD_SOURCES ${CHILI_HOT_SOURCES})
endif()
set(CHILI_FIRST_SOURCES ${CHILI_HOT_SOURCES} ${CHILI_SLOW_SOURCES})
if(CHILI_FIRST_SOURCES)
    list(REMOVE_DUPLICATES CHILI_FIRST_SOURCES)
    list(REMOVE_ITEM CHILI_GENERATED ${CHILI_FIRST_SOURCES})
    list(PREPEND CHILI_GENERATED ${CHILI_FIRST_SOURCES})
endif()

add_library(chili_generated OBJECT ${CHILI_GENERATED})
target_link_libraries(chili_generated PRIVATE psprecomp_core)
target_include_directories(chili_generated PRIVATE "${CHILI_GENERATED_DIR}")

if(MSVC)
    if(PSPRECOMP_GENERATED_OPT_LEVEL STREQUAL "0")
        set(CHILI_GENERATED_OPT /Od /Ob0)
    elseif(PSPRECOMP_GENERATED_OPT_LEVEL STREQUAL "1")
        set(CHILI_GENERATED_OPT /O1 /Ob2)
    else()
        set(CHILI_GENERATED_OPT /O2 /Ob2)
    endif()
    # /Ob2 is required for speed: at /Ob0 the __forceinline register and memory
    # accessors stay calls. /w14883 reports every function MSVC declines to
    # optimize because of its size, which is every unit of real game code that
    # is not in CHILI_HOT_UNITS. /GL- keeps the corpus out of LTCG when
    # PSPRECOMP_LTO is on.
    set(CHILI_GENERATED_COMMON /bigobj /w14883 /GL- ${PSPRECOMP_MSVC_MP_FLAG})
    set_source_files_properties(${CHILI_GENERATED} PROPERTIES
        COMPILE_OPTIONS "${CHILI_GENERATED_OPT};${CHILI_GENERATED_COMMON}")
    if(CHILI_COLD_SOURCES)
        set_source_files_properties(${CHILI_COLD_SOURCES} PROPERTIES
            COMPILE_OPTIONS "/Od;/Ob0;${CHILI_GENERATED_COMMON}")
    endif()
    # Real optimization of a 16 KiB unit costs minutes and about 1.8 GB per
    # compiler process, so it is opt-in per profiled unit.
    if(CHILI_HOT_SOURCES)
        set_property(SOURCE ${CHILI_HOT_SOURCES} APPEND PROPERTY COMPILE_OPTIONS /d2OptimizeHugeFunctions)
    endif()
    list(LENGTH CHILI_GENERATED CHILI_GENERATED_COUNT)
    list(LENGTH CHILI_HOT_SOURCES CHILI_HOT_COUNT)
    list(LENGTH CHILI_COLD_SOURCES CHILI_COLD_COUNT)
    list(JOIN CHILI_GENERATED_OPT " " CHILI_GENERATED_OPT_TEXT)
    message(STATUS "Chili: ${CHILI_GENERATED_COUNT} generated sources at ${CHILI_GENERATED_OPT_TEXT}, ${CHILI_HOT_COUNT} hot, ${CHILI_COLD_COUNT} cold at /Od /Ob0")
else()
    set_source_files_properties(${CHILI_GENERATED} PROPERTIES COMPILE_OPTIONS
        "-O${PSPRECOMP_GENERATED_OPT_LEVEL};-g0")
endif()

# ---------------------------------------------------------------------------
# Host executable.
#
# Every .cpp under host/ (subdirectories included) is compiled into
# chili_recomp; a new host source needs no edit here. Anything under host/ that
# must stay out (a second main(), a tool) goes in CHILI_HOST_EXCLUDE as a path
# relative to this directory.
# ---------------------------------------------------------------------------
set(CHILI_HOST_EXCLUDE "" CACHE STRING
    "Sources under host/ kept out of chili_recomp (paths relative to profiles/chili)")
file(GLOB_RECURSE CHILI_HOST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/host/*.cpp")
foreach(CHILI_HOST_SKIP IN LISTS CHILI_HOST_EXCLUDE)
    list(REMOVE_ITEM CHILI_HOST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/${CHILI_HOST_SKIP}")
endforeach()

add_executable(chili_recomp ${CHILI_HOST_SOURCES} $<TARGET_OBJECTS:chili_generated>)
# The generator expression stops multi-config generators appending a
# per-configuration directory: the executable is always <build>/bin/chili_recomp.exe.
set_target_properties(chili_recomp PROPERTIES RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/bin>")

# Target-wide: inline renderer/TLS definitions must agree in every relevant TU.
# Default OFF retains the uninstrumented renderer; runtime opt-in is separate.
option(CHILI_PARALLEL_SHADE_OBSERVATION "Compile passive parallel shading participation diagnostics" OFF)
if(CHILI_PARALLEL_SHADE_OBSERVATION)
    target_compile_definitions(chili_recomp PRIVATE CHILI_PARALLEL_SHADE_OBSERVATION)
endif()

target_include_directories(chili_recomp PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/host"
    "${CHILI_GENERATED_DIR}"
    "${CHILI_FFMPEG}/include")
target_link_libraries(chili_recomp PRIVATE psprecomp_core chili_media_backend
    ws2_32 xinput9_1_0 user32 gdi32 winmm bcrypt ole32 avrt
    d3d12 dxgi dxguid d3dcompiler windowscodecs d2d1 dwrite)
if(MSVC)
    target_compile_options(chili_recomp PRIVATE ${PSPRECOMP_MSVC_MP_FLAG})
    # Suppressed-optimization generated units have native frames of 85-180 KB
    # and chain up to 48 deep, so keep the same stack reserve as the VCS host.
    # /MAP writes bin/chili_recomp.map so sampled addresses resolve to functions.
    target_link_options(chili_recomp PRIVATE /STACK:67108864 /MAP)
endif()

# Program icon, version information and application manifest (resources/). The build keeps the
# name chili_recomp.exe, which the run scripts use; scripts/package.ps1 names the packaged copy
# ChiliConCarnage.exe and makes it a program without a console window.
if(WIN32)
    target_sources(chili_recomp PRIVATE resources/ChiliConCarnage.rc resources/ChiliConCarnage.manifest)
    set_source_files_properties(resources/ChiliConCarnage.rc PROPERTIES
        INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/resources"
        OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/resources/app_icon.ico;${CMAKE_CURRENT_SOURCE_DIR}/host/version.hpp")
endif()

chili_copy_media_runtime(chili_recomp)

# Offline replay of GE lists captured with CHILI_GE_CAPTURE_DIR
# (host/ge_frame_capture.hpp). It gets the same compile options as the host
# sources, so a capture replays to the same VRAM bytes the game produced.
file(GLOB CHILI_GPU_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/host/gpu/*.cpp")
add_executable(chili_ge_replay tools/chili_ge_replay.cpp host/thread_runtime.cpp ${CHILI_GPU_SOURCES})
set_target_properties(chili_ge_replay PROPERTIES RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/bin>")
target_include_directories(chili_ge_replay PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/host")
target_link_libraries(chili_ge_replay PRIVATE psprecomp_core d3d12 dxgi dxguid d3dcompiler windowscodecs ole32)
if(MSVC)
    target_compile_options(chili_ge_replay PRIVATE ${PSPRECOMP_MSVC_MP_FLAG})
    target_compile_definitions(chili_ge_replay PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# The mods folder beside the executable, with the README for modders (host/mod_files.hpp).
add_custom_command(TARGET chili_recomp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/bin/mods"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/mods/README.md" "${CMAKE_BINARY_DIR}/bin/mods/README.md"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/mods/TEXTURES.md" "${CMAKE_BINARY_DIR}/bin/mods/TEXTURES.md")
