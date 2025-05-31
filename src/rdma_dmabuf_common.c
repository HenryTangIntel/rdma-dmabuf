// rdma_dmabuf_common.c
#include "rdma_dmabuf_common.h"

// Initialize Gaudi device and allocate DMA-buf
int init_gaudi_dmabuf(rdma_context_t *ctx, size_t size) {
    ctx->buffer_size = size;
    ctx->host_device_va = 0;  // Initialize to 0
    
    // Try to open Gaudi device
    enum hlthunk_device_name devices[] = {
        HLTHUNK_DEVICE_GAUDI3,
        HLTHUNK_DEVICE_GAUDI2,
        HLTHUNK_DEVICE_GAUDI,
        HLTHUNK_DEVICE_DONT_CARE
    };
    
    for (int i = 0; i < 4; i++) {
        ctx->gaudi_fd = hlthunk_open(devices[i], NULL);
        if (ctx->gaudi_fd >= 0) break;
    }
    
    if (ctx->gaudi_fd < 0) {
        printf("No Gaudi device found, using regular memory\n");
        ctx->buffer = aligned_alloc(4096, size);
        if (!ctx->buffer) return -1;
        memset(ctx->buffer, 0, size);
        ctx->dmabuf_fd = -1;
        ctx->host_device_va = 0;  // No Gaudi mapping possible
        return 0;
    }
    
    // Get hardware info
    if (hlthunk_get_hw_ip_info(ctx->gaudi_fd, &ctx->hw_info) != 0) {
        hlthunk_close(ctx->gaudi_fd);
        ctx->gaudi_fd = -1;
        return -1;
    }
    
    printf("Gaudi device opened successfully\n");
    
    // Check if we should force host memory mode (for testing)
    if (getenv("FORCE_HOST_MEMORY")) {
        printf("FORCE_HOST_MEMORY set - using host-mapped memory for CPU visibility\n");
        ctx->gaudi_handle = 0;
        ctx->device_va = 0;
        ctx->dmabuf_fd = -1;
        goto use_host_memory;
    }
    
    // Allocate device memory
    ctx->gaudi_handle = hlthunk_device_memory_alloc(ctx->gaudi_fd, size, 0, true, true);
    if (ctx->gaudi_handle == 0) {
        printf("Failed to allocate Gaudi memory, using regular memory\n");
        goto use_host_memory;
    }
    
    // Map device memory
    ctx->device_va = hlthunk_device_memory_map(ctx->gaudi_fd, ctx->gaudi_handle, 0);
    if (ctx->device_va == 0) {
        hlthunk_device_memory_free(ctx->gaudi_fd, ctx->gaudi_handle);
        hlthunk_close(ctx->gaudi_fd);
        return -1;
    }
    
    // Export as DMA-buf
    ctx->dmabuf_fd = hlthunk_device_mapped_memory_export_dmabuf_fd(
        ctx->gaudi_fd, ctx->device_va, size, 0, (O_RDWR | O_CLOEXEC));
    
    if (ctx->dmabuf_fd < 0) {
        printf("DMA-buf export failed, creating host-mapped buffer\n");
use_host_memory:
        // Clean up any device allocations if we're falling back
        if (ctx->gaudi_handle) {
            if (ctx->device_va) {
                hlthunk_memory_unmap(ctx->gaudi_fd, ctx->device_va);
                ctx->device_va = 0;
            }
            hlthunk_device_memory_free(ctx->gaudi_fd, ctx->gaudi_handle);
            ctx->gaudi_handle = 0;
        }
        
        // Fallback: allocate host memory and map it to Gaudi
        ctx->buffer = aligned_alloc(4096, size);
        if (!ctx->buffer) {
            hlthunk_memory_unmap(ctx->gaudi_fd, ctx->device_va);
            hlthunk_device_memory_free(ctx->gaudi_fd, ctx->gaudi_handle);
            return -1;
        }
        memset(ctx->buffer, 0, size);
        
        // Map host buffer to Gaudi's address space for CPU-HPU data transfer
        ctx->host_device_va = hlthunk_host_memory_map(ctx->gaudi_fd, ctx->buffer, 0, size);
        if (ctx->host_device_va) {
            printf("Host buffer mapped to Gaudi at 0x%lx\n", ctx->host_device_va);
            printf("CPU can now read/write data that HPU can access\n");
        } else {
            printf("Host memory mapping to Gaudi failed, but buffer still usable\n");
        }
    } else {
        printf("DMA-buf created successfully (fd=%d)\n", ctx->dmabuf_fd);
        // For DMA-buf case, we might still want CPU access for debugging
        // Try to mmap the DMA-buf
        ctx->buffer = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, ctx->dmabuf_fd, 0);
        if (ctx->buffer == MAP_FAILED) {
            ctx->buffer = NULL;
            printf("DMA-buf mmap failed - CPU access not available (this is normal)\n");
        } else {
            printf("DMA-buf mapped to CPU address %p\n", ctx->buffer);
        }
    }
    
    return 0;
}

