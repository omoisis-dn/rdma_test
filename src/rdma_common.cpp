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

int allocate_and_register_buffer(RDMAConnection& conn, uint32_t buffer_size, bool use_gpu_memory, int gpu_device_id) {
    conn.buffer_size = buffer_size;
    
    if (use_gpu_memory) {
        // Set the GPU device
        if (gpu_device_id >= 0) {
            hipError_t hip_err = hipSetDevice(gpu_device_id);
            if (hip_err != hipSuccess) {
                std::cerr << "Could not set GPU device " << gpu_device_id << ": " << hipGetErrorString(hip_err) << std::endl;
                return -1;
            }
            std::cout << "Using GPU device " << gpu_device_id << std::endl;
        }
        
        // Allocate GPU device memory using HIP
        hipError_t hip_err = hipMalloc(&conn.buffer, conn.buffer_size);
        if (hip_err != hipSuccess) {
            std::cerr << "Could not allocate GPU buffer: " << hipGetErrorString(hip_err) << std::endl;
            return -1;
        }
        
        // Initialize GPU buffer to zero
        hip_err = hipMemset(conn.buffer, 0, conn.buffer_size);
        if (hip_err != hipSuccess) {
            std::cerr << "Could not initialize GPU buffer: " << hipGetErrorString(hip_err) << std::endl;
            cleanup_rdma_test_resources(conn);
            return -1;
        }
        
        std::cout << "Allocated " << conn.buffer_size << " bytes on GPU device" << std::endl;
    } else {
        // Allocate a single large buffer aligned to page size (host memory)
        conn.buffer = aligned_alloc(4096, conn.buffer_size);
        if (!conn.buffer) {
            std::cerr << "Could not allocate buffer" << std::endl;
            return -1;
        }
        memset(conn.buffer, 0, conn.buffer_size);
        std::cout << "Allocated " << conn.buffer_size << " bytes on host" << std::endl;
    }
    
    // Register the entire buffer as a single memory region
    // Note: ibv_reg_mr should work with GPU memory if GPU Direct RDMA is supported
    conn.memory_region = ibv_reg_mr(conn.protection_domain, conn.buffer, conn.buffer_size,
                                     IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
                                     IBV_ACCESS_REMOTE_WRITE);
    if (!conn.memory_region) {
        std::cerr << "Could not register memory region" << std::endl;
        cleanup_rdma_test_resources(conn);
        return -1;
    }
    
    return 0;
}

