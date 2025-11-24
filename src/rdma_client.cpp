#include "rdma_client.h"
#include "rdma_common.h"
#include <iostream>
#include <chrono>
#include <cstring>

int setup_client_connection(RDMAConnection& conn, const TestConfig& config, const std::string& server_address) {
    // Initialize RDMA device
    if (init_rdma_device(conn, config.device_name, config.port) != 0) {
        return -1;
    }

    // Register memory
    uint32_t buffer_size = config.message_size * 2;
    if (register_memory(conn, buffer_size) != 0) {
        cleanup_rdma_connection(conn);
        return -1;
    }

    // Create queue pair
    if (create_queue_pair(conn) != 0) {
        cleanup_rdma_connection(conn);
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
    // In a real implementation, you would exchange QP information via sockets here

    return 0;
}

double measure_latency(RDMAConnection& conn, uint32_t message_size, uint32_t num_iterations) {
    std::cout << "Measuring latency with message size: " << message_size 
              << " bytes, iterations: " << num_iterations << std::endl;

    auto start = std::chrono::high_resolution_clock::now();
    
    for (uint32_t i = 0; i < num_iterations; i++) {
        // Post send work request
        struct ibv_sge sge;
        sge.addr = (uintptr_t)conn.buffer;
        sge.length = message_size;
        sge.lkey = conn.memory_region->lkey;

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

double measure_bandwidth(RDMAConnection& conn, uint32_t message_size, uint32_t num_iterations) {
    std::cout << "Measuring bandwidth with message size: " << message_size 
              << " bytes, iterations: " << num_iterations << std::endl;

    auto start = std::chrono::high_resolution_clock::now();
    
    for (uint32_t i = 0; i < num_iterations; i++) {
        // Post send work request
        struct ibv_sge sge;
        sge.addr = (uintptr_t)conn.buffer;
        sge.length = message_size;
        sge.lkey = conn.memory_region->lkey;

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

