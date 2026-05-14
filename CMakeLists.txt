cmake_minimum_required(VERSION 3.16)

project(imageBufferCplus
    VERSION 0.1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

pkg_check_modules(LIBCAMERA REQUIRED IMPORTED_TARGET libcamera)

add_executable(image_buffer_capture
    main.cpp
    src/CaptureEngine.cpp
    src/RingBuffer.cpp
    src/TcpServer.cpp
)

target_include_directories(image_buffer_capture
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(image_buffer_capture
    PRIVATE
        PkgConfig::LIBCAMERA
        Threads::Threads
)

target_compile_options(image_buffer_capture
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)
