#ifndef RDMA_DMABUF_COMMON_HPP
#define RDMA_DMABUF_COMMON_HPP

#include <unistd.h> 
#include <cstdint>
#include <string>
#include <optional>
#include <vector>
#include <iostream>
#include <stdexcept>
#include <sys/mman.h>
#include <netdb.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <infiniband/verbs.h>
#include "hlthunk.h"

constexpr size_t MSG_SIZE = 1024;
constexpr size_t RDMA_BUFFER_SIZE = 4 * 1024 * 1024; // 4MB default

// Connection information exchanged between client and server
struct CmConData {
    uint64_t addr;      // Buffer address
    uint32_t rkey;      // Remote key
    uint32_t qp_num;    // Queue pair number
    uint16_t lid;       // Local ID
    uint8_t gid[16];    // Global ID
} __attribute__((packed));

// HPU (Gaudi) management class
class HpuManager {
public:
    HpuManager();
    ~HpuManager();

    // Initialize Gaudi device and allocate DMA-buf or fallback to regular memory
    void initialize(size_t size);
    
    // Getters for buffer information
    void* getBuffer() const { return buffer_; }
    int getDmabufFd() const { return dmabuf_fd_; }
    uint64_t getDeviceVa() const { return device_va_; }
    size_t getBufferSize() const { return buffer_size_; }

private:
    void cleanup();
    bool tryOpenGaudiDevice();
    bool allocateDeviceMemory(size_t size);
    bool mapDeviceMemory();
    bool exportDmabuf();
    bool allocateHostMemory(size_t size);
    bool mapHostMemoryToGaudi();

    int gaudi_fd_{-1};
    int dmabuf_fd_{-1};
    uint64_t gaudi_handle_{0};
    uint64_t device_va_{0};
    uint64_t host_device_va_{0};
    void* buffer_{nullptr};
    size_t buffer_size_{0};
    hlthunk_hw_ip_info hw_info_{};
};

// RDMA verbs management class
class RdmaVerbs {
public:
    RdmaVerbs();
    ~RdmaVerbs();

    // Initialize RDMA resources
    void initialize(const std::string& ib_dev_name, HpuManager& hpu);

    // Connect queue pair
    void connectQp(const std::string& server_name, int port);

    // Post send and receive operations
    void postSend(int opcode);
    void postReceive();
    
    // Poll for completion
    bool pollCompletion();

    // Getters for socket and remote properties
    int getSock() const { return sock_; }

private:
    void cleanup();
    bool initializeDevice(const std::string& ib_dev_name);
    bool setupResources(HpuManager& hpu);
    bool setupSocket(const std::string& server_name, int port);
    bool exchangeConnectionData();
    bool modifyQpToInit();
    bool modifyQpToRtr();
    bool modifyQpToRts();

    struct ibv_context* ib_ctx_{nullptr};
    struct ibv_pd* pd_{nullptr};
    struct ibv_mr* mr_{nullptr};
    struct ibv_cq* cq_{nullptr};
    struct ibv_qp* qp_{nullptr};
    struct ibv_port_attr port_attr_{};
    CmConData remote_props_{};
    int sock_{-1};
    HpuManager* hpu_{nullptr};
};

// Helper functions
inline uint64_t htonll(uint64_t val) { return htobe64(val); }
inline uint64_t ntohll(uint64_t val) { return be64toh(val); }

#endif // RDMA_DMABUF_COMMON_HPP