cmake_minimum_required(VERSION 3.20)
project(ChiliAnalysis LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
get_filename_component(REPO "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
include("${CMAKE_CURRENT_SOURCE_DIR}/../media_backend.cmake")
# The framework library the tests link: build it first from the repository root (BUILDING.md, "Tests"):
#   cmake -S . -B out/framework -G "Visual Studio 18 2026" -A x64 && cmake --build out/framework --config Release --target psprecomp_core
set(CHILI_FRAMEWORK_LIB "${REPO}/out/framework/Release/psprecomp_core.lib" CACHE FILEPATH "psprecomp_core.lib built from the repository root")
if(NOT EXISTS "${CHILI_FRAMEWORK_LIB}" AND EXISTS "${REPO}/out/framework/psprecomp_core.lib")
    set(CHILI_FRAMEWORK_LIB "${REPO}/out/framework/psprecomp_core.lib" CACHE FILEPATH "psprecomp_core.lib built from the repository root" FORCE)
endif()
if(NOT EXISTS "${CHILI_FRAMEWORK_LIB}")
    message(FATAL_ERROR "ChiliAnalysis: ${CHILI_FRAMEWORK_LIB} is missing. Build psprecomp_core first (BUILDING.md, Tests), or pass -DCHILI_FRAMEWORK_LIB=<file>.")
endif()
# Some tests read captures or files of the developer's own copy of the game (profiles/chili/analysis, game/,
# kept out of the repository). They are defined but disabled unless CHILI_CAPTURE_TESTS is ON.
option(CHILI_CAPTURE_TESTS "Also run the tests that read captures under profiles/chili/analysis and files of game/disc0" OFF)
function(chili_capture_test name)
    if(NOT CHILI_CAPTURE_TESTS)
        set_tests_properties(${name} PROPERTIES DISABLED TRUE)
    endif()
endfunction()
set_source_files_properties(../host/av_runtime.cpp PROPERTIES INCLUDE_DIRECTORIES "${CHILI_FFMPEG}/include")
add_executable(chili_av_test av_test.cpp ../host/av_runtime.cpp ../host/sas_runtime.cpp ../host/atrac_runtime.cpp ../host/atrac_stream.cpp ../host/thread_runtime.cpp)
target_include_directories(chili_av_test PRIVATE "${REPO}/include" "${CHILI_MEDIA_HOST}")
target_link_libraries(chili_av_test PRIVATE "${CHILI_FRAMEWORK_LIB}" chili_media_backend)
chili_copy_media_runtime(chili_av_test)
add_executable(chili_controller_test controller_test.cpp ../host/controller_runtime.cpp ../host/thread_runtime.cpp)
target_include_directories(chili_controller_test PRIVATE "${REPO}/include")
target_link_libraries(chili_controller_test PRIVATE "${CHILI_FRAMEWORK_LIB}" xinput9_1_0 user32)
add_executable(chili_settings_test system_settings_test.cpp)
target_include_directories(chili_settings_test PRIVATE "${REPO}/include")
target_link_libraries(chili_settings_test PRIVATE "${CHILI_FRAMEWORK_LIB}")
add_executable(chili_network_test network_test.cpp ../host/network_runtime.cpp ../host/partition_memory.cpp)
target_include_directories(chili_network_test PRIVATE "${REPO}/include")
target_link_libraries(chili_network_test PRIVATE "${CHILI_FRAMEWORK_LIB}" ws2_32)
add_executable(chili_semaphore_test semaphore_test.cpp ../host/semaphores.cpp ../host/thread_runtime.cpp)
target_include_directories(chili_semaphore_test PRIVATE "${REPO}/include")
target_link_libraries(chili_semaphore_test PRIVATE "${CHILI_FRAMEWORK_LIB}")
add_executable(chili_umd_test umd_test.cpp ../host/file_runtime.cpp ../host/thread_runtime.cpp)
target_include_directories(chili_umd_test PRIVATE "${REPO}/include")
target_link_libraries(chili_umd_test PRIVATE "${CHILI_FRAMEWORK_LIB}")
add_executable(chili_callback_test callback_test.cpp ../host/thread_runtime.cpp)
target_include_directories(chili_callback_test PRIVATE "${REPO}/include")
target_link_libraries(chili_callback_test PRIVATE "${CHILI_FRAMEWORK_LIB}")
add_executable(chili_event_test event_flags_test.cpp ../host/event_flags.cpp ../host/thread_runtime.cpp)
target_include_directories(chili_event_test PRIVATE "${REPO}/include")
target_link_libraries(chili_event_test PRIVATE "${CHILI_FRAMEWORK_LIB}")
add_executable(chili_file_test file_runtime_test.cpp ../host/file_runtime.cpp)
target_include_directories(chili_file_test PRIVATE "${REPO}/include")
target_link_libraries(chili_file_test PRIVATE "${CHILI_FRAMEWORK_LIB}")
add_executable(chili_fpl_test fixed_pool_test.cpp ../host/fixed_pool.cpp ../host/partition_memory.cpp ../host/thread_runtime.cpp)
target_include_directories(chili_fpl_test PRIVATE "${REPO}/include")
target_link_libraries(chili_fpl_test PRIVATE "${CHILI_FRAMEWORK_LIB}")
add_executable(chili_rtc_test rtc_runtime_test.cpp ../host/rtc_runtime.cpp)
target_include_directories(chili_rtc_test PRIVATE "${REPO}/include")
target_link_libraries(chili_rtc_test PRIVATE "${CHILI_FRAMEWORK_LIB}")
add_executable(chili_partition_test partition_memory_test.cpp ../host/partition_memory.cpp)
target_include_directories(chili_partition_test PRIVATE "${REPO}/include")
target_link_libraries(chili_partition_test PRIVATE "${CHILI_FRAMEWORK_LIB}")
add_executable(chili_opcode_audit opcode_audit.cpp)
target_include_directories(chili_opcode_audit PRIVATE "${REPO}/include")
target_link_libraries(chili_opcode_audit PRIVATE "${CHILI_FRAMEWORK_LIB}")
add_executable(chili_thread_test thread_runtime_test.cpp ../host/thread_runtime.cpp)
target_include_directories(chili_thread_test PRIVATE "${REPO}/include")
target_link_libraries(chili_thread_test PRIVATE "${CHILI_FRAMEWORK_LIB}")
if(EXISTS "${CMAKE_BINARY_DIR}/sort-fixture/generated.cpp")
    add_executable(chili_sort_test sort_fixture_test.cpp "${CMAKE_BINARY_DIR}/sort-fixture/generated.cpp")
    target_include_directories(chili_sort_test PRIVATE "${REPO}/include")
    target_link_libraries(chili_sort_test PRIVATE "${CHILI_FRAMEWORK_LIB}")
endif()
if(EXISTS "${CMAKE_BINARY_DIR}/startup-probe/generated.cpp")
    add_executable(chili_startup_probe "../host/main.cpp" "../host/thread_runtime.cpp" "../host/thread_lifecycle.cpp" "../host/partition_memory.cpp" "../host/rtc_runtime.cpp" "../host/libc_time_runtime.cpp" "../host/savedata_runtime.cpp" "../host/savedata_size_query.cpp" "../host/savedata_request.cpp" "../host/save_slot_win32.cpp" "../host/param_sfo.cpp" "../host/savedata_storage.cpp" "../host/native_save_codec.cpp" "../host/native_savedata_backend.cpp" "../host/message_dialog_runtime.cpp" "../host/message_dialog_win32.cpp" "../host/fixed_pool.cpp" "../host/event_flags.cpp" "../host/semaphores.cpp" "../host/network_runtime.cpp" "../host/controller_runtime.cpp" "../host/av_runtime.cpp" "../host/sas_runtime.cpp" "../host/atrac_runtime.cpp" "../host/atrac_stream.cpp" "../host/mpeg_runtime.cpp" "../host/audio_runtime.cpp" "../host/file_runtime.cpp" "${CMAKE_BINARY_DIR}/startup-probe/generated.cpp")
    target_sources(chili_startup_probe PRIVATE "../host/volatile_memory_runtime.cpp")
    target_include_directories(chili_startup_probe PRIVATE "${REPO}/include")
    target_link_libraries(chili_startup_probe PRIVATE "${CHILI_FRAMEWORK_LIB}" ws2_32 xinput9_1_0 user32 gdi32 winmm bcrypt chili_media_backend)
    chili_copy_media_runtime(chili_startup_probe)
endif()
if(EXISTS "${CMAKE_BINARY_DIR}/integer-fixture/generated.cpp")
    add_executable(chili_integer_test integer_fixture_test.cpp "${CMAKE_BINARY_DIR}/integer-fixture/generated.cpp")
    target_include_directories(chili_integer_test PRIVATE "${REPO}/include")
    target_link_libraries(chili_integer_test PRIVATE "${CHILI_FRAMEWORK_LIB}")
endif()






add_executable(chili_sas_test sas_runtime_test.cpp ../host/sas_runtime.cpp)
target_include_directories(chili_sas_test PRIVATE "${REPO}/include")
target_link_libraries(chili_sas_test PRIVATE "${CHILI_FRAMEWORK_LIB}")

set_source_files_properties(../host/atrac_runtime.cpp PROPERTIES INCLUDE_DIRECTORIES "${CHILI_FFMPEG}/include")

add_executable(chili_audio_test audio_test.cpp ../host/audio_runtime.cpp ../host/audio_feed.cpp ../host/thread_runtime.cpp)
target_include_directories(chili_audio_test PRIVATE "${REPO}/include")
target_link_libraries(chili_audio_test PRIVATE "${CHILI_FRAMEWORK_LIB}" ole32 avrt)
# Reserve and release, the host ring with its feeder thread (resume level, restart, playback), the game's queue over it.
add_test(NAME chili_audio COMMAND chili_audio_test)

add_executable(chili_psmf_test psmf_test.cpp)

add_executable(chili_ge_test ge_test.cpp ../host/thread_runtime.cpp)
target_include_directories(chili_ge_test PRIVATE "${REPO}/include")
target_link_libraries(chili_ge_test PRIVATE "${CHILI_FRAMEWORK_LIB}")

add_executable(chili_display_test display_test.cpp ../host/thread_runtime.cpp)
target_include_directories(chili_display_test PRIVATE "${REPO}/include")
target_link_libraries(chili_display_test PRIVATE "${CHILI_FRAMEWORK_LIB}")

add_executable(chili_power_test power_test.cpp)
target_include_directories(chili_power_test PRIVATE "${REPO}/include")
target_link_libraries(chili_power_test PRIVATE "${CHILI_FRAMEWORK_LIB}")

file(GLOB VFPU_STORE_GENERATED "${REPO}/profiles/chili/analysis/vfpu_store_fixture/generated/*.cpp")
if(VFPU_STORE_GENERATED)
add_executable(chili_vfpu_store_test vfpu_store_test.cpp ${VFPU_STORE_GENERATED})
target_include_directories(chili_vfpu_store_test PRIVATE "${REPO}/include" "${REPO}/profiles/chili/analysis/vfpu_store_fixture/generated")
target_link_libraries(chili_vfpu_store_test PRIVATE "${CHILI_FRAMEWORK_LIB}")
endif()

add_executable(chili_window_test window_test.cpp ../host/thread_runtime.cpp)
target_include_directories(chili_window_test PRIVATE "${REPO}/include")
target_link_libraries(chili_window_test PRIVATE "${CHILI_FRAMEWORK_LIB}" user32 gdi32)

# Compile the complete host independently while generated corpus rebuilds.
add_library(chili_host_compile OBJECT ../host/main.cpp)
target_include_directories(chili_host_compile PRIVATE "${REPO}/include" "${REPO}/profiles/chili/analysis/generated")

add_executable(chili_indexed_texture_test ge_indexed_texture_test.cpp)
target_include_directories(chili_indexed_texture_test PRIVATE "${REPO}/include")
target_link_libraries(chili_indexed_texture_test PRIVATE "${CHILI_FRAMEWORK_LIB}")

add_executable(chili_triangle_test ge_triangle_test.cpp)
target_include_directories(chili_triangle_test PRIVATE "${REPO}/include")
target_link_libraries(chili_triangle_test PRIVATE "${CHILI_FRAMEWORK_LIB}")

set_source_files_properties(../host/mpeg_runtime.cpp PROPERTIES INCLUDE_DIRECTORIES "${CHILI_FFMPEG}/include")
add_executable(chili_mpeg_test mpeg_runtime_test.cpp ../host/mpeg_runtime.cpp ../host/av_runtime.cpp ../host/sas_runtime.cpp ../host/thread_runtime.cpp)
target_include_directories(chili_mpeg_test PRIVATE "${REPO}/include" "${CHILI_MEDIA_HOST}")
target_link_libraries(chili_mpeg_test PRIVATE "${CHILI_FRAMEWORK_LIB}" chili_media_backend)
chili_copy_media_runtime(chili_mpeg_test)

add_executable(chili_lifecycle_test thread_lifecycle_test.cpp ../host/thread_runtime.cpp ../host/thread_lifecycle.cpp ../host/partition_memory.cpp)
target_include_directories(chili_lifecycle_test PRIVATE "${REPO}/include" "${CMAKE_CURRENT_SOURCE_DIR}/../host")
target_link_libraries(chili_lifecycle_test PRIVATE "${CHILI_FRAMEWORK_LIB}")

add_executable(chili_feedback_test ge_feedback_test.cpp)
target_include_directories(chili_feedback_test PRIVATE "${REPO}/include")
target_link_libraries(chili_feedback_test PRIVATE "${CHILI_FRAMEWORK_LIB}")

set_source_files_properties(../host/atrac_stream.cpp PROPERTIES INCLUDE_DIRECTORIES "${CHILI_FFMPEG}/include")
add_executable(chili_atrac_stream_test atrac_stream_test.cpp ../host/atrac_stream.cpp)
target_include_directories(chili_atrac_stream_test PRIVATE "${REPO}/include" "${CHILI_MEDIA_HOST}")
target_link_libraries(chili_atrac_stream_test PRIVATE "${CHILI_FRAMEWORK_LIB}" chili_media_backend)
chili_copy_media_runtime(chili_atrac_stream_test)
add_executable(chili_atrac_runtime_stream_test atrac_runtime_stream_test.cpp ../host/atrac_runtime.cpp ../host/atrac_stream.cpp ../host/av_runtime.cpp ../host/thread_runtime.cpp ../host/sas_runtime.cpp)
target_include_directories(chili_atrac_runtime_stream_test PRIVATE "${REPO}/include" "${CHILI_MEDIA_HOST}")
target_link_libraries(chili_atrac_runtime_stream_test PRIVATE "${CHILI_FRAMEWORK_LIB}" chili_media_backend)
chili_copy_media_runtime(chili_atrac_runtime_stream_test)
# FFmpeg's own decode of an ATRAC3plus RIFF file, the reference for tools/speech_compare.py.
add_executable(chili_at3_reference at3_reference.cpp)
target_include_directories(chili_at3_reference PRIVATE "${CHILI_MEDIA_HOST}")
target_link_libraries(chili_at3_reference PRIVATE chili_media_backend)
chili_copy_media_runtime(chili_at3_reference)
# The installer's music: the title and main menu music read from the player's own disc and written as a WAV
# (sound_export.cpp). ChiliSoundExport.exe needs only avcodec, avutil and swresample beside it (the C++
# runtime is linked in). The test needs no game data; with -DCHILI_TEST_ISO=<the game's .iso> it also
# exports both tracks from that disc and checks them.
add_executable(chili_sound_export sound_export.cpp)
set_target_properties(chili_sound_export PROPERTIES OUTPUT_NAME ChiliSoundExport MSVC_RUNTIME_LIBRARY MultiThreaded)
target_include_directories(chili_sound_export PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../host")
target_include_directories(chili_sound_export SYSTEM PRIVATE "${CHILI_FFMPEG}/include")
foreach(CHILI_CODEC_LIB avcodec avutil swresample)
    target_link_libraries(chili_sound_export PRIVATE "${CHILI_FFMPEG}/lib/${CHILI_CODEC_LIB}.lib")
endforeach()
chili_copy_media_runtime(chili_sound_export)
add_executable(chili_sound_export_test sound_export_test.cpp)
if(MSVC)
  target_compile_options(chili_sound_export PRIVATE /W4)
  target_compile_options(chili_sound_export_test PRIVATE /W4)
endif()
if(CHILI_TEST_ISO AND EXISTS "${CHILI_TEST_ISO}")
  add_test(NAME chili_sound_export COMMAND chili_sound_export_test "${CMAKE_BINARY_DIR}/sound-export-scratch" $<TARGET_FILE:chili_sound_export> "${CHILI_TEST_ISO}")
else()
  add_test(NAME chili_sound_export COMMAND chili_sound_export_test "${CMAKE_BINARY_DIR}/sound-export-scratch" $<TARGET_FILE:chili_sound_export>)
endif()
add_executable(chili_mip_texture_test ge_mip_texture_test.cpp)
target_include_directories(chili_mip_texture_test PRIVATE "${REPO}/include")
target_link_libraries(chili_mip_texture_test PRIVATE "${CHILI_FRAMEWORK_LIB}")

add_executable(chili_triangle_raster_test ge_triangle_raster_test.cpp)
target_include_directories(chili_triangle_raster_test PRIVATE "${REPO}/include")
target_link_libraries(chili_triangle_raster_test PRIVATE "${CHILI_FRAMEWORK_LIB}")

add_executable(chili_triangle_through_test ge_triangle_through_test.cpp)
target_include_directories(chili_triangle_through_test PRIVATE "${REPO}/include")
target_link_libraries(chili_triangle_through_test PRIVATE "${CHILI_FRAMEWORK_LIB}")

add_executable(chili_libc_time_test libc_time_runtime_test.cpp ../host/libc_time_runtime.cpp ../host/rtc_runtime.cpp ../host/thread_runtime.cpp)
target_include_directories(chili_libc_time_test PRIVATE "${REPO}/include")
target_link_libraries(chili_libc_time_test PRIVATE "${CHILI_FRAMEWORK_LIB}")

add_executable(chili_texture_formats_test ge_texture_formats_test.cpp)
target_include_directories(chili_texture_formats_test PRIVATE "${REPO}/include")
target_link_libraries(chili_texture_formats_test PRIVATE "${CHILI_FRAMEWORK_LIB}")

add_executable(chili_depth_precision_test ge_depth_precision_test.cpp)
target_include_directories(chili_depth_precision_test PRIVATE "${REPO}/include")
target_link_libraries(chili_depth_precision_test PRIVATE "${CHILI_FRAMEWORK_LIB}")

add_executable(chili_mip_texture_formats_test ge_mip_texture_formats_test.cpp)
target_include_directories(chili_mip_texture_formats_test PRIVATE "${REPO}/include")
target_link_libraries(chili_mip_texture_formats_test PRIVATE "${CHILI_FRAMEWORK_LIB}")

add_executable(chili_vertex_decoder_test ge_vertex_decoder_test.cpp)
target_include_directories(chili_vertex_decoder_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../host")

add_executable(chili_generalized_vertex_raster_test ge_generalized_vertex_raster_test.cpp ../host/thread_runtime.cpp)
target_include_directories(chili_generalized_vertex_raster_test PRIVATE "${REPO}/include" "${CMAKE_CURRENT_SOURCE_DIR}/../host")
target_link_libraries(chili_generalized_vertex_raster_test PRIVATE "${CHILI_FRAMEWORK_LIB}")

add_executable(chili_savedata_test savedata_runtime_test.cpp ../host/savedata_runtime.cpp ../host/savedata_size_query.cpp ../host/savedata_request.cpp ../host/thread_runtime.cpp)
target_include_directories(chili_savedata_test PRIVATE "${REPO}/include")
target_link_libraries(chili_savedata_test PRIVATE "${CHILI_FRAMEWORK_LIB}")
# The LIST request, its errors, and what a mode the port does not implement does (cancelled, never a stop).
add_test(NAME chili_savedata_list_runtime COMMAND chili_savedata_test "${CMAKE_CURRENT_SOURCE_DIR}/../analysis/savedata_request_ram.bin" "${CMAKE_BINARY_DIR}/savedata-list-scratch")
chili_capture_test(chili_savedata_list_runtime)

add_executable(chili_ge_deformation_raster_test ge_deformation_raster_test.cpp ../host/thread_runtime.cpp)
target_include_directories(chili_ge_deformation_raster_test PRIVATE "${REPO}/include" "${CMAKE_CURRENT_SOURCE_DIR}/../host")
target_link_libraries(chili_ge_deformation_raster_test PRIVATE "${CHILI_FRAMEWORK_LIB}")

add_executable(chili_message_dialog_test message_dialog_test.cpp ../host/message_dialog_runtime.cpp ../host/message_dialog_win32.cpp ../host/thread_runtime.cpp)
target_include_directories(chili_message_dialog_test PRIVATE "${REPO}/include" "${REPO}/profiles/chili/host")
target_link_libraries(chili_message_dialog_test PRIVATE "${CHILI_FRAMEWORK_LIB}" user32 gdi32)

add_executable(chili_message_presenter_acceptance message_presenter_acceptance.cpp ../host/message_dialog_win32.cpp)
target_include_directories(chili_message_presenter_acceptance PRIVATE "${REPO}/include" "${REPO}/profiles/chili/host")
target_link_libraries(chili_message_presenter_acceptance PRIVATE "${CHILI_FRAMEWORK_LIB}" user32 gdi32)
# Actual owned PSMF captures are read at test time, never embedded in the binary.
add_executable(chili_mpeg_capture_test mpeg_capture_test.cpp ../host/mpeg_runtime.cpp ../host/av_runtime.cpp ../host/sas_runtime.cpp ../host/thread_runtime.cpp)
target_include_directories(chili_mpeg_capture_test PRIVATE "${REPO}/include" "${CMAKE_CURRENT_SOURCE_DIR}/../host" "${CHILI_MEDIA_HOST}" "${CHILI_FFMPEG}/include")
target_link_libraries(chili_mpeg_capture_test PRIVATE "${CHILI_FRAMEWORK_LIB}" chili_media_backend)
chili_copy_media_runtime(chili_mpeg_capture_test)
enable_testing()
add_test(NAME chili_captured_valid_psmf COMMAND chili_mpeg_capture_test "${CMAKE_CURRENT_SOURCE_DIR}/../analysis" valid)
chili_capture_test(chili_captured_valid_psmf)

add_executable(chili_ge_lighting_raster_test ge_lighting_raster_test.cpp ../host/thread_runtime.cpp)
target_include_directories(chili_ge_lighting_raster_test PRIVATE "${REPO}/include" "${CMAKE_CURRENT_SOURCE_DIR}/../host")
target_link_libraries(chili_ge_lighting_raster_test PRIVATE "${CHILI_FRAMEWORK_LIB}")
add_test(NAME chili_ge_lighting_raster COMMAND chili_ge_lighting_raster_test)

add_executable(chili_savedata_sizes_runtime_test savedata_sizes_runtime_test.cpp ../host/savedata_runtime.cpp ../host/savedata_size_query.cpp ../host/savedata_request.cpp ../host/thread_runtime.cpp)
target_include_directories(chili_savedata_sizes_runtime_test PRIVATE "${REPO}/include" "${CMAKE_CURRENT_SOURCE_DIR}/../host")
target_link_libraries(chili_savedata_sizes_runtime_test PRIVATE "${CHILI_FRAMEWORK_LIB}")
add_test(NAME chili_savedata_sizes_runtime COMMAND chili_savedata_sizes_runtime_test "${CMAKE_CURRENT_SOURCE_DIR}/../analysis/savedata_sizes_request_ram.bin")
chili_capture_test(chili_savedata_sizes_runtime)

add_executable(chili_ge_fog_test ge_fog_test.cpp)
target_include_directories(chili_ge_fog_test PRIVATE "${REPO}/include" "${CMAKE_CURRENT_SOURCE_DIR}/../host")
add_test(NAME chili_ge_fog COMMAND chili_ge_fog_test)
add_executable(chili_ge_fog_raster_test ge_fog_raster_test.cpp ../host/thread_runtime.cpp)
target_include_directories(chili_ge_fog_raster_test PRIVATE "${REPO}/include" "${CMAKE_CURRENT_SOURCE_DIR}/../host")
target_link_libraries(chili_ge_fog_raster_test PRIVATE "${CHILI_FRAMEWORK_LIB}")
add_test(NAME chili_ge_fog_raster COMMAND chili_ge_fog_raster_test)
add_test(NAME chili_fog_regression_raster COMMAND chili_triangle_raster_test)
add_test(NAME chili_fog_regression_deformation COMMAND chili_ge_deformation_raster_test)
add_test(NAME chili_fog_regression_through COMMAND chili_triangle_through_test)
add_test(NAME chili_fog_regression_layout COMMAND chili_generalized_vertex_raster_test)
add_test(NAME chili_fog_regression_ge COMMAND chili_ge_test)

# Native PC save components; the game codec never enables deterministic test APIs.
add_executable(chili_native_save_codec_test native_save_codec_test.cpp ../host/native_save_codec.cpp)
target_include_directories(chili_native_save_codec_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../host")
target_compile_definitions(chili_native_save_codec_test PRIVATE CHILI_NATIVE_SAVE_CODEC_TESTING)
target_link_libraries(chili_native_save_codec_test PRIVATE bcrypt)
add_test(NAME chili_native_save_codec COMMAND chili_native_save_codec_test)
add_executable(chili_param_sfo_test param_sfo_test.cpp ../host/param_sfo.cpp)
target_include_directories(chili_param_sfo_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../host")
add_test(NAME chili_param_sfo COMMAND chili_param_sfo_test "${CMAKE_CURRENT_SOURCE_DIR}/../game/disc0/PSP_GAME/PARAM.SFO" "${CMAKE_CURRENT_SOURCE_DIR}/../analysis/sfo_reference_fixtures/pspsdk_default.sfo" "${CMAKE_CURRENT_SOURCE_DIR}/../analysis/sfo_reference_fixtures/mksfoex_default_derived.sfo")
chili_capture_test(chili_param_sfo)
add_executable(chili_savedata_storage_test savedata_storage_test.cpp ../host/savedata_storage.cpp)
target_include_directories(chili_savedata_storage_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../host")
add_test(NAME chili_savedata_storage COMMAND chili_savedata_storage_test "${CMAKE_CURRENT_SOURCE_DIR}/../analysis/chili-savedata-storage-prep")
chili_capture_test(chili_savedata_storage)
add_executable(chili_save_slot_presenter_test save_slot_presenter_test.cpp ../host/save_slot_win32.cpp)
target_include_directories(chili_save_slot_presenter_test PRIVATE "${REPO}/include" "${CMAKE_CURRENT_SOURCE_DIR}/../host")
target_link_libraries(chili_save_slot_presenter_test PRIVATE user32 gdi32)
add_test(NAME chili_save_slot_presenter COMMAND chili_save_slot_presenter_test)
add_executable(chili_save_request_test savedata_request_test.cpp ../host/savedata_request.cpp)
target_include_directories(chili_save_request_test PRIVATE "${REPO}/include" "${CMAKE_CURRENT_SOURCE_DIR}/../host")
target_link_libraries(chili_save_request_test PRIVATE "${CHILI_FRAMEWORK_LIB}")
add_test(NAME chili_save_request COMMAND chili_save_request_test "${CMAKE_CURRENT_SOURCE_DIR}/../analysis")
chili_capture_test(chili_save_request)

# Complete native profile operation, with actual captured input only at test runtime.
add_executable(chili_savedata_operation_test savedata_operation_test.cpp ../host/savedata_runtime.cpp ../host/savedata_size_query.cpp ../host/savedata_request.cpp ../host/thread_runtime.cpp)
target_include_directories(chili_savedata_operation_test PRIVATE "${REPO}/include" "${CMAKE_CURRENT_SOURCE_DIR}/../host")
target_link_libraries(chili_savedata_operation_test PRIVATE "${CHILI_FRAMEWORK_LIB}")
add_test(NAME chili_savedata_operation COMMAND chili_savedata_operation_test "${CMAKE_CURRENT_SOURCE_DIR}/../analysis")
chili_capture_test(chili_savedata_operation)

add_executable(chili_native_savedata_backend_test native_savedata_backend_test.cpp ../host/native_savedata_backend.cpp ../host/savedata_request.cpp ../host/param_sfo.cpp ../host/savedata_storage.cpp ../host/native_save_codec.cpp)
target_include_directories(chili_native_savedata_backend_test PRIVATE "${REPO}/include" "${CMAKE_CURRENT_SOURCE_DIR}/../host")
target_compile_definitions(chili_native_savedata_backend_test PRIVATE CHILI_NATIVE_SAVEDATA_BACKEND_TESTING)
target_link_libraries(chili_native_savedata_backend_test PRIVATE bcrypt "${CHILI_FRAMEWORK_LIB}")
add_test(NAME chili_native_savedata_backend COMMAND chili_native_savedata_backend_test "${CMAKE_CURRENT_SOURCE_DIR}/../analysis/chili-native-savedata-backend-fixtures" "${CMAKE_CURRENT_SOURCE_DIR}/../analysis")
chili_capture_test(chili_native_savedata_backend)
# The composition fixture uses exactly the production UI/backend/crypto interfaces.
add_executable(chili_native_save_acceptance_test native_save_acceptance.cpp ../host/savedata_runtime.cpp ../host/savedata_request.cpp ../host/native_savedata_backend.cpp ../host/save_slot_win32.cpp ../host/savedata_storage.cpp ../host/param_sfo.cpp ../host/native_save_codec.cpp ../host/savedata_size_query.cpp ../host/thread_runtime.cpp)
target_include_directories(chili_native_save_acceptance_test PRIVATE "${REPO}/include" "${CMAKE_CURRENT_SOURCE_DIR}/../host")
target_link_libraries(chili_native_save_acceptance_test PRIVATE bcrypt user32 gdi32 "${CHILI_FRAMEWORK_LIB}")
add_test(NAME chili_native_save_acceptance COMMAND chili_native_save_acceptance_test "${CMAKE_CURRENT_SOURCE_DIR}/../analysis/chili-native-save-acceptance-fixtures" "${CMAKE_CURRENT_SOURCE_DIR}/../analysis")
chili_capture_test(chili_native_save_acceptance)
# The saves after the first one: automatic save over the profile's slot, twenty in a row, the kept earlier
# save, faults during a replacement, the list save's overwrite question, modes the port does not implement,
# the delete list. Real runtime, backend and files; a made-up request, so it needs no capture file.
add_executable(chili_savedata_overwrite_test savedata_overwrite_test.cpp ../host/savedata_runtime.cpp ../host/savedata_request.cpp ../host/native_savedata_backend.cpp ../host/savedata_storage.cpp ../host/param_sfo.cpp ../host/native_save_codec.cpp ../host/savedata_size_query.cpp ../host/thread_runtime.cpp)
target_include_directories(chili_savedata_overwrite_test PRIVATE "${REPO}/include" "${CMAKE_CURRENT_SOURCE_DIR}/../host")
target_compile_definitions(chili_savedata_overwrite_test PRIVATE CHILI_NATIVE_SAVEDATA_BACKEND_TESTING)
target_link_libraries(chili_savedata_overwrite_test PRIVATE bcrypt "${CHILI_FRAMEWORK_LIB}")
add_test(NAME chili_savedata_overwrite COMMAND chili_savedata_overwrite_test)

# Explicit diagnostic guest input/capture and sampled native utility controls.
add_executable(chili_live_control_test diagnostic_live_channel_test.cpp)
target_include_directories(chili_live_control_test SYSTEM PRIVATE "${REPO}/include" "${CMAKE_CURRENT_SOURCE_DIR}/../host")
if(MSVC)
  target_compile_options(chili_live_control_test PRIVATE /W4 /WX /permissive-)
endif()
add_test(NAME chili_live_control COMMAND chili_live_control_test "${CMAKE_CURRENT_SOURCE_DIR}")
add_executable(chili_utility_input_test utility_input_test.cpp ../host/save_slot_win32.cpp ../host/message_dialog_win32.cpp)
target_include_directories(chili_utility_input_test PRIVATE "${REPO}/include" "${CMAKE_CURRENT_SOURCE_DIR}/../host")
target_link_libraries(chili_utility_input_test PRIVATE user32 gdi32 "${CHILI_FRAMEWORK_LIB}")
add_test(NAME chili_utility_input COMMAND chili_utility_input_test)

# Real PSP volatile backing, firmware outputs and native scheduler handoff.
add_executable(chili_volatile_memory_test volatile_memory_test.cpp ../host/volatile_memory_runtime.cpp ../host/thread_runtime.cpp ../host/thread_lifecycle.cpp)
target_include_directories(chili_volatile_memory_test PRIVATE "${REPO}/include" "${CMAKE_CURRENT_SOURCE_DIR}/../host")
target_link_libraries(chili_volatile_memory_test PRIVATE "${CHILI_FRAMEWORK_LIB}")
add_test(NAME chili_volatile_memory COMMAND chili_volatile_memory_test "${CMAKE_CURRENT_SOURCE_DIR}/../game/disc0/PSP_GAME/SYSDIR/BOOT.BIN")
chili_capture_test(chili_volatile_memory)

# Additional GE RGB equations, keeping legacy32/OFF behavior under comparison.
add_executable(chili_ge_blend_test ge_blend_test.cpp)
target_include_directories(chili_ge_blend_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../host")
if(MSVC)
  target_compile_options(chili_ge_blend_test PRIVATE /W4 /WX /permissive-)
endif()
add_test(NAME chili_ge_blend COMMAND chili_ge_blend_test)
add_executable(chili_ge_blend_integration_test ge_blend_integration_test.cpp ../host/thread_runtime.cpp)
target_include_directories(chili_ge_blend_integration_test PRIVATE "${REPO}/include" "${CMAKE_CURRENT_SOURCE_DIR}/../host" "${CMAKE_CURRENT_SOURCE_DIR}/ge_blend_baseline")
target_link_libraries(chili_ge_blend_integration_test PRIVATE "${CHILI_FRAMEWORK_LIB}")
add_test(NAME chili_ge_blend_integration COMMAND chili_ge_blend_integration_test)

# Actual mixed-W mission draw plus independent preclipped dyadic geometry.
add_executable(chili_ge_homogeneous_clip_test ge_homogeneous_clip_test.cpp)
target_include_directories(chili_ge_homogeneous_clip_test PRIVATE "${REPO}/include" "${CMAKE_CURRENT_SOURCE_DIR}/../host")
target_link_libraries(chili_ge_homogeneous_clip_test PRIVATE "${CHILI_FRAMEWORK_LIB}")
add_test(NAME chili_ge_homogeneous_clip COMMAND chili_ge_homogeneous_clip_test "${CMAKE_CURRENT_SOURCE_DIR}/../analysis/live_mission_20261002_02_ge_failure_16829_failure")
chili_capture_test(chili_ge_homogeneous_clip)

# Combined vertex morphing and skinning, including the actual first-mission capture.
add_executable(chili_ge_combined_deformation_test ge_combined_deformation_test.cpp ../host/thread_runtime.cpp)
target_include_directories(chili_ge_combined_deformation_test PRIVATE "${REPO}/include" "${CMAKE_CURRENT_SOURCE_DIR}/../host")
target_link_libraries(chili_ge_combined_deformation_test PRIVATE "${CHILI_FRAMEWORK_LIB}")
add_test(NAME chili_ge_combined_deformation COMMAND chili_ge_combined_deformation_test "${CMAKE_CURRENT_SOURCE_DIR}/../analysis/live_mission_20261002_03_ge_failure_11650_failure")
chili_capture_test(chili_ge_combined_deformation)

# Untextured through sprites, including independent whole-VRAM captured oracle.
add_executable(chili_ge_untextured_sprite_test ge_untextured_sprite_test.cpp ../host/thread_runtime.cpp)
target_include_directories(chili_ge_untextured_sprite_test PRIVATE "${REPO}/include" "${CMAKE_CURRENT_SOURCE_DIR}/../host")
target_link_libraries(chili_ge_untextured_sprite_test PRIVATE "${CHILI_FRAMEWORK_LIB}")
add_test(NAME chili_ge_untextured_sprite COMMAND chili_ge_untextured_sprite_test "${CMAKE_CURRENT_SOURCE_DIR}/../analysis/live_mission_20261002_04_ge_failure_13061_failure" "${CMAKE_CURRENT_SOURCE_DIR}/../analysis/untextured_sprite_expected_vram.bin")
chili_capture_test(chili_ge_untextured_sprite)

# Optional observer acceptance; counters do not establish gameplay FPS.
add_executable(chili_performance_telemetry_test performance_telemetry_test.cpp ../host/thread_runtime.cpp)
target_include_directories(chili_performance_telemetry_test PRIVATE "${REPO}/include" "${CMAKE_CURRENT_SOURCE_DIR}/../host")
target_link_libraries(chili_performance_telemetry_test PRIVATE "${CHILI_FRAMEWORK_LIB}" user32 gdi32)
if(MSVC)
  target_compile_options(chili_performance_telemetry_test PRIVATE /W4 /WX /wd4244 /wd4324)
  target_compile_definitions(chili_performance_telemetry_test PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_test(NAME chili_performance_telemetry COMMAND chili_performance_telemetry_test)

# Documented blend factors and the actual destination-color postprocess draw.
add_executable(chili_ge_dstcolor_sprite_test ge_dstcolor_sprite_test.cpp ../host/thread_runtime.cpp)
target_include_directories(chili_ge_dstcolor_sprite_test PRIVATE "${REPO}/include" "${CMAKE_CURRENT_SOURCE_DIR}/../host")
target_link_libraries(chili_ge_dstcolor_sprite_test PRIVATE "${CHILI_FRAMEWORK_LIB}")
add_test(NAME chili_ge_dstcolor_sprite COMMAND chili_ge_dstcolor_sprite_test "${CMAKE_CURRENT_SOURCE_DIR}/../analysis/live_mission_20261002_05_ge_failure_10751_failure" "${CMAKE_CURRENT_SOURCE_DIR}/../analysis/destination_blend_expected_vram.bin")
chili_capture_test(chili_ge_dstcolor_sprite)
add_executable(chili_ge_dstcolor_integration_test ge_dstcolor_integration_test.cpp ../host/thread_runtime.cpp)
target_include_directories(chili_ge_dstcolor_integration_test PRIVATE "${REPO}/include" "${CMAKE_CURRENT_SOURCE_DIR}/../host")
target_link_libraries(chili_ge_dstcolor_integration_test PRIVATE "${CHILI_FRAMEWORK_LIB}")
add_test(NAME chili_ge_dstcolor_integration COMMAND chili_ge_dstcolor_integration_test)
add_executable(chili_ge_blend_factors_test ge_blend_factors_test.cpp)
target_include_directories(chili_ge_blend_factors_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../host")
add_test(NAME chili_ge_blend_factors COMMAND chili_ge_blend_factors_test)

# Sparse video-write provenance, preserving disabled/enabled native ABI outputs.
add_executable(chili_mpeg_provenance_test mpeg_provenance_test.cpp ../host/mpeg_runtime.cpp ../host/av_runtime.cpp ../host/sas_runtime.cpp ../host/thread_runtime.cpp)
target_include_directories(chili_mpeg_provenance_test PRIVATE "${REPO}/include" "${CMAKE_CURRENT_SOURCE_DIR}/../host" "${CHILI_MEDIA_HOST}")
target_include_directories(chili_mpeg_provenance_test SYSTEM PRIVATE "${CHILI_FFMPEG}/include")
if(MSVC)
  target_compile_options(chili_mpeg_provenance_test PRIVATE /W4 /WX /EHsc /wd4324)
  target_compile_definitions(chili_mpeg_provenance_test PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
target_link_libraries(chili_mpeg_provenance_test PRIVATE "${CHILI_FRAMEWORK_LIB}" chili_media_backend)
chili_copy_media_runtime(chili_mpeg_provenance_test)
add_test(NAME chili_mpeg_provenance COMMAND chili_mpeg_provenance_test "${CMAKE_CURRENT_SOURCE_DIR}/../game/disc0/PSP_GAME/ICON1.PMF")
chili_capture_test(chili_mpeg_provenance)
add_test(NAME chili_mpeg_runtime_regression COMMAND chili_mpeg_test "${CMAKE_CURRENT_SOURCE_DIR}/../game/disc0/PSP_GAME/ICON1.PMF" "${CMAKE_CURRENT_SOURCE_DIR}/../game/disc0/PSP_GAME/SND0.AT3")
chili_capture_test(chili_mpeg_runtime_regression)

# Coarse sprite phase measurement, with full captured renderer-state parity.
add_executable(chili_nontriangle_telemetry_test nontriangle_telemetry_test.cpp ../host/thread_runtime.cpp)
target_include_directories(chili_nontriangle_telemetry_test PRIVATE "${REPO}/include" "${CMAKE_CURRENT_SOURCE_DIR}/../host" "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(chili_nontriangle_telemetry_test PRIVATE "${CHILI_FRAMEWORK_LIB}" user32 gdi32)
if(MSVC)
  target_compile_options(chili_nontriangle_telemetry_test PRIVATE /W4 /WX /wd4244 /wd4324)
  target_compile_definitions(chili_nontriangle_telemetry_test PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_test(NAME chili_nontriangle_telemetry COMMAND chili_nontriangle_telemetry_test "${CMAKE_CURRENT_SOURCE_DIR}/../analysis/live_mission_20261002_04_ge_failure_13061_failure" "${CMAKE_CURRENT_SOURCE_DIR}/../analysis/ge_after_coincident_17_failure")
chili_capture_test(chili_nontriangle_telemetry)

# Exact contributing taps and existing scalar APIs; no texture snapshot or epsilon.
add_executable(chili_ge_zero_tap_feedback_test ge_zero_tap_feedback_test.cpp ../host/thread_runtime.cpp)
target_include_directories(chili_ge_zero_tap_feedback_test PRIVATE "${REPO}/include" "${CMAKE_CURRENT_SOURCE_DIR}/../host" "${CMAKE_CURRENT_SOURCE_DIR}/zero_tap_feedback_baseline")
target_link_libraries(chili_ge_zero_tap_feedback_test PRIVATE "${CHILI_FRAMEWORK_LIB}" user32 gdi32)
add_executable(chili_sprite_accessor_test sprite_accessor_test.cpp ../host/thread_runtime.cpp)
target_include_directories(chili_sprite_accessor_test PRIVATE "${REPO}/include" "${CMAKE_CURRENT_SOURCE_DIR}/../host" "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(chili_sprite_accessor_test PRIVATE "${CHILI_FRAMEWORK_LIB}" user32 gdi32)
add_executable(chili_sprite_memory_accessor_test memory_accessor_test.cpp ../host/thread_runtime.cpp)
target_include_directories(chili_sprite_memory_accessor_test PRIVATE "${REPO}/include" "${CMAKE_CURRENT_SOURCE_DIR}/../host")
target_link_libraries(chili_sprite_memory_accessor_test PRIVATE "${CHILI_FRAMEWORK_LIB}" user32 gdi32)
if(MSVC)
  foreach(chili_scalar_target chili_ge_zero_tap_feedback_test chili_sprite_accessor_test chili_sprite_memory_accessor_test)
    target_compile_options(${chili_scalar_target} PRIVATE /W4 /WX /wd4244 /wd4324)
    target_compile_definitions(${chili_scalar_target} PRIVATE _CRT_SECURE_NO_WARNINGS)
  endforeach()
endif()
add_test(NAME chili_ge_zero_tap_feedback COMMAND chili_ge_zero_tap_feedback_test "${CMAKE_CURRENT_SOURCE_DIR}/../analysis/live_mission_20261002_06_ge_failure_13219_failure" "${CMAKE_CURRENT_SOURCE_DIR}/../analysis/zero_tap_feedback_expected_vram.bin")
chili_capture_test(chili_ge_zero_tap_feedback)
add_test(NAME chili_sprite_accessor COMMAND chili_sprite_accessor_test "${CMAKE_CURRENT_SOURCE_DIR}/../analysis/live_mission_20261002_04_ge_failure_13061_failure" "${CMAKE_CURRENT_SOURCE_DIR}/../analysis/ge_after_coincident_17_failure" "${CMAKE_CURRENT_SOURCE_DIR}/../analysis/live_mission_20261002_05_ge_failure_10751_failure")
chili_capture_test(chili_sprite_accessor)
add_test(NAME chili_sprite_memory_accessor COMMAND chili_sprite_memory_accessor_test)
add_test(NAME chili_sprite_memory_accessor_watch_vram COMMAND chili_sprite_memory_accessor_test --watch)
set_tests_properties(chili_sprite_memory_accessor_watch_vram PROPERTIES ENVIRONMENT "PSPRECOMP_WATCH_WRITE=0x04000000;PSPRECOMP_WATCH_WRITE_SIZE=0x00800000")
add_test(NAME chili_sprite_memory_accessor_watch_ram COMMAND chili_sprite_memory_accessor_test --watch)
set_tests_properties(chili_sprite_memory_accessor_watch_ram PROPERTIES ENVIRONMENT "PSPRECOMP_WATCH_WRITE=0x08000000;PSPRECOMP_WATCH_WRITE_SIZE=0x02000000")
add_test(NAME chili_sprite_memory_accessor_watch_scratch COMMAND chili_sprite_memory_accessor_test --watch)
set_tests_properties(chili_sprite_memory_accessor_watch_scratch PROPERTIES ENVIRONMENT "PSPRECOMP_WATCH_WRITE=0x00010000;PSPRECOMP_WATCH_WRITE_SIZE=0x00004000")

# Finite signed quad helper interpolation; covered/pole checks remain strict.
add_executable(chili_ge_mip_footprint_test ge_mip_footprint_test.cpp ../host/thread_runtime.cpp)
target_include_directories(chili_ge_mip_footprint_test PRIVATE "${REPO}/include" "${CMAKE_CURRENT_SOURCE_DIR}/../host" "${CMAKE_CURRENT_SOURCE_DIR}/mip_helper_baseline")
target_link_libraries(chili_ge_mip_footprint_test PRIVATE "${CHILI_FRAMEWORK_LIB}" user32 gdi32)
if(MSVC)
  target_compile_options(chili_ge_mip_footprint_test PRIVATE /W4 /WX /wd4244 /wd4324)
  target_compile_definitions(chili_ge_mip_footprint_test PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_test(NAME chili_ge_mip_footprint COMMAND chili_ge_mip_footprint_test "${CMAKE_CURRENT_SOURCE_DIR}/../analysis/live_mission_20261002_07_ge_failure_12753_failure")
chili_capture_test(chili_ge_mip_footprint)
add_test(NAME chili_mip_texture_regression COMMAND chili_mip_texture_test)
add_test(NAME chili_mip_texture_formats_regression COMMAND chili_mip_texture_formats_test)

# Optional integer pacing attribution; audio completes on the guest clock, no device is opened.
add_executable(chili_pacing_counter_test pacing_counter_test.cpp ../host/thread_runtime.cpp ../host/audio_runtime.cpp ../host/audio_feed.cpp)
target_include_directories(chili_pacing_counter_test PRIVATE "${REPO}/include" "${CMAKE_CURRENT_SOURCE_DIR}/../host")
target_link_libraries(chili_pacing_counter_test PRIVATE "${CHILI_FRAMEWORK_LIB}" ole32 avrt)
if(MSVC)
  target_compile_options(chili_pacing_counter_test PRIVATE /W4 /WX /wd4244 /wd4324)
  target_compile_definitions(chili_pacing_counter_test PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_test(NAME chili_pacing_counter COMMAND chili_pacing_counter_test "${CMAKE_CURRENT_SOURCE_DIR}/../analysis/chili-pacing-counter-fixtures/candidate_trace.bin")
chili_capture_test(chili_pacing_counter)
add_test(NAME chili_pacing_thread_regression COMMAND chili_thread_test)
add_test(NAME chili_pacing_display_regression COMMAND chili_display_test)
add_test(NAME chili_pacing_callback_regression COMMAND chili_callback_test)

# Callback removal preserves an entered finish interrupt's saved context.
add_executable(chili_ge_callback_lifecycle_test ge_callback_lifecycle_test.cpp ../host/thread_runtime.cpp)
target_include_directories(chili_ge_callback_lifecycle_test PRIVATE "${REPO}/include" "${CMAKE_CURRENT_SOURCE_DIR}/../host")
target_link_libraries(chili_ge_callback_lifecycle_test PRIVATE "${CHILI_FRAMEWORK_LIB}" user32 gdi32)
if(MSVC)
  target_compile_options(chili_ge_callback_lifecycle_test PRIVATE /W4 /WX /wd4244 /wd4324)
  target_compile_definitions(chili_ge_callback_lifecycle_test PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_test(NAME chili_ge_callback_lifecycle COMMAND chili_ge_callback_lifecycle_test)

# Captured triangle commit fast-path parity; timing is an explicit fixture option.
add_executable(chili_triangle_accessor_test triangle_accessor_benchmark_test.cpp ../host/thread_runtime.cpp)
target_include_directories(chili_triangle_accessor_test PRIVATE "${REPO}/include" "${CMAKE_CURRENT_SOURCE_DIR}/../host" "${CMAKE_CURRENT_SOURCE_DIR}/triangle_accessor_baseline")
target_link_libraries(chili_triangle_accessor_test PRIVATE "${CHILI_FRAMEWORK_LIB}" user32 gdi32)
if(MSVC)
  target_compile_options(chili_triangle_accessor_test PRIVATE /W4 /WX /wd4244 /wd4324)
  target_compile_definitions(chili_triangle_accessor_test PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_test(NAME chili_triangle_accessor COMMAND chili_triangle_accessor_test "${CMAKE_CURRENT_SOURCE_DIR}/../analysis/live_mission_20261002_07_ge_failure_12753_failure")
chili_capture_test(chili_triangle_accessor)
add_test(NAME chili_triangle_accessor_watch COMMAND chili_triangle_accessor_test "${CMAKE_CURRENT_SOURCE_DIR}/../analysis/live_mission_20261002_07_ge_failure_12753_failure" --watch)
chili_capture_test(chili_triangle_accessor_watch)
set_tests_properties(chili_triangle_accessor_watch PROPERTIES ENVIRONMENT "PSPRECOMP_WATCH_WRITE=0x04000000;PSPRECOMP_WATCH_WRITE_SIZE=0x00800000")

# Visible through-sprite mip filtering and shared public texture-read parity.
add_executable(chili_ge_sprite_mip_test ge_sprite_mip_test.cpp ../host/thread_runtime.cpp)
target_include_directories(chili_ge_sprite_mip_test PRIVATE "${REPO}/include" "${CMAKE_CURRENT_SOURCE_DIR}/../host" "${CMAKE_CURRENT_SOURCE_DIR}/sprite_mip_baseline")
target_link_libraries(chili_ge_sprite_mip_test PRIVATE "${CHILI_FRAMEWORK_LIB}" user32 gdi32)
add_executable(chili_texture_accessor_test texture_accessor_test.cpp ../host/thread_runtime.cpp)
target_include_directories(chili_texture_accessor_test PRIVATE "${REPO}/include" "${CMAKE_CURRENT_SOURCE_DIR}/../host" "${CMAKE_CURRENT_SOURCE_DIR}/texture_accessor_baseline")
target_link_libraries(chili_texture_accessor_test PRIVATE "${CHILI_FRAMEWORK_LIB}" user32 gdi32)
add_executable(chili_prepared_sampler_parity_test prepared_sampler_parity_test.cpp)
target_include_directories(chili_prepared_sampler_parity_test PRIVATE "${REPO}/include" "${CMAKE_CURRENT_SOURCE_DIR}/../host" "${CMAKE_CURRENT_SOURCE_DIR}/texture_accessor_baseline")
target_link_libraries(chili_prepared_sampler_parity_test PRIVATE "${CHILI_FRAMEWORK_LIB}")
if(MSVC)
  foreach(chili_sampler_target chili_ge_sprite_mip_test chili_texture_accessor_test chili_prepared_sampler_parity_test)
    target_compile_options(${chili_sampler_target} PRIVATE /W4 /WX /wd4244 /wd4324)
    target_compile_definitions(${chili_sampler_target} PRIVATE _CRT_SECURE_NO_WARNINGS)
  endforeach()
endif()
add_test(NAME chili_ge_sprite_mip COMMAND chili_ge_sprite_mip_test "${CMAKE_CURRENT_SOURCE_DIR}/../analysis/live_mission_20261002_08_ge_failure_15323_failure")
chili_capture_test(chili_ge_sprite_mip)
add_test(NAME chili_texture_accessor COMMAND chili_texture_accessor_test "${CMAKE_CURRENT_SOURCE_DIR}/../analysis/live_mission_20261002_07_ge_failure_12753_failure" "${CMAKE_CURRENT_SOURCE_DIR}/../analysis/live_mission_20261002_02_ge_failure_16829_failure")
chili_capture_test(chili_texture_accessor)
add_test(NAME chili_prepared_sampler_parity COMMAND chili_prepared_sampler_parity_test)

# Per-triangle T4 AUTO quad memo, exact helper trace and captured HLE parity.
add_executable(chili_quad_cache_test quad_cache_test.cpp ../host/thread_runtime.cpp)
add_executable(chili_quad_cache_capture_test quad_cache_capture_test.cpp ../host/thread_runtime.cpp)
foreach(chili_quad_target chili_quad_cache_test chili_quad_cache_capture_test)
  target_include_directories(${chili_quad_target} PRIVATE "${REPO}/include" "${CMAKE_CURRENT_SOURCE_DIR}/../host" "${CMAKE_CURRENT_SOURCE_DIR}/quad_cache_baseline")
  target_link_libraries(${chili_quad_target} PRIVATE "${CHILI_FRAMEWORK_LIB}" user32 gdi32)
  if(MSVC)
    target_compile_options(${chili_quad_target} PRIVATE /W4 /WX /wd4244 /wd4324)
    target_compile_definitions(${chili_quad_target} PRIVATE _CRT_SECURE_NO_WARNINGS)
  endif()
endforeach()
add_test(NAME chili_quad_cache COMMAND chili_quad_cache_test)
add_test(NAME chili_quad_cache_capture COMMAND chili_quad_cache_capture_test "${CMAKE_CURRENT_SOURCE_DIR}/../analysis/live_mission_20261002_07_ge_failure_12753_failure" "${CMAKE_CURRENT_SOURCE_DIR}/../analysis/live_mission_20261002_02_ge_failure_16829_failure")
chili_capture_test(chili_quad_cache_capture)

# Passive successful guest-controller-read provenance; no new input sampling.
add_executable(chili_controller_sampling_test controller_sampling_test.cpp ../host/controller_runtime.cpp ../host/thread_runtime.cpp)
add_executable(chili_controller_sampling_baseline_test controller_sampling_test.cpp controller_sampling_baseline/controller_runtime.cpp ../host/thread_runtime.cpp)
target_include_directories(chili_controller_sampling_baseline_test BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/controller_sampling_baseline")
target_compile_definitions(chili_controller_sampling_baseline_test PRIVATE CONTROLLER_ORIGINAL)
foreach(chili_ctrl_sampling_target chili_controller_sampling_test chili_controller_sampling_baseline_test)
  target_include_directories(${chili_ctrl_sampling_target} PRIVATE "${REPO}/include" "${CMAKE_CURRENT_SOURCE_DIR}/../host")
  target_link_libraries(${chili_ctrl_sampling_target} PRIVATE "${CHILI_FRAMEWORK_LIB}" xinput9_1_0 user32)
  if(MSVC)
    target_compile_options(${chili_ctrl_sampling_target} PRIVATE /W4 /WX /wd4244 /wd4324)
    target_compile_definitions(${chili_ctrl_sampling_target} PRIVATE _CRT_SECURE_NO_WARNINGS)
  endif()
endforeach()
add_test(NAME chili_controller_sampling COMMAND chili_controller_sampling_test "${CMAKE_CURRENT_SOURCE_DIR}/../analysis/chili-controller-sampling-fixtures/candidate_trace.bin")
chili_capture_test(chili_controller_sampling)
add_test(NAME chili_controller_sampling_baseline COMMAND chili_controller_sampling_baseline_test "${CMAKE_CURRENT_SOURCE_DIR}/../analysis/chili-controller-sampling-fixtures/baseline_trace.bin")
chili_capture_test(chili_controller_sampling_baseline)
add_test(NAME chili_controller_sampling_original_parity COMMAND "${CMAKE_COMMAND}" -E compare_files "${CMAKE_CURRENT_SOURCE_DIR}/../analysis/chili-controller-sampling-fixtures/baseline_trace.bin" "${CMAKE_CURRENT_SOURCE_DIR}/../analysis/chili-controller-sampling-fixtures/candidate_trace.bin")
chili_capture_test(chili_controller_sampling_original_parity)
set_tests_properties(chili_controller_sampling_original_parity PROPERTIES DEPENDS "chili_controller_sampling_baseline;chili_controller_sampling")
add_test(NAME chili_controller_sampling_abi_regression COMMAND chili_controller_test)

# Validate only contributing mip levels; original/full sampler and captured oracle.
add_executable(chili_required_mip_levels_test ge_required_mip_levels_test.cpp ../host/thread_runtime.cpp)
target_include_directories(chili_required_mip_levels_test PRIVATE "${REPO}/include" "${CMAKE_CURRENT_SOURCE_DIR}/../host")
target_link_libraries(chili_required_mip_levels_test PRIVATE "${CHILI_FRAMEWORK_LIB}" user32 gdi32)
if(MSVC)
  target_compile_options(chili_required_mip_levels_test PRIVATE /W4 /WX /wd4244 /wd4324)
  target_compile_definitions(chili_required_mip_levels_test PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_test(NAME chili_required_mip_levels COMMAND chili_required_mip_levels_test "${CMAKE_CURRENT_SOURCE_DIR}/../analysis/live_mission_20261002_09_ge_failure_33026_failure")
chili_capture_test(chili_required_mip_levels)

add_executable(chili_effective_texture_pitch_test ge_effective_texture_pitch_test.cpp ../host/thread_runtime.cpp)
target_include_directories(chili_effective_texture_pitch_test PRIVATE "${REPO}/include" "${CMAKE_CURRENT_SOURCE_DIR}/../host")
target_link_libraries(chili_effective_texture_pitch_test PRIVATE "${CHILI_FRAMEWORK_LIB}" user32 gdi32)
if(MSVC)
  target_compile_options(chili_effective_texture_pitch_test PRIVATE /W4 /WX /wd4244 /wd4324)
  target_compile_definitions(chili_effective_texture_pitch_test PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_test(NAME chili_effective_texture_pitch COMMAND chili_effective_texture_pitch_test "${CMAKE_CURRENT_SOURCE_DIR}/../analysis/live_mission_20261002_10_ge_failure_32848_failure")
chili_capture_test(chili_effective_texture_pitch)

# Synchronous parallel triangle shading with ordered main-thread commits.
add_executable(chili_parallel_pool_test parallel_pool_test.cpp)
add_executable(chili_parallel_raster_test parallel_raster_test.cpp)
add_executable(chili_parallel_capture_test parallel_capture_test.cpp ../host/thread_runtime.cpp)
add_executable(chili_parallel_capture_diagnostic parallel_capture_test.cpp ../host/thread_runtime.cpp)
add_executable(chili_parallel_color_watch_test parallel_capture_watch.cpp ../host/thread_runtime.cpp)
add_executable(chili_parallel_depth_watch_test parallel_depth_watch.cpp)
foreach(chili_parallel_target chili_parallel_pool_test chili_parallel_raster_test chili_parallel_capture_test chili_parallel_capture_diagnostic chili_parallel_color_watch_test chili_parallel_depth_watch_test)
  target_include_directories(${chili_parallel_target} PRIVATE "${REPO}/include" "${CMAKE_CURRENT_SOURCE_DIR}/../host" "${CMAKE_CURRENT_SOURCE_DIR}/parallel_shading_baseline")
  target_link_libraries(${chili_parallel_target} PRIVATE "${CHILI_FRAMEWORK_LIB}" user32 gdi32)
  if(MSVC)
    target_compile_options(${chili_parallel_target} PRIVATE /W4 /WX /wd4244 /wd4324 /fp:precise)
    target_compile_definitions(${chili_parallel_target} PRIVATE _CRT_SECURE_NO_WARNINGS)
  endif()
endforeach()
foreach(chili_parallel_diagnostic chili_parallel_raster_test chili_parallel_capture_diagnostic chili_parallel_color_watch_test chili_parallel_depth_watch_test)
  target_compile_definitions(${chili_parallel_diagnostic} PRIVATE CHILI_PARALLEL_SHADE_DIAGNOSTIC)
endforeach()
set(CHILI_PARALLEL_CAPTURE_ARGS "${CMAKE_CURRENT_SOURCE_DIR}/../analysis/live_mission_20261002_07_ge_failure_12753_failure" "${CMAKE_CURRENT_SOURCE_DIR}/../analysis/live_mission_20261002_02_ge_failure_16829_failure" "${CMAKE_CURRENT_SOURCE_DIR}/../analysis/live_mission_20261002_09_ge_failure_33026_failure")
add_test(NAME chili_parallel_pool COMMAND chili_parallel_pool_test)
add_test(NAME chili_parallel_raster COMMAND chili_parallel_raster_test)
add_test(NAME chili_parallel_capture COMMAND chili_parallel_capture_test ${CHILI_PARALLEL_CAPTURE_ARGS})
chili_capture_test(chili_parallel_capture)
add_test(NAME chili_parallel_capture_diagnostic COMMAND chili_parallel_capture_diagnostic ${CHILI_PARALLEL_CAPTURE_ARGS})
chili_capture_test(chili_parallel_capture_diagnostic)
add_test(NAME chili_parallel_color_watch COMMAND chili_parallel_color_watch_test "${CMAKE_CURRENT_SOURCE_DIR}/../analysis/live_mission_20261002_02_ge_failure_16829_failure")
chili_capture_test(chili_parallel_color_watch)
set_tests_properties(chili_parallel_color_watch PROPERTIES ENVIRONMENT "PSPRECOMP_WATCH_WRITE=0x040E8000;PSPRECOMP_WATCH_WRITE_SIZE=64")
add_test(NAME chili_parallel_depth_watch COMMAND chili_parallel_depth_watch_test)
set_tests_properties(chili_parallel_depth_watch PROPERTIES ENVIRONMENT "PSPRECOMP_WATCH_WRITE=0x04080000;PSPRECOMP_WATCH_WRITE_SIZE=2")

# Source-only proposed native caller/outer-telemetry acceptance. Root execution
# is required after applying these exact sources; these are not benchmark tests.
foreach(chili_observer_mode on off)
  set(chili_observer_target chili_parallel_live_integration_${chili_observer_mode})
  add_executable(${chili_observer_target} parallel_live_integration_test.cpp ../host/thread_runtime.cpp)
  target_include_directories(${chili_observer_target} PRIVATE "${REPO}/include" "${CMAKE_CURRENT_SOURCE_DIR}/../host")
  target_link_libraries(${chili_observer_target} PRIVATE "${CHILI_FRAMEWORK_LIB}" user32 gdi32)
  if(MSVC)
    target_compile_options(${chili_observer_target} PRIVATE /W4 /WX /wd4244 /wd4324 /fp:precise)
    target_compile_definitions(${chili_observer_target} PRIVATE _CRT_SECURE_NO_WARNINGS)
  endif()
  if(chili_observer_mode STREQUAL "on")
    target_compile_definitions(${chili_observer_target} PRIVATE CHILI_PARALLEL_SHADE_OBSERVATION)
  endif()
  add_test(NAME ${chili_observer_target} COMMAND ${chili_observer_target})
endforeach()

# >>> chili_ge_replay: re-executes a captured GE list offline (host/ge_frame_capture.hpp).
# The d3d12 backend needs the renderer sources, as in the game's own build (../CMakeLists.txt).
file(GLOB CHILI_GPU_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../host/gpu/*.cpp")
add_executable(chili_ge_replay chili_ge_replay.cpp ../host/thread_runtime.cpp ${CHILI_GPU_SOURCES})
target_include_directories(chili_ge_replay PRIVATE "${REPO}/include" "${CMAKE_CURRENT_SOURCE_DIR}/../host")
target_link_libraries(chili_ge_replay PRIVATE "${CHILI_FRAMEWORK_LIB}" d3d12 dxgi dxguid d3dcompiler)
if(MSVC)
  target_compile_options(chili_ge_replay PRIVATE /W4 /wd4244 /wd4324 /fp:precise)
  target_compile_definitions(chili_ge_replay PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
# <<< chili_ge_replay

# State hash, PNG writer, input record/replay, live channel, frame timer,
# guest-clocked audio sink, scripted dialogs and run options.
add_executable(chili_run_support_test run_support_test.cpp ../host/controller_runtime.cpp ../host/thread_runtime.cpp)
target_include_directories(chili_run_support_test PRIVATE "${REPO}/include" "${CMAKE_CURRENT_SOURCE_DIR}/../host")
target_link_libraries(chili_run_support_test PRIVATE "${CHILI_FRAMEWORK_LIB}" xinput9_1_0 user32 gdi32 winmm)
if(MSVC)
  target_compile_definitions(chili_run_support_test PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_test(NAME chili_run_support COMMAND chili_run_support_test "${CMAKE_BINARY_DIR}/run-support-scratch")

# Keyboard, mouse and gamepad bindings (ChiliNative.ini) and the real-time pacer.
add_executable(chili_pc_input_test pc_input_test.cpp)
target_include_directories(chili_pc_input_test PRIVATE "${REPO}/include" "${CMAKE_CURRENT_SOURCE_DIR}/../host")
target_link_libraries(chili_pc_input_test PRIVATE xinput9_1_0 user32)
if(MSVC)
  target_compile_options(chili_pc_input_test PRIVATE /W4 /WX /permissive- /wd4324)
  target_compile_definitions(chili_pc_input_test PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_test(NAME chili_pc_input COMMAND chili_pc_input_test)

# The game's vibration call through the host function over its pad's empty SetControllerVibration.
add_executable(chili_pad_rumble_hook_test pad_rumble_hook_test.cpp)
target_include_directories(chili_pad_rumble_hook_test PRIVATE "${REPO}/include" "${CMAKE_CURRENT_SOURCE_DIR}/../host")
target_link_libraries(chili_pad_rumble_hook_test PRIVATE "${CHILI_FRAMEWORK_LIB}" xinput9_1_0 user32)
if(MSVC)
  target_compile_options(chili_pad_rumble_hook_test PRIVATE /W4 /WX /permissive- /wd4324)
  target_compile_definitions(chili_pad_rumble_hook_test PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_test(NAME chili_pad_rumble_hook COMMAND chili_pad_rumble_hook_test)

# The two-stick control layer over a made-up player in guest memory: where a frame's turn goes on foot, in
# a dive, on the ground after it, on a wall; the exact stick; the view's pitch.
add_executable(chili_dual_analog_test dual_analog_test.cpp ../host/dual_analog.cpp)
target_include_directories(chili_dual_analog_test PRIVATE "${REPO}/include" "${CMAKE_CURRENT_SOURCE_DIR}/../host")
target_link_libraries(chili_dual_analog_test PRIVATE "${CHILI_FRAMEWORK_LIB}" xinput9_1_0 user32)
if(MSVC)
  target_compile_options(chili_dual_analog_test PRIVATE /W4 /permissive- /wd4324)
  target_compile_definitions(chili_dual_analog_test PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_test(NAME chili_dual_analog COMMAND chili_dual_analog_test)

# Mod file layer: NAZ directory, spliced archive view, configpsp.txt overlay, mod
# selection, guest IO over the layers. With the game's disc folder present it
# also lists the real archive through the layer and compares files.
add_executable(chili_mod_files_test mod_files_test.cpp ../host/file_runtime.cpp)
target_include_directories(chili_mod_files_test PRIVATE "${REPO}/include" "${CMAKE_CURRENT_SOURCE_DIR}/../host")
target_link_libraries(chili_mod_files_test PRIVATE "${CHILI_FRAMEWORK_LIB}")
if(MSVC)
  target_compile_options(chili_mod_files_test PRIVATE /W4 /wd4324)
  target_compile_definitions(chili_mod_files_test PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
# The game's disc folder (BUILDING.md: extracted from your own .iso); another place is given with -DCHILI_MOD_TEST_DISC=<folder>.
set(CHILI_MOD_TEST_DISC "${CMAKE_CURRENT_SOURCE_DIR}/../game/disc0" CACHE PATH "Disc folder read by the mod layer tests")
if(EXISTS "${CHILI_MOD_TEST_DISC}/PSP_GAME/USRDIR/assets00.naz")
  add_test(NAME chili_mod_files COMMAND chili_mod_files_test "${CMAKE_BINARY_DIR}/mod-files-scratch" "${CHILI_MOD_TEST_DISC}")
else()
  add_test(NAME chili_mod_files COMMAND chili_mod_files_test "${CMAKE_BINARY_DIR}/mod-files-scratch")
endif()

# The "Settings" entries in the game's own menus (host/menu_entry_blocks.hpp, host/settings_entry.hpp): the main
# menu's Fiesta line turned in place and the pause line appended, on fragments and blocks made in the game's
# layout; the look at the game's menu with every pad sample, on a made-up guest memory; the pause legend's
# rectangles. With the game's disc folder present every block of the real archive is edited and read back.
add_executable(chili_menu_entry_test menu_entry_test.cpp settings_entry_test.cpp)
target_include_directories(chili_menu_entry_test PRIVATE "${REPO}/include" "${CMAKE_CURRENT_SOURCE_DIR}/../host")
target_link_libraries(chili_menu_entry_test PRIVATE "${CHILI_FRAMEWORK_LIB}")
if(MSVC)
  target_compile_options(chili_menu_entry_test PRIVATE /W4 /WX /permissive- /wd4324)
  target_compile_definitions(chili_menu_entry_test PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
if(EXISTS "${CHILI_MOD_TEST_DISC}/PSP_GAME/USRDIR/assets00.naz")
  add_test(NAME chili_menu_entry COMMAND chili_menu_entry_test "${CHILI_MOD_TEST_DISC}")
else()
  add_test(NAME chili_menu_entry COMMAND chili_menu_entry_test)
endif()

# The letter spacing of a replaced text font (host/font_advances.hpp): the file's grammar and its refusals, the
# tie to the sheet by the hash of the PNG file, the two glyph tables of a block made in the game's layout
# served with the file's advances. With the game's disc folder present the tables of the real todpsp_game.main.
add_executable(chili_font_advances_test font_advances_test.cpp)
target_include_directories(chili_font_advances_test PRIVATE "${REPO}/include" "${CMAKE_CURRENT_SOURCE_DIR}/../host")
target_link_libraries(chili_font_advances_test PRIVATE "${CHILI_FRAMEWORK_LIB}")
if(MSVC)
  target_compile_options(chili_font_advances_test PRIVATE /W4 /WX /permissive- /wd4324)
  target_compile_definitions(chili_font_advances_test PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
if(EXISTS "${CHILI_MOD_TEST_DISC}/PSP_GAME/USRDIR/assets00.naz")
  add_test(NAME chili_font_advances COMMAND chili_font_advances_test "${CMAKE_BINARY_DIR}/font-advances-scratch" "${CHILI_MOD_TEST_DISC}")
else()
  add_test(NAME chili_font_advances COMMAND chili_font_advances_test "${CMAKE_BINARY_DIR}/font-advances-scratch")
endif()
# naz.py and texture_psp.py (list, unpack, pack; texture to PNG and back).
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND AND EXISTS "${CHILI_MOD_TEST_DISC}/PSP_GAME/USRDIR/assets00.naz")
  add_test(NAME chili_mod_tools COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_mod_tools.py" "${CHILI_MOD_TEST_DISC}/PSP_GAME/USRDIR/assets00.naz")
elseif(Python3_FOUND)
  add_test(NAME chili_mod_tools COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_mod_tools.py")
endif()

# The disc as an .iso read in place (host/disc_image.hpp): a small image made by the test is read
# directly and through the guest's file calls, with a mod folder over it. With CHILI_TEST_ISO naming
# the real image and the disc folder present, every file of the two is compared.
add_executable(chili_disc_image_test disc_image_test.cpp ../host/file_runtime.cpp)
target_include_directories(chili_disc_image_test PRIVATE "${REPO}/include" "${CMAKE_CURRENT_SOURCE_DIR}/../host")
target_link_libraries(chili_disc_image_test PRIVATE "${CHILI_FRAMEWORK_LIB}")
if(MSVC)
  target_compile_options(chili_disc_image_test PRIVATE /W4 /wd4324)
  target_compile_definitions(chili_disc_image_test PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
set(CHILI_TEST_ISO "" CACHE FILEPATH "The .iso the disc folder was unpacked from, for the disc image test")
if(CHILI_TEST_ISO AND EXISTS "${CHILI_TEST_ISO}" AND EXISTS "${CHILI_MOD_TEST_DISC}/PSP_GAME/USRDIR/assets00.naz")
  add_test(NAME chili_disc_image COMMAND chili_disc_image_test "${CMAKE_BINARY_DIR}/disc-image-scratch" "${CHILI_TEST_ISO}" "${CHILI_MOD_TEST_DISC}")
else()
  add_test(NAME chili_disc_image COMMAND chili_disc_image_test "${CMAKE_BINARY_DIR}/disc-image-scratch")
endif()


# The on-screen button prompts: every picture of every style painted at scales 1, 2 and 4 (PNG sheets in the
# scratch folder, for eyes), the overlays of the game's seven textures and their composition, the overlay
# table the renderer reads, the style and binding rules, the controller with its test variables.
add_executable(chili_prompt_glyphs_test prompt_glyphs_test.cpp ../host/prompt_glyphs.cpp ../host/gpu/ge_texture_mods.cpp)
target_include_directories(chili_prompt_glyphs_test PRIVATE "${REPO}/include" "${CMAKE_CURRENT_SOURCE_DIR}/../host")
target_link_libraries(chili_prompt_glyphs_test PRIVATE xinput9_1_0 user32 ole32 windowscodecs d2d1 dwrite)
if(MSVC)
  target_compile_options(chili_prompt_glyphs_test PRIVATE /W4 /WX /permissive- /wd4324)
  target_compile_definitions(chili_prompt_glyphs_test PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_test(NAME chili_prompt_glyphs COMMAND chili_prompt_glyphs_test "${CMAKE_BINARY_DIR}/prompt-glyphs-scratch")

# The filter and level bias of a texture's sampler (host/gpu/ge_sampler_rule.hpp): the game's own textures and
# the button prompts' pictures keep theirs to the bit; a mod's picture is read from the level with one texel
# per screen pixel.
add_executable(chili_sampler_rule_test sampler_rule_test.cpp)
target_include_directories(chili_sampler_rule_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../host")
if(MSVC)
  target_compile_options(chili_sampler_rule_test PRIVATE /W4 /WX /permissive-)
  target_compile_definitions(chili_sampler_rule_test PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()
add_test(NAME chili_sampler_rule COMMAND chili_sampler_rule_test)

# Where the game's full-screen glow is among a frame's draws (host/gpu/ge_glow_blocks.hpp), which the renderer
# then draws at the PSP's size ([Rendering] Glow): made-up frames and the rule's edges; and, where the capture
# files are (profiles/chili/captures/ge, kept out of the repository), the draws of two mission frames and of
# two copies to the screen.
add_executable(chili_glow_blocks_test glow_blocks_test.cpp)
target_include_directories(chili_glow_blocks_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../host")
if(MSVC)
  target_compile_options(chili_glow_blocks_test PRIVATE /W4 /WX /permissive-)
  target_compile_definitions(chili_glow_blocks_test PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()
set(CHILI_GLOW_TEST_CAPTURES "${CMAKE_CURRENT_SOURCE_DIR}/../captures/ge" CACHE PATH "Folder with the captured mission frames read by the glow test")
if(EXISTS "${CHILI_GLOW_TEST_CAPTURES}/mission_9450_9453_trace.jsonl")
  add_test(NAME chili_glow_blocks COMMAND chili_glow_blocks_test "${CHILI_GLOW_TEST_CAPTURES}")
else()
  add_test(NAME chili_glow_blocks COMMAND chili_glow_blocks_test)
endif()

# The pictures of texture mods from their files to the renderer (host/gpu/ge_texture_mods.cpp) on made-up PNG
# files: the plan of the start, decoding ahead on two workers, the rows of a screen picture and a wrong guess
# decoded again, a store too small for all, a picture asked for before its turn, an unreadable file, the dump.
add_executable(chili_texture_store_test texture_store_test.cpp ../host/gpu/ge_texture_mods.cpp)
target_include_directories(chili_texture_store_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../host")
target_link_libraries(chili_texture_store_test PRIVATE ole32 windowscodecs)
if(MSVC)
  target_compile_options(chili_texture_store_test PRIVATE /W4 /WX /permissive-)
  target_compile_definitions(chili_texture_store_test PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_test(NAME chili_texture_store COMMAND chili_texture_store_test "${CMAKE_BINARY_DIR}/texture-store-scratch")

# The host page's drawing: the picture rectangle shared by the renderer and the mouse, the glyph table found in
# a made-up memory image, text set as the game sets its menu text (places from a capture of the game), the
# draw list the overlay pass takes.
add_executable(chili_page_draw_test page_draw_test.cpp)
target_include_directories(chili_page_draw_test PRIVATE "${REPO}/include" "${CMAKE_CURRENT_SOURCE_DIR}/../host")
if(MSVC)
  target_compile_options(chili_page_draw_test PRIVATE /W4 /WX /permissive- /wd4324)
  target_compile_definitions(chili_page_draw_test PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_test(NAME chili_page_draw COMMAND chili_page_draw_test)

# ChiliNative.ini kept line by line and changed one key at a time (host/ini_document.hpp), the twin of the
# launcher's IniDocument.cs: cases written by hand, the file written by read, change and swap under the gate
# shared with the launcher, and every save of the launcher's --every-value run replayed by the game's writer
# (tools/ini_fixture: the file the launcher began with, each save's changes, length and hashes of what it wrote).
add_executable(chili_ini_document_test ini_document_test.cpp)
target_include_directories(chili_ini_document_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../host")
if(MSVC)
  target_compile_options(chili_ini_document_test PRIVATE /W4 /WX /permissive-)
  target_compile_definitions(chili_ini_document_test PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_test(NAME chili_ini_document COMMAND chili_ini_document_test "${CMAKE_BINARY_DIR}/ini-document-scratch" "${CMAKE_CURRENT_SOURCE_DIR}/ini_fixture")

# The settings table of the game's own settings page (host/settings_table.hpp) against the launcher's source
# (launcher/src/Model.cs and Pages.cs: the same keys in the same order, the same defaults, values, words and
# ranges) and against the game's own readers (every value, every slider step and every default is taken).
add_executable(chili_settings_table_test settings_table_test.cpp)
target_include_directories(chili_settings_table_test PRIVATE "${REPO}/include" "${CMAKE_CURRENT_SOURCE_DIR}/../host")
target_link_libraries(chili_settings_table_test PRIVATE "${CHILI_FRAMEWORK_LIB}" xinput9_1_0 user32)
if(MSVC)
  target_compile_options(chili_settings_table_test PRIVATE /W4 /WX /permissive- /wd4324)
  target_compile_definitions(chili_settings_table_test PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_test(NAME chili_settings_table COMMAND chili_settings_table_test "${CMAKE_CURRENT_SOURCE_DIR}/../launcher/src")

# The settings while the game runs (host/settings_apply.hpp, PcInput::reconfigure): a value changed on the
# page is in force at once for every input key, with the pad finder and the pad in use kept; what waits for a
# restart; the locks; the rows over more than one key (Quality, Textures, HD pack); Defaults; the file written
# through host/ini_document.hpp; the test variable's steps.
add_executable(chili_settings_apply_test settings_apply_test.cpp)
target_include_directories(chili_settings_apply_test PRIVATE "${REPO}/include" "${CMAKE_CURRENT_SOURCE_DIR}/../host")
target_link_libraries(chili_settings_apply_test PRIVATE xinput9_1_0 user32)
if(MSVC)
  target_compile_options(chili_settings_apply_test PRIVATE /W4 /WX /permissive- /wd4324)
  target_compile_definitions(chili_settings_apply_test PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_test(NAME chili_settings_apply COMMAND chili_settings_apply_test "${CMAKE_BINARY_DIR}/settings-apply-scratch")

# The game's own settings page (host/settings_page.hpp) without the game: pad samples, the pointer and the
# clock in; the rows, the settings file and the draw list out (pages and rows, held buttons at the opening,
# wrap, repeat and whole-row scrolling, switches, choices and sliders, rows that wait for a restart, locks and
# their reasons in the pause menu, Defaults and its question, hover, click, drag, wheel, the legends as
# buttons, the text inside its frame).
add_executable(chili_settings_page_test settings_page_test.cpp)
target_include_directories(chili_settings_page_test PRIVATE "${REPO}/include" "${CMAKE_CURRENT_SOURCE_DIR}/../host")
target_link_libraries(chili_settings_page_test PRIVATE xinput9_1_0 user32)
if(MSVC)
  target_compile_options(chili_settings_page_test PRIVATE /W4 /WX /permissive- /wd4324)
  target_compile_definitions(chili_settings_page_test PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_test(NAME chili_settings_page COMMAND chili_settings_page_test "${CMAKE_BINARY_DIR}/settings-page-scratch")
