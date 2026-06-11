/**
 * @file test_pipeline.cpp
 * @brief Unit and light-integration tests for the full processing pipeline:
 *        timestamp_processor.hpp, output_formatter.hpp, batch_accumulator.hpp,
 *        and the end-to-end process_pipeline() flow.
 *
 *  Suites:
 *    TimestampProcessorTest — grouping, per-sensor sequences, correlation logic,
 *                             sort, prepare_rules, generate_output
 *    OutputFormatterTest    — format_value, format_alarm_lines, format_nominal_line
 *    BatchAccumulatorTest   — strategies, counters, audit files
 *    ProcessingTest         — output format, cross-batch state, CSV helpers,
 *                             batch flush semantics, benchmark mode
 *    PipelineTest           — multi-rule scenarios with cross-batch state
 */

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include <omp.h>

#include "batch_accumulator.hpp"
#include "csv_parser.hpp"
#include "output_formatter.hpp"
#include "rules_engine.hpp"
#include "test_helpers.hpp"
#include "timestamp_processor.hpp"
#include "types.hpp"

namespace {

using namespace astralog;
using namespace astralog::test;

// ===========================================================================
// TimestampProcessorTest
// ===========================================================================

TEST(TimestampProcessorTest, BuildTimestampGroupsEmpty) {
    std::vector<ParsedRecord> empty;
    EXPECT_TRUE(build_timestamp_groups(empty).empty());
}

TEST(TimestampProcessorTest, BuildTimestampGroupsMultiple) {
    std::vector<ParsedRecord> records = {
        make_record("T1", 0, 1.0),
        make_record("T1", 1, 2.0),
        make_record("T2", 0, 3.0),
        make_record("T3", 2, 4.0),
    };
    auto groups = build_timestamp_groups(records);

    ASSERT_EQ(groups.size(), static_cast<size_t>(3));
    EXPECT_EQ(groups[0].timestamp, "T1");
    EXPECT_EQ(groups[0].readings.size(), static_cast<size_t>(2));
    EXPECT_EQ(groups[1].timestamp, "T2");
    EXPECT_EQ(groups[2].timestamp, "T3");
}

TEST(TimestampProcessorTest, BuildTimestampGroupsUnknownToken) {
    std::vector<ParsedRecord> records = {
        make_record("T1", -1, 99.9),
        make_record("T1",  0,  1.0),
    };
    auto groups = build_timestamp_groups(records);
    ASSERT_EQ(groups.size(), static_cast<size_t>(1));
    EXPECT_EQ(groups[0].readings.size(), static_cast<size_t>(2));
}

TEST(TimestampProcessorTest, BuildPerSensorSequences) {
    std::vector<ParsedRecord> records = {
        make_record("T1", 0, 1.0),
        make_record("T1", 1, 2.0),
        make_record("T2", 0, 3.0),
        make_record("T2", 2, 4.0),
    };
    auto groups = build_timestamp_groups(records);
    auto seqs   = build_per_sensor_sequences(groups, 3);

    ASSERT_EQ(seqs[0].size(), static_cast<size_t>(2));
    EXPECT_EQ(seqs[0][0].first, static_cast<size_t>(0));
    EXPECT_EQ(seqs[0][1].first, static_cast<size_t>(1));
    ASSERT_EQ(seqs[1].size(), static_cast<size_t>(1));
    ASSERT_EQ(seqs[2].size(), static_cast<size_t>(1));
}

TEST(TimestampProcessorTest, BuildPerSensorSequencesIgnoresOutOfRange) {
    std::vector<ParsedRecord> records = {
        make_record("T1", -1, 9.9),  // invalid token
        make_record("T1",  5, 1.0),  // out of range (num_sensors=3)
        make_record("T1",  0, 2.0),  // valid
    };
    auto groups = build_timestamp_groups(records);
    auto seqs   = build_per_sensor_sequences(groups, 3);

    EXPECT_EQ(seqs[0].size(), static_cast<size_t>(1));
    EXPECT_EQ(seqs[1].size(), static_cast<size_t>(0));
    EXPECT_EQ(seqs[2].size(), static_cast<size_t>(0));
}

TEST(TimestampProcessorTest, SortViolationsForOutput) {
    RuleDefinition high_rule, low_rule;
    high_rule.rule_id  = "R_HIGH"; high_rule.priority = Priority::HIGH;
    low_rule.rule_id   = "R_LOW";  low_rule.priority  = Priority::LOW;

    RuleViolation vh(&high_rule); vh.add(0, 1.0);
    RuleViolation vl(&low_rule);  vl.add(0, 2.0);

    std::vector<std::vector<RuleViolation>> all_v = {{vl, vh}};
    sort_violations_for_output(all_v);

    EXPECT_EQ(all_v[0][0].rule->priority, Priority::HIGH);
    EXPECT_EQ(all_v[0][1].rule->priority, Priority::LOW);
}

TEST(TimestampProcessorTest, SortViolationsTieBreakByRuleId) {
    RuleDefinition ra, rb;
    ra.rule_id = "R_A"; ra.priority = Priority::HIGH;
    rb.rule_id = "R_B"; rb.priority = Priority::HIGH;

    RuleViolation va(&ra); va.add(0, 1.0);
    RuleViolation vb(&rb); vb.add(0, 2.0);

    std::vector<std::vector<RuleViolation>> all_v = {{vb, va}};
    sort_violations_for_output(all_v);

    EXPECT_EQ(all_v[0][0].rule->rule_id, "R_A");
    EXPECT_EQ(all_v[0][1].rule->rule_id, "R_B");
}

TEST(TimestampProcessorTest, SortViolationsWithNullRule) {
    RuleDefinition r; r.rule_id = "R1"; r.priority = Priority::MEDIUM;
    RuleViolation v_with(&r); v_with.add(0, 1.0);
    RuleViolation v_null;     v_null.add(0, 2.0);   // rule == nullptr

    std::vector<std::vector<RuleViolation>> all_v = {{v_null, v_with}};
    EXPECT_NO_THROW(sort_violations_for_output(all_v));
}

TEST(TimestampProcessorTest, PipelineStateInitializeIdempotent) {
    std::vector<RuleDefinition> rules;
    { RuleDefinition r; r.rule_id = "R1"; r.type = RuleType::THRESHOLD;
      r.sensor_id = "S1"; r.op = CompOp::GT; r.threshold = 5.0;
      r.priority = Priority::LOW; rules.push_back(r); }
    std::unordered_map<std::string, int> token_map = {{"S1", 0}};
    auto prepared = prepare_rules(rules, token_map, 1);

    PipelineState state;
    state.initialize(1, prepared.sensor_rules);
    EXPECT_TRUE(state.initialized);

    // Second call must not reset existing state
    state.sensor_states[0].has_previous = true;
    state.initialize(1, prepared.sensor_rules);
    EXPECT_TRUE(state.sensor_states[0].has_previous);
}

TEST(TimestampProcessorTest, PrepareRulesOutOfRangeSensorTokenIgnored) {
    std::unordered_map<std::string, int> token_map = {{"S1", 0}};
    std::vector<RuleDefinition> rules;
    rules.push_back(make_rule("R_UNK", RuleType::THRESHOLD, "NONEXISTENT",
                              CompOp::GT, 5.0, Priority::LOW));
    auto prepared = prepare_rules(rules, token_map, 1);
    EXPECT_TRUE(prepared.sensor_rules[0].threshold.empty());
}

TEST(TimestampProcessorTest, CorrelationOrRule) {
    std::unordered_map<std::string, int> token_map = {{"S1", 0}, {"S2", 1}};
    std::vector<std::string> inverse = {"S1", "S2"};

    std::vector<RuleDefinition> rules;
    rules.push_back(make_rule("R1",  RuleType::THRESHOLD, "S1", CompOp::GT, 100.0, Priority::LOW));
    rules.push_back(make_rule("R2",  RuleType::THRESHOLD, "S2", CompOp::GT, 200.0, Priority::LOW));
    { RuleDefinition r; r.rule_id = "R_OR"; r.type = RuleType::CORRELATION;
      r.logic = "OR"; r.sub_rules = {"R1", "R2"}; r.priority = Priority::HIGH;
      rules.push_back(r); }

    std::vector<ParsedRecord> records = {
        make_record("2025-01-01T00:00:00Z", 0, 150.0),  // triggers R1
        make_record("2025-01-01T00:00:00Z", 1,  50.0),  // does NOT trigger R2
    };
    auto prepared = prepare_rules(rules, token_map, 2);
    PipelineState state;
    auto result = process_pipeline(records, prepared, inverse, state, false);

    EXPECT_EQ(result.num_anomalies, static_cast<uint64_t>(1));
    ASSERT_FALSE(result.alarm_lines[0].empty());
    EXPECT_NE(result.alarm_lines[0].find("R_OR"), std::string::npos);
}

TEST(TimestampProcessorTest, CorrelationAndNotTriggeredWhenOnlyOneSubRuleFires) {
    std::unordered_map<std::string, int> token_map = {{"S1", 0}, {"S2", 1}};
    std::vector<std::string> inverse = {"S1", "S2"};

    std::vector<RuleDefinition> rules;
    rules.push_back(make_rule("R1", RuleType::THRESHOLD, "S1", CompOp::GT, 100.0, Priority::LOW));
    rules.push_back(make_rule("R2", RuleType::THRESHOLD, "S2", CompOp::GT, 200.0, Priority::LOW));
    { RuleDefinition r; r.rule_id = "R_AND"; r.type = RuleType::CORRELATION;
      r.logic = "AND"; r.sub_rules = {"R1", "R2"}; r.priority = Priority::HIGH;
      rules.push_back(r); }

    std::vector<ParsedRecord> records = {
        make_record("T1", 0, 150.0),  // R1 fires
        make_record("T1", 1,  50.0),  // R2 does not
    };
    auto prepared = prepare_rules(rules, token_map, 2);
    PipelineState state;
    auto result = process_pipeline(records, prepared, inverse, state, false);

    EXPECT_EQ(result.num_anomalies, static_cast<uint64_t>(1));
    EXPECT_NE(result.alarm_lines[0].find("R1"),   std::string::npos);
    EXPECT_EQ(result.alarm_lines[0].find("R_AND"), std::string::npos);
}

TEST(TimestampProcessorTest, CorrelationAndTriggeredWhenAllSubRulesFire) {
    std::unordered_map<std::string, int> token_map = {{"S1", 0}, {"S2", 1}};
    std::vector<std::string> inverse = {"S1", "S2"};

    std::vector<RuleDefinition> rules;
    rules.push_back(make_rule("R1", RuleType::THRESHOLD, "S1", CompOp::GT, 100.0, Priority::LOW));
    rules.push_back(make_rule("R2", RuleType::THRESHOLD, "S2", CompOp::GT, 100.0, Priority::LOW));
    { RuleDefinition r; r.rule_id = "R_AND"; r.type = RuleType::CORRELATION;
      r.logic = "AND"; r.sub_rules = {"R1", "R2"}; r.priority = Priority::HIGH;
      rules.push_back(r); }

    std::vector<ParsedRecord> records = {
        make_record("T1", 0, 200.0),
        make_record("T1", 1, 200.0),
    };
    auto prepared = prepare_rules(rules, token_map, 2);
    PipelineState state;
    auto result = process_pipeline(records, prepared, inverse, state, false);

    EXPECT_EQ(result.num_anomalies, static_cast<uint64_t>(1));
    EXPECT_NE(result.alarm_lines[0].find("R_AND"), std::string::npos);
}

TEST(TimestampProcessorTest, CorrelationSkipsTimestampWithNoViolations) {
    std::unordered_map<std::string, int> token_map = {{"S1", 0}};
    std::vector<std::string> inverse = {"S1"};

    std::vector<RuleDefinition> rules;
    rules.push_back(make_rule("R1", RuleType::THRESHOLD, "S1", CompOp::GT, 1000.0, Priority::LOW));
    { RuleDefinition r; r.rule_id = "R_OR"; r.type = RuleType::CORRELATION;
      r.logic = "OR"; r.sub_rules = {"R1"}; r.priority = Priority::HIGH;
      rules.push_back(r); }

    auto prepared = prepare_rules(rules, token_map, 1);
    PipelineState state;
    auto result = process_pipeline({make_record("T1", 0, 5.0)},
                                   prepared, inverse, state, false);

    EXPECT_EQ(result.num_nominal,   static_cast<uint64_t>(1));
    EXPECT_EQ(result.num_anomalies, static_cast<uint64_t>(0));
}

TEST(TimestampProcessorTest, EvaluateRulesParallelCrossBatchStepDiff) {
    std::unordered_map<std::string, int> token_map = {{"S1", 0}};
    std::vector<std::string> inverse = {"S1"};

    std::vector<RuleDefinition> rules;
    rules.push_back(make_rule("R_SD", RuleType::STEP_DIFF, "S1", CompOp::LT, -10.0, Priority::HIGH));

    auto prepared = prepare_rules(rules, token_map, 1);
    PipelineState state;

    // Batch 1: value 100 — no previous yet, cannot fire
    auto r1 = process_pipeline({make_record("T1", 0, 100.0)}, prepared, inverse, state, false);
    EXPECT_EQ(r1.num_anomalies, static_cast<uint64_t>(0));

    // Batch 2: value 85 — delta = -15 < -10 → fires
    auto r2 = process_pipeline({make_record("T2", 0, 85.0)}, prepared, inverse, state, false);
    EXPECT_EQ(r2.num_anomalies, static_cast<uint64_t>(1));

    // Batch 3: value 84 — delta = -1, not < -10 → no fire
    auto r3 = process_pipeline({make_record("T3", 0, 84.0)}, prepared, inverse, state, false);
    EXPECT_EQ(r3.num_anomalies, static_cast<uint64_t>(0));
}

TEST(TimestampProcessorTest, GenerateOutputParallelNominal) {
    std::vector<std::string> inverse = {"S1", "S2"};
    std::vector<ParsedRecord> records = {
        make_record("T1", 0, 10.0),
        make_record("T1", 1, 20.0),
        make_record("T2", 0, 30.0),
    };
    std::unordered_map<std::string, int> token_map = {{"S1", 0}, {"S2", 1}};
    std::vector<RuleDefinition> no_rules;
    auto prepared = prepare_rules(no_rules, token_map, 2);
    PipelineState state;
    auto result = process_pipeline(records, prepared, inverse, state, false);

    EXPECT_EQ(result.num_nominal,   static_cast<uint64_t>(2));
    EXPECT_EQ(result.num_anomalies, static_cast<uint64_t>(0));
    EXPECT_EQ(result.valid_lines[0], "T1;NOMINAL;S1:10.0|S2:20.0\n");
    EXPECT_EQ(result.valid_lines[1], "T2;NOMINAL;S1:30.0\n");
}

// ===========================================================================
// OutputFormatterTest
// ===========================================================================

TEST(OutputFormatterTest, FormatValueEdgeCases) {
    EXPECT_EQ(format_value(-3.5),     "-3.5");
    EXPECT_EQ(format_value(100.0),    "100.0");
    EXPECT_EQ(format_value(3.141592), "3.141592");
    EXPECT_EQ(format_value(0.0),      "0.0");
}

TEST(OutputFormatterTest, FormatValuePrecision) {
    EXPECT_EQ(format_value(1.0),         "1.0");
    EXPECT_EQ(format_value(1.5),         "1.5");
    EXPECT_EQ(format_value(1.50),        "1.5");
    EXPECT_EQ(format_value(1.23456789),  "1.234568");  // rounds at 6th decimal
    EXPECT_EQ(format_value(-5.0),        "-5.0");
    EXPECT_EQ(format_value(0.0),         "0.0");
    EXPECT_EQ(format_value(100.0),       "100.0");
}

TEST(OutputFormatterTest, FormatAlarmLinesNullRule) {
    RuleViolation bad_v;  // rule == nullptr
    bad_v.add(0, 42.0);
    std::vector<std::string> inverse = {"TEMP-01"};
    std::string result = format_alarm_lines("2025-01-01T00:00:00Z", {bad_v}, inverse);
    EXPECT_NE(result.find("UNKNOWN_RULE"), std::string::npos);
    EXPECT_NE(result.find("LOW"),          std::string::npos);
}

TEST(OutputFormatterTest, FormatAlarmLinesUnknownSensorToken) {
    RuleDefinition rule; rule.rule_id = "RX"; rule.priority = Priority::LOW;
    RuleViolation v(&rule);
    v.add(-1, 7.0);  // token -1 → UNKNOWN
    std::vector<std::string> inverse = {"TEMP-01"};
    std::string result = format_alarm_lines("2025-01-01T00:00:00Z", {v}, inverse);
    EXPECT_NE(result.find("UNKNOWN"), std::string::npos);
}

TEST(OutputFormatterTest, FormatAlarmLinesExtraStorage) {
    RuleDefinition rule;
    rule.rule_id  = "R_BIG";
    rule.priority = Priority::HIGH;
    rule.type     = RuleType::CORRELATION;

    RuleViolation v(&rule);
    std::vector<std::string> inverse;
    for (int i = 0; i < 6; ++i) {
        inverse.push_back("S" + std::to_string(i));
        v.add(i, static_cast<double>(i) * 10.0);
    }
    std::string out = format_alarm_lines("TS", {v}, inverse);
    EXPECT_NE(out.find("S0"),   std::string::npos);
    EXPECT_NE(out.find("S5"),   std::string::npos);
    EXPECT_NE(out.find("50.0"), std::string::npos);
}

TEST(OutputFormatterTest, FormatNominalLineUnknownToken) {
    std::vector<std::string> inverse = {"TEMP-01"};
    TimestampGroup group;
    group.timestamp = "2025-01-01T00:00:00Z";
    group.readings.push_back({-1, 99.9});   // token -1 → UNKNOWN
    group.readings.push_back({ 5,  1.0});   // token out of range → UNKNOWN
    EXPECT_NE(format_nominal_line(group, inverse).find("UNKNOWN"), std::string::npos);
}

TEST(OutputFormatterTest, FormatNominalLineEmptyReadings) {
    std::vector<std::string> inverse = {"S1"};
    TimestampGroup group;
    group.timestamp = "TS";
    EXPECT_EQ(format_nominal_line(group, inverse), "TS;NOMINAL;\n");
}

// ===========================================================================
// BatchAccumulatorTest
// ===========================================================================

TEST(BatchAccumulatorTest, StrategyStringConversions) {
    EXPECT_EQ(string_to_strategy("count"), BatchStrategy::COUNT);
    EXPECT_EQ(string_to_strategy("COUNT"), BatchStrategy::COUNT);
    EXPECT_EQ(string_to_strategy("time"),  BatchStrategy::TIME);
    EXPECT_EQ(string_to_strategy("TIME"),  BatchStrategy::TIME);
    EXPECT_THROW(string_to_strategy("unknown"), std::invalid_argument);

    EXPECT_STREQ(strategy_to_string(BatchStrategy::COUNT), "COUNT");
    EXPECT_STREQ(strategy_to_string(BatchStrategy::TIME),  "TIME");
}

TEST(BatchAccumulatorTest, StrategyToStringUnknown) {
    BatchStrategy invalid = static_cast<BatchStrategy>(99);
    EXPECT_STREQ(strategy_to_string(invalid), "UNKNOWN");
}

TEST(BatchAccumulatorTest, TimeStrategyFlushes) {
    std::vector<std::string> inverse = {"S1"};
    BatchConfig config;
    config.strategy          = BatchStrategy::TIME;
    config.time_threshold_ms = 50.0;
    config.write_audit_files = false;

    BatchAccumulator acc(config, inverse);
    ParsedRecord rec;
    rec.timestamp = "2025-01-01T00:00:00Z"; rec.sensor_token = 0; rec.value = 1.0;
    acc.add_record(rec);

    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    EXPECT_TRUE(acc.should_flush());
    EXPECT_EQ(acc.flush().size(), static_cast<size_t>(1));
}

TEST(BatchAccumulatorTest, AuditFileIsWritten) {
    std::vector<std::string> inverse = {"TEMP-01"};
    BatchConfig config;
    config.count_threshold   = 1;
    config.batch_output_dir  = "test_audit_tmp";
    config.write_audit_files = true;
    std::filesystem::remove_all(config.batch_output_dir);

    BatchAccumulator acc(config, inverse);
    ParsedRecord rec;
    rec.timestamp = "2025-01-01T00:00:00Z"; rec.sensor_token = 0; rec.value = 99.0;
    acc.add_record(rec);

    EXPECT_TRUE(acc.should_flush());
    acc.flush();
    EXPECT_TRUE(std::filesystem::exists(config.batch_output_dir));
    bool has_file = false;
    for ([[maybe_unused]] const auto& e :
         std::filesystem::directory_iterator(config.batch_output_dir))
        has_file = true;
    EXPECT_TRUE(has_file);
    std::filesystem::remove_all(config.batch_output_dir);
}

TEST(BatchAccumulatorTest, CountersAndConfig) {
    std::vector<std::string> inverse = {"S1"};
    BatchConfig config;
    config.count_threshold   = 2;
    config.write_audit_files = false;

    BatchAccumulator acc(config, inverse);
    EXPECT_EQ(acc.batch_count(),    0);
    EXPECT_EQ(acc.buffered_count(), static_cast<size_t>(0));

    ParsedRecord rec; rec.timestamp = "T"; rec.sensor_token = 0; rec.value = 1.0;
    acc.add_record(rec);
    EXPECT_EQ(acc.buffered_count(), static_cast<size_t>(1));
    acc.add_record(rec);
    EXPECT_TRUE(acc.should_flush());
    acc.flush();
    EXPECT_EQ(acc.batch_count(), 1);
    EXPECT_EQ(acc.config().count_threshold, static_cast<size_t>(2));
}

TEST(BatchAccumulatorTest, FlushRemainingEmpty) {
    std::vector<std::string> inverse = {"S1"};
    BatchConfig config;
    config.count_threshold   = 10;
    config.write_audit_files = false;
    BatchAccumulator acc(config, inverse);
    EXPECT_TRUE(acc.flush_remaining().empty());
}

TEST(BatchAccumulatorTest, CountStrategyExactBoundary) {
    std::vector<std::string> inverse = {"S1"};
    BatchConfig config;
    config.count_threshold   = 3;
    config.write_audit_files = false;
    BatchAccumulator acc(config, inverse);

    ParsedRecord rec; rec.timestamp = "T"; rec.sensor_token = 0; rec.value = 1.0;
    acc.add_record(rec); EXPECT_FALSE(acc.should_flush());  // 1 < 3
    acc.add_record(rec); EXPECT_FALSE(acc.should_flush());  // 2 < 3
    acc.add_record(rec); EXPECT_TRUE (acc.should_flush());  // 3 >= 3

    auto batch = acc.flush();
    EXPECT_EQ(batch.size(),        static_cast<size_t>(3));
    EXPECT_EQ(acc.buffered_count(),static_cast<size_t>(0));
    EXPECT_EQ(acc.batch_count(),   1);
}

// ===========================================================================
// ProcessingTest — output format, pipeline semantics, batch flush, CSV helpers
// ===========================================================================

TEST(ProcessingTest, StrictOutputFormat) {
    std::vector<std::string> inverse = {"TEMP-01", "PRES-01"};

    TimestampGroup group;
    group.timestamp = "2025-11-15T12:00:00Z";
    group.readings.push_back({0, 25.0});
    group.readings.push_back({1, 101.3});

    EXPECT_EQ(format_nominal_line(group, inverse),
              "2025-11-15T12:00:00Z;NOMINAL;TEMP-01:25.0|PRES-01:101.3\n");

    RuleDefinition simple_rule = make_rule("R1", RuleType::THRESHOLD,
                                           "TEMP-01", CompOp::GT, 50.0, Priority::MEDIUM);
    RuleViolation simple(&simple_rule); simple.add(0, 51.2);

    RuleDefinition corr_rule;
    corr_rule.rule_id = "R4"; corr_rule.type = RuleType::CORRELATION;
    corr_rule.priority = Priority::HIGH;
    RuleViolation corr(&corr_rule); corr.add(0, 51.2); corr.add(1, 98.0);

    EXPECT_EQ(format_alarm_lines("2025-11-15T12:00:05Z", {simple, corr}, inverse),
              "2025-11-15T12:00:05Z;R1;MEDIUM;TEMP-01;51.2\n"
              "2025-11-15T12:00:05Z;R4;HIGH;TEMP-01,PRES-01;51.2,98.0\n");
}

TEST(ProcessingTest, RuleViolationInlineAndFallbackStorage) {
    RuleDefinition rule = make_rule("R_BIG", RuleType::CORRELATION,
                                    "", CompOp::GT, 0.0, Priority::HIGH);
    RuleViolation violation(&rule);
    for (int i = 0; i < 6; ++i) violation.add(i, 10.0 + i);

    EXPECT_EQ(violation.size(),              static_cast<size_t>(6));
    EXPECT_EQ(violation.sensor_token_at(0),  0);
    EXPECT_EQ(violation.value_at(0),         10.0);
    EXPECT_EQ(violation.sensor_token_at(4),  4);
    EXPECT_EQ(violation.value_at(5),         15.0);
    EXPECT_EQ(violation.rule->rule_id,       "R_BIG");
    EXPECT_EQ(violation.rule->priority,      Priority::HIGH);
}

TEST(ProcessingTest, PipelineRuleSemantics) {
    std::unordered_map<std::string, int> token_map = {
        {"TEMP-01", 0}, {"PRES-01", 1}, {"VOLT-MAIN", 2},
    };
    std::vector<std::string> inverse = {"TEMP-01", "PRES-01", "VOLT-MAIN"};

    std::vector<RuleDefinition> rules;
    rules.push_back(make_rule("R1", RuleType::THRESHOLD, "TEMP-01", CompOp::GT, 50.0, Priority::MEDIUM));
    rules.push_back(make_rule("R2", RuleType::STEP_DIFF, "PRES-01", CompOp::LT, -2.0, Priority::LOW));
    auto stateful = make_rule("R3", RuleType::STATEFUL, "VOLT-MAIN", CompOp::LT, 20.0, Priority::HIGH, 3);
    rules.push_back(stateful);
    { RuleDefinition corr; corr.rule_id = "R4"; corr.type = RuleType::CORRELATION;
      corr.logic = "AND"; corr.sub_rules = {"R1", "R2"}; corr.priority = Priority::HIGH;
      rules.push_back(corr); }

    std::vector<ParsedRecord> records = {
        make_record("2025-11-15T12:00:00Z", 0, 25.0),
        make_record("2025-11-15T12:00:00Z", 1, 101.0),
        make_record("2025-11-15T12:00:00Z", 2, 24.0),
        make_record("2025-11-15T12:00:01Z", 0, 51.0),
        make_record("2025-11-15T12:00:01Z", 1, 98.0),
        make_record("2025-11-15T12:00:01Z", 2, 19.0),
        make_record("2025-11-15T12:00:02Z", 0, 40.0),
        make_record("2025-11-15T12:00:02Z", 1, 98.0),
        make_record("2025-11-15T12:00:02Z", 2, 19.0),
        make_record("2025-11-15T12:00:03Z", 0, 40.0),
        make_record("2025-11-15T12:00:03Z", 1, 98.0),
        make_record("2025-11-15T12:00:03Z", 2, 19.0),
    };

    auto prepared = prepare_rules(rules, token_map, 3);
    PipelineState state;
    auto result = process_pipeline(records, prepared, inverse, state, false);

    EXPECT_EQ(result.num_timestamps, static_cast<uint64_t>(4));
    EXPECT_EQ(result.num_nominal,    static_cast<uint64_t>(2));
    EXPECT_EQ(result.num_anomalies,  static_cast<uint64_t>(2));
    EXPECT_EQ(result.valid_lines[0],
              "2025-11-15T12:00:00Z;NOMINAL;TEMP-01:25.0|PRES-01:101.0|VOLT-MAIN:24.0\n");
    EXPECT_EQ(result.alarm_lines[1],
              "2025-11-15T12:00:01Z;R4;HIGH;TEMP-01,PRES-01;51.0,98.0\n"
              "2025-11-15T12:00:01Z;R1;MEDIUM;TEMP-01;51.0\n"
              "2025-11-15T12:00:01Z;R2;LOW;PRES-01;98.0\n");
    EXPECT_EQ(result.alarm_lines[3],
              "2025-11-15T12:00:03Z;R3;HIGH;VOLT-MAIN;19.0\n");
}

TEST(ProcessingTest, CrossBatchStatePersistence) {
    std::unordered_map<std::string, int> token_map = {{"VOLT-MAIN", 0}};
    std::vector<std::string> inverse = {"VOLT-MAIN"};
    auto stateful = make_rule("R3", RuleType::STATEFUL, "VOLT-MAIN", CompOp::LT, 20.0, Priority::HIGH, 3);
    std::vector<RuleDefinition> rules_cs = {stateful};
    auto prepared = prepare_rules(rules_cs, token_map, 1);
    PipelineState state;

    auto first  = process_pipeline({make_record("2025-11-15T12:00:00Z", 0, 19.0),
                                    make_record("2025-11-15T12:00:01Z", 0, 19.0)},
                                   prepared, inverse, state, false);
    auto second = process_pipeline({make_record("2025-11-15T12:00:02Z", 0, 19.0)},
                                   prepared, inverse, state, false);

    EXPECT_EQ(first.num_anomalies,  static_cast<uint64_t>(0));
    EXPECT_EQ(second.num_anomalies, static_cast<uint64_t>(1));
    EXPECT_EQ(second.alarm_lines[0], "2025-11-15T12:00:02Z;R3;HIGH;VOLT-MAIN;19.0\n");
}

TEST(ProcessingTest, CSVValidationHelpers) {
    std::string_view ts, sensor, value_field, priority;

    EXPECT_TRUE(tokenize_csv_line("2025-11-15T12:00:00Z,TEMP-01,25.5,HIGH",
                                  ts, sensor, value_field, priority));
    EXPECT_EQ(ts,          "2025-11-15T12:00:00Z");
    EXPECT_EQ(sensor,      "TEMP-01");
    EXPECT_EQ(value_field, "25.5");
    EXPECT_EQ(priority,    "HIGH");

    std::vector<std::string> inverse = {"TEMP-01", "PRES-01"};
    auto tvm = build_token_view_map(inverse);
    EXPECT_EQ(lookup_sensor_token(tvm, sensor),    0);
    EXPECT_EQ(lookup_sensor_token(tvm, "UNKNOWN"), -1);

    EXPECT_FALSE(tokenize_csv_line("2025-11-15T12:00:00Z,TEMP-01,25.5",
                                   ts, sensor, value_field, priority));
    EXPECT_TRUE(validate_schema(ts, sensor, value_field, priority));
    EXPECT_FALSE(validate_schema(ts, "", value_field, priority));
    EXPECT_FALSE(validate_schema(ts, sensor, "ERR", priority));

    double parsed = 0.0;
    EXPECT_TRUE(parse_value("25.5", parsed));
    EXPECT_EQ(parsed, 25.5);
    EXPECT_FALSE(parse_value("SYSTEM_HALT_ERR", parsed));
}

TEST(ProcessingTest, BenchmarkModeDisablesAuditFiles) {
    std::vector<std::string> inverse = {"TEMP-01"};
    BatchConfig config;
    config.count_threshold   = 1;
    config.batch_output_dir  = "test_batches_disabled_tmp";
    config.write_audit_files = false;
    std::filesystem::remove_all(config.batch_output_dir);

    BatchAccumulator acc(config, inverse);
    acc.add_record(make_record("2025-11-15T12:00:00Z", 0, 1.0));
    EXPECT_EQ(acc.flush().size(), static_cast<size_t>(1));
    EXPECT_FALSE(std::filesystem::exists(config.batch_output_dir));
}

TEST(ProcessingTest, BatchFlushKeepsTimestampsIntact) {
    std::vector<std::string> inverse = {"TEMP-01"};
    BatchConfig config;
    config.count_threshold   = 2;
    config.batch_output_dir  = "test_batches_tmp";
    config.write_audit_files = false;

    BatchAccumulator acc(config, inverse);
    std::vector<ParsedRecord> records = {
        make_record("2025-11-15T12:00:00Z", 0, 1.0),
        make_record("2025-11-15T12:00:00Z", 0, 2.0),
        make_record("2025-11-15T12:00:00Z", 0, 3.0),
        make_record("2025-11-15T12:00:01Z", 0, 4.0),
    };

    std::vector<size_t> flushed_sizes;
    for (size_t i = 0; i < records.size(); ++i) {
        const bool boundary = (i + 1 == records.size()) ||
                              (records[i + 1].timestamp != records[i].timestamp);
        acc.add_record(std::move(records[i]));
        if (boundary && acc.should_flush())
            flushed_sizes.push_back(acc.flush().size());
    }
    auto final_batch = acc.flush_remaining();
    if (!final_batch.empty()) flushed_sizes.push_back(final_batch.size());

    ASSERT_EQ(flushed_sizes.size(), static_cast<size_t>(2));
    EXPECT_EQ(flushed_sizes[0], static_cast<size_t>(3));
    EXPECT_EQ(flushed_sizes[1], static_cast<size_t>(1));
    std::filesystem::remove_all(config.batch_output_dir);
}

// ===========================================================================
// PipelineTest — multi-rule / cross-batch scenarios
// ===========================================================================

TEST(PipelineTest, StepDiffNoFireOnFirstReading) {
    std::unordered_map<std::string, int> token_map = {{"S1", 0}};
    std::vector<std::string> inverse = {"S1"};
    std::vector<RuleDefinition> rules_sd = {
        make_rule("R_SD", RuleType::STEP_DIFF, "S1", CompOp::LT, -1.0, Priority::LOW)};
    auto prepared = prepare_rules(rules_sd, token_map, 1);
    PipelineState state;
    auto result = process_pipeline({make_record("T1", 0, 1.0)}, prepared, inverse, state, false);
    EXPECT_EQ(result.num_nominal,   static_cast<uint64_t>(1));
    EXPECT_EQ(result.num_anomalies, static_cast<uint64_t>(0));
}

TEST(PipelineTest, MultipleRuleTypesAtSameTimestamp) {
    std::unordered_map<std::string, int> token_map = {{"S1", 0}, {"S2", 1}};
    std::vector<std::string> inverse = {"S1", "S2"};

    std::vector<RuleDefinition> rules_mr = {
        make_rule("THRESH", RuleType::THRESHOLD, "S1", CompOp::GT, 50.0, Priority::HIGH),
        make_rule("STEP",   RuleType::STEP_DIFF,  "S2", CompOp::LT, -5.0, Priority::MEDIUM)};
    auto prepared = prepare_rules(rules_mr, token_map, 2);
    PipelineState state;

    // T1: establish previous for step_diff
    auto r1 = process_pipeline({make_record("T1", 0, 30.0),
                                 make_record("T1", 1, 100.0)},
                                prepared, inverse, state, false);
    EXPECT_EQ(r1.num_nominal, static_cast<uint64_t>(1));

    // T2: both rules fire
    auto r2 = process_pipeline({make_record("T2", 0, 60.0),
                                 make_record("T2", 1, 90.0)},
                                prepared, inverse, state, false);
    EXPECT_EQ(r2.num_anomalies, static_cast<uint64_t>(1));
    EXPECT_NE(r2.alarm_lines[0].find("THRESH"), std::string::npos);
    EXPECT_NE(r2.alarm_lines[0].find("STEP"),   std::string::npos);
}

TEST(PipelineTest, StatefulCrossBatchPersistence) {
    std::unordered_map<std::string, int> token_map = {{"S1", 0}};
    std::vector<std::string> inverse = {"S1"};
    std::vector<RuleDefinition> rules_sf = {
        make_rule("R_SF", RuleType::STATEFUL, "S1", CompOp::LT, 10.0, Priority::HIGH, 4)};
    auto prepared = prepare_rules(rules_sf, token_map, 1);
    PipelineState state;

    for (int i = 0; i < 3; ++i) {
        auto r = process_pipeline({make_record("T" + std::to_string(i), 0, 5.0)},
                                   prepared, inverse, state, false);
        EXPECT_EQ(r.num_anomalies, static_cast<uint64_t>(0)) << "Batch " << i;
    }
    auto r4 = process_pipeline({make_record("T3", 0, 5.0)},
                                prepared, inverse, state, false);
    EXPECT_EQ(r4.num_anomalies, static_cast<uint64_t>(1));
}

} // namespace
