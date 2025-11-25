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

# Link libraries
target_link_libraries(rdma_perf_test ${IBVERBS_LIBRARIES})
target_compile_options(rdma_perf_test PRIVATE ${IBVERBS_CFLAGS_OTHER})

# Compiler flags
target_compile_options(rdma_perf_test PRIVATE -Wall -Wextra -O3)

# Installation
install(TARGETS rdma_perf_test DESTINATION bin)

