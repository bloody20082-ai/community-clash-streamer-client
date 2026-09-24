cmake_minimum_required(VERSION 3.24)
project(CommunityClashStreamer VERSION 3.4.0 LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "Community Clash Streamer Client V3 ist nur fuer Windows x64.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(LiveKit CONFIG REQUIRED)

add_executable(CommunityClashStreamer WIN32
  src/main.cpp
  src/http_client.cpp
  src/desktop_capture.cpp
  src/loopback_audio.cpp
  src/app.rc
)

target_include_directories(CommunityClashStreamer PRIVATE src)

target_compile_definitions(CommunityClashStreamer PRIVATE
  UNICODE
  _UNICODE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
)

target_compile_options(CommunityClashStreamer PRIVATE
  $<$<CXX_COMPILER_ID:MSVC>:/W4 /EHsc /O2 /utf-8>
)

target_link_libraries(CommunityClashStreamer PRIVATE
  LiveKit::livekit
  d3d11
  dxgi
  winhttp
  ole32
  uuid
  avrt
  mmdevapi
  dwmapi
  comctl32
  gdiplus
  windowsapp
)

add_custom_command(TARGET CommunityClashStreamer POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_RUNTIME_DLLS:CommunityClashStreamer>
          $<TARGET_FILE_DIR:CommunityClashStreamer>
  COMMAND_EXPAND_LISTS
)

install(TARGETS CommunityClashStreamer RUNTIME DESTINATION .)
