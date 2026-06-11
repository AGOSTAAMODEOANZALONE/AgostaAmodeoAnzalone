# Testing Improvements Report

This document summarizes the testing work added to strengthen the verification
of AstraLog-HPC, with special focus on integration between software components
and robustness of the OpenMP/concurrency behavior.

## Goal

The previous test suite mainly checked isolated behavior through
`test/test_processing.cpp`. That was useful for basic rule semantics, but weak
for component integration and parallel execution because the existing test
program forced `omp_set_num_threads(1)`.

The new tests add:

- integration checks between adjacent software components;
- full executable golden-file tests;
- concurrency equivalence tests across OpenMP thread counts;
- metamorphic tests across batch sizes;
- parser/fault-handling integration checks.

Production source files in `src/` were not changed.

## Added Files

### Component Integration Tests

File: `test/test_component_integration.cpp`

This test executable validates the interfaces between the project components:

- YAML parser to token-map creation;
- token maps to CSV parser;
- rules parser to prepared rule lookup tables;
- CSV parser to `BatchAccumulator`;
- `BatchAccumulator` to `PipelineState` and `process_pipeline`;
- rule/pipeline results to output formatting;
- faulty CSV handling with unknown sensors and invalid rows.

The test uses real parser and pipeline functions rather than mocked data wherever
possible, so it exercises realistic component contracts.

### Concurrency And Metamorphic Integration Tests

File: `test/test_concurrency_integration.cpp`

This test builds a synthetic multi-sensor dataset and verifies that the final
outputs are invariant under different execution conditions.

It checks:

- same output with `1, 2, 3, 4, 8, 12, 16` OpenMP threads;
- repeated parallel runs to detect nondeterministic behavior;
- same output with different batch sizes, including boundary-sensitive sizes.

This directly targets the OpenMP areas of the code:

- parallel CSV/order assumptions;
- parallel per-sensor rule evaluation;
- parallel output formatting;
- deterministic alarm ordering after merging thread-local violations;
- state persistence across batches.

### End-To-End Golden-File Test Runner

File: `test/run_e2e_fixture.cmake`

This CMake script runs the real `astralog_processing` executable against a small
fixture and compares generated files byte-for-byte against expected outputs.

It verifies the complete chain:

```text
sensors.yaml
rules.json
telemetry.csv
    -> astralog_processing
    -> valid_data.csv
    -> alarms.log
```

It runs the executable with multiple thread counts and batch sizes:

```text
threads=1,  batch_size=1000
threads=2,  batch_size=1
threads=3,  batch_size=2
threads=4,  batch_size=3
threads=8,  batch_size=4
threads=12, batch_size=5
```

The expected result is byte-identical `valid_data.csv` and `alarms.log` for all
cases.

The script also runs one non-benchmark case and checks that batch audit files
are generated.

### Test Fixtures

Directory: `test/fixtures/`

Added deterministic fixtures:

- `test/fixtures/integration/sensors.yaml`
- `test/fixtures/integration/rules.json`
- `test/fixtures/integration/telemetry.csv`
- `test/fixtures/integration/expected_valid_data.csv`
- `test/fixtures/integration/expected_alarms.log`
- `test/fixtures/faults/telemetry_faults.csv`

The integration fixture covers:

- nominal timestamps;
- threshold anomaly;
- step-difference anomaly;
- stateful anomaly;
- correlation `AND`;
- correlation `OR`;
- cross-batch state behavior;
- deterministic alarm ordering by priority and rule id.

The fault fixture covers:

- unknown sensor handling;
- `ERR` marker rejection;
- non-numeric value rejection;
- missing field rejection;
- extra field rejection.

## CMake/CTest Integration

File changed: `CMakeLists.txt`

The `BUILD_TESTS` option now builds and registers:

```text
processing_tests
component_integration_tests
concurrency_integration_tests
e2e_golden_thread_batch_matrix
```

CTest labels were added:

```text
component_integration_tests: integration
concurrency_integration_tests: integration;concurrency;metamorphic
e2e_golden_thread_batch_matrix: integration;e2e;concurrency;metamorphic
```

## What The Tests Prove

The tests do not prove that the program is perfect; testing cannot do that.
They are designed to expose likely failures in the highest-risk areas:

- mismatched sensor-token interpretation between YAML, CSV, and rules;
- rules loaded correctly but not attached to the right sensor;
- timestamp groups split incorrectly by batching;
- stateful and step-difference state lost across batches;
- nondeterministic output caused by OpenMP scheduling;
- output formatting mismatches after correct anomaly detection;
- invalid telemetry corrupting valid output.

## How To Run

On Linux, WSL, or the G100 environment:

```bash
cmake -S . -B build_test -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON
cmake --build build_test -j
ctest --test-dir build_test --output-on-failure
```

To run only the new integration tests:

```bash
ctest --test-dir build_test -L integration --output-on-failure
```

To run only concurrency/metamorphic tests:

```bash
ctest --test-dir build_test -L concurrency --output-on-failure
ctest --test-dir build_test -L metamorphic --output-on-failure
```

## Local Verification Status

In the current Windows shell, a full compiled verification could not be executed
because the following tools are not available on `PATH`:

```text
cmake
ctest
g++
gcc
clang++
cl
make
ninja
bash
```

`wsl.exe` exists, but no WSL Linux distribution is installed.

The following verification was still completed locally:

- confirmed the expected test files exist;
- confirmed CMake registers all new tests;
- confirmed the integration fixture has 20 telemetry rows over 5 timestamps;
- confirmed expected golden output has 3 nominal lines and 7 alarm lines;
- confirmed the faulty CSV fixture has 2 valid rows and 5 invalid rows;
- confirmed the end-to-end CMake runner references the correct fixture and
  golden files.

The final required verification step is to run the CTest commands above in a
Linux/C++ environment.
