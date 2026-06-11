/**
 * @file output_formatter.hpp
 * @brief Spec-compliant output formatting for valid_data.csv and alarms.log.
 *
 * Output formats (bare semicolon separators, per ESA spec):
 *
 *   valid_data.csv (NOMINAL timestamps):
 *     TIMESTAMP;NOMINAL;SENSOR_1:VALUE_1|SENSOR_2:VALUE_2|...
 *
 *   alarms.log (ANOMALOUS timestamps):
 *     TIMESTAMP;RULE_ID;PRIORITY;SENSOR(S);VALUE(S)
 *
 * For correlation rules, sensors and values are comma separated:
 *     TIMESTAMP;R4;HIGH;TEMP-01,PRES-01;51.2,98.0
 */

#ifndef ASTRALOG_OUTPUT_FORMATTER_HPP
#define ASTRALOG_OUTPUT_FORMATTER_HPP

#include <cstdio>
#include <string>
#include <vector>

#include "types.hpp"

namespace astralog {

// ===========================================================================
// Value formatting
// ===========================================================================

/**
 * @brief Format a double to string, stripping trailing zeros after the
 *        decimal point but keeping at least one decimal digit.
 *
 * Examples: 35.979 → "35.979", 36.0 → "36.0", 100.0 → "100.0"
 */
inline std::string format_value(double val) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.6f", val);
    std::string s(buf);
    size_t dot = s.find('.');
    if (dot != std::string::npos) {
        size_t last_nonzero = s.find_last_not_of('0');
        if (last_nonzero != std::string::npos && last_nonzero > dot) {
            s.erase(last_nonzero + 1);
        } else if (last_nonzero == dot) {
            s.erase(dot + 2);  // Keep at least "X.0"
        }
    }
    return s;
}

// ===========================================================================
// NOMINAL line formatting
// ===========================================================================

/**
 * @brief Format a NOMINAL line for valid_data.csv.
 *
 * Format: TIMESTAMP;NOMINAL;SENSOR_1:VALUE_1|SENSOR_2:VALUE_2|...
 *
 * @param group              The timestamp group containing all sensor readings
 * @param inverse_token_map  Lookup: sensor_token → sensor_id string
 * @return                   Formatted line (with trailing newline)
 */
inline std::string format_nominal_line(
    const TimestampGroup& group,
    const std::vector<std::string>& inverse_token_map)
{
    std::string line;
    line.reserve(128);

    line += group.timestamp;
    line += ";NOMINAL;";

    for (size_t i = 0; i < group.readings.size(); ++i) {
        if (i > 0) line += '|';
        int token = group.readings[i].sensor_token;
        if (token >= 0 && static_cast<size_t>(token) < inverse_token_map.size()) {
            line += inverse_token_map[token];
        } else {
            line += "UNKNOWN";
        }
        line += ':';
        line += format_value(group.readings[i].value);
    }

    line += '\n';
    return line;
}

// ===========================================================================
// Alarm line formatting
// ===========================================================================

/**
 * @brief Format alarm lines for alarms.log.
 *
 * For simple/step_diff/stateful rules (single sensor):
 *   TIMESTAMP;RULE_ID;PRIORITY;SENSOR;VALUE
 *
 * For correlation rules (multiple sensors):
 *   TIMESTAMP;RULE_ID;PRIORITY;SENSOR_1,SENSOR_2;VALUE_1,VALUE_2
 *
 * @param timestamp          The timestamp string
 * @param violations         All rule violations at this timestamp
 * @param inverse_token_map  Lookup: sensor_token → sensor_id string
 * @return                   Concatenated alarm lines (each with trailing newline)
 */
inline std::string format_alarm_lines(
    const std::string& timestamp,
    const std::vector<RuleViolation>& violations,
    const std::vector<std::string>& inverse_token_map)
{
    std::string lines;
    lines.reserve(violations.size() * 80);

    for (const auto& v : violations) {
        lines += timestamp;
        lines += ';';
        lines += v.rule ? v.rule->rule_id : "UNKNOWN_RULE";
        lines += ';';
        lines += priority_to_string(v.rule ? v.rule->priority
                                           : Priority::LOW);
        lines += ';';

        // Sensor names (comma separated for correlation)
        for (size_t i = 0; i < v.size(); ++i) {
            if (i > 0) lines += ',';
            int token = v.sensor_token_at(i);
            if (token >= 0 && static_cast<size_t>(token) < inverse_token_map.size()) {
                lines += inverse_token_map[token];
            } else {
                lines += "UNKNOWN";
            }
        }

        lines += ';';

        // Values (comma separated for correlation)
        for (size_t i = 0; i < v.size(); ++i) {
            if (i > 0) lines += ',';
            lines += format_value(v.value_at(i));
        }

        lines += '\n';
    }

    return lines;
}

} // namespace astralog

#endif // ASTRALOG_OUTPUT_FORMATTER_HPP
