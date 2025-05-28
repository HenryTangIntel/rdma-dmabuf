#include <errno.h>
#include <fcntl.h>
#include "hlthunk.h"
#include "hlthunk_tests.h"

#define MEMORY_SIZE 1024 // 1 KB

int main(void)
{ 
        const char *busid = "0000:4d:00.0";
        int ret;
        uint64_t device_va;

        //int ctrl_fd = hlthunk_open_control_by_name(HLTHUNK_DEVICE_DONT_CARE, busid);
	//if (ctrl_fd < 0) return -1;
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
        strcpy(host_memory, "Hello, Gaudi!");

        // Step 4: Map host memory to device virtual address space
        ret = hlthunk_host_memory_map(fd, host_memory, 0, MEMORY_SIZE);
        if (ret < 0) {
                fprintf(stderr, "Failed to map host memory: %s\n", strerror(-ret));
                munmap(host_memory, MEMORY_SIZE);
                hlthunk_close(fd);
                return -1;
        }

        device_va = (uint64_t)ret; // Device virtual address

        printf("Host memory mapped to device VA: 0x%llx\n", device_va);

        // Step 5: Use the device virtual address in device operations
        // (Example: Assume a kernel or command buffer uses device_va)
        // For simplicity, just print the mapped data
        printf("Data at host memory: %s\n", (char *)host_memory);

        // Step 6: Unmap and free memory
        ret = hlthunk_device_memory_unmap(fd, device_va);
        if (ret < 0) {
                fprintf(stderr, "Failed to unmap host memory: %s\n", strerror(-ret));
        }

        munmap(host_memory, MEMORY_SIZE);


        // Step 7: Close the device
        int rc = hlthunk_close(fd);
        if (rc < 0) {
                fprintf(stderr, "Failed to close device: %s\n", strerror(errno));
                return -1;
        }

        return 0;

}
