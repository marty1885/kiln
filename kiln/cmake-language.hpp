#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <string_view>
#include <variant>
#include <expected>
#include <optional>

namespace kiln {

struct ParseError {
    size_t row;
    size_t col;
    size_t offset;
    size_t length;
    std::string reason;
};

struct CallLocation {
    std::string file;
    size_t row;
    size_t col;
    size_t offset;
    size_t length;
    std::string command;
};

struct VariableReference;

using ArgumentPart = std::variant<std::string, VariableReference>;

struct VariableReference {
    std::string namespace_prefix;         // "", "ENV", "CACHE" (ENV and CACHE for future use)
    std::vector<ArgumentPart> name_parts; // Recursively contains strings and VariableReferences
};

struct Argument {
    std::vector<ArgumentPart> parts;
    bool quoted;
};

enum class ConditionOp : uint8_t {
    Fallback = 0,
    BoolCheck, // if(X)
    Defined,   // if(DEFINED X)
    Target,    // if(TARGET X)
    Exists,    // if(EXISTS X)
    IsDirectory,
    IsAbsolute,
    IsSymlink,
    Command,
    // Binary — numeric
    BinaryEqual,
    BinaryNotEqual,
    BinaryLess,
    BinaryGreater,
    BinaryLessEqual,
    BinaryGreaterEqual,
    // Binary — string
    BinaryStrEqual,
    BinaryStrLess,
    BinaryStrGreater,
    BinaryStrLessEqual,
    BinaryStrGreaterEqual,
    // Binary — version
    BinaryVersionEqual,
    BinaryVersionLess,
    BinaryVersionGreater,
    BinaryVersionLessEqual,
    BinaryVersionGreaterEqual,
    // Binary — other
    BinaryMatches,
    BinaryInList,
    BinaryIsNewerThan,
    // Compound — homogeneous AND/OR chains of simple sub-conditions
    CompoundAnd,
    CompoundOr,
};

struct PreParsedCondition {
    ConditionOp op = ConditionOp::Fallback;
    uint8_t flags = 0; // bit 0: negated, bit 1: has_dynamic_args
    uint8_t left_idx = 0;
    uint8_t right_idx = 0;
    bool negated() const { return flags & 1; }
    bool has_dynamic_args() const { return flags & 2; }

