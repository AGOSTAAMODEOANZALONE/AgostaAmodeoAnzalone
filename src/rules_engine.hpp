/**
 * @file rules_engine.hpp
 * @brief Rule loading from JSON and inline rule evaluation functions.
 *
 * Components:
 *   - MiniJsonParser: dependency-free JSON parser for rules.json
 *   - load_rules(): deserializes JSON array → vector<RuleDefinition>
 *   - sort_rules_by_priority(): ensures HIGH→MEDIUM→LOW evaluation order
 *   - evaluate_threshold(): simple absolute threshold check
 *   - evaluate_step_diff(): signed delta between consecutive readings
 *   - evaluate_stateful(): consecutive violation persistence counter
 *
 * Design notes:
 *   - step_diff uses SIGNED delta (current - previous), NOT abs().
 *     This matches the spec: operator "<" with value -2.0 means
 *     "delta < -2.0", detecting drops greater than 2 units.
 *   - All evaluators are inline and non-allocating on the hot path.
 */

#ifndef ASTRALOG_RULES_ENGINE_HPP
#define ASTRALOG_RULES_ENGINE_HPP

#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <algorithm>
#include <cctype>

#include "types.hpp"

namespace astralog {

// ===========================================================================
// Minimal JSON parser (dependency-free)
// ===========================================================================

namespace detail {

class MiniJsonParser {
public:
    struct JsonValue {
        enum Type { STRING, NUMBER, ARRAY, OBJECT, NONE } type = NONE;
        std::string                                     str_val;
        double                                          num_val = 0.0;
        std::vector<JsonValue>                          arr_val;
        std::vector<std::pair<std::string, JsonValue>>  obj_val;

        const JsonValue* find(const std::string& key) const {
            for (const auto& kv : obj_val)
                if (kv.first == key) return &kv.second;
            return nullptr;
        }
        std::string as_string(const std::string& def = "") const {
            return (type == STRING) ? str_val : def;
        }
        double as_number(double def = 0.0) const {
            return (type == NUMBER) ? num_val : def;
        }
        int as_int(int def = 0) const {
            return (type == NUMBER) ? static_cast<int>(num_val) : def;
        }
    };

    static JsonValue parse(const std::string& input) {
        MiniJsonParser p(input);
        return p.parse_value();
    }

private:
    std::string_view src_;
    size_t           pos_;

    explicit MiniJsonParser(const std::string& s) : src_(s), pos_(0) {}

    void skip_ws() {
        while (pos_ < src_.size() &&
               std::isspace(static_cast<unsigned char>(src_[pos_])))
            ++pos_;
    }
    char peek() const {
        if (pos_ >= src_.size())
            throw std::runtime_error("MiniJsonParser: unexpected end of input");
        return src_[pos_];
    }
    char advance() {
        if (pos_ >= src_.size())
            throw std::runtime_error("MiniJsonParser: unexpected end of input");
        return src_[pos_++];
    }
    void expect(char c) {
        skip_ws();
        if (advance() != c)
            throw std::runtime_error(
                std::string("MiniJsonParser: expected '") + c +
                "' at position " + std::to_string(pos_ - 1));
    }