// Initialize RDMA resources
int init_rdma_resources(rdma_context_t *ctx, const char *ib_dev_name) {
    struct ibv_device **dev_list;
    struct ibv_device *ib_dev = NULL;
    int num_devices, i;
    
    // Get device list
    dev_list = ibv_get_device_list(&num_devices);
    if (!dev_list || num_devices == 0) {
        fprintf(stderr, "No IB devices found\n");
        return -1;
    }
    
    // Find requested device or use first
    for (i = 0; i < num_devices; i++) {
        if (!ib_dev_name || strcmp(ibv_get_device_name(dev_list[i]), ib_dev_name) == 0) {
            ib_dev = dev_list[i];
            break;
        }
    }
    
    if (!ib_dev) {
        fprintf(stderr, "IB device not found\n");
        ibv_free_device_list(dev_list);
        return -1;
    }
    
    // Open device
    ctx->ib_ctx = ibv_open_device(ib_dev);
    if (!ctx->ib_ctx) {
        fprintf(stderr, "Failed to open IB device\n");
        ibv_free_device_list(dev_list);
        return -1;
    }
    
    printf("Opened IB device: %s\n", ibv_get_device_name(ib_dev));
    
    // Query port
    if (ibv_query_port(ctx->ib_ctx, 1, &ctx->port_attr)) {
        fprintf(stderr, "Failed to query port\n");
        goto cleanup;
    }
    
    // Allocate PD
    ctx->pd = ibv_alloc_pd(ctx->ib_ctx);
    if (!ctx->pd) {
        fprintf(stderr, "Failed to allocate PD\n");
        goto cleanup;
    }
    
    // Create CQ
    ctx->cq = ibv_create_cq(ctx->ib_ctx, 10, NULL, NULL, 0);
    if (!ctx->cq) {
        fprintf(stderr, "Failed to create CQ\n");
        goto cleanup;
    }
    
    // Register memory - ensure all access flags are set
    int mr_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | 
                   IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_ATOMIC;
    
    if (ctx->dmabuf_fd >= 0) {
        // Try direct DMA-buf registration
        ctx->mr = ibv_reg_dmabuf_mr(ctx->pd, 0, ctx->buffer_size, 
                                    (uint64_t)ctx->device_va, ctx->dmabuf_fd, mr_flags);
        if (ctx->mr) {
            printf("DMA-buf registered successfully with IB\n");
        } else {
            printf("DMA-buf registration failed, trying fallback\n");
        }
    }
    
    if (!ctx->mr && ctx->buffer) {
        // Register regular memory
        ctx->mr = ibv_reg_mr(ctx->pd, ctx->buffer, ctx->buffer_size, mr_flags);
        if (!ctx->mr) {
            fprintf(stderr, "Failed to register memory\n");
            goto cleanup;
        }
        printf("Regular memory registered with IB\n");
    }
    
    if (!ctx->mr) {
        fprintf(stderr, "No memory could be registered\n");
        goto cleanup;
    }
    
    // Create QP
    struct ibv_qp_init_attr qp_init_attr = {
        .qp_type = IBV_QPT_RC,
        .sq_sig_all = 1,
        .send_cq = ctx->cq,
        .recv_cq = ctx->cq,
        .cap = {
            .max_send_wr = 10,
            .max_recv_wr = 10,
            .max_send_sge = 1,
            .max_recv_sge = 1
        }
    };
    
    ctx->qp = ibv_create_qp(ctx->pd, &qp_init_attr);
    if (!ctx->qp) {
        fprintf(stderr, "Failed to create QP\n");
        goto cleanup;
    }
    
    ibv_free_device_list(dev_list);
    return 0;
    
cleanup:
    if (ctx->mr) ibv_dereg_mr(ctx->mr);
    if (ctx->cq) ibv_destroy_cq(ctx->cq);
    if (ctx->pd) ibv_dealloc_pd(ctx->pd);
    if (ctx->ib_ctx) ibv_close_device(ctx->ib_ctx);
    ibv_free_device_list(dev_list);
    return -1;
}

