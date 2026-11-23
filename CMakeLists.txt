cmake_minimum_required(VERSION 3.20)
project(ssd_city LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(LOCAL_IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../cs2-internal/build/_deps/imgui-src")
if(EXISTS "${LOCAL_IMGUI_DIR}/imgui.h")
    set(IMGUI_DIR "${LOCAL_IMGUI_DIR}")
else()
    include(FetchContent)
    FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG v1.91.5
    )
    FetchContent_MakeAvailable(imgui)
    set(IMGUI_DIR "${imgui_SOURCE_DIR}")
endif()

add_library(imgui_dx11 STATIC
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_win32.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_dx11.cpp"
)
target_include_directories(imgui_dx11 PUBLIC "${IMGUI_DIR}" "${IMGUI_DIR}/backends")
target_compile_definitions(imgui_dx11 PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

add_executable(ssd_city WIN32
    src/main.cpp
    src/scanner.cpp
    src/city_layout.cpp
    src/city_renderer.cpp
    src/app.manifest
)

target_include_directories(ssd_city PRIVATE src)

if(MSVC)
    target_compile_options(imgui_dx11 PRIVATE /W0 /utf-8 /O2 /EHsc)
    target_compile_options(ssd_city PRIVATE /W4 /utf-8 /O2 /EHsc)
    set_property(TARGET imgui_dx11 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set_property(TARGET ssd_city PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    target_compile_options(ssd_city PRIVATE -Wall -Wextra -O2)
endif()

target_compile_definitions(ssd_city PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

target_link_libraries(ssd_city PRIVATE
    imgui_dx11
    d3d11
    dxgi
    d3dcompiler
    user32
    gdi32
    shell32
    ole32
    uuid
    shlwapi
)
