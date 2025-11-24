#ifndef RDMA_COMMON_H
#define RDMA_COMMON_H

#include <infiniband/verbs.h>
#include <string>
#include <cstdint>

// RDMA connection parameters
struct RDMAConnection {
    struct ibv_context* context;
    struct ibv_pd* protection_domain;
    struct ibv_cq* completion_queue;
    struct ibv_qp* queue_pair;
    struct ibv_mr* memory_region;
    void* buffer;
    uint32_t buffer_size;
    uint32_t local_lid;
    uint32_t remote_lid;
    uint32_t qp_num;
    uint16_t port_num;
};

// Test configuration
struct TestConfig {
    uint32_t message_size;
    uint32_t num_iterations;
    bool measure_latency;
    bool measure_bandwidth;
    std::string device_name;
    uint16_t port;  // RDMA port number
    uint16_t tcp_port;  // TCP port for connection establishment
};

// QP information exchange structure
struct QPInfo {
    uint32_t lid;
    uint32_t qp_num;
};

// Function declarations
int init_rdma_device(RDMAConnection& conn, const std::string& device_name, uint16_t port);
int create_queue_pair(RDMAConnection& conn);
int register_memory(RDMAConnection& conn, uint32_t buffer_size);
void cleanup_rdma_connection(RDMAConnection& conn);
int poll_completion(RDMAConnection& conn);
int list_rdma_devices();
int exchange_qp_info_server(int sockfd, RDMAConnection& conn);
int exchange_qp_info_client(int sockfd, RDMAConnection& conn);
int connect_qp_to_rts(RDMAConnection& conn, const QPInfo& remote_info);

#endif // RDMA_COMMON_H