// Socket operations for connection establishment
static int sock_connect(const char *server_name, int port) {
    struct addrinfo hints = {0}, *res;
    char port_str[6];
    int sockfd;
    
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (!server_name) hints.ai_flags = AI_PASSIVE;
    
    sprintf(port_str, "%d", port);
    if (getaddrinfo(server_name, port_str, &hints, &res)) {
        return -1;
    }
    
    sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sockfd < 0) {
        freeaddrinfo(res);
        return -1;
    }
    
    if (server_name) {
        // Client: connect
        if (connect(sockfd, res->ai_addr, res->ai_addrlen)) {
            close(sockfd);
            freeaddrinfo(res);
            return -1;
        }
    } else {
        // Server: bind and listen
        int reuse = 1;
        setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        
        if (bind(sockfd, res->ai_addr, res->ai_addrlen)) {
            close(sockfd);
            freeaddrinfo(res);
            return -1;
        }
        
        listen(sockfd, 1);
        int client_fd = accept(sockfd, NULL, NULL);
        close(sockfd);
        sockfd = client_fd;
    }
    
    freeaddrinfo(res);
    return sockfd;
}

static int sock_sync_data(int sock, size_t size, void *local_data, void *remote_data) {
    if (write(sock, local_data, size) != size) return -1;
    if (read(sock, remote_data, size) != size) return -1;
    return 0;
}

// Modify QP state machine
static int modify_qp_to_init(struct ibv_qp *qp) {
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_INIT,
        .port_num = 1,
        .pkey_index = 0,
        .qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | 
                          IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_ATOMIC
    };
    return ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);
}

static int modify_qp_to_rtr(struct ibv_qp *qp, uint32_t remote_qpn, uint16_t dlid, uint8_t *dgid) {
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_RTR,
        .path_mtu = IBV_MTU_4096,
        .dest_qp_num = remote_qpn,
        .rq_psn = 0,
        .max_dest_rd_atomic = 1,
        .min_rnr_timer = 12,
        .ah_attr = {
            .is_global = 0,
            .dlid = dlid,
            .sl = 0,
            .src_path_bits = 0,
            .port_num = 1
        }
    };
    
    if (dgid && memcmp(dgid, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 16)) {
        attr.ah_attr.is_global = 1;
        memcpy(&attr.ah_attr.grh.dgid, dgid, 16);
        attr.ah_attr.grh.sgid_index = 0;
        attr.ah_attr.grh.hop_limit = 1;
    }
    
    return ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                         IBV_QP_DEST_QPN | IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | 
                         IBV_QP_MIN_RNR_TIMER);
}

static int modify_qp_to_rts(struct ibv_qp *qp) {
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_RTS,
        .timeout = 14,
        .retry_cnt = 7,
        .rnr_retry = 7,
        .sq_psn = 0,
        .max_rd_atomic = 1
    };
    return ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                         IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC);
}

