cmake_minimum_required(VERSION 3.22.1)
project(untitled)

set(CMAKE_CXX_STANDARD 20)


# Manually locate liburing (since it lacks CMake config)
find_path(LIBURING_INCLUDE_DIR liburing.h REQUIRED)
find_library(LIBURING_LIBRARY uring REQUIRED)

add_executable(io_uring_test main.cpp)
target_include_directories(io_uring_test PRIVATE ${LIBURING_INCLUDE_DIR})
target_link_libraries(io_uring_test PRIVATE ${LIBURING_LIBRARY} -fcoroutines)
