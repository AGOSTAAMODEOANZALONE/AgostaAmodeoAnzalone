#include <cassert>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <omp.h>

#include "batch_accumulator.hpp"
#include "csv_parser.hpp"
#include "output_formatter.hpp"
#include "timestamp_processor.hpp"
#include "types.hpp"

namespace {

using namespace astralog;

ParsedRecord make_record(const std::string& timestamp,
                         int sensor_token,
                         double value) {
    ParsedRecord rec;
    rec.timestamp = timestamp;
    rec.sensor_token = sensor_token;
    rec.value = value;
    return rec;
}

RuleDefinition make_rule(const std::string& id,
                         RuleType type,
                         const std::string& sensor_id,
                         CompOp op,
                         double threshold,
                         Priority priority) {
    RuleDefinition rule;
    rule.rule_id = id;
    rule.type = type;
    rule.sensor_id = sensor_id;
    rule.op = op;
    rule.threshold = threshold;
    rule.priority = priority;
    return rule;
}

void test_strict_output_format() {
    std::vector<std::string> inverse = {"TEMP-01", "PRES-01"};

    TimestampGroup group;
    group.timestamp = "2025-11-15T12:00:00Z";
    group.readings.push_back({0, 25.0});
    group.readings.push_back({1, 101.3});

    assert(format_nominal_line(group, inverse) ==
           "2025-11-15T12:00:00Z;NOMINAL;TEMP-01:25.0|PRES-01:101.3\n");

    RuleDefinition simple_rule = make_rule("R1", RuleType::THRESHOLD,
                                           "TEMP-01", CompOp::GT,
                                           50.0, Priority::MEDIUM);
    RuleViolation simple(&simple_rule);
    simple.add(0, 51.2);

    RuleDefinition corr_rule;
    corr_rule.rule_id = "R4";
    corr_rule.type = RuleType::CORRELATION;
    corr_rule.priority = Priority::HIGH;
    RuleViolation corr(&corr_rule);
    corr.add(0, 51.2);
    corr.add(1, 98.0);

    assert(format_alarm_lines("2025-11-15T12:00:05Z",
                              {simple, corr},
                              inverse) ==
           "2025-11-15T12:00:05Z;R1;MEDIUM;TEMP-01;51.2\n"
           "2025-11-15T12:00:05Z;R4;HIGH;TEMP-01,PRES-01;51.2,98.0\n");
}

void test_rule_violation_inline_and_fallback_storage() {
    RuleDefinition rule = make_rule("R_BIG", RuleType::CORRELATION,
                                    "", CompOp::GT, 0.0, Priority::HIGH);
    RuleViolation violation(&rule);

    for (int i = 0; i < 6; ++i) {
        violation.add(i, 10.0 + i);
    }

    assert(violation.size() == 6);
    assert(violation.sensor_token_at(0) == 0);
    assert(violation.value_at(0) == 10.0);
    assert(violation.sensor_token_at(4) == 4);
    assert(violation.value_at(5) == 15.0);
    assert(violation.rule->rule_id == "R_BIG");
    assert(violation.rule->priority == Priority::HIGH);
}

void test_pipeline_rule_semantics() {
    std::unordered_map<std::string, int> token_map = {
        {"TEMP-01", 0},
        {"PRES-01", 1},
        {"VOLT-MAIN", 2},
    };
    std::vector<std::string> inverse = {"TEMP-01", "PRES-01", "VOLT-MAIN"};

    std::vector<RuleDefinition> rules;
    rules.push_back(make_rule("R1", RuleType::THRESHOLD, "TEMP-01",
                              CompOp::GT, 50.0, Priority::MEDIUM));
    rules.push_back(make_rule("R2", RuleType::STEP_DIFF, "PRES-01",
                              CompOp::LT, -2.0, Priority::LOW));

    RuleDefinition stateful = make_rule("R3", RuleType::STATEFUL, "VOLT-MAIN",
                                        CompOp::LT, 20.0, Priority::HIGH);
    stateful.consecutive_measurements = 3;
    rules.push_back(stateful);

    RuleDefinition corr;
    corr.rule_id = "R4";
    corr.type = RuleType::CORRELATION;
    corr.logic = "AND";
    corr.sub_rules = {"R1", "R2"};
    corr.priority = Priority::HIGH;
    rules.push_back(corr);

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

    assert(result.num_timestamps == 4);
    assert(result.num_nominal == 2);
    assert(result.num_anomalies == 2);

    assert(result.valid_lines[0] ==
           "2025-11-15T12:00:00Z;NOMINAL;"
           "TEMP-01:25.0|PRES-01:101.0|VOLT-MAIN:24.0\n");
    assert(result.valid_lines[2] ==
           "2025-11-15T12:00:02Z;NOMINAL;"
           "TEMP-01:40.0|PRES-01:98.0|VOLT-MAIN:19.0\n");

    assert(result.alarm_lines[1] ==
           "2025-11-15T12:00:01Z;R4;HIGH;TEMP-01,PRES-01;51.0,98.0\n"
           "2025-11-15T12:00:01Z;R1;MEDIUM;TEMP-01;51.0\n"
           "2025-11-15T12:00:01Z;R2;LOW;PRES-01;98.0\n");
    assert(result.alarm_lines[3] ==
           "2025-11-15T12:00:03Z;R3;HIGH;VOLT-MAIN;19.0\n");
}

void test_cross_batch_state_persistence() {
    std::unordered_map<std::string, int> token_map = {{"VOLT-MAIN", 0}};
    std::vector<std::string> inverse = {"VOLT-MAIN"};

    RuleDefinition stateful = make_rule("R3", RuleType::STATEFUL, "VOLT-MAIN",
                                        CompOp::LT, 20.0, Priority::HIGH);
    stateful.consecutive_measurements = 3;
    std::vector<RuleDefinition> rules = {stateful};

    auto prepared = prepare_rules(rules, token_map, 1);
    PipelineState state;
    std::vector<ParsedRecord> batch1 = {
        make_record("2025-11-15T12:00:00Z", 0, 19.0),
        make_record("2025-11-15T12:00:01Z", 0, 19.0),
    };
    std::vector<ParsedRecord> batch2 = {
        make_record("2025-11-15T12:00:02Z", 0, 19.0),
    };

    auto first = process_pipeline(batch1, prepared, inverse, state, false);
    auto second = process_pipeline(batch2, prepared, inverse, state, false);

    assert(first.num_anomalies == 0);
    assert(second.num_anomalies == 1);
    assert(second.alarm_lines[0] ==
           "2025-11-15T12:00:02Z;R3;HIGH;VOLT-MAIN;19.0\n");
}

void test_csv_validation_helpers() {
    std::string_view ts;
    std::string_view sensor;
    std::string_view value_field;
    std::string_view priority;

    assert(tokenize_csv_line("2025-11-15T12:00:00Z,TEMP-01,25.5,HIGH",
                             ts, sensor, value_field, priority));
    assert(ts == "2025-11-15T12:00:00Z");
    assert(sensor == "TEMP-01");
    assert(value_field == "25.5");
    assert(priority == "HIGH");

    std::vector<std::string> inverse = {"TEMP-01", "PRES-01"};
    auto token_view_map = build_token_view_map(inverse);
    assert(lookup_sensor_token(token_view_map, sensor) == 0);
    assert(lookup_sensor_token(token_view_map, "UNKNOWN") == -1);

    assert(!tokenize_csv_line("2025-11-15T12:00:00Z,TEMP-01,25.5",
                              ts, sensor, value_field, priority));
    assert(!tokenize_csv_line("2025-11-15T12:00:00Z,TEMP-01,25.5,HIGH,EXTRA",
                              ts, sensor, value_field, priority));

    assert(validate_schema(ts, sensor, value_field, priority));
    assert(!validate_schema(ts, "", value_field, priority));
    assert(!validate_schema(ts, "CORRUPT", value_field, priority));
    assert(!validate_schema(ts, sensor, "ERR", priority));

    double parsed = 0.0;
    assert(parse_value("25.5", parsed));
    assert(parsed == 25.5);
    assert(!parse_value("SYSTEM_HALT_ERR", parsed));
}

void test_benchmark_mode_disables_audit_files() {
    std::vector<std::string> inverse = {"TEMP-01"};
    BatchConfig config;
    config.count_threshold = 1;
    config.batch_output_dir = "test_batches_disabled_tmp";
    config.write_audit_files = false;

    std::filesystem::remove_all(config.batch_output_dir);

    BatchAccumulator accumulator(config, inverse);
    accumulator.add_record(make_record("2025-11-15T12:00:00Z", 0, 1.0));
    auto batch = accumulator.flush();

    assert(batch.size() == 1);
    assert(!std::filesystem::exists(config.batch_output_dir));
}

void test_batch_flush_keeps_timestamps_intact() {
    std::vector<std::string> inverse = {"TEMP-01"};
    BatchConfig config;
    config.count_threshold = 2;
    config.batch_output_dir = "test_batches_tmp";

    BatchAccumulator accumulator(config, inverse);
    std::vector<ParsedRecord> records = {
        make_record("2025-11-15T12:00:00Z", 0, 1.0),
        make_record("2025-11-15T12:00:00Z", 0, 2.0),
        make_record("2025-11-15T12:00:00Z", 0, 3.0),
        make_record("2025-11-15T12:00:01Z", 0, 4.0),
    };

    std::vector<size_t> flushed_sizes;
    for (size_t i = 0; i < records.size(); ++i) {
        const bool timestamp_boundary =
            (i + 1 == records.size()) ||
            (records[i + 1].timestamp != records[i].timestamp);

        accumulator.add_record(std::move(records[i]));

        if (timestamp_boundary && accumulator.should_flush()) {
            auto batch = accumulator.flush();
            flushed_sizes.push_back(batch.size());
        }
    }

    auto final_batch = accumulator.flush_remaining();
    if (!final_batch.empty()) {
        flushed_sizes.push_back(final_batch.size());
    }

    assert(flushed_sizes.size() == 2);
    assert(flushed_sizes[0] == 3);
    assert(flushed_sizes[1] == 1);

    std::filesystem::remove_all(config.batch_output_dir);
}

} // namespace

int main() {

    std::cout << "Running processing tests...\n";
    omp_set_num_threads(1);

    test_strict_output_format();
    test_rule_violation_inline_and_fallback_storage();
    test_pipeline_rule_semantics();
    test_cross_batch_state_persistence();
    test_csv_validation_helpers();
    test_benchmark_mode_disables_audit_files();
    test_batch_flush_keeps_timestamps_intact();

    std::cout << "All tests passed\n";

    return 0;
}
