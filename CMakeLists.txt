cmake_minimum_required(VERSION 3.20)
project(msitool LANGUAGES CXX)

# MSVC / Windows SDK only: Msi.Lib (Windows Installer) and Cabinet.Lib (FDI).
if(NOT MSVC)
    message(FATAL_ERROR "msitool requires MSVC (cl.exe) and the Windows SDK")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# WIN32: the GUI is the default entry (no args / `gui`). CLI mode attaches
# to the parent console, so one binary serves both use cases.
add_executable(msitool WIN32
    src/main.cpp
    src/util.cpp
    src/msidb.cpp
    src/cab.cpp
    src/extract.cpp
    src/pack.cpp
    src/ui_draw.cpp
    src/ui_widgets.cpp
    src/ui_app.cpp
)

target_include_directories(msitool PRIVATE src)
target_compile_definitions(msitool PRIVATE
    UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS
)
target_compile_options(msitool PRIVATE /utf-8 /W4 /EHsc /permissive-)

target_link_libraries(msitool PRIVATE
    msi        # Msi.Lib      : MsiOpenDatabaseW / MsiDatabaseOpenViewW / ...
    cabinet    # Cabinet.Lib  : FDI (FDICopy) + FCI (FCIAddFile)
    shell32    # CommandLineToArgvW / IFileDialog / ShellExecuteW
    ole32      # CoCreateGuid / OleInitialize / COM
    user32     # owner-drawn HWNDs, message loop, DPI
    gdi32      # all Fluent painting (double buffered GDI)
    advapi32   # registry (theme / recent file settings)
    comdlg32   # COMDLG_FILTERSPEC for the file pickers
    uuid       # CLSID_FileOpenDialog / IID_IFileDialog
)

# Build with:  cmake -S . -B build -G "Visual Studio 17 2022" -A x64
#              cmake --build build --config Release
