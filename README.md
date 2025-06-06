# rdma-gdr(dmabuf)

A client-server implementation for DMA buffer operations using RDMA verbs on Habana Gaudi hardware.

## Overview

This project demonstrates how to use RDMA (Remote Direct Memory Access) verbs with DMA buffers on Habana Gaudi hardware. It consists of a client and server implementation that establish a connection and perform memory operations directly between devices.

## Summary:
This implementation has successfully achieved:

✅ Zero-copy data transfer between Gaudi and NIC  
✅ Direct DMA-buf registration without CPU mapping  
✅ Bidirectional communication using Send/Receive  
✅ One-sided RDMA Write operations  
⚠️  RDMA Read needs further investigation

## RDMA DMA-buf Data Flow Overview

This document explains the data flow for the RDMA DMA-buf implementation that enables zero-copy data transfer between Intel Gaudi accelerators over RoCE/InfiniBand networks.

## System Architecture

The system demonstrates zero-copy RDMA between Gaudi accelerators using DMA-buf. The implementation allows direct device-to-device data transfers without CPU involvement.

## Data Flow Phases

### 1. Initialization Phase

Both client and server perform the following initialization steps:

```
1. Open Gaudi device (Gaudi2/Gaudi2)
2. Allocate device memory on Gaudi HBM
3. Export memory as DMA-buf file descriptor
4. Register DMA-buf with InfiniBand NIC
5. Establish RDMA connection over TCP
```

### 2. Memory Allocation Flow

The memory allocation process follows this sequence:

```c
init_gaudi_dmabuf()
  ├─ hlthunk_device_memory_alloc()    // Allocate on Gaudi HBM
  ├─ hlthunk_device_memory_map()      // Get device VA (e.g., 0x1001001800000000)
  ├─ hlthunk_export_dmabuf_fd()       // Create DMA-buf (fd=3)
  └─ mmap()                           // Fails (normal - no CPU access to device memory)
```

**Key Points:**
- Memory is allocated in Gaudi's High Bandwidth Memory (HBM)
- Device Virtual Address (VA) is obtained for RDMA operations
- DMA-buf file descriptor enables sharing with other devices
- mmap() failure is expected - CPU cannot access device memory

### 3. RDMA Registration Flow

The RDMA subsystem registration process:

```c
init_rdma_resources()
  ├─ ibv_alloc_pd()        // Create protection domain
  ├─ ibv_create_cq()       // Create completion queue
  ├─ ibv_reg_dmabuf_mr()   // Register DMA-buf with NIC
  │   └─ NIC gets permission to DMA from Gaudi memory
  └─ ibv_create_qp()       // Create queue pair for communication
```

### 4. Connection Establishment

The connection setup uses TCP for initial handshake:

```
TCP Socket Exchange (Client ←→ Server):
  - Exchange device Virtual Addresses (VAs)
  - Exchange remote keys (rkeys)
  - Exchange Queue Pair (QP) numbers
  - Exchange Local IDs (LIDs) and Global IDs (GIDs)
  - Transition QPs through states: INIT → RTR → RTS
```

### 5. Data Transfer Flow (Send/Receive)

#### Client Side Operations:
```

1. Data resides in Gaudi memory @ 0x1001001800000000                   
2. post_send() → NIC initiates DMA read from Gaudi
3. NIC → Network → Remote NIC
4. poll_completion() → Wait for send completion
```

#### Server Side Operations:
```
1. post_receive() → Prepare buffer for incoming data
2. Remote NIC → DMA write to Gaudi @ 0x1001001800000000
3. poll_completion() → Data arrival notification
4. Process data (via Gaudi kernel - no CPU access)
5. Send response back to client
```

### 6. RDMA Write Flow (One-sided Operation)

Server performs direct write to client memory:

```
Server → Client:
1. Server: Data prepared in Gaudi memory
2. Server: post_send(IBV_WR_RDMA_WRITE)
3. Server NIC: DMA read from local Gaudi
4. Network transfer
5. Client NIC: DMA write to client Gaudi @ 0x1001001800000000                   
6. No client CPU/software involvement (true one-sided)
```

### 7. Zero-Copy Data Path

The complete hardware data path:

```
Gaudi2 HBM → PCIe → NIC → Network → NIC → PCIe → Gaudi2 HBM
     ↑                                                    ↑
     |                                                    |
0x1001001800000000                              0x1001001800000000                   
```

## Key Implementation Details

### Why "DMA-buf mmap failed" is Normal

1. **Hardware Architecture:**
   - Gaudi device memory is physically located on the accelerator card
   - Memory is optimized for Gaudi/accelerator access patterns
   - No direct CPU load/store path exists to device memory

2. **Cannot inspect data without special tools:**
   - Need Gaudi kernels to process/verify data
   - Must DMA copy to host memory for debugging
   - Production systems process data in-place on device


## Prerequisites

- Habana Gaudi hardware
- RDMA-capable network infrastructure
- Ubuntu Linux (tested on Ubuntu 22.04 LTS)
- CMake (3.22+)
- C++ compiler with C++11 support
- Habana SynapseAI software stack
- RDMA verbs libraries (`libibverbs`, `tcp socket` or `librdmacm`)

## Building the Project

```bash
# Create build directory (if it doesn't exist)
mkdir -p build

# Navigate to build directory
cd build

# Generate a release build with CMake
cmake ..

# OR Generate a debug build with CMake
cmake .. -DCMAKE_BUILD_TYPE=Debug

# Build the project
make
```

## Usage

### Running the Server

```bash
./build/rdma_server [options]
```

### Running the Client

```bash
./build/rdma_client [server-address] [options]
```

## Project Structure

- `include/` - Header files
- `src/` - Source files

## License

This project is licensed under the MIT License.  
See the [LICENSE](LICENSE) file for details.

## Contributing

Contributions are welcome! Please open issues or pull requests for bug fixes, improvements, or new features.

To contribute:
1. Fork the repository
2. Create a new branch for your feature or fix
3. Commit your changes with clear messages
4. Open a pull request describing your changes

For major changes, please open an issue first to discuss what you would like to change.

## Contact

For questions, suggestions, or support, please open an issue on GitHub or contact the maintainer at:  
**Email:** [henry.yu.tang@gmail.com]  
**GitHub:** [https://github.com/HenryTangIntel/rdma-dmabuf](https://github.com/HenryTangIntel/rdma-dmabuf)




