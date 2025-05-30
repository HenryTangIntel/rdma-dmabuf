#ifndef RDMA_DMABUF_CLIENT_HPP
#define RDMA_DMABUF_CLIENT_HPP

#include "hpuverbs.hpp"
#include <string>
#include <optional>

class DmabufClient {
public:
    DmabufClient(int argc, char* argv[]);
    void run();

private:
    void parseArguments(int argc, char* argv[]);
    void displayBufferData(const std::string& label, void* buffer, size_t size) const;
    void initializeBuffer(int iteration);
    void communicationLoop();
    void performRdmaRead();
    void signalServerDone();

    std::string server_name_;
    int port_{20000};
    std::optional<std::string> ib_dev_name_;
    size_t buffer_size_{RDMA_BUFFER_SIZE};
    HpuManager hpu_;
    RdmaVerbs rdma_;
};

#endif // RDMA_DMABUF_CLIENT_HPP