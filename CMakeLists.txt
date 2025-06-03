cmake_minimum_required(VERSION 3.10)
project(dmabuf C)

set(CMAKE_C_STANDARD 99)

# Ensure POSIX functions are available
add_definitions(-D_POSIX_C_SOURCE=200809L)

set(CMAKE_VERBOSE_MAKEFILE ON)

# Find required packages
find_package(PkgConfig REQUIRED)
pkg_check_modules(IBVERBS REQUIRED libibverbs)

# Build hl-thunk libraries before any target is built
execute_process(
    COMMAND /opt/habanalabs/src/hl-thunk/build.sh
    WORKING_DIRECTORY /opt/habanalabs/src/hl-thunk
    RESULT_VARIABLE HLTHUNK_BUILD_RESULT
    OUTPUT_VARIABLE HLTHUNK_BUILD_OUTPUT
    ERROR_VARIABLE HLTHUNK_BUILD_ERROR
)

if(NOT ${HLTHUNK_BUILD_RESULT} EQUAL "0")
    message(FATAL_ERROR "Failed to build hl-thunk libraries. Error: ${HLTHUNK_BUILD_ERROR} Output: ${HLTHUNK_BUILD_OUTPUT}")
endif()

# Include directories
include_directories(
    ${IBVERBS_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    /opt/habanalabs/src/hl-thunk/include
    /opt/habanalabs/src/hl-thunk/include/uapi
    /opt/habanalabs/src/hl-thunk/src
    /opt/habanalabs/src/habanalabs/include/uapi
    /opt/habanalabs/src/hldk/include/specs/misc
    /opt/habanalabs/src/hl-thunk/include/specs/common
    /usr/include
    /usr/include/drm
)

# Link directories
link_directories(
    ${IBVERBS_LIBRARY_DIRS}
    /opt/habanalabs/src/hl-thunk/build/lib
    /usr/lib/x86_64-linux-gnu
)

# Glob hlthunk libraries
file(GLOB HLTHUNK_LIBRARIES "/opt/habanalabs/src/hl-thunk/build/lib/*.so" "/opt/habanalabs/src/hl-thunk/build/lib/*.a")

# Source files
set(SOURCES
    src/rdma_common.c
)

# Server executable
add_executable(rdma_server
    src/rdma_server.c
    ${SOURCES}
)

target_link_libraries(rdma_server
    PRIVATE
    ${IBVERBS_LIBRARIES}
    ${HLTHUNK_LIBRARIES}
)

# Client executable
add_executable(rdma_client
    src/rdma_client.c
    ${SOURCES}
)

target_link_libraries(rdma_client
    PRIVATE
    ${IBVERBS_LIBRARIES}
    ${HLTHUNK_LIBRARIES}
)
