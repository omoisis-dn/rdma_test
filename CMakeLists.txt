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

# Find HIP (ROCm) for GPU memory allocation
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
if(ROCM_PATH AND EXISTS "${ROCM_PATH}/include/hip")
    message(STATUS "Found HIP headers in ${ROCM_PATH}")
    set(HIP_INCLUDE_DIRS "${ROCM_PATH}/include")
    
    # Try to find HIP runtime library
    find_library(HIP_LIBRARIES 
        NAMES amdhip64 hip_hcc
        PATHS "${ROCM_PATH}/lib" 
        NO_DEFAULT_PATH
    )
    
    if(HIP_LIBRARIES)
        message(STATUS "Found HIP library: ${HIP_LIBRARIES}")
        set(USE_GPU_MEMORY ON)
    else()
        message(STATUS "HIP headers found but library not found, using host memory")
        set(USE_GPU_MEMORY OFF)
    endif()
else()
    message(STATUS "HIP not found, using host memory")
    message(STATUS "  To enable GPU memory, set ROCM_PATH environment variable or pass -DROCM_PATH=/path/to/rocm")
    set(USE_GPU_MEMORY OFF)
endif()

# Source files
set(SOURCES
    src/main.cpp
    src/rdma_client.cpp
    src/rdma_server.cpp
    src/rdma_common.cpp
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
)

if(USE_GPU_MEMORY)
    target_include_directories(rdma_perf_test PRIVATE ${HIP_INCLUDE_DIRS})
endif()

# Link libraries
target_link_libraries(rdma_perf_test ${IBVERBS_LIBRARIES})
target_compile_options(rdma_perf_test PRIVATE ${IBVERBS_CFLAGS_OTHER})

if(USE_GPU_MEMORY)
    target_link_libraries(rdma_perf_test ${HIP_LIBRARIES})
    target_compile_definitions(rdma_perf_test PRIVATE USE_GPU_MEMORY)
endif()

# Compiler flags
target_compile_options(rdma_perf_test PRIVATE -Wall -Wextra -O3)

# Installation
install(TARGETS rdma_perf_test DESTINATION bin)

