#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <omp.h>

#include "batch_accumulator.hpp"
#include "timestamp_processor.hpp"
#include "types.hpp"

namespace {

using namespace astralog;

struct TestData {
    std::vector<std::string> sensor_ids;
    std::unordered_map<std::string, int> token_map;
    std::vector<std::string> inverse;
    std::vector<ParsedRecord> records;
};

struct OutputSnapshot {
    std::string valid;
    std::string alarms;
    uint64_t timestamps = 0;
    uint64_t anomalies = 0;
    uint64_t nominal = 0;
};

void require_true(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Actual, typename Expected>
void require_equal(const Actual& actual,
                   const Expected& expected,
                   const std::string& message) {
    if (!(actual == expected)) {
        std::ostringstream oss;
        oss << message << " (actual=" << actual
            << ", expected=" << expected << ")";
        throw std::runtime_error(oss.str());
    }
}

std::string two_digits(int value) {
    std::ostringstream oss;
    oss << std::setfill('0') << std::setw(2) << value;
    return oss.str();
}

std::string rule_id(int value) {
    std::ostringstream oss;
    oss << 'R' << std::setfill('0') << std::setw(3) << value;
    return oss.str();
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

ParsedRecord make_record(const std::string& timestamp,
                         int sensor_token,
                         double value) {
    ParsedRecord record;
    record.timestamp = timestamp;
    record.sensor_token = sensor_token;
    record.value = value;
    return record;
}

OutputSnapshot snapshot_from_result(const ProcessingResult& result) {
    OutputSnapshot snapshot;
    snapshot.timestamps = result.num_timestamps;
    snapshot.anomalies = result.num_anomalies;
    snapshot.nominal = result.num_nominal;

    for (const auto& line : result.valid_lines) {
        if (!line.empty()) snapshot.valid += line;
    }
    for (const auto& line : result.alarm_lines) {
        if (!line.empty()) snapshot.alarms += line;
    }

    return snapshot;
}

void append_result(OutputSnapshot& aggregate, const ProcessingResult& result) {
    auto current = snapshot_from_result(result);
    aggregate.valid += current.valid;
    aggregate.alarms += current.alarms;
    aggregate.timestamps += current.timestamps;
    aggregate.anomalies += current.anomalies;
    aggregate.nominal += current.nominal;
}

void require_same_snapshot(const OutputSnapshot& actual,
                           const OutputSnapshot& expected,
                           const std::string& context) {
    require_equal(actual.timestamps, expected.timestamps,
                  context + ": timestamp count mismatch");
    require_equal(actual.anomalies, expected.anomalies,
                  context + ": anomaly count mismatch");
    require_equal(actual.nominal, expected.nominal,
                  context + ": nominal count mismatch");
    require_equal(actual.valid, expected.valid,
                  context + ": valid output mismatch");
    require_equal(actual.alarms, expected.alarms,
                  context + ": alarm output mismatch");
}

TestData build_test_data() {
    TestData data;

    for (int s = 0; s < 12; ++s) {
        std::string sensor_id = "S" + two_digits(s);
        data.sensor_ids.push_back(sensor_id);
        data.token_map[sensor_id] = s;
    }
    data.inverse = data.sensor_ids;

    for (int t = 0; t < 48; ++t) {
        const std::string timestamp =
            "2026-02-01T00:00:" + two_digits(t) + "Z";

        for (int s = 0; s < 12; ++s) {
            double value = 20.0 + s;

            if (s == 0) {
                value = (t % 7 == 0) ? 120.0 : 40.0;
            } else if (s == 1) {
                value = 200.0 - (3.0 * t);
            } else if (s == 2) {
                value = (t % 9 < 5) ? 10.0 : 30.0;
            } else if (s == 3) {
                value = (t % 4 == 0) ? 5.0 : 100.0;
            } else if (t % 6 == s % 6) {
                value = 100.0 + s;
            }

            data.records.push_back(make_record(timestamp, s, value));
        }
    }

    return data;
}

std::vector<RuleDefinition> build_rules() {
    std::vector<RuleDefinition> rules;

    rules.push_back(make_rule("R001", RuleType::THRESHOLD, "S00",
                              CompOp::GT, 90.0, Priority::HIGH));
    rules.push_back(make_rule("R002", RuleType::STEP_DIFF, "S01",
                              CompOp::LT, -2.0, Priority::MEDIUM));

    RuleDefinition stateful = make_rule("R003", RuleType::STATEFUL, "S02",
                                        CompOp::LT, 20.0, Priority::LOW);
    stateful.consecutive_measurements = 3;
    rules.push_back(stateful);

    rules.push_back(make_rule("R004", RuleType::THRESHOLD, "S03",
                              CompOp::LT, 10.0, Priority::HIGH));

    for (int s = 4; s < 12; ++s) {
        Priority priority = (s % 3 == 0) ? Priority::HIGH
                          : (s % 3 == 1) ? Priority::MEDIUM
                                         : Priority::LOW;
        rules.push_back(make_rule(rule_id(10 + s), RuleType::THRESHOLD,
                                  "S" + two_digits(s),
                                  CompOp::GT, 90.0, priority));
    }

    RuleDefinition corr_and;
    corr_and.rule_id = "R100";
    corr_and.type = RuleType::CORRELATION;
    corr_and.logic = "AND";
    corr_and.sub_rules = {"R001", "R002"};
    corr_and.priority = Priority::HIGH;
    rules.push_back(corr_and);

    RuleDefinition corr_or;
    corr_or.rule_id = "R101";
    corr_or.type = RuleType::CORRELATION;
    corr_or.logic = "OR";
    corr_or.sub_rules = {"R003", "R004"};
    corr_or.priority = Priority::MEDIUM;
    rules.push_back(corr_or);

    return rules;
}

OutputSnapshot run_single_pass(const TestData& data, int threads) {
    omp_set_dynamic(0);
    omp_set_num_threads(threads);

    auto rules = build_rules();
    auto prepared = prepare_rules(rules, data.token_map,
                                  static_cast<int>(data.sensor_ids.size()));
    PipelineState state;

    return snapshot_from_result(
        process_pipeline(data.records, prepared, data.inverse, state, false));
}

OutputSnapshot run_batched(const TestData& data,
                           int threads,
                           size_t batch_size) {
    omp_set_dynamic(0);
    omp_set_num_threads(threads);

    auto rules = build_rules();
    auto prepared = prepare_rules(rules, data.token_map,
                                  static_cast<int>(data.sensor_ids.size()));

    BatchConfig config;
    config.strategy = BatchStrategy::COUNT;
    config.count_threshold = batch_size;
    config.write_audit_files = false;

    BatchAccumulator accumulator(config, data.inverse);
    PipelineState state;
    OutputSnapshot aggregate;

    auto process_batch = [&](std::vector<ParsedRecord>& batch) {
        auto result = process_pipeline(batch, prepared, data.inverse,
                                       state, false);
        append_result(aggregate, result);
    };

    for (size_t i = 0; i < data.records.size(); ++i) {
        const bool timestamp_boundary =
            (i + 1 == data.records.size()) ||
            (data.records[i + 1].timestamp != data.records[i].timestamp);

        accumulator.add_record(data.records[i]);

        if (timestamp_boundary && accumulator.should_flush()) {
            auto batch = accumulator.flush();
            process_batch(batch);
        }
    }

    auto final_batch = accumulator.flush_remaining();
    if (!final_batch.empty()) {
        process_batch(final_batch);
    }

    return aggregate;
}

void test_thread_count_equivalence() {
    const auto data = build_test_data();
    const auto baseline = run_single_pass(data, 1);

    require_equal(baseline.timestamps, static_cast<uint64_t>(48),
                  "Synthetic concurrency dataset timestamp count mismatch");
    require_true(!baseline.alarms.empty(),
                 "Synthetic concurrency dataset should produce alarms");

    for (int threads : {2, 3, 4, 8, 12, 16}) {
        auto observed = run_single_pass(data, threads);
        require_same_snapshot(observed, baseline,
                              "Thread-count equivalence failed");
    }
}

void test_repeated_parallel_determinism() {
    const auto data = build_test_data();
    const auto baseline = run_single_pass(data, 1);

    for (int iteration = 0; iteration < 25; ++iteration) {
        for (int threads : {2, 4, 8, 12}) {
            auto observed = run_single_pass(data, threads);
            require_same_snapshot(observed, baseline,
                                  "Repeated parallel determinism failed");
        }
    }
}

void test_batch_size_metamorphic_relation() {
    const auto data = build_test_data();
    const auto baseline = run_single_pass(data, 8);

    for (size_t batch_size : {1u, 5u, 7u, 11u, 12u, 13u, 37u, 1000u}) {
        auto observed = run_batched(data, 8, batch_size);
        require_same_snapshot(observed, baseline,
                              "Batch-size metamorphic relation failed");
    }
}

} // namespace

int main() {
    try {
        test_thread_count_equivalence();
        test_repeated_parallel_determinism();
        test_batch_size_metamorphic_relation();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[concurrency_integration_tests] " << e.what() << "\n";
        return 1;
    }
}
