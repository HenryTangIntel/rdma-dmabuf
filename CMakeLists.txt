cmake_minimum_required(VERSION 3.10)
project(dmabuf C)

set(CMAKE_C_STANDARD 99)

# Ensure POSIX functions are available
add_definitions(-D_POSIX_C_SOURCE=200809L)

# Find required packages
find_package(PkgConfig REQUIRED)
pkg_check_modules(IBVERBS REQUIRED libibverbs)

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
    src/rdma_dmabuf_common.c
)

# Server executable
add_executable(server
    src/rdma_dmabuf_server.c
    ${SOURCES}
)

target_link_libraries(server
    PRIVATE
    ${IBVERBS_LIBRARIES}
    ${HLTHUNK_LIBRARIES}
)

# Client executable
add_executable(client
    src/rdma_dmabuf_client.c
    ${SOURCES}
)

target_link_libraries(client
    PRIVATE
    ${IBVERBS_LIBRARIES}
    ${HLTHUNK_LIBRARIES}
)

# Client executable
add_executable(hpu
    src/hpu.c
    ${SOURCES}
)

target_link_libraries(hpu
    PRIVATE

    ${IBVERBS_LIBRARIES}
    ${HLTHUNK_LIBRARIES}
)


# Client executable
add_executable(hpu-htod
    src/hpu-htod.c
    ${SOURCES}
)

target_link_libraries(hpu-htod
    PRIVATE

    ${IBVERBS_LIBRARIES}
    ${HLTHUNK_LIBRARIES}
)

# Custom commands to copy executables
add_custom_command(
    TARGET server
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
            $<TARGET_FILE:server>
            ${CMAKE_BINARY_DIR}/dmabuf_server
    COMMENT "Copying server to dmabuf_server"
)

add_custom_command(
    TARGET client
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
            $<TARGET_FILE:client>
            ${CMAKE_BINARY_DIR}/dmabuf_client
    COMMENT "Copying client to dmabuf_client"
)

# Installation
install(TARGETS server client
    DESTINATION bin
)
install(FILES
    ${CMAKE_BINARY_DIR}/dmabuf_server
    ${CMAKE_BINARY_DIR}/dmabuf_client
    DESTINATION bin
)
