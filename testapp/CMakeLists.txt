cmake_minimum_required(VERSION 3.20)
project(testapp LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# ImGui sources
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/imgui)
set(IMGUI_SOURCES
  ${IMGUI_DIR}/imgui.cpp
  ${IMGUI_DIR}/imgui_draw.cpp
  ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
  ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
)

include_directories(
  src
  ${CMAKE_SOURCE_DIR}/include
  ${IMGUI_DIR}
  ${IMGUI_DIR}/backends
)

# Final version — built-in procon2tool init (WinUSB vendor interface)
set(TESTAPP_SOURCES
  src/testapp_final.cpp
  src/JoyConDecoder.cpp
  src/DsuServer.cpp
  ${IMGUI_SOURCES}
)

add_executable(testapp WIN32 ${TESTAPP_SOURCES})

# Previous experimental version (src/testapp.cpp), preserved as a separate executable
add_executable(testapp_old WIN32
  src/testapp.cpp
  src/JoyConDecoder.cpp
  src/DsuServer.cpp
  ${IMGUI_SOURCES}
)


add_executable(dsu_pointer_tester WIN32
  src/dsu_pointer_tester.cpp
)

add_executable(probe
  src/probe.cpp
)
target_link_directories(probe PRIVATE ${CMAKE_SOURCE_DIR}/lib)
target_link_libraries(probe PRIVATE setupapi hid winmm user32)
if(MSVC)
  target_compile_options(probe PRIVATE /W3)
endif()

target_link_directories(testapp PRIVATE ${CMAKE_SOURCE_DIR}/lib)
target_link_libraries(testapp
    PRIVATE
        setupapi
        hid
        winusb
        shell32
        winmm
        ViGEmClient
        windowsapp
        ws2_32
        d3d11
        dxgi
        d3dcompiler
)
target_link_directories(testapp_old PRIVATE ${CMAKE_SOURCE_DIR}/lib)
target_link_libraries(testapp_old
    PRIVATE
        setupapi
        hid
        winusb
        ViGEmClient
        windowsapp
        ws2_32
        d3d11
        dxgi
        d3dcompiler
)
target_link_libraries(dsu_pointer_tester
    PRIVATE
        ws2_32
        user32
        gdi32
)
if(MSVC)
  target_compile_options(testapp PRIVATE /W3 /permissive-)
  target_compile_options(dsu_pointer_tester PRIVATE /W3 /permissive-)
else()
  target_compile_options(testapp PRIVATE -Wall -Wextra -pedantic)
  target_compile_options(dsu_pointer_tester PRIVATE -Wall -Wextra -pedantic)
endif()
