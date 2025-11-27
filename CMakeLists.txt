cmake_minimum_required(VERSION 3.10)
project(RDMAPerfTest VERSION 1.0.0 LANGUAGES CXX)

# Export compile commands for clangd and other language servers
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find required libraries
find_package(PkgConfig REQUIRED)
pkg_check_modules(IBVERBS REQUIRED libibverbs)

# Find HIP (ROCm) - REQUIRED for GPU memory allocation support
# Allow user to specify ROCm path via ROCM_PATH environment variable or -DROCM_PATH
if(DEFINED ENV{ROCM_PATH})
    set(ROCM_PATH $ENV{ROCM_PATH} CACHE PATH "Path to ROCm installation")
elseif(NOT DEFINED ROCM_PATH)
    # Try to find ROCm in common locations
    if(EXISTS "/opt/rocm")
        set(ROCM_PATH "/opt/rocm" CACHE PATH "Path to ROCm installation")
    elseif(EXISTS "/opt/rocm-7.0.1")
        set(ROCM_PATH "/opt/rocm-7.0.1" CACHE PATH "Path to ROCm installation")
    endif()
endif()

if(ROCM_PATH)
    # Add ROCm to CMAKE_PREFIX_PATH so find_package can locate it
    list(APPEND CMAKE_PREFIX_PATH "${ROCM_PATH}")
    message(STATUS "Looking for HIP in: ${ROCM_PATH}")
endif()

# Find HIP runtime library manually (we only need runtime, not HIP language support)
# HIP language support adds GPU compilation flags which we don't need
if(NOT ROCM_PATH OR NOT EXISTS "${ROCM_PATH}/include/hip")
    message(FATAL_ERROR "HIP (ROCm) not found! HIP is required for this build.\n"
                        "  Please set ROCM_PATH environment variable or pass -DROCM_PATH=/path/to/rocm\n"
                        "  Example: cmake -DROCM_PATH=/opt/rocm-7.0.1 ..")
endif()

message(STATUS "Found HIP headers in ${ROCM_PATH}")
set(HIP_INCLUDE_DIRS "${ROCM_PATH}/include")

# Try to find HIP runtime library
find_library(HIP_LIBRARIES 
    NAMES amdhip64 hip_hcc
    PATHS "${ROCM_PATH}/lib" 
    NO_DEFAULT_PATH
    REQUIRED
)

if(NOT HIP_LIBRARIES)
    message(FATAL_ERROR "HIP runtime library not found in ${ROCM_PATH}/lib\n"
                        "  Required libraries: amdhip64 or hip_hcc")
endif()

message(STATUS "Found HIP library: ${HIP_LIBRARIES}")

# Source files
set(SOURCES
    src/main.cpp
    src/rdma_client.cpp
    src/rdma_server.cpp
    src/rdma_common.cpp
    src/rdma_device_discovery.cpp
)

set(HEADERS
    include/rdma_client.h
    include/rdma_server.h
    include/rdma_common.h
)

# Create executable
add_executable(rdma_perf_test ${SOURCES} ${HEADERS})

# Include directories
target_include_directories(rdma_perf_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${IBVERBS_INCLUDE_DIRS}
    ${HIP_INCLUDE_DIRS}
)

# Link libraries
target_link_libraries(rdma_perf_test ${IBVERBS_LIBRARIES} ${HIP_LIBRARIES})
target_compile_options(rdma_perf_test PRIVATE ${IBVERBS_CFLAGS_OTHER})

# Compiler flags
target_compile_options(rdma_perf_test PRIVATE -Wall -Wextra -O3)

# Installation
install(TARGETS rdma_perf_test DESTINATION bin)

