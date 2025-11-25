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
    uint32_t buffer_size = config.message_size * 2;
    if (create_protection_domain_resources(conn, buffer_size) != 0) {
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
        // Post send work request
        struct ibv_sge sge;
        sge.addr = (uintptr_t)conn.buffers[0];
        sge.length = message_size;
        sge.lkey = conn.memory_region[0]->lkey;

        struct ibv_send_wr wr;
        memset(&wr, 0, sizeof(wr));
        wr.sg_list = &sge;
        wr.num_sge = 1;
        wr.opcode = IBV_WR_SEND;
        wr.send_flags = IBV_SEND_SIGNALED;

        struct ibv_send_wr* bad_wr;
        if (ibv_post_send(conn.queue_pair, &wr, &bad_wr)) {
            std::cerr << "Failed to post send" << std::endl;
            return -1.0;
        }

        // Wait for completion
        struct ibv_wc wc;
        int num_completions = 0;
        while (num_completions == 0) {
            num_completions = ibv_poll_cq(conn.completion_queue, 1, &wc);
            if (num_completions < 0) {
                std::cerr << "Poll CQ failed" << std::endl;
                return -1.0;
            }
        }

        if (wc.status != IBV_WC_SUCCESS) {
            std::cerr << "Work completion error: " << ibv_wc_status_str(wc.status) << std::endl;
            return -1.0;
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    
    double avg_latency_us = (duration / (double)num_iterations) / 1000.0;
    return avg_latency_us;
}

int post_send_work_request(RDMAConnection& conn, uint32_t message_size, int buffer_index) {
    struct ibv_sge sge;
    sge.addr = (uintptr_t)conn.buffers[buffer_index];
    sge.length = message_size;
    sge.lkey = conn.memory_region[buffer_index]->lkey;

    struct ibv_send_wr wr;
    memset(&wr, 0, sizeof(wr));
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_SEND;
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.wr_id = buffer_index;
    wr.next = nullptr;

    struct ibv_send_wr* bad_wr;
    if (ibv_post_send(conn.queue_pair, &wr, &bad_wr)) {
        std::cerr << "Failed to post send" << std::endl;
        return -1;
    }

    return 0;
}

double measure_bandwidth(RDMAConnection& conn, uint32_t message_size, uint32_t num_iterations, int num_messages) {
    std::cout << "Measuring bandwidth with message size: " << message_size 
              << " bytes, iterations: " << num_iterations << std::endl;

    auto start = std::chrono::high_resolution_clock::now();
    
    for (uint32_t i = 0; i < num_iterations; i++) {
        int sent_count = 0;
        // Post send work request
        for (int j = 0; j < std::min(num_messages, NUM_BUFFERS); j++) {
            if (post_send_work_request(conn, message_size, j) != 0) {
                std::cerr << "Failed to post send" << std::endl;
                return -1.0;
            }
            sent_count++;
        }

        struct ibv_wc wc[NUM_BUFFERS];
        int total_completions = 0;
        while (total_completions < num_messages) {
            // Wait for completion
            int num_completions = 0;
            num_completions = ibv_poll_cq(conn.completion_queue, NUM_BUFFERS, wc);
            if (num_completions < 0) {
                std::cerr << "Poll CQ failed" << std::endl;
                return -1.0;
            }
            for (int k = 0; k < num_completions; k++) {
                if (wc[k].status != IBV_WC_SUCCESS) {
                    std::cerr << "Work completion error: " << ibv_wc_status_str(wc[k].status) << std::endl;
                    return -1.0;
                }
                ++total_completions;
                if (sent_count < num_messages) {
                    if (post_send_work_request(conn, message_size, wc[k].wr_id) != 0) {
                        std::cerr << "Failed to post send" << std::endl;
                        return -1.0;
                    }
                    ++sent_count;
                }
            }
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    
    double total_bytes = (double)message_size * num_iterations * num_messages;
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
        double bandwidth = measure_bandwidth(conn, config.message_size, config.num_iterations, config.num_messages);
        if (bandwidth > 0) {
            std::cout << "Bandwidth: " << bandwidth << " Gbps" << std::endl;
        }
    }

    cleanup_rdma_connection(conn);
    return 0;
}

