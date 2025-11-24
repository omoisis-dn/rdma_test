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
    uint16_t port;
};

// Function declarations
int init_rdma_device(RDMAConnection& conn, const std::string& device_name, uint16_t port);
int create_queue_pair(RDMAConnection& conn);
int register_memory(RDMAConnection& conn, uint32_t buffer_size);
void cleanup_rdma_connection(RDMAConnection& conn);
int poll_completion(RDMAConnection& conn);

#endif // RDMA_COMMON_H

