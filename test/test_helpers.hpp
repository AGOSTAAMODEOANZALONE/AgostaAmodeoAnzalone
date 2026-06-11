/**
 * @file test_helpers.hpp
 * @brief Shared factory helpers used across all unit/integration test files.
 *
 * Include this header (and nothing else from the test layer) whenever you
 * need to construct ParsedRecord, RuleDefinition, or RuleViolation objects
 * in a test.
 */

#pragma once

#include <string>

#include "rules_engine.hpp"
#include "types.hpp"

namespace astralog::test {

/// Build a minimal ParsedRecord.
inline ParsedRecord make_record(const std::string& timestamp,
                                int                sensor_token,
                                double             value)
{
    ParsedRecord r;
    r.timestamp    = timestamp;
    r.sensor_token = sensor_token;
    r.value        = value;
    return r;
}

/// Build a RuleDefinition with all common fields set.
inline RuleDefinition make_rule(const std::string& id,
                                RuleType           type,
                                const std::string& sensor_id,
                                CompOp             op,
                                double             threshold,
                                Priority           priority,
                                int                consec = 1)
{
    RuleDefinition r;
    r.rule_id                  = id;
    r.type                     = type;
    r.sensor_id                = sensor_id;
    r.op                       = op;
    r.threshold                = threshold;
    r.priority                 = priority;
    r.consecutive_measurements = consec;
    return r;
}

} // namespace astralog::test
