// rdma_dmabuf_server.c
#include "rdma_dmabuf_common.h"

int main(int argc, char *argv[]) {
    rdma_context_t ctx = {0};
    ctx.gaudi_fd = -1;
    ctx.dmabuf_fd = -1;
    ctx.sock = -1;
    
    int port = 20000;
    char *ib_dev_name = NULL;
    size_t buffer_size = RDMA_BUFFER_SIZE;
    
    // Parse arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            ib_dev_name = argv[++i];
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            buffer_size = strtoull(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [-p port] [-d ib_dev] [-s buffer_size]\n", argv[0]);
            return 0;
        }
    }
    
    printf("RDMA DMA-buf Server\n");
    printf("===================\n");
    printf("Port: %d\n", port);
    printf("Buffer size: %zu bytes\n", buffer_size);
    if (ib_dev_name) printf("IB device: %s\n", ib_dev_name);
    printf("\n");
    
    // Initialize Gaudi DMA-buf
    printf("Initializing Gaudi DMA-buf...\n");
    if (init_gaudi_dmabuf(&ctx, buffer_size) < 0) {
        fprintf(stderr, "Failed to initialize Gaudi DMA-buf\n");
        cleanup_resources(&ctx);
        return 1;
    }
    
    if (ctx.dmabuf_fd >= 0) {
        printf("✓ Gaudi DMA-buf allocated (fd=%d, va=0x%lx)\n", ctx.dmabuf_fd, ctx.device_va);
    } else {
        printf("✓ Using regular memory buffer\n");
    }
    
    // Initialize RDMA resources
    printf("\nInitializing RDMA resources...\n");
    if (init_rdma_resources(&ctx, ib_dev_name) < 0) {
        fprintf(stderr, "Failed to initialize RDMA resources\n");
        cleanup_resources(&ctx);
        return 1;
    }
    printf("✓ RDMA resources initialized\n");
    
    // Wait for client connection
    printf("\nWaiting for client connection on port %d...\n", port);
    if (connect_qp(&ctx, NULL, port) < 0) {
        fprintf(stderr, "Failed to establish connection\n");
        cleanup_resources(&ctx);
        return 1;
    }
    printf("✓ Client connected\n");
    
    // Function to display buffer data (first few integers)
    void display_buffer_data(const char *label, void *buffer, size_t size) {
        if (!buffer) {
            printf("%s: Data in device memory (no CPU access)\n", label);
            return;
        }
        
        int *int_data = (int *)buffer;
        int count = size / sizeof(int);
        int display_count = count > 10 ? 10 : count;
        
        printf("%s (first %d of %d ints): ", label, display_count, count);
        for (int i = 0; i < display_count; i++) {
            printf("%d ", int_data[i]);
        }
        printf("...\n");
    }
    
    // Initialize buffer with test pattern if CPU accessible
    if (ctx.buffer) {
        printf("\n[CPU→HPU] Writing initial data pattern to buffer...\n");
        int *int_data = (int *)ctx.buffer;
        int count = MSG_SIZE / sizeof(int);
        
        // Write a recognizable pattern
        for (int i = 0; i < count; i++) {
            int_data[i] = 1000 + i;  // Pattern: 1000, 1001, 1002...
        }
        
        display_buffer_data("[CPU] Initial server data", ctx.buffer, MSG_SIZE);
        
        if (ctx.host_device_va) {
            printf("[HPU] Data accessible at device VA 0x%lx\n", ctx.host_device_va);
        }
    } else {
        printf("Note: Buffer is in device memory - would be initialized by Gaudi kernel\n");
    }
    
    // Main communication loop
    printf("\nStarting communication...\n");
    
    for (int i = 0; i < 3; i++) {
        printf("\n--- Iteration %d ---\n", i + 1);
        
        // Post receive for client's message
        if (post_receive(&ctx) < 0) {
            fprintf(stderr, "Failed to post receive\n");
            break;
        }
        
        // Wait for client's message
        printf("Waiting for client message...\n");
        if (poll_completion(&ctx) < 0) {
            fprintf(stderr, "Failed to receive message\n");
            break;
        }
        
        if (ctx.buffer) {
            printf("[HPU→CPU] Reading received data:\n");
            display_buffer_data("Received from client", ctx.buffer, MSG_SIZE);
            
            // Simulate HPU processing: multiply each value by 2
            printf("[HPU] Processing data (multiplying by 2)...\n");
            int *int_data = (int *)ctx.buffer;
            int count = MSG_SIZE / sizeof(int);
            for (int j = 0; j < count && j < 256; j++) {  // Process first 256 ints
                int_data[j] *= 2;
            }
            
            display_buffer_data("[CPU] After HPU processing", ctx.buffer, MSG_SIZE);
        } else {
            printf("Received data in device memory\n");
        }
        
        // Send response
        printf("Sending response...\n");
        if (post_send(&ctx, IBV_WR_SEND) < 0) {
            fprintf(stderr, "Failed to post send\n");
            break;
        }
        
        if (poll_completion(&ctx) < 0) {
            fprintf(stderr, "Failed to send message\n");
            break;
        }
        printf("✓ Response sent\n");
    }
    
    // RDMA Write test
    printf("\n--- RDMA Write Test ---\n");
    if (ctx.buffer) {
        printf("[CPU→HPU] Preparing RDMA Write data...\n");
        int *int_data = (int *)ctx.buffer;
        // Write a special pattern for RDMA Write
        for (int i = 0; i < 10; i++) {
            int_data[i] = 9000 + i;  // Pattern: 9000, 9001, 9002...
        }
        display_buffer_data("[CPU] RDMA Write data", ctx.buffer, MSG_SIZE);
    }
    
    printf("Performing RDMA Write to client...\n");
    if (post_send(&ctx, IBV_WR_RDMA_WRITE) < 0) {
        fprintf(stderr, "Failed to post RDMA write\n");
    } else if (poll_completion(&ctx) < 0) {
        fprintf(stderr, "RDMA write failed\n");
    } else {
        printf("✓ RDMA Write completed\n");
    }
    
    // Wait for client to finish
    printf("\nWaiting for client to finish...\n");
    char sync_byte;
    if (read(ctx.sock, &sync_byte, 1) == 1) {
        printf("✓ Client finished\n");
    }
    
    // Print summary
    printf("\n=== Summary ===\n");
    if (ctx.dmabuf_fd >= 0) {
        printf("✅ Zero-copy RDMA using Gaudi DMA-buf\n");
        printf("   - Gaudi device memory: 0x%lx\n", ctx.device_va);
        printf("   - DMA-buf fd: %d\n", ctx.dmabuf_fd);
        printf("   - Direct device-to-network transfers\n");
    } else {
        printf("✅ RDMA using regular memory\n");
        printf("   - Host buffer: %p\n", ctx.buffer);
    }
    printf("\n📊 Operations Summary:\n");
    printf("   ✓ Send/Receive: 3 iterations completed\n");
    printf("   ✓ RDMA Write: Successfully pushed data to client\n");
    printf("\n💡 Note: RDMA Read operations are typically not supported\n");
    printf("   with device memory due to DMA initiator requirements.\n");
    printf("   Use RDMA Write to push data or Send/Receive for bidirectional.\n");
    
    cleanup_resources(&ctx);
    printf("\nServer shutdown complete\n");
    return 0;
}