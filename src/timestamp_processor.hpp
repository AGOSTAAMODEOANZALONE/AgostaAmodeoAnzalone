/**
 * @file timestamp_processor.hpp
 * @brief Per-timestamp grouping, parallel rule evaluation, and correlation.
 *
 * ## Processing Pipeline
 *
 * Phase 1 (Sequential): Build timestamp groups
 *   Scan the ordered record list and group consecutive records that share
 *   the same timestamp into TimestampGroup objects.
 *
 * Phase 2 (Sequential): Build per-sensor reading sequences
 *   For each sensor, build an ordered list of (group_idx, reading_idx)
 *   pairs for O(1) lookup during evaluation.
 *
 * Phase 3 (Parallel): Per-sensor rule evaluation
 *   OpenMP parallel-for across sensors. Each thread processes one sensor's
 *   entire timeline, evaluating threshold, step_diff, and stateful rules.
 *   This exploits the fact that different sensors are fully independent.
 *   Step_diff and stateful rules require per-sensor sequential processing,
 *   which is naturally satisfied within each thread.
 *
 * Phase 4 (Sequential): Merge + Correlation evaluation
 *   Merge per-thread violation maps. Then walk timestamps sequentially to
 *   evaluate correlation rules (AND/OR of sub-rule results).
 *
 * Phase 5 (Parallel): Output formatting
 *   OpenMP parallel-for across timestamps. Each thread formats its
 *   assigned timestamp as either a NOMINAL or ANOMALOUS line.
 *
 * ## Parallelism Characteristics
 *
 *   - Phase 1-2: O(N) sequential scans, fast
 *   - Phase 3: parallelism ∝ number of sensors (12 in current config)
 *   - Phase 4: sequential but lightweight (correlation rules only)
 *   - Phase 5: parallelism ∝ number of timestamps (thousands+)
 */

#ifndef ASTRALOG_TIMESTAMP_PROCESSOR_HPP
#define ASTRALOG_TIMESTAMP_PROCESSOR_HPP

#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <iostream>
#include <chrono>
#include <utility>

#include <omp.h>

#include "types.hpp"
#include "rules_engine.hpp"
#include "output_formatter.hpp"

namespace astralog {

// ===========================================================================
// Per-sensor rule lookup table
// ===========================================================================

/**
 * @struct SensorRuleSet
 * @brief Pre-built lookup table mapping a sensor token to its applicable
 *        rules, sorted by priority within each type.
 */
struct SensorRuleSet {
    std::vector<const RuleDefinition*> threshold;
    std::vector<const RuleDefinition*> step_diff;

    struct StatefulEntry {
        const RuleDefinition* rule;
        int slot;  ///< Index into SensorState::consecutive_violations[]
    };
    std::vector<StatefulEntry> stateful;
};

// ===========================================================================
// PipelineState: cross-batch sensor state persistence
// ===========================================================================

/**
 * @struct PipelineState
 * @brief Wraps per-sensor SensorState objects so they persist across
 *        batch boundaries.
 *
 * When processing data in batches, step-diff rules need the previous
 * reading from the last record of the prior batch, and stateful rules
 * need their consecutive_violations counters to carry forward.
 *
 * Usage:
 *   PipelineState state;
 *   for each batch:
 *       result = process_pipeline(batch, rules, token_map, num_sensors, state);
 */
struct PipelineState {
    std::vector<SensorState> sensor_states;  ///< Indexed by sensor_token
    bool initialized = false;                ///< True after first batch

