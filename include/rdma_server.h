#ifndef RDMA_SERVER_H
#define RDMA_SERVER_H

#include "rdma_common.h"
#include <string>

int run_server(const TestConfig& config);
int setup_server_connection(RDMAConnection& conn, const TestConfig& config);
int create_server_socket(uint16_t port);

#endif // RDMA_SERVER_H

