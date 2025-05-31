#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include "hlthunk.h"

int main() {
    int fd = -1;
    void *host_buffer = NULL;
    uint64_t device_va = 0;
    size_t buffer_size = 4096;
    
    printf("Gaudi-CPU Memory Sharing Example\n");
    printf("================================\n\n");
    
    // Open Gaudi device
    fd = hlthunk_open(HLTHUNK_DEVICE_DONT_CARE, NULL);
    if (fd < 0) {
        fprintf(stderr, "Failed to open Gaudi device: %s\n", strerror(errno));
        return 1;
    }
    
    printf("✓ Opened Gaudi device (fd=%d)\n", fd);
    
    // Method 1: Host Memory Mapped to Gaudi (CPU can access)
    printf("\nMethod 1: Host Memory Mapped to Gaudi\n");
    printf("--------------------------------------\n");
    
    // Allocate host memory
    host_buffer = aligned_alloc(4096, buffer_size);
    if (!host_buffer) {
        fprintf(stderr, "Failed to allocate host memory\n");
        hlthunk_close(fd);
        return 1;
    }
    
    printf("✓ Allocated host memory at %p\n", host_buffer);
    
    // Initialize with data
    int *data = (int *)host_buffer;
    for (int i = 0; i < 10; i++) {
        data[i] = i * 100;
    }
    printf("✓ CPU wrote data: %d %d %d %d %d ...\n", 
           data[0], data[1], data[2], data[3], data[4]);
    
    // Map to Gaudi
    device_va = hlthunk_host_memory_map(fd, host_buffer, 0, buffer_size);
    if (device_va == 0) {
        fprintf(stderr, "Failed to map host memory to Gaudi\n");
        free(host_buffer);
        hlthunk_close(fd);
        return 1;
    }
    
    printf("✓ Mapped to Gaudi at device VA: 0x%lx\n", device_va);
    printf("\nNow both CPU and Gaudi can access this memory:\n");
    printf("  - CPU accesses via: %p\n", host_buffer);
    printf("  - Gaudi accesses via: 0x%lx\n", device_va);
    
    // Simulate Gaudi modifying the data
    printf("\n[Simulating Gaudi operation...]\n");
    for (int i = 0; i < 10; i++) {
        data[i] *= 2;  // In reality, Gaudi kernel would do this
    }
    
    // CPU reads the result
    printf("✓ CPU reads modified data: %d %d %d %d %d ...\n",
           data[0], data[1], data[2], data[3], data[4]);
    
    // Method 2: Device Memory (CPU cannot access directly)
    printf("\nMethod 2: Device Memory (HBM)\n");
    printf("-----------------------------\n");
    
    // Allocate device memory
    uint64_t device_handle = hlthunk_device_memory_alloc(fd, buffer_size, 0, false, false);
    if (device_handle == 0) {
        fprintf(stderr, "Failed to allocate device memory\n");
        hlthunk_memory_unmap(fd, device_va);
        free(host_buffer);
        hlthunk_close(fd);
        return 1;
    }
    
    uint64_t hbm_va = hlthunk_device_memory_map(fd, device_handle, 0);
    if (hbm_va == 0) {
        fprintf(stderr, "Failed to map device memory\n");
        hlthunk_device_memory_free(fd, device_handle);
        hlthunk_memory_unmap(fd, device_va);
        free(host_buffer);
        hlthunk_close(fd);
        return 1;
    }
    
    printf("✓ Allocated HBM at device VA: 0x%lx\n", hbm_va);
    printf("❌ CPU cannot access this address directly\n");
    printf("   To read this memory, you must:\n");
    printf("   1. Use DMA to copy from HBM (0x%lx) to host-mapped memory (0x%lx)\n", 
           hbm_va, device_va);
    printf("   2. Then CPU can read from host buffer (%p)\n", host_buffer);
    
    // Cleanup
    hlthunk_memory_unmap(fd, hbm_va);
    hlthunk_device_memory_free(fd, device_handle);
    hlthunk_memory_unmap(fd, device_va);
    free(host_buffer);
    hlthunk_close(fd);
    
    printf("\n✓ Cleanup complete\n");
    
    printf("\nSummary:\n");
    printf("- Host memory mapped to Gaudi: CPU can read/write directly\n");
    printf("- Device memory (HBM): CPU cannot access, must use DMA\n");
    printf("- For RDMA with CPU visibility, use host-mapped memory\n");
    
    return 0;
}
