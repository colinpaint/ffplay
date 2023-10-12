#
cmake_minimum_required (VERSION 3.18)
set (CMAKE_CXX_STANDARD 20)
set (CMAKE_CXX_STANDARD_REQUIRED ON)
set (CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "limited configs" FORCE)
#
if (CMAKE_HOST_SYSTEM_NAME STREQUAL Windows)
  set (BUILD_MSVC_COMPILE_OPTIONS "/arch:AVX" CACHE STRING "msvc compile options")
  option (USE_AVX512 "use avx512" ON)
  option (USE_AVX2 "use avx2" OFF)
elseif (CMAKE_HOST_SYSTEM_NAME STREQUAL Linux)
  set (BUILD_LINUX_COMPILE_OPTIONS "-Ofast" CACHE STRING "gcc compile options")
endif()
#

set (SDL_TEST OFF CACHE BOOL "" FORCE)
set (SDL_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory (SDL2)

project (ffplay VERSION 1.0.0)
  add_executable (${PROJECT_NAME} config.h config_components.h
                                  ffplay.c opt_common.h opt_common.c cmdutils.h cmdutils.c)

  message (STATUS "using ${CMAKE_HOST_SYSTEM_NAME}")
  if (CMAKE_HOST_SYSTEM_NAME STREQUAL Windows)
    target_compile_options (${PROJECT_NAME} PUBLIC /W4
                                                   /fp:fast
                                                   ${BUILD_MSVC_COMPILE_OPTIONS})
    if (USE_AVX512)
      message (STATUS "windows built with AVX X2 AVX512")
      target_compile_options (${PROJECT_NAME} PUBLIC /arch:AVX512)
    elseif (USE_AVX2)
      message (STATUS "windows built with AVX AVX2")
      target_compile_options (${PROJECT_NAME} PUBLIC /arch:AVX2)
    else()
      message (STATUS "windows built with AVX")
      target_compile_options (${PROJECT_NAME} PUBLIC /arch:AVX)
    endif()

    message (STATUS "using prebuilt windows64 ffmpeg,SDL lib/windows64")
    # FFmpeg
    target_include_directories (${PROJECT_NAME} PUBLIC include/ffmpeg)
    target_link_directories (${PROJECT_NAME} PUBLIC lib/windows64)
    target_link_libraries (${PROJECT_NAME} PUBLIC
                           avdevice avfilter avformat avcodec avutil swresample swscale)
    # SDL2
    target_link_libraries (${PROJECT_NAME} PUBLIC SDL2)

  else (CMAKE_HOST_SYSTEM_NAME STREQUAL Linux)
    target_compile_definitions (${PROJECT_NAME} PUBLIC _LARGEFILE64_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options (${PROJECT_NAME} PUBLIC -Wall
                                                   -Wextra
                                                   -Wno-missing-field-initializers
                                                   -Wno-format-security
                                                   -Wno-format-overflow
                                                   -Wno-unused-parameter
                                                   -pedantic
                                                   -flax-vector-conversions
                                                   -ftree-vectorize
                                                   -fno-exceptions
                                                   -march=native
                                                   -g
                                                   ${BUILD_LINUX_COMPILE_OPTIONS})
    # FFmpeg
    message (STATUS "using linux installed ffmpeg library")
    find_package (PkgConfig REQUIRED)
    pkg_check_modules (FFMPEG REQUIRED IMPORTED_TARGET
                       libavdevice libavfilter libavformat libavcodec libavutil libswresample libswscale)
    target_include_directories (${PROJECT_NAME} PUBLIC . ../FFmpeg)
    target_link_libraries (${PROJECT_NAME} PUBLIC PkgConfig::FFMPEG)

    # SDL2
    message (STATUS "using local copy of linux SDL include")
    target_include_directories (${PROJECT_NAME} PUBLIC include/SDL2/include)
    if (CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
      message (STATUS "using prebuilt linux ARM SDL lib/arm64 library")
      target_link_directories (${PROJECT_NAME} PUBLIC lib/arm64)
      target_link_libraries (${PROJECT_NAME} PUBLIC libSDL2.a libSDL2main.a dl)
    else()
      message (STATUS "using prebuilt linux 64 SDL lib/linux64 library")
      target_link_directories (${PROJECT_NAME} PUBLIC lib/linux64)
      target_link_libraries (${PROJECT_NAME} PUBLIC libSDL2.a)
    endif()

  endif()
