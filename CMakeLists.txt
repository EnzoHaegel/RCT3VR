cmake_minimum_required(VERSION 3.20)
project(rct3vr CXX RC)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "RCT3.exe is 32-bit: configure with an x86 toolchain (use build.cmd)")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

add_library(rct3vr SHARED
  src/main.cpp
  src/log.cpp
  src/config.cpp
  src/ctab.cpp
  src/image.cpp
  src/hooks.cpp
  src/capture.cpp
  src/stereo.cpp
  src/probe.cpp
  src/keys.cpp
  src/rct3vr.rc
)
set_target_properties(rct3vr PROPERTIES OUTPUT_NAME d3d9)
target_include_directories(rct3vr PRIVATE src third_party/openxr-sdk/include)
target_compile_definitions(rct3vr PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(rct3vr PRIVATE /W3 /utf-8)
target_link_libraries(rct3vr PRIVATE windowscodecs ole32 d3d11 dxgi dxguid)
target_link_options(rct3vr PRIVATE /SAFESEH:NO)
target_sources(rct3vr PRIVATE src/xr.cpp)

# Standalone headset test of the OpenXR output (test pattern, no game needed).
add_executable(xrtest tools/xrtest.cpp src/xr.cpp src/log.cpp src/config.cpp)
target_include_directories(xrtest PRIVATE src third_party/openxr-sdk/include)
target_compile_definitions(xrtest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(xrtest PRIVATE /W3 /utf-8)
target_link_libraries(xrtest PRIVATE d3d11 dxgi)

# Checks D3D9 <-> D3D11 texture sharing on this machine.
add_executable(sharetest tools/sharetest.cpp)
target_link_libraries(sharetest PRIVATE d3d11 dxgi)
add_executable(tracetest tools/tracetest.cpp)
target_link_options(tracetest PRIVATE /SAFESEH:NO)
