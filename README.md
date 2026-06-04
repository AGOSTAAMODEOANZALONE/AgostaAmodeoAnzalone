# 🚀 AstraLog-HPC: Full Track Implementation

## **Software Engineering for HPC — A.Y. 2025-2026**

This repository contains the **Full Track** solution for the **AstraLog-HPC** project, developed as a simulated response to a "Call for Tenders" issued by the European Space Agency (ESA). AstraLog-HPC is a high-performance telemetry monitoring system that identifies anomalies in spacecraft subsystems by evaluating configurable rules against streamed sensor data.

---

## 👥 Team Members & Effort

| Name | Person Code | Role | Effort (Hours) |
| :--- | :--- | :--- | :--- |
| **Francesco Agosta** | <!-- TODO: insert --> | DevOps, CI/CD Pipeline & SLURM Integration | <!-- TODO --> |
| **Gabriele Amodeo** | <!-- TODO: insert --> | Software Architect & Core Engine Development | <!-- TODO --> |
| **Antonello Anzalone** | <!-- TODO: insert --> | QA, Testing & Containerization | <!-- TODO --> |

<!-- TODO: Fill in person codes and actual effort hours before submission -->

---

## 🎯 Selected Track

**Full Track** — Group of 3 students.

As a 3-person group, we focused on:
- Rule processing using input CSV telemetry data
- Improving rule processing performance through distribution/parallelization via OpenMP
- Automated test execution
- CI/CD pipeline for build, test, containerization, and HPC deployment

---

## 💻 Programming Language & Libraries

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

## 📁 Repository Structure

```text
.
├── CMakeLists.txt              # Build configuration (C++17, OpenMP)
├── Singularity.def             # Container definition (g++, cmake, OpenMP)
├── job.sh                      # SLURM script for Galileo 100
├── LICENSE                     # MIT License
├── build_and_run.sh            # Local build + run convenience script
├── clean_and_run.sh            # Clean build + run script
├── clean_outputs.sh            # Remove output files
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
├── test/                       # Test suite
│   └── (test files)
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
│   └── AmodeoAnzaloneAgosta.pdf  # Requirement analysis & design document
│
└── output/                     # Generated outputs (gitignored)
    ├── valid_data.csv
    ├── alarms.log
    └── batches/                # Batch audit files (one .txt per flushed batch)
        ├── batch_001_YYYYMMDD_HHMMSS.txt
        └── ...
```

---

## 🛠️ Software Organisation & Architecture

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
| Orchestrator | `main.cpp` | Thin wiring of all components, CLI argument parsing, batch loop |

### Simplifications & Variations

Compared to the Phase 1 architecture, the following simplifications were made:

1. **Header-only modules**: Rather than separate `.cpp` compilation units, all modules are implemented as header-only (inline) files included by `main.cpp`. This simplifies the build (single translation unit) while maintaining logical separation. For a larger codebase, separate compilation would be preferred.

2. **No MQTT ingestion**: As a 3-person group, we focus on file-based rule processing. The MQTT digital twin integration is a 4-person group requirement.

### Batch Accumulator

The **Batch Accumulator** component sits between CSV validation and rule evaluation, implementing the architecture described in our Phase 1 design document (Section 3.1.1, FR.3, FR.7, Use Case 3). It supports two flushing strategies:

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
                          ┌─────────────────────────┐
                          │  for each flushed batch: │
                          │    → write audit .txt    │
                          │    → process_pipeline()  │
                          │    → append output files │
                          └─────────────────────────┘
                                        ↓
                              valid_data.csv + alarms.log
```

**Cross-batch state persistence**: Sensor state (`previous_value` for step-diff rules, `consecutive_violations` for stateful rules) is carried forward between batches via a `PipelineState` object. This ensures that rules evaluate identically regardless of batch size — processing 10 batches of 100 records produces the same output as 1 batch of 1000 records.

Each flushed batch writes an audit file to `output/batches/` (e.g., `batch_001_20251115_120000.txt`) containing the CSV records in that batch, providing full traceability.

---

## ⚡ Distribution & Parallelisation (3-Person Requirement)

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

### Data Dependencies

```
                    Independent           Dependent
CSV lines      ──────────────→     (none — fully parallel)
Sensors        ──────────────→     (none across sensors)
Same sensor    ←─────────────      step_diff needs previous_value
               ←─────────────      stateful needs violation counter
