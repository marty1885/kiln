#include "registry.hpp"
#include "../interperter.hpp"
#include <string>
#include <string_view>
#include <vector>
#include <charconv>
#include <sstream>
#include <iomanip>
#include <cstdint>
#include <stdexcept>

namespace kiln {

// Single-pass recursive descent math evaluator operating directly on string_view.
// No intermediate tokenization step.
class MathEvaluator {
public:
    explicit MathEvaluator(std::string_view expr) : expr_(expr), pos_(0) {}

    int64_t evaluate() {
        skip_whitespace();
        if (pos_ >= expr_.size()) return 0;
        int64_t result = parse_bitwise_or();
        skip_whitespace();
        if (pos_ < expr_.size()) { throw std::runtime_error(std::string("Unexpected character in expression: '") + expr_[pos_] + "'"); }
        return result;
    }

private:
    std::string_view expr_;
    size_t pos_;

    void skip_whitespace() {
        while (pos_ < expr_.size() && (expr_[pos_] == ' ' || expr_[pos_] == '\t' || expr_[pos_] == '\n' || expr_[pos_] == '\r')) ++pos_;
    }

    char peek() const { return pos_ < expr_.size() ? expr_[pos_] : '\0'; }

    int64_t parse_bitwise_or() {
        int64_t left = parse_bitwise_xor();
        while (peek() == '|') {
            ++pos_;
            left |= parse_bitwise_xor();
        }
        return left;
    }

    int64_t parse_bitwise_xor() {
        int64_t left = parse_bitwise_and();
        while (peek() == '^') {
            ++pos_;
            left ^= parse_bitwise_and();
        }
        return left;
    }

    int64_t parse_bitwise_and() {
        int64_t left = parse_shift();
        while (peek() == '&') {
            ++pos_;
            left &= parse_shift();
        }
        return left;
    }

    int64_t parse_shift() {
        int64_t left = parse_additive();
        for (;;) {
            skip_whitespace();
            if (pos_ + 1 < expr_.size() && expr_[pos_] == '<' && expr_[pos_ + 1] == '<') {
                pos_ += 2;
                left <<= parse_additive();
            } else if (pos_ + 1 < expr_.size() && expr_[pos_] == '>' && expr_[pos_ + 1] == '>') {
                pos_ += 2;
                left >>= parse_additive();
            } else {
                break;
            }
        }
        return left;
    }

    int64_t parse_additive() {
        int64_t left = parse_multiplicative();
        for (;;) {
            skip_whitespace();
            char c = peek();
            if (c == '+') {
                ++pos_;
                left += parse_multiplicative();
            } else if (c == '-') {
                ++pos_;
                left -= parse_multiplicative();
            } else
                break;
        }
        return left;
    }

    int64_t parse_multiplicative() {
        int64_t left = parse_unary();
        for (;;) {
            skip_whitespace();
            char c = peek();
            if (c == '*') {
                ++pos_;
                left *= parse_unary();
            } else if (c == '/') {
                ++pos_;
                int64_t right = parse_unary();
                if (right == 0) throw std::runtime_error("Division by zero");
                left /= right;
            } else if (c == '%') {
                ++pos_;
                int64_t right = parse_unary();
                if (right == 0) throw std::runtime_error("Modulo by zero");
                left %= right;
            } else {
                break;
            }
        }
        return left;
    }

    int64_t parse_unary() {
        skip_whitespace();
        char c = peek();
        if (c == '+') {
            ++pos_;
            return parse_unary();
        }
        if (c == '-') {
            ++pos_;
            return -parse_unary();
        }
        if (c == '~') {
            ++pos_;
            return ~parse_unary();
        }
        return parse_primary();
    }

    int64_t parse_primary() {
        skip_whitespace();
        char c = peek();

        if (c == '(') {
            ++pos_;
            int64_t result = parse_bitwise_or();
            skip_whitespace();
            if (peek() != ')') throw std::runtime_error("Expected ')'");
            ++pos_;
            return result;
        }

        if (std::isdigit(static_cast<unsigned char>(c))) { return parse_number(); }

        if (c == '\0') { throw std::runtime_error("Unexpected end of expression"); }
        throw std::runtime_error(std::string("Invalid character in expression: '") + c + "'");
    }

