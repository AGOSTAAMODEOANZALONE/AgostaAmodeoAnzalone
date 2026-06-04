/**
 * @file batch_accumulator.hpp
 * @brief Batch Accumulator component for AstraLog-HPC.
 *
 * Sits between CSV validation and the rule evaluation pipeline.
 * Accumulates valid ParsedRecord objects and flushes them in batches
 * using one of two strategies:
 *
 *   - COUNT:  Flush every N valid records
 *   - TIME:   Flush every N milliseconds of wall-clock time
 *
 * Each flush writes a timestamped audit file to disk and returns the
 * batch of records for processing by the rule engine. This component
 * matches the Batch Accumulator described in the Phase 1 design document
 * (Section 3.1.1, FR.3, FR.7, Use Case 3).
 *
 * ## Design Notes
 *
 *   - The accumulator operates in memory; the .txt audit file is written
 *     as a side-effect of flush() for traceability and debugging.
 *   - Time-based strategy uses wall-clock elapsed time from the moment
 *     the current batch started accumulating. On small files the entire
 *     file may fit in a single batch.
 *   - Thread safety is NOT required: the accumulator is called from the
 *     sequential main loop (between parallel CSV parse and parallel
 *     rule evaluation).
 */

#ifndef ASTRALOG_BATCH_ACCUMULATOR_HPP
#define ASTRALOG_BATCH_ACCUMULATOR_HPP

#include <cstddef>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <filesystem>

#include "types.hpp"

namespace astralog {

// ===========================================================================
// Batch strategy enumeration
// ===========================================================================

/// Batch flushing strategy
enum class BatchStrategy : uint8_t {
    COUNT = 0,   ///< Flush every N valid records
    TIME  = 1    ///< Flush every N milliseconds of wall-clock time
};

inline const char* strategy_to_string(BatchStrategy s) noexcept {
    switch (s) {
        case BatchStrategy::COUNT: return "COUNT";
        case BatchStrategy::TIME:  return "TIME";
        default:                   return "UNKNOWN";
    }
}

inline BatchStrategy string_to_strategy(const std::string& s) {
    if (s == "count" || s == "COUNT") return BatchStrategy::COUNT;
    if (s == "time"  || s == "TIME")  return BatchStrategy::TIME;
    throw std::invalid_argument("Unknown batch strategy: " + s);
}

// ===========================================================================
// Batch configuration
// ===========================================================================

/**
 * @struct BatchConfig
 * @brief Configuration for the batch accumulator.
 */
struct BatchConfig {
    BatchStrategy strategy;           ///< COUNT or TIME
    size_t        count_threshold;    ///< For COUNT: flush every N valid records
    double        time_threshold_ms;  ///< For TIME: flush every N ms
    std::string   batch_output_dir;   ///< Where to write .txt batch audit files

    BatchConfig()
        : strategy(BatchStrategy::COUNT)
        , count_threshold(1000)
        , time_threshold_ms(5000.0)
        , batch_output_dir("output/batches")
    {}
};

// ===========================================================================
// Batch Accumulator
// ===========================================================================

/**
 * @class BatchAccumulator
 * @brief Accumulates valid records and flushes them in configurable batches.
 *
 * Usage:
 *   BatchAccumulator acc(config);
 *   for (auto& record : all_valid_records) {
 *       acc.add_record(std::move(record));
 *       if (acc.should_flush()) {
 *           auto batch = acc.flush();
 *           // process batch through rule engine...
 *       }
 *   }
 *   auto final_batch = acc.flush_remaining();
 *   if (!final_batch.empty()) {
 *       // process final partial batch...
 *   }
 */
class BatchAccumulator {
public:
    explicit BatchAccumulator(const BatchConfig& config)
        : config_(config)
        , batch_number_(0)
        , batch_start_time_(std::chrono::steady_clock::now())
    {
        buffer_.reserve(config.count_threshold > 0 ? config.count_threshold : 1000);
    }

    /**
     * @brief Push one valid record into the accumulator buffer.
     */
    void add_record(ParsedRecord rec) {
        buffer_.push_back(std::move(rec));
    }

    /**
     * @brief Check if the flush threshold has been met.
     *
     * For COUNT: true when buffer size >= count_threshold.
     * For TIME:  true when wall-clock elapsed >= time_threshold_ms.
     */
    bool should_flush() const {
        if (buffer_.empty()) return false;

        switch (config_.strategy) {
            case BatchStrategy::COUNT:
                return buffer_.size() >= config_.count_threshold;

            case BatchStrategy::TIME: {
                auto now = std::chrono::steady_clock::now();
                double elapsed_ms = std::chrono::duration<double, std::milli>(
                    now - batch_start_time_).count();
                return elapsed_ms >= config_.time_threshold_ms;
            }

            default:
                return false;
        }
    }

