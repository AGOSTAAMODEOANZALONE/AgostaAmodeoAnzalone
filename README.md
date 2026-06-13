# AstraLog-HPC: Full Track Implementation

## **Software Engineering for HPC — A.Y. 2025-2026**

This repository contains the **Full Track** solution for the **AstraLog-HPC** project, developed as a simulated response to a "Call for Tenders" issued by the European Space Agency (ESA). AstraLog-HPC is a high-performance telemetry monitoring system that identifies anomalies in spacecraft subsystems by evaluating configurable rules against streamed sensor data.

---

## Team Members & Effort

| Name | Person Code | Role | Effort (Hours) |
| :--- | :--- | :--- | :--- |
| **Francesco Agosta** | 10898065 | Testing, Software & CI/CD Supervisor | 90h |
| **Gabriele Amodeo** | 10829738 | CI/CD, Software & Testing Supervisor | 80h |
| **Antonello Anzalone** | 10892053 | Software, Testing & CI/CD Supervisor | 100h |

---

## Selected Track

**Full Track** — Group of 3 students.

As a 3-person group, we focused on:
- Rule processing using input CSV telemetry data
- Improving rule processing performance through distribution/parallelization via OpenMP
- Automated test execution
- CI/CD pipeline for build, test, containerization, and HPC deployment

---

## Programming Language & Libraries

| Component | Technology | Purpose |
| :--- | :--- | :--- |
| Language | **C++17** | Core processing engine |
| Parallelism | **OpenMP** | Shared-memory multi-threading for parallel parsing, evaluation, and formatting |
| Memory I/O | **POSIX mmap(2)** | Zero-copy memory-mapped file access for high-throughput CSV ingestion |
| Build System | **CMake 3.16+** | Cross-platform build configuration |
| Container | **Singularity/Apptainer** | Reproducible execution environment for CINECA G100 |
| Scheduler | **SLURM** | HPC job submission on Galileo 100 |

No external libraries are required beyond the C++ standard library and OpenMP. JSON parsing and YAML parsing are implemented in-house to avoid dependencies.

---

##  Repository Structure

```text
.
├── CMakeLists.txt              # Build configuration (C++17, OpenMP)
├── Doxyfile                    # Doxygen API documentation configuration
├── Singularity.def             # Container definition (g++, cmake, OpenMP)
├── job.sh                      # SLURM script for Galileo 100
├── LICENSE                     # MIT License
├── build_and_run.sh            # Local build/run script with clean and benchmark modes
├── build_apptainer.sh          # Build Apptainer image locally using Docker (macOS friendly)
├── profile_gprof.sh            # Optional gprof profiling script
│
├── src/                        # Source code
│   ├── main.cpp                # Entry point & orchestration
│   ├── types.hpp               # Shared types, enums, data structures
│   ├── csv_parser.hpp          # Memory-mapped CSV parsing & validation
│   ├── yaml_parser.hpp         # Sensor YAML configuration loader
│   ├── rules_engine.hpp        # Rule loading (JSON) & evaluation functions
│   ├── batch_accumulator.hpp   # Count/Time batch accumulation strategy
│   ├── timestamp_processor.hpp # 5-phase parallel processing pipeline
│   └── output_formatter.hpp    # Spec-compliant output formatting
│
├── test/                       # Comprehensive test suite
│   ├── fixtures/               # Test cases inputs and expected outputs
│   ├── run_e2e_fixture.cmake   # CMake end-to-end verification script
│   ├── test_csv_parser.cpp     # CSV parsing unit tests
│   ├── test_helpers.hpp        # Helper utilities for tests
│   ├── test_integration_component.cpp   # Integration tests between components
│   ├── test_integration_concurrency.cpp # OpenMP concurrency & metamorphic tests
│   ├── test_pipeline.cpp       # Processing pipeline integration tests
│   ├── test_rules_engine.cpp   # Rules engine evaluation unit tests
│   └── test_types.cpp          # Data types unit tests
│
├── input/                      # Input data
│   ├── sensors_SAT_ALPHA.yaml  # Sensor configuration (12 sensors)
│   ├── rules_SAT_ALPHA.json    # Monitoring rules (20 rules)
│   └── telemetry/              # CSV telemetry data files
│       ├── export_sat_alpha_small.csv   (430 KB)
│       ├── export_sat_alpha_medium.csv  (4.3 MB)
│       └── export_sat_alpha_large.csv   (43 MB)
│
├── docs/                       # Documentation
    ├── ProjectSE4HPC2026.pdf   # Project description
|   ├── A_Y__2025_2026_Software_Engineering_for_AstraLog_HPC_Project_v_1_0.pdf 
│   ├── A_Y__2025_2026_Software_Engineering_for_AstraLog_HPC_Project_v_2_0.pdf 
│   └── doxygen_documentation.pdf    # Generated Doxygen API documentation in PDF format
│
└── output/                     # Generated outputs (gitignored)
    ├── valid_data.csv
    ├── alarms.log
    └── batches/                # Batch audit files (one .txt per flushed batch)
        ├── batch_001_YYYYMMDD_HHMMSS.txt
        └── ...
```

