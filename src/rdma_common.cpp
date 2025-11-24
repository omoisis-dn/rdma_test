#include "rdma_common.h"
#include <iostream>
#include <cstring>
#include <cstdlib>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

int init_rdma_device(RDMAConnection& conn, const std::string& device_name, uint16_t port) {
    // Get device list
    int num_devices;
    struct ibv_device** device_list = ibv_get_device_list(&num_devices);
    if (!device_list) {
        std::cerr << "Failed to get IB devices list" << std::endl;
        return -1;
    }

    if (num_devices == 0) {
        std::cerr << "No IB devices found" << std::endl;
        ibv_free_device_list(device_list);
        return -1;
    }

    // Find the specified device or use the first one
    struct ibv_device* device = nullptr;
    if (device_name.empty()) {
        device = device_list[0];
        std::cout << "Using device: " << ibv_get_device_name(device) << std::endl;
    } else {
        for (int i = 0; i < num_devices; i++) {
            if (device_name == ibv_get_device_name(device_list[i])) {
                device = device_list[i];
                break;
            }
        }
        if (!device) {
            std::cerr << "Device " << device_name << " not found" << std::endl;
            ibv_free_device_list(device_list);
            return -1;
        }
    }

    // Open device
    conn.context = ibv_open_device(device);
    ibv_free_device_list(device_list);
    if (!conn.context) {
        std::cerr << "Could not open device" << std::endl;
        return -1;
    }

    // Allocate protection domain
    conn.protection_domain = ibv_alloc_pd(conn.context);
    if (!conn.protection_domain) {
        std::cerr << "Could not allocate protection domain" << std::endl;
        ibv_close_device(conn.context);
        return -1;
    }

    // Query port attributes
    struct ibv_port_attr port_attr;
    if (ibv_query_port(conn.context, port, &port_attr)) {
        std::cerr << "Could not query port " << port << std::endl;
        ibv_dealloc_pd(conn.protection_domain);
        ibv_close_device(conn.context);
        return -1;
    }

    conn.local_lid = port_attr.lid;
    conn.port_num = port;

    std::cout << "Opened device, LID: " << conn.local_lid << std::endl;
    return 0;
}

int create_queue_pair(RDMAConnection& conn) {
    // Create completion queue
    conn.completion_queue = ibv_create_cq(conn.context, 10, nullptr, nullptr, 0);
    if (!conn.completion_queue) {
        std::cerr << "Could not create completion queue" << std::endl;
        return -1;
    }

    // Create queue pair attributes
    struct ibv_qp_init_attr qp_init_attr;
    memset(&qp_init_attr, 0, sizeof(qp_init_attr));
    qp_init_attr.qp_type = IBV_QPT_RC;
    qp_init_attr.sq_sig_all = 0;
    qp_init_attr.send_cq = conn.completion_queue;
    qp_init_attr.recv_cq = conn.completion_queue;
    qp_init_attr.cap.max_send_wr = 10;
    qp_init_attr.cap.max_recv_wr = 10;
    qp_init_attr.cap.max_send_sge = 1;
    qp_init_attr.cap.max_recv_sge = 1;
    qp_init_attr.cap.max_inline_data = 0;

    // Create queue pair
    conn.queue_pair = ibv_create_qp(conn.protection_domain, &qp_init_attr);
    if (!conn.queue_pair) {
        std::cerr << "Could not create queue pair" << std::endl;
        ibv_destroy_cq(conn.completion_queue);
        return -1;
    }

    return 0;
}

int register_memory(RDMAConnection& conn, uint32_t buffer_size) {
    // Allocate buffer
    conn.buffer = aligned_alloc(4096, buffer_size);
    if (!conn.buffer) {
        std::cerr << "Could not allocate buffer" << std::endl;
        return -1;
    }
    memset(conn.buffer, 0, buffer_size);
    conn.buffer_size = buffer_size;

    // Register memory region
    conn.memory_region = ibv_reg_mr(conn.protection_domain, conn.buffer, buffer_size,
                                    IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
                                    IBV_ACCESS_REMOTE_WRITE);
    if (!conn.memory_region) {
        std::cerr << "Could not register memory region" << std::endl;
        free(conn.buffer);
        return -1;
    }

    std::cout << "Registered memory region: " << buffer_size << " bytes" << std::endl;
    return 0;
}

