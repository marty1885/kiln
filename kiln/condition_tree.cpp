#include "cmake-language.hpp"

#include <algorithm>
#include <string>

namespace kiln {

namespace {

// Check if an argument is a bare unquoted string literal (no variable references).
bool is_bare_literal(const Argument& arg) {
    return !arg.quoted && arg.parts.size() == 1 && std::holds_alternative<std::string>(arg.parts[0]);
}

// Get the string value of a bare literal argument.
// Caller must verify is_bare_literal() first.
const std::string& get_literal_string(const Argument& arg) {
    return std::get<std::string>(arg.parts[0]);
}

// Map a keyword string (already uppercase) to its ConditionNode::Type.
// Returns nullopt if not a recognized keyword.
std::optional<ConditionNode::Type> keyword_to_type(const std::string& upper) {
    using T = ConditionNode::Type;
    // Logical
    if (upper == "OR") return T::OR;
    if (upper == "AND") return T::AND;
    if (upper == "NOT") return T::NOT;
    // Numeric comparison
    if (upper == "EQUAL") return T::EQUAL;
    if (upper == "NOT_EQUAL") return T::NOT_EQUAL;
    if (upper == "LESS") return T::LESS;
    if (upper == "GREATER") return T::GREATER;
    if (upper == "LESS_EQUAL") return T::LESS_EQUAL;
    if (upper == "GREATER_EQUAL") return T::GREATER_EQUAL;
    // String comparison
    if (upper == "STREQUAL") return T::STREQUAL;
    if (upper == "STRLESS") return T::STRLESS;
    if (upper == "STRGREATER") return T::STRGREATER;
    if (upper == "STRLESS_EQUAL") return T::STRLESS_EQUAL;
    if (upper == "STRGREATER_EQUAL") return T::STRGREATER_EQUAL;
    // Version comparison
    if (upper == "VERSION_EQUAL") return T::VERSION_EQUAL;
    if (upper == "VERSION_LESS") return T::VERSION_LESS;
    if (upper == "VERSION_GREATER") return T::VERSION_GREATER;
    if (upper == "VERSION_LESS_EQUAL") return T::VERSION_LESS_EQUAL;
    if (upper == "VERSION_GREATER_EQUAL") return T::VERSION_GREATER_EQUAL;
    // Other binary
    if (upper == "MATCHES") return T::MATCHES;
    if (upper == "IN_LIST") return T::IN_LIST;
    if (upper == "IS_NEWER_THAN") return T::IS_NEWER_THAN;
    // Unary
    if (upper == "DEFINED") return T::DEFINED;
    if (upper == "TARGET") return T::TARGET;
    if (upper == "EXISTS") return T::EXISTS;
    if (upper == "IS_DIRECTORY") return T::IS_DIRECTORY;
    if (upper == "IS_ABSOLUTE") return T::IS_ABSOLUTE;
    if (upper == "IS_SYMLINK") return T::IS_SYMLINK;
    if (upper == "COMMAND") return T::COMMAND;
    if (upper == "POLICY") return T::POLICY;
    return std::nullopt;
}

bool is_binary_op(ConditionNode::Type t) {
    using T = ConditionNode::Type;
    switch (t) {
    case T::EQUAL:
    case T::NOT_EQUAL:
    case T::LESS:
    case T::GREATER:
    case T::LESS_EQUAL:
    case T::GREATER_EQUAL:
    case T::STREQUAL:
    case T::STRLESS:
    case T::STRGREATER:
    case T::STRLESS_EQUAL:
    case T::STRGREATER_EQUAL:
    case T::VERSION_EQUAL:
    case T::VERSION_LESS:
    case T::VERSION_GREATER:
    case T::VERSION_LESS_EQUAL:
    case T::VERSION_GREATER_EQUAL:
    case T::MATCHES:
    case T::IN_LIST:
    case T::IS_NEWER_THAN:
        return true;
    default:
        return false;
    }
}

bool is_unary_op(ConditionNode::Type t) {
    using T = ConditionNode::Type;
    switch (t) {
    case T::DEFINED:
    case T::TARGET:
    case T::EXISTS:
    case T::IS_DIRECTORY:
    case T::IS_ABSOLUTE:
    case T::IS_SYMLINK:
    case T::COMMAND:
    case T::POLICY:
        return true;
    default:
        return false;
    }
}

// Recursive descent tree builder.
// Uses an arena (nodes vector) and returns the index of the root node,
// or nullopt on failure.
struct TreeBuilder {
    const std::vector<Argument>& condition;
    const TokenClassifier& classify;
    std::vector<ConditionNode>& nodes;
    size_t pos = 0;
    bool failed = false;

