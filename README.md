# 🚀 AstraLog-HPC: Full Track Implementation
### **Software Engineering for HPC - A.Y. 2025-2026**

This repository contains the **Full Track** solution for the **AstraLog-HPC** project, developed to respond to a simulated "Call for Tenders" issued by the European Space Agency (ESA).

---

## 🌌 AstraLog Control
Here you can access the official documentation hub and web interface for the **AstraLog-HPC** project. 

**[Click here to access AstraLog Control](https://simonereale.github.io/astralog-control/)**

---

## 👥 Team Members & Effort

| Name Surname | Person Code | Role / Main Focus | Effort (Hours) |
| :--- | :--- | :--- | :--- |
| **Student 1** | 12345678 | e.g., Software Architect & Backend Logic | XXh |
| **Student 2** | 12345678 | e.g., DevOps, CI/CD Pipeline & SLURM | XXh |
| **Student 3** | 12345678 | e.g., QA, Pytest & Singularity Container | XXh |

*(Note: If your group has 3 or 4 students, explicitly describe who handled the distribution/parallelization logic below).*

---

## 📁 Expand this Repository Structure

```text
.
├── requirements.txt           # Python dependencies (paho-mqtt, pytest)
├── src/
│   ├── astralog_collector.py  # Core ingestion & ESA validation logic
│   ├── collector_output/      # Directory for batched telemetry files
│   └── __init__.py
└── tests/
    ├── test_collector.py      # Automated test suite
    └── __init__.py
├── Singularity.def            # Container definition (to be added)
├── job.sh                     # SLURM script for Galileo 100 (to be added)
└── docs/                      # Requirement analysis & Design documents
```

---

## 🛠️ Software Organization & Architecture (You can change this)

### Language and Libraries
- **Language:** Python 3.9+
- **Libraries:** `paho-mqtt` (for HiveMQ Broker connection), `pytest` (for unit testing).

### Architecture & Relation to Phase 1
The code is structured to strictly separate the **MQTT Ingestion layer** from the **Storage and Processing layers**. The `TelemetryCollector` class handles data validation and buffering, acting as the bridge between the real-time spacecraft digital twin and the offline HPC rule-processing system. 

---

## 🎯 ESA Compliance & Implementation Details

Following the official project documentation (Section 3), the system filters incoming data to handle real-world space communication noise:
1. **Malformed JSON:** Drops packets with invalid syntax.
2. **Schema Errors:** Ensures all mandatory fields (`timestamp`, `sensor_id`, `value`, `priority`) are present.
3. **Type Errors:** Verifies that sensor `value` is numerical.

The system supports two **Batch Accumulation Strategies**:
- **Count-based:** Flushes to a timestamped `.txt` file every *N* valid messages.
- **Time-based:** Flushes to a timestamped `.txt` file every *N* milliseconds.

---

## 💻 Local Setup & Usage

### 1. Installation
Install the required dependencies:
```bash
pip install -r requirements.txt
```

### 2. Running the Collector
To avoid module resolution errors, always run the module from the root directory using the `-m` flag.

**Example 1: Batch every 100 valid messages**
```bash
python3 -m src.astralog_collector --mode count --limit 100
```

**Example 2: Batch every 5000 milliseconds (5 seconds)**
```bash
python3 -m src.astralog_collector --mode time --limit 5000
```

---

## 🧪 Testing & Rationale

We implemented our test suite using `pytest`. The tests are located in `tests/test_collector.py`.
- **Rationale behind test cases:** The tests were designed to cover the core business logic without requiring an active MQTT connection.

To run the tests locally:
```bash
python3 -m pytest tests/
```

---

## 🚀 Pipeline & DevOps Workflow

- **CI/CD Pipeline:** Configured in `.github/workflows/main.yml` to automatically run `pytest` on every push.
- **Containerization:** A `Singularity.def` file builds an isolated Python environment to ensure reproducibility on the cluster.
- **HPC Execution:** `job.sh` is configured to run the containerized application on the **CINECA Galileo 100** cluster using the `g100_all_serial` partition.

---


## 📄 License
This project is licensed under the **MIT License**. See the `LICENSE` file for more details.
