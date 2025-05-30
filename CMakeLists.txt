cmake_minimum_required(VERSION 3.10)
project(RDMADmabuf LANGUAGES CXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Ensure POSIX functions are available
add_definitions(-D_POSIX_C_SOURCE=200809L)

# Find required packages
find_package(PkgConfig REQUIRED)
pkg_check_modules(IBVERBS REQUIRED libibverbs)

# Include directories
include_directories(
    ${IBVERBS_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    /workspace/hl-thunk/include
    /workspace/hl-thunk/include/uapi
    /workspace/hl-thunk/src
    /workspace/habanalabs/include/uapi
    /workspace/hldk/include/specs/misc
    /workspace/hl-thunk/include/specs/common
    /usr/include
    /usr/include/drm
)

# Link directories
link_directories(
    ${IBVERBS_LIBRARY_DIRS}
    /workspace/hl-thunk/build/lib
    /usr/lib/x86_64-linux-gnu
)

# Glob hlthunk libraries
file(GLOB HLTHUNK_LIBRARIES "/workspace/hl-thunk/build/lib/*.so" "/workspace/hl-thunk/build/lib/*.a")

# Source files
set(SOURCES
    src/hpuverbs.cpp
)

# Server executable
add_executable(rdma_dmabuf_server
    src/server.cpp
    ${SOURCES}
)

target_link_libraries(rdma_dmabuf_server
    PRIVATE
    ${IBVERBS_LIBRARIES}
    ${HLTHUNK_LIBRARIES}
)

# Client executable
add_executable(rdma_dmabuf_client
    src/client.cpp
    ${SOURCES}
)

target_link_libraries(rdma_dmabuf_client
    PRIVATE
    ${IBVERBS_LIBRARIES}
    ${HLTHUNK_LIBRARIES}
)

# Installation
install(TARGETS rdma_dmabuf_server rdma_dmabuf_client
    DESTINATION bin
)