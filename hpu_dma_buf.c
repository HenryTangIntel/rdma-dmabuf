#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <infiniband/verbs.h>
#include "hlthunk.h"

#define MEMORY_SIZE 4096 // 4 KB

int main(void)
{
    const char *busid = "0000:4d:00.0";

    // Step 1: Open Gaudi device
    int fd = hlthunk_open(HLTHUNK_DEVICE_DONT_CARE, busid);
    if (fd < 0) {
        fprintf(stderr, "Failed to open device: %s\n", strerror(errno));
        return -1;
    }

    // Step 2: Allocate device memory using 5-arg version
    uint64_t page_size = 4096;
    bool shared = false;
    bool NOT_CONTIGUOUS = 0; // Use non-contiguous memory allocation

    uint64_t device_handle = hlthunk_device_memory_alloc(fd, MEMORY_SIZE, page_size, NOT_CONTIGUOUS, shared);
    if (device_handle == 0) {
        fprintf(stderr, "Failed to allocate device memory\n");
        hlthunk_close(fd);
        return -1;
    }

    printf("[HLTHUNK] Allocated device memory, handle: 0x%llx\n", (unsigned long long)device_handle);


    // Map the device memory to a virtual address space
    uint64_t mapped_addr = hlthunk_device_memory_map(fd, device_handle, 0);
    if (mapped_addr == 0) {
        fprintf(stderr, "Failed to map device memory\n");
        hlthunk_device_memory_free(fd, device_handle);
        hlthunk_close(fd);
        return -1;
    }

    // Step 3: Export to DMA-BUF with correct 4-arg version
    uint32_t export_flags = 0;
    int dma_buf_fd = hlthunk_device_memory_export_dmabuf_fd(fd, device_handle, MEMORY_SIZE, export_flags);
    if (dma_buf_fd < 0) {
        fprintf(stderr, "Failed to export device memory to DMA-BUF\n");
        hlthunk_device_memory_free(fd, device_handle);
        hlthunk_close(fd);
        return -1;
    }

    printf("[DMA-BUF] Exported DMA-BUF FD: %d\n", dma_buf_fd);

    // Step 4: Register DMA-BUF with RDMA stack
    struct ibv_device **dev_list = ibv_get_device_list(NULL);
    if (!dev_list) {
        perror("Failed to get IB devices");
        goto cleanup;
    }

    struct ibv_context *ctx = ibv_open_device(dev_list[0]);
    if (!ctx) {
        perror("Failed to open IB device");
        ibv_free_device_list(dev_list);
        goto cleanup;
    }

    struct ibv_pd *pd = ibv_alloc_pd(ctx);
    if (!pd) {
        perror("Failed to alloc PD");
        ibv_close_device(ctx);
        ibv_free_device_list(dev_list);
        goto cleanup;
    }

    uint64_t iova = 0; // NIC-assigned virtual address
    struct ibv_mr *mr = ibv_reg_dmabuf_mr(
        pd,
        0, // offset into dma-buf
        MEMORY_SIZE,
        iova,
        dma_buf_fd,
        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE
    );

    if (!mr) {
        perror("Failed to register MR from DMA-BUF");
    } else {
        printf("[RDMA] Memory Region registered: lkey=0x%x, rkey=0x%x\n", mr->lkey, mr->rkey);
        ibv_dereg_mr(mr);
    }

    ibv_dealloc_pd(pd);
    ibv_close_device(ctx);
    ibv_free_device_list(dev_list);

cleanup:
    close(dma_buf_fd);
    hlthunk_device_memory_free(fd, device_handle);
    hlthunk_close(fd);
    return 0;
}