// Connect QP
int connect_qp(rdma_context_t *ctx, const char *server_name, int port) {
    struct cm_con_data_t local_con_data = {0}, remote_con_data = {0};
    union ibv_gid my_gid = {0};
    char temp_char;
    
    // Connect socket
    ctx->sock = sock_connect(server_name, port);
    if (ctx->sock < 0) {
        fprintf(stderr, "Failed to establish TCP connection\n");
        return -1;
    }
    
    // Get local GID if using RoCE
    if (ctx->port_attr.link_layer == IBV_LINK_LAYER_ETHERNET) {
        ibv_query_gid(ctx->ib_ctx, 1, 0, &my_gid);
    }
    
    // Prepare local connection data
    if (ctx->dmabuf_fd >= 0) {
        local_con_data.addr = htonll(ctx->device_va);
    } else {
        local_con_data.addr = htonll((uintptr_t)ctx->buffer);
    }
    local_con_data.rkey = htonl(ctx->mr->rkey);
    local_con_data.qp_num = htonl(ctx->qp->qp_num);
    local_con_data.lid = htons(ctx->port_attr.lid);
    memcpy(local_con_data.gid, &my_gid, 16);
    
    // Exchange connection data
    if (sock_sync_data(ctx->sock, sizeof(struct cm_con_data_t), 
                       &local_con_data, &remote_con_data)) {
        fprintf(stderr, "Failed to exchange connection data\n");
        return -1;
    }
    
    // Save remote properties
    ctx->remote_props.addr = ntohll(remote_con_data.addr);
    ctx->remote_props.rkey = ntohl(remote_con_data.rkey);
    ctx->remote_props.qp_num = ntohl(remote_con_data.qp_num);
    ctx->remote_props.lid = ntohs(remote_con_data.lid);
    memcpy(ctx->remote_props.gid, remote_con_data.gid, 16);
    
    // Modify QP states
    if (modify_qp_to_init(ctx->qp)) {
        fprintf(stderr, "Failed to modify QP to INIT\n");
        return -1;
    }
    
    if (modify_qp_to_rtr(ctx->qp, ctx->remote_props.qp_num, 
                         ctx->remote_props.lid, ctx->remote_props.gid)) {
        fprintf(stderr, "Failed to modify QP to RTR\n");
        return -1;
    }
    
    if (modify_qp_to_rts(ctx->qp)) {
        fprintf(stderr, "Failed to modify QP to RTS\n");
        return -1;
    }
    
    // Sync before starting
    if (sock_sync_data(ctx->sock, 1, "Q", &temp_char)) {
        fprintf(stderr, "Sync error\n");
        return -1;
    }
    
    return 0;
}

// Post send operation
int post_send(rdma_context_t *ctx, int opcode) {
    struct ibv_sge sge = {
        .addr = ctx->dmabuf_fd >= 0 ? ctx->device_va : (uintptr_t)ctx->buffer,
        .length = MSG_SIZE,
        .lkey = ctx->mr->lkey
    };
    
    struct ibv_send_wr sr = {
        .wr_id = 0,
        .sg_list = &sge,
        .num_sge = 1,
        .opcode = opcode,
        .send_flags = IBV_SEND_SIGNALED,
    };
    
    if (opcode != IBV_WR_SEND) {
        sr.wr.rdma.remote_addr = ctx->remote_props.addr;
        sr.wr.rdma.rkey = ctx->remote_props.rkey;
    }
    
    struct ibv_send_wr *bad_wr;
    return ibv_post_send(ctx->qp, &sr, &bad_wr);
}

// Post receive operation
int post_receive(rdma_context_t *ctx) {
    struct ibv_sge sge = {
        .addr = ctx->dmabuf_fd >= 0 ? ctx->device_va : (uintptr_t)ctx->buffer,
        .length = MSG_SIZE,
        .lkey = ctx->mr->lkey
    };
    
    struct ibv_recv_wr rr = {
        .wr_id = 0,
        .sg_list = &sge,
        .num_sge = 1,
    };
    
    struct ibv_recv_wr *bad_wr;
    return ibv_post_recv(ctx->qp, &rr, &bad_wr);
}

