// rdma_common.h
#ifndef RDMA_COMMON_H
#define RDMA_COMMON_H

#include <assert.h>
#include <byteswap.h>
#include <endian.h>
#include <errno.h>
#include <getopt.h>
#include <infiniband/verbs.h>
#include <inttypes.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <linux/dma-heap.h>
#include <sys/ioctl.h>
#include <stdbool.h>

#define HAVE_HLTHUNK

// Include hlthunk for Intel Gaudi support
#ifdef HAVE_HLTHUNK
#include "hlthunk.h"
#endif

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

#define MAX_POLL_CQ_TIMEOUT 2000
#define MSG "This is alice, how are you?"
#define RDMAMSGR "RDMA read operation"
#define RDMAMSGW "RDMA write operation"
#define MSG_SIZE (strlen(MSG) + 1)
#define DMA_HEAP_PATH "/dev/dma_heap/system"
#define BUFFER_SIZE ((MSG_SIZE + 4095) & ~4095) // Round up MSG_SIZE to page size (4KB)

#if __BYTE_ORDER == __LITTLE_ENDIAN
static inline uint64_t htonll(uint64_t x) { return bswap_64(x); }
static inline uint64_t ntohll(uint64_t x) { return bswap_64(x); }
#elif __BYTE_ORDER == __BIG_ENDIAN
static inline uint64_t htonll(uint64_t x) { return x; }
static inline uint64_t ntohll(uint64_t x) { return x; }
#else
#error __BYTE_ORDER is not defined
#endif

#define ERROR(fmt, args...)                                                    \
    { fprintf(stderr, "ERROR: %s(): " fmt, __func__, ##args); }
#define ERR_DIE(fmt, args...)                                                  \
    {                                                                          \
        ERROR(fmt, ##args);                                                    \
        exit(EXIT_FAILURE);                                                \
    }
#define INFO(fmt, args...)                                                     \
    { printf("INFO: %s(): " fmt, __func__, ##args); }
#define WARN(fmt, args...)                                                     \
    { printf("WARN: %s(): " fmt, __func__, ##args); }

#define CHECK(expr)                                                            \
    {                                                                          \
        int rc = (expr);                                                       \
        if (rc != 0) {                                                         \
            perror(strerror(errno));                                           \
            exit(EXIT_FAILURE);                                                \
        }                                                                      \
    }

enum buffer_type {
    BUFFER_TYPE_MALLOC,
    BUFFER_TYPE_DMA_HEAP,
    BUFFER_TYPE_GAUDI
};

struct config_t {
    const char *dev_name;
    char *server_name;
    uint32_t tcp_port;
    int ib_port;
    int gid_idx;
    int use_gaudi;  // 0 = auto, 1 = force gaudi, -1 = disable gaudi
    size_t buffer_size;  // Allow custom buffer size
};

struct cm_con_data_t {
    uint64_t addr;
    uint32_t rkey;
    uint32_t qp_num;
    uint16_t lid;
    uint8_t gid[16];
} __attribute__((packed));

#ifdef HAVE_HLTHUNK
struct gaudi_context {
    int gaudi_fd;
    uint64_t gaudi_handle;
    uint64_t device_va;
    uint64_t host_device_va;
    struct hlthunk_hw_ip_info hw_info;
};
#endif

struct resources {
    struct ibv_device_attr device_attr;
    struct ibv_port_attr port_attr;
    struct cm_con_data_t remote_props;
    struct ibv_context *ib_ctx;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_mr *mr;
    char *buf;
    int sock;
    int dma_fd; // DMA-BUF file descriptor
    enum buffer_type buf_type;
    size_t buf_size;
#ifdef HAVE_HLTHUNK
    struct gaudi_context gaudi;
#endif
};

extern struct config_t config;

// Function declarations
int sock_connect(const char *server_name, int port);
int sock_sync_data(int sockfd, int xfer_size, char *local_data, char *remote_data);
void print_config(void);
void print_usage(const char *progname);
void resources_init(struct resources *res);
int resources_create(struct resources *res);
int resources_destroy(struct resources *res);
int connect_qp(struct resources *res);
int post_send(struct resources *res, int opcode);
int post_receive(struct resources *res);
int poll_completion(struct resources *res);
int sync_dmabuf(int dmabuf_fd, uint64_t flags);

// Gaudi-specific functions
#ifdef HAVE_HLTHUNK
int init_gaudi_device(struct resources *res);
int allocate_gaudi_dmabuf(struct resources *res, size_t size);
void cleanup_gaudi_context(struct resources *res);
#endif

#endif