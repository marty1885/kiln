#include "condition_evaluator.hpp"
#include "interperter.hpp"
#include "regex.hpp"
#include "clock_cache.hpp"
#include "CMakeArray.hpp"
#include "parse_number.hpp"
#include "utils.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <charconv>
#include <filesystem>
#include <limits>
#include <sstream>

namespace kiln {

namespace {

// Set of keywords that should not be dereferenced as variables
// All keywords are in uppercase; comparisons are done case-insensitively
constexpr std::array keywords = {"(",
                                 ")",
                                 "AND",
                                 "COMMAND",
                                 "DEFINED",
                                 "EQUAL",
                                 "EXISTS",
                                 "GREATER",
                                 "GREATER_EQUAL",
                                 "IN_LIST",
                                 "IS_ABSOLUTE",
                                 "IS_DIRECTORY",
                                 "IS_NEWER_THAN",
                                 "IS_SYMLINK",
                                 "LESS",
                                 "LESS_EQUAL",
                                 "MATCHES",
                                 "NOT",
                                 "NOT_EQUAL",
                                 "OR",
                                 "POLICY",
                                 "STREQUAL",
                                 "STRGREATER",
                                 "STRGREATER_EQUAL",
                                 "STRLESS",
                                 "STRLESS_EQUAL",
                                 "TARGET",
                                 "TEST",
                                 "VERSION_EQUAL",
                                 "VERSION_GREATER",
                                 "VERSION_GREATER_EQUAL",
                                 "VERSION_LESS",
                                 "VERSION_LESS_EQUAL"};

// Boolean constants that have fixed truthiness values (case-insensitive)
constexpr std::array boolean_constants = {"FALSE", "IGNORE", "N", "NO", "NOTFOUND", "OFF", "ON", "TRUE", "Y", "YES"};

// Case-insensitive check against boolean_constants using length-based dispatch.
// All constants are pure ASCII letters so (c | 0x20) gives case-insensitive comparison.
bool is_boolean_constant_ci(std::string_view token) {
    auto ci_eq = [](std::string_view a, const char* b) {
        for (size_t i = 0; i < a.size(); ++i)
            if ((a[i] | 0x20) != (b[i] | 0x20)) return false;
        return true;
    };
    switch (token.size()) {
    case 1:
        return (token[0] | 0x20) == 'n' || (token[0] | 0x20) == 'y';
    case 2:
        return ci_eq(token, "NO") || ci_eq(token, "ON");
    case 3:
        return ci_eq(token, "OFF") || ci_eq(token, "YES");
    case 4:
        return ci_eq(token, "TRUE");
    case 5:
        return ci_eq(token, "FALSE");
    case 6:
        return ci_eq(token, "IGNORE");
    case 8:
        return ci_eq(token, "NOTFOUND");
    default:
        return false;
    }
}

// Set of binary operator keywords
constexpr std::array binary_operators = {"EQUAL",
                                         "GREATER",
                                         "GREATER_EQUAL",
                                         "IN_LIST",
                                         "IS_NEWER_THAN",
                                         "LESS",
                                         "LESS_EQUAL",
                                         "MATCHES",
                                         "NOT_EQUAL",
                                         "STREQUAL",
                                         "STRGREATER",
                                         "STRGREATER_EQUAL",
                                         "STRLESS",
                                         "STRLESS_EQUAL",
                                         "VERSION_EQUAL",
                                         "VERSION_GREATER",
                                         "VERSION_GREATER_EQUAL",
                                         "VERSION_LESS",
                                         "VERSION_LESS_EQUAL"};

// Unary operator keywords
constexpr std::array unary_keywords = {"COMMAND",    "DEFINED", "EXISTS", "IS_ABSOLUTE", "IS_DIRECTORY",
                                       "IS_SYMLINK", "POLICY",  "TARGET", "TEST"};

// --- Shared helpers used by both ConditionParser and fast-path ---

bool is_numeric_constant(std::string_view s) {
    if (s.empty()) return false;
    size_t start = 0;
    if (s[0] == '-' || s[0] == '+') start = 1;
    if (start >= s.length()) return false;
    for (size_t i = start; i < s.length(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(s[i])) && s[i] != '.') return false;
    }
    return true;
}

// Returns a reference to the token string. Fast path returns reference into
// the AST (stable); slow path evaluates into caller-provided buffer.
// WARNING: reference is invalidated by the next call with the same buffer.
const std::string& get_token_string(Interpreter& interp, const Argument& arg, std::string& buffer) {
    if (!arg.quoted && arg.parts.size() == 1 && std::holds_alternative<std::string>(arg.parts[0])) {
        return std::get<std::string>(arg.parts[0]);
    }
    buffer = interp.evaluate_argument(arg);
    return buffer;
}

bool is_keyword(std::string_view token) {
    // Arrays are sorted — use binary search instead of linear scan.
    return std::binary_search(keywords.begin(), keywords.end(), token, [](std::string_view a, std::string_view b) { return a < b; });
}

bool is_binary_operator(std::string_view token) {
    // Length-based dispatch eliminates most comparisons for non-operator tokens.
    switch (token.size()) {
    case 4:
        return token == "LESS";
    case 5:
        return token == "EQUAL";
    case 7:
        return token == "GREATER" || token == "IN_LIST" || token == "MATCHES" || token == "STRLESS";
    case 8:
        return token == "STREQUAL";
    case 9:
        return token == "NOT_EQUAL";
    case 10:
        return token == "LESS_EQUAL" || token == "STRGREATER";
    case 12:
        return token == "VERSION_LESS";
    case 13:
        return token == "GREATER_EQUAL" || token == "IS_NEWER_THAN" || token == "STRLESS_EQUAL" || token == "VERSION_EQUAL";
    case 15:
        return token == "VERSION_GREATER";
    case 16:
        return token == "STRGREATER_EQUAL";
    case 18:
        return token == "VERSION_LESS_EQUAL";
    case 21:
        return token == "VERSION_GREATER_EQUAL";
    default:
        return false;
    }
}

bool is_unary_keyword(std::string_view token) {
    switch (token.size()) {
    case 4:
        return token == "TEST";
    case 6:
        return token == "EXISTS" || token == "POLICY" || token == "TARGET";
    case 7:
        return token == "COMMAND" || token == "DEFINED";
    case 10:
        return token == "IS_SYMLINK";
    case 11:
        return token == "IS_ABSOLUTE";
    case 12:
        return token == "IS_DIRECTORY";
    default:
        return false;
    }
}

// Evaluate MATCHES with CMAKE_MATCH_* side effects. Returns error string on failure.
struct MatchesResult {
    bool matched = false;
    std::string error;
};

MatchesResult evaluate_matches(Interpreter& interp, const std::string& left, const std::string& pattern) {
    thread_local ClockCache<std::string, Regex> cache(8, [](const std::string& p) { return Regex::from_cmake_regex(p); });
    auto re = cache.get(pattern);
    if (!re) { return {false, "MATCHES: invalid regex: " + re.error()}; }
    std::vector<std::string> captures;
    bool result = (*re)->search(left, captures);

    if (result) {
        interp.set_variable("CMAKE_MATCH_COUNT", std::to_string(captures.size() - 1));
        for (size_t i = 0; i < captures.size() && i < 10; ++i) { interp.set_variable("CMAKE_MATCH_" + std::to_string(i), captures[i]); }
        for (size_t i = captures.size(); i < 10; ++i) { interp.set_variable("CMAKE_MATCH_" + std::to_string(i), ""); }
    } else {
        interp.set_variable("CMAKE_MATCH_COUNT", "0");
        for (size_t i = 0; i < 10; ++i) { interp.set_variable("CMAKE_MATCH_" + std::to_string(i), ""); }
    }
    return {result, {}};
}

// --- classify_condition helpers ---

// Check if argument is a single unquoted bare string literal (no variable references)
bool is_bare_literal(const Argument& arg) {
    return !arg.quoted && arg.parts.size() == 1 && std::holds_alternative<std::string>(arg.parts[0]);
}

// Get the bare literal string from an argument. Only valid if is_bare_literal() is true.
std::string_view get_bare_literal(const Argument& arg) {
    return std::get<std::string>(arg.parts[0]);
}

// Try to resolve an argument to a string_view without any allocation.
// Works for: bare literals ("foo"), and simple ${VAR} references (no namespace,
// no nested refs). Returns nullopt if the argument is too complex.
std::optional<std::string_view> try_resolve_sv(Interpreter& interp, const Argument& arg) {
    if (arg.parts.size() != 1) return std::nullopt;

    const auto& part = arg.parts[0];
    if (std::holds_alternative<std::string>(part)) {
        // Bare literal — return view into AST
        return std::string_view(std::get<std::string>(part));
    }
    if (std::holds_alternative<VariableReference>(part)) {
        const auto& ref = std::get<VariableReference>(part);
        if (!ref.namespace_prefix.empty()) return std::nullopt;
        if (ref.name_parts.size() != 1) return std::nullopt;
        if (!std::holds_alternative<std::string>(ref.name_parts[0])) return std::nullopt;
        auto view = interp.get_variable_view(std::get<std::string>(ref.name_parts[0]));
        if (!view) return std::string_view{}; // undefined var → empty
        return *view;
    }
    return std::nullopt;
}

// Check if argument contains any VariableReference parts
bool arg_has_varref(const Argument& arg) {
    for (const auto& part : arg.parts) {
        if (std::holds_alternative<VariableReference>(part)) return true;
    }
    return false;
}

// --- eval_operand for fast-path ---
// Simplified evaluate_token that skips keyword linear scan (safe because classify_condition
// guards ensure operands aren't keywords).
// Returns a string_view valid until the next variable mutation or scope change.
// When dereference produces a result, it points into the variable store; otherwise
// it points into the AST or the caller-provided buffer.
std::string_view eval_operand_sv(Interpreter& interp, const Argument& arg, std::string& buffer) {
    const std::string& token = get_token_string(interp, arg, buffer);

    // Quoted strings returned as-is
    if (arg.quoted) return token;

    // Single variable reference → already expanded
    if (arg.parts.size() == 1 && std::holds_alternative<VariableReference>(arg.parts[0])) { return token; }

    // Numeric constant → return as-is
    if (is_numeric_constant(token)) return token;

    // Dereference as variable. Undefined → keep literal. In binary
    // expressions CMake auto-dereferences even names like N, Y, ON, and OFF
    // before falling back to their literal boolean-constant spelling.
    auto view = interp.get_variable_view(token);
    return view.has_value() ? *view : std::string_view(token);
}

// Fast numeric comparison: try integer first (most common in CMake), then double.
// Returns true if both strings were successfully parsed as numbers, with result in cmp.
// cmp is <0, 0, or >0 like strcmp.
bool try_numeric_compare(std::string_view left, std::string_view right, int& cmp) {
    // CMake parses only the leading numeric portion (e.g. "0;\"No error\"" → 0),
    // so we only require from_chars to succeed, not consume the entire string.
    // However, we must prefer double over int when either operand has a fractional
    // part (e.g. "3.31.0" should parse as 3.31, not truncate to int 3).
    int64_t li, ri;
    auto lr = std::from_chars(left.data(), left.data() + left.size(), li);
    auto rr = std::from_chars(right.data(), right.data() + right.size(), ri);

    bool left_has_dot = lr.ec == std::errc{} && lr.ptr < left.data() + left.size() && *lr.ptr == '.';
    bool right_has_dot = rr.ec == std::errc{} && rr.ptr < right.data() + right.size() && *rr.ptr == '.';

    // Try double first if either side looks like it has a fractional part
    if (left_has_dot || right_has_dot) {
        double ld, rd;
        auto lrd = std::from_chars(left.data(), left.data() + left.size(), ld);
        auto rrd = std::from_chars(right.data(), right.data() + right.size(), rd);
        if (lrd.ec == std::errc{} && rrd.ec == std::errc{}) {
            cmp = (ld > rd) - (ld < rd);
            return true;
        }
    }

    // Use integer comparison when both parsed cleanly as integers
    if (lr.ec == std::errc{} && rr.ec == std::errc{}) {
        cmp = (li > ri) - (li < ri);
        return true;
    }

    // Fall back to double for other cases
    double ld, rd;
    auto lrd = std::from_chars(left.data(), left.data() + left.size(), ld);
    auto rrd = std::from_chars(right.data(), right.data() + right.size(), rd);
    if (lrd.ec == std::errc{} && rrd.ec == std::errc{}) {
        cmp = (ld > rd) - (ld < rd);
        return true;
    }
    return false;
}

// Apply a numeric comparison operator given a three-way cmp result.
bool apply_numeric_op(ConditionOp op, int cmp) {
    switch (op) {
    case ConditionOp::BinaryEqual:
        return cmp == 0;
    case ConditionOp::BinaryNotEqual:
        return cmp != 0;
    case ConditionOp::BinaryLess:
        return cmp < 0;
    case ConditionOp::BinaryGreater:
        return cmp > 0;
    case ConditionOp::BinaryLessEqual:
        return cmp <= 0;
    case ConditionOp::BinaryGreaterEqual:
        return cmp >= 0;
    default:
        return false;
    }
}

// --- Filter + fallback for dynamic args that expanded to empty/semicolons ---
std::expected<bool, InterpreterError> evaluate_condition_with_filtering(Interpreter& interp, const std::vector<Argument>& condition,
                                                                        size_t row, size_t col, size_t offset, size_t length) {
    // CMake argument elision and list expansion
    std::vector<Argument> filtered;
    for (const auto& arg : condition) {
        bool has_var_ref = false;
        for (const auto& part : arg.parts) {
            if (std::holds_alternative<VariableReference>(part)) {
                has_var_ref = true;
                break;
            }
        }

        if (has_var_ref && !arg.quoted) {
            std::string val = interp.evaluate_argument(arg);
            if (val.empty()) continue; // elision
            for (auto item : CMakeArrayIterator(val)) {
                if (item.empty()) continue;
                Argument new_arg;
                new_arg.quoted = false;
                new_arg.parts.push_back(std::string(item));
                filtered.push_back(std::move(new_arg));
            }
        } else {
            filtered.push_back(arg);
        }
    }
    return evaluate_condition(interp, filtered, row, col, offset, length);
}

// IS_NEWER_THAN: returns true if lhs is newer-or-equal, OR either path can't
// be stat'd (CMake quirk: missing files compare as "newer" so the rule fires).
bool is_newer_than(const std::string& lhs, const std::string& rhs) {
    std::error_code ec1, ec2;
    auto t1 = std::filesystem::last_write_time(lhs, ec1);
    auto t2 = std::filesystem::last_write_time(rhs, ec2);
    if (ec1 || ec2) return true;
    return t1 >= t2;
}

// Single dispatch table for all classified ops, used by both the compound
// sub-condition path and the fast-path evaluator.
//
// Caller responsibilities:
//   - Operand classification guards: classify_condition rejects bare-keyword
//     operands, so eval_operand_sv (which skips the keyword check) is safe here.
//   - Negation: caller applies pp.negated() / sub.negated() to the result.
//   - Error mapping: regex errors surface as std::unexpected(msg). Compound
//     callers translate to nullopt (fall back); fast-path translates to
//     interp.set_fatal_error.
//
// right_arg is null for unary ops; binary ops without it return an internal
// error (should never happen if the op was classified correctly).
std::expected<bool, std::string> dispatch_op(Interpreter& interp, ConditionOp op, const Argument& left_arg, const Argument* right_arg) {
    switch (op) {
    case ConditionOp::BoolCheck: {
        std::string buffer;
        const std::string& token = get_token_string(interp, left_arg, buffer);
        if (is_boolean_constant_ci(token)) return !Interpreter::is_falsy(token);
        if (is_numeric_constant(token)) return !Interpreter::is_falsy(token);
        if (left_arg.quoted) return false;
        auto view = interp.get_variable_view(token);
        return view.has_value() && !Interpreter::is_falsy(*view);
    }
    case ConditionOp::Defined: {
        std::string buffer;
        const std::string& var_name = get_token_string(interp, left_arg, buffer);
        if (var_name.size() > 6 && var_name.compare(0, 6, "CACHE{") == 0 && var_name.back() == '}') {
            return interp.get_cache_variables().contains(var_name.substr(6, var_name.size() - 7));
        }
        return interp.is_variable_set(var_name);
    }
    case ConditionOp::Target: {
        std::string buffer;
        const std::string& name = get_token_string(interp, left_arg, buffer);
        return interp.find_target(name) != nullptr;
    }
    case ConditionOp::Exists:
        return interp.cached_file_exists(interp.evaluate_argument(left_arg));
    case ConditionOp::IsDirectory:
        return interp.cached_is_directory(interp.evaluate_argument(left_arg));
    case ConditionOp::IsAbsolute:
        return std::filesystem::path(interp.evaluate_argument(left_arg)).is_absolute();
    case ConditionOp::IsSymlink:
        return std::filesystem::is_symlink(interp.evaluate_argument(left_arg));
    case ConditionOp::Command: {
        std::string buffer;
        auto name = eval_operand_sv(interp, left_arg, buffer);
        return interp.has_user_function(std::string(name));
    }
    default:
        break; // fall through to binary handling
    }

    if (!right_arg) return std::unexpected(std::string("dispatch_op: binary op missing right operand"));

    // BinaryIsNewerThan needs the full evaluate_argument output (paths), not
    // the operand-style resolution. Handle before the eval_operand_sv calls
    // so we don't pay for those.
    if (op == ConditionOp::BinaryIsNewerThan) {
        std::string lpath = interp.evaluate_argument(left_arg);
        std::string rpath = interp.evaluate_argument(*right_arg);
        return is_newer_than(lpath, rpath);
    }

    std::string buf_l, buf_r;
    auto left = eval_operand_sv(interp, left_arg, buf_l);
    auto right = eval_operand_sv(interp, *right_arg, buf_r);

    switch (op) {
    case ConditionOp::BinaryEqual:
    case ConditionOp::BinaryNotEqual:
    case ConditionOp::BinaryLess:
    case ConditionOp::BinaryGreater:
    case ConditionOp::BinaryLessEqual:
    case ConditionOp::BinaryGreaterEqual: {
        int cmp;
        if (try_numeric_compare(left, right, cmp)) return apply_numeric_op(op, cmp);
        if (op == ConditionOp::BinaryEqual) return left == right;
        if (op == ConditionOp::BinaryNotEqual) return left != right;
        return false;
    }
    case ConditionOp::BinaryStrEqual:
        return left == right;
    case ConditionOp::BinaryStrLess:
        return left < right;
    case ConditionOp::BinaryStrGreater:
        return left > right;
    case ConditionOp::BinaryStrLessEqual:
        return left <= right;
    case ConditionOp::BinaryStrGreaterEqual:
        return left >= right;

    case ConditionOp::BinaryVersionEqual:
    case ConditionOp::BinaryVersionLess:
    case ConditionOp::BinaryVersionGreater:
    case ConditionOp::BinaryVersionLessEqual:
    case ConditionOp::BinaryVersionGreaterEqual: {
        int cmp = compare_versions(std::string(left), std::string(right));
        switch (op) {
        case ConditionOp::BinaryVersionEqual:
            return cmp == 0;
        case ConditionOp::BinaryVersionLess:
            return cmp < 0;
        case ConditionOp::BinaryVersionGreater:
            return cmp > 0;
        case ConditionOp::BinaryVersionLessEqual:
            return cmp <= 0;
        case ConditionOp::BinaryVersionGreaterEqual:
            return cmp >= 0;
        default:
            return false;
        }
    }
    case ConditionOp::BinaryMatches: {
        auto mr = evaluate_matches(interp, std::string(left), std::string(right));
        if (!mr.error.empty()) return std::unexpected(mr.error);
        return mr.matched;
    }
    case ConditionOp::BinaryInList:
        return cmake_list_contains(std::string(right), std::string(left));

    default:
        return false;
    }
}

// ConditionParser (full recursive descent — fallback path)
class ConditionParser {
public:
    ConditionParser(Interpreter& interp, const std::vector<Argument>& condition) : interp_(interp), condition_(condition) {}

