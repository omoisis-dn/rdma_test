#ifndef RDMA_CLIENT_H
#define RDMA_CLIENT_H

#include "rdma_common.h"
#include <string>

int run_client(const TestConfig& config, const std::string& server_address);
int setup_client_connection(RDMAConnection& conn, const TestConfig& config, const std::string& server_address);
double measure_latency(RDMAConnection& conn, uint32_t message_size, uint32_t num_iterations);
double measure_bandwidth(RDMAConnection& conn, uint32_t message_size, uint32_t num_iterations);

#endif // RDMA_CLIENT_H