    int64_t parse_number() {
        const char* start = expr_.data() + pos_;

        // Check for hex prefix
        if (pos_ + 1 < expr_.size() && expr_[pos_] == '0' && (expr_[pos_ + 1] == 'x' || expr_[pos_ + 1] == 'X')) {
            pos_ += 2;
            while (pos_ < expr_.size() && std::isxdigit(static_cast<unsigned char>(expr_[pos_]))) { ++pos_; }
            const char* end = expr_.data() + pos_;
            // from_chars for hex doesn't expect "0x" prefix — skip it
            int64_t val;
            auto r = std::from_chars(start + 2, end, val, 16);
            if (r.ec != std::errc{} || r.ptr != end) {
                throw std::runtime_error(std::string("Invalid hex number: ") + std::string(start, end));
            }
            return val;
        }

        // Decimal
        while (pos_ < expr_.size() && std::isdigit(static_cast<unsigned char>(expr_[pos_]))) { ++pos_; }
        const char* end = expr_.data() + pos_;
        int64_t val;
        auto r = std::from_chars(start, end, val, 10);
        if (r.ec != std::errc{} || r.ptr != end) { throw std::runtime_error(std::string("Invalid number: ") + std::string(start, end)); }
        return val;
    }
};

// ---------------------------------------------------------------------------
// Parse-time classification + runtime fast path for math(EXPR ...).
// ---------------------------------------------------------------------------
//
// Recognized shape (anything else → nullopt, caller falls back to slow path):
//   math(EXPR <out_var> "<expression>" [OUTPUT_FORMAT DECIMAL|HEXADECIMAL])
// where:
//   - EXPR, out_var, OUTPUT_FORMAT, and the format keyword are bare string
//     literals (no variable references in those argument slots);
//   - the expression is a flat chain of operands separated by + - * / %,
//     where each operand is either a single VariableReference part or a
//     decimal integer literal (optional unary sign). No parens, no hex,
//     no bitwise/shift, no nested ops.
//
// Why this subset: it's the body of every kiln inner-loop math() in
// numeric workloads, and the runtime executor avoids all of: building the
// expanded vector<string>, joining expression parts into one std::string,
// the 8-level recursive-descent walk in MathEvaluator, and the builtin
// dispatch hashmap lookup.

static std::optional<std::string_view> as_static_literal(const Argument& a) {
    if (a.parts.size() != 1) return std::nullopt;
    auto* s = std::get_if<std::string>(&a.parts[0]);
    if (!s) return std::nullopt;
    return std::string_view(*s);
}

std::optional<PreParsedMath> classify_math(const std::vector<Argument>& args) {
    if (args.size() < 3 || args.size() == 4 || args.size() > 5) return std::nullopt;

    auto first = as_static_literal(args[0]);
    if (!first || *first != "EXPR") return std::nullopt;

    auto out = as_static_literal(args[1]);
    if (!out) return std::nullopt;

    bool hex = false;
    if (args.size() == 5) {
        auto kw = as_static_literal(args[3]);
        if (!kw || *kw != "OUTPUT_FORMAT") return std::nullopt;
        auto fmt = as_static_literal(args[4]);
        if (!fmt) return std::nullopt;
        if (*fmt == "HEXADECIMAL")
            hex = true;
        else if (*fmt != "DECIMAL")
            return std::nullopt;
    }

    PreParsedMath pp;
    pp.out_var = std::string(*out);
    pp.hex_output = hex;

    const auto& expr_arg = args[2];
    enum { ExpectOperand, ExpectOp } state = ExpectOperand;

    for (size_t pi = 0; pi < expr_arg.parts.size(); ++pi) {
        const auto& part = expr_arg.parts[pi];
        if (std::holds_alternative<VariableReference>(part)) {
            if (state != ExpectOperand) return std::nullopt;
            if (pi > UINT16_MAX) return std::nullopt;
            PreParsedMath::Operand op;
            op.is_literal = false;
            op.var_part_idx = static_cast<uint16_t>(pi);
            op.literal = 0;
            pp.operands.push_back(op);
            state = ExpectOp;
            continue;
        }

        // String part: walk char-by-char, alternating operand/op as state demands.
        std::string_view s = std::get<std::string>(part);
        size_t i = 0;
        while (i < s.size()) {
            char c = s[i];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++i;
                continue;
            }

            if (state == ExpectOperand) {
                bool neg = false;
                while (c == '+' || c == '-') {
                    if (c == '-') neg = !neg;
                    ++i;
                    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
                    if (i >= s.size()) return std::nullopt;
                    c = s[i];
                }
                if (c < '0' || c > '9') return std::nullopt;
                if (c == '0' && i + 1 < s.size() && (s[i + 1] == 'x' || s[i + 1] == 'X')) return std::nullopt;
                int64_t v = 0;
                while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
                    v = v * 10 + (s[i] - '0');
                    ++i;
                }
                PreParsedMath::Operand op;
                op.is_literal = true;
                op.var_part_idx = 0;
                op.literal = neg ? -v : v;
                pp.operands.push_back(op);
                state = ExpectOp;
            } else {
                if (c == '+' || c == '-' || c == '*' || c == '/' || c == '%') {
                    pp.ops.push_back(c);
                    ++i;
                    state = ExpectOperand;
                } else {
                    return std::nullopt;
                }
            }
        }
    }

    if (state != ExpectOp) return std::nullopt;
    if (pp.operands.empty()) return std::nullopt;
    if (pp.operands.size() != pp.ops.size() + 1) return std::nullopt;
    return pp;
}