    bool parse() { return parse_or(); }

    const std::string& error() const { return error_msg_; }
    size_t pos() const { return pos_; }

private:
    Interpreter& interp_;
    const std::vector<Argument>& condition_;
    size_t pos_ = 0;
    std::string error_msg_;
    std::string eval_buffer_;

    const std::string& parser_get_token_string(const Argument& arg) { return get_token_string(interp_, arg, eval_buffer_); }

    std::string_view peek_bare() const {
        if (pos_ >= condition_.size()) return {};
        const auto& arg = condition_[pos_];
        if (!arg.quoted && arg.parts.size() == 1 && std::holds_alternative<std::string>(arg.parts[0]))
            return std::get<std::string>(arg.parts[0]);
        return {};
    }

    std::string evaluate_token(const Argument& arg) {
        const std::string& token = parser_get_token_string(arg);

        if (arg.quoted || std::find(keywords.begin(), keywords.end(), token) != keywords.end()) { return token; }

        if (is_numeric_constant(token)) { return token; }

        if (arg.parts.size() == 1 && std::holds_alternative<VariableReference>(arg.parts[0])) { return token; }

        auto view = interp_.get_variable_view(token);
        return view.has_value() ? std::string(*view) : std::string(token);
    }

    bool parse_or() {
        bool left = parse_and();
        while (pos_ < condition_.size() && error_msg_.empty()) {
            if (peek_bare() == "OR") {
                pos_++;
                if (pos_ >= condition_.size()) {
                    error_msg_ = "OR operator requires a right operand";
                    return false;
                }
                bool right = parse_and();
                left = left || right;
            } else {
                break;
            }
        }
        return left;
    }