int create_protection_domain_resources(RDMAConnection& conn, uint32_t buffer_size, uint32_t chunk_size, uint32_t num_in_flight, uint32_t num_queue_pairs, bool use_gpu_memory, int gpu_device_id) {
    // Note: Protection domain should already be allocated before calling this function
    
    conn.chunk_size = chunk_size;
    conn.num_in_flight = num_in_flight;
    conn.num_queue_pairs = num_queue_pairs;
    conn.next_qp_index = 0;
    
    if (allocate_and_register_buffer(conn, buffer_size, use_gpu_memory, gpu_device_id) != 0) {
        std::cerr << "Could not allocate and register buffer" << std::endl;
        return -1;
    }

    // Allocate arrays for multiple QPs and CQs
    conn.completion_queues = new struct ibv_cq*[num_queue_pairs];
    conn.queue_pairs = new struct ibv_qp*[num_queue_pairs];
    if (!conn.completion_queues || !conn.queue_pairs) {
        std::cerr << "Could not allocate arrays for queue pairs" << std::endl;
        cleanup_rdma_test_resources(conn);
        return -1;
    }
    
    // Initialize arrays
    for (uint32_t i = 0; i < num_queue_pairs; i++) {
        conn.completion_queues[i] = nullptr;
        conn.queue_pairs[i] = nullptr;
    }

    // Create completion queues (one per QP)
    // Note: Client only sends, server only receives, so we only need num_in_flight
    for (uint32_t i = 0; i < num_queue_pairs; i++) {
        conn.completion_queues[i] = ibv_create_cq(conn.context, num_in_flight, nullptr, nullptr, 0);
        if (!conn.completion_queues[i]) {
            std::cerr << "Could not create completion queue " << i << std::endl;
            cleanup_rdma_test_resources(conn);
            return -1;
        }
    }
    
    // Create queue pair attributes
    struct ibv_qp_init_attr qp_init_attr;
    memset(&qp_init_attr, 0, sizeof(qp_init_attr));
    qp_init_attr.qp_type = IBV_QPT_RC;
    qp_init_attr.sq_sig_all = 0;
    qp_init_attr.cap.max_send_wr = num_in_flight;
    qp_init_attr.cap.max_recv_wr = num_in_flight;
    qp_init_attr.cap.max_send_sge = 1;
    qp_init_attr.cap.max_recv_sge = 1;
    qp_init_attr.cap.max_inline_data = 0;

    // Create queue pairs
    for (uint32_t i = 0; i < num_queue_pairs; i++) {
        qp_init_attr.send_cq = conn.completion_queues[i];
        qp_init_attr.recv_cq = conn.completion_queues[i];
        
        conn.queue_pairs[i] = ibv_create_qp(conn.protection_domain, &qp_init_attr);
        if (!conn.queue_pairs[i]) {
            std::cerr << "Could not create queue pair " << i << std::endl;
            cleanup_rdma_test_resources(conn);
            return -1;
        }
    }

    std::cout << "Created protection domain resources: memory region (" << conn.buffer_size 
              << " bytes total), chunk size: " << chunk_size 
              << " bytes, in-flight: " << num_in_flight 
              << ", " << num_queue_pairs << " completion queue(s), " << num_queue_pairs << " queue pair(s)" << std::endl;
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
        // Try to free as GPU memory first, then fall back to CPU memory
        // This works because hipFree will fail if it's not GPU memory
        hipError_t hip_err = hipFree(conn.buffer);
        if (hip_err != hipSuccess) {
            // If hipFree fails, it's likely CPU memory, so use free()
            free(conn.buffer);
        }
        conn.buffer = nullptr;
    }
    
    // Cleanup multiple QPs and CQs
    if (conn.queue_pairs) {
        for (uint32_t i = 0; i < conn.num_queue_pairs; i++) {
            if (conn.queue_pairs[i]) {
                ibv_destroy_qp(conn.queue_pairs[i]);
            }
        }
        delete[] conn.queue_pairs;
        conn.queue_pairs = nullptr;
    }
    
    if (conn.completion_queues) {
        for (uint32_t i = 0; i < conn.num_queue_pairs; i++) {
            if (conn.completion_queues[i]) {
                ibv_destroy_cq(conn.completion_queues[i]);
            }
        }
        delete[] conn.completion_queues;
        conn.completion_queues = nullptr;
    }
    
    conn.num_queue_pairs = 0;
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
    int total_completions = 0;
    struct ibv_wc wc[32];
    
    // Poll all completion queues
    for (uint32_t qp_idx = 0; qp_idx < conn.num_queue_pairs && total_completions < max_completions; qp_idx++) {
        int num_completions = ibv_poll_cq(conn.completion_queues[qp_idx], std::min(32, max_completions), wc);
        if (num_completions < 0) {
            std::cerr << "Poll CQ failed for QP " << qp_idx << std::endl;
            return -1;
        }
        
        for (int i = 0; i < num_completions; i++) {
            if (wc[i].status != IBV_WC_SUCCESS) {
                std::cerr << "Work completion error on QP " << qp_idx << ": " << ibv_wc_status_str(wc[i].status) << std::endl;
                return -1;
            }
        }
        
        total_completions += num_completions;
    }
    
    return total_completions;
}