// Resolve a VariableReference's value to int64. Returns false if the value
// is missing, empty, or not parseable as a decimal/hex int. Mirrors what
// MathEvaluator::parse_number accepts so behavior matches the slow path.
static bool resolve_var_to_int(Interpreter& interp, const VariableReference& ref, int64_t& out) {
    // Only fast-path the simple case: bare ${NAME} with a literal name. Nested
    // names (${MY_${PREFIX}_VAR}) and namespaced refs ($ENV{...}, $CACHE{...})
    // are rare in math expressions; they fall back to the slow path.
    if (!ref.namespace_prefix.empty()) return false;
    if (ref.name_parts.size() != 1) return false;
    auto* name_ptr = std::get_if<std::string>(&ref.name_parts[0]);
    if (!name_ptr) return false;

    auto looked_up = interp.get_variable_view(*name_ptr);
    if (!looked_up) return false;
    std::string_view value_sv = *looked_up;

    // Trim ASCII whitespace — CMake math accepts " 42 " style.
    size_t b = 0, e = value_sv.size();
    while (b < e && (value_sv[b] == ' ' || value_sv[b] == '\t')) ++b;
    while (e > b && (value_sv[e - 1] == ' ' || value_sv[e - 1] == '\t')) --e;
    if (b == e) return false;

    const char* start = value_sv.data() + b;
    const char* end = value_sv.data() + e;
    bool neg = false;
    if (*start == '+' || *start == '-') {
        neg = (*start == '-');
        ++start;
        if (start == end) return false;
    }

    int64_t v = 0;
    if (end - start > 2 && start[0] == '0' && (start[1] == 'x' || start[1] == 'X')) {
        auto r = std::from_chars(start + 2, end, v, 16);
        if (r.ec != std::errc{} || r.ptr != end) return false;
    } else {
        auto r = std::from_chars(start, end, v, 10);
        if (r.ec != std::errc{} || r.ptr != end) return false;
    }
    out = neg ? -v : v;
    return true;
}

