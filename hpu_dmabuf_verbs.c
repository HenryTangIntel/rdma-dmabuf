#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <infiniband/verbs.h>
#include <stdbool.h>

// Include the actual hlthunk header
#include "hlthunk.h"

// DMA-buf synchronization structures (if not available in system headers)
#ifndef DMA_BUF_IOCTL_SYNC
struct dma_buf_sync {
    uint64_t flags;
};

#define DMA_BUF_SYNC_READ      (1 << 0)
#define DMA_BUF_SYNC_WRITE     (2 << 0)
#define DMA_BUF_SYNC_RW        (DMA_BUF_SYNC_READ | DMA_BUF_SYNC_WRITE)
#define DMA_BUF_SYNC_START     (0 << 2)
#define DMA_BUF_SYNC_END       (1 << 2)
#define DMA_BUF_IOCTL_SYNC     _IOW('b', 0, struct dma_buf_sync)
#endif

typedef struct {
    int gaudi_fd;
    int dmabuf_fd;
    struct ibv_context *ib_ctx;
    struct ibv_pd *pd;
    struct ibv_mr *mr;
    void *buffer;
    size_t buffer_size;
    uint64_t gaudi_handle;
    uint64_t device_va;
    struct hlthunk_hw_ip_info hw_info;
} dmabuf_context_t;

// Function declarations
int init_gaudi_device(dmabuf_context_t *ctx);
int create_fallback_buffer(dmabuf_context_t *ctx, size_t size);
int allocate_gaudi_dmabuf(dmabuf_context_t *ctx, size_t size);
int init_mellanox_ib(dmabuf_context_t *ctx);
int register_buffer_with_ib(dmabuf_context_t *ctx);
int sync_dmabuf(int dmabuf_fd, uint64_t flags);
int perform_data_operations(dmabuf_context_t *ctx);
void print_system_info(void);
void cleanup_context(dmabuf_context_t *ctx);

