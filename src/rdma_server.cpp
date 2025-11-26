#include "rdma_server.h"
#include "rdma_common.h"
#include <iostream>
#include <chrono>
#include <thread>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>

int setup_server_connection(RDMAConnection& conn, const TestConfig& config) {
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

    std::cout << "Server ready. QP number: " << conn.queue_pair->qp_num << std::endl;
    std::cout << "Waiting for client connection..." << std::endl;

    return 0;
}

int create_server_socket(uint16_t port) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        std::cerr << "Failed to create socket" << std::endl;
        return -1;
    }
    
    int opt = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        std::cerr << "Failed to set socket options" << std::endl;
        close(sockfd);
        return -1;
    }
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    
    if (bind(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "Failed to bind socket to port " << port << std::endl;
        close(sockfd);
        return -1;
    }
    
    if (listen(sockfd, 1) < 0) {
        std::cerr << "Failed to listen on socket" << std::endl;
        close(sockfd);
        return -1;
    }
    
    std::cout << "Server listening on port " << port << " for client connections..." << std::endl;
    return sockfd;
}

int serve_forever(RDMAConnection& conn) {
    // Post initial receive work requests for all in-flight slots
    for (uint32_t i = 0; i < conn.num_in_flight; i++) {
        if (post_receive_work_request(conn, i) != 0) {
            std::cerr << "Failed to post initial receive work request for slot " << i << std::endl;
            return -1;
        }
    }
    
    std::cout << "Posted " << conn.num_in_flight << " initial receive work requests" << std::endl;
    
    // Keep polling for receive completions (automatically reposts receives)
    while (true) {
        poll_receive_completions(conn);
    }
}

int run_server(const TestConfig& config) {
    RDMAConnection conn = {};
    
    if (setup_server_connection(conn, config) != 0) {
        return -1;
    }

    // Create server socket
    int server_sock = create_server_socket(config.tcp_port);
    if (server_sock < 0) {
        cleanup_rdma_connection(conn);
        return -1;
    }
    
    // Accept client connection
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &client_len);
    if (client_sock < 0) {
        std::cerr << "Failed to accept client connection" << std::endl;
        close(server_sock);
        cleanup_rdma_connection(conn);
        return -1;
    }
    
    std::cout << "Client connected from " << inet_ntoa(client_addr.sin_addr) << std::endl;
    
    // Exchange QP information
    if (exchange_qp_info_server(client_sock, conn) != 0) {
        close(client_sock);
        close(server_sock);
        cleanup_rdma_connection(conn);
        return -1;
    }
    
    close(client_sock);
    close(server_sock);
    
    std::cout << "Server is running. Press Ctrl+C to exit." << std::endl;

    serve_forever(conn);

    cleanup_rdma_connection(conn);
    return 0;
}