---

## Software Organisation & Architecture

### Relationship to the Phase 1 Design Document

The implementation follows the component architecture defined in our requirement analysis document (see `docs/`). The main components map as follows:

| Design Document Component | Implementation File(s) | Notes |
| :--- | :--- | :--- |
| Data Ingestion Module | `csv_parser.hpp` | Memory-mapped CSV parsing with parallel line processing |
| Validation & Filtering | `csv_parser.hpp` | Inline ESA-compliant validation (malformed JSON→CSV, schema, type errors) |
| Sensor Configuration | `yaml_parser.hpp` | Lightweight YAML parser for `sensors.yaml` |
| Rule Engine | `rules_engine.hpp` | Rule loading from JSON + 4 evaluation functions |
| Batch Accumulator | `batch_accumulator.hpp` | Count-based and time-based batch flushing with audit trail |
| Processing Pipeline | `timestamp_processor.hpp` | 5-phase parallel pipeline (grouping → evaluation → correlation → output) |
| Output Formatter | `output_formatter.hpp` | Spec-compliant `valid_data.csv` and `alarms.log` generation |
| Orchestrator | `main.cpp` | Thin wiring of all components, CLI argument parsing, precomputed rule setup, batch loop |

### Simplifications & Variations

The following simplifications were made:

1. **Header-only modules**: Rather than separate `.cpp` compilation units, all modules are implemented as header-only (inline) files included by `main.cpp`. This simplifies the build (single translation unit) while maintaining logical separation. For a larger codebase, separate compilation would be preferred.

2. **No MQTT ingestion**: As a 3-person group, we focus on file-based rule processing. The MQTT digital twin integration is a 4-person group requirement.

### Batch Accumulator

The **Batch Accumulator** component sits between CSV validation and rule evaluation, implementing the architecture described in our Phase 1 design document (Section 3.1.1, FR.3, FR.7, Use Case 3). It supports two flushing strategies and flushes only at timestamp boundaries, so all readings for a timestamp stay in the same batch. A `--benchmark` mode disables batch audit-file writes and per-batch phase logs for cleaner performance measurements.

| Strategy | Trigger | CLI Flag | Default |
| :--- | :--- | :--- | :--- |
| **COUNT** | Every N valid records | `--batch-size <n>` | 1000 |
| **TIME** | Every N ms of wall-clock time | `--batch-interval <ms>` | 5000 |

**Pipeline flow with batching:**

```
CSV File → mmap → Parallel Parse → Valid Records
                                        ↓
                               BatchAccumulator
                              (count or time flush)
                                        ↓
                          ┌───────────────────────────┐
                          │  for each flushed batch:  │
                          │    → write audit .txt*    │
                          │    → process_pipeline()   │
                          │    → append output files  │
                          └───────────────────────────┘
                                        ↓
                              valid_data.csv + alarms.log
```

**Cross-batch state persistence**: Sensor state (`previous_value` for step-diff rules, `consecutive_violations` for stateful rules) is carried forward between batches via a `PipelineState` object. Together with timestamp-boundary flushing, this ensures that rule evaluation and per-timestamp output remain stable across batch sizes.

Each flushed batch writes an audit file to `output/batches/` (e.g., `batch_001_20251115_120000.txt`) containing the CSV records in that batch, providing full traceability. Audit files are skipped when `--benchmark` is enabled.

