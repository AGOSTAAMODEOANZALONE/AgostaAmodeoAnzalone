/**
 * @file csv_parser.hpp
 * @brief Memory-mapped CSV parser with parallel line parsing.
 *
 * Pipeline:
 *   1. Memory-map the CSV file using POSIX mmap(2)
 *   2. Sequential pre-scan to build line offset index (O(1) random access)
 *   3. OpenMP parallel-for to parse and validate lines concurrently
 *   4. Merge thread-local results into a single ordered vector
 *
 * Validation (ESA-compliant):
 *   - Malformed lines: not exactly 4 comma-separated fields
 *   - Schema errors: empty fields, "ERR" or "CORRUPT" markers
 *   - Type errors: non-numeric value field
 */

#ifndef ASTRALOG_CSV_PARSER_HPP
#define ASTRALOG_CSV_PARSER_HPP

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <charconv>

// POSIX mmap
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

// OpenMP
#include <omp.h>

#include "types.hpp"

// Portability: MAP_POPULATE is Linux-specific (prefaults page tables)
#ifndef MAP_POPULATE
#define MAP_POPULATE 0
#endif

namespace astralog {

struct TokenViewEntry {
    std::string_view sensor_id;
    int token;
};

using TokenViewMap = std::vector<TokenViewEntry>;

// ===========================================================================
// Inverse token map utility
// ===========================================================================

/**
 * @brief Build an inverse token map: sensor_token (int) → sensor_id (string).
 *
 * Given the forward map (sensor_id → token), returns a vector where
 * index i contains the sensor name for token i.
 */
inline std::vector<std::string> build_inverse_token_map(
    const std::unordered_map<std::string, int>& token_map)
{
    std::vector<std::string> inverse(token_map.size());
    for (const auto& [name, idx] : token_map) {
        if (idx >= 0 && static_cast<size_t>(idx) < inverse.size())
            inverse[idx] = name;
    }
    return inverse;
}

/**
 * @brief Build a string_view token lookup table backed by inverse_token_map storage.
 *
 * The returned keys are non-owning views into inverse_token_map, so the
 * inverse_token_map vector must outlive the returned lookup table.
 */
inline TokenViewMap build_token_view_map(
    const std::vector<std::string>& inverse_token_map)
{
    TokenViewMap token_view_map;
    token_view_map.reserve(inverse_token_map.size());

    for (size_t i = 0; i < inverse_token_map.size(); ++i) {
        if (!inverse_token_map[i].empty()) {
            token_view_map.push_back({
                std::string_view(inverse_token_map[i].data(),
                                 inverse_token_map[i].size()),
                static_cast<int>(i)
            });
        }
    }

    return token_view_map;
}

inline int lookup_sensor_token(const TokenViewMap& token_map,
                               std::string_view sensor_id) noexcept
{
    for (const auto& entry : token_map) {
        if (entry.sensor_id == sensor_id) {
            return entry.token;
        }
    }
    return -1;
}

// ===========================================================================
// Memory-mapped file
// ===========================================================================

/**
 * @struct MappedFile
 * @brief RAII wrapper for a read-only memory-mapped file.
 */
struct MappedFile {
    const char* data = nullptr;
    size_t      size = 0;
    int         fd   = -1;

    ~MappedFile() {
        if (data && data != MAP_FAILED)
            munmap(const_cast<char*>(data), size);
        if (fd >= 0)
            close(fd);
    }

