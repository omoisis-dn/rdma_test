# RDMA Performance Test

A C++ application for testing RDMA (Remote Direct Memory Access) performance on Linux using InfiniBand Verbs API.

## Requirements

- Linux operating system
- CMake 3.10 or higher
- Make
- libibverbs development libraries
- InfiniBand or RDMA-capable network adapter

### Installing Dependencies

On Ubuntu/Debian:
```bash
sudo apt-get update
sudo apt-get install cmake build-essential libibverbs-dev
```

On RHEL/CentOS/Fedora:
```bash
sudo yum install cmake gcc-c++ make libibverbs-devel
# or for newer versions:
sudo dnf install cmake gcc-c++ make libibverbs-devel
```

## Building

```bash
mkdir build
cd build
cmake ..
make
```

The executable will be created as `rdma_perf_test` in the `build` directory.

## Usage

### List Available Devices

List all available RDMA devices on the system:
```bash
./rdma_perf_test --list-devices
```

This will display detailed information about each RDMA device including:
- Device name and GUID
- Number of physical ports
- Maximum memory region size, queue pairs, and completion queues
- Port status, LID, and maximum MTU for each port

### Server Mode

Run the application as a server:
```bash
./rdma_perf_test --server
```

### Client Mode

Run the application as a client to connect to a server:
```bash
./rdma_perf_test --client <server_address> --latency --bandwidth
```

### Options

- `-L, --list-devices`: List available RDMA devices
- `-s, --server`: Run as server
- `-c, --client <address>`: Run as client (connect to server)
- `-d, --device <name>`: RDMA device name (default: first available)
- `-p, --port <num>`: Port number (default: 1)
- `-m, --message-size <size>`: Message size in bytes (default: 4096)
- `-n, --iterations <num>`: Number of iterations (default: 1000)
- `-l, --latency`: Measure latency
- `-b, --bandwidth`: Measure bandwidth
- `-h, --help`: Show help message

### Examples

```bash
# List available RDMA devices
./rdma_perf_test --list-devices

# Terminal 1: Start server
./rdma_perf_test --server --port 1

# Terminal 2: Run client with latency and bandwidth tests
./rdma_perf_test --client localhost --latency --bandwidth --message-size 8192 --iterations 10000
```

## Project Structure

```
rdma_test/
├── CMakeLists.txt          # CMake build configuration
├── README.md               # This file
├── include/                # Header files
│   ├── rdma_common.h      # Common RDMA structures and functions
│   ├── rdma_server.h      # Server-side functions
│   └── rdma_client.h      # Client-side functions
└── src/                    # Source files
    ├── main.cpp           # Main entry point
    ├── rdma_common.cpp    # Common RDMA implementation
    ├── rdma_server.cpp    # Server implementation
    └── rdma_client.cpp    # Client implementation
```

## Notes

This is a basic RDMA performance testing framework. For production use, you may want to add:
- Socket-based connection establishment for QP information exchange
- More comprehensive error handling
- Additional performance metrics
- Support for different RDMA operations (RDMA Write, RDMA Read)
- Multi-threaded testing
- Statistical analysis of results

## License

This project is provided as-is for RDMA performance testing purposes.