Rules are prepared once after configuration loading: sensor tokens are assigned, rule vectors are sorted by priority, per-sensor lookup tables are built, and correlation rules are cached. Each batch reuses these precomputed structures while `PipelineState` carries step-diff and stateful history across batches.

Rule violations use small-buffer storage: the common single-sensor alarm stores its sensor/value pair inline and keeps a pointer to the stable rule definition instead of copying rule metadata. Larger correlation alarms can still spill to dynamic storage, preserving flexibility without paying heap-allocation cost for ordinary violations.

Before formatting `alarms.log`, violations for each timestamp are ordered by rule priority (`HIGH`, then `MEDIUM`, then `LOW`) with rule ID as a deterministic tie-breaker.

---

## Distribution & Parallelisation (3-Person Requirement)

### Strategy

We use **OpenMP shared-memory parallelism** across three phases of the pipeline:

| Phase | Parallelism | Granularity | Scaling Factor |
| :--- | :--- | :--- | :--- |
| CSV Parsing | `omp parallel for schedule(static)` | Per CSV line | ∝ number of lines (100K–1M+) |
| Rule Evaluation | `omp parallel for schedule(dynamic)` | Per sensor | ∝ number of sensors (12) |
| Output Formatting | `omp parallel for schedule(static)` | Per timestamp | ∝ number of timestamps (10K–100K+) |

### Why Per-Sensor Parallelism for Rule Evaluation?

**Step-difference** and **stateful** rules have **per-sensor sequential dependencies**:
- Step-diff needs the previous value of the *same sensor*
- Stateful needs a consecutive violation counter per sensor

Processing each sensor's full timeline on a single thread satisfies these dependencies naturally, while different sensors are fully independent and can run in parallel. Thread-local violation maps eliminate all locking on the critical path.


### Performance

The following benchmark timings were collected on the CINECA Galileo 100 (Intel CascadeLake nodes) using **8 OpenMP threads** across different batch sizes to compare overhead:

| Dataset | Size | Lines | Batch Size | Wall Time | Throughput |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **small** | 430 KB | ~7K | 500 | 23 ms | 0.601840 Mrows/s |
| | | | 1,000 | 138 ms | 0.428036 Mrows/s |
| **medium** | 4.3 MB | ~70K | 5,000 | 202 ms | 1.025890 Mrows/s |
| | | | 10,000 | 205 ms | 1.134030 Mrows/s |
| **large** | 43 MB | ~700K | 50,000 | 783 ms | 1.391560 Mrows/s |
| | | | 100,000 | 834 ms | 1.391560 Mrows/s |

---

## Test Cases & Rationale

The test suite covers the following areas:

1. **Rule evaluation correctness**: Unit tests for each rule type (threshold, step_difference, stateful, correlation) verifying correct alarm triggering.
2. **Step-diff signed delta**: Regression tests ensuring the signed delta correctly detects drops and rises.
3. **Stateful counter behaviour**: Tests for counter increment, reset on non-violation, and alarm triggering at exactly `consecutive_measurements`.
4. **CSV validation and token lookup**: Tests malformed lines, missing fields, non-numeric values, "ERR"/"CORRUPT" markers, and `string_view` sensor-token lookup.
5. **RuleViolation storage**: Tests inline sensor/value storage and fallback storage for larger correlation alarms.
6. **Cross-batch state persistence**: Tests that stateful counters carry across batch boundaries.
7. **Output format compliance**: Verifies exact `valid_data.csv` and `alarms.log` format against spec examples.
8. **Per-timestamp grouping**: Tests that NOMINAL is only emitted when zero rules fire, and that any violation suppresses the NOMINAL line.
9. **Batch boundary safety**: Tests that count-based flushing waits until a timestamp is complete.
10. **Benchmark mode**: Tests that audit files are skipped when benchmark mode disables them.

---

## CI/CD Pipeline

The pipeline is configured in `.github/workflows/` and automates the verification and deployment lifecycle using two main workflows:

