#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

#include "hlthunk.h"

#define BUFFER_SIZE (16 * 1024)  // 16KB buffer
#define TEST_INT_COUNT 1000      // Number of integers to transfer

int main(int argc, char *argv[]) {
    int rc = 0;
    void *host_buffer = NULL;
    uint64_t device_va = 0;
    int fd = -1;
    
    printf("HPU-CPU Data Transfer Example\n");
    printf("=============================\n");
    
    // Step 1: Open the HPU device
    // Try multiple device types in order of preference
    enum hlthunk_device_name device_types[] = {
        HLTHUNK_DEVICE_GAUDI3,
        HLTHUNK_DEVICE_GAUDI2,
        HLTHUNK_DEVICE_GAUDI,
        HLTHUNK_DEVICE_DONT_CARE
    };
    
    for (int i = 0; i < 4; i++) {
        fd = hlthunk_open(device_types[i], NULL);
        if (fd >= 0) {
            // Found a working device
            enum hlthunk_device_name device_type = hlthunk_get_device_name_from_fd(fd);
            const char *device_name = "Unknown";
            
            switch (device_type) {
                case HLTHUNK_DEVICE_GAUDI3: device_name = "Gaudi3"; break;
                case HLTHUNK_DEVICE_GAUDI2: device_name = "Gaudi2"; break;
                case HLTHUNK_DEVICE_GAUDI: device_name = "Gaudi"; break;
                default: break;
            }
            
            printf("[HPU] Successfully opened %s device (fd: %d)\n", device_name, fd);
            break;
        }
    }
    
    if (fd < 0) {
        fprintf(stderr, "Failed to open any HPU device: %s\n", strerror(errno));
        return 1;
    }
    
    // Print device information
    struct hlthunk_hw_ip_info hw_info;
    if (hlthunk_get_hw_ip_info(fd, &hw_info) == 0) {
        printf("[HPU] Device info:\n");
        printf("  - DRAM base: 0x%lx, size: %lu MB\n", 
               hw_info.dram_base_address, hw_info.dram_size / (1024*1024));
        printf("  - SRAM base: 0x%lx, size: %u KB\n", 
               hw_info.sram_base_address, hw_info.sram_size / 1024);
        printf("  - Device ID: 0x%x\n", hw_info.device_id);
    }
    
    // Step 2: Allocate host memory (page-aligned for better performance)
    host_buffer = aligned_alloc(4096, BUFFER_SIZE);
    if (!host_buffer) {
        fprintf(stderr, "Failed to allocate host memory: %s\n", strerror(errno));
        rc = 1;
        goto cleanup;
    }
    
    printf("[CPU] Allocated host buffer at %p (%d bytes)\n", host_buffer, BUFFER_SIZE);
    
    // Initialize memory to a known pattern (array of integers)
    int *int_buffer = (int*)host_buffer;
    printf("[CPU] Writing initial data pattern...\n");
    for (int i = 0; i < TEST_INT_COUNT; i++) {
        int_buffer[i] = i * 10;  // Simple pattern: 0, 10, 20, 30...
    }
    
    // Step 3: Map host memory to HPU device's address space
    printf("[CPU] Mapping host memory to HPU device address space...\n");
    device_va = hlthunk_host_memory_map(fd, host_buffer, 0, BUFFER_SIZE);
    if (device_va == 0) {
        fprintf(stderr, "Failed to map host memory to device: %s\n", strerror(errno));
        rc = 1;
        goto cleanup;
    }
    
    printf("[HPU] Host memory mapped to device VA: 0x%lx\n", device_va);
    
    // Step 4: CPU can read/write to this memory directly
    printf("[CPU] CPU wrote initial values (first 5 elements): %d, %d, %d, %d, %d\n", 
           int_buffer[0], int_buffer[1], int_buffer[2], int_buffer[3], int_buffer[4]);
    
    // Step 5: Simulate HPU reading and modifying the data
    // In a real application, the HPU would execute a kernel to process this data
    // Here we're just simulating that by modifying the data directly
    printf("[HPU] Simulating HPU operation (doubling all values)...\n");
    
    for (int i = 0; i < TEST_INT_COUNT; i++) {
        int_buffer[i] *= 2;  // Double each value
    }
    
    // Step 6: CPU reads back the modified data
    printf("[CPU] Reading back HPU-modified data (first 5 elements): %d, %d, %d, %d, %d\n",
           int_buffer[0], int_buffer[1], int_buffer[2], int_buffer[3], int_buffer[4]);
    
    // Step 7: Perform another operation (CPU to HPU)
    printf("[CPU] CPU performing another operation (adding 5 to each value)...\n");
    
    for (int i = 0; i < TEST_INT_COUNT; i++) {
        int_buffer[i] += 5;  // Add 5 to each value
    }
    
    // Verify the results
    printf("[CPU] Final values (first 5 elements): %d, %d, %d, %d, %d\n",
           int_buffer[0], int_buffer[1], int_buffer[2], int_buffer[3], int_buffer[4]);
    
    // Verify last element
    printf("[CPU] Final value of last element: %d (expected: %d)\n", 
           int_buffer[TEST_INT_COUNT-1], 
           (TEST_INT_COUNT-1) * 10 * 2 + 5);
           
    if (int_buffer[TEST_INT_COUNT-1] == (TEST_INT_COUNT-1) * 10 * 2 + 5) {
        printf("[HPU] ✅ Data transfer verification PASSED!\n");
    } else {
        printf("[HPU] ❌ Data transfer verification FAILED!\n");
        rc = 1;
    }
    
cleanup:
    // Step 8: Clean up resources
    if (device_va != 0) {
        printf("[HPU] Unmapping device memory...\n");
        hlthunk_memory_unmap(fd, device_va);
    }
    
    if (host_buffer != NULL) {
        free(host_buffer);
    }
    
    if (fd >= 0) {
        printf("[HPU] Closing device...\n");
        hlthunk_close(fd);
    }
    
    return rc;
}