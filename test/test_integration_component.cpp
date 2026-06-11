#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <omp.h>

#include "batch_accumulator.hpp"
#include "csv_parser.hpp"
#include "rules_engine.hpp"
#include "timestamp_processor.hpp"
#include "types.hpp"
#include "yaml_parser.hpp"

namespace {

namespace fs = std::filesystem;
using namespace astralog;

// ---------------------------------------------------------------------------
// Fixture path helpers
// ---------------------------------------------------------------------------

fs::path fixture_root() {
    // Allow override via environment variable for CI
    const char* env = std::getenv("ASTRALOG_FIXTURE_ROOT");
    return env ? fs::path(env) : fs::path("test/fixtures");
}

fs::path integration_dir() { return fixture_root() / "integration"; }
fs::path faults_dir()      { return fixture_root() / "faults"; }

// ---------------------------------------------------------------------------
// Internal helpers (kept as plain functions, called from TEST bodies)
// ---------------------------------------------------------------------------

std::string read_all(const fs::path& path) {
    std::ifstream input(path, std::ios::in | std::ios::binary);
    EXPECT_TRUE(input.is_open()) << "Cannot open fixture file: " << path;
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::unordered_map<std::string, int>
build_token_map_from_sensors(const std::vector<std::string>& sensor_ids) {
    std::unordered_map<std::string, int> token_map;
    for (const auto& sensor_id : sensor_ids) {
        if (token_map.find(sensor_id) == token_map.end()) {
            token_map[sensor_id] = static_cast<int>(token_map.size());
        }
    }
    return token_map;
}

std::vector<ParsedRecord> parse_csv_fixture(
    const fs::path& csv_path,
    const TokenViewMap& token_view_map,
    int threads,
    uint64_t& total_valid,
    uint64_t& total_invalid)
{
    omp_set_dynamic(0);
    omp_set_num_threads(threads);

    MappedFile csv_file;
    EXPECT_TRUE(csv_file.open(csv_path.string().c_str()))
        << "Failed to memory-map CSV fixture: " << csv_path;

    auto line_offsets = build_line_offsets(csv_file.data, csv_file.size);
    return parse_csv_parallel(csv_file, line_offsets, token_view_map,
                              total_valid, total_invalid);
}

bool same_records(const std::vector<ParsedRecord>& lhs,
                  const std::vector<ParsedRecord>& rhs) {
    if (lhs.size() != rhs.size()) return false;
    for (size_t i = 0; i < lhs.size(); ++i) {
        if (lhs[i].timestamp    != rhs[i].timestamp)    return false;
        if (lhs[i].sensor_token != rhs[i].sensor_token) return false;
        if (lhs[i].value        != rhs[i].value)        return false;
    }
    return true;
}

struct OutputSnapshot {
    std::string valid;
    std::string alarms;
    uint64_t timestamps = 0;
    uint64_t anomalies  = 0;
    uint64_t nominal    = 0;
};

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
    aggregate.valid       += cur.valid;
    aggregate.alarms      += cur.alarms;
    aggregate.timestamps  += cur.timestamps;
    aggregate.anomalies   += cur.anomalies;
    aggregate.nominal     += cur.nominal;
}

OutputSnapshot run_pipeline_once(const std::vector<ParsedRecord>& records,
                                 const PreparedRules& prepared_rules,
                                 const std::vector<std::string>& inverse) {
    PipelineState state;
    return snapshot_from_result(
        process_pipeline(records, prepared_rules, inverse, state, false));
}

OutputSnapshot run_pipeline_batched(const std::vector<ParsedRecord>& records,
                                    const PreparedRules& prepared_rules,
                                    const std::vector<std::string>& inverse,
                                    size_t batch_size) {
    BatchConfig config;
    config.strategy         = BatchStrategy::COUNT;
    config.count_threshold  = batch_size;
    config.write_audit_files = false;

    BatchAccumulator accumulator(config, inverse);
    PipelineState state;
    OutputSnapshot aggregate;

    auto process_batch = [&](std::vector<ParsedRecord>& batch) {
        auto result = process_pipeline(batch, prepared_rules, inverse, state, false);
        append_result(aggregate, result);
    };

    for (size_t i = 0; i < records.size(); ++i) {
        const bool boundary =
            (i + 1 == records.size()) ||
            (records[i + 1].timestamp != records[i].timestamp);
        accumulator.add_record(records[i]);
        if (boundary && accumulator.should_flush()) {
            auto batch = accumulator.flush();
            process_batch(batch);
        }
    }
    auto final_batch = accumulator.flush_remaining();
    if (!final_batch.empty()) process_batch(final_batch);
    return aggregate;
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
// TEST cases
// ---------------------------------------------------------------------------

TEST(ComponentIntegrationTest, YamlToCsvTokenIntegration) {
    const auto sensor_ids =
        load_sensors_from_yaml((integration_dir() / "sensors.yaml").string());
    ASSERT_EQ(sensor_ids.size(), static_cast<size_t>(4))
        << "Unexpected number of sensors loaded from YAML";

    auto token_map     = build_token_map_from_sensors(sensor_ids);
    auto inverse       = build_inverse_token_map(token_map);
    auto token_view_map = build_token_view_map(inverse);

    EXPECT_EQ(inverse[0], std::string("S-A"));
    EXPECT_EQ(inverse[1], std::string("S-B"));
    EXPECT_EQ(inverse[2], std::string("S-C"));
    EXPECT_EQ(inverse[3], std::string("S-D"));

    uint64_t valid = 0, invalid = 0;
    auto baseline = parse_csv_fixture(integration_dir() / "telemetry.csv",
                                     token_view_map, 1, valid, invalid);

    EXPECT_EQ(valid,   static_cast<uint64_t>(20));
    EXPECT_EQ(invalid, static_cast<uint64_t>(0));

    EXPECT_EQ(baseline[0].sensor_token, 0);
    EXPECT_EQ(baseline[1].sensor_token, 1);
    EXPECT_EQ(baseline[2].sensor_token, 2);
    EXPECT_EQ(baseline[3].sensor_token, 3);

    for (int threads : {2, 3, 4, 8}) {
        uint64_t tv = 0, ti = 0;
        auto threaded = parse_csv_fixture(integration_dir() / "telemetry.csv",
                                         token_view_map, threads, tv, ti);
        EXPECT_EQ(tv, valid)   << "Thread=" << threads << ": valid count mismatch";
        EXPECT_EQ(ti, invalid) << "Thread=" << threads << ": invalid count mismatch";
        EXPECT_TRUE(same_records(threaded, baseline))
            << "Thread=" << threads << ": record order not preserved";
    }
}

TEST(ComponentIntegrationTest, RulesToPreparedRulesIntegration) {
    const auto sensor_ids =
        load_sensors_from_yaml((integration_dir() / "sensors.yaml").string());
    auto token_map = build_token_map_from_sensors(sensor_ids);

    auto rules   = load_rules((integration_dir() / "rules.json").string());
    auto prepared = prepare_rules(rules, token_map,
                                  static_cast<int>(sensor_ids.size()));

    EXPECT_EQ(prepared.threshold_count,          static_cast<size_t>(1));
    EXPECT_EQ(prepared.step_diff_count,          static_cast<size_t>(1));
    EXPECT_EQ(prepared.stateful_count,           static_cast<size_t>(1));
    EXPECT_EQ(prepared.correlation_rules.size(), static_cast<size_t>(2));

    EXPECT_EQ(prepared.sensor_rules[0].threshold[0]->rule_id, std::string("R001"));
    EXPECT_EQ(prepared.sensor_rules[1].step_diff[0]->rule_id, std::string("R002"));
    EXPECT_EQ(prepared.sensor_rules[2].stateful[0].rule->rule_id, std::string("R003"));
    EXPECT_EQ(prepared.correlation_rules[0]->rule_id, std::string("R004"));
    EXPECT_EQ(prepared.correlation_rules[1]->rule_id, std::string("R005"));
}

TEST(ComponentIntegrationTest, CsvToBatchAccumulatorIntegration) {
    const auto sensor_ids =
        load_sensors_from_yaml((integration_dir() / "sensors.yaml").string());
    auto token_map      = build_token_map_from_sensors(sensor_ids);
    auto inverse        = build_inverse_token_map(token_map);
    auto token_view_map = build_token_view_map(inverse);

    uint64_t valid = 0, invalid = 0;
    auto records = parse_csv_fixture(integration_dir() / "telemetry.csv",
                                     token_view_map, 4, valid, invalid);

    BatchConfig config;
    config.strategy         = BatchStrategy::COUNT;
    config.count_threshold  = 2;
    config.write_audit_files = false;

    BatchAccumulator accumulator(config, inverse);
    std::vector<std::vector<ParsedRecord>> batches;

    for (size_t i = 0; i < records.size(); ++i) {
        const bool boundary =
            (i + 1 == records.size()) ||
            (records[i + 1].timestamp != records[i].timestamp);
        accumulator.add_record(records[i]);
        if (boundary && accumulator.should_flush()) {
            batches.push_back(accumulator.flush());
        }
    }
    auto final_batch = accumulator.flush_remaining();
    if (!final_batch.empty()) batches.push_back(std::move(final_batch));

    ASSERT_EQ(batches.size(), static_cast<size_t>(5));
    for (size_t i = 0; i < batches.size(); ++i) {
        EXPECT_EQ(batches[i].size(), static_cast<size_t>(4))
            << "Batch " << i << " split a timestamp group";
        const std::string ts = batches[i][0].timestamp;
        for (const auto& rec : batches[i]) {
            EXPECT_EQ(rec.timestamp, ts) << "Batch " << i << " has mixed timestamps";
        }
    }
}

TEST(ComponentIntegrationTest, BatchPipelineFormatterIntegration) {
    const auto sensor_ids =
        load_sensors_from_yaml((integration_dir() / "sensors.yaml").string());
    auto token_map      = build_token_map_from_sensors(sensor_ids);
    auto inverse        = build_inverse_token_map(token_map);
    auto token_view_map = build_token_view_map(inverse);

    uint64_t valid = 0, invalid = 0;
    auto records = parse_csv_fixture(integration_dir() / "telemetry.csv",
                                     token_view_map, 4, valid, invalid);

    auto rules    = load_rules((integration_dir() / "rules.json").string());
    auto prepared = prepare_rules(rules, token_map,
                                  static_cast<int>(sensor_ids.size()));

    const auto expected_valid  = read_all(integration_dir() / "expected_valid_data.csv");
    const auto expected_alarms = read_all(integration_dir() / "expected_alarms.log");

    auto single_pass = run_pipeline_once(records, prepared, inverse);
    EXPECT_EQ(single_pass.valid,      expected_valid)  << "Valid data output mismatch";
    EXPECT_EQ(single_pass.alarms,     expected_alarms) << "Alarm output mismatch";
    EXPECT_EQ(single_pass.timestamps, static_cast<uint64_t>(5));
    EXPECT_EQ(single_pass.nominal,    static_cast<uint64_t>(3));
    EXPECT_EQ(single_pass.anomalies,  static_cast<uint64_t>(2));

    for (size_t bs : {1u, 2u, 3u, 4u, 5u, 7u, 20u, 1000u}) {
        auto batched = run_pipeline_batched(records, prepared, inverse, bs);
        expect_same_snapshot(batched, single_pass,
                             "batch_size=" + std::to_string(bs));
    }
}

TEST(ComponentIntegrationTest, FaultyCsvIntegration) {
    const auto sensor_ids =
        load_sensors_from_yaml((integration_dir() / "sensors.yaml").string());
    auto token_map      = build_token_map_from_sensors(sensor_ids);
    auto inverse        = build_inverse_token_map(token_map);
    auto token_view_map = build_token_view_map(inverse);

    uint64_t valid = 0, invalid = 0;
    auto records = parse_csv_fixture(faults_dir() / "telemetry_faults.csv",
                                     token_view_map, 3, valid, invalid);

    EXPECT_EQ(valid,           static_cast<uint64_t>(2));
    EXPECT_EQ(invalid,         static_cast<uint64_t>(5));
    EXPECT_EQ(records.size(),  static_cast<size_t>(2));
    EXPECT_EQ(records[0].sensor_token, 0);
    EXPECT_EQ(records[1].sensor_token, -1);

    std::vector<RuleDefinition> no_rules;
    auto prepared = prepare_rules(no_rules, token_map,
                                  static_cast<int>(sensor_ids.size()));
    auto snapshot = run_pipeline_once(records, prepared, inverse);

    EXPECT_EQ(snapshot.valid,
              std::string("2026-01-01T00:00:00Z;NOMINAL;S-A:10.0|UNKNOWN:12.0\n"));
    EXPECT_TRUE(snapshot.alarms.empty());
}

} // namespace
