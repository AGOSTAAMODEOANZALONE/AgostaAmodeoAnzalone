/**
 * @file yaml_parser.hpp
 * @brief Lightweight YAML parser for sensor configuration files.
 *
 * Implements a simple line-based parser that extracts sensor IDs from
 * the sensors.yaml file without requiring external YAML libraries.
 * Looks for lines containing "- id:" or "id:" patterns.
 */

#ifndef ASTRALOG_YAML_PARSER_HPP
#define ASTRALOG_YAML_PARSER_HPP

#include <string>
#include <string_view>
#include <vector>
#include <fstream>
#include <stdexcept>
#include <cctype>

namespace astralog {

/**
 * @brief Parse a YAML sensor configuration file and extract all sensor IDs.
 *
 * @param filepath  Path to the sensors.yaml file
 * @return          Vector of sensor ID strings (e.g. "TEMP-001", "PRES-002")
 * @throws          std::runtime_error if the file cannot be opened
 */
inline std::vector<std::string> load_sensors_from_yaml(const std::string& filepath) {
    std::ifstream ifs(filepath);
    if (!ifs.is_open()) {
        throw std::runtime_error("Failed to open YAML file: " + filepath);
    }

    std::vector<std::string> sensor_ids;
    std::string line;

    while (std::getline(ifs, line)) {
        // Strip leading whitespace
        size_t start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) continue;
        std::string_view trimmed(line.c_str() + start, line.size() - start);

        // Strip trailing whitespace
        while (!trimmed.empty() &&
               std::isspace(static_cast<unsigned char>(trimmed.back()))) {
            trimmed.remove_suffix(1);
        }

        // Match "- id: VALUE" or "id: VALUE"
        std::string_view value_part;
        if (trimmed.substr(0, 5) == "- id:") {
            value_part = trimmed.substr(5);
        } else if (trimmed.substr(0, 3) == "id:") {
            value_part = trimmed.substr(3);
        } else {
            continue;
        }

        // Trim whitespace and quotes from the value
        size_t val_start = value_part.find_first_not_of(" \t'\"");
        if (val_start == std::string_view::npos) continue;
        std::string_view val = value_part.substr(val_start);
        while (!val.empty() &&
               (std::isspace(static_cast<unsigned char>(val.back())) ||
                val.back() == '\'' || val.back() == '"')) {
            val.remove_suffix(1);
        }

        if (!val.empty()) {
            sensor_ids.emplace_back(val);
        }
    }

    return sensor_ids;
}

} // namespace astralog

#endif // ASTRALOG_YAML_PARSER_HPP