    bool parse_and() {
        bool left = parse_not();
        while (pos_ < condition_.size() && error_msg_.empty()) {
            if (peek_bare() == "AND") {
                pos_++;
                if (pos_ >= condition_.size()) {
                    error_msg_ = "AND operator requires a right operand";
                    return false;
                }
                bool right = parse_not();
                left = left && right;
            } else {
                break;
            }
        }
        return left;
    }

    bool parse_not() {
        if (pos_ >= condition_.size()) {
            error_msg_ = "Unexpected end of condition";
            return false;
        }

        if (peek_bare() == "NOT") {
            if (pos_ + 1 < condition_.size()) {
                // Only check bare unquoted literals as potential operators/keywords.
                // Quoted arguments are never operators regardless of their expanded value.
                auto next_bare = std::string_view{};
                const auto& next_arg = condition_[pos_ + 1];
                if (!next_arg.quoted && next_arg.parts.size() == 1 && std::holds_alternative<std::string>(next_arg.parts[0])) {
                    next_bare = std::get<std::string>(next_arg.parts[0]);
                }
                if (!next_bare.empty() && is_binary_operator(next_bare)) {
                    // NOT followed by a binary operator keyword — treat NOT as a plain value
                    return parse_comparison();
                }
                if (next_bare != "AND" && next_bare != "OR") {
                    pos_++;
                    return !parse_not();
                }
            }
        }
        return parse_comparison();
    }

