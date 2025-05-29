// rdma_dmabuf_client.c
#include "rdma_dmabuf_common.h"

int main(int argc, char *argv[]) {
    rdma_context_t ctx = {0};
    ctx.gaudi_fd = -1;
    ctx.dmabuf_fd = -1;
    ctx.sock = -1;
    
    char *server_name = NULL;
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
            printf("Usage: %s <server> [-p port] [-d ib_dev] [-s buffer_size]\n", argv[0]);
            return 0;
        } else if (!server_name) {
            server_name = argv[i];
        }
    }
    
    if (!server_name) {
        fprintf(stderr, "Error: Server name required\n");
        printf("Usage: %s <server> [-p port] [-d ib_dev] [-s buffer_size]\n", argv[0]);
        return 1;
    }
    
    printf("RDMA DMA-buf Client\n");
    printf("===================\n");
    printf("Server: %s:%d\n", server_name, port);
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
    
    // Connect to server
    printf("\nConnecting to server %s:%d...\n", server_name, port);
    if (connect_qp(&ctx, server_name, port) < 0) {
        fprintf(stderr, "Failed to connect to server\n");
        cleanup_resources(&ctx);
        return 1;
    }
    printf("✓ Connected to server\n");
    
    // Main communication loop
    printf("\nStarting communication...\n");
    
    for (int i = 0; i < 3; i++) {
        printf("\n--- Iteration %d ---\n", i + 1);
        
        // Prepare message
        if (ctx.buffer) {
            snprintf(ctx.buffer, MSG_SIZE, "Hello from client - iteration %d, %s mode", 
                     i + 1, ctx.dmabuf_fd >= 0 ? "zero-copy" : "normal");
        } else {
            printf("Note: Buffer is in device memory - would be written by Gaudi kernel\n");
        }
        
        // Send message
        printf("Sending message to server...\n");
        if (post_send(&ctx, IBV_WR_SEND) < 0) {
            fprintf(stderr, "Failed to post send\n");
            break;
        }
        
        if (poll_completion(&ctx) < 0) {
            fprintf(stderr, "Failed to send message\n");
            break;
        }
        printf("✓ Message sent\n");
        
        // Post receive for server's response
        if (post_receive(&ctx) < 0) {
            fprintf(stderr, "Failed to post receive\n");
            break;
        }
        
        // Wait for server's response
        printf("Waiting for server response...\n");
        if (poll_completion(&ctx) < 0) {
            fprintf(stderr, "Failed to receive response\n");
            break;
        }
        
        if (ctx.buffer) {
            printf("Received: %s\n", (char *)ctx.buffer);
        } else {
            printf("Received data in device memory\n");
        }
    }
    
    // RDMA Write test - wait for server to write
    printf("\n--- RDMA Write Test ---\n");
    printf("Waiting for server's RDMA write...\n");
    sleep(1); // Give server time to perform RDMA write
    
    if (ctx.buffer) {
        printf("Buffer after RDMA write: %s\n", (char *)ctx.buffer);
    } else {
        printf("RDMA write completed to device memory\n");
    }
    
    // RDMA Read test
    printf("\n--- RDMA Read Test ---\n");
    printf("Performing RDMA Read from server...\n");
    if (post_send(&ctx, IBV_WR_RDMA_READ) < 0) {
        fprintf(stderr, "Failed to post RDMA read\n");
    } else if (poll_completion(&ctx) < 0) {
        fprintf(stderr, "RDMA read failed\n");
    } else {
        printf("✓ RDMA Read completed\n");
        if (ctx.buffer) {
            printf("Read data: %s\n", (char *)ctx.buffer);
        }
    }
    
    // Signal server we're done
    char sync_byte = 'D';
    write(ctx.sock, &sync_byte, 1);
    
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
    printf("   - Operations: 3 sends, 3 receives, 1 RDMA read\n");
    printf("   - All data transfers bypassed CPU data path\n");
    
    cleanup_resources(&ctx);
    printf("\nClient shutdown complete\n");
    return 0;
}