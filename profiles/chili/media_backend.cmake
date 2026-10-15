# FFmpeg 7.1 (LGPL 2.1 or later; headers, import libraries and DLLs) and the decoder around it
# (media/vcs_media_decoder.*, from upstream PSPRecomp's VCS profile, MIT). See third_party/ffmpeg/README.md.
get_filename_component(CHILI_FFMPEG "${CMAKE_CURRENT_LIST_DIR}/third_party/ffmpeg" ABSOLUTE)
get_filename_component(CHILI_MEDIA_HOST "${CMAKE_CURRENT_LIST_DIR}/media" ABSOLUTE)
if(NOT TARGET chili_media_backend)
    add_library(chili_media_backend STATIC "${CHILI_MEDIA_HOST}/vcs_media_decoder.cpp")
    target_include_directories(chili_media_backend PRIVATE "${CHILI_MEDIA_HOST}" "${CHILI_FFMPEG}/include")
    foreach(CHILI_CODEC_LIB avcodec avformat avutil swresample swscale)
        target_link_libraries(chili_media_backend PUBLIC "${CHILI_FFMPEG}/lib/${CHILI_CODEC_LIB}.lib")
    endforeach()
endif()
# The FFmpeg DLLs go beside the executables. Targets that share an output
# directory share one copy step: a post-build copy per target raced under Ninja
# when two of them linked at the same time.
function(chili_copy_media_runtime target)
    get_target_property(directory ${target} RUNTIME_OUTPUT_DIRECTORY)
    if(NOT directory)
        set(directory "${CMAKE_CURRENT_BINARY_DIR}")
        if(CMAKE_CONFIGURATION_TYPES)
            string(APPEND directory "/$<CONFIG>")
        endif()
    endif()
    string(MD5 key "${directory}")
    set(copy_target "chili_media_runtime_${key}")
    if(NOT TARGET ${copy_target})
        if(CMAKE_VERSION VERSION_LESS 3.26)
            set(copy_directory copy_directory)
        else()
            set(copy_directory copy_directory_if_different)
        endif()
        add_custom_target(${copy_target}
            COMMAND ${CMAKE_COMMAND} -E make_directory "${directory}"
            COMMAND ${CMAKE_COMMAND} -E ${copy_directory} "${CHILI_FFMPEG}/bin" "${directory}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CHILI_FFMPEG}/COPYING.LGPLv2.1" "${directory}/COPYING.FFmpeg.LGPLv2.1"
            VERBATIM)
    endif()
    add_dependencies(${target} ${copy_target})
endfunction()
