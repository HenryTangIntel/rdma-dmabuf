#include "hpuverbs.hpp"

HpuManager::HpuManager() = default;

HpuManager::~HpuManager() {
    cleanup();
}

void HpuManager::initialize(size_t size) {
    buffer_size_ = size;

    if (!tryOpenGaudiDevice()) {
        std::cout << "No Gaudi device found, using regular memory\n";
        if (!allocateHostMemory(size)) {
            throw std::runtime_error("Failed to allocate host memory");
        }
        return;
    }

    if (hlthunk_get_hw_ip_info(gaudi_fd_, &hw_info_) != 0) {
        hlthunk_close(gaudi_fd_);
        gaudi_fd_ = -1;
        throw std::runtime_error("Failed to get Gaudi hardware info");
    }

    std::cout << "Gaudi device opened successfully\n";

    if (!allocateDeviceMemory(size)) {
        std::cout << "Failed to allocate Gaudi memory, using regular memory\n";
        cleanup();
        if (!allocateHostMemory(size)) {
            throw std::runtime_error("Failed to allocate host memory");
        }
        return;
    }

    if (!mapDeviceMemory()) {
        cleanup();
        throw std::runtime_error("Failed to map device memory");
    }

    if (!exportDmabuf()) {
        std::cout << "DMA-buf export failed, creating host-mapped buffer\n";
        if (!allocateHostMemory(size)) {
            cleanup();
            throw std::runtime_error("Failed to allocate host memory");
        }
        if (mapHostMemoryToGaudi()) {
            std::cout << "Host buffer mapped to Gaudi at 0x" << std::hex << host_device_va_ << std::dec << "\n";
            std::cout << "CPU can now read/write data that HPU can access\n";
        } else {
            std::cout << "Host memory mapping to Gaudi failed, but buffer still usable\n";
        }
    } else {
        std::cout << "DMA-buf created successfully (fd=" << dmabuf_fd_ << ")\n";
        buffer_ = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, dmabuf_fd_, 0);
        if (buffer_ == MAP_FAILED) {
            buffer_ = nullptr;
            std::cout << "DMA-buf mmap failed - CPU access not available (this is normal)\n";
        } else {
            std::cout << "DMA-buf mapped to CPU address " << buffer_ << "\n";
        }
    }
}

bool HpuManager::tryOpenGaudiDevice() {
    static const hlthunk_device_name devices[] = {
        HLTHUNK_DEVICE_GAUDI3,
        HLTHUNK_DEVICE_GAUDI2,
        HLTHUNK_DEVICE_GAUDI,
        HLTHUNK_DEVICE_DONT_CARE
    };

    for (const auto& device : devices) {
        gaudi_fd_ = hlthunk_open(device, nullptr);
        if (gaudi_fd_ >= 0) return true;
    }
    return false;
}

bool HpuManager::allocateDeviceMemory(size_t size) {
    gaudi_handle_ = hlthunk_device_memory_alloc(gaudi_fd_, size, 0, true, true);
    return gaudi_handle_ != 0;
}

bool HpuManager::mapDeviceMemory() {
    device_va_ = hlthunk_device_memory_map(gaudi_fd_, gaudi_handle_, 0);
    return device_va_ != 0;
}

bool HpuManager::exportDmabuf() {
    dmabuf_fd_ = hlthunk_device_mapped_memory_export_dmabuf_fd(
        gaudi_fd_, device_va_, buffer_size_, 0, (O_RDWR | O_CLOEXEC));
    return dmabuf_fd_ >= 0;
}

bool HpuManager::allocateHostMemory(size_t size) {
    buffer_ = aligned_alloc(4096, size);
    if (!buffer_) return false;
    memset(buffer_, 0, size);
    return true;
}

bool HpuManager::mapHostMemoryToGaudi() {
    host_device_va_ = hlthunk_host_memory_map(gaudi_fd_, buffer_, 0, buffer_size_);
    return host_device_va_ != 0;
}