int exchange_test_params_and_qp_info_server(int sockfd, RDMAConnection& conn, TestParams& test_params, bool use_gpu_memory, int gpu_device_id) {
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
    if (create_protection_domain_resources(conn, test_params.buffer_size, test_params.chunk_size, test_params.num_in_flight, test_params.num_queue_pairs, use_gpu_memory, gpu_device_id) != 0) {
        std::cerr << "Failed to create protection domain resources" << std::endl;
        return -1;
    }
    
    // Step 3: Transition all QPs to INIT
    struct ibv_qp_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_INIT;
    attr.port_num = conn.port_num;
    attr.pkey_index = 0;
    attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
                           IBV_ACCESS_REMOTE_WRITE;
    
    for (uint32_t i = 0; i < conn.num_queue_pairs; i++) {
        if (ibv_modify_qp(conn.queue_pairs[i], &attr,
                          IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS)) {
            std::cerr << "Failed to modify QP " << i << " to INIT" << std::endl;
            return -1;
        }
    }
    
    std::cout << "Server QPs initialized. QP numbers: ";
    for (uint32_t i = 0; i < conn.num_queue_pairs; i++) {
        std::cout << conn.queue_pairs[i]->qp_num;
        if (i < conn.num_queue_pairs - 1) std::cout << ", ";
    }
    std::cout << std::endl;
    
    // Step 4: Send our QP info and buffer info to client (for RDMA WRITE)
    // Send buffer info once (same for all QPs)
    QPInfo buffer_info;
    buffer_info.qp_num = 0;  // Not used for buffer info
    buffer_info.gid = conn.local_gid;
    buffer_info.remote_buffer_addr = (uint64_t)conn.buffer;  // Server's buffer address
    buffer_info.remote_rkey = conn.memory_region->rkey;       // Server's memory region key
    
    if (send(sockfd, &buffer_info, sizeof(buffer_info), 0) != sizeof(buffer_info)) {
        std::cerr << "Failed to send buffer info" << std::endl;
        return -1;
    }
    
    // Send QP info for each QP
    QPInfo* local_qp_infos = new QPInfo[conn.num_queue_pairs];
    for (uint32_t i = 0; i < conn.num_queue_pairs; i++) {
        local_qp_infos[i].qp_num = conn.queue_pairs[i]->qp_num;
        local_qp_infos[i].gid = conn.local_gid;
        local_qp_infos[i].remote_buffer_addr = 0;  // Not needed
        local_qp_infos[i].remote_rkey = 0;         // Not needed
    }
    
    if (send(sockfd, local_qp_infos, sizeof(QPInfo) * conn.num_queue_pairs, 0) != (ssize_t)(sizeof(QPInfo) * conn.num_queue_pairs)) {
        std::cerr << "Failed to send QP info" << std::endl;
        delete[] local_qp_infos;
        return -1;
    }
    delete[] local_qp_infos;
    
    // Step 5: Receive client's QP info
    QPInfo* remote_qp_infos = new QPInfo[conn.num_queue_pairs];
    if (recv(sockfd, remote_qp_infos, sizeof(QPInfo) * conn.num_queue_pairs, 0) != (ssize_t)(sizeof(QPInfo) * conn.num_queue_pairs)) {
        std::cerr << "Failed to receive QP info" << std::endl;
        delete[] remote_qp_infos;
        return -1;
    }
    
    // Use first QP's GID (all should be the same)
    conn.remote_gid = remote_qp_infos[0].gid;
    
    // Step 6: Connect all QPs to RTS
    for (uint32_t i = 0; i < conn.num_queue_pairs; i++) {
        if (connect_qp_to_rts(conn, remote_qp_infos[i], i) != 0) {
            delete[] remote_qp_infos;
            return -1;
        }
    }
    delete[] remote_qp_infos;
    
    std::cout << "QPs connected. Remote GID: ";
    for (int i = 0; i < 16; i++) {
        printf("%02x", conn.remote_gid.raw[i]);
        if (i == 7) printf(":");
    }
    std::cout << std::endl;
    return 0;
}

