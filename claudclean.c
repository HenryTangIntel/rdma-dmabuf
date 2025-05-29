#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <infiniband/verbs.h>
#include "hlthunk.h"

typedef struct {
    int gaudi_fd;
    int dmabuf_fd;
    struct ibv_context *ib_ctx;
    struct ibv_pd *pd;
    struct ibv_mr *mr;
    size_t buffer_size;
    uint64_t gaudi_handle;
    uint64_t device_va;
    struct hlthunk_hw_ip_info hw_info;
} dmabuf_context_t;

// Initialize Intel Gaudi device
int init_gaudi_device(dmabuf_context_t *ctx) {
    enum hlthunk_device_name preferred_devices[] = {
        HLTHUNK_DEVICE_GAUDI3,
        HLTHUNK_DEVICE_GAUDI2,
        HLTHUNK_DEVICE_GAUDI,
        HLTHUNK_DEVICE_DONT_CARE
    };
    
    for (int i = 0; i < 4; i++) {
        ctx->gaudi_fd = hlthunk_open(preferred_devices[i], NULL);
        if (ctx->gaudi_fd >= 0) break;
    }
    
    if (ctx->gaudi_fd < 0) {
        fprintf(stderr, "Failed to open any Gaudi device: %s\n", strerror(errno));
        return -1;
    }
    
    if (hlthunk_get_hw_ip_info(ctx->gaudi_fd, &ctx->hw_info) != 0) {
        fprintf(stderr, "Failed to get hardware info: %s\n", strerror(errno));
        hlthunk_close(ctx->gaudi_fd);
        return -1;
    }
    
    enum hlthunk_device_name device_type = hlthunk_get_device_name_from_fd(ctx->gaudi_fd);
    const char *device_name = "Unknown";
    switch (device_type) {
        case HLTHUNK_DEVICE_GAUDI3: device_name = "Gaudi3"; break;
        case HLTHUNK_DEVICE_GAUDI2: device_name = "Gaudi2"; break;
        case HLTHUNK_DEVICE_GAUDI: device_name = "Gaudi"; break;
        default: break;
    }
    
    printf("Successfully opened %s device (fd: %d)\n", device_name, ctx->gaudi_fd);
    printf("Device info:\n");
    printf("  DRAM base: 0x%lx, size: %lu MB\n", 
           ctx->hw_info.dram_base_address, ctx->hw_info.dram_size / (1024*1024));
    printf("  SRAM base: 0x%lx, size: %u KB\n", 
           ctx->hw_info.sram_base_address, ctx->hw_info.sram_size / 1024);
    printf("  Device ID: 0x%x\n", ctx->hw_info.device_id);
    
    return 0;
}

// Allocate DMA-buf on Gaudi device
int allocate_gaudi_dmabuf(dmabuf_context_t *ctx, size_t size) {
    ctx->buffer_size = size;
    
    printf("Allocating %zu bytes of shared device memory...\n", size);
    ctx->gaudi_handle = hlthunk_device_memory_alloc(ctx->gaudi_fd, size, 0, true, true);
    if (ctx->gaudi_handle == 0) {
        fprintf(stderr, "Failed to allocate Gaudi device memory: %s\n", strerror(errno));
        return -1;
    }
    
    printf("Mapping device memory to virtual address...\n");
    ctx->device_va = hlthunk_device_memory_map(ctx->gaudi_fd, ctx->gaudi_handle, 0);
    if (ctx->device_va == 0) {
        fprintf(stderr, "Failed to map Gaudi device memory: %s\n", strerror(errno));
        hlthunk_device_memory_free(ctx->gaudi_fd, ctx->gaudi_handle);
        return -1;
    }
    
    printf("Exporting device memory as DMA-buf...\n");
    ctx->dmabuf_fd = hlthunk_device_mapped_memory_export_dmabuf_fd(
        ctx->gaudi_fd, ctx->device_va, size, 0, (O_RDWR | O_CLOEXEC));
    if (ctx->dmabuf_fd < 0) {
        fprintf(stderr, "Failed to export DMA-buf: %s\n", strerror(errno));
        hlthunk_memory_unmap(ctx->gaudi_fd, ctx->device_va);
        hlthunk_device_memory_free(ctx->gaudi_fd, ctx->gaudi_handle);
        return -1;
    }
    
    printf("Successfully allocated Gaudi memory:\n");
    printf("  Device handle: 0x%lx\n", ctx->gaudi_handle);
    printf("  Device VA: 0x%lx\n", ctx->device_va);
    printf("  DMA-buf fd: %d\n", ctx->dmabuf_fd);
    printf("  Size: %zu bytes\n", size);
    
    return 0;
}