// Poll for completion
int poll_completion(rdma_context_t *ctx) {
    struct ibv_wc wc;
    int polls = 0;
    
    while (polls++ < 1000000) {
        int ne = ibv_poll_cq(ctx->cq, 1, &wc);
        if (ne < 0) {
            fprintf(stderr, "Poll CQ failed\n");
            return -1;
        }
        if (ne > 0) {
            if (wc.status != IBV_WC_SUCCESS) {
                fprintf(stderr, "Work completion error: %s\n", ibv_wc_status_str(wc.status));
                return -1;
            }
            return 0;
        }
        usleep(1);
    }
    
    fprintf(stderr, "Poll timeout\n");
    return -1;
}

// Cleanup resources
void cleanup_resources(rdma_context_t *ctx) {
    if (ctx->qp) ibv_destroy_qp(ctx->qp);
    if (ctx->mr) ibv_dereg_mr(ctx->mr);
    if (ctx->cq) ibv_destroy_cq(ctx->cq);
    if (ctx->pd) ibv_dealloc_pd(ctx->pd);
    if (ctx->ib_ctx) ibv_close_device(ctx->ib_ctx);
    
    if (ctx->dmabuf_fd >= 0) {
        close(ctx->dmabuf_fd);
    }
    
    if (ctx->buffer && ctx->dmabuf_fd < 0) {
        // Unmap from Gaudi if it was mapped
        if (ctx->host_device_va && ctx->gaudi_fd >= 0) {
            hlthunk_memory_unmap(ctx->gaudi_fd, ctx->host_device_va);
        }
        free(ctx->buffer);
    } else if (ctx->buffer) {
        // Unmap DMA-buf mmap
        munmap(ctx->buffer, ctx->buffer_size);
    }
    
    if (ctx->gaudi_handle) {
        if (ctx->device_va) {
            hlthunk_memory_unmap(ctx->gaudi_fd, ctx->device_va);
        }
        hlthunk_device_memory_free(ctx->gaudi_fd, ctx->gaudi_handle);
    } else if (ctx->buffer) {
        free(ctx->buffer);
    }
    
    if (ctx->gaudi_fd >= 0) {
        hlthunk_close(ctx->gaudi_fd);
    }
    
    if (ctx->sock >= 0) {
        close(ctx->sock);
    }
}

// Simulate HPU operations on the buffer
void simulate_hpu_operation(rdma_context_t *ctx, const char *operation) {
    if (!ctx->buffer) {
        printf("[HPU] Operation '%s' would be performed by HPU kernel on device memory\n", operation);
        return;
    }
    
    printf("[HPU] Simulating HPU operation: %s\n", operation);
    int *int_data = (int *)ctx->buffer;
    int count = MSG_SIZE / sizeof(int);
    
    if (strcmp(operation, "square") == 0) {
        // Square each value
        for (int i = 0; i < count && i < 256; i++) {
            int_data[i] = int_data[i] * int_data[i];
        }
    } else if (strcmp(operation, "increment") == 0) {
        // Increment each value by 1000
        for (int i = 0; i < count && i < 256; i++) {
            int_data[i] += 1000;
        }
    } else if (strcmp(operation, "reverse") == 0) {
        // Reverse the order of elements
        for (int i = 0; i < count/2 && i < 128; i++) {
            int temp = int_data[i];
            int_data[i] = int_data[count - 1 - i];
            int_data[count - 1 - i] = temp;
        }
    }
    
    printf("[HPU] Operation '%s' completed\n", operation);
}

// Write test pattern to buffer for verification
int write_test_pattern(rdma_context_t *ctx, int pattern_base) {
    if (!ctx->buffer) {
        printf("[TEST] Cannot write pattern - no CPU-accessible buffer\n");
        printf("      Data would be written by Gaudi kernel to device memory\n");
        return -1;
    }
    
    printf("[TEST] Writing test pattern (base=%d) to buffer...\n", pattern_base);
    
    int *data = (int *)ctx->buffer;
    int count = MSG_SIZE / sizeof(int);
    
    // Write recognizable pattern
    for (int i = 0; i < count && i < 256; i++) {
        data[i] = pattern_base + i;
    }
    
    // Display first few values
    printf("[TEST] Written: ");
    for (int i = 0; i < 10 && i < count; i++) {
        printf("%d ", data[i]);
    }
    printf("...\n");
    
    if (ctx->host_device_va) {
        printf("[TEST] Pattern written to host buffer, accessible by Gaudi at VA 0x%lx\n", 
               ctx->host_device_va);
    }
    
    return 0;
}