    /**
     * @brief Flush the current buffer: return records and write audit file.
     *
     * Resets the internal buffer and timer for the next batch.
     *
     * @return Vector of ParsedRecord that were in the buffer
     */
    std::vector<ParsedRecord> flush() {
        ++batch_number_;

        std::vector<ParsedRecord> batch;
        batch.swap(buffer_);

        // Write audit file
        write_batch_file(batch);

        // Reset timer for next batch (TIME strategy)
        batch_start_time_ = std::chrono::steady_clock::now();

        // Re-reserve buffer for next batch
        buffer_.reserve(config_.count_threshold > 0 ? config_.count_threshold : 1000);

        return batch;
    }

    /**
     * @brief Flush any remaining records (final partial batch at end-of-file).
     *
     * @return Vector of remaining ParsedRecord (may be empty)
     */
    std::vector<ParsedRecord> flush_remaining() {
        if (buffer_.empty()) return {};
        return flush();
    }

    /**
     * @brief How many batches have been flushed so far.
     */
    int batch_count() const noexcept {
        return batch_number_;
    }

    /**
     * @brief How many records are currently buffered (not yet flushed).
     */
    size_t buffered_count() const noexcept {
        return buffer_.size();
    }

    /**
     * @brief Get the batch configuration.
     */
    const BatchConfig& config() const noexcept {
        return config_;
    }

private:
    BatchConfig                config_;
    std::vector<ParsedRecord>  buffer_;
    int                        batch_number_;
    std::chrono::steady_clock::time_point batch_start_time_;

    /**
     * @brief Write a batch audit file to disk.
     *
     * File format:
     *   # Batch NNN | Strategy: COUNT | Size: XXXX records
     *   # Flushed at: YYYY-MM-DDTHH:MM:SSZ
     *   timestamp,sensor_id,value
     *   2025-11-15T12:00:00Z,TEMP-01,25.5
     *   ...
     */
    void write_batch_file(const std::vector<ParsedRecord>& batch) {
        namespace fs = std::filesystem;

        try {
            fs::create_directories(config_.batch_output_dir);
        } catch (const std::exception& e) {
            std::cerr << "[batch] WARNING: cannot create batch output dir: "
                      << e.what() << "\n";
            return;
        }

        // Build filename: batch_001_20251115_120000.txt
        auto now = std::chrono::system_clock::now();
        auto now_t = std::chrono::system_clock::to_time_t(now);
        std::tm tm_buf{};
#if defined(_WIN32)
        gmtime_s(&tm_buf, &now_t);
#else
        gmtime_r(&now_t, &tm_buf);
#endif

        std::ostringstream fname;
        fname << "batch_"
              << std::setfill('0') << std::setw(3) << batch_number_
              << "_"
              << std::setfill('0') << std::setw(4) << (tm_buf.tm_year + 1900)
              << std::setfill('0') << std::setw(2) << (tm_buf.tm_mon + 1)
              << std::setfill('0') << std::setw(2) << tm_buf.tm_mday
              << "_"
              << std::setfill('0') << std::setw(2) << tm_buf.tm_hour
              << std::setfill('0') << std::setw(2) << tm_buf.tm_min
              << std::setfill('0') << std::setw(2) << tm_buf.tm_sec
              << ".txt";

        std::string filepath = (fs::path(config_.batch_output_dir) / fname.str()).string();

        std::ofstream ofs(filepath, std::ios::out | std::ios::trunc);
        if (!ofs.is_open()) {
            std::cerr << "[batch] WARNING: cannot open batch file: "
                      << filepath << "\n";
            return;
        }

        // Header comments
        ofs << "# Batch " << std::setfill('0') << std::setw(3) << batch_number_
            << " | Strategy: " << strategy_to_string(config_.strategy)
            << " | Size: " << batch.size() << " records\n";

        ofs << "# Flushed at: "
            << std::setfill('0') << std::setw(4) << (tm_buf.tm_year + 1900) << "-"
            << std::setfill('0') << std::setw(2) << (tm_buf.tm_mon + 1) << "-"
            << std::setfill('0') << std::setw(2) << tm_buf.tm_mday << "T"
            << std::setfill('0') << std::setw(2) << tm_buf.tm_hour << ":"
            << std::setfill('0') << std::setw(2) << tm_buf.tm_min << ":"
            << std::setfill('0') << std::setw(2) << tm_buf.tm_sec << "Z\n";

        // CSV header
        ofs << "timestamp,sensor_id,value\n";

        // Data rows
        for (const auto& rec : batch) {
            ofs << rec.timestamp << "," << rec.sensor_id << "," << rec.value << "\n";
        }

        ofs.close();
    }
};

} // namespace astralog

#endif // ASTRALOG_BATCH_ACCUMULATOR_HPP
