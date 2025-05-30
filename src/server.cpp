#include "server.hpp"
#include <cstring>
#include <stdexcept>
#include <unistd.h> 

DmabufServer::DmabufServer(int argc, char* argv[]) {
    parseArguments(argc, argv);
    std::cout << "RDMA DMA-buf Server\n===================\n";
    std::cout << "Port: " << port_ << "\n";
    std::cout << "Buffer size: " << buffer_size_ << " bytes\n";
    if (ib_dev_name_) std::cout << "IB device: " << *ib_dev_name_ << "\n";
    std::cout << "\n";
}

void DmabufServer::parseArguments(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            port_ = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            ib_dev_name_ = argv[++i];
        } else if (std::strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            buffer_size_ = std::strtoull(argv[++i], nullptr, 0);
        } else if (std::strcmp(argv[i], "-h") == 0) {
            std::cout << "Usage: " << argv[0] << " [-p port] [-d ib_dev] [-s buffer_size]\n";
            std::exit(0);
        }
    }
}

void DmabufServer::displayBufferData(const std::string& label, void* buffer, size_t size) const {
    if (!buffer) {
        std::cout << label << ": Data in device memory (no CPU access)\n";
        return;
    }

    int* int_data = static_cast<int*>(buffer);
    int count = size / sizeof(int);
    int display_count = count > 10 ? 10 : count;

    std::cout << label << " (first " << display_count << " of " << count << " ints): ";
    for (int i = 0; i < display_count; ++i) {
        std::cout << int_data[i] << " ";
    }
    std::cout << "...\n";
}

void DmabufServer::initializeBuffer() {
    if (hpu_.getBuffer()) {
        std::cout << "\n[CPU→HPU] Writing initial data pattern to buffer...\n";
        int* int_data = static_cast<int*>(hpu_.getBuffer());
        int count = MSG_SIZE / sizeof(int);

        for (int i = 0; i < count; ++i) {
            int_data[i] = 1000 + i; // Pattern: 1000, 1001, 1002...
        }

        displayBufferData("[CPU] Initial server data", hpu_.getBuffer(), MSG_SIZE);

        if (hpu_.getDmabufFd() < 0 && hpu_.getDeviceVa()) {
            std::cout << "[HPU] Data accessible at device VA 0x" << std::hex << hpu_.getDeviceVa() << std::dec << "\n";
        }
    } else {
        std::cout << "Note: Buffer is in device memory - would be initialized by Gaudi kernel\n";
    }
}

void DmabufServer::communicationLoop() {
    std::cout << "\nStarting communication...\n";
    for (int i = 0; i < 3; ++i) {
        std::cout << "\n--- Iteration " << (i + 1) << " ---\n";

        try {
            rdma_.postReceive();
            std::cout << "Waiting for client message...\n";
            rdma_.pollCompletion();

            if (hpu_.getBuffer()) {
                std::cout << "[HPU→CPU] Reading received data:\n";
                displayBufferData("Received from client", hpu_.getBuffer(), MSG_SIZE);

                std::cout << "[HPU] Processing data (multiplying by 2)...\n";
                int* int_data = static_cast<int*>(hpu_.getBuffer());
                int count = MSG_SIZE / sizeof(int);
                for (int j = 0; j < count && j < 256; ++j) {
                    int_data[j] *= 2;
                }

                displayBufferData("[CPU] After HPU processing", hpu_.getBuffer(), MSG_SIZE);
            } else {
                std::cout << "Received data in device memory\n";
            }

            std::cout << "Sending response...\n";
            rdma_.postSend(IBV_WR_SEND);
            rdma_.pollCompletion();
            std::cout << "✓ Response sent\n";
        } catch (const std::exception& e) {
            std::cerr << "Error in communication loop: " << e.what() << "\n";
            throw;
        }
    }
}

void DmabufServer::performRdmaWrite() {
    std::cout << "\n--- RDMA Write Test ---\n";
    if (hpu_.getBuffer()) {
        std::cout << "[CPU→HPU] Preparing RDMA Write data...\n";
        int* int_data = static_cast<int*>(hpu_.getBuffer());
        for (int i = 0; i < 10; ++i) {
            int_data[i] = 9000 + i; // Pattern: 9000, 9001, 9002...
        }
        displayBufferData("[CPU] RDMA Write data", hpu_.getBuffer(), MSG_SIZE);
    }

    std::cout << "Performing RDMA Write to client...\n";
    try {
        rdma_.postSend(IBV_WR_RDMA_WRITE);
        rdma_.pollCompletion();
        std::cout << "✓ RDMA Write completed\n";
    } catch (const std::exception& e) {
        std::cerr << "RDMA write failed: " << e.what() << "\n";
        throw;
    }
}

void DmabufServer::waitForClientFinish() {
    std::cout << "\nWaiting for client to finish...\n";
    char sync_byte;
    if (read(rdma_.getSock(), &sync_byte, 1) == 1) {
        std::cout << "✓ Client finished\n";
    }
}

void DmabufServer::run() {
    try {
        std::cout << "Initializing Gaudi DMA-buf...\n";
        hpu_.initialize(buffer_size_);
        if (hpu_.getDmabufFd() >= 0) {
            std::cout << "✓ Gaudi DMA-buf allocated (fd=" << hpu_.getDmabufFd() << ", va=0x" << std::hex << hpu_.getDeviceVa() << std::dec << ")\n";
        } else {
            std::cout << "✓ Using regular memory buffer\n";
        }

        std::cout << "\nInitializing RDMA resources...\n";
        rdma_.initialize(ib_dev_name_.value_or(""), hpu_);
        std::cout << "✓ RDMA resources initialized\n";

        std::cout << "\nWaiting for client connection on port " << port_ << "...\n";
        rdma_.connectQp("", port_);
        std::cout << "✓ Client connected\n";

        initializeBuffer();
        communicationLoop();
        performRdmaWrite();
        waitForClientFinish();

        std::cout << "\n=== Summary ===\n";
        if (hpu_.getDmabufFd() >= 0) {
            std::cout << "✅ Zero-copy RDMA using Gaudi DMA-buf\n";
            std::cout << "   - Gaudi device memory: 0x" << std::hex << hpu_.getDeviceVa() << std::dec << "\n";
            std::cout << "   - DMA-buf fd: " << hpu_.getDmabufFd() << "\n";
            std::cout << "   - Direct device-to-network transfers\n";
        } else {
            std::cout << "✅ RDMA using regular memory\n";
            std::cout << "   - Host buffer: " << hpu_.getBuffer() << "\n";
        }
        std::cout << "\n📊 Operations Summary:\n";
        std::cout << "   ✓ Send/Receive: 3 iterations completed\n";
        std::cout << "   ✓ RDMA Write: Successfully pushed data to client\n";
        std::cout << "\n💡 Note: RDMA Read operations are typically not supported\n";
        std::cout << "   with device memory due to DMA initiator requirements.\n";
        std::cout << "   Use RDMA Write to push data or Send/Receive for bidirectional.\n";
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        throw;
    }

    std::cout << "\nServer shutdown complete\n";
}

int main(int argc, char* argv[]) {
    try {
        DmabufServer server(argc, argv);
        server.run();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Server failed: " << e.what() << "\n";
        return 1;
    }
}