    /// Open and memory-map a file. Returns false on failure.
    bool open(const char* path) {
        fd = ::open(path, O_RDONLY);
        if (fd < 0) {
            std::cerr << "[mmap] ERROR: cannot open file: " << path << "\n";
            return false;
        }

        struct stat st;
        if (fstat(fd, &st) < 0) {
            std::cerr << "[mmap] ERROR: cannot stat file: " << path << "\n";
            close(fd); fd = -1;
            return false;
        }

        size = static_cast<size_t>(st.st_size);
        data = static_cast<const char*>(
            mmap(nullptr, size, PROT_READ, MAP_PRIVATE | MAP_POPULATE, fd, 0));

        if (data == MAP_FAILED) {
            std::cerr << "[mmap] ERROR: mmap failed for: " << path << "\n";
            data = nullptr;
            close(fd); fd = -1;
            return false;
        }

        // Advise the kernel we will read sequentially
        madvise(const_cast<char*>(data), size, MADV_SEQUENTIAL);
        return true;
    }
};

// ===========================================================================
// Line offset index
// ===========================================================================

/**
 * @brief Build a vector of byte offsets for each data line (skipping header).
 *
 * Enables O(1) random access to any CSV data line by index.
 */
inline std::vector<size_t> build_line_offsets(const char* data, size_t size) {
    std::vector<size_t> offsets;
    offsets.reserve(size / 50);  // ~50 bytes per line estimate

    // Skip header line
    size_t pos = 0;
    while (pos < size && data[pos] != '\n') ++pos;
    ++pos;

    // Record start of each data line
    while (pos < size) {
        offsets.push_back(pos);
        while (pos < size && data[pos] != '\n') ++pos;
        ++pos;
    }

    return offsets;
}

/// Extract a line as string_view from mmap data at the given offset.
inline std::string_view get_line(const char* data, size_t size,
                                 size_t offset) noexcept {
    size_t end = offset;
    while (end < size && data[end] != '\n') ++end;
    return std::string_view(data + offset, end - offset);
}

// ===========================================================================
// Zero-copy CSV tokenization and validation
// ===========================================================================

/**
 * @brief Tokenize a CSV line into exactly 4 comma-separated fields.
 * @return true if exactly 4 fields found (no more, no fewer)
 */
inline bool tokenize_csv_line(std::string_view line,
                              std::string_view& f0, std::string_view& f1,
                              std::string_view& f2, std::string_view& f3) noexcept {
    // Strip trailing \r or \n
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
        line.remove_suffix(1);
    if (line.empty()) return false;

    size_t c1 = line.find(',');
    if (c1 == std::string_view::npos) return false;
    size_t c2 = line.find(',', c1 + 1);
    if (c2 == std::string_view::npos) return false;
    size_t c3 = line.find(',', c2 + 1);
    if (c3 == std::string_view::npos) return false;
    // Reject if there is a 5th field
    if (line.find(',', c3 + 1) != std::string_view::npos) return false;

    f0 = line.substr(0, c1);
    f1 = line.substr(c1 + 1, c2 - c1 - 1);
    f2 = line.substr(c2 + 1, c3 - c2 - 1);
    f3 = line.substr(c3 + 1);
    return true;
}

/**
 * @brief ESA-compliant schema validation.
 *
 * Rejects: empty fields, "ERR" or "CORRUPT" markers in any field.
 */
inline bool validate_schema(std::string_view f0, std::string_view f1,
                            std::string_view f2, std::string_view f3) noexcept {
    if (f0.empty() || f1.empty() || f2.empty() || f3.empty()) return false;
    auto is_corrupt = [](std::string_view s) {
        return s == "ERR" || s == "CORRUPT";
    };
    return !is_corrupt(f0) && !is_corrupt(f1) && !is_corrupt(f2) && !is_corrupt(f3);
}

/**
 * @brief Parse a value field to double using std::from_chars (C++17).
 *        Non-allocating, exception-free.
 */
inline bool parse_value(std::string_view field, double& val) noexcept {
    auto result = std::from_chars(field.data(), field.data() + field.size(), val);
    return (result.ec == std::errc{} && result.ptr == field.data() + field.size());
}

// ===========================================================================
// Parallel CSV parsing
// ===========================================================================

/**
 * @brief Parse the entire CSV file in parallel, returning validated records
 *        in original CSV line order.
 *
 * Uses OpenMP static scheduling to ensure each thread processes a contiguous
 * block of lines. Concatenating thread-local results in thread order preserves
 * the original file ordering.
 *
 * @param csv_file    Memory-mapped CSV file
 * @param line_offsets Pre-computed line offset index
 * @param token_map   sensor_id → integer token mapping
 * @param[out] total_valid   Count of valid rows
 * @param[out] total_invalid Count of invalid/dropped rows
 * @return            Vector of ParsedRecord in original CSV order
 */
inline std::vector<ParsedRecord> parse_csv_parallel(
    const MappedFile& csv_file,
    const std::vector<size_t>& line_offsets,
    const TokenViewMap& token_map,
    uint64_t& total_valid,
    uint64_t& total_invalid)
{
    const size_t total_lines = line_offsets.size();
    const int num_threads = omp_get_max_threads();

    std::vector<std::vector<ParsedRecord>> thread_records(num_threads);
    uint64_t valid_count = 0, invalid_count = 0;

    #pragma omp parallel reduction(+:valid_count, invalid_count)
    {
        int tid = omp_get_thread_num();
        thread_records[tid].reserve(total_lines / num_threads + 1);

        #pragma omp for schedule(static)
        for (size_t i = 0; i < total_lines; ++i) {
            std::string_view line = get_line(csv_file.data, csv_file.size,
                                             line_offsets[i]);

            std::string_view f_ts, f_sensor, f_value, f_priority;
            if (!tokenize_csv_line(line, f_ts, f_sensor, f_value, f_priority)) {
                ++invalid_count;
                continue;
            }
            if (!validate_schema(f_ts, f_sensor, f_value, f_priority)) {
                ++invalid_count;
                continue;
            }
            double value;
            if (!parse_value(f_value, value)) {
                ++invalid_count;
                continue;
            }

            ++valid_count;

            ParsedRecord rec;
            rec.timestamp  = std::string(f_ts);
            rec.value      = value;

            // Tiny fixed lookup table: avoids allocating and avoids hash cost.
            rec.sensor_token = lookup_sensor_token(token_map, f_sensor);

            thread_records[tid].push_back(std::move(rec));
        }
    }

    total_valid   = valid_count;
    total_invalid = invalid_count;

    // Merge thread-local vectors in thread order → preserves CSV line order
    std::vector<ParsedRecord> all_records;
    size_t total_size = 0;
    for (int t = 0; t < num_threads; ++t) total_size += thread_records[t].size();
    all_records.reserve(total_size);

    for (int t = 0; t < num_threads; ++t) {
        for (auto& rec : thread_records[t]) {
            all_records.push_back(std::move(rec));
        }
    }

    return all_records;
}

/**
 * @brief Backward-compatible overload for callers that still own a string map.
 */
inline std::vector<ParsedRecord> parse_csv_parallel(
    const MappedFile& csv_file,
    const std::vector<size_t>& line_offsets,
    const std::unordered_map<std::string, int>& token_map,
    uint64_t& total_valid,
    uint64_t& total_invalid)
{
    auto inverse_token_map = build_inverse_token_map(token_map);
    auto token_view_map = build_token_view_map(inverse_token_map);
    return parse_csv_parallel(csv_file, line_offsets, token_view_map,
                              total_valid, total_invalid);
}

} // namespace astralog

#endif // ASTRALOG_CSV_PARSER_HPP