void HpuManager::cleanup() {
    if (dmabuf_fd_ >= 0) {
        close(dmabuf_fd_);
        dmabuf_fd_ = -1;
    }

    if (buffer_ && dmabuf_fd_ < 0) {
        if (host_device_va_ && gaudi_fd_ >= 0) {
            hlthunk_memory_unmap(gaudi_fd_, host_device_va_);
        }
        free(buffer_);
    } else if (buffer_) {
        munmap(buffer_, buffer_size_);
    }
    buffer_ = nullptr;

    if (gaudi_handle_) {
        if (device_va_) {
            hlthunk_memory_unmap(gaudi_fd_, device_va_);
        }
        hlthunk_device_memory_free(gaudi_fd_, gaudi_handle_);
        gaudi_handle_ = 0;
        device_va_ = 0;
    }

    if (gaudi_fd_ >= 0) {
        hlthunk_close(gaudi_fd_);
        gaudi_fd_ = -1;
    }
}

RdmaVerbs::RdmaVerbs() = default;

RdmaVerbs::~RdmaVerbs() {
    cleanup();
}

void RdmaVerbs::initialize(const std::string& ib_dev_name, HpuManager& hpu) {
    hpu_ = &hpu;
    if (!initializeDevice(ib_dev_name)) {
        throw std::runtime_error("Failed to initialize IB device");
    }
    if (!setupResources(hpu)) {
        throw std::runtime_error("Failed to setup RDMA resources");
    }
}

void RdmaVerbs::connectQp(const std::string& server_name, int port) {
    if (!setupSocket(server_name, port)) {
        throw std::runtime_error("Failed to establish TCP connection");
    }
    if (!exchangeConnectionData()) {
        throw std::runtime_error("Failed to exchange connection data");
    }
    if (!modifyQpToInit()) {
        throw std::runtime_error("Failed to modify QP to INIT");
    }
    if (!modifyQpToRtr()) {
        throw std::runtime_error("Failed to modify QP to RTR");
    }
    if (!modifyQpToRts()) {
        throw std::runtime_error("Failed to modify QP to RTS");
    }
}

void RdmaVerbs::postSend(int opcode) {
    struct ibv_sge sge = {
        .addr = hpu_->getDmabufFd() >= 0 ? hpu_->getDeviceVa() : reinterpret_cast<uintptr_t>(hpu_->getBuffer()),
        .length = MSG_SIZE,
        .lkey = mr_->lkey
    };

    struct ibv_send_wr sr = {
        .wr_id = 0,
        .sg_list = &sge,
        .num_sge = 1,
        .opcode = static_cast<ibv_wr_opcode>(opcode),
        .send_flags = IBV_SEND_SIGNALED,
    };

    if (opcode != IBV_WR_SEND) {
        sr.wr.rdma.remote_addr = remote_props_.addr;
        sr.wr.rdma.rkey = remote_props_.rkey;
    }

    struct ibv_send_wr* bad_wr;
    if (ibv_post_send(qp_, &sr, &bad_wr)) {
        throw std::runtime_error("Failed to post send");
    }
}

void RdmaVerbs::postReceive() {
    struct ibv_sge sge = {
        .addr = hpu_->getDmabufFd() >= 0 ? hpu_->getDeviceVa() : reinterpret_cast<uintptr_t>(hpu_->getBuffer()),
        .length = MSG_SIZE,
        .lkey = mr_->lkey
    };

    struct ibv_recv_wr rr = {
        .wr_id = 0,
        .sg_list = &sge,
        .num_sge = 1,
    };

    struct ibv_recv_wr* bad_wr;
    if (ibv_post_recv(qp_, &rr, &bad_wr)) {
        throw std::runtime_error("Failed to post receive");
    }
}

bool RdmaVerbs::pollCompletion() {
    struct ibv_wc wc;
    int polls = 0;

    while (polls++ < 1000000) {
        int ne = ibv_poll_cq(cq_, 1, &wc);
        if (ne < 0) {
            throw std::runtime_error("Poll CQ failed");
        }
        if (ne > 0) {
            if (wc.status != IBV_WC_SUCCESS) {
                throw std::runtime_error("Work completion error: " + std::string(ibv_wc_status_str(wc.status)));
            }
            return true;
        }
        usleep(1);
    }

    throw std::runtime_error("Poll timeout");
}