    // Compound AND/OR support: inline array of simple sub-conditions.
    struct SubCondition {
        ConditionOp op;
        uint8_t flags;    // bit 0: negated, bit 1: has_dynamic_args
        uint8_t left_idx; // index into the ORIGINAL condition argument vector
        uint8_t right_idx;
        bool negated() const { return flags & 1; }
        bool has_dynamic_args() const { return flags & 2; }
    };
    static constexpr uint8_t MAX_SUB = 8;
    uint8_t num_sub = 0;
    SubCondition subs[MAX_SUB] = {};
};

struct CommandInvocation;
struct IfBlock;
struct FunctionBlock;
struct MacroBlock;
struct ForeachBlock;
struct WhileBlock;
struct BlockBlock;

using AstNode = std::variant<CommandInvocation, IfBlock, FunctionBlock, MacroBlock, ForeachBlock, WhileBlock, BlockBlock>;

struct ElseIfBlock {
    std::vector<Argument> condition;
    std::vector<AstNode> body;
    PreParsedCondition pre_parsed;
    size_t row = 0;
    size_t col = 0;
    size_t offset = 0;
    size_t length = 0;
};

struct IfBlock {
    std::vector<Argument> condition;
    std::vector<AstNode> then_branch;
    std::vector<ElseIfBlock> elseif_branches;
    std::vector<AstNode> else_branch;
    PreParsedCondition pre_parsed;
    size_t row = 0;
    size_t col = 0;
    size_t offset = 0;
    size_t length = 0;
};

struct FunctionBlock {
    std::string name;       // Empty when name is dynamic; resolved at registration
    Argument name_argument; // Unevaluated name (used when `name` is empty)
    std::vector<std::string> parameters;
    std::vector<AstNode> body;
    std::string definition_file = ""; // File where function was defined
    std::string definition_dir = "";  // Directory where function was defined
};

struct MacroBlock {
    std::string name;       // Empty when name is dynamic; resolved at registration
    Argument name_argument; // Unevaluated name (used when `name` is empty)
    std::vector<std::string> parameters;
    std::vector<AstNode> body;
    std::string definition_file = ""; // File where macro was defined (for consistency)
    std::string definition_dir = "";  // Directory where macro was defined (for consistency)
};

// Foreach loop parameter types
struct ForeachSimple {
    std::vector<Argument> items; // Items to iterate over
};

struct ForeachRange {
    std::optional<Argument> start; // If not provided, defaults to 0
    Argument stop;                 // Required
    std::optional<Argument> step;  // If not provided, defaults to 1
};

struct ForeachIn {
    std::vector<Argument> lists; // Variable names to expand as lists
    std::vector<Argument> items; // Literal items
};

struct ForeachZipLists {
    std::vector<std::string> loop_vars; // Multiple loop variable names
    std::vector<Argument> lists;        // List variable names to zip together
};

struct ForeachBlock {
    Argument loop_var; // The loop variable name (used for simple, range, and in modes)
    std::variant<ForeachSimple, ForeachRange, ForeachIn, ForeachZipLists> params;
    std::vector<AstNode> body;
    size_t row = 0;
    size_t col = 0;
    size_t offset = 0;
    size_t length = 0;
};

struct WhileBlock {
    std::vector<Argument> condition;
    std::vector<AstNode> body;
    PreParsedCondition pre_parsed;
    size_t row = 0;
    size_t col = 0;
    size_t offset = 0;
    size_t length = 0;
};

struct BlockBlock {
    bool scope_for_variables = true;         // CMake default: block() creates a variable scope
    std::vector<std::string> propagate_vars; // Variables to propagate back to parent scope
    std::vector<AstNode> body;
    size_t row = 0;
    size_t col = 0;
    size_t offset = 0;
    size_t length = 0;
};

// Pre-classified math(EXPR ...) shape. Built at parse time when the
// expression is a flat chain of var-refs and decimal literals separated by
// + - * / %. Lets execute_command skip string concatenation, MathEvaluator's
// recursive descent, and the builtin dispatch on the hot path. Same idea as
// PreParsedCondition for if/while.
struct PreParsedMath {
    struct Operand {
        bool is_literal;       // true: use `literal`; false: resolve var ref at `var_part_idx`
        uint16_t var_part_idx; // index into expression Argument's parts
        int64_t literal;
    };
    std::vector<Operand> operands; // operands.size() == ops.size() + 1
    std::vector<char> ops;         // each one of + - * / %
    std::string out_var;           // destination variable name (parse-time literal)
    bool hex_output = false;       // OUTPUT_FORMAT HEXADECIMAL
};

// Pre-classified string(SUBSTRING <input> <begin> <length> <out_var>) shape.
// Recognized when each of input/begin/length is either a parse-time literal
// or a simple bare ${VAR} reference (no nested names, no namespaces), and
// out_var is a parse-time literal. Lets execute_command bypass argument
// expansion, dispatch, parse_number on the index args, and the SUBSTRING
// handler entirely on the dominant case in the matmul bench (1.9M+ calls).
struct PreParsedSubstring {
    // input: either a literal string or a single bare ${VAR}
    bool input_is_var = false;
    std::string input; // value (when !is_var) or var name (when is_var)
    // begin: literal int or single bare ${VAR}
    bool begin_is_var = false;
    int64_t begin_literal = 0;
    std::string begin_var;
    // length: same; -1 means to-end
    bool length_is_var = false;
    int64_t length_literal = 0;
    std::string length_var;
    // out_var name (parse-time literal)
    std::string out_var;
};

struct CommandInvocation {
    std::string identifier;
    std::vector<Argument> arguments;
    size_t row = 0;
    size_t col = 0;
    size_t offset = 0;
    size_t length = 0;
    std::optional<PreParsedMath> pre_parsed_math;           // math() fast path
    std::optional<PreParsedSubstring> pre_parsed_substring; // string(SUBSTRING) fast path
};

// Parse-time classification. Returns nullopt unless the shape fits the
// fast-path subset (defined in math.cpp / string.cpp).
std::optional<PreParsedMath> classify_math(const std::vector<Argument>& args);
std::optional<PreParsedSubstring> classify_substring(const std::vector<Argument>& args);

class Parser {
public:
    explicit Parser(std::string_view content, std::string filename = "");

    std::expected<std::vector<AstNode>, ParseError> parse();

    std::string_view source() const { return content_; }

private:
    std::string_view content_;
    std::string filename_; // Current file being parsed
    size_t pos_ = 0;
    size_t row_ = 1;
    size_t col_ = 1;

    std::expected<std::vector<AstNode>, ParseError> parse_block(const std::vector<std::string>& terminators);
    std::expected<IfBlock, ParseError> parse_if_block(const CommandInvocation& if_command);
    std::expected<FunctionBlock, ParseError> parse_function_block(const CommandInvocation& function_command);
    std::expected<MacroBlock, ParseError> parse_macro_block(const CommandInvocation& macro_command);
    std::expected<ForeachBlock, ParseError> parse_foreach_block(const CommandInvocation& foreach_command);
    std::expected<WhileBlock, ParseError> parse_while_block(const CommandInvocation& while_command);
    std::expected<BlockBlock, ParseError> parse_block_block(const CommandInvocation& block_command);
    void consume_whitespace();
    std::expected<CommandInvocation, ParseError> parse_command_invocation();
    std::expected<CommandInvocation, ParseError> parse_command_body(std::string identifier, size_t cmd_row, size_t cmd_col,
                                                                    size_t cmd_offset);
    std::expected<Argument, ParseError> parse_argument();
    std::expected<std::vector<ArgumentPart>, ParseError> parse_unquoted_argument_value();
    std::expected<std::vector<ArgumentPart>, ParseError> parse_quoted_argument_value();
    std::expected<std::string, ParseError> parse_bracket_argument();
    std::expected<VariableReference, ParseError> parse_variable_reference(bool inside_quotes);
    void check_old_style(const CommandInvocation& cmd, const std::vector<Argument>& expected_args);
};

} // namespace kiln
