cmake_minimum_required(VERSION 3.10)
project(rdma-tutorial C)

set(CMAKE_C_STANDARD 99)

# Find required packages
find_package(PkgConfig REQUIRED)
pkg_check_modules(IBVERBS REQUIRED libibverbs)

include_directories(
    ${IBVERBS_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}
    /workspace/hl-thunk/include
    /workspace/hl-thunk/include/uapi
    /workspace/hl-thunk/src
    /workspace/habanalabs/include/uapi
    /workspace/hldk/include/specs/misc
    /workspace/hl-thunk/include/specs/common
    /usr/include
    /usr/include/drm
    )

link_directories(
    ${IBVERBS_LIBRARY_DIRS}
    /workspace/hl-thunk/build/lib
    /usr/lib/x86_64-linux-gnu
)

file(GLOB HLTHUNK_LIBRARIES "/workspace/hl-thunk/build/lib/*.so" "/workspace/hl-thunk/build/lib/*.a")
file(GLOB HLTHUNK_TEST_LIBRARIES "/workspace/hl-thunk/tests/build/*.so" "/workspace/hl-thunk/test/build/*.a") 

add_executable(client client.c rdma_common.c)
target_link_libraries(client ${IBVERBS_LIBRARIES} ${HLTHUNK_LIBRARIES})

add_executable(server server.c rdma_common.c)
target_link_libraries(server ${IBVERBS_LIBRARIES} ${HLTHUNK_LIBRARIES})


find_library(CMOCKA_LIBRARY NAMES cmocka libcmocka)
if (NOT CMOCKA_LIBRARY)
    message(FATAL_ERROR "CMocka library not found")
endif()

#add_executable(hpu hpu.c)
#target_link_libraries(hpu PRIVATE hl-thunk-tests ${CMOCKA_LIBRARY})

add_executable(hpu hpu.c)
target_link_libraries(hpu PRIVATE ${IBVERBS_LIBRARIES} ${HLTHUNK_LIBRARIES} ${HLTHUNK_TEST_LIBRARIES})


add_executable(hpu_dma_buf hpu_dma_buf.c)
target_link_libraries(hpu_dma_buf PRIVATE ${IBVERBS_LIBRARIES} ${HLTHUNK_LIBRARIES} ${HLTHUNK_TEST_LIBRARIES})

add_executable(hpu_dmabuf_verbs hpu_dmabuf_verbs.c)
target_link_libraries(hpu_dmabuf_verbs PRIVATE ${IBVERBS_LIBRARIES} ${HLTHUNK_LIBRARIES} ${HLTHUNK_TEST_LIBRARIES})

add_executable(clean_dmabufverbs clean_dmabufverbs.c)
target_link_libraries(clean_dmabufverbs PRIVATE ${IBVERBS_LIBRARIES} ${HLTHUNK_LIBRARIES} ${HLTHUNK_TEST_LIBRARIES})






add_custom_command(TARGET client server 
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        ${CMAKE_CURRENT_BINARY_DIR}/client
        ${CMAKE_CURRENT_BINARY_DIR}/server
        ${CMAKE_CURRENT_BINARY_DIR}/..
    COMMENT "Copying client, server to parent directory"
)