// Verify test pattern in buffer
int verify_test_pattern(rdma_context_t *ctx, int expected_base) {
    if (!ctx->buffer) {
        printf("[TEST] Cannot verify pattern - no CPU-accessible buffer\n");
        printf("      Verification would be done by Gaudi kernel\n");
        return -1;
    }
    
    printf("[TEST] Verifying test pattern (expected base=%d)...\n", expected_base);
    
    int *data = (int *)ctx->buffer;
    int count = MSG_SIZE / sizeof(int);
    int errors = 0;
    
    // Display first few values
    printf("[TEST] Received: ");
    for (int i = 0; i < 10 && i < count; i++) {
        printf("%d ", data[i]);
    }
    printf("...\n");
    
    // Verify pattern
    for (int i = 0; i < count && i < 256; i++) {
        int expected = expected_base + i;
        if (data[i] != expected) {
            if (errors < 5) {  // Report first few errors
                printf("[TEST] Error at index %d: expected %d, got %d\n", 
                       i, expected, data[i]);
            }
            errors++;
        }
    }
    
    if (errors == 0) {
        printf("[TEST] ✅ Pattern verification PASSED! All %d values correct.\n", 
               (count < 256 ? count : 256));
        return 0;
    } else {
        printf("[TEST] ❌ Pattern verification FAILED! %d errors found.\n", errors);
        return -1;
    }
}

// Debug function to read device memory for inspection
int debug_read_device_memory(rdma_context_t *ctx, size_t offset, size_t count) {
    if (!ctx->gaudi_fd || !ctx->device_va) {
        printf("[DEBUG] No device memory to inspect\n");
        return -1;
    }
    
    if (ctx->buffer) {
        // If we have CPU access, just read directly
        printf("[DEBUG] Reading from CPU-accessible buffer at offset %zu:\n", offset);
        int *int_data = (int *)((char *)ctx->buffer + offset);
        printf("Data (first %zu ints): ", count);
        for (size_t i = 0; i < count && i < 10; i++) {
            printf("%d ", int_data[i]);
        }
        printf("\n");
        return 0;
    }
    
    // For device-only memory, we need to DMA it to host
    printf("[DEBUG] Device memory inspection at VA 0x%lx + offset %zu\n", ctx->device_va, offset);
    
    // Allocate temporary host buffer
    size_t copy_size = count * sizeof(int);
    void *temp_buffer = aligned_alloc(4096, copy_size);
    if (!temp_buffer) {
        printf("[DEBUG] Failed to allocate temp buffer\n");
        return -1;
    }
    
    // Map to device
    uint64_t temp_va = hlthunk_host_memory_map(ctx->gaudi_fd, temp_buffer, 0, copy_size);
    if (!temp_va) {
        printf("[DEBUG] Failed to map temp buffer to device\n");
        free(temp_buffer);
        return -1;
    }
    
    printf("[DEBUG] Temporary buffer mapped at device VA 0x%lx\n", temp_va);
    printf("[DEBUG] To inspect device memory, you would need to:\n");
    printf("  1. Submit DMA: src=0x%lx → dst=0x%lx, size=%zu\n", 
           ctx->device_va + offset, temp_va, copy_size);
    printf("  2. Execute DMA engine copy\n");
    printf("  3. Read from temp buffer\n");
    
    // In production, you would submit a DMA command here
    // For now, show the concept
    
    printf("\n[DEBUG] Device memory layout:\n");
    printf("  - DMA-buf fd: %d\n", ctx->dmabuf_fd);
    printf("  - Device VA: 0x%lx\n", ctx->device_va);
    printf("  - Size: %zu bytes\n", ctx->buffer_size);
    printf("  - Data is in Gaudi DRAM, inaccessible to CPU directly\n");
    
    // Cleanup
    hlthunk_memory_unmap(ctx->gaudi_fd, temp_va);
    free(temp_buffer);
    
    return 0;
}