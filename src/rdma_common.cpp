#include "rdma_common.h"
#include <iostream>
#include <cstring>
#include <cstdlib>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>

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

    // Query port attributes
    struct ibv_port_attr port_attr;
    if (ibv_query_port(conn.context, port, &port_attr)) {
        std::cerr << "Could not query port " << port << std::endl;
        ibv_close_device(conn.context);
        return -1;
    }

    conn.port_num = port;
    conn.gid_index = 1;
    if (ibv_query_gid(conn.context, port, conn.gid_index, &conn.local_gid)) {
        std::cerr << "Could not find a valid GID for port " << port << std::endl;
        ibv_close_device(conn.context);
        return -1;    
    }
    std::cout << "Opened RoCE device, GID index: " << (int)conn.gid_index << ", GID: ";
    for (int i = 0; i < 16; i++) {
        printf("%02x", conn.local_gid.raw[i]);
        if (i == 7) printf(":");
    }
    std::cout << std::endl;
    
    memset(&conn.remote_gid, 0, sizeof(conn.remote_gid));
    
    return 0;
}

int allocate_and_register_buffer(RDMAConnection& conn, uint32_t buffer_size) {
    conn.buffer_size = buffer_size;
    
    // Allocate a single large buffer aligned to page size
    conn.buffer = aligned_alloc(4096, conn.buffer_size);
    if (!conn.buffer) {
        std::cerr << "Could not allocate buffer" << std::endl;
        return -1;
    }
    memset(conn.buffer, 0, conn.buffer_size);
    
    // Register the entire buffer as a single memory region
    conn.memory_region = ibv_reg_mr(conn.protection_domain, conn.buffer, conn.buffer_size,
                                     IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
                                     IBV_ACCESS_REMOTE_WRITE);
    if (!conn.memory_region) {
        std::cerr << "Could not register memory region" << std::endl;
        free(conn.buffer);
        return -1;
    }
    
    return 0;
}

int create_protection_domain_resources(RDMAConnection& conn, uint32_t buffer_size, uint32_t chunk_size, uint32_t num_in_flight) {
    // Note: Protection domain should already be allocated before calling this function
    
    conn.chunk_size = chunk_size;
    conn.num_in_flight = num_in_flight;
    
    if (allocate_and_register_buffer(conn, buffer_size) != 0) {
        std::cerr << "Could not allocate and register buffer" << std::endl;
        return -1;
    }

    // Create completion queue (size should accommodate in-flight operations)
    // Note: Client only sends, server only receives, so we only need num_in_flight
    conn.completion_queue = ibv_create_cq(conn.context, num_in_flight, nullptr, nullptr, 0);
    if (!conn.completion_queue) {
        std::cerr << "Could not create completion queue" << std::endl;
        return -1;
    }
    
    // Configure CQ moderation to match ib_send_bw behavior
    // cq_count=1 means generate event after 1 completion (minimal batching)
    // cq_period=0 means no timeout (only count-based)
    // Note: CQ moderation is primarily useful for interrupt-driven applications.
    // Since we're using polling, the benefit is minimal, but we set it to match ib_send_bw.
    struct ibv_modify_cq_attr cq_attr;
    memset(&cq_attr, 0, sizeof(cq_attr));
    cq_attr.attr_mask = IBV_CQ_ATTR_MODERATE;
    cq_attr.moderate.cq_count = 1;  // Match ib_send_bw "CQ Moderation: 1"
    cq_attr.moderate.cq_period = 0; // No timeout
    if (ibv_modify_cq(conn.completion_queue, &cq_attr)) {
        // CQ moderation might not be supported on all devices, so this is not fatal
        // Just log a warning and continue
        std::cerr << "Warning: Could not set CQ moderation (may not be supported)" << std::endl;
    }

    // Create queue pair attributes
    struct ibv_qp_init_attr qp_init_attr;
    memset(&qp_init_attr, 0, sizeof(qp_init_attr));
    qp_init_attr.qp_type = IBV_QPT_RC;
    qp_init_attr.sq_sig_all = 0;
    qp_init_attr.send_cq = conn.completion_queue;
    qp_init_attr.recv_cq = conn.completion_queue;
    qp_init_attr.cap.max_send_wr = num_in_flight;
    qp_init_attr.cap.max_recv_wr = num_in_flight;
    qp_init_attr.cap.max_send_sge = 1;
    qp_init_attr.cap.max_recv_sge = 1;
    qp_init_attr.cap.max_inline_data = 0;

    // Create queue pair
    conn.queue_pair = ibv_create_qp(conn.protection_domain, &qp_init_attr);
    if (!conn.queue_pair) {
        std::cerr << "Could not create queue pair" << std::endl;
        return -1;
    }

    std::cout << "Created protection domain resources: memory region (" << conn.buffer_size 
              << " bytes total), chunk size: " << chunk_size 
              << " bytes, in-flight: " << num_in_flight 
              << ", completion queue, queue pair" << std::endl;
    return 0;
}

