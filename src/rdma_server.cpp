#include "rdma_server.h"
#include "rdma_common.h"
#include <iostream>
#include <chrono>
#include <thread>

int setup_server_connection(RDMAConnection& conn, const TestConfig& config) {
    // Initialize RDMA device
    if (init_rdma_device(conn, config.device_name, config.port) != 0) {
        return -1;
    }

    // Register memory
    uint32_t buffer_size = config.message_size * 2; // Extra space for testing
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

    std::cout << "Server ready. LID: " << conn.local_lid 
              << ", QP number: " << conn.queue_pair->qp_num << std::endl;
    std::cout << "Waiting for client connection..." << std::endl;

    return 0;
}

int run_server(const TestConfig& config) {
    RDMAConnection conn = {};
    
    if (setup_server_connection(conn, config) != 0) {
        return -1;
    }

    // In a real implementation, you would exchange QP information via sockets
    // For now, this is a basic structure
    std::cout << "Server is running. Press Ctrl+C to exit." << std::endl;
    
    // Keep server running
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        // Poll for completions
        poll_completion(conn);
    }

    cleanup_rdma_connection(conn);
    return 0;
}