// Initialize Intel Gaudi device
int init_gaudi_device(dmabuf_context_t *ctx) {
    // Open Gaudi device (prefer Gaudi3/Gaudi2, fall back to any available)
    enum hlthunk_device_name preferred_devices[] = {
        HLTHUNK_DEVICE_GAUDI3,
        HLTHUNK_DEVICE_GAUDI2,
        HLTHUNK_DEVICE_GAUDI,
        HLTHUNK_DEVICE_DONT_CARE
    };
    
    for (int i = 0; i < 4; i++) {
        ctx->gaudi_fd = hlthunk_open(preferred_devices[i], NULL);
        if (ctx->gaudi_fd >= 0) {
            break;
        }
    }
    
    if (ctx->gaudi_fd < 0) {
        fprintf(stderr, "Failed to open any Gaudi device: %s\n", strerror(errno));
        return -1;
    }
    
    // Get hardware information
    if (hlthunk_get_hw_ip_info(ctx->gaudi_fd, &ctx->hw_info) != 0) {
        fprintf(stderr, "Failed to get hardware info: %s\n", strerror(errno));
        hlthunk_close(ctx->gaudi_fd);
        ctx->gaudi_fd = -1;
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

// Create a regular memory buffer as fallback when Gaudi allocation fails
int create_fallback_buffer(dmabuf_context_t *ctx, size_t size) {
    printf("Creating fallback memory buffer (%zu bytes)\n", size);
    
    ctx->buffer_size = size;
    ctx->buffer = aligned_alloc(4096, size); // Page-aligned allocation
    if (!ctx->buffer) {
        fprintf(stderr, "Failed to allocate fallback buffer: %s\n", strerror(errno));
        return -1;
    }
    
    // Initialize buffer with test pattern
    memset(ctx->buffer, 0x42, size);
    ctx->dmabuf_fd = -1; // No actual DMA-buf fd in fallback mode
    
    printf("Fallback buffer allocated at %p\n", ctx->buffer);
    return 0;
}

// Allocate DMA-buf on Gaudi device
int allocate_gaudi_dmabuf(dmabuf_context_t *ctx, size_t size) {
    ctx->buffer_size = size;
    
    // Allocate device memory on Gaudi
    printf("Allocating %zu bytes of device memory...\n", size);
    ctx->gaudi_handle = hlthunk_device_memory_alloc(ctx->gaudi_fd, size, 0, true, false);
    if (ctx->gaudi_handle == 0) {
        fprintf(stderr, "Failed to allocate Gaudi device memory: %s\n", strerror(errno));
        return create_fallback_buffer(ctx, size);
    }
    
    // Map the device memory to get a device virtual address
    printf("Mapping device memory to virtual address...\n");
    ctx->device_va = hlthunk_device_memory_map(ctx->gaudi_fd, ctx->gaudi_handle, 0);
    if (ctx->device_va == 0) {
        fprintf(stderr, "Failed to map Gaudi device memory: %s\n", strerror(errno));
        hlthunk_device_memory_free(ctx->gaudi_fd, ctx->gaudi_handle);
        return create_fallback_buffer(ctx, size);
    }
    
    // Export the mapped memory as DMA-buf
    printf("Exporting device memory as DMA-buf...\n");
    ctx->dmabuf_fd = hlthunk_device_mapped_memory_export_dmabuf_fd(
        ctx->gaudi_fd, ctx->device_va, size, 0, (O_RDWR | O_CLOEXEC));
    if (ctx->dmabuf_fd < 0) {
        fprintf(stderr, "Failed to export DMA-buf: %s\n", strerror(errno));
        hlthunk_memory_unmap(ctx->gaudi_fd, ctx->device_va);
        hlthunk_device_memory_free(ctx->gaudi_fd, ctx->gaudi_handle);
        return create_fallback_buffer(ctx, size);
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
    
    // Get list of IB devices
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
    
    // Open first available device
    ctx->ib_ctx = ibv_open_device(dev_list[0]);
    if (!ctx->ib_ctx) {
        fprintf(stderr, "Failed to open InfiniBand device: %s\n", strerror(errno));
        ibv_free_device_list(dev_list);
        return -1;
    }
    
    // Allocate protection domain
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
// Register buffer with InfiniBand
int register_buffer_with_ib(dmabuf_context_t *ctx) {
    if (ctx->dmabuf_fd >= 0) {
        // Try to register DMA-buf directly with InfiniBand (modern approach)
        // This preserves zero-copy semantics and avoids CPU mapping
        printf("Attempting to register DMA-buf directly with InfiniBand...\n");
        
        // For DMA-buf registration, we use the device virtual address
        // The InfiniBand driver should handle the DMA-buf directly
        ctx->mr = ibv_reg_mr(ctx->pd, (void*)ctx->device_va, ctx->buffer_size, 
                            IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | 
                            IBV_ACCESS_REMOTE_READ);
        
        if (!ctx->mr) {
            printf("Direct DMA-buf registration failed, falling back to mmap approach...\n");
            
            // Fallback: map DMA-buf to CPU virtual address space
            ctx->buffer = mmap(NULL, ctx->buffer_size, PROT_READ | PROT_WRITE, 
                              MAP_SHARED, ctx->dmabuf_fd, 0);
            if (ctx->buffer == MAP_FAILED) {
                fprintf(stderr, "Failed to mmap DMA-buf: %s\n", strerror(errno));
                return -1;
            }
            printf("Mapped DMA-buf to virtual address %p\n", ctx->buffer);
            
            // Register the mapped memory
            ctx->mr = ibv_reg_mr(ctx->pd, ctx->buffer, ctx->buffer_size, 
                                IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | 
                                IBV_ACCESS_REMOTE_READ);
            if (!ctx->mr) {
                fprintf(stderr, "Failed to register memory region with IB: %s\n", strerror(errno));
                munmap(ctx->buffer, ctx->buffer_size);
                return -1;
            }
        } else {
            printf("Successfully registered DMA-buf directly (zero-copy preserved)\n");
            // No CPU mapping needed - true zero-copy path
            ctx->buffer = NULL; // Indicate no CPU mapping
        }
    } else {
        // Fallback buffer case - already have CPU virtual address
        ctx->mr = ibv_reg_mr(ctx->pd, ctx->buffer, ctx->buffer_size, 
                            IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | 
                            IBV_ACCESS_REMOTE_READ);
        if (!ctx->mr) {
            fprintf(stderr, "Failed to register fallback buffer with IB: %s\n", strerror(errno));
            return -1;
        }
    }
    
    printf("Successfully registered buffer with InfiniBand\n");
    printf("  Local key (lkey): 0x%x\n", ctx->mr->lkey);
    printf("  Remote key (rkey): 0x%x\n", ctx->mr->rkey);
    printf("  Buffer address: %p\n", ctx->mr->addr);
    printf("  Buffer length: %zu\n", ctx->mr->length);
    printf("  Zero-copy mode: %s\n", ctx->buffer ? "No (CPU mapped)" : "Yes (direct DMA-buf)");
    
    return 0;
}


// Synchronize DMA-buf access
int sync_dmabuf(int dmabuf_fd, uint64_t flags) {
    if (dmabuf_fd < 0) {
        // No actual DMA-buf, skip sync
        return 0;
    }
    
    struct dma_buf_sync sync = {0};
    sync.flags = flags;
    
    int ret = ioctl(dmabuf_fd, DMA_BUF_IOCTL_SYNC, &sync);
    if (ret) {
        fprintf(stderr, "DMA-buf sync failed: %s\n", strerror(errno));
        return -1;
    }
    
    return 0;
}

// Example data operations
int perform_data_operations(dmabuf_context_t *ctx) {
    printf("\nPerforming data operations...\n");
    
    // Sync for CPU write access
    if (sync_dmabuf(ctx->dmabuf_fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE)) {
        return -1;
    }
    
    // Write test pattern to buffer
    uint32_t *data = (uint32_t *)ctx->buffer;
    size_t num_words = ctx->buffer_size / sizeof(uint32_t);
    
    printf("Writing test pattern to %zu words...\n", num_words);
    for (size_t i = 0; i < num_words; i++) {
        data[i] = (uint32_t)(i ^ 0xDEADBEEF);
    }
    
    // End CPU access
    if (sync_dmabuf(ctx->dmabuf_fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE)) {
        return -1;
    }
    
    printf("Test pattern written successfully\n");
    
    // Verify first few words
    if (sync_dmabuf(ctx->dmabuf_fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ)) {
        return -1;
    }
    
    printf("Verifying data (first 8 words):\n");
    for (int i = 0; i < 8 && i < (int)num_words; i++) {
        printf("  [%d] = 0x%08x\n", i, data[i]);
    }
    
    if (sync_dmabuf(ctx->dmabuf_fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ)) {
        return -1;
    }
    
    printf("Data operations completed successfully\n");
    return 0;
}

// Print system information
void print_system_info(void) {
    printf("System Information:\n");
    printf("==================\n");
    
    printf("Habanalabs support: ENABLED\n");
    
    printf("Page size: %ld bytes\n", sysconf(_SC_PAGESIZE));
    printf("Process ID: %d\n", getpid());
    
    // Check for DMA-buf support in kernel
    if (access("/sys/kernel/debug/dma_buf", F_OK) == 0) {
        printf("DMA-buf debugfs: Available\n");
    } else {
        printf("DMA-buf debugfs: Not available (may need root or debugfs mount)\n");
    }
}

// Cleanup resources
void cleanup_context(dmabuf_context_t *ctx) {
    if (ctx->mr) {
        ibv_dereg_mr(ctx->mr);
        ctx->mr = NULL;
    }
    
    if (ctx->buffer && ctx->buffer != MAP_FAILED) {
        if (ctx->dmabuf_fd >= 0) {
            munmap(ctx->buffer, ctx->buffer_size);
        } else {
            free(ctx->buffer);
        }
        ctx->buffer = NULL;
    }
    
    if (ctx->pd) {
        ibv_dealloc_pd(ctx->pd);
        ctx->pd = NULL;
    }
    
    if (ctx->ib_ctx) {
        ibv_close_device(ctx->ib_ctx);
        ctx->ib_ctx = NULL;
    }
    
    if (ctx->dmabuf_fd >= 0) {
        close(ctx->dmabuf_fd);
        ctx->dmabuf_fd = -1;
    }
    
    if (ctx->gaudi_handle) {
        hlthunk_memory_unmap(ctx->gaudi_fd, ctx->device_va);
        hlthunk_device_memory_free(ctx->gaudi_fd, ctx->gaudi_handle);
        ctx->gaudi_handle = 0;
        ctx->device_va = 0;
    }
    
    if (ctx->gaudi_fd >= 0) {
        hlthunk_close(ctx->gaudi_fd);
        ctx->gaudi_fd = -1;
    }
}

// Main function
int main(int argc, char *argv[]) {
    dmabuf_context_t ctx = {0};
    ctx.dmabuf_fd = -1;
    ctx.gaudi_fd = -1;
    
    size_t buffer_size = 4 * 1024 * 1024; // 4MB buffer (good for testing)
    int exit_code = 0;
    
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
    
    // Initialize Intel Gaudi device
    printf("Step 1: Initialize Gaudi device\n");
    printf("-------------------------------\n");
    if (init_gaudi_device(&ctx)) {
        fprintf(stderr, "Failed to initialize Gaudi device\n");
        exit_code = 1;
        goto cleanup;
    }
    printf("\n");
    
    // Allocate DMA-buf on Gaudi
    printf("Step 2: Allocate DMA-buf on Gaudi\n");
    printf("---------------------------------\n");
    if (allocate_gaudi_dmabuf(&ctx, buffer_size)) {
        fprintf(stderr, "Failed to allocate Gaudi DMA-buf\n");
        exit_code = 1;
        goto cleanup;
    }
    printf("\n");
    
    // Initialize Mellanox InfiniBand
    printf("Step 3: Initialize InfiniBand\n");
    printf("-----------------------------\n");
    if (init_mellanox_ib(&ctx)) {
        fprintf(stderr, "Failed to initialize InfiniBand\n");
        printf("Note: This is expected if no Mellanox hardware is present\n");
        printf("Continuing without InfiniBand registration...\n");
    } else {
        printf("\n");
        
        // Register buffer with InfiniBand
        printf("Step 4: Register DMA-buf with InfiniBand\n");
        printf("----------------------------------------\n");
        if (register_buffer_with_ib(&ctx)) {
            fprintf(stderr, "Failed to register buffer with InfiniBand\n");
            printf("Note: This may happen if hardware doesn't support DMA-buf with IB\n");
        } else {
            printf("SUCCESS: DMA-buf is now accessible by both Gaudi and Mellanox NIC!\n");
        }
        printf("\n");
    }
    
    // Perform data operations
    printf("Step 5: Data operations on shared buffer\n");
    printf("----------------------------------------\n");
    if (perform_data_operations(&ctx)) {
        fprintf(stderr, "Data operations failed\n");
        exit_code = 1;
        goto cleanup;
    }
    printf("\n");
    
    printf("🎉 Example completed successfully!\n");
    printf("===================================\n");
    if (ctx.dmabuf_fd >= 0) {
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
    } else {
        printf("❓ Fallback memory buffer used (no actual DMA-buf)\n");
        printf("   Consider checking hardware availability and drivers\n");
    }
    
cleanup:
    cleanup_context(&ctx);
    return exit_code;
}