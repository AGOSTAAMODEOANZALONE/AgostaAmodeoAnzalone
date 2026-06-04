# AstraLog-HPC — Full Track Gap Analysis (Group of 3)

> Comprehensive review of what's **missing, broken, or needs improvement** against the project spec.

---

## Summary of Current State

| Area | Status | Notes |
|---|---|---|
| Source code (`src/`) | ⚠️ Partial | Single-file engine with rules engine header; no modular structure |
| Test suite (`test/`) | ❌ Missing | `tests/` directory doesn't exist; CMakeLists references `tests/test_processing.cpp` but file is absent |
| CI/CD pipeline | ❌ Missing | No `.github/workflows/` directory at all |
| Singularity container | ⚠️ Stale | References Python, ZeroMQ, and old architecture; doesn't match current C++-only code |
| README | ⚠️ Incomplete | Placeholder values; missing many required sections |
| Output format compliance | ❌ Incorrect | `valid_data.csv` outputs per-sensor lines, not per-timestamp aggregated lines |
| Parallelization (3-person req.) | ⚠️ Weak | OpenMP parallel parse exists but rule evaluation is fully sequential |
| SLURM output files | ❌ Missing | `astralog_*.out` / `astralog_*.err` not committed to repo |

---

## 🔴 Critical Blockers (Must Fix)

### 1. `test/` directory is completely missing

> [!CAUTION]
> The spec says: *"test where you will store all test cases"*. Your repo has **zero test files**. The CMakeLists.txt references `tests/test_processing.cpp` but the file doesn't exist. This is a guaranteed score loss on both "implementation" and "CI/CD pipeline" components.

**What to do:**
- Create a `test/` directory (the spec says `test`, not `tests` — rename to match)
- Write C++ unit tests covering:
  - **Each rule type** (simple threshold, step_difference, stateful, correlation)
  - **Edge cases**: first reading for step_diff (no previous), stateful counter reset, correlation AND vs OR
  - **Validation**: malformed CSV lines, missing fields, non-numeric values, "ERR"/"CORRUPT" markers
  - **Output format correctness**: verify `valid_data.csv` and `alarms.log` exact formats
- Consider using a lightweight framework (Catch2 or Google Test) or even raw `assert()` based tests

---

### 2. No CI/CD Pipeline (`.github/workflows/`)

> [!CAUTION]
> The spec requires a CI/CD pipeline that *"automatically build[s] the C++ core and run[s] all tests on every push"*, builds the Singularity container, and automates deployment to G100. Your repo has **no `.github/workflows/` directory at all**.

**What to do:**
- Create `.github/workflows/ci.yml` with at minimum:
  - **Build**: compile the C++ project with CMake
  - **Test**: run the test suite (`cmake -DBUILD_TESTS=ON`)
  - **Container build**: build the Singularity/Apptainer `.sif` image
  - (Stretch) **Deploy**: `scp` the container + inputs to G100 and `sbatch` the job
- Use GitHub Secrets for any CINECA credentials (never hard-code)

---

### 3. Output format is **wrong** for `valid_data.csv`

