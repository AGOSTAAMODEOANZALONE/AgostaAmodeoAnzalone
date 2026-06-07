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
#include <algorithm>
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

/**
 * @struct PreparedRules
 * @brief Rule lookup structures built once after configuration loading.
 *
 * Pointers inside this structure refer to the stable RuleDefinition objects
 * owned by the rules vector loaded in main().
 */
struct PreparedRules {
    std::vector<SensorRuleSet> sensor_rules;
    std::vector<const RuleDefinition*> correlation_rules;
    size_t threshold_count = 0;
    size_t step_diff_count = 0;
    size_t stateful_count = 0;
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
 *   auto prepared = prepare_rules(rules, token_map, num_sensors);
 *   for each batch:
 *       result = process_pipeline(batch, prepared, inverse_token_map, state);
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
// Rule preparation
// ===========================================================================

/**
 * @brief Precompute all per-sensor and correlation rule lookup structures.
 *
 * This removes repeated rule categorisation, priority sorting, and sensor-rule
 * table construction from the per-batch processing path.
 */
inline PreparedRules prepare_rules(
    std::vector<RuleDefinition>& all_rules,
    const std::unordered_map<std::string, int>& token_map,
    int num_sensors)
{
    assign_sensor_tokens(all_rules, token_map);

    std::vector<const RuleDefinition*> threshold_rules;
    std::vector<const RuleDefinition*> step_diff_rules;
    std::vector<const RuleDefinition*> stateful_rules;
    std::vector<const RuleDefinition*> correlation_rules;

    threshold_rules.reserve(all_rules.size());
    step_diff_rules.reserve(all_rules.size());
    stateful_rules.reserve(all_rules.size());
    correlation_rules.reserve(all_rules.size());

    for (const auto& r : all_rules) {
        switch (r.type) {
            case RuleType::THRESHOLD:   threshold_rules.push_back(&r); break;
            case RuleType::STEP_DIFF:   step_diff_rules.push_back(&r); break;
            case RuleType::STATEFUL:    stateful_rules.push_back(&r);  break;
            case RuleType::CORRELATION: correlation_rules.push_back(&r); break;
        }
    }

    sort_by_priority(threshold_rules);
    sort_by_priority(step_diff_rules);
    sort_by_priority(stateful_rules);
    sort_by_priority(correlation_rules);

    PreparedRules prepared;
    prepared.threshold_count = threshold_rules.size();
    prepared.step_diff_count = step_diff_rules.size();
    prepared.stateful_count = stateful_rules.size();
    prepared.correlation_rules = std::move(correlation_rules);
    prepared.sensor_rules.resize(num_sensors);

    for (const auto* r : threshold_rules) {
        if (r->sensor_token_idx >= 0 && r->sensor_token_idx < num_sensors)
            prepared.sensor_rules[r->sensor_token_idx].threshold.push_back(r);
    }
    for (const auto* r : step_diff_rules) {
        if (r->sensor_token_idx >= 0 && r->sensor_token_idx < num_sensors)
            prepared.sensor_rules[r->sensor_token_idx].step_diff.push_back(r);
    }
    for (const auto* r : stateful_rules) {
        if (r->sensor_token_idx >= 0 && r->sensor_token_idx < num_sensors) {
            auto& stateful = prepared.sensor_rules[r->sensor_token_idx].stateful;
            int slot = static_cast<int>(stateful.size());
            stateful.push_back({r, slot});
        }
    }

    return prepared;
}

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

    // Better estimate: ~12 sensors per timestamp in typical satellite data
    const size_t estimated_groups = records.size() / 12 + 1;
    groups.reserve(estimated_groups);

