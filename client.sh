#!/usr/bin/bash

# Basic client connect
# sudo ./client -d mlx5_0 -g 0 172.26.47.90

# Client with Intel Gaudi support (auto-detect)
# sudo ./client -d mlx5_0 -g 0 -G 0 172.26.47.90

# Client with Intel Gaudi forced
# sudo ./client -d mlx5_0 -g 0 -G 1 172.26.47.90

# Client without Intel Gaudi (force disabled)
# sudo ./client -d mlx5_0 -g 0 -G -1 172.26.47.90

# Client with custom buffer size (4MB)
# sudo ./client -d mlx5_0 -g 0 -s 4194304 172.26.47.90

# Default: auto-detect Gaudi support
sudo ./client -d mlx5_0 -g 0 172.26.47.90