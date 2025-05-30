#ifndef HPU_VERBS_HPP
#define HPU_VERBS_HPP

#include "hpuverbs.hpp"
#include <string>
#include <optional>

class DmabufServer {
public:
    DmabufServer(int argc, char* argv[]);
    void run();

private:
    void parseArguments(int argc, char* argv[]);
    void displayBufferData(const std::string& label, void* buffer, size_t size) const;
    void initializeBuffer();
    void communicationLoop();
    void performRdmaWrite();
    void waitForClientFinish();

    int port_{20000};
    std::optional<std::string> ib_dev_name_;
    size_t buffer_size_{RDMA_BUFFER_SIZE};
    HpuManager hpu_;
    RdmaVerbs rdma_;
};

#endif // RDMA_DMABUF_SERVER_HPP