    bool parse_comparison() {
        if (pos_ >= condition_.size()) return false;

        if (peek_bare() == "MATCHES" && pos_ + 1 < condition_.size()) {
            pos_ += 2;
            return false;
        }

        if (!condition_[pos_].quoted) {
            const std::string& current_token = parser_get_token_string(condition_[pos_]);
            if (is_binary_operator(current_token) && pos_ + 1 < condition_.size()) {
                error_msg_ = "if given arguments: \"" + current_token + "\" - missing left operand";
                return false;
            }
        }

        size_t start_pos = pos_;
        bool unary_result = parse_unary_or_primary();

        if (pos_ >= condition_.size()) { return unary_result; }

        // Only bare unquoted literals can be operators
        auto op_bare = peek_bare();
        std::string op(op_bare);

        if (op == "EQUAL" || op == "LESS" || op == "GREATER" || op == "LESS_EQUAL" || op == "GREATER_EQUAL" || op == "NOT_EQUAL") {
            pos_++;
            if (pos_ >= condition_.size()) {
                error_msg_ = op + " operator requires a right operand";
                return false;
            }
            std::string left = evaluate_token(condition_[start_pos]);
            std::string right = evaluate_token(condition_[pos_++]);
            int cmp;
            if (try_numeric_compare(left, right, cmp)) {
                if (op == "EQUAL") return cmp == 0;
                if (op == "NOT_EQUAL") return cmp != 0;
                if (op == "LESS") return cmp < 0;
                if (op == "GREATER") return cmp > 0;
                if (op == "LESS_EQUAL") return cmp <= 0;
                if (op == "GREATER_EQUAL") return cmp >= 0;
            } else {
                if (op == "EQUAL") return left == right;
                if (op == "NOT_EQUAL") return left != right;
                return false;
            }
        } else if (op == "STREQUAL" || op == "STRLESS" || op == "STRGREATER" || op == "STRLESS_EQUAL" || op == "STRGREATER_EQUAL") {
            pos_++;
            if (pos_ >= condition_.size()) {
                error_msg_ = op + " operator requires a right operand";
                return false;
            }
            std::string left = evaluate_token(condition_[start_pos]);
            std::string right = evaluate_token(condition_[pos_++]);
            if (op == "STREQUAL") return left == right;
            if (op == "STRLESS") return left < right;
            if (op == "STRGREATER") return left > right;
            if (op == "STRLESS_EQUAL") return left <= right;
            if (op == "STRGREATER_EQUAL") return left >= right;
        } else if (op.starts_with("VERSION_")) {
            pos_++;
            if (pos_ >= condition_.size()) {
                error_msg_ = op + " operator requires a right operand";
                return false;
            }
            std::string left = evaluate_token(condition_[start_pos]);
            std::string right = evaluate_token(condition_[pos_++]);
            int cmp = compare_versions(left, right);
            if (op == "VERSION_EQUAL") return cmp == 0;
            if (op == "VERSION_LESS") return cmp < 0;
            if (op == "VERSION_GREATER") return cmp > 0;
            if (op == "VERSION_LESS_EQUAL") return cmp <= 0;
            if (op == "VERSION_GREATER_EQUAL") return cmp >= 0;
        } else if (op == "MATCHES") {
            pos_++;
            if (pos_ >= condition_.size()) {
                error_msg_ = "MATCHES operator requires a right operand";
                return false;
            }
            std::string pattern = evaluate_token(condition_[pos_++]);
            std::string left = evaluate_token(condition_[start_pos]);
            auto mr = evaluate_matches(interp_, left, pattern);
            if (!mr.error.empty()) {
                error_msg_ = mr.error;
                return false;
            }
            return mr.matched;
        } else if (op == "IN_LIST") {
            pos_++;
            if (pos_ >= condition_.size()) {
                error_msg_ = "IN_LIST operator requires a right operand";
                return false;
            }
            std::string value = evaluate_token(condition_[start_pos]);
            std::string list_str = evaluate_token(condition_[pos_++]);
            return cmake_list_contains(list_str, value);
        } else if (op == "IS_NEWER_THAN") {
            pos_++;
            if (pos_ >= condition_.size()) {
                error_msg_ = "IS_NEWER_THAN operator requires a right operand";
                return false;
            }
            std::string left = interp_.evaluate_argument(condition_[start_pos]);
            std::string right = interp_.evaluate_argument(condition_[pos_++]);
            return is_newer_than(left, right);
        }

        return unary_result;
    }