    // Classify the token at current position.
    // Returns the type if it's a keyword, PRIMARY if it's an operand.
    // Sets failed=true and returns nullopt if classification itself fails.
    std::optional<ConditionNode::Type> classify_at(size_t idx) {
        if (idx >= condition.size()) return std::nullopt;
        auto result = classify(condition[idx], idx);
        if (!result.has_value()) {
            failed = true;
            return std::nullopt;
        }
        return *result;
    }

    // Peek at the token type at current position without consuming.
    std::optional<ConditionNode::Type> peek_type() {
        if (pos >= condition.size() || failed) return std::nullopt;
        return classify_at(pos);
    }

    // Check if token at position is a specific keyword type.
    bool is_at(ConditionNode::Type expected) {
        auto t = peek_type();
        return t.has_value() && *t == expected;
    }

    // Parse OR: left-associative, lowest precedence
    std::optional<uint16_t> parse_or() {
        auto left = parse_and();
        if (!left || failed) return std::nullopt;

        while (pos < condition.size() && !failed && is_at(ConditionNode::Type::OR)) {
            pos++; // consume OR
            auto right = parse_and();
            if (!right || failed) return std::nullopt;
            ConditionNode node{ConditionNode::Type::OR, *left, *right};
            *left = static_cast<uint16_t>(nodes.size());
            nodes.push_back(node);
        }
        return left;
    }

    // Parse AND: left-associative
    std::optional<uint16_t> parse_and() {
        auto left = parse_not();
        if (!left || failed) return std::nullopt;

        while (pos < condition.size() && !failed && is_at(ConditionNode::Type::AND)) {
            pos++; // consume AND
            auto right = parse_not();
            if (!right || failed) return std::nullopt;
            ConditionNode node{ConditionNode::Type::AND, *left, *right};
            *left = static_cast<uint16_t>(nodes.size());
            nodes.push_back(node);
        }
        return left;
    }

    // Parse NOT: right-associative, higher precedence than AND/OR
    std::optional<uint16_t> parse_not() {
        if (pos >= condition.size() || failed) return std::nullopt;

        if (is_at(ConditionNode::Type::NOT)) {
            // Check if NOT has a valid operand (not followed by AND/OR/end)
            if (pos + 1 < condition.size()) {
                auto next = classify_at(pos + 1);
                if (failed) return std::nullopt;
                if (next.has_value() && *next != ConditionNode::Type::AND && *next != ConditionNode::Type::OR) {
                    pos++;                      // consume NOT
                    auto operand = parse_not(); // right-associative
                    if (!operand || failed) return std::nullopt;
                    ConditionNode node{ConditionNode::Type::NOT, *operand, 0};
                    auto idx = static_cast<uint16_t>(nodes.size());
                    nodes.push_back(node);
                    return idx;
                }
            }
            // NOT without valid operand: fall through to treat as primary
        }
        return parse_comparison();
    }

    // Parse binary comparison: left operand, then optional binary operator + right operand
    std::optional<uint16_t> parse_comparison() {
        if (pos >= condition.size() || failed) return std::nullopt;

        size_t start_pos = pos;
        auto left = parse_unary_or_primary();
        if (!left || failed) return std::nullopt;

        // Check if next token is a binary operator
        if (pos >= condition.size() || failed) return left;

        auto op_type = peek_type();
        if (!op_type || failed) return left;

        if (is_binary_op(*op_type)) {
            pos++; // consume operator
            if (pos >= condition.size()) {
                failed = true;
                return std::nullopt;
            }

            // For binary ops, left and right store arg indices, not node indices.
            // We need to emit a binary node with the arg indices of the operands.
            // But the left operand was already parsed as a unary/primary node.
            // We need the arg index of the left operand.
            // The left node should be a PRIMARY node; get its arg index.
            auto& left_node = nodes[*left];
            uint16_t left_arg;
            if (left_node.type == ConditionNode::Type::PRIMARY) {
                left_arg = left_node.left;
            } else {
                // Left side of binary op is a complex expression (e.g., unary test).
                // This is unusual but we handle it by failing to pre-parse.
                failed = true;
                return std::nullopt;
            }

            uint16_t right_arg = static_cast<uint16_t>(pos);
            pos++; // consume right operand

            // Replace the left PRIMARY node with the binary node
            // (reuse its slot to avoid orphaned nodes)
            nodes[*left] = ConditionNode{*op_type, left_arg, right_arg};
            return left;
        }

        return left;
    }

