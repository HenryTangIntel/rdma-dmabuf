#!/usr/bin/bash

# Basic server start
# sudo ./server -d mlx5_0 -g 0

# Server with Intel Gaudi support (auto-detect)
# sudo ./server -d mlx5_0 -g 0 -G 0

# Server with Intel Gaudi forced
# sudo ./server -d mlx5_0 -g 0 -G 1

# Server without Intel Gaudi (force disabled)
# sudo ./server -d mlx5_0 -g 0 -G -1

# Server with custom buffer size (4MB)
# sudo ./server -d mlx5_0 -g 0 -s 4194304

# Default: auto-detect Gaudi support
sudo ./server -d mlx5_0 -g 0