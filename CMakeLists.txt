cmake_minimum_required(VERSION 3.16)
project(astralog_hpc CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find OpenMP
find_package(OpenMP REQUIRED)

# Optimisation flags for cache-aligned, vectorised code
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -DNDEBUG")

# Main executable — single-file unified engine
add_executable(astralog_processing
    src/main.cpp
)

target_include_directories(astralog_processing PRIVATE
    src
)

target_link_libraries(astralog_processing
    OpenMP::OpenMP_CXX
)

# Set output directory
set_target_properties(astralog_processing PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# Optional: Test executable
option(BUILD_TESTS "Build test suite" OFF)
if(BUILD_TESTS)
    enable_testing()
    add_executable(processing_tests test/test_processing.cpp)
    target_include_directories(processing_tests PRIVATE
        src
    )
    target_link_libraries(processing_tests
        OpenMP::OpenMP_CXX
    )
    add_test(NAME processing_tests COMMAND processing_tests)
endif()
