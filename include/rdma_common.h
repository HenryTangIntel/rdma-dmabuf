// rdma_dmabuf_common.h
#ifndef RDMA_DMABUF_COMMON_H
#define RDMA_DMABUF_COMMON_H


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <netdb.h>
#include <infiniband/verbs.h>
#include "hlthunk.h"

#define MSG_SIZE 1024
#define RDMA_BUFFER_SIZE (4 * 1024 * 1024)  // 4MB default

// Connection information exchanged between client and server
struct cm_con_data_t {
    uint64_t addr;      // Buffer address
    uint32_t rkey;      // Remote key
    uint32_t qp_num;    // Queue pair number
    uint16_t lid;       // Local ID
    uint8_t gid[16];    // Global ID
} __attribute__((packed));

// RDMA resources
typedef struct {
    // Gaudi resources
    int gaudi_fd;
    int dmabuf_fd;
    uint64_t gaudi_handle;
    uint64_t device_va;
    struct hlthunk_hw_ip_info hw_info;
    
    // IB resources
    struct ibv_context *ib_ctx;
    struct ibv_pd *pd;
    struct ibv_mr *mr;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_port_attr port_attr;
    
    // Connection info
    struct cm_con_data_t remote_props;
    int sock;
    
    // Buffer info
    size_t buffer_size;
    void *buffer;  // For CPU access if available
    uint64_t host_device_va;  // Host buffer mapped to Gaudi
} rdma_context_t;

// Function declarations
int init_gaudi_dmabuf(rdma_context_t *ctx, size_t size);
int init_rdma_resources(rdma_context_t *ctx, const char *ib_dev_name);
int connect_qp(rdma_context_t *ctx, const char *server_name, int port);
int post_send(rdma_context_t *ctx, int opcode);
int post_receive(rdma_context_t *ctx);
int poll_completion(rdma_context_t *ctx);
void cleanup_resources(rdma_context_t *ctx);
void simulate_hpu_operation(rdma_context_t *ctx, const char *operation);

// Helper functions
static inline uint64_t htonll(uint64_t val) {
    return htobe64(val);
}

static inline uint64_t ntohll(uint64_t val) {
    return be64toh(val);
}

#endif // RDMA_DMABUF_COMMON_H
