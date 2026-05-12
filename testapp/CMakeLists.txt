cmake_minimum_required(VERSION 3.20)
project(testapp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include_directories(
  src
  ${CMAKE_SOURCE_DIR}/include
)

set(TESTAPP_SOURCES
  src/testapp.cpp
  src/JoyConDecoder.cpp
  src/DsuServer.cpp
)

add_executable(testapp ${TESTAPP_SOURCES})

add_executable(dsu_pointer_tester WIN32
  src/dsu_pointer_tester.cpp
)

target_link_directories(testapp PRIVATE ${CMAKE_SOURCE_DIR}/lib)
target_link_libraries(testapp
    PRIVATE
        setupapi
        hid
        ViGEmClient
        windowsapp
        ws2_32
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
