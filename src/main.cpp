/**
 * @file main.cpp
 * @brief AstraLog-HPC — Entry point and orchestration.
 *
 * This file is a thin orchestrator that:
 *   1. Parses command-line arguments (including batch configuration)
 *   2. Loads configuration (sensors YAML, rules JSON)
 *   3. Memory-maps the CSV telemetry file
 *   4. Feeds valid records through the BatchAccumulator
 *   5. Delegates each batch to the modular processing pipeline
 *   6. Writes output files (valid_data.csv, alarms.log, batch audit files)
 *
 * All heavy lifting is in the header modules:
 *   - csv_parser.hpp:            Parallel CSV parsing with mmap
 *   - yaml_parser.hpp:           Sensor configuration loading
 *   - rules_engine.hpp:          Rule loading and evaluation functions
 *   - batch_accumulator.hpp:     Count/Time batch accumulation strategy
 *   - timestamp_processor.hpp:   5-phase parallel processing pipeline
 *   - output_formatter.hpp:      Spec-compliant output formatting
 *
 * Build:
 *   mkdir build && cd build && cmake .. -DCMAKE_BUILD_TYPE=Release && make
 *
 * Run:
 *   ./build/bin/astralog_processing --csv input/telemetry/export_sat_alpha_small.csv
 */

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <filesystem>
#include <chrono>

#include <omp.h>

// AstraLog-HPC modules
#include "types.hpp"
#include "csv_parser.hpp"
#include "yaml_parser.hpp"
#include "rules_engine.hpp"
#include "output_formatter.hpp"
#include "batch_accumulator.hpp"
#include "timestamp_processor.hpp"

namespace fs = std::filesystem;

// ===========================================================================
// Command-line argument utilities
// ===========================================================================

namespace {

std::string get_arg(int argc, char* argv[], const std::string& name,
                    const std::string& default_val = "") {
    for (int i = 1; i < argc - 1; ++i) {
        if (std::string(argv[i]) == name)
            return std::string(argv[i + 1]);
    }
    return default_val;
}

int get_int_arg(int argc, char* argv[], const std::string& name,
                int default_val) {
    std::string val = get_arg(argc, argv, name);
    if (val.empty()) return default_val;
    try { return std::stoi(val); }
    catch (...) { return default_val; }
}

double get_double_arg(int argc, char* argv[], const std::string& name,
                      double default_val) {
    std::string val = get_arg(argc, argv, name);
    if (val.empty()) return default_val;
    try { return std::stod(val); }
    catch (...) { return default_val; }
}

bool has_flag(int argc, char* argv[], const std::string& name) {
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == name) return true;
    }
    return false;
}

void print_usage(const char* prog) {
    std::cerr
        << "Usage: " << prog << " [options]\n"
        << "\n"
        << "Options:\n"
        << "  --csv <path>              Path to telemetry CSV file (required)\n"
        << "  --rules <path>            Path to rules JSON file "
           "(default: input/rules_SAT_ALPHA.json)\n"
        << "  --sensors <path>          Path to sensors YAML file "
           "(default: input/sensors_SAT_ALPHA.yaml)\n"
        << "  --output-dir <dir>        Output directory (default: output/)\n"
        << "  --threads <n>             Number of OpenMP threads "
           "(default: OMP_NUM_THREADS or all)\n"
        << "\n"
        << "Batch Accumulator Options:\n"
        << "  --batch-strategy <s>      Batch strategy: 'count' or 'time' "
           "(default: count)\n"
        << "  --batch-size <n>          Batch size for count strategy "
           "(default: 1000)\n"
        << "  --batch-interval <ms>     Batch interval for time strategy in ms "
           "(default: 5000)\n"
        << "\n"
        << "  --help                    Show this help message\n";
}

} // anonymous namespace

// ===========================================================================
// Main
// ===========================================================================

