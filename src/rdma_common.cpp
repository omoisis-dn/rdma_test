#include "rdma_common.h"
#include <iostream>
#include <cstring>
#include <cstdlib>

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

