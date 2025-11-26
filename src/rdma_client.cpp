#include "rdma_client.h"
#include "rdma_common.h"
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <chrono>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>

int setup_client_connection(RDMAConnection& conn, const TestConfig& config, const std::string& server_address, int& sockfd) {
    // Initialize RDMA device (opens device, queries port/GID)
    if (init_rdma_device(conn, config.device_name, config.port) != 0) {
        return -1;
    }

    // Allocate protection domain (needed before creating QP, but buffers will be allocated later)
    conn.protection_domain = ibv_alloc_pd(conn.context);
    if (!conn.protection_domain) {
        std::cerr << "Could not allocate protection domain" << std::endl;
        ibv_close_device(conn.context);
        return -1;
    }

    std::cout << "Client RDMA device initialized. Connecting to server at " << server_address << std::endl;
    
    // Connect to server via TCP socket
    sockfd = connect_to_server(server_address, config.tcp_port);
    if (sockfd < 0) {
        ibv_dealloc_pd(conn.protection_domain);
        ibv_close_device(conn.context);
        return -1;
    }
    
    // Validate that buffer_size is divisible by chunk_size
    if (config.buffer_size % config.chunk_size != 0) {
        std::cerr << "Error: buffer_size (" << config.buffer_size 
                  << ") must be divisible by chunk_size (" << config.chunk_size << ")" << std::endl;
        return -1;
    }
    
    // Prepare test parameters
    TestParams test_params;
    test_params.buffer_size = config.buffer_size;
    test_params.chunk_size = config.chunk_size;
    test_params.num_in_flight = config.num_in_flight;
    
    // Exchange test parameters and QP information (socket stays open)
    if (exchange_test_params_and_qp_info_client(sockfd, conn, test_params) != 0) {
        close(sockfd);
        ibv_dealloc_pd(conn.protection_domain);
        ibv_close_device(conn.context);
        return -1;
    }
    
    // Socket remains open for sending completion signal
    return 0;
}

int connect_to_server(const std::string& server_address, uint16_t port) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        std::cerr << "Failed to create socket" << std::endl;
        return -1;
    }
    
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    
    // Try to resolve hostname or use as IP address
    if (inet_pton(AF_INET, server_address.c_str(), &server_addr.sin_addr) <= 0) {
        // Try hostname resolution
        struct hostent* he = gethostbyname(server_address.c_str());
        if (he == nullptr) {
            std::cerr << "Failed to resolve server address: " << server_address << std::endl;
            close(sockfd);
            return -1;
        }
        memcpy(&server_addr.sin_addr, he->h_addr_list[0], he->h_length);
    }
    
    if (connect(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "Failed to connect to server at " << server_address 
                  << ":" << port << std::endl;
        close(sockfd);
        return -1;
    }
    
    std::cout << "Connected to server at " << server_address << ":" << port << std::endl;
    return sockfd;
}

double measure_latency(RDMAConnection& conn, uint32_t buffer_size, uint32_t chunk_size, uint32_t num_iterations) {
    std::cout << "Measuring latency with buffer size: " << buffer_size 
              << " bytes, chunk size: " << chunk_size 
              << " bytes, iterations: " << num_iterations << std::endl;

    auto start = std::chrono::high_resolution_clock::now();
    
    for (uint32_t i = 0; i < num_iterations; i++) {
        // Send the entire buffer as a single chunk for latency measurement
        if (post_send_chunk(conn, 0, buffer_size) != 0) {
            std::cerr << "Failed to post send" << std::endl;
            return -1.0;
        }

        // Wait for send completion
        int num_completions = 0;
        while (num_completions == 0) {
            num_completions = poll_send_completions(conn, 1);
            if (num_completions < 0) {
                return -1.0;
            }
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    
    double avg_latency_us = (duration / (double)num_iterations) / 1000.0;
    return avg_latency_us;
}


double measure_bandwidth(RDMAConnection& conn, uint32_t buffer_size, uint32_t chunk_size, uint32_t num_iterations) {
    std::cout << "Measuring bandwidth with buffer size: " << buffer_size 
              << " bytes, chunk size: " << chunk_size 
              << " bytes, iterations: " << num_iterations 
              << ", in-flight: " << conn.num_in_flight << std::endl;

    auto start = std::chrono::high_resolution_clock::now();
    
    // Calculate number of chunks per iteration
    uint32_t chunks_per_iteration = buffer_size / chunk_size;
    uint32_t total_chunks = chunks_per_iteration * num_iterations;
    
    uint32_t chunks_sent = 0;
    uint32_t chunks_completed = 0;
    
    // Keep sending chunks until we've completed all iterations
    while (chunks_completed < total_chunks) {
        // Post as many sends as we can (up to num_in_flight)
        while (chunks_sent < total_chunks && 
               (chunks_sent - chunks_completed) < conn.num_in_flight) {
            // Calculate which chunk within the current iteration
            uint32_t chunk_in_iteration = chunks_sent % chunks_per_iteration;
            uint32_t chunk_offset = chunk_in_iteration * chunk_size;
            
            if (post_send_chunk(conn, chunk_offset, chunk_size) != 0) {
                std::cerr << "Failed to post send" << std::endl;
                return -1.0;
            }
            chunks_sent++;
        }
        
        // Poll for send completions (all completions are sends)
        int num_completions = poll_send_completions(conn, total_chunks - chunks_completed);
        if (num_completions < 0) {
            return -1.0;
        }
        chunks_completed += num_completions;
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    
    double total_bytes = (double)buffer_size * num_iterations;
    double duration_seconds = duration / 1e9;
    double bandwidth_gbps = (total_bytes * 8) / (duration_seconds * 1e9);

    std::cout << "Duration: " << std::fixed << std::setprecision(3) << duration_seconds << " seconds" << std::endl;
    
    return bandwidth_gbps;
}

int run_client(const TestConfig& config, const std::string& server_address) {
    RDMAConnection conn = {};
    int sockfd = -1;
    
    if (setup_client_connection(conn, config, server_address, sockfd) != 0) {
        return -1;
    }

    // Run performance tests
    if (config.measure_latency) {
        double latency = measure_latency(conn, config.buffer_size, config.chunk_size, config.num_iterations);
        if (latency > 0) {
            std::cout << "Average latency: " << latency << " microseconds" << std::endl;
        }
    }

    if (config.measure_bandwidth) {
        double bandwidth = measure_bandwidth(conn, config.buffer_size, config.chunk_size, config.num_iterations);
        if (bandwidth > 0) {
            std::cout << "Bandwidth: " << bandwidth << " Gbps" << std::endl;
        }
    }

    // Send test completion signal to server
    char test_done = 0;
    if (send(sockfd, &test_done, 1, 0) != 1) {
        std::cerr << "Failed to send test completion signal" << std::endl;
    } else {
        std::cout << "Test completed, signal sent to server" << std::endl;
    }
    
    close(sockfd);
    cleanup_rdma_connection(conn);
    return 0;
}