void cleanup_rdma_test_resources(RDMAConnection& conn) {
    // Cleanup only test-specific resources (buffer, MR, CQ, QP)
    // Keep device and protection domain for reuse
    if (conn.memory_region) {
        ibv_dereg_mr(conn.memory_region);
        conn.memory_region = nullptr;
    }
    if (conn.buffer) {
        free(conn.buffer);
        conn.buffer = nullptr;
    }
    if (conn.queue_pair) {
        ibv_destroy_qp(conn.queue_pair);
        conn.queue_pair = nullptr;
    }
    if (conn.completion_queue) {
        ibv_destroy_cq(conn.completion_queue);
        conn.completion_queue = nullptr;
    }
}

void cleanup_rdma_connection(RDMAConnection& conn) {
    cleanup_rdma_test_resources(conn);
    if (conn.protection_domain) {
        ibv_dealloc_pd(conn.protection_domain);
        conn.protection_domain = nullptr;
    }
    if (conn.context) {
        ibv_close_device(conn.context);
        conn.context = nullptr;
    }
}

// poll_receive_completions removed - not needed for RDMA WRITE (one-sided operation)
// The server doesn't need to poll for receives since RDMA WRITE writes directly to the buffer

int poll_send_completions(RDMAConnection& conn, int max_completions) {
    struct ibv_wc wc[32];
    int num_completions = ibv_poll_cq(conn.completion_queue, std::min(32, max_completions), wc);
    if (num_completions < 0) {
        std::cerr << "Poll CQ failed" << std::endl;
        return -1;
    }
    
    for (int i = 0; i < num_completions; i++) {
        if (wc[i].status != IBV_WC_SUCCESS) {
            std::cerr << "Work completion error: " << ibv_wc_status_str(wc[i].status) << std::endl;
            return -1;
        }
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

int exchange_test_params_and_qp_info_server(int sockfd, RDMAConnection& conn, TestParams& test_params) {
    // Step 1: Receive test parameters from client
    TestParams client_params;
    if (recv(sockfd, &client_params, sizeof(client_params), 0) != sizeof(client_params)) {
        std::cerr << "Failed to receive test parameters from client" << std::endl;
        return -1;
    }
    
    std::cout << "Received test parameters from client: buffer_size=" << client_params.buffer_size
              << ", chunk_size=" << client_params.chunk_size
              << ", num_in_flight=" << client_params.num_in_flight << std::endl;
    
    // Validate that buffer_size is divisible by chunk_size
    if (client_params.buffer_size % client_params.chunk_size != 0) {
        std::cerr << "Error: buffer_size (" << client_params.buffer_size 
                  << ") must be divisible by chunk_size (" << client_params.chunk_size << ")" << std::endl;
        return -1;
    }
    
    // Use client's parameters (they must match)
    test_params = client_params;
    
    // Step 2: Now initialize buffers with the received parameters
    if (create_protection_domain_resources(conn, test_params.buffer_size, test_params.chunk_size, test_params.num_in_flight) != 0) {
        std::cerr << "Failed to create protection domain resources" << std::endl;
        return -1;
    }
    
    // Step 3: Transition QP to INIT
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
        return -1;
    }
    
    std::cout << "Server QP initialized. QP number: " << conn.queue_pair->qp_num << std::endl;
    
    // Step 4: Send our QP info and buffer info to client (for RDMA WRITE)
    QPInfo local_info;
    local_info.qp_num = conn.queue_pair->qp_num;
    local_info.gid = conn.local_gid;
    local_info.remote_buffer_addr = (uint64_t)conn.buffer;  // Server's buffer address
    local_info.remote_rkey = conn.memory_region->rkey;       // Server's memory region key
    
    if (send(sockfd, &local_info, sizeof(local_info), 0) != sizeof(local_info)) {
        std::cerr << "Failed to send QP info" << std::endl;
        return -1;
    }
    
    // Step 5: Receive client's QP info
    QPInfo remote_info;
    if (recv(sockfd, &remote_info, sizeof(remote_info), 0) != sizeof(remote_info)) {
        std::cerr << "Failed to receive QP info" << std::endl;
        return -1;
    }
    
    conn.remote_gid = remote_info.gid;
    
    // Step 6: Connect QP to RTS
    if (connect_qp_to_rts(conn, remote_info) != 0) {
        return -1;
    }
    
    std::cout << "QP connected. Remote GID: ";
    for (int i = 0; i < 16; i++) {
        printf("%02x", remote_info.gid.raw[i]);
        if (i == 7) printf(":");
    }
    std::cout << ", Remote QP: " << remote_info.qp_num << std::endl;
    return 0;
}

int exchange_test_params_and_qp_info_client(int sockfd, RDMAConnection& conn, const TestParams& test_params) {
    // Step 1: Send test parameters to server
    if (send(sockfd, &test_params, sizeof(test_params), 0) != sizeof(test_params)) {
        std::cerr << "Failed to send test parameters to server" << std::endl;
        return -1;
    }
    
    std::cout << "Sent test parameters to server: buffer_size=" << test_params.buffer_size
              << ", chunk_size=" << test_params.chunk_size
              << ", num_in_flight=" << test_params.num_in_flight << std::endl;
    
    // Step 2: Now initialize buffers with the same parameters
    if (create_protection_domain_resources(conn, test_params.buffer_size, test_params.chunk_size, test_params.num_in_flight) != 0) {
        std::cerr << "Failed to create protection domain resources" << std::endl;
        return -1;
    }
    
    // Step 3: Transition QP to INIT
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
        return -1;
    }
    
    std::cout << "Client QP initialized. QP number: " << conn.queue_pair->qp_num << std::endl;
    
    // Step 4: Receive server's QP info
    QPInfo server_info;
    if (recv(sockfd, &server_info, sizeof(server_info), 0) != sizeof(server_info)) {
        std::cerr << "Failed to receive server QP info" << std::endl;
        return -1;
    }
    
    // Step 5: Send our QP info to server (client doesn't need to send buffer info for RDMA WRITE)
    QPInfo local_info;
    local_info.qp_num = conn.queue_pair->qp_num;
    local_info.gid = conn.local_gid;
    local_info.remote_buffer_addr = 0;  // Not needed from client
    local_info.remote_rkey = 0;         // Not needed from client
    
    if (send(sockfd, &local_info, sizeof(local_info), 0) != sizeof(local_info)) {
        std::cerr << "Failed to send QP info" << std::endl;
        return -1;
    }
    
    conn.remote_gid = server_info.gid;
    // Store remote buffer address and rkey for RDMA WRITE operations
    conn.remote_buffer_addr = server_info.remote_buffer_addr;
    conn.remote_rkey = server_info.remote_rkey;
    
    // Step 6: Connect QP to RTS
    if (connect_qp_to_rts(conn, server_info) != 0) {
        return -1;
    }
    
    std::cout << "QP connected. Remote GID: ";
    for (int i = 0; i < 16; i++) {
        printf("%02x", server_info.gid.raw[i]);
        if (i == 7) printf(":");
    }
    std::cout << ", Remote QP: " << server_info.qp_num << std::endl;
    return 0;
}