    std::string current_ts;
    for (const auto& rec : records) {
        if (rec.timestamp != current_ts) {
            groups.push_back({});
            groups.back().timestamp = rec.timestamp;
            groups.back().readings.reserve(16);  // Typical sensor count
            current_ts = rec.timestamp;
        }

        groups.back().readings.push_back({rec.sensor_token, rec.value});
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

    // Pre-allocate: each sensor appears roughly once per timestamp, with
    // slack for slightly out-of-sync or duplicated sensor packets.
    const size_t reserve_per_sensor = groups.size() + groups.size() / 2 + 1;
    for (auto& seq : sequences) {
        seq.reserve(reserve_per_sensor);
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

    // Per-thread violation storage: flat pre-allocated 2D vector.
    // thread_violations[tid][group_idx] is a vector of violations.
    // This replaces the old unordered_map, eliminating all hashing overhead.
    std::vector<std::vector<std::vector<RuleViolation>>>
        thread_violations(num_threads,
                          std::vector<std::vector<RuleViolation>>(num_groups));

    #pragma omp parallel
    {
        int tid = omp_get_thread_num();
        auto& my_violations = thread_violations[tid];

        auto append_violation = [&](size_t group_idx,
                                    const RuleDefinition& rule,
                                    int sensor_token,
                                    double value) {
            auto& group_violations = my_violations[group_idx];
            if (group_violations.empty()) {
                group_violations.reserve(8);
            }
            RuleViolation violation(&rule);
            violation.add(sensor_token, value);
            group_violations.push_back(std::move(violation));
        };

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

                // --- THRESHOLD rules (sorted by priority) ---
                for (const auto* rule : rules.threshold) {
                    if (evaluate_threshold(value, *rule)) {
                        append_violation(group_idx, *rule,
                                         reading.sensor_token, value);
                    }
                }

                // --- STEP_DIFF rules (signed delta, no abs!) ---
                if (state.has_previous) {
                    for (const auto* rule : rules.step_diff) {
                        if (evaluate_step_diff(value, state, *rule)) {
                            append_violation(group_idx, *rule,
                                             reading.sensor_token, value);
                        }
                    }
                }

                // --- STATEFUL rules ---
                for (size_t si = 0; si < rules.stateful.size(); ++si) {
                    const auto& entry = rules.stateful[si];
                    if (evaluate_stateful(value, state, *entry.rule,
                                          entry.slot)) {
                        append_violation(group_idx, *entry.rule,
                                         reading.sensor_token, value);
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

    // Merge per-thread violations into per-timestamp vector.
    // O(1) array access replaces the old hash map iteration.
    std::vector<std::vector<RuleViolation>> result(num_groups);
    for (int t = 0; t < num_threads; ++t) {
        for (size_t g = 0; g < num_groups; ++g) {
            auto& src = thread_violations[t][g];
            if (!src.empty()) {
                auto& dst = result[g];
                dst.insert(dst.end(),
                           std::make_move_iterator(src.begin()),
                           std::make_move_iterator(src.end()));
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

    // Small struct for linear-scan lookup (replaces unordered_map).
    // Violations per timestamp are typically 1-5, so linear scan is
    // faster than hashing for these tiny N values.
    struct FiredEntry {
        const RuleDefinition* rule;  // non-owning pointer, avoids copy
        int   sensor_token;
        double value;
    };

    for (size_t g = 0; g < groups.size(); ++g) {
        if (timestamp_violations[g].empty()) continue;

        timestamp_violations[g].reserve(
            timestamp_violations[g].size() + correlation_rules.size());

        // Build flat lookup array of fired rule IDs and their data
        std::vector<FiredEntry> fired;
        fired.reserve(timestamp_violations[g].size());

        for (const auto& v : timestamp_violations[g]) {
            int st = v.empty() ? -1 : v.sensor_token_at(0);
            double val = v.empty() ? 0.0 : v.value_at(0);
            fired.push_back({v.rule, st, val});
        }

        // Lambda: linear scan to check if a rule_id fired
        auto find_fired = [&](const std::string& target) -> const FiredEntry* {
            for (const auto& f : fired) {
                if (f.rule && f.rule->rule_id == target) return &f;
            }
            return nullptr;
        };

        // Evaluate each correlation rule
        for (const auto* rule : correlation_rules) {
            bool triggered = false;

            if (rule->logic == "AND") {
                triggered = true;
                for (const auto& sr_id : rule->sub_rules) {
                    if (!find_fired(sr_id)) {
                        triggered = false;
                        break;
                    }
                }
            } else if (rule->logic == "OR") {
                triggered = false;
                for (const auto& sr_id : rule->sub_rules) {
                    if (find_fired(sr_id)) {
                        triggered = true;
                        break;
                    }
                }
            }

            if (triggered) {
                RuleViolation corr_v(rule);

                // Collect sensors and values from all sub-rules
                for (const auto& sr_id : rule->sub_rules) {
                    const FiredEntry* entry = find_fired(sr_id);
                    if (entry && entry->sensor_token >= 0) {
                        corr_v.add(entry->sensor_token, entry->value);
                    }
                }

                timestamp_violations[g].push_back(std::move(corr_v));
            }
        }
    }
}

inline void sort_violations_for_output(
    std::vector<std::vector<RuleViolation>>& timestamp_violations)
{
    auto priority_rank = [](const RuleViolation& v) {
        return v.rule ? static_cast<int>(v.rule->priority) : -1;
    };

    auto rule_id = [](const RuleViolation& v) -> const std::string& {
        static const std::string unknown = "UNKNOWN_RULE";
        return v.rule ? v.rule->rule_id : unknown;
    };

    for (auto& violations : timestamp_violations) {
        std::stable_sort(
            violations.begin(), violations.end(),
            [&](const RuleViolation& a, const RuleViolation& b) {
                int pa = priority_rank(a);
                int pb = priority_rank(b);
                if (pa != pb) return pa > pb;  // HIGH > MEDIUM > LOW
                return rule_id(a) < rule_id(b);
            });
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
    const std::vector<std::string>& inverse_token_map,
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
            valid_lines[g] = format_nominal_line(groups[g], inverse_token_map);
        } else {
            // ANOMALOUS: one line per violated rule
            alarm_lines[g] = format_alarm_lines(groups[g].timestamp,
                                                timestamp_violations[g],
                                                inverse_token_map);
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
 * @param prepared_rules      Precomputed per-sensor and correlation rules
 * @param inverse_token_map   sensor_token to sensor_id lookup
 * @param state               PipelineState for cross-batch persistence
 * @param verbose             Emit per-phase timing logs when true
 * @return                    ProcessingResult with output strings and stats
 */
inline ProcessingResult process_pipeline(
    const std::vector<ParsedRecord>& records,
    const PreparedRules& prepared_rules,
    const std::vector<std::string>& inverse_token_map,
    PipelineState& state,
    bool verbose = true)
{
    auto pipeline_start = std::chrono::high_resolution_clock::now();
    const int num_sensors = static_cast<int>(prepared_rules.sensor_rules.size());

    // Initialize PipelineState on first batch
    state.initialize(num_sensors, prepared_rules.sensor_rules);

    // --- Phase 1: Build timestamp groups ---
    auto phase1_start = std::chrono::high_resolution_clock::now();
    auto groups = build_timestamp_groups(records);
    auto phase1_end = std::chrono::high_resolution_clock::now();
    double phase1_ms = std::chrono::duration<double, std::milli>(
        phase1_end - phase1_start).count();

    if (verbose) {
        std::cerr << "[pipeline] Phase 1: Grouped " << groups.size()
                  << " unique timestamps in " << phase1_ms << " ms\n";
    }

    // --- Phase 2: Build per-sensor reading sequences ---
    auto phase2_start = std::chrono::high_resolution_clock::now();
    auto sensor_sequences = build_per_sensor_sequences(groups, num_sensors);
    auto phase2_end = std::chrono::high_resolution_clock::now();
    double phase2_ms = std::chrono::duration<double, std::milli>(
        phase2_end - phase2_start).count();

    if (verbose) {
        std::cerr << "[pipeline] Phase 2: Built per-sensor sequences in "
                  << phase2_ms << " ms\n";
    }

    // --- Phase 3: Parallel per-sensor rule evaluation ---
    auto phase3_start = std::chrono::high_resolution_clock::now();
    auto timestamp_violations = evaluate_rules_parallel(
        groups, sensor_sequences, prepared_rules.sensor_rules,
        num_sensors, &state);
    auto phase3_end = std::chrono::high_resolution_clock::now();
    double phase3_ms = std::chrono::duration<double, std::milli>(
        phase3_end - phase3_start).count();

    if (verbose) {
        std::cerr << "[pipeline] Phase 3: Parallel rule evaluation in "
                  << phase3_ms << " ms (" << omp_get_max_threads()
                  << " threads)\n";
    }

    // --- Phase 4: Correlation evaluation ---
    auto phase4_start = std::chrono::high_resolution_clock::now();
    evaluate_correlations(groups, timestamp_violations,
                          prepared_rules.correlation_rules);
    sort_violations_for_output(timestamp_violations);
    auto phase4_end = std::chrono::high_resolution_clock::now();
    double phase4_ms = std::chrono::duration<double, std::milli>(
        phase4_end - phase4_start).count();

    if (verbose) {
        std::cerr << "[pipeline] Phase 4: Correlation evaluation in "
                  << phase4_ms << " ms\n";
    }

    // --- Phase 5: Parallel output generation ---
    auto phase5_start = std::chrono::high_resolution_clock::now();
    std::vector<std::string> valid_lines, alarm_lines;
    generate_output_parallel(groups, timestamp_violations,
                             inverse_token_map,
                             valid_lines, alarm_lines);
    auto phase5_end = std::chrono::high_resolution_clock::now();
    double phase5_ms = std::chrono::duration<double, std::milli>(
        phase5_end - phase5_start).count();

    if (verbose) {
        std::cerr << "[pipeline] Phase 5: Parallel output formatting in "
                  << phase5_ms << " ms\n";
    }

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
 * @brief Backward-compatible overload that prepares rules for this call.
 */
inline ProcessingResult process_pipeline(
    const std::vector<ParsedRecord>& records,
    std::vector<RuleDefinition>& all_rules,
    const std::unordered_map<std::string, int>& token_map,
    const std::vector<std::string>& inverse_token_map,
    int num_sensors,
    PipelineState& state,
    bool verbose = true)
{
    PreparedRules prepared_rules =
        prepare_rules(all_rules, token_map, num_sensors);
    return process_pipeline(records, prepared_rules, inverse_token_map,
                            state, verbose);
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
    const std::vector<std::string>& inverse_token_map,
    int num_sensors)
{
    PipelineState state;
    return process_pipeline(records, all_rules, token_map,
                            inverse_token_map, num_sensors, state);
}

} // namespace astralog

#endif // ASTRALOG_TIMESTAMP_PROCESSOR_HPP
