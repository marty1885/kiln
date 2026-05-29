#pragma once

#include "cmake-language.hpp"
#include <expected>
#include <vector>

namespace kiln {

class Interpreter;
struct InterpreterError;

// Classify a condition at parse time into a recognized pattern for fast-path evaluation.
PreParsedCondition classify_condition(const std::vector<Argument>& condition);

// Full recursive descent parser fallback.
std::expected<bool, InterpreterError> evaluate_condition(Interpreter& interp, const std::vector<Argument>& condition, size_t row,
                                                         size_t col, size_t offset, size_t length);

// Fast-path evaluator using pre-parsed condition. Falls back to full parser when needed.
std::expected<bool, InterpreterError> evaluate_condition(Interpreter& interp, const std::vector<Argument>& condition,
                                                         const PreParsedCondition& pp, size_t row, size_t col, size_t offset,
                                                         size_t length);

} // namespace kiln
