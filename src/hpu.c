// gaudi_dma_copy.c
// Example of DMA copy from Gaudi HBM to host memory

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <stdint.h>
#include "hlthunk.h"

// Gaudi DMA packet structures (simplified)
// Note: Actual structures depend on Gaudi generation
struct gaudi_dma_packet {
    uint32_t opcode;
    uint32_t engine_id;
    uint64_t src_addr;
    uint64_t dst_addr;
    uint32_t size;
    uint32_t flags;
} __attribute__((packed));

// Function to perform DMA copy
int perform_dma_copy(int fd, uint64_t src_addr, uint64_t dst_addr, size_t size) {
    int rc;
    
    printf("\nPerforming DMA copy:\n");
    printf("  Source (HBM): 0x%lx\n", src_addr);
    printf("  Destination (Host): 0x%lx\n", dst_addr);
    printf("  Size: %zu bytes\n", size);
    
    // Step 1: Allocate command buffer
    uint32_t cb_size = 4096;  // Enough for our DMA command
    uint64_t cb_handle = hlthunk_request_command_buffer(fd, cb_size, 0);
    if (!cb_handle) {
        fprintf(stderr, "Failed to allocate command buffer\n");
        return -1;
    }
    
    // Step 2: Build DMA command
    // Note: The exact implementation depends on your hlthunk version and Gaudi generation
    printf("Command buffer allocated with handle: %lu\n", cb_handle);
    
    // Step 3: Show conceptual DMA operation
    printf("\nDMA operation concept:\n");
    printf("  - Source (HBM): 0x%lx\n", src_addr);
    printf("  - Destination (Host): 0x%lx\n", dst_addr);
    printf("  - Size: %zu bytes\n", size);
    
    printf("\nTo perform actual DMA, you would:\n");
    printf("1. Write DMA packet to command buffer\n");
    printf("2. Submit command using hlthunk_command_submission()\n");
    printf("3. Wait for completion using hlthunk_wait_for_cs()\n");
    
    printf("\nThe exact DMA packet format depends on your Gaudi device:\n");
    printf("- Gaudi1: Uses QMAN DMA packets\n");
    printf("- Gaudi2: Uses PDMA engine packets\n");
    printf("- Consult Habana documentation for packet formats\n");
    
    // Cleanup
    hlthunk_destroy_command_buffer(fd, cb_handle);
    
    printf("\n✓ DMA concept demonstrated (actual implementation device-specific)\n");
    
    return 0;
}

// Alternative: Use higher-level memory operations if available
int copy_using_memory_ops(int fd, uint64_t src_addr, uint64_t dst_addr, size_t size) {
    printf("\nAlternative: Using memory operations\n");
    
    // Some versions of hlthunk provide higher-level APIs
    // Check if these are available in your version:
    
    // Option 1: Direct memory read (if supported)
    // hlthunk_memory_read(fd, src_addr, size, dst_buffer);
    
    // Option 2: DMA operations API (if supported)
    // hlthunk_dma_copy(fd, src_addr, dst_addr, size);
    
    printf("Check hlthunk documentation for available APIs\n");
    
    return 0;
}

int main() {
    int fd = -1;
    void *host_buffer = NULL;
    uint64_t host_device_va = 0;
    uint64_t hbm_handle = 0;
    uint64_t hbm_va = 0;
    size_t buffer_size = 4096;
    int rc = 0;
    
    printf("Gaudi DMA Copy Example\n");
    printf("======================\n");
    
    // Open device
    fd = hlthunk_open(HLTHUNK_DEVICE_DONT_CARE, NULL);
    if (fd < 0) {
        fprintf(stderr, "Failed to open device\n");
        return 1;
    }
    
    // Allocate and map host buffer
    host_buffer = aligned_alloc(4096, buffer_size);
    if (!host_buffer) {
        fprintf(stderr, "Failed to allocate host buffer\n");
        hlthunk_close(fd);
        return 1;
    }
    
    // Initialize host buffer
    memset(host_buffer, 0, buffer_size);
    
    // Map host buffer to device
    host_device_va = hlthunk_host_memory_map(fd, host_buffer, 0, buffer_size);
    if (!host_device_va) {
        fprintf(stderr, "Failed to map host buffer\n");
        free(host_buffer);
        hlthunk_close(fd);
        return 1;
    }
    
    printf("Host buffer mapped to device VA: 0x%lx\n", host_device_va);
    
    // Allocate HBM
    hbm_handle = hlthunk_device_memory_alloc(fd, buffer_size, 0, false, false);
    if (!hbm_handle) {
        fprintf(stderr, "Failed to allocate HBM\n");
        rc = 1;
        goto cleanup;
    }
    
    hbm_va = hlthunk_device_memory_map(fd, hbm_handle, 0);
    if (!hbm_va) {
        fprintf(stderr, "Failed to map HBM\n");
        rc = 1;
        goto cleanup;
    }
    
    printf("HBM allocated at device VA: 0x%lx\n", hbm_va);
    
    // Simulate data in HBM (in practice, a Gaudi kernel would write this)
    printf("\n[In practice, Gaudi kernel would write data to HBM here]\n");
    
    // Perform DMA copy from HBM to host
    rc = perform_dma_copy(fd, hbm_va, host_device_va, buffer_size);
    if (rc == 0) {
        // Now CPU can read the data
        printf("\nCPU can now read data from host buffer:\n");
        int *data = (int *)host_buffer;
        printf("First 10 integers: ");
        for (int i = 0; i < 10; i++) {
            printf("%d ", data[i]);
        }
        printf("\n");
    }
    
    // Try alternative method
    copy_using_memory_ops(fd, hbm_va, host_device_va, buffer_size);
    
cleanup:
    if (hbm_va) hlthunk_memory_unmap(fd, hbm_va);
    if (hbm_handle) hlthunk_device_memory_free(fd, hbm_handle);
    if (host_device_va) hlthunk_memory_unmap(fd, host_device_va);
    if (host_buffer) free(host_buffer);
    if (fd >= 0) hlthunk_close(fd);
    
    return rc;
}

// Simpler example using memcpy for demonstration
void simple_example() {
    printf("\n=== Simplified Conceptual Flow ===\n");
    printf("1. Gaudi writes data to HBM at 0x1001001800000000\n");
    printf("2. CPU wants to read this data but cannot access HBM directly\n");
    printf("3. Submit DMA command:\n");
    printf("   - Source: 0x1001001800000000 (HBM)\n");
    printf("   - Destination: 0xfff0000100200000 (host buffer in device address space)\n");
    printf("4. Wait for DMA completion\n");
    printf("5. CPU reads from host buffer at 0x55644eac8000\n");
    printf("\nThe key is that the same memory has two addresses:\n");
    printf("- CPU view: 0x55644eac8000 (virtual address)\n");
    printf("- Device view: 0xfff0000100200000 (device address)\n");
}
