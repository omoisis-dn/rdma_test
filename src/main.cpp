#include <iostream>
#include <string>
#include <cstring>
#include "rdma_client.h"
#include "rdma_server.h"

void print_usage(const char* program_name) {
    std::cout << "Usage: " << program_name << " [options]\n"
              << "Options:\n"
              << "  -s, --server              Run as server\n"
              << "  -c, --client <address>     Run as client (connect to server)\n"
              << "  -d, --device <name>        RDMA device name (default: first available)\n"
              << "  -p, --port <num>           Port number (default: 1)\n"
              << "  -m, --message-size <size>  Message size in bytes (default: 4096)\n"
              << "  -n, --iterations <num>     Number of iterations (default: 1000)\n"
              << "  -l, --latency             Measure latency\n"
              << "  -b, --bandwidth           Measure bandwidth\n"
              << "  -h, --help                Show this help message\n"
              << std::endl;
}

int main(int argc, char* argv[]) {
    TestConfig config = {};
    config.message_size = 4096;
    config.num_iterations = 1000;
    config.measure_latency = false;
    config.measure_bandwidth = false;
    config.device_name = "";
    config.port = 1;

    bool is_server = false;
    bool is_client = false;
    std::string server_address;

    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--server") == 0) {
            is_server = true;
        } else if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--client") == 0) {
            is_client = true;
            if (i + 1 < argc) {
                server_address = argv[++i];
            } else {
                std::cerr << "Error: --client requires an address" << std::endl;
                print_usage(argv[0]);
                return 1;
            }
        } else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--device") == 0) {
            if (i + 1 < argc) {
                config.device_name = argv[++i];
            } else {
                std::cerr << "Error: --device requires a device name" << std::endl;
                print_usage(argv[0]);
                return 1;
            }
        } else if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0) {
            if (i + 1 < argc) {
                config.port = std::stoi(argv[++i]);
            } else {
                std::cerr << "Error: --port requires a port number" << std::endl;
                print_usage(argv[0]);
                return 1;
            }
        } else if (strcmp(argv[i], "-m") == 0 || strcmp(argv[i], "--message-size") == 0) {
            if (i + 1 < argc) {
                config.message_size = std::stoul(argv[++i]);
            } else {
                std::cerr << "Error: --message-size requires a size" << std::endl;
                print_usage(argv[0]);
                return 1;
            }
        } else if (strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--iterations") == 0) {
            if (i + 1 < argc) {
                config.num_iterations = std::stoul(argv[++i]);
            } else {
                std::cerr << "Error: --iterations requires a number" << std::endl;
                print_usage(argv[0]);
                return 1;
            }
        } else if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--latency") == 0) {
            config.measure_latency = true;
        } else if (strcmp(argv[i], "-b") == 0 || strcmp(argv[i], "--bandwidth") == 0) {
            config.measure_bandwidth = true;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown option: " << argv[i] << std::endl;
            print_usage(argv[0]);
            return 1;
        }
    }

    // Default to measuring both if neither is specified
    if (!config.measure_latency && !config.measure_bandwidth) {
        config.measure_latency = true;
        config.measure_bandwidth = true;
    }

    // Run as server or client
    if (is_server) {
        std::cout << "Starting RDMA performance test server..." << std::endl;
        return run_server(config);
    } else if (is_client) {
        std::cout << "Starting RDMA performance test client..." << std::endl;
        return run_client(config, server_address);
    } else {
        std::cerr << "Error: Must specify either --server or --client" << std::endl;
        print_usage(argv[0]);
        return 1;
    }

    return 0;
}

