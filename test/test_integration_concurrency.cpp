#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <omp.h>

#include "batch_accumulator.hpp"
#include "test_helpers.hpp"
#include "timestamp_processor.hpp"
#include "types.hpp"

namespace {

using namespace astralog;
using namespace astralog::test;

// ---------------------------------------------------------------------------
// CI guard: limit expensive iterations when ASTRALOG_CI is set
// ---------------------------------------------------------------------------

/// Returns true when running inside CI (ASTRALOG_CI env var is set).
static bool is_ci() {
    return std::getenv("ASTRALOG_CI") != nullptr;
}

/// Full iteration count for local runs; reduced count for CI.
static int parallel_iterations() {
    return is_ci() ? 3 : 25;
}

// ---------------------------------------------------------------------------
// Data structures
// ---------------------------------------------------------------------------

struct TestData {
    std::vector<std::string>              sensor_ids;
    std::unordered_map<std::string, int>  token_map;
    std::vector<std::string>              inverse;
    std::vector<ParsedRecord>             records;
};

struct OutputSnapshot {
    std::string valid;
    std::string alarms;
    uint64_t timestamps = 0;
    uint64_t anomalies  = 0;
    uint64_t nominal    = 0;
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string two_digits(int value) {
    std::ostringstream oss;
    oss << std::setfill('0') << std::setw(2) << value;
    return oss.str();
}

static std::string rule_id(int value) {
    std::ostringstream oss;
    oss << 'R' << std::setfill('0') << std::setw(3) << value;
    return oss.str();
}

// make_rule and make_record are provided by test_helpers.hpp

OutputSnapshot snapshot_from_result(const ProcessingResult& result) {
    OutputSnapshot snap;
    snap.timestamps = result.num_timestamps;
    snap.anomalies  = result.num_anomalies;
    snap.nominal    = result.num_nominal;
    for (const auto& line : result.valid_lines)
        if (!line.empty()) snap.valid  += line;
    for (const auto& line : result.alarm_lines)
        if (!line.empty()) snap.alarms += line;
    return snap;
}

void append_result(OutputSnapshot& aggregate, const ProcessingResult& result) {
    auto cur = snapshot_from_result(result);
    aggregate.valid      += cur.valid;
    aggregate.alarms     += cur.alarms;
    aggregate.timestamps += cur.timestamps;
    aggregate.anomalies  += cur.anomalies;
    aggregate.nominal    += cur.nominal;
}

void expect_same_snapshot(const OutputSnapshot& actual,
                          const OutputSnapshot& expected,
                          const std::string& context) {
    EXPECT_EQ(actual.timestamps, expected.timestamps) << context << ": timestamp count";
    EXPECT_EQ(actual.anomalies,  expected.anomalies)  << context << ": anomaly count";
    EXPECT_EQ(actual.nominal,    expected.nominal)    << context << ": nominal count";
    EXPECT_EQ(actual.valid,      expected.valid)      << context << ": valid output";
    EXPECT_EQ(actual.alarms,     expected.alarms)     << context << ": alarm output";
}

// ---------------------------------------------------------------------------
// Dataset / rule builders
// ---------------------------------------------------------------------------

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
            if      (s == 0) value = (t % 7 == 0) ? 120.0 : 40.0;
            else if (s == 1) value = 200.0 - (3.0 * t);
            else if (s == 2) value = (t % 9 < 5) ? 10.0 : 30.0;
            else if (s == 3) value = (t % 4 == 0) ? 5.0 : 100.0;
            else if (t % 6 == s % 6) value = 100.0 + s;

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
        Priority prio = (s % 3 == 0) ? Priority::HIGH
                      : (s % 3 == 1) ? Priority::MEDIUM
                                     : Priority::LOW;
        rules.push_back(make_rule(rule_id(10 + s), RuleType::THRESHOLD,
                                  "S" + two_digits(s),
                                  CompOp::GT, 90.0, prio));
    }