1. **Pull Request Verification (`CI_testing.yaml`)**
   - **Trigger**: Runs on any pull request opened, updated, or reopened against the `main` branch.
   - **Environment**: Ubuntu environment with dependencies installed: `build-essential`, `cmake`, and `libomp-dev`.
   - **Execution**:
     - Configures and compiles the project with CMake in Release mode with `-DCI=ON`.
     - Runs the Google Test unit and integration suite using `ctest --output-on-failure`.
     - Executes an end-to-end integration and metamorphic test runner (`run_e2e_fixture.cmake`) verifying byte-for-byte correctness across varying thread counts and batch sizes.

2. **Apptainer Image Build & HPC Deployment (`build-apptainer.yml`)**
   - **Trigger**: Runs on pushes to `main` or via manual triggers.
   - **Build**: Compiles the Apptainer container image `astralog.sif` from `Singularity.def` using a setup action on an Ubuntu runner.
   - **HPC Deployment**: Deploys the built `.sif` image, along with `job.sh` and inputs, to the CINECA Galileo 100 cluster via `scp` using SSH keys and certificates stored in GitHub Secrets. It then triggers `ssh` to load the container execution module and submit the SLURM job via `sbatch job.sh`.

---

## Difficulties Faced

### Overcome

- **Per-timestamp output semantics**: The specification required producing a single aggregated NOMINAL line per timestamp containing all sensor values, but only when no rule violation occurred at that timestamp.
We designed the processing around timestamps by introducing a `TimestampGroup` structure. This enabled a two-pass strategy:
    1. Evaluate all rules for the timestamp.
    2. Decide whether to emit violations or a single aggregated NOMINAL output.
- **Parallelization with stateful and step-diff rules**: Stateful and step-diff rules require per-sensor sequential processing, but naively serializing the entire pipeline wastes cores. The solution was to parallelize *across sensors* while processing each sensor's timeline sequentially within a thread.

- **Decoupled Evaluation of Correlation Rules**: Correlation rules could not be evaluated in isolation because they depend on the results of other rules at the same timestamp. This made them unsuitable for the same parallel evaluation strategy used for single-sensor rules.
To solve this, rule processing was split into separate phases:
    - Phase 3 evaluates single-sensor rules in parallel.
    - Phase 4 sequentially walks the accumulated violations and evaluates correlation rules using logical AND / OR conditions.

    We also introduced a custom RuleViolation structure to aggregate multiple sub-rule violations efficiently, while avoiding unnecessary heap allocation in the common single-sensor case.

- **Deterministic Output Ordering**: Parallel rule evaluation introduced the risk of non-deterministic output ordering, especially when multiple sensors or timestamps were processed concurrently. To avoid inconsistent results between runs, the pipeline needed to preserve a deterministic ordering of timestamps and violations before writing the final output. This is especially important for testing, debugging, and comparing outputs against expected results.

- **Balancing Correctness and Performance**:  The project required careful trade-offs between maximizing parallelism and preserving the semantics of temporal rules. Some computations could be parallelized safely, while others had strict ordering constraints. The final design uses parallelism only where it does not compromise correctness, separating independent sensor-level work from timestamp-level aggregation and correlation evaluation.

-  **CINECA Certificate Authentication in CI**: Setting up automated SCP/SSH connections to Galileo 100 required injecting transient SSH user certificates. We resolved this by configuring dynamic key-writing steps within GitHub Actions.



### Persistent Challenges

-  **Limited Sensor-Level Parallelism**: Because Phase 3 parallelizes rule evaluation at the sensor level, the maximum available parallelism is bounded by the number of sensors in the dataset. For example, with only 12 sensors, a 48-core node such as Galileo 100 cannot be fully utilized during this phase. This limits scalability on high-core-count HPC systems.

- **Sequential Bottleneck in Correlation Evaluation**: Correlation rules are currently evaluated after single-sensor rules and require walking the accumulated violations. Since these rules depend on already computed results at the same timestamp, this phase is harder to parallelize. As the number of timestamps, violations, or correlation rules grows, this phase may become a bottleneck.

- **Load Imbalance Across Sensors**: Not all sensors necessarily have the same number of records or the same rule complexity. Some threads may finish earlier than others, leaving cores idle while heavier sensor timelines continue processing.

- **I/O and Output Generation Overhead**
As the dataset grows, output generation may become a limiting factor, especially if many violations are produced. Even if rule evaluation is parallelized, writing results in deterministic order can introduce synchronization or buffering overhead.