int connect_qp_to_rts(RDMAConnection& conn, const QPInfo& remote_info) {
    // Query port attributes to get actual MTU
    struct ibv_port_attr port_attr;
    if (ibv_query_port(conn.context, conn.port_num, &port_attr)) {
        std::cerr << "Failed to query port attributes" << std::endl;
        return -1;
    }
    
    // Check if port is active
    if (port_attr.state != IBV_PORT_ACTIVE) {
        std::cerr << "Port is not active (state: " << port_attr.state << ")" << std::endl;
        return -1;
    }
    
    // Query device attributes to get capabilities
    struct ibv_device_attr device_attr;
    if (ibv_query_device(conn.context, &device_attr)) {
        std::cerr << "Failed to query device attributes" << std::endl;
        return -1;
    }
    
    // Use the actual port MTU (or a smaller one if 4096 is not supported)
    enum ibv_mtu path_mtu = port_attr.max_mtu;
    
    // Get max atomic capabilities (use minimum of device capabilities)
    // If device doesn't support atomic operations, use 0
    uint8_t max_dest_rd_atomic = (device_attr.max_qp_rd_atom < 16) ? device_attr.max_qp_rd_atom : 16;
    
    uint8_t max_rd_atomic = (device_attr.max_qp_init_rd_atom < 16) ? device_attr.max_qp_init_rd_atom : 16;

    // Transition to RTR (Ready to Receive)
    struct ibv_qp_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTR;
    attr.path_mtu = path_mtu;
    attr.dest_qp_num = remote_info.qp_num;
    // Use a non-zero PSN for RoCE
    attr.rq_psn = 1;
    attr.max_dest_rd_atomic = max_dest_rd_atomic;
    attr.min_rnr_timer = 12;
    
    // Setup address handle for RoCE (GID-based addressing)
    struct ibv_ah_attr ah_attr;
    memset(&ah_attr, 0, sizeof(ah_attr));
    ah_attr.is_global = 1;
    ah_attr.grh.hop_limit = 255;
    ah_attr.grh.dgid = remote_info.gid;
    ah_attr.grh.sgid_index = conn.gid_index;
    ah_attr.grh.traffic_class = 0;
    ah_attr.grh.flow_label = 0;
    ah_attr.dlid = 0;
    ah_attr.sl = 0;
    ah_attr.src_path_bits = 0;
    ah_attr.port_num = conn.port_num;
    attr.ah_attr = ah_attr;
    
    int flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
                IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
    
    if (ibv_modify_qp(conn.queue_pair, &attr, flags)) {
        std::cerr << "Failed to modify QP to RTR: " << strerror(errno) << std::endl;
        std::cerr << "  Local GID: ";
        for (int i = 0; i < 16; i++) {
            printf("%02x", conn.local_gid.raw[i]);
            if (i == 7) printf(":");
        }
        std::cerr << std::endl;
        std::cerr << "  Remote GID: ";
        for (int i = 0; i < 16; i++) {
            printf("%02x", remote_info.gid.raw[i]);
            if (i == 7) printf(":");
        }
        std::cerr << std::endl;
        std::cerr << "  Remote QP: " << remote_info.qp_num << ", Port: " << (int)conn.port_num << std::endl;
        std::cerr << "  GID index: " << (int)conn.gid_index << std::endl;
        std::cerr << "  Path MTU: " << path_mtu << ", Max dest RD atomic: " << (int)max_dest_rd_atomic << std::endl;
        std::cerr << "  Port state: " << port_attr.state << " (should be " << IBV_PORT_ACTIVE << ")" << std::endl;
        return -1;
    }
    
    // Transition to RTS (Ready to Send)
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTS;
    attr.timeout = 14;
    attr.retry_cnt = 7;
    attr.rnr_retry = 7;
    // Use a non-zero PSN for RoCE (must match rq_psn)
    attr.sq_psn = 1;
    attr.max_rd_atomic = max_rd_atomic;
    
    flags = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
            IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;
    
    if (ibv_modify_qp(conn.queue_pair, &attr, flags)) {
        std::cerr << "Failed to modify QP to RTS: " << strerror(errno) << std::endl;
        return -1;
    }
    
    return 0;
}

