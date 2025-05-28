#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "hlthunk.h"

#define MEMORY_SIZE 1024 // 1 KB

int main(void)
{
    const char *busid = "0000:4d:00.0";

    // Step 1: Open the Gaudi device
    int fd = hlthunk_open(HLTHUNK_DEVICE_DONT_CARE, busid);
    if (fd < 0) {
        fprintf(stderr, "Failed to open device: %s\n", strerror(errno));
        return -1;
    }

    // Step 2: Allocate host memory (page-aligned for DMA)
    void *host_memory = mmap(NULL, MEMORY_SIZE, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (host_memory == MAP_FAILED) {
        fprintf(stderr, "Failed to allocate host memory: %s\n", strerror(errno));
        hlthunk_close(fd);
        return -1;
    }

    // Step 3: Initialize host memory (optional)
    memset(host_memory, 0, MEMORY_SIZE);
    strcpy((char *)host_memory, "Hello, Gaudi!");

    // Step 4: Map host memory to device virtual address space
    uint64_t device_va = hlthunk_host_memory_map(fd, host_memory, 0, MEMORY_SIZE);
    if (device_va == 0) {
        fprintf(stderr, "Failed to map host memory to device VA\n");
        munmap(host_memory, MEMORY_SIZE);
        hlthunk_close(fd);
        return -1;
    }

    printf("Host memory mapped to device VA: 0x%llx\n", (unsigned long long)device_va);
    printf("Data at host memory: %s\n", (char *)host_memory);

    // Step 5: Unmap and free memory
    if (hlthunk_memory_unmap(fd, device_va) < 0) {
        fprintf(stderr, "Failed to unmap host memory from device\n");
    }

    munmap(host_memory, MEMORY_SIZE);

    // Step 6: Close the device
    if (hlthunk_close(fd) < 0) {
        fprintf(stderr, "Failed to close device: %s\n", strerror(errno));
        return -1;
    }

    return 0;
}
