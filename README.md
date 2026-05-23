# 🚀 AstraLog-HPC: Full Track Implementation

## **Software Engineering for HPC - A.Y. 2025-2026**

This repository contains the **Full Track** solution for the **AstraLog-HPC** project, developed to respond to a simulated "Call for Tenders" issued by the European Space Agency (ESA).

---

## 🌌 AstraLog Control

Here you can access the official documentation hub and web interface for the **AstraLog-HPC** project.

**[Click here to access AstraLog Control](https://simonereale.github.io/astralog-control/)**

---

## 👥 Team Members & Effort

| Name Surname | Person Code | Role / Main Focus | Effort (Hours) |
| :--- | :--- | :--- | :--- |
| **Gabriele Amodeo** | 12345678 | e.g., Software Architect & Backend Logic | XXh |
| **Francesco Agosta** | 12345678 | e.g., DevOps, CI/CD Pipeline & SLURM | XXh |
| **Antonello Anzalone** | 12345678 | e.g., QA, Pytest & Singularity Container | XXh |

*(Note: If your group has 3 or 4 students, explicitly describe who handled the distribution/parallelization logic below).*

---

## 📁 Repository Structure

We adopted a hybrid-language repository structure, keeping C++ headers and source files colocated for maximum developer velocity.

```text
.
├── requirements.txt           # Python dependencies (paho-mqtt, pytest, pyzmq)
├── CMakeLists.txt             # C++ Build configuration
├── src/
│   ├── ingestion/             # The Python Defensive Boundary
│   │   ├── astralog_collector.py  
│   │   └── __init__.py
│   └── processing/            # The C++ Parallel Core
│       ├── main.cpp           # C++ Entry (connects to the ZeroMQ PULL socket)
│       ├── dispatcher.hpp     # Routing interfaces
│       ├── dispatcher.cpp
│       ├── worker.hpp         # Worker thread logic and state persistence
│       ├── worker.cpp
│       ├── sequencer.hpp      # Correlation Barrier and Logging Proxy logic
│       └── sequencer.cpp
├── tests/
│   ├── test_ingestion.py      # Python pytest suite
│   └── test_processing.cpp    # C++ unit tests 
├── docs/                      # Requirement analysis & Design documents
├── Singularity.def            # Container definition (Python + g++ + CMake + ZeroMQ)
└── job.sh                     # SLURM script for Galileo 100

```

---

## 🛠️ Software Organization & Architecture

### Languages and Libraries

* **Languages:** Python 3.9+ (Ingestion) & C++17 (Processing Core).
* **Libraries:** `paho-mqtt` (HiveMQ Broker), `pyzmq` & `libzmq` (ZeroMQ for IPC and Event Bus).

### The Distributed Architecture

To maximize parallel efficiency while maintaining a robust defensive boundary against corrupted telemetry, the architecture is split across five core interfaces:

1. **Initialization Interface:** A Python launcher parses `sensors.yaml` and `rules.json`, pre-sorts rules by priority, and injects these static routing parameters into the C++ environment at startup.
2. **Dataflow Pipes (Python):** Synchronous, in-memory generator chains (`Ingestor` $\rightarrow$ `Validator` $\rightarrow$ `Accumulator`) filter out malformed JSON and schema errors before data can consume memory in the batch buffer.
3. **ZeroMQ Bridge (Python $\rightarrow$ C++):** To bypass file I/O bottlenecks during batch handoffs, Python publishes flattened batches via a ZeroMQ `PUSH` socket, which are instantly consumed by the C++ Dispatcher's `PULL` socket via `ipc://`.
4. **Internal Dispatch Routing (C++):** The Dispatcher routes validated records to the Worker Pool, strictly sharding the data by `sensor_id` to allow lock-free state persistence.
5. **Event Stream (ZeroMQ Pub/Sub):** C++ Workers publish their boolean outcomes to a central `inproc://` bus. The Correlation Sequencer acts as a synchronization barrier at time $T_{n}$, consuming these results to evaluate logical rules, and republishing any complex anomalies. Finally, a single Logging Proxy thread consumes all terminal events to perform thread-safe, atomic appends to the parallel file system.

---

## 🎯 ESA Compliance & Implementation Details

Following the official project documentation, the system filters incoming data to handle real-world space communication noise:

1. **Malformed JSON:** Drops packets with invalid syntax.
2. **Schema Errors:** Ensures all mandatory fields (`timestamp`, `sensor_id`, `value`, `priority`) are present.
3. **Type Errors:** Verifies that sensor `value` is numerical.

The system supports two **Batch Accumulation Strategies**:

* **Count-based:** Flushes to the C++ core every *N* valid messages.
* **Time-based:** Flushes to the C++ core every *N* milliseconds.

---

## 💻 Local Setup & Usage

### 1. Prerequisites

Ensure you have Python 3.9+, a C++17 compatible compiler (e.g., `g++`), `cmake`, and ZeroMQ development headers (`libzmq3-dev`) installed on your system.

### 2. Build the C++ Core

```bash
mkdir build
cd build
cmake ..
make
cd ..

```

### 3. Running the System

Install the required Python dependencies:

```bash
pip install -r requirements.txt

```

Launch the system from the root directory (the launcher handles both the Python ingestion and C++ executable):

```bash
python3 -m src.ingestion.astralog_collector --mode count --limit 100

```

---

## 🧪 Testing & Rationale

* **Python Ingestion:** We implemented our test suite using `pytest` located in `tests/test_ingestion.py`. These tests verify the pipeline's defensive capabilities against corrupted JSON without requiring an active MQTT connection.
* **C++ Processing:** The C++ core logic (sharding, rule evaluation, and correlation) is tested in `tests/test_processing.cpp` ensuring that threading logic and stateful rule tracking perform correctly under simulated loads.

---

## 🚀 Pipeline & DevOps Workflow

* **CI/CD Pipeline:** Configured in `.github/workflows/main.yml` to automatically build the C++ core and run all tests on every push.
* **Containerization:** The `Singularity.def` file builds an isolated environment containing Python, GCC, CMake, and ZeroMQ to ensure reproducibility on the cluster.
* **HPC Execution:** `job.sh` is configured to run the containerized application on the **CINECA Galileo 100** cluster using the `g100_all_serial` partition.
