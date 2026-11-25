cmake_minimum_required(VERSION 3.20)
project(FindMyMouse LANGUAGES CXX RC)

if(NOT MSVC)
    message(FATAL_ERROR "FindMyMouse must be built with MSVC (Visual Studio 2019/2022) and the Windows 10 SDK 18362 or newer.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: the resulting exe has no VC++ redistributable dependency.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(FindMyMouse WIN32
    src/main.cpp
    src/FindMyMouse.cpp
    src/Settings.cpp
    src/FindMyMouse.rc
)

target_compile_definitions(FindMyMouse PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_compile_options(FindMyMouse PRIVATE /utf-8 /permissive- /EHsc /W4 /bigobj)
target_precompile_headers(FindMyMouse PRIVATE src/pch.h)
target_link_libraries(FindMyMouse PRIVATE windowsapp CoreMessaging shell32 user32 advapi32 ole32)
# The manifest is embedded through the .rc file.
target_link_options(FindMyMouse PRIVATE /MANIFEST:NO)