bool RdmaVerbs::initializeDevice(const std::string& ib_dev_name) {
    int num_devices;
    struct ibv_device** dev_list = ibv_get_device_list(&num_devices);
    if (!dev_list || num_devices == 0) {
        std::cerr << "No IB devices found\n";
        return false;
    }

    struct ibv_device* ib_dev = nullptr;
    for (int i = 0; i < num_devices; ++i) {
        if (ib_dev_name.empty() || ib_dev_name == ibv_get_device_name(dev_list[i])) {
            ib_dev = dev_list[i];
            break;
        }
    }

    if (!ib_dev) {
        std::cerr << "IB device not found\n";
        ibv_free_device_list(dev_list);
        return false;
    }

    ib_ctx_ = ibv_open_device(ib_dev);
    ibv_free_device_list(dev_list);
    if (!ib_ctx_) {
        std::cerr << "Failed to open IB device\n";
        return false;
    }

    std::cout << "Opened IB device: " << ibv_get_device_name(ib_dev) << "\n";
    return true;
}

bool RdmaVerbs::setupResources(HpuManager& hpu) {
    if (ibv_query_port(ib_ctx_, 1, &port_attr_)) {
        std::cerr << "Failed to query port\n";
        return false;
    }

    pd_ = ibv_alloc_pd(ib_ctx_);
    if (!pd_) {
        std::cerr << "Failed to allocate PD\n";
        return false;
    }

    cq_ = ibv_create_cq(ib_ctx_, 10, nullptr, nullptr, 0);
    if (!cq_) {
        std::cerr << "Failed to create CQ\n";
        return false;
    }

    int mr_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | 
                   IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_ATOMIC;

    if (hpu.getDmabufFd() >= 0) {
        mr_ = ibv_reg_dmabuf_mr(pd_, 0, hpu.getBufferSize(), hpu.getDeviceVa(), hpu.getDmabufFd(), mr_flags);
        if (mr_) {
            std::cout << "DMA-buf registered successfully with IB\n";
        } else {
            std::cout << "DMA-buf registration failed, trying fallback\n";
        }
    }

    if (!mr_ && hpu.getBuffer()) {
        mr_ = ibv_reg_mr(pd_, hpu.getBuffer(), hpu.getBufferSize(), mr_flags);
        if (!mr_) {
            std::cerr << "Failed to register memory\n";
            return false;
        }
        std::cout << "Regular memory registered with IB\n";
    }

    if (!mr_) {
        std::cerr << "No memory could be registered\n";
        return false;
    }

    struct ibv_qp_init_attr qp_init_attr = {};
    qp_init_attr.send_cq = cq_;
    qp_init_attr.recv_cq = cq_;
    qp_init_attr.cap.max_send_wr = 1;
    qp_init_attr.cap.max_recv_wr = 1;
    qp_init_attr.cap.max_send_sge = 1;
    qp_init_attr.cap.max_recv_sge = 1;
    qp_init_attr.qp_type = IBV_QPT_RC;
    qp_init_attr.sq_sig_all = 1;


    qp_ = ibv_create_qp(pd_, &qp_init_attr);
    if (!qp_) {
        std::cerr << "Failed to create QP\n";
        return false;
    }

    return true;
}

bool RdmaVerbs::setupSocket(const std::string& server_name, int port) {
    struct addrinfo hints = {}, *res;
    std::string port_str = std::to_string(port);

    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (server_name.empty()) hints.ai_flags = AI_PASSIVE;

    if (getaddrinfo(server_name.empty() ? nullptr : server_name.c_str(), port_str.c_str(), &hints, &res)) {
        return false;
    }

    sock_ = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock_ < 0) {
        freeaddrinfo(res);
        return false;
    }

    if (!server_name.empty()) {
        if (connect(sock_, res->ai_addr, res->ai_addrlen)) {
            close(sock_);
            freeaddrinfo(res);
            return false;
        }
    } else {
        int reuse = 1;
        setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        if (bind(sock_, res->ai_addr, res->ai_addrlen) || listen(sock_, 1)) {
            close(sock_);
            freeaddrinfo(res);
            return false;
        }
        int client_fd = accept(sock_, nullptr, nullptr);
        close(sock_);
        sock_ = client_fd;
    }

    freeaddrinfo(res);
    return true;
}

