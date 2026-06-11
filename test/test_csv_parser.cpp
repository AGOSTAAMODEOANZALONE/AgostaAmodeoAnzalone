/**
 * @file test_csv_parser.cpp
 * @brief Unit tests for csv_parser.hpp — tokenising, field validation,
 *        value parsing, line-offset building, and token-map helpers.
 */

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "csv_parser.hpp"

namespace {

using namespace astralog;

// ---------------------------------------------------------------------------
// build_inverse_token_map
// ---------------------------------------------------------------------------

TEST(CsvParserTest, BuildInverseTokenMap) {
    std::unordered_map<std::string, int> token_map = {
        {"TEMP-01", 0},
        {"PRES-01", 1},
    };
    auto inverse = build_inverse_token_map(token_map);
    ASSERT_EQ(inverse.size(), static_cast<size_t>(2));
    EXPECT_EQ(inverse[0], "TEMP-01");
    EXPECT_EQ(inverse[1], "PRES-01");
}

// ---------------------------------------------------------------------------
// build_token_view_map / lookup_sensor_token
// ---------------------------------------------------------------------------

TEST(CsvParserTest, BuildTokenViewMapEmpty) {
    std::vector<std::string> inverse;
    auto tvm = build_token_view_map(inverse);
    EXPECT_TRUE(tvm.empty());
    EXPECT_EQ(lookup_sensor_token(tvm, "anything"), -1);
}

TEST(CsvParserTest, BuildTokenViewMapSkipsEmpty) {
    // Inverse map with a gap (empty string at index 1)
    std::vector<std::string> inverse = {"S1", "", "S3"};
    auto tvm = build_token_view_map(inverse);
    EXPECT_EQ(lookup_sensor_token(tvm, "S1"), 0);
    EXPECT_EQ(lookup_sensor_token(tvm, "S3"), 2);
    EXPECT_EQ(lookup_sensor_token(tvm, ""),   -1);
}

// ---------------------------------------------------------------------------
// tokenize_csv_line
// ---------------------------------------------------------------------------

TEST(CsvParserTest, TokenizeCsvLineEmptyLine) {
    std::string_view f0, f1, f2, f3;
    EXPECT_FALSE(tokenize_csv_line("", f0, f1, f2, f3));
}

TEST(CsvParserTest, TokenizeCsvLineCrLfStripped) {
    std::string_view f0, f1, f2, f3;
    EXPECT_TRUE(tokenize_csv_line("2025-01-01T00:00:00Z,S1,10.0,HIGH\r\n",
                                  f0, f1, f2, f3));
    EXPECT_EQ(f3, "HIGH");
}

TEST(CsvParserTest, TokenizeFourFieldsExact) {
    std::string_view f0, f1, f2, f3;
    EXPECT_TRUE(tokenize_csv_line("A,B,C,D", f0, f1, f2, f3));
    EXPECT_EQ(f0, "A"); EXPECT_EQ(f1, "B");
    EXPECT_EQ(f2, "C"); EXPECT_EQ(f3, "D");
}

TEST(CsvParserTest, TokenizeTooFewFields) {
    std::string_view f0, f1, f2, f3;
    EXPECT_FALSE(tokenize_csv_line("A,B,C", f0, f1, f2, f3));
}

TEST(CsvParserTest, TokenizeTooManyFields) {
    std::string_view f0, f1, f2, f3;
    EXPECT_FALSE(tokenize_csv_line("A,B,C,D,E", f0, f1, f2, f3));
}

TEST(CsvParserTest, TokenizeEmptyFields) {
    std::string_view f0, f1, f2, f3;
    // Empty sensor_id field — tokenize succeeds, schema validation rejects
    EXPECT_TRUE(tokenize_csv_line("TS,,1.0,HIGH", f0, f1, f2, f3));
    EXPECT_EQ(f1, "");
}

// ---------------------------------------------------------------------------
// validate_schema
// ---------------------------------------------------------------------------

TEST(CsvParserTest, ValidateSchemaCorruptMarker) {
    EXPECT_FALSE(validate_schema("2025-01-01T00:00:00Z", "CORRUPT", "10.0", "HIGH"));
    EXPECT_FALSE(validate_schema("CORRUPT",              "S1",      "10.0", "HIGH"));
    EXPECT_FALSE(validate_schema("2025-01-01T00:00:00Z", "S1",      "CORRUPT", "HIGH"));
    EXPECT_FALSE(validate_schema("2025-01-01T00:00:00Z", "S1",      "10.0", "CORRUPT"));
}

TEST(CsvParserTest, ValidateSchemaERRMarker) {
    EXPECT_FALSE(validate_schema("TS", "S1", "ERR",  "HIGH"));
    EXPECT_FALSE(validate_schema("TS", "ERR", "1.0", "HIGH"));
    EXPECT_FALSE(validate_schema("ERR", "S1", "1.0", "HIGH"));
    EXPECT_FALSE(validate_schema("TS", "S1",  "1.0", "ERR"));
}

TEST(CsvParserTest, ValidateSchemaEmptyFields) {
    EXPECT_FALSE(validate_schema("",   "S1", "1.0", "HIGH"));
    EXPECT_FALSE(validate_schema("TS", "",   "1.0", "HIGH"));
    EXPECT_FALSE(validate_schema("TS", "S1", "",    "HIGH"));
    EXPECT_FALSE(validate_schema("TS", "S1", "1.0", ""));
}

TEST(CsvParserTest, ValidateSchemaValid) {
    EXPECT_TRUE(validate_schema("TS", "S1", "1.0", "HIGH"));
}

// ---------------------------------------------------------------------------
// parse_value
// ---------------------------------------------------------------------------

TEST(CsvParserTest, ParseValueScientificNotation) {
    double val = 0.0;
    EXPECT_TRUE(parse_value("3.14", val));
    EXPECT_DOUBLE_EQ(val, 3.14);
}

TEST(CsvParserTest, ParseValueTrailingCharsRejects) {
    double val = 0.0;
    EXPECT_FALSE(parse_value("12.0abc", val));
}

TEST(CsvParserTest, ParseValueEmptyRejects) {
    double val = 0.0;
    EXPECT_FALSE(parse_value("", val));
}

TEST(CsvParserTest, ParseValueNegative) {
    double val = 0.0;
    EXPECT_TRUE(parse_value("-99.5", val));
    EXPECT_DOUBLE_EQ(val, -99.5);
}

// ---------------------------------------------------------------------------
// build_line_offsets / get_line
// ---------------------------------------------------------------------------

TEST(CsvParserTest, BuildLineOffsets) {
    const char* csv =
        "timestamp,sensor_id,value,priority\n"
        "T1,S1,1.0,HIGH\n"
        "T2,S2,2.0,MEDIUM\n"
        "T3,S3,3.0,LOW\n";
    size_t sz = std::strlen(csv);

    auto offsets = build_line_offsets(csv, sz);
    ASSERT_EQ(offsets.size(), static_cast<size_t>(3));

    EXPECT_EQ(get_line(csv, sz, offsets[0]), "T1,S1,1.0,HIGH");
    EXPECT_EQ(get_line(csv, sz, offsets[1]), "T2,S2,2.0,MEDIUM");
    EXPECT_EQ(get_line(csv, sz, offsets[2]), "T3,S3,3.0,LOW");
}

TEST(CsvParserTest, BuildLineOffsetsNoData) {
    // Only a header — zero data lines
    const char* csv = "timestamp,sensor_id,value,priority\n";
    auto offsets = build_line_offsets(csv, std::strlen(csv));
    EXPECT_TRUE(offsets.empty());
}

TEST(CsvParserTest, GetLineAtEnd) {
    // Last line without a trailing newline
    const char* csv = "header\nline1\nlast_no_nl";
    size_t sz = std::strlen(csv);
    auto offsets = build_line_offsets(csv, sz);
    ASSERT_GE(offsets.size(), static_cast<size_t>(1));
    EXPECT_EQ(get_line(csv, sz, offsets.back()), "last_no_nl");
}

} // namespace
