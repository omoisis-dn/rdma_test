#ifndef RDMA_DEVICE_DISCOVERY_H
#define RDMA_DEVICE_DISCOVERY_H

#include <string>

// RDMA device discovery functions
int list_rdma_devices();

// GPU device discovery functions
int list_gpu_devices();

// PCI topology functions
std::string get_rdma_device_pci_address(const std::string& device_name);
std::string get_gpu_pci_address(int gpu_id);
int calculate_pci_distance(const std::string& pci1, const std::string& pci2);
int find_best_gpu_for_nic(const std::string& nic_pci_address);

#endif // RDMA_DEVICE_DISCOVERY_H

