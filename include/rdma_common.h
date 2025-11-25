#ifndef RDMA_COMMON_H
#define RDMA_COMMON_H

#include <infiniband/verbs.h>
#include <string>
#include <cstdint>

#define NUM_BUFFERS 32

// RDMA connection parameters (RoCE only)
struct RDMAConnection {
    struct ibv_context* context;
    struct ibv_pd* protection_domain;
    struct ibv_cq* completion_queue;
    struct ibv_qp* queue_pair;
    struct ibv_mr* memory_region[NUM_BUFFERS];
    void* buffers[NUM_BUFFERS];
    uint32_t buffer_size;
    uint16_t port_num;
    union ibv_gid local_gid;
    union ibv_gid remote_gid;
    uint8_t gid_index;
};

// Test configuration
struct TestConfig {
    uint32_t message_size;
    uint32_t num_iterations;
    uint32_t num_messages;
    bool measure_latency;
    bool measure_bandwidth;
    std::string device_name;
    uint16_t port;  // RDMA port number
    uint16_t tcp_port;  // TCP port for connection establishment
};

// QP information exchange structure
struct QPInfo {
    uint32_t qp_num;
    union ibv_gid gid;
};

// Function declarations
int init_rdma_device(RDMAConnection& conn, const std::string& device_name, uint16_t port);
int create_protection_domain_resources(RDMAConnection& conn, uint32_t buffer_size);
void cleanup_rdma_connection(RDMAConnection& conn);
int poll_completion(RDMAConnection& conn);
int list_rdma_devices();
int exchange_qp_info_server(int sockfd, RDMAConnection& conn);
int exchange_qp_info_client(int sockfd, RDMAConnection& conn);
int connect_qp_to_rts(RDMAConnection& conn, const QPInfo& remote_info);
int post_receive_work_request(RDMAConnection& conn, uint32_t size, int buffer_index);

#endif // RDMA_COMMON_H

