#include "rdma_server.h"
#include "rdma_common.h"
#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <sys/select.h>
#include <errno.h>

int setup_server_connection(RDMAConnection& conn, const TestConfig& config) {
    // Initialize RDMA device (opens device, queries port/GID)
    // Note: We don't create buffers yet - that happens after receiving test params from client
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

    std::cout << "Server RDMA device initialized. Waiting for client connection..." << std::endl;

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

int serve_test(RDMAConnection& /* conn */, int client_sock) {
    // RDMA WRITE is one-sided - no need to post receives
    // The client writes directly to the server's buffer
    std::cout << "Test started. Waiting for completion signal..." << std::endl;
    
    // Wait for client to signal test completion
    // Client will send a single byte (0) when test is complete
    char test_done = 0;
    fd_set readfds;
    struct timeval timeout;
    
    while (true) {
        // Check if client sent test completion signal
        FD_ZERO(&readfds);
        FD_SET(client_sock, &readfds);
        timeout.tv_sec = 0;
        timeout.tv_usec = 1000;  // 1ms timeout
        
        int select_result = select(client_sock + 1, &readfds, nullptr, nullptr, &timeout);
        if (select_result > 0 && FD_ISSET(client_sock, &readfds)) {
            // Client sent something - check if it's the test completion signal
            ssize_t bytes = recv(client_sock, &test_done, 1, MSG_PEEK | MSG_DONTWAIT);
            if (bytes > 0) {
                // Client closed connection or sent completion signal
                recv(client_sock, &test_done, 1, 0);
                std::cout << "Test completed by client" << std::endl;
                break;
            } else if (bytes == 0) {
                // Client closed connection
                std::cout << "Client disconnected" << std::endl;
                break;
            }
        } else if (select_result < 0 && errno != EINTR) {
            std::cerr << "Select error: " << strerror(errno) << std::endl;
            return -1;
        }
    }
    
    return 0;
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
    
    std::cout << "Server ready. Waiting for clients (Press Ctrl+C to exit)..." << std::endl;
    
    // Loop to handle multiple sequential tests
    while (true) {
        // Accept client connection
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &client_len);
        if (client_sock < 0) {
            std::cerr << "Failed to accept client connection" << std::endl;
            break;
        }
        
        std::cout << "\n=== New client connected from " << inet_ntoa(client_addr.sin_addr) << " ===" << std::endl;
        
        // Exchange test parameters and QP information
        TestParams test_params;
        if (exchange_test_params_and_qp_info_server(client_sock, conn, test_params, config.use_gpu_memory, config.gpu_device_id) != 0) {
            std::cerr << "Failed to exchange test parameters and QP info" << std::endl;
            close(client_sock);
            continue;  // Continue to next client
        }
        
        // Serve the test (keep socket open to detect completion)
        if (serve_test(conn, client_sock) != 0) {
            std::cerr << "Error during test execution" << std::endl;
        }
        
        close(client_sock);
        
        // Cleanup test-specific resources (buffer, MR, CQ, QP) but keep device and PD
        std::cout << "Cleaning up test resources..." << std::endl;
        cleanup_rdma_test_resources(conn);
        
        std::cout << "Ready for next client..." << std::endl;
    }
    
    close(server_sock);
    cleanup_rdma_connection(conn);
    return 0;
}