int exchange_test_params_and_qp_info_client(int sockfd, RDMAConnection& conn, const TestParams& test_params, bool use_gpu_memory, int gpu_device_id) {
    // Step 1: Send test parameters to server
    if (send(sockfd, &test_params, sizeof(test_params), 0) != sizeof(test_params)) {
        std::cerr << "Failed to send test parameters to server" << std::endl;
        return -1;
    }
    
    std::cout << "Sent test parameters to server: buffer_size=" << test_params.buffer_size
              << ", chunk_size=" << test_params.chunk_size
              << ", num_in_flight=" << test_params.num_in_flight
              << ", num_queue_pairs=" << test_params.num_queue_pairs << std::endl;
    
    // Step 2: Now initialize buffers with the same parameters
    if (create_protection_domain_resources(conn, test_params.buffer_size, test_params.chunk_size, test_params.num_in_flight, test_params.num_queue_pairs, use_gpu_memory, gpu_device_id) != 0) {
        std::cerr << "Failed to create protection domain resources" << std::endl;
        return -1;
    }
    
    // Step 3: Transition all QPs to INIT
    struct ibv_qp_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_INIT;
    attr.port_num = conn.port_num;
    attr.pkey_index = 0;
    attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
                           IBV_ACCESS_REMOTE_WRITE;
    
    for (uint32_t i = 0; i < conn.num_queue_pairs; i++) {
        if (ibv_modify_qp(conn.queue_pairs[i], &attr,
                          IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS)) {
            std::cerr << "Failed to modify QP " << i << " to INIT" << std::endl;
            return -1;
        }
    }
    
    std::cout << "Client QPs initialized. QP numbers: ";
    for (uint32_t i = 0; i < conn.num_queue_pairs; i++) {
        std::cout << conn.queue_pairs[i]->qp_num;
        if (i < conn.num_queue_pairs - 1) std::cout << ", ";
    }
    std::cout << std::endl;
    
    // Step 4: Receive server's buffer info
    QPInfo server_buffer_info;
    if (recv(sockfd, &server_buffer_info, sizeof(server_buffer_info), 0) != sizeof(server_buffer_info)) {
        std::cerr << "Failed to receive server buffer info" << std::endl;
        return -1;
    }
    
    conn.remote_gid = server_buffer_info.gid;
    // Store remote buffer address and rkey for RDMA WRITE operations
    conn.remote_buffer_addr = server_buffer_info.remote_buffer_addr;
    conn.remote_rkey = server_buffer_info.remote_rkey;
    
    // Receive server's QP info
    QPInfo* server_qp_infos = new QPInfo[conn.num_queue_pairs];
    if (recv(sockfd, server_qp_infos, sizeof(QPInfo) * conn.num_queue_pairs, 0) != (ssize_t)(sizeof(QPInfo) * conn.num_queue_pairs)) {
        std::cerr << "Failed to receive server QP info" << std::endl;
        delete[] server_qp_infos;
        return -1;
    }
    
    // Step 5: Send our QP info to server
    QPInfo* local_qp_infos = new QPInfo[conn.num_queue_pairs];
    for (uint32_t i = 0; i < conn.num_queue_pairs; i++) {
        local_qp_infos[i].qp_num = conn.queue_pairs[i]->qp_num;
        local_qp_infos[i].gid = conn.local_gid;
        local_qp_infos[i].remote_buffer_addr = 0;  // Not needed from client
        local_qp_infos[i].remote_rkey = 0;         // Not needed from client
    }
    
    if (send(sockfd, local_qp_infos, sizeof(QPInfo) * conn.num_queue_pairs, 0) != (ssize_t)(sizeof(QPInfo) * conn.num_queue_pairs)) {
        std::cerr << "Failed to send QP info" << std::endl;
        delete[] local_qp_infos;
        delete[] server_qp_infos;
        return -1;
    }
    delete[] local_qp_infos;
    
    // Step 6: Connect all QPs to RTS
    for (uint32_t i = 0; i < conn.num_queue_pairs; i++) {
        if (connect_qp_to_rts(conn, server_qp_infos[i], i) != 0) {
            delete[] server_qp_infos;
            return -1;
        }
    }
    delete[] server_qp_infos;
    
    std::cout << "QPs connected. Remote GID: ";
    for (int i = 0; i < 16; i++) {
        printf("%02x", conn.remote_gid.raw[i]);
        if (i == 7) printf(":");
    }
    std::cout << std::endl;
    return 0;
}

int connect_qp_to_rts(RDMAConnection& conn, const QPInfo& remote_info, uint32_t qp_index) {
    // Select the QP to use
    if (qp_index >= conn.num_queue_pairs) {
        std::cerr << "Invalid QP index: " << qp_index << " (max: " << conn.num_queue_pairs << ")" << std::endl;
        return -1;
    }
    struct ibv_qp* qp = conn.queue_pairs[qp_index];
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
    
    if (ibv_modify_qp(qp, &attr, flags)) {
        std::cerr << "Failed to modify QP " << qp_index << " to RTR: " << strerror(errno) << std::endl;
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
    
    if (ibv_modify_qp(qp, &attr, flags)) {
        std::cerr << "Failed to modify QP " << qp_index << " to RTS: " << strerror(errno) << std::endl;
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

    // Use first QP for receive (for RDMA WRITE, server doesn't need receives, but if this is used, use QP 0)
    struct ibv_recv_wr* bad_wr;
    if (ibv_post_recv(conn.queue_pairs[0], &wr, &bad_wr)) {
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

    // Select QP using round-robin
    uint32_t qp_idx = conn.next_qp_index;
    struct ibv_qp* qp_to_use = conn.queue_pairs[qp_idx];
    conn.next_qp_index = (conn.next_qp_index + 1) % conn.num_queue_pairs;

    struct ibv_send_wr* bad_wr;
    if (ibv_post_send(qp_to_use, &wr, &bad_wr)) {
        std::cerr << "Failed to post RDMA write work request" << std::endl;
        return -1;
    }
    
    return 0;
}

