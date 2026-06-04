/**
 * @file types.hpp
 * @brief Core types, enumerations, and data structures for AstraLog-HPC.
 *
 * This header defines all shared types used across the processing pipeline:
 *   - Priority, RuleType, CompOp enumerations with string conversion
 *   - RuleDefinition: in-memory representation of a monitoring rule
 *   - SensorState: per-sensor mutable state for step-diff and stateful rules
 *   - ParsedRecord: a single validated CSV row
 *   - TimestampGroup: all sensor readings at a single timestamp
 *   - RuleViolation: a detected rule violation (simple or correlation)
 */

#ifndef ASTRALOG_TYPES_HPP
#define ASTRALOG_TYPES_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <stdexcept>

namespace astralog {

// ===========================================================================
// Enumerations
// ===========================================================================

/// Alert priority levels (LOW < MEDIUM < HIGH)
enum class Priority : uint8_t {
    LOW    = 0,
    MEDIUM = 1,
    HIGH   = 2
};

inline const char* priority_to_string(Priority p) noexcept {
    switch (p) {
        case Priority::LOW:    return "LOW";
        case Priority::MEDIUM: return "MEDIUM";
        case Priority::HIGH:   return "HIGH";
        default:               return "UNKNOWN";
    }
}

inline Priority string_to_priority(std::string_view s) {
    if (s == "LOW"    || s == "low")    return Priority::LOW;
    if (s == "MEDIUM" || s == "medium") return Priority::MEDIUM;
    if (s == "HIGH"   || s == "high")   return Priority::HIGH;
    throw std::invalid_argument(std::string("Unknown priority: ") + std::string(s));
}

/// Monitoring rule types
enum class RuleType : uint8_t {
    THRESHOLD   = 0,   ///< Simple absolute threshold check ("simple" in JSON)
    STEP_DIFF   = 1,   ///< Signed difference between consecutive readings
    STATEFUL    = 2,   ///< Consecutive violation persistence counter
    CORRELATION = 3    ///< Logical AND/OR across sub-rules at same timestamp
};

inline const char* ruletype_to_string(RuleType t) noexcept {
    switch (t) {
        case RuleType::THRESHOLD:   return "threshold";
        case RuleType::STEP_DIFF:   return "step_diff";
        case RuleType::STATEFUL:    return "stateful";
        case RuleType::CORRELATION: return "correlation";
        default:                    return "unknown";
    }
}

inline RuleType string_to_ruletype(std::string_view s) {
    if (s == "threshold" || s == "simple")          return RuleType::THRESHOLD;
    if (s == "step_difference" || s == "step_diff") return RuleType::STEP_DIFF;
    if (s == "stateful")                            return RuleType::STATEFUL;
    if (s == "correlation")                         return RuleType::CORRELATION;
    throw std::invalid_argument(std::string("Unknown rule type: ") + std::string(s));
}

/// Comparison operators for rule evaluation
enum class CompOp : uint8_t {
    GT, GE, LT, LE, EQ, NE
};

inline CompOp string_to_compop(std::string_view s) {
    if (s == ">")  return CompOp::GT;
    if (s == ">=") return CompOp::GE;
    if (s == "<")  return CompOp::LT;
    if (s == "<=") return CompOp::LE;
    if (s == "==") return CompOp::EQ;
    if (s == "!=") return CompOp::NE;
    throw std::invalid_argument(std::string("Unknown operator: ") + std::string(s));
}

/// Inline comparison: lhs <op> rhs
inline bool evaluate_comparison(double lhs, CompOp op, double rhs) noexcept {
    switch (op) {
        case CompOp::GT: return lhs > rhs;
        case CompOp::GE: return lhs >= rhs;
        case CompOp::LT: return lhs < rhs;
        case CompOp::LE: return lhs <= rhs;
        case CompOp::EQ: return lhs == rhs;
        case CompOp::NE: return lhs != rhs;
        default:         return false;
    }
}

// ===========================================================================
// Rule Definition
// ===========================================================================

/**
 * @struct RuleDefinition
 * @brief In-memory representation of a single monitoring rule loaded from JSON.
 */
struct RuleDefinition {
    std::string rule_id;                   ///< Unique identifier (e.g. "R001")
    RuleType    type;                      ///< THRESHOLD, STEP_DIFF, STATEFUL, or CORRELATION
    std::string sensor_id;                 ///< Target sensor (empty for correlation)
    CompOp      op;                        ///< Comparison operator
    double      threshold;                 ///< Comparison value
    Priority    priority;                  ///< Alert priority

    int         consecutive_measurements;  ///< Required consecutive violations (stateful only)

    // Correlation-specific fields
    std::string              logic;        ///< "AND" or "OR"
    std::vector<std::string> sub_rules;    ///< Sub-rule IDs referenced by correlation

    // Runtime index: maps sensor_id → integer token for O(1) array access
    int         sensor_token_idx;

    RuleDefinition()
        : type(RuleType::THRESHOLD)
        , op(CompOp::GT)
        , threshold(0.0)
        , priority(Priority::LOW)
        , consecutive_measurements(1)
        , sensor_token_idx(-1)
    {}
};

// ===========================================================================
// Sensor State (per-sensor mutable state for step-diff and stateful rules)
// ===========================================================================

/**
 * @struct SensorState
 * @brief Per-sensor mutable state for sequential rule evaluation.
 *
 * Cache-line aligned (64 bytes) to avoid false sharing in OpenMP
 * parallel-for loops when different threads process adjacent sensors.
 */
struct alignas(64) SensorState {
    double           previous_value;          ///< Last observed reading
    bool             has_previous;            ///< True after first reading
    std::vector<int> consecutive_violations;  ///< Per-stateful-rule counters

    SensorState() : previous_value(0.0), has_previous(false) {}
};

// ===========================================================================
// Parsed Record (a single validated CSV row)
// ===========================================================================

struct ParsedRecord {
    std::string timestamp;
    std::string sensor_id;
    double      value;
    int         sensor_token;   ///< Index into sensor arrays, -1 if unknown

    ParsedRecord() : value(0.0), sensor_token(-1) {}
};

// ===========================================================================
// Timestamp Group (all sensor readings at a single timestamp)
// ===========================================================================

/**
 * @struct TimestampGroup
 * @brief Groups all valid sensor readings that share the same timestamp.
 *
 * Used for per-timestamp NOMINAL/ANOMALOUS decision logic.
 */
struct TimestampGroup {
    std::string timestamp;

    struct Reading {
        int         sensor_token;
        std::string sensor_id;
        double      value;
    };
    std::vector<Reading> readings;
};

// ===========================================================================
// Rule Violation (detected anomaly — simple or correlation)
// ===========================================================================

/**
 * @struct RuleViolation
 * @brief Represents a single rule violation at a given timestamp.
 *
 * For simple/step_diff/stateful rules: one sensor_id and one value.
 * For correlation rules: multiple sensor_ids and values from sub-rules.
 */
struct RuleViolation {
    std::string              rule_id;
    Priority                 priority;
    std::vector<std::string> sensor_ids;
    std::vector<double>      values;
};

} // namespace astralog

#endif // ASTRALOG_TYPES_HPP