bool try_execute_pre_parsed_math(Interpreter& interp, const PreParsedMath& pp, const std::vector<Argument>& args) {
    constexpr size_t MAX_INLINE = 16;
    int64_t stack_vals[MAX_INLINE];
    std::vector<int64_t> heap_vals;
    int64_t* vals = stack_vals;
    if (pp.operands.size() > MAX_INLINE) {
        heap_vals.resize(pp.operands.size());
        vals = heap_vals.data();
    }

    const auto& expr_parts = args[2].parts;
    for (size_t i = 0; i < pp.operands.size(); ++i) {
        const auto& op = pp.operands[i];
        if (op.is_literal) {
            vals[i] = op.literal;
        } else {
            const auto& part = expr_parts[op.var_part_idx];
            const auto& vref = std::get<VariableReference>(part);
            if (!resolve_var_to_int(interp, vref, vals[i])) return false;
        }
    }

    // Two-pass precedence: collapse * / % into adjacent values, then sum + / -.
    // n is small (typically 2-4) so we operate in-place with a write index.
    size_t n = pp.operands.size();
    size_t out_n = 1;
    std::vector<char> hi_ops;
    hi_ops.reserve(pp.ops.size());
    int64_t cur = vals[0];
    for (size_t i = 0; i < pp.ops.size(); ++i) {
        char o = pp.ops[i];
        int64_t rhs = vals[i + 1];
        if (o == '*') {
            cur *= rhs;
        } else if (o == '/') {
            if (rhs == 0) return false;
            cur /= rhs;
        } else if (o == '%') {
            if (rhs == 0) return false;
            cur %= rhs;
        } else {
            // + or -: emit current term, start new one.
            vals[out_n - 1] = cur;
            hi_ops.push_back(o);
            cur = rhs;
            ++out_n;
        }
    }
    vals[out_n - 1] = cur;

    int64_t result = vals[0];
    for (size_t i = 0; i < hi_ops.size(); ++i) {
        if (hi_ops[i] == '+')
            result += vals[i + 1];
        else
            result -= vals[i + 1];
    }
    (void) n;

    std::string result_str;
    if (pp.hex_output) {
        char buf[32];
        // CMake's hex output is "0x<lowercase hex, no leading zeros>".
        // Negative values are two's-complement on 64 bits (matches MathEvaluator).
        auto u = static_cast<uint64_t>(result);
        result_str = "0x";
        if (u == 0) {
            result_str += '0';
        } else {
            char* p = buf + sizeof(buf);
            while (u) {
                int d = u & 0xf;
                *--p = static_cast<char>(d < 10 ? '0' + d : 'a' + d - 10);
                u >>= 4;
            }
            result_str.append(p, buf + sizeof(buf) - p);
        }
    } else {
        result_str = std::to_string(result);
    }

    interp.set_variable(pp.out_var, result_str);
    return true;
}

void register_math_builtins(Interpreter& interp) {
    interp.add_builtin("math", [](Interpreter& interp, const std::vector<std::string>& args) {
        // math(EXPR <variable> "<expression>" [OUTPUT_FORMAT <DECIMAL|HEXADECIMAL>])
        if (args.size() < 3) {
            interp.set_fatal_error("math() requires at least 3 arguments: math(EXPR <variable> <expression>)");
            return;
        }

        if (args[0] != "EXPR") {
            interp.set_fatal_error("math() only supports EXPR mode, got: " + args[0]);
            return;
        }

        const std::string& var_name = args[1];
        const std::string& expression = args[2];

        // Parse optional OUTPUT_FORMAT
        std::string_view output_format = "DECIMAL";
        if (args.size() >= 5) {
            if (args[3] == "OUTPUT_FORMAT") {
                output_format = args[4];
            } else {
                interp.set_fatal_error("math(EXPR): unexpected argument '" + args[3] + "', expected OUTPUT_FORMAT");
                return;
            }
            if (args.size() > 5) {
                interp.set_fatal_error("math(EXPR): too many arguments");
                return;
            }
        } else if (args.size() == 4) {
            interp.set_fatal_error("math(EXPR): OUTPUT_FORMAT requires a value (DECIMAL or HEXADECIMAL)");
            return;
        }

        try {
            MathEvaluator eval(expression);
            int64_t result = eval.evaluate();

            std::string result_str;
            if (output_format == "HEXADECIMAL") {
                std::stringstream ss;
                ss << "0x" << std::hex << result;
                result_str = ss.str();
            } else if (output_format == "DECIMAL") {
                result_str = std::to_string(result);
            } else {
                interp.set_fatal_error("Invalid OUTPUT_FORMAT: " + std::string(output_format) + ". Must be DECIMAL or HEXADECIMAL.");
                return;
            }

            interp.set_variable(var_name, result_str);
        } catch (const std::exception& e) { interp.set_fatal_error(std::string("math(EXPR) error: ") + e.what()); }
    });
}

} // namespace kiln