    bool parse_unary_or_primary() {
        if (pos_ >= condition_.size()) return false;

        if (peek_bare() == "(") {
            pos_++;
            if (peek_bare() == ")") {
                pos_++;
                return false;
            }
            bool result = parse_or();
            if (peek_bare() != ")") {
                error_msg_ = "Expected ')' to close group";
                return false;
            }
            pos_++;
            return result;
        }

        std::string token = parser_get_token_string(condition_[pos_]);

        if (!condition_[pos_].quoted && token == "DEFINED" && pos_ + 1 < condition_.size()) {
            pos_++;
            std::string var_name = parser_get_token_string(condition_[pos_++]);
            if (var_name.size() > 6 && var_name.compare(0, 6, "CACHE{") == 0 && var_name.back() == '}') {
                std::string cache_var = var_name.substr(6, var_name.size() - 7);
                return interp_.get_cache_variables().contains(cache_var);
            }
            return interp_.is_variable_set(var_name);
        } else if (!condition_[pos_].quoted && token == "TARGET" && pos_ + 1 < condition_.size()) {
            pos_++;
            std::string target_name = parser_get_token_string(condition_[pos_++]);
            return interp_.find_target(target_name) != nullptr;
        } else if (!condition_[pos_].quoted && token == "EXISTS" && pos_ + 1 < condition_.size()) {
            pos_++;
            std::string path = interp_.evaluate_argument(condition_[pos_++]);
            return interp_.cached_file_exists(path);
        } else if (!condition_[pos_].quoted && token == "IS_DIRECTORY" && pos_ + 1 < condition_.size()) {
            pos_++;
            std::string path = interp_.evaluate_argument(condition_[pos_++]);
            return interp_.cached_is_directory(path);
        } else if (!condition_[pos_].quoted && token == "IS_ABSOLUTE" && pos_ + 1 < condition_.size()) {
            pos_++;
            std::string path = interp_.evaluate_argument(condition_[pos_++]);
            return std::filesystem::path(path).is_absolute();
        } else if (!condition_[pos_].quoted && token == "IS_SYMLINK" && pos_ + 1 < condition_.size()) {
            pos_++;
            std::string path = interp_.evaluate_argument(condition_[pos_++]);
            return std::filesystem::is_symlink(path);
        } else if (!condition_[pos_].quoted && token == "COMMAND" && pos_ + 1 < condition_.size()) {
            pos_++;
            std::string name = evaluate_token(condition_[pos_++]);
            return interp_.has_user_function(name);
        } else if (!condition_[pos_].quoted && token == "POLICY" && pos_ + 1 < condition_.size()) {
            pos_++;
            pos_++;
            return true;
        }

        const Argument& arg = condition_[pos_++];
        std::string token_str = parser_get_token_string(arg);

        if (is_boolean_constant_ci(token_str)) { return !Interpreter::is_falsy(token_str); }

        if (is_numeric_constant(token_str)) { return !Interpreter::is_falsy(token_str); }

        if (arg.quoted) { return false; }

        auto val_view = interp_.get_variable_view(token_str);
        return val_view.has_value() && !Interpreter::is_falsy(*val_view);
    }
};

// Evaluate a single sub-condition from a compound AND/OR chain.
// Returns the boolean result. For dynamic sub-conditions that expand to
// empty or contain semicolons, returns std::nullopt to signal fallback.
std::optional<bool> evaluate_sub_condition(Interpreter& interp, const std::vector<Argument>& condition,
                                           const PreParsedCondition::SubCondition& sub) {
    bool is_binary = (static_cast<uint8_t>(sub.op) >= static_cast<uint8_t>(ConditionOp::BinaryEqual) && sub.op != ConditionOp::CompoundAnd
                      && sub.op != ConditionOp::CompoundOr);

    // Dynamic args: any operand whose varref expansion is empty or contains
    // ';' triggers a full-parser fallback (CMake list elision semantics).
    if (sub.has_dynamic_args()) {
        auto check = [&](uint8_t idx) -> bool {
            const auto& arg = condition[idx];
            if (arg.quoted || !arg_has_varref(arg)) return true;
            std::string val = interp.evaluate_argument(arg);
            return !val.empty() && val.find(';') == std::string::npos;
        };
        if (!check(sub.left_idx)) return std::nullopt;
        if (is_binary && !check(sub.right_idx)) return std::nullopt;
    }

    const Argument* right = is_binary ? &condition[sub.right_idx] : nullptr;
    auto r = dispatch_op(interp, sub.op, condition[sub.left_idx], right);
    if (!r) return std::nullopt; // regex error → caller falls back to full parser
    return sub.negated() ? !*r : *r;
}

} // anonymous namespace

// --- classify_condition (parse-time) ---