void cleanup_rdma_connection(RDMAConnection& conn) {
    if (conn.memory_region) {
        ibv_dereg_mr(conn.memory_region);
    }
    if (conn.buffer) {
        free(conn.buffer);
    }
    if (conn.queue_pair) {
        ibv_destroy_qp(conn.queue_pair);
    }
    if (conn.completion_queue) {
        ibv_destroy_cq(conn.completion_queue);
    }
    if (conn.protection_domain) {
        ibv_dealloc_pd(conn.protection_domain);
    }
    if (conn.context) {
        ibv_close_device(conn.context);
    }
}

int poll_completion(RDMAConnection& conn) {
    struct ibv_wc wc;
    int num_completions = 0;
    
    while (ibv_poll_cq(conn.completion_queue, 1, &wc) > 0) {
        if (wc.status != IBV_WC_SUCCESS) {
            std::cerr << "Work completion error: " << ibv_wc_status_str(wc.status) << std::endl;
            return -1;
        }
        num_completions++;
    }
    
    return num_completions;
}

int list_rdma_devices() {
    int num_devices;
    struct ibv_device** device_list = ibv_get_device_list(&num_devices);
    
    if (!device_list) {
        std::cerr << "Failed to get IB devices list" << std::endl;
        return -1;
    }

    if (num_devices == 0) {
        std::cout << "No RDMA devices found on this system." << std::endl;
        ibv_free_device_list(device_list);
        return 0;
    }

    std::cout << "Available RDMA devices:" << std::endl;
    std::cout << "======================" << std::endl;

    for (int i = 0; i < num_devices; i++) {
        struct ibv_device* device = device_list[i];
        const char* device_name = ibv_get_device_name(device);
        
        std::cout << "\nDevice " << i << ":" << std::endl;
        std::cout << "  Name: " << device_name << std::endl;
        std::cout << "  GUID: " << std::hex << ibv_get_device_guid(device) << std::dec << std::endl;
        
        // Try to open device to get more information
        struct ibv_context* context = ibv_open_device(device);
        if (context) {
            struct ibv_device_attr device_attr;
            if (ibv_query_device(context, &device_attr) == 0) {
                std::cout << "  Physical Ports: " << device_attr.phys_port_cnt << std::endl;
                std::cout << "  Max MR Size: " << device_attr.max_mr_size << " bytes" << std::endl;
                std::cout << "  Max QP: " << device_attr.max_qp << std::endl;
                std::cout << "  Max CQ: " << device_attr.max_cq << std::endl;
                
                // Query port information
                for (uint8_t port = 1; port <= device_attr.phys_port_cnt; port++) {
                    struct ibv_port_attr port_attr;
                    if (ibv_query_port(context, port, &port_attr) == 0) {
                        std::cout << "  Port " << (int)port << ":" << std::endl;
                        std::cout << "    State: ";
                        switch (port_attr.state) {
                            case IBV_PORT_DOWN: std::cout << "Down"; break;
                            case IBV_PORT_INIT: std::cout << "Initializing"; break;
                            case IBV_PORT_ARMED: std::cout << "Armed"; break;
                            case IBV_PORT_ACTIVE: std::cout << "Active"; break;
                            default: std::cout << "Unknown"; break;
                        }
                        std::cout << std::endl;
                        std::cout << "    LID: " << port_attr.lid << std::endl;
                        std::cout << "    Max MTU: ";
                        switch (port_attr.max_mtu) {
                            case IBV_MTU_256: std::cout << "256"; break;
                            case IBV_MTU_512: std::cout << "512"; break;
                            case IBV_MTU_1024: std::cout << "1024"; break;
                            case IBV_MTU_2048: std::cout << "2048"; break;
                            case IBV_MTU_4096: std::cout << "4096"; break;
                            default: std::cout << "Unknown"; break;
                        }
                        std::cout << " bytes" << std::endl;
                    }
                }
            }
            
            ibv_close_device(context);
        }
    }

    std::cout << "\n" << std::endl;
    ibv_free_device_list(device_list);
    return 0;
}