Timestamps     ──────────────→     (none for output formatting)
Correlation    ←─────────────      needs all sub-rule results at same timestamp
```

### Performance

On the CINECA Galileo 100 (48-core Intel CascadeLake):

| Dataset | Size | Lines | Wall Time | Throughput |
| :--- | :--- | :--- | :--- | :--- |
| small | 430 KB | ~7K | <!-- TODO: fill after benchmarking --> | <!-- TODO --> |
| medium | 4.3 MB | ~70K | <!-- TODO --> | <!-- TODO --> |
| large | 43 MB | ~700K | <!-- TODO --> | <!-- TODO --> |

<!-- TODO: Fill in after running benchmarks on G100 -->

---

## 🧪 Test Cases & Rationale

<!-- TODO: Describe test cases after implementing test/ directory -->

The test suite covers the following areas:

1. **Rule evaluation correctness**: Unit tests for each rule type (threshold, step_difference, stateful, correlation) verifying correct alarm triggering.
2. **Step-diff signed delta**: Regression tests ensuring the signed delta (no `abs()`) correctly detects drops and rises.
3. **Stateful counter behaviour**: Tests for counter increment, reset on non-violation, and alarm triggering at exactly `consecutive_measurements`.
4. **CSV validation**: Tests for malformed lines, missing fields, non-numeric values, and "ERR"/"CORRUPT" markers.
5. **Output format compliance**: Verifies exact `valid_data.csv` and `alarms.log` format against spec examples.
6. **Per-timestamp grouping**: Tests that NOMINAL is only emitted when zero rules fire, and that any violation suppresses the NOMINAL line.

---

## 🚀 CI/CD Pipeline

<!-- TODO: Describe pipeline after implementing .github/workflows/ -->

The pipeline is configured in `.github/workflows/` and automates:

1. **Build**: Compiles the C++ project with CMake on every push
2. **Test**: Runs the full test suite (`cmake -DBUILD_TESTS=ON`)
3. **Container Build**: Builds the Singularity/Apptainer `.sif` image
4. **HPC Deployment**: Transfers the container to CINECA G100 and submits a SLURM job

Credentials for CINECA access are stored as GitHub Secrets (never hard-coded).

---

## 🧗 Difficulties Faced

<!-- TODO: Expand with actual experiences -->

### Overcome

1. **Per-timestamp output semantics**: The spec requires a single aggregated NOMINAL line per timestamp with *all* sensor values, but only if *zero* rules fire. This required redesigning the pipeline from per-record to per-timestamp processing, introducing the `TimestampGroup` data structure and a two-pass approach (evaluate → decide).

2. **Step-diff rule semantics**: The original implementation used `std::abs()` on the delta, which prevented detection of directional changes (e.g., drops). Reading the spec example carefully (`operator: "<", value: -2.0`) revealed that the signed delta must be compared directly.

3. **Parallelization with stateful rules**: Stateful and step-diff rules require per-sensor sequential processing, but naively serializing the entire pipeline wastes cores. The solution was to parallelize *across sensors* while processing each sensor's timeline sequentially within a thread.

4. **Output format compliance**: Getting the exact separator format right (semicolon-space `"; "` for fields, comma-space `", "` for correlation multi-values, pipe `|` for sensor aggregation) required careful attention to the spec examples.

### Ongoing / Not Yet Resolved

<!-- TODO: Document any unresolved issues -->

---

## 🤖 Usage of Generative AI

<!-- TODO: Fill in with actual GenAI usage details before submission -->

During this project, we used the following Generative AI tools:

- **Tool(s) used**: <!-- e.g., ChatGPT, GitHub Copilot, Claude, Gemini -->
- **Inputs provided**: <!-- e.g., prompts describing the desired functionality, code snippets for review -->
- **Outputs obtained**: <!-- e.g., code suggestions, architecture advice, debugging help -->
- **Verification & integration**: <!-- e.g., manually reviewed all generated code, tested for correctness, adapted to our architecture -->

All AI-generated outputs were critically reviewed, tested, and adapted to ensure correctness and alignment with our architectural design. We take full responsibility for every line of code and documentation in this repository.

---

## 💻 Local Setup & Usage

### Prerequisites

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

### Output Files

- `output/valid_data.csv` — NOMINAL timestamps (all sensors, pipe-separated)
- `output/alarms.log` — Rule violations (one line per violated rule)
- `output/batches/` — Batch audit files (one `.txt` per flushed batch)

---

## 📜 License

This project is licensed under the MIT License. See [LICENSE](LICENSE) for details.