PreParsedCondition classify_condition(const std::vector<Argument>& condition) {
    PreParsedCondition pp;
    const size_t n = condition.size();

    if (n == 0) return pp;
    // Indices are uint8_t (max 255).
    if (n > std::numeric_limits<uint8_t>::max()) return pp;

    // Helper: check if an operand-position argument is a bare keyword literal.
    // If so, we can't fast-path because the full parser has special keyword handling.
    auto operand_is_keyword = [&](size_t idx) -> bool {
        if (!is_bare_literal(condition[idx])) return false;
        auto lit = get_bare_literal(condition[idx]);
        return is_keyword(lit);
    };

    // Helper: check dynamic args flag
    auto compute_dynamic_flags = [&](uint8_t& flags, std::initializer_list<size_t> indices) {
        for (size_t idx : indices) {
            if (idx < n && !condition[idx].quoted && arg_has_varref(condition[idx])) {
                flags |= 2; // has_dynamic_args
                return;
            }
        }
    };

    // Map operator string to ConditionOp for binary operators
    auto binary_op_from_string = [](std::string_view s) -> ConditionOp {
        if (s == "EQUAL") return ConditionOp::BinaryEqual;
        if (s == "NOT_EQUAL") return ConditionOp::BinaryNotEqual;
        if (s == "LESS") return ConditionOp::BinaryLess;
        if (s == "GREATER") return ConditionOp::BinaryGreater;
        if (s == "LESS_EQUAL") return ConditionOp::BinaryLessEqual;
        if (s == "GREATER_EQUAL") return ConditionOp::BinaryGreaterEqual;
        if (s == "STREQUAL") return ConditionOp::BinaryStrEqual;
        if (s == "STRLESS") return ConditionOp::BinaryStrLess;
        if (s == "STRGREATER") return ConditionOp::BinaryStrGreater;
        if (s == "STRLESS_EQUAL") return ConditionOp::BinaryStrLessEqual;
        if (s == "STRGREATER_EQUAL") return ConditionOp::BinaryStrGreaterEqual;
        if (s == "VERSION_EQUAL") return ConditionOp::BinaryVersionEqual;
        if (s == "VERSION_LESS") return ConditionOp::BinaryVersionLess;
        if (s == "VERSION_GREATER") return ConditionOp::BinaryVersionGreater;
        if (s == "VERSION_LESS_EQUAL") return ConditionOp::BinaryVersionLessEqual;
        if (s == "VERSION_GREATER_EQUAL") return ConditionOp::BinaryVersionGreaterEqual;
        if (s == "MATCHES") return ConditionOp::BinaryMatches;
        if (s == "IN_LIST") return ConditionOp::BinaryInList;
        if (s == "IS_NEWER_THAN") return ConditionOp::BinaryIsNewerThan;
        return ConditionOp::Fallback;
    };

    auto unary_op_from_string = [](std::string_view s) -> ConditionOp {
        if (s == "DEFINED") return ConditionOp::Defined;
        if (s == "TARGET") return ConditionOp::Target;
        if (s == "EXISTS") return ConditionOp::Exists;
        if (s == "IS_DIRECTORY") return ConditionOp::IsDirectory;
        if (s == "IS_ABSOLUTE") return ConditionOp::IsAbsolute;
        if (s == "IS_SYMLINK") return ConditionOp::IsSymlink;
        if (s == "COMMAND") return ConditionOp::Command;
        return ConditionOp::Fallback;
    };

    if (n == 1) {
        // if(X) → BoolCheck
        pp.op = ConditionOp::BoolCheck;
        pp.left_idx = 0;
        compute_dynamic_flags(pp.flags, {0});
        return pp;
    }

    if (n == 2) {
        if (!is_bare_literal(condition[0])) return pp; // Fallback
        auto kw = get_bare_literal(condition[0]);

        if (kw == "NOT") {
            // NOT X → BoolCheck negated
            pp.op = ConditionOp::BoolCheck;
            pp.flags |= 1; // negated
            pp.left_idx = 1;
            compute_dynamic_flags(pp.flags, {1});
            return pp;
        }

        ConditionOp uop = unary_op_from_string(kw);
        if (uop != ConditionOp::Fallback) {
            // DEFINED/TARGET/EXISTS/... X
            pp.op = uop;
            pp.left_idx = 1;
            compute_dynamic_flags(pp.flags, {1});
            return pp;
        }

        return pp; // Fallback
    }

    if (n == 3) {
        // Check for NOT + unary-keyword + X
        if (is_bare_literal(condition[0]) && get_bare_literal(condition[0]) == "NOT" && is_bare_literal(condition[1])) {
            auto kw = get_bare_literal(condition[1]);
            ConditionOp uop = unary_op_from_string(kw);
            if (uop != ConditionOp::Fallback) {
                pp.op = uop;
                pp.flags |= 1; // negated
                pp.left_idx = 2;
                compute_dynamic_flags(pp.flags, {2});
                return pp;
            }
        }

        // Check for X binary-op Y
        if (is_bare_literal(condition[1])) {
            auto op_str = get_bare_literal(condition[1]);
            ConditionOp bop = binary_op_from_string(op_str);
            if (bop != ConditionOp::Fallback) {
                // Guard: left operand must not be a bare keyword
                if (operand_is_keyword(0)) return pp;
                // Guard: left operand must not be a unary keyword
                if (is_bare_literal(condition[0]) && is_unary_keyword(get_bare_literal(condition[0]))) return pp;
                // Guard: right operand must not be a bare keyword (except it's fine for some)
                // Actually the full parser evaluates both sides as evaluate_token, which handles keywords
                // But we need to guard against operands that ARE binary operators (would confuse full parser)
                // The classify guards are: operands must not be keywords. But actually, the full parser
                // handles keywords fine in evaluate_token (returns them as-is). The real concern is that
                // our fast-path eval_operand skips the keyword check. So we must guard.
                if (operand_is_keyword(2)) return pp;

                pp.op = bop;
                pp.left_idx = 0;
                pp.right_idx = 2;
                compute_dynamic_flags(pp.flags, {0, 2});
                return pp;
            }
        }

        return pp; // Fallback
    }

    if (n == 4) {
        // NOT X binary-op Y
        if (!is_bare_literal(condition[0]) || get_bare_literal(condition[0]) != "NOT") return pp;
        if (!is_bare_literal(condition[2])) return pp;

        auto op_str = get_bare_literal(condition[2]);
        ConditionOp bop = binary_op_from_string(op_str);
        if (bop == ConditionOp::Fallback) return pp;

        // Guard: left operand (token[1]) must not be a bare keyword
        if (operand_is_keyword(1)) return pp;
        // Guard: token[1] must not be a unary keyword
        if (is_bare_literal(condition[1]) && is_unary_keyword(get_bare_literal(condition[1]))) return pp;
        // NOT-before-binary guard: token[1] must not be a binary operator
        if (is_bare_literal(condition[1]) && is_binary_operator(get_bare_literal(condition[1]))) return pp;
        // Guard: right operand must not be a keyword
        if (operand_is_keyword(3)) return pp;

        pp.op = bop;
        pp.flags |= 1; // negated
        pp.left_idx = 1;
        pp.right_idx = 3;
        compute_dynamic_flags(pp.flags, {1, 3});
        return pp;
    }

    // --- Compound AND/OR chains (n >= 5) ---
    // Scan for AND/OR at top level. Bail on parentheses or mixed AND+OR.
    {
        bool has_and = false, has_or = false, has_parens = false;
        for (size_t i = 0; i < n; ++i) {
            if (!is_bare_literal(condition[i])) continue;
            auto lit = get_bare_literal(condition[i]);
            if (lit == "AND")
                has_and = true;
            else if (lit == "OR")
                has_or = true;
            else if (lit == "(" || lit == ")")
                has_parens = true;
        }

        if (has_parens) return pp;          // Parenthesized → fallback
        if (has_and && has_or) return pp;   // Mixed AND+OR → fallback
        if (!has_and && !has_or) return pp; // No connectives → fallback

        ConditionOp compound_op = has_and ? ConditionOp::CompoundAnd : ConditionOp::CompoundOr;
        std::string_view connective = has_and ? "AND" : "OR";

        // Split into sub-expressions at connective boundaries.
        // Each sub-expression must be 1-3 tokens (handled by existing classify logic).
        uint8_t num_sub = 0;
        size_t sub_start = 0;

        auto classify_sub = [&](size_t start, size_t end) -> bool {
            size_t sub_n = end - start;
            if (sub_n == 0 || sub_n > 3) return false;
            if (num_sub >= PreParsedCondition::MAX_SUB) return false;

            auto& sub = pp.subs[num_sub];
            sub.flags = 0;

            if (sub_n == 1) {
                // BoolCheck
                sub.op = ConditionOp::BoolCheck;
                sub.left_idx = static_cast<uint8_t>(start);
                sub.right_idx = 0;
                // Dynamic flag
                if (!condition[start].quoted && arg_has_varref(condition[start])) sub.flags |= 2;
            } else if (sub_n == 2) {
                // NOT X or UNARY X
                if (!is_bare_literal(condition[start])) return false;
                auto kw = get_bare_literal(condition[start]);
                if (kw == "NOT") {
                    sub.op = ConditionOp::BoolCheck;
                    sub.flags |= 1; // negated
                    sub.left_idx = static_cast<uint8_t>(start + 1);
                    if (!condition[start + 1].quoted && arg_has_varref(condition[start + 1])) sub.flags |= 2;
                } else {
                    ConditionOp uop = unary_op_from_string(kw);
                    if (uop == ConditionOp::Fallback) return false;
                    sub.op = uop;
                    sub.left_idx = static_cast<uint8_t>(start + 1);
                    if (!condition[start + 1].quoted && arg_has_varref(condition[start + 1])) sub.flags |= 2;
                }
                sub.right_idx = 0;
            } else {
                // sub_n == 3: X binary-op Y  or  NOT UNARY X  or  NOT X (bool)
                // Check for NOT + unary: NOT DEFINED x, NOT EXISTS x, etc.
                if (is_bare_literal(condition[start]) && get_bare_literal(condition[start]) == "NOT"
                    && is_bare_literal(condition[start + 1])) {
                    auto kw = get_bare_literal(condition[start + 1]);
                    ConditionOp uop = unary_op_from_string(kw);
                    if (uop != ConditionOp::Fallback) {
                        sub.op = uop;
                        sub.flags |= 1; // negated
                        sub.left_idx = static_cast<uint8_t>(start + 2);
                        sub.right_idx = 0;
                        if (!condition[start + 2].quoted && arg_has_varref(condition[start + 2])) sub.flags |= 2;
                        num_sub++;
                        return true;
                    }
                }

                // X binary-op Y
                if (!is_bare_literal(condition[start + 1])) return false;
                auto op_str = get_bare_literal(condition[start + 1]);
                ConditionOp bop = binary_op_from_string(op_str);
                if (bop == ConditionOp::Fallback) return false;
                // MATCHES has side effects (sets CMAKE_MATCH_0..9 and
                // CMAKE_MATCH_COUNT). CMake always evaluates all sub-expressions
                // (no short-circuit), so MATCHES side effects are always visible.
                // Our compound path short-circuits, so we must exclude MATCHES
                // to avoid silently skipping those writes.
                if (bop == ConditionOp::BinaryMatches) return false;
                // Guard: operands must not be bare keywords
                if (operand_is_keyword(start)) return false;
                if (is_bare_literal(condition[start]) && is_unary_keyword(get_bare_literal(condition[start]))) return false;
                if (operand_is_keyword(start + 2)) return false;
                sub.op = bop;
                sub.left_idx = static_cast<uint8_t>(start);
                sub.right_idx = static_cast<uint8_t>(start + 2);
                // Dynamic flags
                if (!condition[start].quoted && arg_has_varref(condition[start])) sub.flags |= 2;
                if (!condition[start + 2].quoted && arg_has_varref(condition[start + 2])) sub.flags |= 2;
            }

            num_sub++;
            return true;
        };

        bool ok = true;
        for (size_t i = 0; i <= n; ++i) {
            bool is_connective = (i < n && is_bare_literal(condition[i]) && get_bare_literal(condition[i]) == connective);
            bool is_end = (i == n);

            if (is_connective || is_end) {
                if (!classify_sub(sub_start, i)) {
                    ok = false;
                    break;
                }
                sub_start = i + 1; // skip the connective token
            }
        }

        if (ok && num_sub >= 2) {
            pp.op = compound_op;
            pp.num_sub = num_sub;
            // Aggregate dynamic flags from all sub-conditions
            for (uint8_t i = 0; i < num_sub; ++i) {
                if (pp.subs[i].has_dynamic_args()) {
                    pp.flags |= 2;
                    break;
                }
            }
            return pp;
        }
    }

    return pp; // Fallback
}