int exchange_qp_info_server(int sockfd, RDMAConnection& conn) {
    // Send our QP info to client
    QPInfo local_info;
    local_info.lid = conn.local_lid;
    local_info.qp_num = conn.queue_pair->qp_num;
    
    if (send(sockfd, &local_info, sizeof(local_info), 0) != sizeof(local_info)) {
        std::cerr << "Failed to send QP info" << std::endl;
        return -1;
    }
    
    // Receive client's QP info
    QPInfo remote_info;
    if (recv(sockfd, &remote_info, sizeof(remote_info), 0) != sizeof(remote_info)) {
        std::cerr << "Failed to receive QP info" << std::endl;
        return -1;
    }
    
    conn.remote_lid = remote_info.lid;
    
    // Connect QP to RTS
    if (connect_qp_to_rts(conn, remote_info) != 0) {
        return -1;
    }
    
    std::cout << "QP connected. Remote LID: " << remote_info.lid 
              << ", Remote QP: " << remote_info.qp_num << std::endl;
    return 0;
}

int exchange_qp_info_client(int sockfd, RDMAConnection& conn) {
    // Receive server's QP info
    QPInfo server_info;
    if (recv(sockfd, &server_info, sizeof(server_info), 0) != sizeof(server_info)) {
        std::cerr << "Failed to receive server QP info" << std::endl;
        return -1;
    }
    
    // Send our QP info to server
    QPInfo local_info;
    local_info.lid = conn.local_lid;
    local_info.qp_num = conn.queue_pair->qp_num;
    
    if (send(sockfd, &local_info, sizeof(local_info), 0) != sizeof(local_info)) {
        std::cerr << "Failed to send QP info" << std::endl;
        return -1;
    }
    
    conn.remote_lid = server_info.lid;
    
    // Connect QP to RTS
    if (connect_qp_to_rts(conn, server_info) != 0) {
        return -1;
    }
    
    std::cout << "QP connected. Remote LID: " << server_info.lid 
              << ", Remote QP: " << server_info.qp_num << std::endl;
    return 0;
}

int connect_qp_to_rts(RDMAConnection& conn, const QPInfo& remote_info) {
    // Transition to RTR (Ready to Receive)
    struct ibv_qp_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTR;
    attr.path_mtu = IBV_MTU_4096;
    attr.dest_qp_num = remote_info.qp_num;
    attr.rq_psn = 0;
    attr.max_dest_rd_atomic = 16;
    attr.min_rnr_timer = 12;
    
    struct ibv_ah_attr ah_attr;
    memset(&ah_attr, 0, sizeof(ah_attr));
    ah_attr.is_global = 0;
    ah_attr.dlid = remote_info.lid;
    ah_attr.sl = 0;
    ah_attr.src_path_bits = 0;
    ah_attr.port_num = conn.port_num;
    attr.ah_attr = ah_attr;
    
    int flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
                IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
    
    if (ibv_modify_qp(conn.queue_pair, &attr, flags)) {
        std::cerr << "Failed to modify QP to RTR" << std::endl;
        return -1;
    }
    
    // Transition to RTS (Ready to Send)
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTS;
    attr.timeout = 14;
    attr.retry_cnt = 7;
    attr.rnr_retry = 7;
    attr.sq_psn = 0;
    attr.max_rd_atomic = 16;
    
    flags = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
            IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;
    
    if (ibv_modify_qp(conn.queue_pair, &attr, flags)) {
        std::cerr << "Failed to modify QP to RTS" << std::endl;
        return -1;
    }
    
    return 0;
}