int post_receive_work_request(RDMAConnection& conn, uint32_t slot_index) {
    if (slot_index >= conn.num_in_flight) {
        std::cerr << "Invalid slot index: " << slot_index << std::endl;
        return -1;
    }
    
    struct ibv_sge sge;
    sge.addr = (uintptr_t)conn.buffer + (slot_index * conn.chunk_size);
    sge.length = conn.chunk_size;
    sge.lkey = conn.memory_region->lkey;

    struct ibv_recv_wr wr;
    memset(&wr, 0, sizeof(wr));
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.wr_id = slot_index;  // Store slot index for reposting
    wr.next = nullptr;

    struct ibv_recv_wr* bad_wr;
    if (ibv_post_recv(conn.queue_pair, &wr, &bad_wr)) {
        std::cerr << "Failed to post receive work request" << std::endl;
        return -1;
    }
    
    return 0;
}

int post_rdma_write_chunk(RDMAConnection& conn, uint32_t chunk_offset, uint32_t chunk_size) {
    // Validate chunk offset and size
    if (chunk_offset + chunk_size > conn.buffer_size) {
        std::cerr << "Invalid chunk: offset " << chunk_offset 
                  << " + size " << chunk_size 
                  << " exceeds buffer size " << conn.buffer_size << std::endl;
        return -1;
    }
    
    struct ibv_sge sge;
    sge.addr = (uintptr_t)conn.buffer + chunk_offset;
    sge.length = chunk_size;
    sge.lkey = conn.memory_region->lkey;

    struct ibv_send_wr wr;
    memset(&wr, 0, sizeof(wr));
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_RDMA_WRITE;
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.wr_id = chunk_offset;  // Store chunk offset for tracking
    wr.wr.rdma.remote_addr = conn.remote_buffer_addr + chunk_offset;  // Remote address for this chunk
    wr.wr.rdma.rkey = conn.remote_rkey;  // Remote memory region key
    wr.next = nullptr;

    struct ibv_send_wr* bad_wr;
    if (ibv_post_send(conn.queue_pair, &wr, &bad_wr)) {
        std::cerr << "Failed to post RDMA write work request" << std::endl;
        return -1;
    }
    
    return 0;
}

