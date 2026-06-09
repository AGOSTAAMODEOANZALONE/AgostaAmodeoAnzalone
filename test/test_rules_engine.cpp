/**
 * @file test_rules_engine.cpp
 * @brief Unit tests for rules_engine.hpp, yaml_parser.hpp, and the embedded
 *        MiniJsonParser — rule loading, evaluation functions, and sensor-token
 *        assignment.  All tests are self-contained (no fixture files except
 *        small temporaries written inline).
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "rules_engine.hpp"
#include "test_helpers.hpp"
#include "types.hpp"
#include "yaml_parser.hpp"

namespace {

using namespace astralog;
using namespace astralog::test;

// ===========================================================================
// rules_engine.hpp — sort_rules_by_priority
// ===========================================================================

TEST(RulesEngineTest, SortRulesByPriority) {
    std::vector<RuleDefinition> rules(3);
    rules[0].priority = Priority::LOW;
    rules[1].priority = Priority::HIGH;
    rules[2].priority = Priority::MEDIUM;

    sort_rules_by_priority(rules);

    EXPECT_EQ(rules[0].priority, Priority::HIGH);
    EXPECT_EQ(rules[1].priority, Priority::MEDIUM);
    EXPECT_EQ(rules[2].priority, Priority::LOW);
}

TEST(RulesEngineTest, SortByPriorityPointers) {
    RuleDefinition low, medium, high;
    low.priority    = Priority::LOW;
    medium.priority = Priority::MEDIUM;
    high.priority   = Priority::HIGH;

    std::vector<const RuleDefinition*> rules = {&low, &medium, &high};
    sort_by_priority(rules);

    EXPECT_EQ(rules[0]->priority, Priority::HIGH);
    EXPECT_EQ(rules[1]->priority, Priority::MEDIUM);
    EXPECT_EQ(rules[2]->priority, Priority::LOW);
}

// ===========================================================================
// rules_engine.hpp — load_rules error paths
// ===========================================================================

TEST(RulesEngineTest, LoadRulesFileNotFoundThrows) {
    EXPECT_THROW(load_rules("/nonexistent/path/rules.json"), std::runtime_error);
}

TEST(RulesEngineTest, LoadRulesNotArrayThrows) {
    const std::string tmp = "test_load_rules_not_array.json";
    { std::ofstream f(tmp); f << "{\"rule_id\": \"R1\"}"; }
    EXPECT_THROW(load_rules(tmp), std::runtime_error);
    std::filesystem::remove(tmp);
}

TEST(RulesEngineTest, LoadRulesMissingRuleIdThrows) {
    const std::string tmp = "test_load_rules_no_id.json";
    { std::ofstream f(tmp);
      f << R"([{"type":"threshold","priority":"LOW","sensor_id":"S1","operator":">","value":10}])"; }
    EXPECT_THROW(load_rules(tmp), std::runtime_error);
    std::filesystem::remove(tmp);
}

TEST(RulesEngineTest, LoadRulesMissingTypeThrows) {
    const std::string tmp = "test_load_rules_no_type.json";
    { std::ofstream f(tmp);
      f << R"([{"rule_id":"R1","priority":"LOW","sensor_id":"S1","operator":">","value":10}])"; }
    EXPECT_THROW(load_rules(tmp), std::runtime_error);
    std::filesystem::remove(tmp);
}

// ===========================================================================
// rules_engine.hpp — load_rules valid / feature paths
// ===========================================================================

TEST(RulesEngineTest, LoadRulesValidMinimal) {
    const std::string tmp = "test_load_rules_valid.json";
    { std::ofstream f(tmp);
      f << R"([
          {"rule_id":"R1","type":"threshold","sensor_id":"S1",
           "operator":">","value":10.0,"priority":"HIGH"},
          {"rule_id":"R2","type":"correlation","logic":"OR",
           "conditions":["R1"],"priority":"MEDIUM"}
      ])"; }
    auto rules = load_rules(tmp);
    std::filesystem::remove(tmp);

    ASSERT_EQ(rules.size(), static_cast<size_t>(2));
    EXPECT_EQ(rules[0].rule_id,  "R1");
    EXPECT_EQ(rules[0].type,     RuleType::THRESHOLD);
    EXPECT_EQ(rules[0].priority, Priority::HIGH);
    EXPECT_EQ(rules[1].rule_id,  "R2");
    EXPECT_EQ(rules[1].type,     RuleType::CORRELATION);
    EXPECT_EQ(rules[1].logic,    "OR");
    ASSERT_EQ(rules[1].sub_rules.size(), static_cast<size_t>(1));
    EXPECT_EQ(rules[1].sub_rules[0], "R1");
}

TEST(RulesEngineTest, LoadRulesDefaultPriorityLow) {
    const std::string tmp = "test_load_rules_default_priority.json";
    { std::ofstream f(tmp);
      f << R"([{"rule_id":"R1","type":"threshold","sensor_id":"S1","operator":">","value":1}])"; }
    auto rules = load_rules(tmp);
    std::filesystem::remove(tmp);

    ASSERT_EQ(rules.size(), static_cast<size_t>(1));
    EXPECT_EQ(rules[0].priority, Priority::LOW);
}

TEST(RulesEngineTest, LoadRulesWithConditionsKey) {
    const std::string tmp = "test_conditions_key.json";
    { std::ofstream f(tmp);
      f << R"([
          {"rule_id":"R1","type":"threshold","sensor_id":"S1","operator":">","value":1,"priority":"LOW"},
          {"rule_id":"R_COR","type":"correlation","logic":"AND",
           "conditions":["R1"],"priority":"HIGH"}
      ])"; }
    auto rules = load_rules(tmp);
    std::filesystem::remove(tmp);

    ASSERT_EQ(rules.size(), static_cast<size_t>(2));
    EXPECT_EQ(rules[1].sub_rules.size(), static_cast<size_t>(1));
    EXPECT_EQ(rules[1].sub_rules[0], "R1");
}

TEST(RulesEngineTest, LoadRulesAllTypes) {
    const std::string tmp = "test_all_types.json";
    { std::ofstream f(tmp);
      f << R"([
          {"rule_id":"T1","type":"threshold",       "sensor_id":"S1","operator":">","value":1,"priority":"HIGH"},
          {"rule_id":"T2","type":"step_difference",  "sensor_id":"S2","operator":"<","value":-1,"priority":"MEDIUM"},
          {"rule_id":"T3","type":"stateful",         "sensor_id":"S3","operator":"<","value":5,"priority":"LOW","consecutive_measurements":3},
          {"rule_id":"T4","type":"correlation",      "logic":"OR","sub_rules":["T1","T2"],"priority":"HIGH"}
      ])"; }
    auto rules = load_rules(tmp);
    std::filesystem::remove(tmp);

    ASSERT_EQ(rules.size(), static_cast<size_t>(4));
    EXPECT_EQ(rules[0].type, RuleType::THRESHOLD);
    EXPECT_EQ(rules[1].type, RuleType::STEP_DIFF);
    EXPECT_EQ(rules[2].type, RuleType::STATEFUL);
    EXPECT_EQ(rules[2].consecutive_measurements, 3);
    EXPECT_EQ(rules[3].type, RuleType::CORRELATION);
    EXPECT_EQ(rules[3].logic, "OR");
}

// ===========================================================================
// rules_engine.hpp — assign_sensor_tokens
// ===========================================================================

TEST(RulesEngineTest, AssignSensorTokens) {
    std::unordered_map<std::string, int> token_map = {{"S1", 0}, {"S2", 1}};
    std::vector<RuleDefinition> rules;
    rules.push_back(make_rule("R1", RuleType::THRESHOLD, "S1",        CompOp::GT, 1.0, Priority::LOW));
    rules.push_back(make_rule("R2", RuleType::THRESHOLD, "S2",        CompOp::GT, 2.0, Priority::LOW));
    rules.push_back(make_rule("R3", RuleType::THRESHOLD, "S_UNKNOWN", CompOp::GT, 3.0, Priority::LOW));
    rules.push_back(make_rule("R4", RuleType::CORRELATION, "",         CompOp::GT, 0.0, Priority::LOW));

    assign_sensor_tokens(rules, token_map);

    EXPECT_EQ(rules[0].sensor_token_idx,  0);
    EXPECT_EQ(rules[1].sensor_token_idx,  1);
    EXPECT_EQ(rules[2].sensor_token_idx, -1);  // unknown sensor
    EXPECT_EQ(rules[3].sensor_token_idx, -1);  // correlation always -1
}

// ===========================================================================
// rules_engine.hpp — evaluate_threshold (all operators)
// ===========================================================================

TEST(RulesEngineTest, EvaluateThresholdAllOps) {
    auto make = [](CompOp op, double thresh) {
        RuleDefinition r; r.op = op; r.threshold = thresh; return r;
    };
    EXPECT_TRUE (evaluate_threshold(10.0, make(CompOp::GT, 9.0)));
    EXPECT_FALSE(evaluate_threshold( 9.0, make(CompOp::GT, 9.0)));
    EXPECT_TRUE (evaluate_threshold( 9.0, make(CompOp::GE, 9.0)));
    EXPECT_TRUE (evaluate_threshold( 5.0, make(CompOp::LT, 9.0)));
    EXPECT_FALSE(evaluate_threshold( 9.0, make(CompOp::LT, 9.0)));
    EXPECT_TRUE (evaluate_threshold( 9.0, make(CompOp::LE, 9.0)));
    EXPECT_TRUE (evaluate_threshold( 9.0, make(CompOp::EQ, 9.0)));
    EXPECT_FALSE(evaluate_threshold( 8.0, make(CompOp::EQ, 9.0)));
    EXPECT_TRUE (evaluate_threshold( 8.0, make(CompOp::NE, 9.0)));
    EXPECT_FALSE(evaluate_threshold( 9.0, make(CompOp::NE, 9.0)));
}

// ===========================================================================
// rules_engine.hpp — evaluate_step_diff
// ===========================================================================

TEST(RulesEngineTest, EvaluateStepDiffNoHistoryReturnsFalse) {
    RuleDefinition rule;
    rule.op        = CompOp::LT;
    rule.threshold = -2.0;
    SensorState state;  // has_previous == false by default
    EXPECT_FALSE(evaluate_step_diff(5.0, state, rule));
}

TEST(RulesEngineTest, EvaluateStepDiffSignedDelta) {
    RuleDefinition rule = make_rule("R", RuleType::STEP_DIFF, "S", CompOp::LT, -2.0, Priority::LOW);
    SensorState state;
    state.has_previous   = true;
    state.previous_value = 100.0;

    EXPECT_TRUE (evaluate_step_diff( 95.0, state, rule));  // -5 < -2  → fires
    EXPECT_FALSE(evaluate_step_diff( 99.0, state, rule));  // -1 not < -2
    EXPECT_FALSE(evaluate_step_diff(105.0, state, rule));  // +5 not < -2
}

// ===========================================================================
// rules_engine.hpp — evaluate_stateful (counter reset & persistence)
// ===========================================================================

TEST(RulesEngineTest, EvaluateStatefulCounterBehaviour) {
    RuleDefinition rule = make_rule("R", RuleType::STATEFUL, "S", CompOp::LT, 20.0, Priority::LOW, 3);
    SensorState state;
    state.consecutive_violations.assign(1, 0);

    EXPECT_FALSE(evaluate_stateful(10.0, state, rule, 0));  // count=1
    EXPECT_FALSE(evaluate_stateful(10.0, state, rule, 0));  // count=2
    EXPECT_TRUE (evaluate_stateful(10.0, state, rule, 0));  // count=3 → fires
    EXPECT_TRUE (evaluate_stateful(10.0, state, rule, 0));  // count=4 → still fires

    EXPECT_FALSE(evaluate_stateful(30.0, state, rule, 0));  // non-violation → reset
    EXPECT_EQ(state.consecutive_violations[0], 0);

    EXPECT_FALSE(evaluate_stateful(10.0, state, rule, 0));  // count=1
    EXPECT_FALSE(evaluate_stateful(10.0, state, rule, 0));  // count=2
    EXPECT_TRUE (evaluate_stateful(10.0, state, rule, 0));  // count=3 → fires again
}

// ===========================================================================
// MiniJsonParser — string escape sequences and number edge cases
// ===========================================================================

TEST(MiniJsonParserTest, StringEscapeSequences) {
    const std::string json = R"([{
        "rule_id": "R\\slash",
        "type": "threshold",
        "sensor_id": "S1",
        "operator": ">",
        "value": 1.0,
        "priority": "HIGH"
    }])";
    const std::string tmp = "test_escape_sequences.json";
    { std::ofstream f(tmp); f << json; }
    auto rules = load_rules(tmp);
    std::filesystem::remove(tmp);

    ASSERT_EQ(rules.size(), static_cast<size_t>(1));
    EXPECT_EQ(rules[0].rule_id, "R\\slash");
}

TEST(MiniJsonParserTest, ExponentialNumber) {
    const std::string tmp = "test_exp.json";
    { std::ofstream f(tmp);
      f << R"([{"rule_id":"R_EXP","type":"threshold","sensor_id":"S1",
                "operator":">","value":1.5e2,"priority":"LOW"}])"; }
    auto rules = load_rules(tmp);
    std::filesystem::remove(tmp);

    ASSERT_EQ(rules.size(), static_cast<size_t>(1));
    EXPECT_DOUBLE_EQ(rules[0].threshold, 150.0);
}

TEST(MiniJsonParserTest, NegativeNumber) {
    const std::string tmp = "test_neg.json";
    { std::ofstream f(tmp);
      f << R"([{"rule_id":"R_NEG","type":"step_diff","sensor_id":"S1",
                "operator":"<","value":-2.5,"priority":"MEDIUM"}])"; }
    auto rules = load_rules(tmp);
    std::filesystem::remove(tmp);

    ASSERT_EQ(rules.size(), static_cast<size_t>(1));
    EXPECT_DOUBLE_EQ(rules[0].threshold, -2.5);
}

TEST(MiniJsonParserTest, NestedObjectAndArray) {
    const std::string tmp = "test_nested.json";
    { std::ofstream f(tmp);
      f << R"([
          {"rule_id":"SUB1","type":"threshold","sensor_id":"S1","operator":">","value":10,"priority":"LOW"},
          {"rule_id":"CORR","type":"correlation","logic":"AND",
           "sub_rules":["SUB1"],"priority":"HIGH"}
      ])"; }
    auto rules = load_rules(tmp);
    std::filesystem::remove(tmp);

    ASSERT_EQ(rules.size(), static_cast<size_t>(2));
    EXPECT_EQ(rules[1].sub_rules.size(), static_cast<size_t>(1));
    EXPECT_EQ(rules[1].sub_rules[0], "SUB1");
}

TEST(MiniJsonParserTest, EmptyArray) {
    const std::string tmp = "test_empty_array.json";
    { std::ofstream f(tmp); f << "[]"; }
    auto rules = load_rules(tmp);
    std::filesystem::remove(tmp);
    EXPECT_TRUE(rules.empty());
}

// ===========================================================================
// yaml_parser.hpp
// ===========================================================================

TEST(YamlParserTest, ParseVariousFormats) {
    const std::string tmp = "test_sensors_formats.yaml";
    { std::ofstream f(tmp);
      f << "sensors:\n"
        << "  - id: SENSOR_A\n"
        << "  - id: 'SENSOR_B'\n"
        << "  - id: \"SENSOR_C\"\n"
        << "  id: SENSOR_D\n"
        << "  other_key: ignored\n"
        << "\n"; }
    auto sensors = load_sensors_from_yaml(tmp);
    std::filesystem::remove(tmp);

    ASSERT_EQ(sensors.size(), static_cast<size_t>(4));
    EXPECT_EQ(sensors[0], "SENSOR_A");
    EXPECT_EQ(sensors[1], "SENSOR_B");
    EXPECT_EQ(sensors[2], "SENSOR_C");
    EXPECT_EQ(sensors[3], "SENSOR_D");
}

TEST(YamlParserTest, FileNotFoundThrows) {
    EXPECT_THROW(load_sensors_from_yaml("/nonexistent/sensors.yaml"),
                 std::runtime_error);
}

TEST(YamlParserTest, EmptyFileReturnsEmptyVector) {
    const std::string tmp = "test_sensors_empty.yaml";
    { std::ofstream f(tmp); /* empty */ }
    auto sensors = load_sensors_from_yaml(tmp);
    std::filesystem::remove(tmp);
    EXPECT_TRUE(sensors.empty());
}

} // namespace
