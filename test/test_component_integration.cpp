#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
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

struct FixturePaths {
    fs::path root;
    fs::path integration;
    fs::path faults;
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

std::string read_all(const fs::path& path) {
    std::ifstream input(path, std::ios::in | std::ios::binary);
    require_true(input.is_open(), "Cannot open fixture file: " + path.string());

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
    uint64_t& total_invalid) {

    omp_set_dynamic(0);
    omp_set_num_threads(threads);

    MappedFile csv_file;
    require_true(csv_file.open(csv_path.string().c_str()),
                 "Failed to memory-map CSV fixture: " + csv_path.string());

    auto line_offsets = build_line_offsets(csv_file.data, csv_file.size);
    return parse_csv_parallel(csv_file, line_offsets, token_view_map,
                              total_valid, total_invalid);
}

bool same_records(const std::vector<ParsedRecord>& lhs,
                  const std::vector<ParsedRecord>& rhs) {
    if (lhs.size() != rhs.size()) return false;

    for (size_t i = 0; i < lhs.size(); ++i) {
        if (lhs[i].timestamp != rhs[i].timestamp) return false;
        if (lhs[i].sensor_token != rhs[i].sensor_token) return false;
        if (lhs[i].value != rhs[i].value) return false;
    }

    return true;
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
    OutputSnapshot current = snapshot_from_result(result);
    aggregate.valid += current.valid;
    aggregate.alarms += current.alarms;
    aggregate.timestamps += current.timestamps;
    aggregate.anomalies += current.anomalies;
    aggregate.nominal += current.nominal;
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
    config.strategy = BatchStrategy::COUNT;
    config.count_threshold = batch_size;
    config.write_audit_files = false;

    BatchAccumulator accumulator(config, inverse);
    PipelineState state;
    OutputSnapshot aggregate;

    auto process_batch = [&](std::vector<ParsedRecord>& batch) {
        auto result = process_pipeline(batch, prepared_rules, inverse,
                                       state, false);
        append_result(aggregate, result);
    };

    for (size_t i = 0; i < records.size(); ++i) {
        const bool timestamp_boundary =
            (i + 1 == records.size()) ||
            (records[i + 1].timestamp != records[i].timestamp);

        accumulator.add_record(records[i]);

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
                  context + ": valid_data output mismatch");
    require_equal(actual.alarms, expected.alarms,
                  context + ": alarms output mismatch");
}

void test_yaml_to_csv_token_integration(const FixturePaths& paths) {
    const auto sensor_ids =
        load_sensors_from_yaml((paths.integration / "sensors.yaml").string());
    require_equal(sensor_ids.size(), static_cast<size_t>(4),
                  "Unexpected number of sensors loaded from YAML");

    auto token_map = build_token_map_from_sensors(sensor_ids);
    auto inverse = build_inverse_token_map(token_map);
    auto token_view_map = build_token_view_map(inverse);

    require_equal(inverse[0], std::string("S-A"),
                  "YAML/token inverse map mismatch for token 0");
    require_equal(inverse[1], std::string("S-B"),
                  "YAML/token inverse map mismatch for token 1");
    require_equal(inverse[2], std::string("S-C"),
                  "YAML/token inverse map mismatch for token 2");
    require_equal(inverse[3], std::string("S-D"),
                  "YAML/token inverse map mismatch for token 3");

    uint64_t valid = 0;
    uint64_t invalid = 0;
    auto baseline = parse_csv_fixture(paths.integration / "telemetry.csv",
                                      token_view_map, 1, valid, invalid);

    require_equal(valid, static_cast<uint64_t>(20),
                  "CSV parser valid-row count mismatch");
    require_equal(invalid, static_cast<uint64_t>(0),
                  "CSV parser invalid-row count mismatch");

    require_equal(baseline[0].sensor_token, 0,
                  "First parsed CSV row did not use YAML token for S-A");
    require_equal(baseline[1].sensor_token, 1,
                  "Second parsed CSV row did not use YAML token for S-B");
    require_equal(baseline[2].sensor_token, 2,
                  "Third parsed CSV row did not use YAML token for S-C");
    require_equal(baseline[3].sensor_token, 3,
                  "Fourth parsed CSV row did not use YAML token for S-D");

    for (int threads : {2, 3, 4, 8}) {
        uint64_t threaded_valid = 0;
        uint64_t threaded_invalid = 0;
        auto threaded = parse_csv_fixture(paths.integration / "telemetry.csv",
                                          token_view_map, threads,
                                          threaded_valid, threaded_invalid);
        require_equal(threaded_valid, valid,
                      "Threaded CSV parse valid-count mismatch");
        require_equal(threaded_invalid, invalid,
                      "Threaded CSV parse invalid-count mismatch");
        require_true(same_records(threaded, baseline),
                     "Parallel CSV parse did not preserve record order");
    }
}

void test_rules_to_prepared_rules_integration(const FixturePaths& paths) {
    const auto sensor_ids =
        load_sensors_from_yaml((paths.integration / "sensors.yaml").string());
    auto token_map = build_token_map_from_sensors(sensor_ids);

    auto rules = load_rules((paths.integration / "rules.json").string());
    auto prepared = prepare_rules(rules, token_map,
                                  static_cast<int>(sensor_ids.size()));

    require_equal(prepared.threshold_count, static_cast<size_t>(1),
                  "Prepared threshold rule count mismatch");
    require_equal(prepared.step_diff_count, static_cast<size_t>(1),
                  "Prepared step-diff rule count mismatch");
    require_equal(prepared.stateful_count, static_cast<size_t>(1),
                  "Prepared stateful rule count mismatch");
    require_equal(prepared.correlation_rules.size(), static_cast<size_t>(2),
                  "Prepared correlation rule count mismatch");

    require_equal(prepared.sensor_rules[0].threshold[0]->rule_id,
                  std::string("R001"),
                  "S-A threshold rule was not attached to token 0");
    require_equal(prepared.sensor_rules[1].step_diff[0]->rule_id,
                  std::string("R002"),
                  "S-B step-diff rule was not attached to token 1");
    require_equal(prepared.sensor_rules[2].stateful[0].rule->rule_id,
                  std::string("R003"),
                  "S-C stateful rule was not attached to token 2");
    require_equal(prepared.correlation_rules[0]->rule_id,
                  std::string("R004"),
                  "Correlation rules were not sorted by priority");
    require_equal(prepared.correlation_rules[1]->rule_id,
                  std::string("R005"),
                  "Correlation rules were not sorted by priority");
}

void test_csv_to_batch_accumulator_integration(const FixturePaths& paths) {
    const auto sensor_ids =
        load_sensors_from_yaml((paths.integration / "sensors.yaml").string());
    auto token_map = build_token_map_from_sensors(sensor_ids);
    auto inverse = build_inverse_token_map(token_map);
    auto token_view_map = build_token_view_map(inverse);

    uint64_t valid = 0;
    uint64_t invalid = 0;
    auto records = parse_csv_fixture(paths.integration / "telemetry.csv",
                                     token_view_map, 4, valid, invalid);

    BatchConfig config;
    config.strategy = BatchStrategy::COUNT;
    config.count_threshold = 2;
    config.write_audit_files = false;

    BatchAccumulator accumulator(config, inverse);
    std::vector<std::vector<ParsedRecord>> batches;

    for (size_t i = 0; i < records.size(); ++i) {
        const bool timestamp_boundary =
            (i + 1 == records.size()) ||
            (records[i + 1].timestamp != records[i].timestamp);

        accumulator.add_record(records[i]);

        if (timestamp_boundary && accumulator.should_flush()) {
            batches.push_back(accumulator.flush());
        }
    }

    auto final_batch = accumulator.flush_remaining();
    if (!final_batch.empty()) {
        batches.push_back(std::move(final_batch));
    }

    require_equal(batches.size(), static_cast<size_t>(5),
                  "Batch accumulator should flush once per timestamp here");

    for (size_t i = 0; i < batches.size(); ++i) {
        require_equal(batches[i].size(), static_cast<size_t>(4),
                      "Batch accumulator split a timestamp group");
        const std::string timestamp = batches[i][0].timestamp;
        for (const auto& record : batches[i]) {
            require_equal(record.timestamp, timestamp,
                          "Batch contains mixed timestamps");
        }
    }
}

void test_batch_pipeline_formatter_integration(const FixturePaths& paths) {
    const auto sensor_ids =
        load_sensors_from_yaml((paths.integration / "sensors.yaml").string());
    auto token_map = build_token_map_from_sensors(sensor_ids);
    auto inverse = build_inverse_token_map(token_map);
    auto token_view_map = build_token_view_map(inverse);

    uint64_t valid = 0;
    uint64_t invalid = 0;
    auto records = parse_csv_fixture(paths.integration / "telemetry.csv",
                                     token_view_map, 4, valid, invalid);

    auto rules = load_rules((paths.integration / "rules.json").string());
    auto prepared = prepare_rules(rules, token_map,
                                  static_cast<int>(sensor_ids.size()));

    const auto expected_valid =
        read_all(paths.integration / "expected_valid_data.csv");
    const auto expected_alarms =
        read_all(paths.integration / "expected_alarms.log");

    auto single_pass = run_pipeline_once(records, prepared, inverse);
    require_equal(single_pass.valid, expected_valid,
                  "Pipeline/formatter valid output did not match fixture");
    require_equal(single_pass.alarms, expected_alarms,
                  "Pipeline/formatter alarm output did not match fixture");
    require_equal(single_pass.timestamps, static_cast<uint64_t>(5),
                  "Pipeline timestamp count mismatch");
    require_equal(single_pass.nominal, static_cast<uint64_t>(3),
                  "Pipeline nominal count mismatch");
    require_equal(single_pass.anomalies, static_cast<uint64_t>(2),
                  "Pipeline anomaly count mismatch");

    for (size_t batch_size : {1u, 2u, 3u, 4u, 5u, 7u, 20u, 1000u}) {
        auto batched = run_pipeline_batched(records, prepared, inverse,
                                            batch_size);
        require_same_snapshot(batched, single_pass,
                              "Batch-size metamorphic relation failed");
    }
}

void test_faulty_csv_integration(const FixturePaths& paths) {
    const auto sensor_ids =
        load_sensors_from_yaml((paths.integration / "sensors.yaml").string());
    auto token_map = build_token_map_from_sensors(sensor_ids);
    auto inverse = build_inverse_token_map(token_map);
    auto token_view_map = build_token_view_map(inverse);

    uint64_t valid = 0;
    uint64_t invalid = 0;
    auto records = parse_csv_fixture(paths.faults / "telemetry_faults.csv",
                                     token_view_map, 3, valid, invalid);

    require_equal(valid, static_cast<uint64_t>(2),
                  "Fault fixture valid-row count mismatch");
    require_equal(invalid, static_cast<uint64_t>(5),
                  "Fault fixture invalid-row count mismatch");
    require_equal(records.size(), static_cast<size_t>(2),
                  "Fault fixture parsed-record size mismatch");
    require_equal(records[0].sensor_token, 0,
                  "Known sensor in fault fixture was not mapped");
    require_equal(records[1].sensor_token, -1,
                  "Unknown sensor should be retained with token -1");

    std::vector<RuleDefinition> no_rules;
    auto prepared = prepare_rules(no_rules, token_map,
                                  static_cast<int>(sensor_ids.size()));
    auto snapshot = run_pipeline_once(records, prepared, inverse);

    require_equal(snapshot.valid,
                  std::string("2026-01-01T00:00:00Z;NOMINAL;"
                              "S-A:10.0|UNKNOWN:12.0\n"),
                  "Unknown-sensor output formatting mismatch");
    require_true(snapshot.alarms.empty(),
                 "No-rule fault fixture should not produce alarms");
}

FixturePaths parse_args(int argc, char* argv[]) {
    fs::path root = (argc > 1) ? fs::path(argv[1]) : fs::path("test/fixtures");
    return {root, root / "integration", root / "faults"};
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        const auto paths = parse_args(argc, argv);

        test_yaml_to_csv_token_integration(paths);
        test_rules_to_prepared_rules_integration(paths);
        test_csv_to_batch_accumulator_integration(paths);
        test_batch_pipeline_formatter_integration(paths);
        test_faulty_csv_integration(paths);

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[component_integration_tests] " << e.what() << "\n";
        return 1;
    }
}
