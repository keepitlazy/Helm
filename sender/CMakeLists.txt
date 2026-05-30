cmake_minimum_required(VERSION 3.20)
project(stream_sender LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static MSVC runtime so the packaged exe needs no VC++ redistributable.
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(MSVC)
  add_compile_options(/W3 /permissive- /EHsc /utf-8)
  # C++/WinRT needs these
  add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
endif()

set(SENDER_SOURCES
  src/main.cpp
  src/capture/WgcCapture.cpp
  src/process/ColorConverter.cpp
  src/encode/MfEncoder.cpp
  src/transport/TcpServer.cpp
  src/transport/PipeServer.cpp
  src/input/InputInjector.cpp
)

# ---- Optional NVENC backend -------------------------------------------------
# Built only when the NVIDIA Video Codec SDK is found. On a machine without an
# NVIDIA GPU/SDK this is skipped and the QSV/Media-Foundation backend is used.
set(NVENC_SDK_DIR "" CACHE PATH "Path to NVIDIA Video Codec SDK root")
if(NVENC_SDK_DIR AND EXISTS "${NVENC_SDK_DIR}/Interface/nvEncodeAPI.h")
  message(STATUS "NVENC SDK found at ${NVENC_SDK_DIR} -- building NVENC backend")
  list(APPEND SENDER_SOURCES src/encode/NvencEncoder.cpp)
  add_compile_definitions(HAVE_NVENC=1)
else()
  message(STATUS "NVENC SDK not found -- NVENC backend disabled (QSV/MF only)")
endif()

add_executable(stream_sender ${SENDER_SOURCES})

target_include_directories(stream_sender PRIVATE src)
if(NVENC_SDK_DIR AND EXISTS "${NVENC_SDK_DIR}/Interface/nvEncodeAPI.h")
  target_include_directories(stream_sender PRIVATE "${NVENC_SDK_DIR}/Interface")
endif()

target_link_libraries(stream_sender PRIVATE
  d3d11 dxgi dxguid d3dcompiler
  windowsapp          # C++/WinRT (Windows Graphics Capture)
  mf mfplat mfreadwrite mfuuid wmcodecdspuuid  # Media Foundation encoder
  ws2_32              # Winsock TCP transport
  windowscodecs       # WIC (JPEG encode + debug BMP dump)
  ole32               # CreateStreamOnHGlobal / GetHGlobalFromStream
  user32              # SetProcessDpiAwarenessContext / MonitorFromPoint
)