    RuleDefinition corr_and;
    corr_and.rule_id   = "R100";
    corr_and.type      = RuleType::CORRELATION;
    corr_and.logic     = "AND";
    corr_and.sub_rules = {"R001", "R002"};
    corr_and.priority  = Priority::HIGH;
    rules.push_back(corr_and);

    RuleDefinition corr_or;
    corr_or.rule_id   = "R101";
    corr_or.type      = RuleType::CORRELATION;
    corr_or.logic     = "OR";
    corr_or.sub_rules = {"R003", "R004"};
    corr_or.priority  = Priority::MEDIUM;
    rules.push_back(corr_or);

    return rules;
}

OutputSnapshot run_single_pass(const TestData& data, int threads) {
    omp_set_dynamic(0);
    omp_set_num_threads(threads);

    auto rules    = build_rules();
    auto prepared = prepare_rules(rules, data.token_map,
                                  static_cast<int>(data.sensor_ids.size()));
    PipelineState state;
    return snapshot_from_result(
        process_pipeline(data.records, prepared, data.inverse, state, false));
}

OutputSnapshot run_batched(const TestData& data, int threads, size_t batch_size) {
    omp_set_dynamic(0);
    omp_set_num_threads(threads);

    auto rules    = build_rules();
    auto prepared = prepare_rules(rules, data.token_map,
                                  static_cast<int>(data.sensor_ids.size()));

    BatchConfig config;
    config.strategy          = BatchStrategy::COUNT;
    config.count_threshold   = batch_size;
    config.write_audit_files = false;

    BatchAccumulator accumulator(config, data.inverse);
    PipelineState state;
    OutputSnapshot aggregate;

    auto process_batch = [&](std::vector<ParsedRecord>& batch) {
        auto result = process_pipeline(batch, prepared, data.inverse, state, false);
        append_result(aggregate, result);
    };

    for (size_t i = 0; i < data.records.size(); ++i) {
        const bool boundary =
            (i + 1 == data.records.size()) ||
            (data.records[i + 1].timestamp != data.records[i].timestamp);
        accumulator.add_record(data.records[i]);
        if (boundary && accumulator.should_flush()) {
            auto batch = accumulator.flush();
            process_batch(batch);
        }
    }
    auto final_batch = accumulator.flush_remaining();
    if (!final_batch.empty()) process_batch(final_batch);
    return aggregate;
}

// ---------------------------------------------------------------------------
// TEST cases
// ---------------------------------------------------------------------------

TEST(ConcurrencyIntegrationTest, ThreadCountEquivalence) {
    const auto data     = build_test_data();
    const auto baseline = run_single_pass(data, 1);

    EXPECT_EQ(baseline.timestamps, static_cast<uint64_t>(48));
    EXPECT_FALSE(baseline.alarms.empty());

    for (int threads : {2, 3, 4, 8, 12, 16}) {
        auto observed = run_single_pass(data, threads);
        expect_same_snapshot(observed, baseline,
                             "threads=" + std::to_string(threads));
    }
}

TEST(ConcurrencyIntegrationTest, RepeatedParallelDeterminism) {
    const auto data     = build_test_data();
    const auto baseline = run_single_pass(data, 1);

    const int iterations = parallel_iterations();
    for (int iter = 0; iter < iterations; ++iter) {
        for (int threads : {2, 4, 8, 12}) {
            auto observed = run_single_pass(data, threads);
            expect_same_snapshot(observed, baseline,
                                 "iter=" + std::to_string(iter) +
                                 " threads=" + std::to_string(threads));
        }
    }
}

TEST(ConcurrencyIntegrationTest, BatchSizeMetamorphicRelation) {
    const auto data     = build_test_data();
    const auto baseline = run_single_pass(data, 8);

    for (size_t bs : {1u, 5u, 7u, 11u, 12u, 13u, 37u, 1000u}) {
        auto observed = run_batched(data, 8, bs);
        expect_same_snapshot(observed, baseline,
                             "batch_size=" + std::to_string(bs));
    }
}

} // namespace
