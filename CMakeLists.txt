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

    add_executable(component_integration_tests
        test/test_component_integration.cpp
    )
    target_include_directories(component_integration_tests PRIVATE
        src
    )
    target_link_libraries(component_integration_tests
        OpenMP::OpenMP_CXX
    )
    add_test(
        NAME component_integration_tests
        COMMAND component_integration_tests
            ${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures
    )

    add_executable(concurrency_integration_tests
        test/test_concurrency_integration.cpp
    )
    target_include_directories(concurrency_integration_tests PRIVATE
        src
    )
    target_link_libraries(concurrency_integration_tests
        OpenMP::OpenMP_CXX
    )
    add_test(
        NAME concurrency_integration_tests
        COMMAND concurrency_integration_tests
    )

    add_test(
        NAME e2e_golden_thread_batch_matrix
        COMMAND ${CMAKE_COMMAND}
            -DASTRALOG_EXE=$<TARGET_FILE:astralog_processing>
            -DFIXTURE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures/integration
            -DOUTPUT_ROOT=${CMAKE_CURRENT_BINARY_DIR}/e2e_golden
            -P ${CMAKE_CURRENT_SOURCE_DIR}/test/run_e2e_fixture.cmake
    )

    set_tests_properties(component_integration_tests PROPERTIES
        LABELS "integration"
    )
    set_tests_properties(concurrency_integration_tests PROPERTIES
        LABELS "integration;concurrency;metamorphic"
    )
    set_tests_properties(e2e_golden_thread_batch_matrix PROPERTIES
        LABELS "integration;e2e;concurrency;metamorphic"
    )
endif()
