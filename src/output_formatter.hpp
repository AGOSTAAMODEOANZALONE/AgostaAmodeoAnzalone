/**
 * @file output_formatter.hpp
 * @brief Spec-compliant output formatting for valid_data.csv and alarms.log.
 *
 * Output formats (semicolons followed by spaces, per ESA spec):
 *
 *   valid_data.csv (NOMINAL timestamps):
 *     TIMESTAMP; NOMINAL; SENSOR_1:VALUE_1|SENSOR_2:VALUE_2|...
 *
 *   alarms.log (ANOMALOUS timestamps):
 *     TIMESTAMP; RULE_ID; PRIORITY; SENSOR(S); VALUE(S)
 *
 * For correlation rules, sensors and values are comma-space separated:
 *     TIMESTAMP; R4; HIGH; TEMP-01, PRES-01; 51.2, 98.0
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
 * Format: TIMESTAMP; NOMINAL; SENSOR_1:VALUE_1|SENSOR_2:VALUE_2|...
 *
 * @param group  The timestamp group containing all sensor readings
 * @return       Formatted line (with trailing newline)
 */
inline std::string format_nominal_line(const TimestampGroup& group) {
    std::string line;
    line.reserve(128);

    line += group.timestamp;
    line += "; NOMINAL; ";

    for (size_t i = 0; i < group.readings.size(); ++i) {
        if (i > 0) line += '|';
        line += group.readings[i].sensor_id;
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
 *   TIMESTAMP; RULE_ID; PRIORITY; SENSOR; VALUE
 *
 * For correlation rules (multiple sensors):
 *   TIMESTAMP; RULE_ID; PRIORITY; SENSOR_1, SENSOR_2; VALUE_1, VALUE_2
 *
 * @param timestamp  The timestamp string
 * @param violations All rule violations at this timestamp
 * @return           Concatenated alarm lines (each with trailing newline)
 */
inline std::string format_alarm_lines(
    const std::string& timestamp,
    const std::vector<RuleViolation>& violations)
{
    std::string lines;
    lines.reserve(violations.size() * 80);

    for (const auto& v : violations) {
        lines += timestamp;
        lines += "; ";
        lines += v.rule_id;
        lines += "; ";
        lines += priority_to_string(v.priority);
        lines += "; ";

        // Sensor IDs (comma-space separated for correlation)
        for (size_t i = 0; i < v.sensor_ids.size(); ++i) {
            if (i > 0) lines += ", ";
            lines += v.sensor_ids[i];
        }

        lines += "; ";

        // Values (comma-space separated for correlation)
        for (size_t i = 0; i < v.values.size(); ++i) {
            if (i > 0) lines += ", ";
            lines += format_value(v.values[i]);
        }

        lines += '\n';
    }

    return lines;
}

} // namespace astralog

#endif // ASTRALOG_OUTPUT_FORMATTER_HPP