- **Scalability Beyond the Current Dataset**
The current design works well for the provided dataset, but its scalability depends strongly on dataset characteristics: number of sensors, number of timestamps, rule complexity, and violation frequency.

### Possible Future Improvements

- Introduce finer-grained parallelism within large sensor timelines where rule dependencies permit it.
- Parallelize correlation rule evaluation by partitioning timestamps when dependencies are local to a timestamp.
- Add load-balancing strategies for sensors with uneven amounts of data.
- Benchmark memory usage and cache behavior on larger datasets.
  Optimize output buffering to reduce I/O overhead.

---

## Usage of Generative AI

During this project, we utilized Generative AI to assist in **Supervised Code Generation** and **AI integration with GitHub Workflows**:

- **Supervised Code Generation**: Assisted in drafting lightweight custom YAML and JSON parsers, optimizing OpenMP loops, and writing test templates.
- **AI integration with GitHub Workflows**: Assisted in building the CI testing pipeline and integrating AI for automated code review and issue review.

All AI-generated outputs were critically reviewed, tested, and adapted to ensure correctness and alignment with our architectural design. We take full responsibility for every line of code and documentation in this repository.

---

## Local Setup & Usage

### Prerequisites

For a complete command reference covering local execution, SLURM execution,
fixed timestamp preprocessing, profiling, and tests, see
`docs/EXECUTION_GUIDE.md`.

- C++17 compatible compiler (e.g., `g++ >= 7`)
- `cmake >= 3.16`
- OpenMP support (typically included with `g++`)

### Build

```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
cd ..
```

Or use the convenience script:

```bash
./build_and_run.sh
./build_and_run.sh --clean
./build_and_run.sh --benchmark
CSV_PATH=input/telemetry/export_sat_alpha_large.csv BATCH_SIZE=100000 ./build_and_run.sh --benchmark
```

For cluster execution, `job.sh` defaults to benchmark mode to avoid batch audit I/O during timing runs. Set `BENCHMARK=0` when audit files are needed:

```bash
sbatch job.sh
sbatch --export=ALL,BENCHMARK=0 job.sh
sbatch --export=ALL,CSV_PATH=input/telemetry/export_sat_alpha_large.csv,BATCH_SIZE=100000 job.sh
```


### Run

```bash
./build/bin/astralog_processing \
    --csv input/telemetry/export_sat_alpha_small.csv \
    --rules input/rules_SAT_ALPHA.json \
    --sensors input/sensors_SAT_ALPHA.yaml \
    --output-dir output/
```

### Command-Line Options

| Option | Default | Description |
| :--- | :--- | :--- |
| `--csv <path>` | *(required)* | Path to telemetry CSV file |
| `--rules <path>` | `input/rules_SAT_ALPHA.json` | Path to rules JSON file |
| `--sensors <path>` | `input/sensors_SAT_ALPHA.yaml` | Path to sensors YAML file |
| `--output-dir <dir>` | `output/` | Output directory |
| `--threads <n>` | All available | Number of OpenMP threads |
| `--batch-strategy <s>` | `count` | Batch flushing strategy: `count` or `time` |
| `--batch-size <n>` | `1000` | Records per batch (count strategy) |
| `--batch-interval <ms>` | `5000` | Milliseconds per batch (time strategy) |
| `--benchmark` | disabled | Disable batch audit files and per-batch phase logs for timing runs |

### Output Files

- `output/valid_data.csv` — NOMINAL timestamps (all sensors, pipe-separated)
- `output/alarms.log` — Rule violations (one line per violated rule)
- `output/batches/` — Batch audit files (one `.txt` per flushed batch, unless `--benchmark` is enabled)

### Doxygen Documentation

API documentation can be generated locally using Doxygen and Graphviz (for class/caller/call diagrams):

```bash
doxygen Doxyfile
```

Once generated, open `html/index.html` in your web browser to browse the interactive class hierarchies, call graphs, and caller graphs.

Alternatively, a pre-compiled PDF version of the Doxygen documentation is available at [docs/doxygen_latex__1_.pdf](docs/doxygen_latex__1_.pdf).

---

## 📜 License

This project is licensed under the MIT License. See [LICENSE](LICENSE) for details.