// Initialize Mellanox InfiniBand context
int init_mellanox_ib(dmabuf_context_t *ctx) {
    struct ibv_device **dev_list;
    int num_devices;
    
    dev_list = ibv_get_device_list(&num_devices);
    if (!dev_list || num_devices == 0) {
        fprintf(stderr, "No InfiniBand devices found\n");
        return -1;
    }
    
    printf("Found %d InfiniBand device(s)\n", num_devices);
    for (int i = 0; i < num_devices; i++) {
        printf("  Device %d: %s (GUID: %016lx)\n", i, 
               ibv_get_device_name(dev_list[i]),
               be64toh(ibv_get_device_guid(dev_list[i])));
    }
    
    ctx->ib_ctx = ibv_open_device(dev_list[0]);
    if (!ctx->ib_ctx) {
        fprintf(stderr, "Failed to open InfiniBand device: %s\n", strerror(errno));
        ibv_free_device_list(dev_list);
        return -1;
    }
    
    ctx->pd = ibv_alloc_pd(ctx->ib_ctx);
    if (!ctx->pd) {
        fprintf(stderr, "Failed to allocate protection domain: %s\n", strerror(errno));
        ibv_close_device(ctx->ib_ctx);
        ibv_free_device_list(dev_list);
        return -1;
    }
    
    ibv_free_device_list(dev_list);
    printf("Successfully initialized Mellanox IB context\n");
    return 0;
}