int main(int argc, char* argv[]) {
    auto wall_start = std::chrono::high_resolution_clock::now();

    // ── Help / Usage ────────────────────────────────────────────────────
    if (has_flag(argc, argv, "--help") || argc < 2) {
        print_usage(argv[0]);
        return 0;
    }

    // ── Parse arguments ─────────────────────────────────────────────────
    std::string csv_path     = get_arg(argc, argv, "--csv");
    std::string rules_path   = get_arg(argc, argv, "--rules",
                                       "input/rules_SAT_ALPHA.json");
    std::string sensors_path = get_arg(argc, argv, "--sensors",
                                       "input/sensors_SAT_ALPHA.yaml");
    std::string output_dir   = get_arg(argc, argv, "--output-dir", "output/");
    int         num_threads  = get_int_arg(argc, argv, "--threads", 0);

    // Batch accumulator arguments
    std::string batch_strategy_str = get_arg(argc, argv, "--batch-strategy",
                                             "count");
    int    batch_size     = get_int_arg(argc, argv, "--batch-size", 1000);
    double batch_interval = get_double_arg(argc, argv, "--batch-interval",
                                           5000.0);

    if (csv_path.empty()) {
        std::cerr << "[main] ERROR: --csv is required\n";
        print_usage(argv[0]);
        return 1;
    }

    // Build batch configuration
    astralog::BatchConfig batch_config;
    try {
        batch_config.strategy         = astralog::string_to_strategy(
                                            batch_strategy_str);
        batch_config.count_threshold  = static_cast<size_t>(batch_size);
        batch_config.time_threshold_ms = batch_interval;
        batch_config.batch_output_dir = (fs::path(output_dir) / "batches").string();
    } catch (const std::exception& e) {
        std::cerr << "[main] ERROR: invalid batch strategy: "
                  << e.what() << "\n";
        return 1;
    }

    // ── Configure OpenMP ────────────────────────────────────────────────
    if (num_threads > 0) {
        omp_set_num_threads(num_threads);
    } else {
        num_threads = omp_get_max_threads();
    }

    std::cerr
        << "=============================================================\n"
        << "  AstraLog-HPC  —  Single-Node OpenMP Engine\n"
        << "=============================================================\n"
        << "  CSV file:         " << csv_path     << "\n"
        << "  Rules file:       " << rules_path   << "\n"
        << "  Sensors file:     " << sensors_path << "\n"
        << "  Output dir:       " << output_dir   << "\n"
        << "  OMP threads:      " << omp_get_max_threads() << "\n"
        << "  Batch strategy:   "
            << astralog::strategy_to_string(batch_config.strategy) << "\n"
        << "  Batch threshold:  ";

    if (batch_config.strategy == astralog::BatchStrategy::COUNT) {
        std::cerr << batch_config.count_threshold << " records\n";
    } else {
        std::cerr << batch_config.time_threshold_ms << " ms\n";
    }

    std::cerr
        << "=============================================================\n";

    // ── 1. Load sensors from YAML ───────────────────────────────────────
    std::unordered_map<std::string, int> token_map;
    int total_sensors = 0;
    try {
        auto sensor_ids = astralog::load_sensors_from_yaml(sensors_path);
        for (const auto& sid : sensor_ids) {
            if (token_map.find(sid) == token_map.end()) {
                token_map[sid] = static_cast<int>(token_map.size());
            }
        }
        total_sensors = static_cast<int>(token_map.size());
        std::cerr << "[sensors] " << total_sensors
                  << " unique sensor(s) loaded from YAML\n";
    } catch (const std::exception& e) {
        std::cerr << "[sensors] FATAL: " << e.what() << "\n";
        return 1;
    }

    // ── 2. Load rules from JSON ─────────────────────────────────────────
    std::vector<astralog::RuleDefinition> rules;
    try {
        rules = astralog::load_rules(rules_path);
        std::cerr << "[rules] Loaded " << rules.size()
                  << " rule(s) from " << rules_path << "\n";
    } catch (const std::exception& e) {
        std::cerr << "[rules] FATAL: " << e.what() << "\n";
        return 1;
    }

    // ── 3. Memory-map the CSV file ──────────────────────────────────────
    astralog::MappedFile csv_file;
    if (!csv_file.open(csv_path.c_str())) {
        std::cerr << "[main] FATAL: failed to memory-map CSV file\n";
        return 1;
    }

    std::cerr << "[mmap] Mapped " << csv_file.size << " bytes ("
              << (csv_file.size / (1024.0 * 1024.0)) << " MB)\n";

    // ── 4. Build line offset index ──────────────────────────────────────
    auto scan_start = std::chrono::high_resolution_clock::now();
    auto line_offsets = astralog::build_line_offsets(csv_file.data,
                                                     csv_file.size);
    auto scan_end = std::chrono::high_resolution_clock::now();
    double scan_ms = std::chrono::duration<double, std::milli>(
        scan_end - scan_start).count();

    std::cerr << "[scan] Indexed " << line_offsets.size()
              << " data lines in " << scan_ms << " ms\n";

    // ── 5. Parallel CSV parsing ─────────────────────────────────────────
    auto parse_start = std::chrono::high_resolution_clock::now();
    uint64_t total_valid = 0, total_invalid = 0;
    auto records = astralog::parse_csv_parallel(csv_file, line_offsets,
                                                 token_map,
                                                 total_valid, total_invalid);
    auto parse_end = std::chrono::high_resolution_clock::now();
    double parse_ms = std::chrono::duration<double, std::milli>(
        parse_end - parse_start).count();

    std::cerr << "[parse] Parsed " << total_valid << " valid, "
              << total_invalid << " invalid rows in " << parse_ms << " ms\n";

    // ── 6. Create output directory and open output files ────────────────
    try {
        fs::create_directories(output_dir);
    } catch (const std::exception& e) {
        std::cerr << "[main] ERROR: failed to create output dir: "
                  << e.what() << "\n";
        return 1;
    }

    std::string valid_path  = (fs::path(output_dir) / "valid_data.csv").string();
    std::string alarms_path = (fs::path(output_dir) / "alarms.log").string();

    // Open output files in truncate mode (will be appended by each batch)
    std::ofstream valid_file(valid_path, std::ios::out | std::ios::trunc);
    std::ofstream alarms_file(alarms_path, std::ios::out | std::ios::trunc);

    if (!valid_file.is_open() || !alarms_file.is_open()) {
        std::cerr << "[main] FATAL: cannot open output files\n";
        return 1;
    }

    // ── 7. Batch Accumulation + Pipeline Processing ─────────────────────
    auto batch_start = std::chrono::high_resolution_clock::now();

    astralog::BatchAccumulator accumulator(batch_config);
    astralog::PipelineState    pipeline_state;

    // Aggregate statistics across all batches
    uint64_t total_timestamps = 0;
    uint64_t total_anomalies  = 0;
    uint64_t total_nominal    = 0;
    double   total_eval_ms    = 0.0;
    double   total_pipeline_ms = 0.0;

    /**
     * @brief Process a single batch through the rule evaluation pipeline
     *        and append results to the output files.
     *
     * Lambda captures output file streams and pipeline state by reference
     * so that sensor state persists across batches.
     */
    auto process_batch = [&](std::vector<astralog::ParsedRecord>& batch,
                             int batch_num) {
        auto batch_t0 = std::chrono::high_resolution_clock::now();

        auto result = astralog::process_pipeline(
            batch, rules, token_map, total_sensors, pipeline_state);

        auto batch_t1 = std::chrono::high_resolution_clock::now();
        double batch_ms = std::chrono::duration<double, std::milli>(
            batch_t1 - batch_t0).count();

        // Append output to files (sequential write preserves ordering)
        for (size_t g = 0; g < result.valid_lines.size(); ++g) {
            if (!result.valid_lines[g].empty())
                valid_file << result.valid_lines[g];
            if (!result.alarm_lines[g].empty())
                alarms_file << result.alarm_lines[g];
        }

        // Accumulate statistics
        total_timestamps  += result.num_timestamps;
        total_anomalies   += result.num_anomalies;
        total_nominal     += result.num_nominal;
        total_eval_ms     += result.eval_ms;
        total_pipeline_ms += result.total_ms;

        // Per-batch progress log
        std::cerr << "[batch] Batch "
                  << std::setfill('0') << std::setw(3) << batch_num
                  << ": " << batch.size() << " records → "
                  << result.num_nominal << " nominal, "
                  << result.num_anomalies << " anomalous ("
                  << batch_ms << " ms)\n";
    };

    // Feed all valid records through the batch accumulator
    for (auto& record : records) {
        accumulator.add_record(std::move(record));

        if (accumulator.should_flush()) {
            auto batch = accumulator.flush();
            process_batch(batch, accumulator.batch_count());
        }
    }

    // Flush any remaining records (final partial batch)
    auto final_batch = accumulator.flush_remaining();
    if (!final_batch.empty()) {
        process_batch(final_batch, accumulator.batch_count());
    }

    auto batch_end = std::chrono::high_resolution_clock::now();
    double batch_total_ms = std::chrono::duration<double, std::milli>(
        batch_end - batch_start).count();

    // Close output files
    valid_file.close();
    alarms_file.close();

    // ── 8. Summary ──────────────────────────────────────────────────────
    auto wall_end = std::chrono::high_resolution_clock::now();
    double total_ms = std::chrono::duration<double, std::milli>(
        wall_end - wall_start).count();

    double throughput_mrows = (total_valid + total_invalid) /
                              (total_ms / 1000.0) / 1e6;
    double data_bw = csv_file.size / (total_ms / 1000.0) / (1024.0 * 1024.0);

    std::cerr
        << "\n"
        << "=============================================================\n"
        << "  AstraLog-HPC  —  EXECUTION COMPLETE\n"
        << "=============================================================\n"
        << "  Total lines processed: "
            << (total_valid + total_invalid) << "\n"
        << "  Valid rows:            " << total_valid << "\n"
        << "  Invalid rows dropped:  " << total_invalid << "\n"
        << "-------------------------------------------------------------\n"
        << "  Batch strategy:        "
            << astralog::strategy_to_string(batch_config.strategy) << "\n"
        << "  Batches processed:     " << accumulator.batch_count() << "\n"
        << "  Batch audit files:     " << batch_config.batch_output_dir << "\n"
        << "-------------------------------------------------------------\n"
        << "  Unique timestamps:     " << total_timestamps << "\n"
        << "  Nominal timestamps:    " << total_nominal << "\n"
        << "  Anomalous timestamps:  " << total_anomalies << "\n"
        << "-------------------------------------------------------------\n"
        << "  Line scan time:        " << scan_ms << " ms\n"
        << "  CSV parse time:        " << parse_ms << " ms\n"
        << "  Batch + Pipeline time: " << batch_total_ms << " ms\n"
        << "    Rule evaluation:     " << total_eval_ms << " ms\n"
        << "    Pipeline total:      " << total_pipeline_ms << " ms\n"
        << "  Total wall time:       " << total_ms << " ms\n"
        << "  Throughput:            " << throughput_mrows << " Mrows/s\n"
        << "  Data bandwidth:        " << data_bw << " MB/s\n"
        << "-------------------------------------------------------------\n"
        << "  Output files:\n"
        << "    " << valid_path  << "\n"
        << "    " << alarms_path << "\n"
        << "=============================================================\n";

    return 0;
}