    /**
     * @brief Initialize sensor states for the given number of sensors
     *        and stateful rule slot counts.
     *
     * @param num_sensors     Total number of known sensors
     * @param sensor_rules    Per-sensor rule lookup tables (for stateful slot counts)
     */
    void initialize(int num_sensors,
                    const std::vector<SensorRuleSet>& sensor_rules) {
        if (initialized) return;
        sensor_states.resize(num_sensors);
        for (int s = 0; s < num_sensors; ++s) {
            sensor_states[s].consecutive_violations.assign(
                sensor_rules[s].stateful.size(), 0);
        }
        initialized = true;
    }
};

// ===========================================================================
// Phase 1: Build timestamp groups
// ===========================================================================

/**
 * @brief Group consecutive records by timestamp.
 *
 * Assumes records are in CSV order (timestamps are monotonically ordered).
 * Records with unknown sensor_token (-1) are still included for output
 * completeness but won't have rules evaluated against them.
 *
 * @param records  Parsed and validated CSV records in order
 * @return         Vector of TimestampGroup, one per unique timestamp
 */
inline std::vector<TimestampGroup> build_timestamp_groups(
    const std::vector<ParsedRecord>& records)
{
    std::vector<TimestampGroup> groups;
    if (records.empty()) return groups;

    groups.reserve(records.size() / 10);  // Rough estimate

    std::string current_ts;
    for (const auto& rec : records) {
        if (rec.timestamp != current_ts) {
            groups.push_back({});
            groups.back().timestamp = rec.timestamp;
            current_ts = rec.timestamp;
        }

        TimestampGroup::Reading reading;
        reading.sensor_token = rec.sensor_token;
        reading.sensor_id    = rec.sensor_id;
        reading.value        = rec.value;
        groups.back().readings.push_back(std::move(reading));
    }

    return groups;
}

// ===========================================================================
// Phase 2: Build per-sensor reading sequences
// ===========================================================================

/**
 * @brief For each sensor, build an ordered sequence of
 *        (group_idx, reading_idx) pairs.
 *
 * This allows Phase 3 to iterate a single sensor's readings in temporal
 * order without scanning all records.
 *
 * @param groups       Timestamp groups from Phase 1
 * @param num_sensors  Total number of known sensors
 * @return             sensor_sequences[sensor_token] = ordered list of
 *                     (group_idx, reading_idx)
 */
inline std::vector<std::vector<std::pair<size_t, size_t>>>
build_per_sensor_sequences(const std::vector<TimestampGroup>& groups,
                           int num_sensors)
{
    std::vector<std::vector<std::pair<size_t, size_t>>> sequences(num_sensors);

    // Pre-allocate: each sensor appears roughly once per timestamp
    for (auto& seq : sequences) {
        seq.reserve(groups.size());
    }

    for (size_t g = 0; g < groups.size(); ++g) {
        for (size_t r = 0; r < groups[g].readings.size(); ++r) {
            int token = groups[g].readings[r].sensor_token;
            if (token >= 0 && token < num_sensors) {
                sequences[token].push_back({g, r});
            }
        }
    }

    return sequences;
}

// ===========================================================================
// Phase 3: Parallel per-sensor rule evaluation
// ===========================================================================

/**
 * @brief Evaluate threshold, step_diff, and stateful rules for all sensors
 *        in parallel using OpenMP.
 *
 * Each sensor is processed by a single thread, ensuring correct sequential
 * ordering for step_diff (which needs the previous reading) and stateful
 * (which needs consecutive violation counts). Different sensors are fully
 * independent, providing parallelism proportional to the number of sensors.
 *
 * @param groups           Timestamp groups
 * @param sensor_sequences Per-sensor reading sequences from Phase 2
 * @param sensor_rules     Per-sensor rule lookup tables
 * @param num_sensors      Total number of known sensors
 * @param pipeline_state   Optional pointer to PipelineState for cross-batch
 *                         sensor state persistence. If non-null, sensor states
 *                         are read from and written back to this object.
 * @return                 Per-timestamp violation map: timestamp_idx → violations
 */
inline std::vector<std::vector<RuleViolation>> evaluate_rules_parallel(
    const std::vector<TimestampGroup>& groups,
    const std::vector<std::vector<std::pair<size_t, size_t>>>& sensor_sequences,
    const std::vector<SensorRuleSet>& sensor_rules,
    int num_sensors,
    PipelineState* pipeline_state = nullptr)
{
    const int num_threads = omp_get_max_threads();
    const size_t num_groups = groups.size();

    // Per-thread violation storage to avoid synchronization
    // thread_violations[tid] maps timestamp_idx → vector of violations
    std::vector<std::unordered_map<size_t, std::vector<RuleViolation>>>
        thread_violations(num_threads);

    #pragma omp parallel
    {
        int tid = omp_get_thread_num();
        auto& my_violations = thread_violations[tid];

        // Each sensor is processed independently → parallel for across sensors
        #pragma omp for schedule(dynamic)
        for (int s = 0; s < num_sensors; ++s) {
            const auto& seq = sensor_sequences[s];
            if (seq.empty()) continue;

            const auto& rules = sensor_rules[s];

            // Per-sensor state: use existing state from PipelineState if
            // available (cross-batch persistence), otherwise create fresh.
            SensorState state;
            if (pipeline_state && pipeline_state->initialized) {
                state = pipeline_state->sensor_states[s];
            } else {
                state.consecutive_violations.assign(rules.stateful.size(), 0);
            }

            for (const auto& [group_idx, reading_idx] : seq) {
                const auto& reading = groups[group_idx].readings[reading_idx];
                double value = reading.value;
                const std::string& sensor_id = reading.sensor_id;

                // --- THRESHOLD rules (sorted by priority) ---
                for (const auto* rule : rules.threshold) {
                    if (evaluate_threshold(value, *rule)) {
                        my_violations[group_idx].push_back({
                            rule->rule_id,
                            rule->priority,
                            {sensor_id},
                            {value}
                        });
                    }
                }

                // --- STEP_DIFF rules (signed delta, no abs!) ---
                if (state.has_previous) {
                    for (const auto* rule : rules.step_diff) {
                        if (evaluate_step_diff(value, state, *rule)) {
                            my_violations[group_idx].push_back({
                                rule->rule_id,
                                rule->priority,
                                {sensor_id},
                                {value}
                            });
                        }
                    }
                }

                // --- STATEFUL rules ---
                for (size_t si = 0; si < rules.stateful.size(); ++si) {
                    const auto& entry = rules.stateful[si];
                    if (evaluate_stateful(value, state, *entry.rule,
                                          entry.slot)) {
                        my_violations[group_idx].push_back({
                            entry.rule->rule_id,
                            entry.rule->priority,
                            {sensor_id},
                            {value}
                        });
                    }
                }

                // Update sensor state for next iteration
                state.previous_value = value;
                state.has_previous = true;
            }

            // Write back final sensor state for cross-batch persistence
            if (pipeline_state) {
                pipeline_state->sensor_states[s] = state;
            }
        }
    }

    // Merge per-thread violations into per-timestamp vector
    std::vector<std::vector<RuleViolation>> result(num_groups);
    for (int t = 0; t < num_threads; ++t) {
        for (auto& [ts_idx, violations] : thread_violations[t]) {
            for (auto& v : violations) {
                result[ts_idx].push_back(std::move(v));
            }
        }
    }

    return result;
}

// ===========================================================================
// Phase 4: Correlation evaluation
// ===========================================================================

/**
 * @brief Evaluate correlation rules across all timestamps.
 *
 * For each timestamp, checks if the sub-rules referenced by each
 * correlation rule have fired. If the logical condition (AND/OR) is
 * satisfied, appends a correlation violation.
 *
 * Must run after Phase 3 so that all sub-rule results are available.
 *
 * @param groups              Timestamp groups
 * @param timestamp_violations Per-timestamp violations (modified in-place)
 * @param correlation_rules   All correlation-type rules (sorted by priority)
 */
inline void evaluate_correlations(
    const std::vector<TimestampGroup>& groups,
    std::vector<std::vector<RuleViolation>>& timestamp_violations,
    const std::vector<const RuleDefinition*>& correlation_rules)
{
    if (correlation_rules.empty()) return;

    for (size_t g = 0; g < groups.size(); ++g) {
        if (timestamp_violations[g].empty()) continue;

        // Build lookup: which rule_ids fired at this timestamp, and their data
        std::unordered_set<std::string> fired_rule_ids;
        std::unordered_map<std::string, std::pair<std::string, double>> rule_data;

        for (const auto& v : timestamp_violations[g]) {
            fired_rule_ids.insert(v.rule_id);
            // Store the first sensor/value for sub-rule lookup
            if (!v.sensor_ids.empty()) {
                rule_data[v.rule_id] = {v.sensor_ids[0], v.values[0]};
            }
        }

        // Evaluate each correlation rule
        for (const auto* rule : correlation_rules) {
            bool triggered = false;

            if (rule->logic == "AND") {
                triggered = true;
                for (const auto& sr_id : rule->sub_rules) {
                    if (fired_rule_ids.find(sr_id) == fired_rule_ids.end()) {
                        triggered = false;
                        break;
                    }
                }
            } else if (rule->logic == "OR") {
                triggered = false;
                for (const auto& sr_id : rule->sub_rules) {
                    if (fired_rule_ids.find(sr_id) != fired_rule_ids.end()) {
                        triggered = true;
                        break;
                    }
                }
            }

            if (triggered) {
                RuleViolation corr_v;
                corr_v.rule_id  = rule->rule_id;
                corr_v.priority = rule->priority;

                // Collect sensors and values from all sub-rules
                for (const auto& sr_id : rule->sub_rules) {
                    auto it = rule_data.find(sr_id);
                    if (it != rule_data.end()) {
                        corr_v.sensor_ids.push_back(it->second.first);
                        corr_v.values.push_back(it->second.second);
                    }
                }

                timestamp_violations[g].push_back(std::move(corr_v));
            }
        }
    }
}

// ===========================================================================
// Phase 5: Parallel output generation
// ===========================================================================

/**
 * @brief Generate output strings for all timestamps in parallel.
 *
 * For each timestamp:
 *   - If no violations → NOMINAL line (written to valid_data.csv)
 *   - If any violations → alarm lines (written to alarms.log)
 *
 * @param groups              Timestamp groups
 * @param timestamp_violations Per-timestamp violations
 * @param[out] valid_lines    NOMINAL output per timestamp (empty if anomalous)
 * @param[out] alarm_lines    Alarm output per timestamp (empty if nominal)
 */
inline void generate_output_parallel(
    const std::vector<TimestampGroup>& groups,
    const std::vector<std::vector<RuleViolation>>& timestamp_violations,
    std::vector<std::string>& valid_lines,
    std::vector<std::string>& alarm_lines)
{
    const size_t n = groups.size();
    valid_lines.resize(n);
    alarm_lines.resize(n);

    #pragma omp parallel for schedule(static)
    for (size_t g = 0; g < n; ++g) {
        if (timestamp_violations[g].empty()) {
            // NOMINAL: aggregated line with all sensor values
            valid_lines[g] = format_nominal_line(groups[g]);
        } else {
            // ANOMALOUS: one line per violated rule
            alarm_lines[g] = format_alarm_lines(groups[g].timestamp,
                                                timestamp_violations[g]);
        }
    }
}

// ===========================================================================
// Convenience: Full processing pipeline
// ===========================================================================

/**
 * @struct ProcessingResult
 * @brief Aggregated output of the full processing pipeline.
 */
struct ProcessingResult {
    std::vector<std::string> valid_lines;    ///< NOMINAL lines per timestamp
    std::vector<std::string> alarm_lines;    ///< Alarm lines per timestamp
    uint64_t                 num_timestamps; ///< Total unique timestamps
    uint64_t                 num_anomalies;  ///< Timestamps with ≥1 violation
    uint64_t                 num_nominal;    ///< Timestamps with 0 violations
    double                   eval_ms;        ///< Phase 3 wall time (ms)
    double                   total_ms;       ///< Full pipeline wall time (ms)
};

/**
 * @brief Run the full processing pipeline: group → evaluate → correlate → output.
 *
 * This overload accepts a PipelineState reference for cross-batch sensor
 * state persistence. When processing multiple batches, pass the same
 * PipelineState object to each call so that step-diff previous_values
 * and stateful consecutive_violations counters carry forward.
 *
 * @param records             Parsed and validated CSV records in order
 * @param all_rules           All loaded rules (will be categorised internally)
 * @param token_map           sensor_id → token mapping
 * @param num_sensors         Total number of known sensors
 * @param state               PipelineState for cross-batch persistence
 * @return                    ProcessingResult with output strings and stats
 */
inline ProcessingResult process_pipeline(
    const std::vector<ParsedRecord>& records,
    std::vector<RuleDefinition>& all_rules,
    const std::unordered_map<std::string, int>& token_map,
    int num_sensors,
    PipelineState& state)
{
    auto pipeline_start = std::chrono::high_resolution_clock::now();

    // --- Categorise and sort rules by type and priority ---

    // Assign sensor tokens
    assign_sensor_tokens(all_rules, token_map);

    // Categorise
    std::vector<const RuleDefinition*> threshold_rules;
    std::vector<const RuleDefinition*> step_diff_rules;
    std::vector<const RuleDefinition*> stateful_rules;
    std::vector<const RuleDefinition*> correlation_rules;

    for (const auto& r : all_rules) {
        switch (r.type) {
            case RuleType::THRESHOLD:   threshold_rules.push_back(&r); break;
            case RuleType::STEP_DIFF:   step_diff_rules.push_back(&r); break;
            case RuleType::STATEFUL:    stateful_rules.push_back(&r);  break;
            case RuleType::CORRELATION: correlation_rules.push_back(&r); break;
        }
    }

    // Sort each category by priority (HIGH first)
    sort_by_priority(threshold_rules);
    sort_by_priority(step_diff_rules);
    sort_by_priority(stateful_rules);
    sort_by_priority(correlation_rules);

    // Build per-sensor rule lookup tables
    std::vector<SensorRuleSet> sensor_rules(num_sensors);
    for (const auto* r : threshold_rules) {
        if (r->sensor_token_idx >= 0 && r->sensor_token_idx < num_sensors)
            sensor_rules[r->sensor_token_idx].threshold.push_back(r);
    }
    for (const auto* r : step_diff_rules) {
        if (r->sensor_token_idx >= 0 && r->sensor_token_idx < num_sensors)
            sensor_rules[r->sensor_token_idx].step_diff.push_back(r);
    }
    for (const auto* r : stateful_rules) {
        if (r->sensor_token_idx >= 0 && r->sensor_token_idx < num_sensors) {
            int slot = static_cast<int>(
                sensor_rules[r->sensor_token_idx].stateful.size());
            sensor_rules[r->sensor_token_idx].stateful.push_back({r, slot});
        }
    }

    // Initialize PipelineState on first batch
    state.initialize(num_sensors, sensor_rules);

    std::cerr << "[rules] Categorised: "
              << threshold_rules.size() << " threshold, "
              << step_diff_rules.size() << " step_diff, "
              << stateful_rules.size() << " stateful, "
              << correlation_rules.size() << " correlation\n";

    // --- Phase 1: Build timestamp groups ---
    auto phase1_start = std::chrono::high_resolution_clock::now();
    auto groups = build_timestamp_groups(records);
    auto phase1_end = std::chrono::high_resolution_clock::now();
    double phase1_ms = std::chrono::duration<double, std::milli>(
        phase1_end - phase1_start).count();

    std::cerr << "[pipeline] Phase 1: Grouped " << groups.size()
              << " unique timestamps in " << phase1_ms << " ms\n";

    // --- Phase 2: Build per-sensor reading sequences ---
    auto phase2_start = std::chrono::high_resolution_clock::now();
    auto sensor_sequences = build_per_sensor_sequences(groups, num_sensors);
    auto phase2_end = std::chrono::high_resolution_clock::now();
    double phase2_ms = std::chrono::duration<double, std::milli>(
        phase2_end - phase2_start).count();

    std::cerr << "[pipeline] Phase 2: Built per-sensor sequences in "
              << phase2_ms << " ms\n";

    // --- Phase 3: Parallel per-sensor rule evaluation ---
    auto phase3_start = std::chrono::high_resolution_clock::now();
    auto timestamp_violations = evaluate_rules_parallel(
        groups, sensor_sequences, sensor_rules, num_sensors, &state);
    auto phase3_end = std::chrono::high_resolution_clock::now();
    double phase3_ms = std::chrono::duration<double, std::milli>(
        phase3_end - phase3_start).count();

    std::cerr << "[pipeline] Phase 3: Parallel rule evaluation in "
              << phase3_ms << " ms (" << omp_get_max_threads()
              << " threads)\n";

    // --- Phase 4: Correlation evaluation ---
    auto phase4_start = std::chrono::high_resolution_clock::now();
    evaluate_correlations(groups, timestamp_violations, correlation_rules);
    auto phase4_end = std::chrono::high_resolution_clock::now();
    double phase4_ms = std::chrono::duration<double, std::milli>(
        phase4_end - phase4_start).count();

    std::cerr << "[pipeline] Phase 4: Correlation evaluation in "
              << phase4_ms << " ms\n";

    // --- Phase 5: Parallel output generation ---
    auto phase5_start = std::chrono::high_resolution_clock::now();
    std::vector<std::string> valid_lines, alarm_lines;
    generate_output_parallel(groups, timestamp_violations,
                             valid_lines, alarm_lines);
    auto phase5_end = std::chrono::high_resolution_clock::now();
    double phase5_ms = std::chrono::duration<double, std::milli>(
        phase5_end - phase5_start).count();

    std::cerr << "[pipeline] Phase 5: Parallel output formatting in "
              << phase5_ms << " ms\n";

    // --- Statistics ---
    uint64_t num_anomalies = 0;
    for (size_t g = 0; g < groups.size(); ++g) {
        if (!timestamp_violations[g].empty()) ++num_anomalies;
    }

    auto pipeline_end = std::chrono::high_resolution_clock::now();
    double total_ms = std::chrono::duration<double, std::milli>(
        pipeline_end - pipeline_start).count();

    ProcessingResult result;
    result.valid_lines    = std::move(valid_lines);
    result.alarm_lines    = std::move(alarm_lines);
    result.num_timestamps = groups.size();
    result.num_anomalies  = num_anomalies;
    result.num_nominal    = groups.size() - num_anomalies;
    result.eval_ms        = phase3_ms;
    result.total_ms       = total_ms;

    return result;
}

/**
 * @brief Convenience overload: single-pass pipeline (no cross-batch state).
 *
 * Creates a temporary PipelineState internally. Use when processing
 * the entire file as one batch (backward-compatible with pre-batching code).
 */
inline ProcessingResult process_pipeline(
    const std::vector<ParsedRecord>& records,
    std::vector<RuleDefinition>& all_rules,
    const std::unordered_map<std::string, int>& token_map,
    int num_sensors)
{
    PipelineState state;
    return process_pipeline(records, all_rules, token_map, num_sensors, state);
}

} // namespace astralog

#endif // ASTRALOG_TIMESTAMP_PROCESSOR_HPP
