cmake_minimum_required(VERSION 3.20)
project(msitool LANGUAGES CXX)

# MSVC / Windows SDK only: Msi.Lib (Windows Installer) and Cabinet.Lib (FDI).
if(NOT MSVC)
    message(FATAL_ERROR "msitool requires MSVC (cl.exe) and the Windows SDK")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(msitool
    src/main.cpp
    src/util.cpp
    src/msidb.cpp
    src/cab.cpp
    src/extract.cpp
    src/pack.cpp
)

target_include_directories(msitool PRIVATE src)
target_compile_definitions(msitool PRIVATE
    UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS
)
target_compile_options(msitool PRIVATE /utf-8 /W4 /EHsc /permissive-)

target_link_libraries(msitool PRIVATE
    msi        # Msi.Lib      : MsiOpenDatabaseW / MsiDatabaseOpenViewW / ...
    cabinet    # Cabinet.Lib  : FDI (FDICopy) + FCI (FCIAddFile)
    shell32    # CommandLineToArgvW
    ole32      # CoCreateGuid / StringFromGUID2
)

# Build with:  cmake -S . -B build -G "Visual Studio 17 2022" -A x64
#              cmake --build build --config Release
