#include "rdma_client.h"
#include "rdma_common.h"
#include <cstdint>
#include <iostream>
#include <chrono>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>

int setup_client_connection(RDMAConnection& conn, const TestConfig& config, const std::string& server_address) {
    // Initialize RDMA device (opens device, queries port/GID)
    if (init_rdma_device(conn, config.device_name, config.port) != 0) {
        return -1;
    }

    // Create protection domain and all resources allocated within it
    if (create_protection_domain_resources(conn, config.message_size, config.num_in_flight) != 0) {
        ibv_close_device(conn.context);
        return -1;
    }

    // Transition QP to INIT
    struct ibv_qp_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_INIT;
    attr.port_num = conn.port_num;
    attr.pkey_index = 0;
    attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
                           IBV_ACCESS_REMOTE_WRITE;

    if (ibv_modify_qp(conn.queue_pair, &attr,
                      IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS)) {
        std::cerr << "Failed to modify QP to INIT" << std::endl;
        cleanup_rdma_connection(conn);
        return -1;
    }

    std::cout << "Client initialized. Connecting to server at " << server_address << std::endl;
    
    // Connect to server via TCP socket
    int sockfd = connect_to_server(server_address, config.tcp_port);
    if (sockfd < 0) {
        cleanup_rdma_connection(conn);
        return -1;
    }
    
    // Exchange QP information
    if (exchange_qp_info_client(sockfd, conn) != 0) {
        close(sockfd);
        cleanup_rdma_connection(conn);
        return -1;
    }
    
    close(sockfd);
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

double measure_latency(RDMAConnection& conn, uint32_t message_size, uint32_t num_iterations) {
    std::cout << "Measuring latency with message size: " << message_size 
              << " bytes, iterations: " << num_iterations << std::endl;

    auto start = std::chrono::high_resolution_clock::now();
    
    for (uint32_t i = 0; i < num_iterations; i++) {
        // Use slot 0 for latency measurement (single message at a time)
        if (post_send_work_request(conn, 0) != 0) {
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


double measure_bandwidth(RDMAConnection& conn, uint32_t message_size, uint32_t num_iterations) {
    std::cout << "Measuring bandwidth with message size: " << message_size 
              << " bytes, iterations: " << num_iterations 
              << ", in-flight: " << conn.num_in_flight << std::endl;

    auto start = std::chrono::high_resolution_clock::now();
    
    uint32_t messages_sent = 0;
    uint32_t messages_completed = 0;
    
    // Keep sending messages until we've completed all iterations
    while (messages_completed < num_iterations) {
        // Post as many sends as we can (up to num_in_flight)
        while (messages_sent < num_iterations && 
               (messages_sent - messages_completed) < conn.num_in_flight) {
            uint32_t slot = messages_sent % conn.num_in_flight;
            if (post_send_work_request(conn, slot) != 0) {
                std::cerr << "Failed to post send" << std::endl;
                return -1.0;
            }
            messages_sent++;
        }
        
        // Poll for send completions (all completions are sends)
        int num_completions = poll_send_completions(conn, num_iterations - messages_completed);
        if (num_completions < 0) {
            return -1.0;
        }
        messages_completed += num_completions;
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    
    double total_bytes = (double)message_size * num_iterations;
    double duration_seconds = duration / 1e9;
    double bandwidth_gbps = (total_bytes * 8) / (duration_seconds * 1e9);
    
    return bandwidth_gbps;
}

int run_client(const TestConfig& config, const std::string& server_address) {
    RDMAConnection conn = {};
    
    if (setup_client_connection(conn, config, server_address) != 0) {
        return -1;
    }

    // Run performance tests
    if (config.measure_latency) {
        double latency = measure_latency(conn, config.message_size, config.num_iterations);
        if (latency > 0) {
            std::cout << "Average latency: " << latency << " microseconds" << std::endl;
        }
    }

    if (config.measure_bandwidth) {
        double bandwidth = measure_bandwidth(conn, config.message_size, config.num_iterations);
        if (bandwidth > 0) {
            std::cout << "Bandwidth: " << bandwidth << " Gbps" << std::endl;
        }
    }

    cleanup_rdma_connection(conn);
    return 0;
}