> [!CAUTION]
> The spec mandates that when **no rules are violated** for a timestamp, you write **a single aggregated line** with **all sensor values for that timestamp**:
> ```
> TIMESTAMP; NOMINAL; [SENSOR_1]:[VALUE_1]|[SENSOR_2]:[VALUE_2]|...
> ```
> Your code (lines 770-778 of [main.cpp](file:///c:/Users/anton/Desktop/AgostaAmodeoAnzalone/src/main.cpp#L770-L778)) writes **one line per sensor per record**:
> ```
> TIMESTAMP;NOMINAL;TEMP-01:25.5
> ```
> This is fundamentally incorrect — the spec says one line per **timestamp** with pipe-separated sensor values.

**What to fix:**
- Group all valid (non-anomalous) records by timestamp
- If **all** sensors at a given timestamp are nominal, emit **one** aggregated line with all sensor values pipe-separated
- If **any** rule fires at that timestamp, emit **nothing** to `valid_data.csv` and instead write alarm lines to `alarms.log`

This is a **semantic correctness** issue that will directly fail verification.

---

### 4. `alarms.log` format has spacing issues

The spec format uses **spaces after semicolons**:
```
TIMESTAMP; RULE_ID; PRIORITY; VIOLATED_SENSOR(S); CURRENT_VALUE(S)
```

Your code (line 713-715) writes:
```
TIMESTAMP;RULE_ID;PRIORITY;SENSOR;VALUE
```

No spaces after `;`. The spec says *"the output formats must be strictly adhered to"*. Add a space after each `;`.

---

### 5. Step-difference rule logic is **wrong**

> [!WARNING]
> Your [evaluate_step_diff](file:///c:/Users/anton/Desktop/AgostaAmodeoAnzalone/src/rules_engine.hpp#L205-L210) function computes `std::abs(delta)` (absolute value of the difference), then applies the operator. But the spec example says:
> ```
> "operator": "<", "value": -2.0
> ```
> Meaning: *"the difference (current - previous) is less than -2.0"* — i.e., the **signed** delta is checked, not the absolute value.
>
> With `abs()`, a drop of -3.0 would become +3.0, and `3.0 < -2.0` is false — you'd **miss the alarm**.

**Fix:** Remove `std::abs()` and evaluate the raw signed delta:
```cpp
double delta = value - state.previous_value;
return evaluate_comparison(delta, rule.op, rule.threshold);
```

---

## 🟠 Major Issues

### 6. No per-timestamp grouping logic

The entire architecture processes records **line by line** from the CSV without grouping them by timestamp. The spec says:

> *"AstraLog-HPC should receive streamed data, filter out syntactically invalid packets and accumulate valid packets into a local batch file; then, when the batch reaches the size you specify, it should apply the four types of rules to all collected valid data."*

And crucially, for output:
- If **no rules** are violated for a timestamp → one NOMINAL line to `valid_data.csv` with **all** sensor values
- If **any rule** is violated → nothing to `valid_data.csv`; alarms to `alarms.log`

This is a **per-timestamp** decision. Your code must:
1. Group records by timestamp
2. Evaluate rules for all sensors at that timestamp
3. Decide NOMINAL vs ANOMALOUS for the **entire timestamp**

---

### 7. Parallelization is insufficient for a 3-person group

> [!IMPORTANT]
> The spec says: *"Groups composed of three students should also focus on improving rule processing performance by distributing/parallelizing tasks as much as possible."*

Your current parallelization:
- ✅ Parallel CSV parsing (OpenMP `parallel for`)
- ❌ Rule evaluation is fully sequential (lines 689-751, single-threaded loop)
- ❌ Output formatting is parallel but writes are sequential

**What to improve:**
- Parallelize rule evaluation across independent timestamps (each timestamp's rules are independent)
- Parallelize across sensor groups within a timestamp (threshold/step_diff rules for different sensors are independent)
- Document your parallelization strategy and any data dependencies (stateful rules require sequential sensor ordering)
- Benchmark: provide comparative timing results (1 thread vs N threads) in the README

---

### 8. Singularity container definition is stale

[Singularity.def](file:///c:/Users/anton/Desktop/AgostaAmodeoAnzalone/Singularity.def) installs Python, pip, pyzmq, ZeroMQ, and references `requirements.txt` — but your project is **pure C++ with OpenMP**. No Python, no ZeroMQ.

**Fix:**
- Remove Python, pip, ZeroMQ dependencies
- Add `libomp-dev` or `libgomp1` for OpenMP support
- Remove `requirements.txt` from `%files` (file doesn't exist in repo anyway)
- Fix `%runscript` — it references a Python command and master/worker architecture that doesn't exist

---

### 9. Priority-based rule evaluation ordering is missing

The spec says:
> *"a rule can have a priority... Such priority applies within a single rule type and means that rules with higher priority are evaluated before the others."*

Your code does not sort rules by priority before evaluation. Within each type (threshold, step_diff, etc.), HIGH-priority rules should be evaluated first, then MEDIUM, then LOW.

---

### 10. Batch accumulation is missing

The spec says:
> *"accumulate valid packets into a local batch file; then, when the batch reaches the size you specify, it should apply the four types of rules"*

Your code reads the entire CSV and processes everything in one pass. While this works functionally, the batching concept is a design requirement. At minimum, implement a configurable batch size and process in batches. This aligns with the architectural design you'll describe in Phase 1.

---

## 🟡 README Gaps

### 11. README is missing required content

The spec requires the README to include **all** of the following:

| Required Section | Present? | Notes |
|---|---|---|
| Names of all team members | ⚠️ | Present but with placeholder person codes ("12345678") |
| Selected track (full) | ✅ | Present |
| Role of each team member | ⚠️ | Placeholder "e.g., ..." text |
| Detailed activities performed | ❌ | Missing entirely |
| Effort spent in hours | ⚠️ | Placeholder "XXh" |
| Programming language & libraries | ⚠️ | Partially — mentions C++17 but incomplete |
| Code organization vs. architecture doc | ❌ | Missing — must explain how `src/` maps to your Phase 1 component diagram |
| Simplifications vs. architecture | ❌ | Missing — must explicitly state deviations |
| Distribution/parallelization approach | ❌ | **Required for 3-person group** |
| Test cases and rationale | ❌ | Missing (no tests exist) |
| Pipeline description | ⚠️ | Mentioned but no pipeline exists |
| Difficulties faced + solutions | ❌ | Missing entirely |
| **GenAI usage paragraph** | ❌ | **Required even if "not applicable"** |

---

## 🔵 Smaller Improvements

### 12. License copyright is wrong
[LICENSE](file:///c:/Users/anton/Desktop/AgostaAmodeoAnzalone/LICENSE) says `Copyright (c) 2026 Simone Reale` — that's your professor. Change to your group's names.

### 13. Stale file: `src/main.cpp.save`
Delete [main.cpp.save](file:///c:/Users/anton/Desktop/AgostaAmodeoAnzalone/src/main.cpp.save) — it's a backup file that shouldn't be in the repo.

### 14. Stale file: `src/ingestion/.antigravitycli/`
This is a tool artifact, not project code. Add to `.gitignore` and remove.

### 15. `.gitignore` is minimal
Add: `*.sif`, `*.o`, `*.out`, `*.err`, `src/main.cpp.save`, `.antigravitycli/`

### 16. Code structure: everything in 2 files
The spec expects modular, well-structured code. Having 824 lines in `main.cpp` and 587 in `rules_engine.hpp` is monolithic. Consider splitting into:
- `src/csv_parser.hpp` — mmap + tokenization
- `src/rules_engine.hpp` — rule definitions and evaluation (keep)
- `src/sensor_state.hpp` — sensor state management
- `src/output_formatter.hpp` — output writing logic
- `src/main.cpp` — orchestration only

### 17. Missing `requirements.txt`
Referenced in [Singularity.def](file:///c:/Users/anton/Desktop/AgostaAmodeoAnzalone/Singularity.def#L33) and [README.md](file:///c:/Users/anton/Desktop/AgostaAmodeoAnzalone/README.md#L25) but doesn't exist. Either create it or remove references.

---

## Prioritized Action Plan

Given your deadline is **June 14**, here's the order of attack:

| Priority | Task | Est. Effort |
|---|---|---|
| 🔴 P0 | Fix output format (`valid_data.csv` aggregation + `alarms.log` spacing) | 4-6h |
| 🔴 P0 | Fix step_diff rule logic (remove `abs()`) | 30min |
| 🔴 P0 | Write test suite in `test/` | 6-8h |
| 🔴 P0 | Create CI/CD pipeline (`.github/workflows/`) | 4-6h |
| 🟠 P1 | Implement per-timestamp grouping and batch logic | 4-6h |
| 🟠 P1 | Add priority-based rule ordering | 1h |
| 🟠 P1 | Fix Singularity.def (remove stale deps) | 1h |
| 🟠 P1 | Improve parallelization + benchmarks (3-person requirement) | 4-6h |
| 🟡 P2 | Complete README with all required sections | 2-3h |
| 🟡 P2 | Fix LICENSE copyright | 5min |
| 🟡 P2 | Clean up stale files + improve .gitignore | 30min |
| 🔵 P3 | Refactor code into more files | 2-3h |
| 🔵 P3 | Commit SLURM output files to repo | 30min |

**Total estimated effort: ~30-40 hours across 3 people ≈ 10-13 hours each over 10 days.**
