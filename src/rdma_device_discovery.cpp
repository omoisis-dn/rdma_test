#include "rdma_device_discovery.h"
#include "rdma_common.h"
#include <iostream>
#include <filesystem>
#include <infiniband/verbs.h>

// RDMA device discovery
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
        std::cout << "  Device Path: " << device->dev_path << std::endl;
        
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

// Get PCI address of an RDMA device from its name
std::string get_rdma_device_pci_address(const std::string& device_name) {
    std::string sysfs_path = "/sys/class/infiniband/" + device_name + "/device";
    
    try {
        if (std::filesystem::exists(sysfs_path)) {
            std::filesystem::path device_path = std::filesystem::canonical(sysfs_path);
            std::string path_str = device_path.string();
            
            // Extract PCI address from path like /sys/devices/pci0000:50/0000:50:03.4/0000:53:00.0/0000:54:00.0
            // Find the last component that matches PCI address pattern (0000:XX:XX.X)
            size_t pos = path_str.find_last_of('/');
            if (pos != std::string::npos) {
                std::string pci_addr = path_str.substr(pos + 1);
                // Check if it matches PCI address pattern
                if (pci_addr.length() >= 12 && pci_addr.find(':') != std::string::npos) {
                    return pci_addr;
                }
            }
        }
    } catch (const std::filesystem::filesystem_error& e) {
        std::cerr << "Error reading RDMA device PCI address: " << e.what() << std::endl;
    }
    
    return "";
}

// Get PCI address of a GPU from its device ID
std::string get_gpu_pci_address(int gpu_id) {
    hipDeviceProp_t prop;
    hipError_t hip_err = hipGetDeviceProperties(&prop, gpu_id);
    if (hip_err == hipSuccess) {
        // return prop.name;
        std::cout << "GPU " << gpu_id << " name: " << prop.name << std::endl;
        std::cout << "GPU " << gpu_id << " pciBusID: " << prop.pciBusID << std::endl;
        std::cout << "GPU " << gpu_id << " pciDeviceID: " << prop.pciDeviceID << std::endl;
        std::cout << "GPU " << gpu_id << " pciDomainID: " << prop.pciDomainID << std::endl;
        std::cout << "GPU " << gpu_id << " pciBusID: " << prop.pciBusID << std::endl;
        std::cout << "GPU " << gpu_id << " pciDeviceID: " << prop.pciDeviceID << std::endl;
        std::cout << "GPU " << gpu_id << " pciDomainID: " << prop.pciDomainID << std::endl;
    }

    // Try to find via /sys/class/drm first (most reliable)
    std::string drm_path = "/sys/class/drm/card" + std::to_string(gpu_id) + "/device";
    try {
        if (std::filesystem::exists(drm_path)) {
            std::filesystem::path device_path = std::filesystem::canonical(drm_path);
            std::string path_str = device_path.string();
            
            // Extract PCI address from path like /sys/devices/pci0000:50/0000:50:03.4/0000:53:00.0/0000:54:00.0
            // Find the last component that matches PCI address pattern
            size_t pos = path_str.find_last_of('/');
            if (pos != std::string::npos) {
                std::string pci_addr = path_str.substr(pos + 1);
                if (pci_addr.length() >= 12 && pci_addr.find(':') != std::string::npos) {
                    return pci_addr;
                }
            }
        }
    } catch (const std::filesystem::filesystem_error& e) {
        // Ignore errors, try alternative method
    }
    
    return ""; 
}

// Calculate PCI topology distance between two PCI addresses
// Returns the number of PCIe hops to reach a common root, or -1 if unable to determine
int calculate_pci_distance(const std::string& pci1, const std::string& pci2) {
    if (pci1.empty() || pci2.empty()) {
        return -1;
    }
    
    // Get the full PCI paths from sysfs
    std::string path1 = "/sys/bus/pci/devices/" + pci1;
    std::string path2 = "/sys/bus/pci/devices/" + pci2;
    
    try {
        if (!std::filesystem::exists(path1) || !std::filesystem::exists(path2)) {
            return -1;
        }
        
        std::filesystem::path canonical1 = std::filesystem::canonical(path1);
        std::filesystem::path canonical2 = std::filesystem::canonical(path2);
        
        // Find common ancestor in the path
        std::filesystem::path common;
        auto it1 = canonical1.begin();
        auto it2 = canonical2.begin();
        
        while (it1 != canonical1.end() && it2 != canonical2.end() && *it1 == *it2) {
            common /= *it1;
            ++it1;
            ++it2;
        }
        
        // Count remaining components in both paths (distance from common root)
        int dist1 = 0, dist2 = 0;
        while (it1 != canonical1.end()) {
            ++it1;
            ++dist1;
        }
        while (it2 != canonical2.end()) {
            ++it2;
            ++dist2;
        }
        
        // Total distance is the sum of distances from common root
        return dist1 + dist2;
    } catch (const std::filesystem::filesystem_error& e) {
        return -1;
    }
}

// Find the best GPU for a given NIC based on PCI topology
int find_best_gpu_for_nic(const std::string& nic_pci_address) {
    if (nic_pci_address.empty()) {
        return -1;
    }
    
    int num_gpus;
    hipError_t hip_err = hipGetDeviceCount(&num_gpus);
    if (hip_err != hipSuccess || num_gpus == 0) {
        return -1;
    }
    
    int best_gpu = -1;
    int best_distance = -1;
    
    for (int i = 0; i < num_gpus; i++) {
        std::string gpu_pci = get_gpu_pci_address(i);
        if (gpu_pci.empty()) {
            continue;
        }
        
        int distance = calculate_pci_distance(nic_pci_address, gpu_pci);
        if (distance >= 0 && (best_distance < 0 || distance < best_distance)) {
            best_distance = distance;
            best_gpu = i;
        }
    }
    
    return best_gpu;
}

// List available GPU devices
int list_gpu_devices() {
    int num_gpus;
    hipError_t hip_err = hipGetDeviceCount(&num_gpus);
    if (hip_err != hipSuccess) {
        std::cerr << "Failed to get GPU device count: " << hipGetErrorString(hip_err) << std::endl;
        return -1;
    }
    
    if (num_gpus == 0) {
        std::cout << "No GPU devices found on this system." << std::endl;
        return 0;
    }
    
    std::cout << "Available GPU devices:" << std::endl;
    std::cout << "====================" << std::endl;
    
    for (int i = 0; i < num_gpus; i++) {
        hipDeviceProp_t prop;
        hip_err = hipGetDeviceProperties(&prop, i);
        if (hip_err != hipSuccess) {
            std::cout << "\nGPU " << i << ": Error getting properties" << std::endl;
            continue;
        }
        
        std::string pci_address = get_gpu_pci_address(i);
        
        std::cout << "\nGPU " << i << ":" << std::endl;
        std::cout << "  Name: " << prop.name << std::endl;
        std::cout << "  PCI Bus ID: " << prop.pciBusID << std::endl;
        if (!pci_address.empty()) {
            std::cout << "  PCI Address: " << pci_address << std::endl;
        }
        std::cout << "  Total Global Memory: " << (prop.totalGlobalMem / (1024 * 1024)) << " MB" << std::endl;
        std::cout << "  Compute Capability: " << prop.major << "." << prop.minor << std::endl;
    }
    
    return 0;
}

