cmake_minimum_required(VERSION 3.16)
project(astralog_hpc CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find OpenMP
find_package(OpenMP REQUIRED)

# Allow overriding the architecture flag (e.g., for container builds)
set(ASTRALOG_ARCH "native" CACHE STRING "Target architecture for compiler (e.g., native, x86-64-v3, cascadelake)")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=${ASTRALOG_ARCH} -DNDEBUG")

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

# ========= Test Section =========

option(BUILD_TESTING "Build the test suite" ON)

if(BUILD_TESTING)
    # Enable testing
    enable_testing()

    # Add Google Test — prefer local copy (avoids network/FD issues in CI),
    # fall back to FetchContent if no local copy exists.
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/googletest/CMakeLists.txt")
        message(STATUS "Using local googletest at ${CMAKE_CURRENT_SOURCE_DIR}/googletest")
        set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
        set(BUILD_GMOCK   OFF CACHE BOOL "" FORCE)
        add_subdirectory(googletest EXCLUDE_FROM_ALL)
    else()
        message(FATAL_ERROR "Googletest not found inside the repository. Please ensure it is embedded.")
    endif()

    # Optional: coverage instrumentation (enabled with -DENABLE_COVERAGE=ON)
    option(ENABLE_COVERAGE "Enable gcov coverage instrumentation" OFF)
    if(ENABLE_COVERAGE)
        add_compile_options(--coverage -fprofile-arcs -ftest-coverage -O0 -fno-inline)
        add_link_options(--coverage)
    endif()

    option(CI "Run tests during workflow" OFF)
    if(CI)
        message(STATUS "Running specific tests for CI")
        add_executable(astralog_tests
            test/test_integration_component.cpp
            test/test_integration_concurrency.cpp
        )
    else()
        message(STATUS "Running all tests locally")
        add_executable(astralog_tests
            test/test_types.cpp
            test/test_csv_parser.cpp
            test/test_rules_engine.cpp
            test/test_pipeline.cpp
            test/test_integration_component.cpp
            test/test_integration_concurrency.cpp
        )
    endif()

    target_include_directories(astralog_tests PRIVATE
        src
    )

    target_link_libraries(astralog_tests PRIVATE
        GTest::gtest
        GTest::gtest_main
        OpenMP::OpenMP_CXX
    )

    include(GoogleTest)
    gtest_discover_tests(astralog_tests
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        DISCOVERY_MODE PRE_TEST
        PROPERTIES
            ENVIRONMENT "ASTRALOG_FIXTURE_ROOT=${CMAKE_SOURCE_DIR}/test/fixtures;OMP_NUM_THREADS=1"
    )
endif()