bool RdmaVerbs::exchangeConnectionData() {
    CmConData local_con_data = {};
    union ibv_gid my_gid = {};

    if (port_attr_.link_layer == IBV_LINK_LAYER_ETHERNET) {
        ibv_query_gid(ib_ctx_, 1, 0, &my_gid);
    }

    local_con_data.addr = htonll(hpu_->getDmabufFd() >= 0 ? hpu_->getDeviceVa() : reinterpret_cast<uintptr_t>(hpu_->getBuffer()));
    local_con_data.rkey = htonl(mr_->rkey);
    local_con_data.qp_num = htonl(qp_->qp_num);
    local_con_data.lid = htons(port_attr_.lid);
    memcpy(local_con_data.gid, &my_gid, 16);

    char temp_char;
    if (write(sock_, &local_con_data, sizeof(CmConData)) != sizeof(CmConData) ||
        read(sock_, &remote_props_, sizeof(CmConData)) != sizeof(CmConData)) {
        return false;
    }

    remote_props_.addr = ntohll(remote_props_.addr);
    remote_props_.rkey = ntohl(remote_props_.rkey);
    remote_props_.qp_num = ntohl(remote_props_.qp_num);
    remote_props_.lid = ntohs(remote_props_.lid);

    if (write(sock_, "Q", 1) != 1 || read(sock_, &temp_char, 1) != 1) {
        return false;
    }

    return true;
}

bool RdmaVerbs::modifyQpToInit() {
    struct ibv_qp_attr attr = {};
    attr.qp_state = IBV_QPS_INIT;
    attr.port_num = 1;
    attr.pkey_index = 0;
    attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE |
                           IBV_ACCESS_REMOTE_READ |
                           IBV_ACCESS_REMOTE_WRITE |
                           IBV_ACCESS_REMOTE_ATOMIC;

    int flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;
    return ibv_modify_qp(qp_, &attr, flags) == 0;
}


bool RdmaVerbs::modifyQpToRtr() {
        
    struct ibv_qp_attr attr = {};
    attr.qp_state = IBV_QPS_RTR;
    attr.path_mtu = IBV_MTU_4096;
    attr.dest_qp_num = remote_props_.qp_num;
    attr.rq_psn = 0;
    attr.max_dest_rd_atomic = 1;
    attr.min_rnr_timer = 12;

    attr.ah_attr.is_global = 0;
    attr.ah_attr.dlid = remote_props_.lid;
    attr.ah_attr.sl = 0;
    attr.ah_attr.src_path_bits = 0;
    attr.ah_attr.port_num = 1;


    if (memcmp(remote_props_.gid, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 16)) {
        attr.ah_attr.is_global = 1;
        memcpy(&attr.ah_attr.grh.dgid, remote_props_.gid, 16);
        attr.ah_attr.grh.sgid_index = 0;
        attr.ah_attr.grh.hop_limit = 1;
    }

    return ibv_modify_qp(qp_, &attr, IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                         IBV_QP_DEST_QPN | IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | 
                         IBV_QP_MIN_RNR_TIMER) == 0;
}

bool RdmaVerbs::modifyQpToRts() {
    struct ibv_qp_attr attr = {};
    attr.qp_state = IBV_QPS_RTS;
    attr.timeout = 14;
    attr.retry_cnt = 7;
    attr.rnr_retry = 7;
    attr.sq_psn = 0;
    attr.max_rd_atomic = 1;

    return ibv_modify_qp(qp_, &attr, IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                         IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC) == 0;
}

void RdmaVerbs::cleanup() {
    if (qp_) {
        ibv_destroy_qp(qp_);
        qp_ = nullptr;
    }
    if (mr_) {
        ibv_dereg_mr(mr_);
        mr_ = nullptr;
    }
    if (cq_) {
        ibv_destroy_cq(cq_);
        cq_ = nullptr;
    }
    if (pd_) {
        ibv_dealloc_pd(pd_);
        pd_ = nullptr;
    }
    if (ib_ctx_) {
        ibv_close_device(ib_ctx_);
        ib_ctx_ = nullptr;
    }
    if (sock_ >= 0) {
        close(sock_);
        sock_ = -1;
    }
}