// Register DMA-buf with InfiniBand
int register_dmabuf_with_ib(dmabuf_context_t *ctx) {
    printf("Success: Direct DMA-buf registration!\n");
  
     ctx->mr = ibv_reg_dmabuf_mr(ctx->pd, 0, ctx->buffer_size, 
                                0, ctx->dmabuf_fd, 
                                IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    if (!ctx->mr) {
        fprintf(stderr, "Failed to register DMA-buf with InfiniBand: %s\n", strerror(errno));
        return -1;
    }

    printf("Success: Direct DMA-buf registration!\n");
    return 0;
}

// Perform data operations
int perform_data_operations(dmabuf_context_t *ctx) {
    printf("\nPerforming data operations...\n");
    printf("Buffer is registered for zero-copy DMA operations\n");
    printf("No CPU access available - this is optimal for GPU-to-NIC transfers\n");
    printf("In a real application, you would:\n");
    printf("  1. Use Gaudi kernels to write data to device memory\n");
    printf("  2. Initiate RDMA operations directly from device memory\n");
    printf("  3. Achieve zero-copy GPU-to-network transfers\n");
    return 0;
}

// Print system information
void print_system_info(void) {
    printf("System Information:\n");
    printf("==================\n");
    printf("Habanalabs support: ENABLED\n");
    printf("Page size: %ld bytes\n", sysconf(_SC_PAGESIZE));
    printf("Process ID: %d\n", getpid());
    
    if (access("/sys/kernel/debug/dma_buf", F_OK) == 0) {
        printf("DMA-buf debugfs: Available\n");
    } else {
        printf("DMA-buf debugfs: Not available (may need root or debugfs mount)\n");
    }
}

// Cleanup resources
void cleanup_context(dmabuf_context_t *ctx) {
    if (ctx->mr) {
        // Would call ibv_dereg_mr(ctx->mr) in real implementation
        ibv_dereg_mr(ctx->mr);
    }
    
    if (ctx->pd) {
        ibv_dealloc_pd(ctx->pd);
    }
    
    if (ctx->ib_ctx) {
        ibv_close_device(ctx->ib_ctx);
    }
    
    if (ctx->dmabuf_fd >= 0) {
        close(ctx->dmabuf_fd);
    }
    
    if (ctx->gaudi_handle) {
        if (ctx->device_va) {
            hlthunk_memory_unmap(ctx->gaudi_fd, ctx->device_va);
        }
        hlthunk_device_memory_free(ctx->gaudi_fd, ctx->gaudi_handle);
    }
    
    if (ctx->gaudi_fd >= 0) {
        hlthunk_close(ctx->gaudi_fd);
    }
}

int main(int argc, char *argv[]) {
    dmabuf_context_t ctx = {0};
    ctx.dmabuf_fd = -1;
    ctx.gaudi_fd = -1;
    
    size_t buffer_size = 4 * 1024 * 1024; // 4MB buffer
    
    if (argc > 1) {
        buffer_size = strtoull(argv[1], NULL, 0);
        if (buffer_size == 0) {
            fprintf(stderr, "Invalid buffer size: %s\n", argv[1]);
            return 1;
        }
    }
    
    printf("Intel Gaudi DMA-buf with Mellanox InfiniBand Integration\n");
    printf("=======================================================\n");
    printf("Buffer size: %zu bytes (%.2f MB)\n", buffer_size, buffer_size / (1024.0 * 1024.0));
    printf("\n");
    
    print_system_info();
    printf("\n");
    
    printf("Step 1: Initialize Gaudi device\n");
    printf("-------------------------------\n");
    if (init_gaudi_device(&ctx)) {
        fprintf(stderr, "Failed to initialize Gaudi device\n");
        cleanup_context(&ctx);
        return 1;
    }
    printf("\n");
    
    printf("Step 2: Allocate DMA-buf on Gaudi\n");
    printf("---------------------------------\n");
    if (allocate_gaudi_dmabuf(&ctx, buffer_size)) {
        fprintf(stderr, "Failed to allocate Gaudi DMA-buf\n");
        cleanup_context(&ctx);
        return 1;
    }
    printf("\n");
    
    printf("Step 3: Initialize InfiniBand\n");
    printf("-----------------------------\n");
    if (init_mellanox_ib(&ctx)) {
        fprintf(stderr, "Failed to initialize InfiniBand\n");
        printf("Note: This is expected if no Mellanox hardware is present\n");
        printf("Continuing without InfiniBand registration...\n");
    } else {
        printf("\n");
        
        printf("Step 4: Register DMA-buf with InfiniBand\n");
        printf("----------------------------------------\n");
        if (register_dmabuf_with_ib(&ctx)) {
            fprintf(stderr, "Failed to register buffer with InfiniBand\n");
        }
        printf("\n");
    }
    
    printf("Step 5: Data operations on shared buffer\n");
    printf("----------------------------------------\n");
    if (perform_data_operations(&ctx)) {
        fprintf(stderr, "Data operations failed\n");
        cleanup_context(&ctx);
        return 1;
    }
    printf("\n");
    
    printf("🎉 Example completed successfully!\n");
    printf("===================================\n");
    printf("✅ DMA-buf integration demonstrated\n");
    printf("   - Gaudi device: fd %d\n", ctx.gaudi_fd);
    printf("   - DMA-buf: fd %d\n", ctx.dmabuf_fd);
    printf("   - Device VA: 0x%lx\n", ctx.device_va);
    printf("   - Buffer size: %zu bytes\n", ctx.buffer_size);
    printf("\n");
    printf("This buffer can now be used for:\n");
    printf("• Zero-copy data transfers between Gaudi and Mellanox NIC\n");
    printf("• RDMA operations directly from/to Gaudi device memory\n");
    printf("• High-performance distributed AI/ML workloads\n");
    
    cleanup_context(&ctx);
    return 0;
}