// --- Full parser fallback ---

std::expected<bool, InterpreterError> evaluate_condition(Interpreter& interp, const std::vector<Argument>& condition, size_t row,
                                                         size_t col, size_t offset, size_t length) {
    if (condition.empty()) { return false; }

    ConditionParser parser(interp, condition);
    bool result = parser.parse();

    if (!parser.error().empty()) {
        interp.set_fatal_error(parser.error());
        return std::unexpected(*interp.get_fatal_error());
    }

    if (parser.pos() < condition.size()) {
        std::string remaining;
        for (size_t i = parser.pos(); i < condition.size(); ++i) {
            if (!remaining.empty()) remaining += " ";
            if (!condition[i].quoted && condition[i].parts.size() == 1 && std::holds_alternative<std::string>(condition[i].parts[0])) {
                remaining += std::get<std::string>(condition[i].parts[0]);
            } else {
                remaining += interp.evaluate_argument(condition[i]);
            }
        }

        auto is_whitespace = [](char ch) { return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n'; };
        size_t start = 0;
        while (start < remaining.size() && is_whitespace(remaining[start])) ++start;
        size_t end = remaining.size();
        while (end > start && is_whitespace(remaining[end - 1])) --end;
        remaining = remaining.substr(start, end - start);

        if (!remaining.empty()) {
            interp.set_fatal_error("if() condition has unexpected tokens: " + remaining);
            return std::unexpected(*interp.get_fatal_error());
        }
    }

    return result;
}

// --- Fast-path evaluator ---

std::expected<bool, InterpreterError> evaluate_condition(Interpreter& interp, const std::vector<Argument>& condition,
                                                         const PreParsedCondition& pp, size_t row, size_t col, size_t offset,
                                                         size_t length) {
    if (condition.empty()) return false;

    // Fallback: use full parser with filtering
    if (pp.op == ConditionOp::Fallback) { return evaluate_condition_with_filtering(interp, condition, row, col, offset, length); }

    // Compound AND/OR: iterate sub-conditions with short-circuit.
    //
    // NOTE: CMake does NOT short-circuit AND/OR — it always evaluates all
    // sub-expressions left-to-right. We short-circuit here for performance,
    // which is safe ONLY because:
    //   1. MATCHES (the only side-effecting operator — sets CMAKE_MATCH_*)
    //      is excluded at classify time (classify_sub rejects BinaryMatches).
    //   2. All remaining operators are pure (no observable state mutation).
    //
    // Known divergence from CMake: if a skipped sub-expression would have
    // produced an error (e.g. via the full parser fallback path), we silently
    // succeed instead. This is acceptable for now since classify_sub only
    // accepts well-formed 1-3 token sub-expressions, but should be revisited
    // if we ever loosen the classifier.
    if (pp.op == ConditionOp::CompoundAnd || pp.op == ConditionOp::CompoundOr) {
        bool is_and = (pp.op == ConditionOp::CompoundAnd);
        bool result = is_and; // AND starts true, OR starts false

        for (uint8_t i = 0; i < pp.num_sub; ++i) {
            auto sub_result = evaluate_sub_condition(interp, condition, pp.subs[i]);
            if (!sub_result.has_value()) {
                // Dynamic arg expanded to empty/semicolons or error → full fallback
                return evaluate_condition_with_filtering(interp, condition, row, col, offset, length);
            }
            if (is_and) {
                if (!*sub_result) {
                    result = false;
                    break;
                }
            } else {
                if (*sub_result) {
                    result = true;
                    break;
                }
            }
        }

        if (pp.negated()) result = !result;
        return result;
    }

    // Safety: indices are set to literal 0-3 by classify_condition (which only
    // accepts n <= 4), so uint8_t overflow is impossible. But verify bounds at
    // runtime in case the condition vector was modified after classification.
    bool has_binary = (static_cast<uint8_t>(pp.op) >= static_cast<uint8_t>(ConditionOp::BinaryEqual) && pp.op != ConditionOp::CompoundAnd
                       && pp.op != ConditionOp::CompoundOr);
    if (pp.left_idx >= condition.size() || (has_binary && pp.right_idx >= condition.size())) {
        return evaluate_condition_with_filtering(interp, condition, row, col, offset, length);
    }

    // Fast path for unary file-test ops with a simple ${VAR} operand.
    // Resolves the variable to string_view and calls cached_is_directory /
    // cached_file_exists directly — zero string allocations on the hot path.
    if (pp.has_dynamic_args() && (pp.op == ConditionOp::IsDirectory || pp.op == ConditionOp::Exists)) {
        if (auto sv = try_resolve_sv(interp, condition[pp.left_idx]); sv && !sv->empty() && sv->find(';') == std::string_view::npos) {
            bool r = (pp.op == ConditionOp::IsDirectory) ? interp.cached_is_directory(*sv) : interp.cached_file_exists(*sv);
            if (pp.negated()) r = !r;
            return r;
        }
    }

    // For dynamic-args operands, pre-expand the varref and stuff the result
    // into a temp Argument. This serves two purposes:
    //   1. CMake list-elision: empty or semicolon-bearing expansions trigger
    //      a fall back to the full parser.
    //   2. Avoids re-evaluating the arg inside dispatch_op (eval_operand_sv
    //      sees a single bare-literal Argument and skips evaluate_argument).
    // Temps must outlive the dispatch_op call, so they live at this scope.
    Argument left_temp, right_temp;
    const Argument* left_arg = &condition[pp.left_idx];
    const Argument* right_arg = has_binary ? &condition[pp.right_idx] : nullptr;

    if (pp.has_dynamic_args()) {
        auto expand = [&](uint8_t idx, Argument& temp, const Argument*& slot) -> bool {
            const auto& arg = condition[idx];
            if (arg.quoted || !arg_has_varref(arg)) return true;
            std::string val = interp.evaluate_argument(arg);
            if (val.empty() || val.find(';') != std::string::npos) return false;
            temp.quoted = false;
            temp.parts.push_back(std::move(val));
            slot = &temp;
            return true;
        };
        if (!expand(pp.left_idx, left_temp, left_arg)) {
            return evaluate_condition_with_filtering(interp, condition, row, col, offset, length);
        }
        if (has_binary && !expand(pp.right_idx, right_temp, right_arg)) {
            return evaluate_condition_with_filtering(interp, condition, row, col, offset, length);
        }
    }

    auto r = dispatch_op(interp, pp.op, *left_arg, right_arg);
    if (!r) {
        interp.set_fatal_error(r.error());
        return std::unexpected(*interp.get_fatal_error());
    }
    bool result = *r;
    if (pp.negated()) result = !result;
    return result;
}

} // namespace kiln