    JsonValue parse_value() {
        skip_ws();
        char c = peek();
        if (c == '"')  return parse_string_value();
        if (c == '{')  return parse_object();
        if (c == '[')  return parse_array();
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c)))
            return parse_number();
        if (c == 't' || c == 'f') return parse_bool();
        if (c == 'n') return parse_null();
        throw std::runtime_error(
            std::string("MiniJsonParser: unexpected '") + c +
            "' at position " + std::to_string(pos_));
    }

    JsonValue parse_string_value() {
        JsonValue v;
        v.type = JsonValue::STRING;
        v.str_val = parse_string();
        return v;
    }

    std::string parse_string() {
        expect('"');
        std::string result;
        while (pos_ < src_.size()) {
            char c = src_[pos_++];
            if (c == '"') return result;
            if (c == '\\') {
                if (pos_ >= src_.size())
                    throw std::runtime_error("MiniJsonParser: unterminated escape");
                char esc = src_[pos_++];
                switch (esc) {
                    case '"':  result += '"';  break;
                    case '\\': result += '\\'; break;
                    case '/':  result += '/';  break;
                    case 'n':  result += '\n'; break;
                    case 't':  result += '\t'; break;
                    case 'r':  result += '\r'; break;
                    default:   result += esc;  break;
                }
            } else {
                result += c;
            }
        }
        throw std::runtime_error("MiniJsonParser: unterminated string");
    }

    JsonValue parse_number() {
        JsonValue v;
        v.type = JsonValue::NUMBER;
        size_t start = pos_;
        if (src_[pos_] == '-') ++pos_;
        while (pos_ < src_.size() &&
               std::isdigit(static_cast<unsigned char>(src_[pos_])))
            ++pos_;
        if (pos_ < src_.size() && src_[pos_] == '.') {
            ++pos_;
            while (pos_ < src_.size() &&
                   std::isdigit(static_cast<unsigned char>(src_[pos_])))
                ++pos_;
        }
        if (pos_ < src_.size() && (src_[pos_] == 'e' || src_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < src_.size() && (src_[pos_] == '+' || src_[pos_] == '-'))
                ++pos_;
            while (pos_ < src_.size() &&
                   std::isdigit(static_cast<unsigned char>(src_[pos_])))
                ++pos_;
        }
        std::string num_str(src_.substr(start, pos_ - start));
        v.num_val = std::stod(num_str);
        return v;
    }

    JsonValue parse_object() {
        JsonValue v;
        v.type = JsonValue::OBJECT;
        expect('{');
        skip_ws();
        if (peek() == '}') { advance(); return v; }
        while (true) {
            skip_ws();
            std::string key = parse_string();
            skip_ws(); expect(':');
            JsonValue val = parse_value();
            v.obj_val.emplace_back(std::move(key), std::move(val));
            skip_ws();
            char c = advance();
            if (c == '}') break;
            if (c != ',')
                throw std::runtime_error(
                    "MiniJsonParser: expected ',' or '}' in object");
        }
        return v;
    }

    JsonValue parse_array() {
        JsonValue v;
        v.type = JsonValue::ARRAY;
        expect('[');
        skip_ws();
        if (peek() == ']') { advance(); return v; }
        while (true) {
            v.arr_val.push_back(parse_value());
            skip_ws();
            char c = advance();
            if (c == ']') break;
            if (c != ',')
                throw std::runtime_error(
                    "MiniJsonParser: expected ',' or ']' in array");
        }
        return v;
    }

    JsonValue parse_bool() {
        JsonValue v;
        v.type = JsonValue::NUMBER;
        if (src_.substr(pos_, 4) == "true")  { pos_ += 4; v.num_val = 1.0; }
        else if (src_.substr(pos_, 5) == "false") { pos_ += 5; v.num_val = 0.0; }
        else throw std::runtime_error(
            "MiniJsonParser: unexpected token at " + std::to_string(pos_));
        return v;
    }

    JsonValue parse_null() {
        if (src_.substr(pos_, 4) == "null") { pos_ += 4; return JsonValue{}; }
        throw std::runtime_error(
            "MiniJsonParser: unexpected token at " + std::to_string(pos_));
    }
};

inline std::string read_file_to_string(const std::string& filepath) {
    std::ifstream ifs(filepath, std::ios::in | std::ios::binary);
    if (!ifs.is_open())
        throw std::runtime_error("Failed to open file: " + filepath);
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}

} // namespace detail

// ===========================================================================
// Rule loading from JSON
// ===========================================================================

/**
 * @brief Load monitoring rules from a JSON file.
 *
 * Expects a top-level JSON array of rule objects. Each object must have
 * at least "rule_id" and "type" fields. Priority defaults to LOW if absent.
 *
 * @param filepath  Path to rules.json
 * @return          Vector of RuleDefinition (unsorted)
 */
