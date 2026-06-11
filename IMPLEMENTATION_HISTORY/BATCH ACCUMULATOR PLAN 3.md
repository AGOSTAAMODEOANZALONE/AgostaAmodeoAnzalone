# Batch Accumulator Integration

Integrate a **Batch Accumulator** component between the CSV validation phase and the rule evaluation pipeline, matching the architecture described in the Phase 1 design document (Section 3.1.1). The accumulator supports two flushing strategies: **count-based** (every N valid messages) and **time-based** (every N milliseconds of wall-clock time).

## User Review Required

> [!IMPORTANT]
> **Sensor state must persist across batches.** Step-diff rules need `previous_value` and stateful rules need `consecutive_violations` counters from the previous batch. The pipeline will carry a `PipelineState` object forward between batches so that rules evaluate correctly across batch boundaries. Without this, stateful/step-diff rules would reset every batch and produce incorrect results.

> [!IMPORTANT]
> **Batch files are written to `output/batches/`.** Each flushed batch produces a timestamped `.txt` file (e.g. `batch_001_20250615_120000.txt`) containing the valid records in that batch. These files serve as an audit trail and can be inspected for debugging. The rule engine processes each batch from memory — it does **not** re-read the `.txt` file.

## Open Questions

> [!IMPORTANT]
> **Default batch size / interval**: I propose `--batch-size 1000` (count strategy) and `--batch-interval 5000` (5 seconds, time strategy) as sensible defaults. If you have a preference from your design document or the professor's guidance, let me know.

> [!NOTE]
> **Time-based strategy on file input**: Since we read from static CSV files (not a live MQTT stream), the time-based strategy uses **wall-clock elapsed time** from when the current batch started accumulating. On small files the entire file may fit in a single batch; on large files the natural parsing pace creates realistic batch boundaries.

## Proposed Changes

### New Component: Batch Accumulator

#### [NEW] [batch_accumulator.hpp](file:///c:/Users/anton/Desktop/AgostaAmodeoAnzalone/src/batch_accumulator.hpp)

New header-only module implementing the `BatchAccumulator` class:

```
enum class BatchStrategy { COUNT, TIME };

struct BatchConfig {
    BatchStrategy strategy;
    size_t        count_threshold;    // For COUNT: flush every N valid records
    double        time_threshold_ms;  // For TIME: flush every N ms
    std::string   batch_output_dir;   // Where to write .txt batch files
};

class BatchAccumulator {
    // Internal buffer of ParsedRecord
    // Flush counter (batch_number_)
    // Wall-clock timer for TIME strategy

public:
    BatchAccumulator(const BatchConfig& config);

    void add_record(ParsedRecord rec);  // Push one valid record
    bool should_flush() const;          // Check if flush threshold met
    std::vector<ParsedRecord> flush();  // Flush buffer → return records + write .txt file
    std::vector<ParsedRecord> flush_remaining();  // Final flush at end-of-file
    int  batch_count() const;           // How many batches flushed so far
};
```

**Batch file format** (`output/batches/batch_NNN_YYYYMMDD_HHMMSS.txt`):
```
# Batch 001 | Strategy: COUNT | Size: 1000 records
# Flushed at: 2025-11-15T12:00:05Z
timestamp,sensor_id,value
2025-11-15T12:00:00Z,TEMP-01,25.5
2025-11-15T12:00:00Z,PRES-01,101.3
...
```

---

### State Persistence Across Batches

#### [MODIFY] [timestamp_processor.hpp](file:///c:/Users/anton/Desktop/AgostaAmodeoAnzalone/src/timestamp_processor.hpp)

1. **New struct `PipelineState`** — wraps per-sensor `SensorState` objects so they can be passed in/out of the pipeline:
   ```cpp
   struct PipelineState {
       std::vector<SensorState> sensor_states;  // Indexed by sensor_token
       bool initialized = false;
   };
   ```

2. **Modify `evaluate_rules_parallel()`** — accept an optional `PipelineState*` parameter:
   - If provided and initialized, use the existing `SensorState` for each sensor instead of creating a fresh one
   - After evaluation, write back the final sensor states to the `PipelineState`

3. **Modify `process_pipeline()`** signature — accept and return `PipelineState`:
   ```cpp
   // Before:
   ProcessingResult process_pipeline(records, all_rules, token_map, num_sensors);
   // After:
   ProcessingResult process_pipeline(records, all_rules, token_map, num_sensors,
                                     PipelineState& state);
   ```

---

### Orchestration Changes

#### [MODIFY] [main.cpp](file:///c:/Users/anton/Desktop/AgostaAmodeoAnzalone/src/main.cpp)

1. **New CLI arguments**:
   - `--batch-strategy <count|time>` (default: `count`)
   - `--batch-size <N>` (default: `1000`, used with `count` strategy)
   - `--batch-interval <ms>` (default: `5000`, used with `time` strategy)

2. **New processing loop** replacing the current single-pass call:
   ```
   parse all CSV records (unchanged)
         ↓
   create BatchAccumulator with config
   create PipelineState (empty)
         ↓
   for each record in parsed_records:
       accumulator.add_record(record)
       if accumulator.should_flush():
           batch = accumulator.flush()
           result = process_pipeline(batch, rules, token_map, num_sensors, state)
           append result to output files
         ↓
   flush_remaining → process final partial batch
         ↓
   write summary statistics
   ```

3. **Output files opened in append mode** — since multiple batches write to the same `valid_data.csv` and `alarms.log`, files are opened once and kept open for the duration.

4. **Per-batch logging** — each batch prints a progress line to stderr:
   ```
   [batch] Batch 001: 1000 records → 850 nominal, 150 anomalous (12.3 ms)
   [batch] Batch 002: 1000 records → 920 nominal, 80 anomalous (11.8 ms)
   ...
   ```

---

### Documentation Update

#### [MODIFY] [README.md](file:///c:/Users/anton/Desktop/AgostaAmodeoAnzalone/README.md)

1. **Remove** the "No streaming/batching" simplification note
2. **Add** Batch Accumulator description to the Architecture section
3. **Add** `--batch-strategy`, `--batch-size`, `--batch-interval` to the CLI options table
4. **Update** the pipeline description to include the batching phase

---

## Verification Plan

### Manual Verification
- Review the generated batch `.txt` files in `output/batches/` to verify correct record counts and formatting
- Verify that `valid_data.csv` and `alarms.log` output is **identical** regardless of batch size (count=100 vs count=10000 vs entire file) — this confirms sensor state persistence is correct
- Check that stateful rule counters and step-diff previous values carry over correctly across batch boundaries