    // Parse unary operators and primary values
    std::optional<uint16_t> parse_unary_or_primary() {
        if (pos >= condition.size() || failed) return std::nullopt;

        auto type = peek_type();
        if (!type || failed) return std::nullopt;

        // Parenthesized group
        // Check for literal "(" — parentheses are always bare literals
        if (is_bare_literal(condition[pos]) && get_literal_string(condition[pos]) == "(") {
            pos++; // consume "("
            auto inner = parse_or();
            if (!inner || failed) return std::nullopt;

            // Expect ")"
            if (pos >= condition.size() || !is_bare_literal(condition[pos]) || get_literal_string(condition[pos]) != ")") {
                failed = true;
                return std::nullopt;
            }
            pos++; // consume ")"

            ConditionNode node{ConditionNode::Type::PAREN, *inner, 0};
            auto idx = static_cast<uint16_t>(nodes.size());
            nodes.push_back(node);
            return idx;
        }

        // Unary operators (DEFINED, TARGET, EXISTS, etc.)
        if (is_unary_op(*type) && pos + 1 < condition.size()) {
            auto op_type = *type;
            pos++; // consume unary keyword
            uint16_t operand_arg = static_cast<uint16_t>(pos);
            pos++; // consume operand

            ConditionNode node{op_type, operand_arg, 0};
            auto idx = static_cast<uint16_t>(nodes.size());
            nodes.push_back(node);
            return idx;
        }

        // Primary value (leaf)
        uint16_t arg_idx = static_cast<uint16_t>(pos);
        pos++;

        ConditionNode node{ConditionNode::Type::PRIMARY, arg_idx, 0};
        auto idx = static_cast<uint16_t>(nodes.size());
        nodes.push_back(node);
        return idx;
    }
};

} // anonymous namespace

std::optional<ConditionAST> build_condition_tree(const std::vector<Argument>& condition, const TokenClassifier& classify) {
    if (condition.empty()) {
        return std::nullopt; // Empty condition handled separately
    }

    ConditionAST ast;
    TreeBuilder builder{condition, classify, ast.nodes};

    auto root = builder.parse_or();
    if (!root || builder.failed) { return std::nullopt; }

    // Check that all tokens were consumed
    if (builder.pos < condition.size()) {
        return std::nullopt; // Leftover tokens — can't pre-parse
    }

    return ast;
}

TokenClassifier make_parse_time_classifier() {
    return [](const Argument& arg, size_t /*pos*/) -> std::optional<ConditionNode::Type> {
        // Quoted args are never keywords
        if (arg.quoted) { return ConditionNode::Type::PRIMARY; }

        // Must be a bare string literal (no variable references)
        if (arg.parts.size() != 1 || !std::holds_alternative<std::string>(arg.parts[0])) {
            // Contains variable references — can't determine at parse time
            // whether this is a keyword or operand.
            // Return PRIMARY (operand) if it's in a position where an operand is valid.
            // But if the caller needs a keyword here, it will fail.
            // We return nullopt to signal "can't classify" — the tree builder
            // will only call this in positions where it needs to know.
            // Actually: we always return PRIMARY for non-classifiable args.
            // The tree builder treats PRIMARY as "this is an operand".
            // If this is in an operator position, the tree builder will see PRIMARY
            // and won't match an operator, so it'll treat it as a primary value
            // or fail depending on context.
            return ConditionNode::Type::PRIMARY;
        }

        std::string upper = kiln::to_upper(std::get<std::string>(arg.parts[0]));

        // Check for parentheses (special tokens, not keywords per se)
        if (upper == "(" || upper == ")") {
            return ConditionNode::Type::PRIMARY; // Handled specially by the tree builder
        }

        auto type = keyword_to_type(upper);
        if (type) return *type;

        return ConditionNode::Type::PRIMARY;
    };
}

std::vector<uint16_t> compute_elision_prone_args(const std::vector<Argument>& condition) {
    std::vector<uint16_t> result;
    for (size_t i = 0; i < condition.size(); ++i) {
        const auto& arg = condition[i];
        if (arg.quoted) continue;
        for (const auto& part : arg.parts) {
            if (std::holds_alternative<VariableReference>(part)) {
                result.push_back(static_cast<uint16_t>(i));
                break;
            }
        }
    }
    return result;
}

} // namespace kiln