inline std::vector<RuleDefinition> load_rules(const std::string& filepath) {
    std::string content = detail::read_file_to_string(filepath);
    auto root = detail::MiniJsonParser::parse(content);

    if (root.type != detail::MiniJsonParser::JsonValue::ARRAY)
        throw std::runtime_error(
            "load_rules: expected top-level JSON array in " + filepath);

    std::vector<RuleDefinition> rules;
    rules.reserve(root.arr_val.size());

    for (const auto& elem : root.arr_val) {
        if (elem.type != detail::MiniJsonParser::JsonValue::OBJECT)
            throw std::runtime_error(
                "load_rules: expected each element to be an object");

        RuleDefinition rule;

        const auto* rid = elem.find("rule_id");
        if (!rid) throw std::runtime_error("Rule missing 'rule_id' field");
        rule.rule_id = rid->as_string();

        const auto* type_val = elem.find("type");
        if (!type_val) throw std::runtime_error("Rule missing 'type' field");
        rule.type = string_to_ruletype(type_val->as_string());

        // Priority defaults to LOW if absent (spec says "optionally")
        const auto* prio = elem.find("priority");
        rule.priority = prio ? string_to_priority(prio->as_string())
                             : Priority::LOW;

        const auto* sensor = elem.find("sensor_id");
        if (sensor) rule.sensor_id = sensor->as_string();

        const auto* op_val = elem.find("operator");
        if (op_val) rule.op = string_to_compop(op_val->as_string());

        const auto* thresh = elem.find("value");
        if (thresh) rule.threshold = thresh->as_number();

        const auto* consec = elem.find("consecutive_measurements");
        if (consec) rule.consecutive_measurements = consec->as_int(1);

        const auto* logic_val = elem.find("logic");
        if (logic_val) rule.logic = logic_val->as_string();

        const auto* sub = elem.find("conditions");
        if (!sub) sub = elem.find("sub_rules");
        if (sub && sub->type == detail::MiniJsonParser::JsonValue::ARRAY) {
            for (const auto& e : sub->arr_val)
                rule.sub_rules.push_back(e.as_string());
        }

        rules.push_back(std::move(rule));
    }

    return rules;
}

// ===========================================================================
// Priority-based rule sorting
// ===========================================================================

/**
 * @brief Sort a vector of rule pointers by priority (HIGH first, then
 *        MEDIUM, then LOW).
 *
 * The spec requires: "rules with higher priority are evaluated before
 * the others" within each rule type.
 */
inline void sort_by_priority(std::vector<const RuleDefinition*>& rules) {
    std::sort(rules.begin(), rules.end(),
              [](const RuleDefinition* a, const RuleDefinition* b) {
                  return static_cast<int>(a->priority) >
                         static_cast<int>(b->priority);
              });
}

// ===========================================================================
// Sensor token mapping
// ===========================================================================

/**
 * @brief Assign sensor_token_idx to each non-correlation rule based on
 *        the provided token map.
 */
inline void assign_sensor_tokens(
    std::vector<RuleDefinition>& rules,
    const std::unordered_map<std::string, int>& token_map)
{
    for (auto& r : rules) {
        if (r.type == RuleType::CORRELATION) {
            r.sensor_token_idx = -1;
            continue;
        }
        auto it = token_map.find(r.sensor_id);
        r.sensor_token_idx = (it != token_map.end()) ? it->second : -1;
    }
}

// ===========================================================================
// Inline rule evaluators
// ===========================================================================

/**
 * @brief Evaluate a THRESHOLD (simple) rule: value <op> threshold
 */
inline bool evaluate_threshold(double value,
                               const RuleDefinition& rule) noexcept {
    return evaluate_comparison(value, rule.op, rule.threshold);
}

/**
 * @brief Evaluate a STEP_DIFF rule using SIGNED delta.
 *
 * delta = current_value - previous_value
 *
 * The spec defines step_difference as checking (current - previous)
 * against the operator and threshold. For example:
 *   operator "<", value -2.0 → fires when delta < -2.0
 *   (i.e. a drop of more than 2 units)
 *
 * NOTE: No std::abs() — the signed delta is compared directly.
 * This is critical for correctness: abs() would invert the sign and
 * cause operators like "<" with negative thresholds to never trigger.
 */
inline bool evaluate_step_diff(double value, const SensorState& state,
                               const RuleDefinition& rule) noexcept {
    if (!state.has_previous) return false;  // First reading → nominal
    double delta = value - state.previous_value;
    return evaluate_comparison(delta, rule.op, rule.threshold);
}

/**
 * @brief Evaluate a STATEFUL rule: consecutive violation persistence.
 *
 * Maintains a per-(sensor, rule) counter. The alarm is triggered when
 * the counter reaches consecutive_measurements, and on every subsequent
 * continuous violation.
 *
 * @param rule_slot  Index into state.consecutive_violations[] for this rule
 */
inline bool evaluate_stateful(double value, SensorState& state,
                              const RuleDefinition& rule,
                              int rule_slot) noexcept {
    bool violated = evaluate_comparison(value, rule.op, rule.threshold);

    if (violated) {
        int& count = state.consecutive_violations[rule_slot];
        ++count;
        return (count >= rule.consecutive_measurements);
    } else {
        state.consecutive_violations[rule_slot] = 0;
        return false;
    }
}

} // namespace astralog

#endif // ASTRALOG_RULES_ENGINE_HPP
