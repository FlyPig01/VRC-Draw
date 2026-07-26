cmake_minimum_required(VERSION 3.28)

project(VRC_Draw VERSION 0.2.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR "VRC-Draw currently supports Windows only.")
endif()

include(FetchContent)

FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.9b
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(imgui)

add_library(vrcdraw_imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
)

target_include_directories(vrcdraw_imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)

target_compile_definitions(vrcdraw_imgui PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)

add_library(vrcdraw_core STATIC
    src/AppPaths.cpp
    src/ExecutionPlan.cpp
    src/ImageProcessor.cpp
    src/LineArtExporter.cpp
    src/LineArtRenderer.cpp
    src/MouseInput.cpp
    src/PathMath.cpp
    src/PathOptimizer.cpp
    src/Settings.cpp
    src/VectorPath.cpp
)

target_include_directories(vrcdraw_core PUBLIC src)
target_compile_definitions(vrcdraw_core PUBLIC
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)
target_link_libraries(vrcdraw_core PUBLIC windowscodecs ole32)

add_executable(VRC-Draw WIN32
    src/DrawingEngine.cpp
    src/Main.cpp
    src/OverlayWindow.cpp
    resources/VRC-Draw.rc
)

target_include_directories(VRC-Draw PRIVATE src resources)
target_compile_definitions(VRC-Draw PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    VRC_DRAW_VERSION="${PROJECT_VERSION}"
)

target_link_libraries(VRC-Draw PRIVATE
    vrcdraw_core
    vrcdraw_imgui
    d3d11
    dxgi
    d3dcompiler
    dwmapi
    comdlg32
    shell32
    ole32
    windowscodecs
)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_custom_target(generate-icon
        COMMAND
            ${Python3_EXECUTABLE}
            "${CMAKE_SOURCE_DIR}/tools/generate_icon.py"
            "${CMAKE_SOURCE_DIR}/assets/vrc-draw.svg"
            "${CMAKE_SOURCE_DIR}/resources/VRC-Draw.ico"
            --individual-dir
            "${CMAKE_SOURCE_DIR}/resources/icons"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Generating combined and single-resolution Windows icons from assets/vrc-draw.svg"
        VERBATIM
    )
endif()

if(MSVC)
    target_compile_options(vrcdraw_core PRIVATE /W4 /permissive- /utf-8)
    target_compile_options(VRC-Draw PRIVATE /W4 /permissive- /utf-8)
    target_compile_options(vrcdraw_imgui PRIVATE /W3 /utf-8)
else()
    target_compile_options(vrcdraw_core PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(VRC-Draw PRIVATE -Wall -Wextra -Wpedantic)
    target_link_options(VRC-Draw PRIVATE -static -static-libgcc -static-libstdc++)
endif()

set_target_properties(VRC-Draw PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/portable/VRC-Draw"
)

foreach(configuration Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${configuration}" configuration_upper)
    set_target_properties(VRC-Draw PROPERTIES
        "RUNTIME_OUTPUT_DIRECTORY_${configuration_upper}" "${CMAKE_BINARY_DIR}/portable/VRC-Draw"
    )
endforeach()

include(CTest)
if(BUILD_TESTING)
    add_executable(vrcdraw_tests
        tests/CoreTests.cpp
    )
    target_link_libraries(vrcdraw_tests PRIVATE vrcdraw_core)
    target_compile_definitions(vrcdraw_tests PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
        VRC_DRAW_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    )
    if(NOT MSVC)
        target_compile_options(vrcdraw_tests PRIVATE -UNDEBUG)
        target_link_options(vrcdraw_tests PRIVATE -static -static-libgcc -static-libstdc++)
    else()
        target_compile_options(vrcdraw_tests PRIVATE /UNDEBUG)
    endif()
    add_test(NAME vrcdraw_core_tests COMMAND vrcdraw_tests)
endif()
