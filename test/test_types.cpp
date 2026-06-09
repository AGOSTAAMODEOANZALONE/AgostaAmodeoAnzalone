/**
 * @file test_types.cpp
 * @brief Unit tests for types.hpp — enums, string converters, and data-structure
 *        helpers (Priority, RuleType, CompOp, RuleViolation, SensorState, etc.).
 *        No file I/O, no OpenMP, no pipeline logic.
 */

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

#include "types.hpp"

namespace {

using namespace astralog;

// ---------------------------------------------------------------------------
// String converters — error paths
// ---------------------------------------------------------------------------

TEST(TypesTest, StringToPriorityInvalidThrows) {
    EXPECT_THROW(string_to_priority("CRITICAL"), std::invalid_argument);
}

TEST(TypesTest, StringToRuleTypeInvalidThrows) {
    EXPECT_THROW(string_to_ruletype("unknown_type"), std::invalid_argument);
}

TEST(TypesTest, StringToCompOpInvalidThrows) {
    EXPECT_THROW(string_to_compop("??"), std::invalid_argument);
}

// ---------------------------------------------------------------------------
// String converters — valid / alias paths
// ---------------------------------------------------------------------------

TEST(TypesTest, StringToRuleTypeAliases) {
    EXPECT_EQ(string_to_ruletype("simple"),          RuleType::THRESHOLD);
    EXPECT_EQ(string_to_ruletype("threshold"),       RuleType::THRESHOLD);
    EXPECT_EQ(string_to_ruletype("step_difference"), RuleType::STEP_DIFF);
    EXPECT_EQ(string_to_ruletype("step_diff"),       RuleType::STEP_DIFF);
    EXPECT_EQ(string_to_ruletype("stateful"),        RuleType::STATEFUL);
    EXPECT_EQ(string_to_ruletype("correlation"),     RuleType::CORRELATION);
}

TEST(TypesTest, StringToPriorityLowercase) {
    EXPECT_EQ(string_to_priority("low"),    Priority::LOW);
    EXPECT_EQ(string_to_priority("medium"), Priority::MEDIUM);
    EXPECT_EQ(string_to_priority("high"),   Priority::HIGH);
}

// ---------------------------------------------------------------------------
// To-string helpers
// ---------------------------------------------------------------------------

TEST(TypesTest, PriorityToString) {
    EXPECT_STREQ(priority_to_string(Priority::LOW),    "LOW");
    EXPECT_STREQ(priority_to_string(Priority::MEDIUM), "MEDIUM");
    EXPECT_STREQ(priority_to_string(Priority::HIGH),   "HIGH");
}

TEST(TypesTest, RuleTypeToString) {
    EXPECT_STREQ(ruletype_to_string(RuleType::THRESHOLD),   "threshold");
    EXPECT_STREQ(ruletype_to_string(RuleType::STEP_DIFF),   "step_diff");
    EXPECT_STREQ(ruletype_to_string(RuleType::STATEFUL),    "stateful");
    EXPECT_STREQ(ruletype_to_string(RuleType::CORRELATION), "correlation");
}

// ---------------------------------------------------------------------------
// evaluate_comparison — all 6 operators
// ---------------------------------------------------------------------------

TEST(TypesTest, EvaluateComparisonAllOperators) {
    EXPECT_TRUE (evaluate_comparison(5.0, CompOp::GT, 4.0));
    EXPECT_FALSE(evaluate_comparison(4.0, CompOp::GT, 4.0));

    EXPECT_TRUE (evaluate_comparison(4.0, CompOp::GE, 4.0));
    EXPECT_TRUE (evaluate_comparison(5.0, CompOp::GE, 4.0));
    EXPECT_FALSE(evaluate_comparison(3.0, CompOp::GE, 4.0));

    EXPECT_TRUE (evaluate_comparison(3.0, CompOp::LT, 4.0));
    EXPECT_FALSE(evaluate_comparison(4.0, CompOp::LT, 4.0));

    EXPECT_TRUE (evaluate_comparison(4.0, CompOp::LE, 4.0));
    EXPECT_TRUE (evaluate_comparison(3.0, CompOp::LE, 4.0));
    EXPECT_FALSE(evaluate_comparison(5.0, CompOp::LE, 4.0));

    EXPECT_TRUE (evaluate_comparison(4.0, CompOp::EQ, 4.0));
    EXPECT_FALSE(evaluate_comparison(5.0, CompOp::EQ, 4.0));

    EXPECT_TRUE (evaluate_comparison(5.0, CompOp::NE, 4.0));
    EXPECT_FALSE(evaluate_comparison(4.0, CompOp::NE, 4.0));
}

// ---------------------------------------------------------------------------
// RuleViolation — empty / inline / extra-storage boundary
// ---------------------------------------------------------------------------

TEST(TypesTest, RuleViolationEmpty) {
    RuleViolation v;
    EXPECT_TRUE(v.empty());
    v.add(0, 1.0);
    EXPECT_FALSE(v.empty());
}

TEST(TypesTest, RuleViolationInlineBoundary) {
    RuleViolation v;
    // Fill exactly INLINE_CAPACITY = 4 entries
    for (int i = 0; i < 4; ++i)
        v.add(i, static_cast<double>(i));

    EXPECT_EQ(v.size(), static_cast<size_t>(4));
    EXPECT_TRUE(v.sensor_tokens_extra.empty());

    // 5th entry spills to extra storage
    v.add(4, 40.0);
    EXPECT_EQ(v.size(), static_cast<size_t>(5));
    EXPECT_EQ(v.sensor_tokens_extra.size(), static_cast<size_t>(1));

    // Accessors work for both inline and extra
    EXPECT_EQ(v.sensor_token_at(3), 3);
    EXPECT_EQ(v.sensor_token_at(4), 4);
    EXPECT_DOUBLE_EQ(v.value_at(4), 40.0);
}

// ---------------------------------------------------------------------------
// Default-constructed structs
// ---------------------------------------------------------------------------

TEST(TypesTest, SensorStateDefaultState) {
    SensorState s;
    EXPECT_DOUBLE_EQ(s.previous_value, 0.0);
    EXPECT_FALSE(s.has_previous);
    EXPECT_TRUE(s.consecutive_violations.empty());
}

TEST(TypesTest, RuleDefinitionDefaults) {
    RuleDefinition r;
    EXPECT_EQ(r.type,     RuleType::THRESHOLD);
    EXPECT_EQ(r.op,       CompOp::GT);
    EXPECT_EQ(r.priority, Priority::LOW);
    EXPECT_DOUBLE_EQ(r.threshold, 0.0);
    EXPECT_EQ(r.consecutive_measurements, 1);
    EXPECT_EQ(r.sensor_token_idx, -1);
    EXPECT_TRUE(r.rule_id.empty());
    EXPECT_TRUE(r.sensor_id.empty());
}

TEST(TypesTest, ParsedRecordDefaults) {
    ParsedRecord r;
    EXPECT_DOUBLE_EQ(r.value, 0.0);
    EXPECT_EQ(r.sensor_token, -1);
    EXPECT_TRUE(r.timestamp.empty());
}

} // namespace
