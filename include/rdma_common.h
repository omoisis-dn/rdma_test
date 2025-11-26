#ifndef RDMA_COMMON_H
#define RDMA_COMMON_H

#include <infiniband/verbs.h>
#include <string>
#include <cstdint>

#ifdef USE_GPU_MEMORY
// Only include HIP runtime API, not device code compilation support
#define __HIP_PLATFORM_AMD__
#include <hip/hip_runtime_api.h>
#endif

// RDMA connection parameters (RoCE only)
struct RDMAConnection {
    struct ibv_context* context;
    struct ibv_pd* protection_domain;
    struct ibv_cq* completion_queue;
    struct ibv_qp* queue_pair;
    struct ibv_mr* memory_region;
    void* buffer;
    uint32_t buffer_size;      // Total buffer size (total data to transfer per iteration)
    uint32_t chunk_size;        // Size of each chunk to send/receive
    uint32_t num_in_flight;     // Number of parallel in-flight chunks
    uint16_t port_num;
    union ibv_gid local_gid;
    union ibv_gid remote_gid;
    uint8_t gid_index;
    uint64_t remote_buffer_addr;  // Remote buffer address for RDMA WRITE
    uint32_t remote_rkey;         // Remote memory region key for RDMA WRITE
};

// Test configuration
struct TestConfig {
    uint32_t buffer_size;       // Total buffer size (total data to transfer per iteration)
    uint32_t chunk_size;        // Size of each chunk to send/receive
    uint32_t num_iterations;    // Number of iterations (each iteration transfers buffer_size bytes)
    uint32_t num_in_flight;     // Number of parallel in-flight chunks
    bool measure_latency;
    bool measure_bandwidth;
    std::string device_name;
    uint16_t port;  // RDMA port number
    uint16_t tcp_port;  // TCP port for connection establishment
};

// Test parameters for socket exchange (must match between client and server)
struct TestParams {
    uint32_t buffer_size;
    uint32_t chunk_size;
    uint32_t num_in_flight;
};

// QP information exchange structure
struct QPInfo {
    uint32_t qp_num;
    union ibv_gid gid;
    uint64_t remote_buffer_addr;  // Remote buffer address for RDMA WRITE
    uint32_t remote_rkey;         // Remote memory region key for RDMA WRITE
};

// Function declarations
int init_rdma_device(RDMAConnection& conn, const std::string& device_name, uint16_t port);
int create_protection_domain_resources(RDMAConnection& conn, uint32_t buffer_size, uint32_t chunk_size, uint32_t num_in_flight);
int post_rdma_write_chunk(RDMAConnection& conn, uint32_t chunk_offset, uint32_t chunk_size);
void cleanup_rdma_connection(RDMAConnection& conn);
void cleanup_rdma_test_resources(RDMAConnection& conn);  // Cleanup only test-specific resources (buffer, MR, CQ, QP)
int poll_send_completions(RDMAConnection& conn, int max_completions);
int list_rdma_devices();
int exchange_test_params_and_qp_info_server(int sockfd, RDMAConnection& conn, TestParams& test_params);
int exchange_test_params_and_qp_info_client(int sockfd, RDMAConnection& conn, const TestParams& test_params);
int connect_qp_to_rts(RDMAConnection& conn, const QPInfo& remote_info);

#endif // RDMA_COMMON_H

