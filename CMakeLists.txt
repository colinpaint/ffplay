#
cmake_minimum_required (VERSION 3.18)
set (CMAKE_CXX_STANDARD 20)
set (CMAKE_CXX_STANDARD_REQUIRED ON)
set (CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "limited configs" FORCE)
#
if (CMAKE_HOST_SYSTEM_NAME STREQUAL Windows)
  set (BUILD_MSVC_COMPILE_OPTIONS "/arch:AVX" CACHE STRING "msvc compile options")
  option (USE_AVX512 "use avx512" OFF)
  option (USE_AVX2 "use avx2" OFF)
elseif (CMAKE_HOST_SYSTEM_NAME STREQUAL Linux)
  set (BUILD_LINUX_COMPILE_OPTIONS "-Ofast" CACHE STRING "gcc compile options")
endif()
#


project (ffplay VERSION 1.0.0)
  if (CMAKE_HOST_SYSTEM_NAME STREQUAL Windows)
    add_executable (${PROJECT_NAME} captureAudio/capture.cpp)

    if (CMAKE_HOST_SYSTEM_NAME STREQUAL Windows)
      target_include_directories (${PROJECT_NAME} PUBLIC ../include/ffmpeg)
      target_link_directories (${PROJECT_NAME} PUBLIC ../lib/windows64)
      target_link_libraries (${PROJECT_NAME} PUBLIC avdevice avfilter avformat avcodec avutil
                                                    swresample swscale)

    else()
      find_package (PkgConfig REQUIRED)
      pkg_check_modules (FFMPEG REQUIRED IMPORTED_TARGET
                         libavdevice libavfilter libavformat libavcodec libavutil
                         libswresample libswscale)

      target_include_directories (${PROJECT_NAME} PUBLIC common PkgConfig::FFMPEG)
      target_link_libraries (${PROJECT_NAME} PUBLIC common PkgConfig::FFMPEG)

    endif()
  endif()
