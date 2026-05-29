#include "cmake-language.hpp"
#include "condition_evaluator.hpp"
#include "printing.hpp"
#include "utils.hpp"

#include <filesystem>
#include <iostream>

namespace {

// Compute (row, col) of byte `off` in `text`, treating CRLF and LF as line breaks.
std::pair<size_t, size_t> locate(std::string_view text, size_t off) {
    size_t r = 1, c = 1;
    for (size_t i = 0; i < off && i < text.size(); ++i) {
        if (text[i] == '\n') {
            r++;
            c = 1;
        } else
            c++;
    }
    return {r, c};
}

} // namespace

namespace {

inline bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

inline bool is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

inline bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

inline bool is_alnum(char c) {
    return is_alpha(c) || is_digit(c);
}

inline bool is_ident_char(char c) {
    return is_alnum(c) || c == '_';
}

// Case-insensitive ASCII comparison (both sides tolerated)
bool ascii_ci_equal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return false;
    }
    return true;
}

// Check if content at [pos, pos+count) is all '=' characters
bool match_equals(std::string_view content, size_t pos, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (content[pos + i] != '=') return false;
    }
    return true;
}

} // anonymous namespace

namespace kiln {

Parser::Parser(std::string_view content, std::string filename) : content_(content), filename_(std::move(filename)) {}

std::expected<std::vector<AstNode>, ParseError> Parser::parse_block(const std::vector<std::string>& terminators) {
    std::vector<AstNode> ast;
    while (pos_ < content_.length()) {
        consume_whitespace();
        if (pos_ >= content_.length()) break;

        // Save position, then scan identifier to check terminators without
        // a separate peek + re-parse. On the common (non-terminator) path,
        // the consumed identifier is passed directly to parse_command_body.
        size_t saved_pos = pos_, saved_row = row_, saved_col = col_;
        while (pos_ < content_.length() && is_ident_char(content_[pos_])) {
            pos_++;
            col_++;
        }
        std::string_view id_view(content_.data() + saved_pos, pos_ - saved_pos);

        // Check terminators (case-insensitive; terminators are always lowercase)
        bool terminated = false;
        for (const auto& term : terminators) {
            if (ascii_ci_equal(id_view, term)) {
                terminated = true;
                break;
            }
        }
        if (terminated) {
            // Restore so the caller can consume the terminator command
            pos_ = saved_pos;
            row_ = saved_row;
            col_ = saved_col;
            break;
        }

        if (id_view.empty()) {
            // Not an identifier - restore and let parse_command_invocation produce the error
            pos_ = saved_pos;
            row_ = saved_row;
            col_ = saved_col;
            auto command_or_error = parse_command_invocation();
            if (!command_or_error) return std::unexpected(command_or_error.error());
        }

        // Pass pre-consumed identifier to parse the rest of the command
        auto command_or_error = parse_command_body(std::string(id_view), saved_row, saved_col, saved_pos);
        if (!command_or_error) return std::unexpected(command_or_error.error());

        // Dispatch block-structuring keywords (case-insensitive)
        if (ascii_ci_equal(id_view, "if")) {
            auto result = parse_if_block(command_or_error.value());
            if (!result) return std::unexpected(result.error());
            ast.emplace_back(std::move(*result));
        } else if (ascii_ci_equal(id_view, "function")) {
            auto result = parse_function_block(command_or_error.value());
            if (!result) return std::unexpected(result.error());
            ast.emplace_back(std::move(*result));
        } else if (ascii_ci_equal(id_view, "macro")) {
            auto result = parse_macro_block(command_or_error.value());
            if (!result) return std::unexpected(result.error());
            ast.emplace_back(std::move(*result));
        } else if (ascii_ci_equal(id_view, "foreach")) {
            auto result = parse_foreach_block(command_or_error.value());
            if (!result) return std::unexpected(result.error());
            ast.emplace_back(std::move(*result));
        } else if (ascii_ci_equal(id_view, "while")) {
            auto result = parse_while_block(command_or_error.value());
            if (!result) return std::unexpected(result.error());
            ast.emplace_back(std::move(*result));
        } else if (ascii_ci_equal(id_view, "block")) {
            auto result = parse_block_block(command_or_error.value());
            if (!result) return std::unexpected(result.error());
            ast.emplace_back(std::move(*result));
        } else {
            ast.emplace_back(std::move(command_or_error.value()));
        }
    }
    return ast;
}

std::expected<IfBlock, ParseError> Parser::parse_if_block(const CommandInvocation& if_command) {
    IfBlock if_block;
    if_block.condition = if_command.arguments;
    if_block.pre_parsed = classify_condition(if_block.condition);
    if_block.row = if_command.row;
    if_block.col = if_command.col;
    if_block.offset = if_command.offset;

    auto then_branch_or_error = parse_block({"elseif", "else", "endif"});
    if (!then_branch_or_error) { return std::unexpected(then_branch_or_error.error()); }
    if_block.then_branch = std::move(then_branch_or_error.value());

    while (true) {
        // Peek at next keyword without a full save/restore cycle
        consume_whitespace();
        size_t saved_pos = pos_, saved_row = row_, saved_col = col_;
        while (pos_ < content_.length() && is_ident_char(content_[pos_])) {
            pos_++;
            col_++;
        }
        std::string_view kw(content_.data() + saved_pos, pos_ - saved_pos);
        // Restore - let parse_command_invocation re-consume for the full command
        pos_ = saved_pos;
        row_ = saved_row;
        col_ = saved_col;

        if (ascii_ci_equal(kw, "elseif")) {
            auto elseif_command_or_error = parse_command_invocation();
            if (!elseif_command_or_error) return std::unexpected(elseif_command_or_error.error());

            ElseIfBlock elseif_branch;
            elseif_branch.condition = std::move(elseif_command_or_error.value().arguments);
            elseif_branch.pre_parsed = classify_condition(elseif_branch.condition);
            elseif_branch.row = elseif_command_or_error.value().row;
            elseif_branch.col = elseif_command_or_error.value().col;
            elseif_branch.offset = elseif_command_or_error.value().offset;

            auto body_or_error = parse_block({"elseif", "else", "endif"});
            if (!body_or_error) return std::unexpected(body_or_error.error());
            elseif_branch.body = std::move(body_or_error.value());
            elseif_branch.length = pos_ - elseif_branch.offset;

            if_block.elseif_branches.push_back(std::move(elseif_branch));
        } else if (ascii_ci_equal(kw, "else")) {
            auto else_command_or_error = parse_command_invocation();
            if (!else_command_or_error) return std::unexpected(else_command_or_error.error());
            check_old_style(else_command_or_error.value(), if_command.arguments);

            auto else_branch_or_error = parse_block({"endif"});
            if (!else_branch_or_error) return std::unexpected(else_branch_or_error.error());
            if_block.else_branch = std::move(else_branch_or_error.value());
            break;
        } else if (ascii_ci_equal(kw, "endif")) {
            break;
        } else {
            break;
        }
    }

    auto endif_command_or_error = parse_command_invocation(); // consume "endif"
    if (!endif_command_or_error) { return std::unexpected(endif_command_or_error.error()); }
    if (!ascii_ci_equal(endif_command_or_error.value().identifier, "endif")) {
        return std::unexpected(ParseError{endif_command_or_error.value().row, endif_command_or_error.value().col,
                                          endif_command_or_error.value().offset, endif_command_or_error.value().length,
                                          "Expected 'endif'"});
    }
    check_old_style(endif_command_or_error.value(), if_command.arguments);

    if_block.length = pos_ - if_block.offset;
    return if_block;
}

std::expected<FunctionBlock, ParseError> Parser::parse_function_block(const CommandInvocation& function_command) {
    FunctionBlock function_block;

    if (function_command.arguments.empty()) {
        return std::unexpected(ParseError{function_command.row, function_command.col, function_command.offset, function_command.length,
                                          "function() requires a name"});
    }

    // First argument is the function name. If it's purely literal (no
    // variable references), resolve it now; otherwise defer to registration
    // time so e.g. function("${VAR}" ...) works.
    {
        const Argument& name_arg = function_command.arguments[0];
        bool all_literal = !name_arg.parts.empty();
        for (const auto& p : name_arg.parts) {
            if (!std::holds_alternative<std::string>(p)) {
                all_literal = false;
                break;
            }
        }
        if (all_literal) {
            std::string name;
            for (const auto& p : name_arg.parts) name += std::get<std::string>(p);
            function_block.name = std::move(name);
        } else {
            function_block.name_argument = name_arg;
        }
    }

    // Remaining arguments are parameter names
    for (size_t i = 1; i < function_command.arguments.size(); ++i) {
        if (!function_command.arguments[i].parts.empty() && std::holds_alternative<std::string>(function_command.arguments[i].parts[0])) {
            function_block.parameters.push_back(std::get<std::string>(function_command.arguments[i].parts[0]));
        } else {
            return std::unexpected(ParseError{function_command.row, function_command.col, function_command.offset, function_command.length,
                                              "function() parameters must be simple identifiers"});
        }
    }

    // Parse the body
    auto body_or_error = parse_block({"endfunction"});
    if (!body_or_error) { return std::unexpected(body_or_error.error()); }
    function_block.body = std::move(body_or_error.value());

    // Consume endfunction
    auto endfunction_command_or_error = parse_command_invocation();
    if (!endfunction_command_or_error) { return std::unexpected(endfunction_command_or_error.error()); }

    if (!ascii_ci_equal(endfunction_command_or_error.value().identifier, "endfunction")) {
        return std::unexpected(ParseError{row_, col_, pos_, 11, "Expected 'endfunction'"});
    }
    std::vector<Argument> expected_endfunction_args;
    if (!function_command.arguments.empty()) { expected_endfunction_args.push_back(function_command.arguments[0]); }
    check_old_style(endfunction_command_or_error.value(), expected_endfunction_args);

    // Populate definition file metadata
    function_block.definition_file = filename_;
    if (!filename_.empty()) { function_block.definition_dir = std::filesystem::path(filename_).parent_path().string(); }

    return function_block;
}

std::expected<MacroBlock, ParseError> Parser::parse_macro_block(const CommandInvocation& macro_command) {
    MacroBlock macro_block;

    if (macro_command.arguments.empty()) {
        return std::unexpected(
            ParseError{macro_command.row, macro_command.col, macro_command.offset, macro_command.length, "macro() requires a name"});
    }

    // First argument is the macro name. If it's purely literal (no
    // variable references), resolve it now; otherwise defer to registration
    // time so e.g. macro("${VAR}" ...) works.
    {
        const Argument& name_arg = macro_command.arguments[0];
        bool all_literal = !name_arg.parts.empty();
        for (const auto& p : name_arg.parts) {
            if (!std::holds_alternative<std::string>(p)) {
                all_literal = false;
                break;
            }
        }
        if (all_literal) {
            std::string name;
            for (const auto& p : name_arg.parts) name += std::get<std::string>(p);
            macro_block.name = std::move(name);
        } else {
            macro_block.name_argument = name_arg;
        }
    }

    // Remaining arguments are parameter names
    for (size_t i = 1; i < macro_command.arguments.size(); ++i) {
        if (!macro_command.arguments[i].parts.empty() && std::holds_alternative<std::string>(macro_command.arguments[i].parts[0])) {
            macro_block.parameters.push_back(std::get<std::string>(macro_command.arguments[i].parts[0]));
        } else {
            return std::unexpected(ParseError{macro_command.row, macro_command.col, macro_command.offset, macro_command.length,
                                              "macro() parameters must be simple identifiers"});
        }
    }

    // Parse the body
    auto body_or_error = parse_block({"endmacro"});
    if (!body_or_error) { return std::unexpected(body_or_error.error()); }
    macro_block.body = std::move(body_or_error.value());

    // Consume endmacro
    auto endmacro_command_or_error = parse_command_invocation();
    if (!endmacro_command_or_error) { return std::unexpected(endmacro_command_or_error.error()); }

    if (!ascii_ci_equal(endmacro_command_or_error.value().identifier, "endmacro")) {
        return std::unexpected(ParseError{row_, col_, pos_, 8, "Expected 'endmacro'"});
    }
    std::vector<Argument> expected_endmacro_args;
    if (!macro_command.arguments.empty()) { expected_endmacro_args.push_back(macro_command.arguments[0]); }
    check_old_style(endmacro_command_or_error.value(), expected_endmacro_args);

    // Populate definition file metadata
    macro_block.definition_file = filename_;
    if (!filename_.empty()) { macro_block.definition_dir = std::filesystem::path(filename_).parent_path().string(); }

    return macro_block;
}

std::expected<ForeachBlock, ParseError> Parser::parse_foreach_block(const CommandInvocation& foreach_command) {
    ForeachBlock foreach_block;
    foreach_block.row = foreach_command.row;
    foreach_block.col = foreach_command.col;
    foreach_block.offset = foreach_command.offset;

    if (foreach_command.arguments.empty()) {
        return std::unexpected(ParseError{foreach_command.row, foreach_command.col, foreach_command.offset, foreach_command.length,
                                          "foreach() requires a loop variable"});
    }

    // Check if this might be ZIP_LISTS syntax: foreach(<var1> <var2> ... IN ZIP_LISTS ...)
    // Look ahead to find IN keyword and check what follows
    size_t in_position = 0;
    bool has_in_keyword = false;
    for (size_t i = 1; i < foreach_command.arguments.size(); ++i) {
        if (!foreach_command.arguments[i].quoted && foreach_command.arguments[i].parts.size() == 1
            && std::holds_alternative<std::string>(foreach_command.arguments[i].parts[0])) {
            const std::string& keyword = std::get<std::string>(foreach_command.arguments[i].parts[0]);
            // CMake foreach keywords (IN, RANGE, LISTS, ITEMS, ZIP_LISTS) are case-sensitive
            if (keyword == "IN") {
                has_in_keyword = true;
                in_position = i;
                break;
            }
            // Stop at other keywords (RANGE means it's not ZIP_LISTS)
            if (keyword == "RANGE") { break; }
        }
    }

    // Check if ZIP_LISTS follows IN
    bool is_zip_lists = false;
    if (has_in_keyword && in_position + 1 < foreach_command.arguments.size()) {
        if (!foreach_command.arguments[in_position + 1].quoted && foreach_command.arguments[in_position + 1].parts.size() == 1
            && std::holds_alternative<std::string>(foreach_command.arguments[in_position + 1].parts[0])) {
            const std::string& next_keyword = std::get<std::string>(foreach_command.arguments[in_position + 1].parts[0]);
            // CMake foreach keywords are case-sensitive
            if (next_keyword == "ZIP_LISTS") { is_zip_lists = true; }
        }
    }

    // Handle ZIP_LISTS mode
    if (is_zip_lists) {
        ForeachZipLists zip_params;

        // Extract loop variables (all arguments before IN)
        for (size_t i = 0; i < in_position; ++i) {
            if (!foreach_command.arguments[i].parts.empty() && foreach_command.arguments[i].parts.size() == 1
                && std::holds_alternative<std::string>(foreach_command.arguments[i].parts[0])) {
                zip_params.loop_vars.push_back(std::get<std::string>(foreach_command.arguments[i].parts[0]));
            } else {
                return std::unexpected(ParseError{foreach_command.row, foreach_command.col, foreach_command.offset, foreach_command.length,
                                                  "foreach(IN ZIP_LISTS) loop variables must be simple identifiers"});
            }
        }

        if (zip_params.loop_vars.empty()) {
            return std::unexpected(ParseError{foreach_command.row, foreach_command.col, foreach_command.offset, foreach_command.length,
                                              "foreach(IN ZIP_LISTS) requires at least one loop variable"});
        }

        // Collect list arguments after ZIP_LISTS
        for (size_t i = in_position + 2; i < foreach_command.arguments.size(); ++i) {
            zip_params.lists.push_back(foreach_command.arguments[i]);
        }

        if (zip_params.lists.empty()) {
            return std::unexpected(ParseError{foreach_command.row, foreach_command.col, foreach_command.offset, foreach_command.length,
                                              "foreach(IN ZIP_LISTS) requires at least one list"});
        }

        foreach_block.params = zip_params;
        // Set loop_var to first variable for completeness (won't be used in interpreter)
        foreach_block.loop_var = Argument{{zip_params.loop_vars[0]}, false};

    } else {
        // Original logic for non-ZIP_LISTS modes
        // Store the full loop variable argument (may contain variable references like _${PREFIX}_VAR)
        foreach_block.loop_var = foreach_command.arguments[0];

        // Determine mode by checking second argument (if present)
        // CMake foreach keywords (IN, RANGE, LISTS, ITEMS, ZIP_LISTS) are case-sensitive
        std::string mode_keyword;
        if (foreach_command.arguments.size() > 1 && !foreach_command.arguments[1].quoted && foreach_command.arguments[1].parts.size() == 1
            && std::holds_alternative<std::string>(foreach_command.arguments[1].parts[0])) {
            mode_keyword = std::get<std::string>(foreach_command.arguments[1].parts[0]);
        }

        if (mode_keyword == "RANGE") {
            // RANGE mode: foreach(i RANGE <stop>) or foreach(i RANGE <start> <stop> [<step>])
            if (foreach_command.arguments.size() < 3) {
                return std::unexpected(ParseError{foreach_command.row, foreach_command.col, foreach_command.offset, foreach_command.length,
                                                  "foreach(RANGE) requires at least a stop value"});
            }
            if (foreach_command.arguments.size() > 5) {
                return std::unexpected(ParseError{foreach_command.row, foreach_command.col, foreach_command.offset, foreach_command.length,
                                                  "foreach(RANGE) accepts at most 3 range values (start, stop, step)"});
            }

            ForeachRange range;
            if (foreach_command.arguments.size() == 3) {
                // RANGE <stop>
                range.stop = foreach_command.arguments[2];
            } else if (foreach_command.arguments.size() == 4) {
                // RANGE <start> <stop>
                range.start = foreach_command.arguments[2];
                range.stop = foreach_command.arguments[3];
            } else { // size == 5
                // RANGE <start> <stop> <step>
                range.start = foreach_command.arguments[2];
                range.stop = foreach_command.arguments[3];
                range.step = foreach_command.arguments[4];
            }
            foreach_block.params = range;

        } else if (mode_keyword == "IN") {
            // IN mode: foreach(i IN [LISTS [<lists>]] [ITEMS [<items>]])
            ForeachIn in_params;

            size_t idx = 2; // Start after "IN"
            while (idx < foreach_command.arguments.size()) {
                std::string keyword;
                if (!foreach_command.arguments[idx].quoted && foreach_command.arguments[idx].parts.size() == 1
                    && std::holds_alternative<std::string>(foreach_command.arguments[idx].parts[0])) {
                    keyword = std::get<std::string>(foreach_command.arguments[idx].parts[0]);
                }

                if (keyword == "LISTS") {
                    // Collect list variable names until we hit ITEMS or end
                    idx++;
                    while (idx < foreach_command.arguments.size()) {
                        // Check if this is the ITEMS keyword
                        std::string next_keyword;
                        if (!foreach_command.arguments[idx].quoted && foreach_command.arguments[idx].parts.size() == 1
                            && std::holds_alternative<std::string>(foreach_command.arguments[idx].parts[0])) {
                            next_keyword = std::get<std::string>(foreach_command.arguments[idx].parts[0]);
                        }
                        if (next_keyword == "ITEMS") {
                            break; // Stop collecting, let the outer loop handle ITEMS
                        }
                        in_params.lists.push_back(foreach_command.arguments[idx]);
                        idx++;
                    }
                } else if (keyword == "ITEMS") {
                    // Collect item values until end
                    idx++;
                    while (idx < foreach_command.arguments.size()) {
                        in_params.items.push_back(foreach_command.arguments[idx]);
                        idx++;
                    }
                } else {
                    return std::unexpected(ParseError{foreach_command.row, foreach_command.col, foreach_command.offset,
                                                      foreach_command.length, "foreach(IN) expected LISTS or ITEMS keyword"});
                }
            }

            // Empty ITEMS/LISTS is valid in CMake - results in zero iterations

            foreach_block.params = in_params;

        } else {
            // Simple mode: foreach(i item1 item2 ...)
            // All arguments from index 1 onwards are items.
            // NOTE: CMake allows foreach(var) with zero items — the body is never executed.
            // An optimizer could detect empty item lists at parse time and skip body parsing,
            // but the body must still be parsed for correctness (matching endforeach).
            ForeachSimple simple;
            simple.items.insert(simple.items.end(), foreach_command.arguments.begin() + 1, foreach_command.arguments.end());
            foreach_block.params = simple;
        }
    }

    // Parse the body
    auto body_or_error = parse_block({"endforeach"});
    if (!body_or_error) { return std::unexpected(body_or_error.error()); }
    foreach_block.body = std::move(body_or_error.value());

    // Consume endforeach
    auto endforeach_command_or_error = parse_command_invocation();
    if (!endforeach_command_or_error) { return std::unexpected(endforeach_command_or_error.error()); }

    if (!ascii_ci_equal(endforeach_command_or_error.value().identifier, "endforeach")) {
        return std::unexpected(ParseError{row_, col_, pos_, 10, "Expected 'endforeach'"});
    }
    std::vector<Argument> expected_endforeach_args;
    if (!foreach_command.arguments.empty()) { expected_endforeach_args.push_back(foreach_command.arguments[0]); }
    check_old_style(endforeach_command_or_error.value(), expected_endforeach_args);

    foreach_block.length = pos_ - foreach_block.offset;
    return foreach_block;
}

std::expected<WhileBlock, ParseError> Parser::parse_while_block(const CommandInvocation& while_command) {
    WhileBlock while_block;
    while_block.row = while_command.row;
    while_block.col = while_command.col;
    while_block.offset = while_command.offset;
    while_block.condition = while_command.arguments;
    while_block.pre_parsed = classify_condition(while_block.condition);

    // Parse the body
    auto body_or_error = parse_block({"endwhile"});
    if (!body_or_error) { return std::unexpected(body_or_error.error()); }
    while_block.body = std::move(body_or_error.value());

    // Consume endwhile
    auto endwhile_command_or_error = parse_command_invocation();
    if (!endwhile_command_or_error) { return std::unexpected(endwhile_command_or_error.error()); }

    if (!ascii_ci_equal(endwhile_command_or_error.value().identifier, "endwhile")) {
        return std::unexpected(ParseError{row_, col_, pos_, 8, "Expected 'endwhile'"});
    }
    check_old_style(endwhile_command_or_error.value(), while_command.arguments);

    while_block.length = pos_ - while_block.offset;
    return while_block;
}

std::expected<BlockBlock, ParseError> Parser::parse_block_block(const CommandInvocation& block_command) {
    BlockBlock block_block;
    block_block.row = block_command.row;
    block_block.col = block_command.col;
    block_block.offset = block_command.offset;
    block_block.scope_for_variables = true; // CMake default: block() creates a variable scope

    // Parse optional arguments: [SCOPE_FOR VARIABLES] [PROPAGATE var1 var2...]
    size_t idx = 0;
    while (idx < block_command.arguments.size()) {
        std::string_view keyword;
        if (!block_command.arguments[idx].quoted && block_command.arguments[idx].parts.size() == 1
            && std::holds_alternative<std::string>(block_command.arguments[idx].parts[0])) {
            keyword = std::get<std::string>(block_command.arguments[idx].parts[0]);
        }

        if (ascii_ci_equal(keyword, "SCOPE_FOR")) {
            // When SCOPE_FOR is explicitly specified, switch from default (both scopes)
            // to only what's explicitly requested
            block_block.scope_for_variables = false; // Will be set true if VARIABLES is specified

            idx++;
            // Next arguments should be VARIABLES and/or POLICIES
            while (idx < block_command.arguments.size()) {
                std::string_view scope_keyword;
                if (!block_command.arguments[idx].quoted && block_command.arguments[idx].parts.size() == 1
                    && std::holds_alternative<std::string>(block_command.arguments[idx].parts[0])) {
                    scope_keyword = std::get<std::string>(block_command.arguments[idx].parts[0]);
                }

                if (ascii_ci_equal(scope_keyword, "VARIABLES")) {
                    block_block.scope_for_variables = true;
                    idx++;
                } else if (ascii_ci_equal(scope_keyword, "POLICIES")) {
                    // Policies not supported, but we accept for compatibility
                    idx++;
                } else {
                    // Not VARIABLES or POLICIES - break to outer loop
                    break;
                }
            }
            // Continue to check for PROPAGATE
            continue;
        } else if (ascii_ci_equal(keyword, "PROPAGATE")) {
            // PROPAGATE implies variable scoping
            block_block.scope_for_variables = true;
            // Collect all remaining arguments as variable names to propagate
            idx++;
            while (idx < block_command.arguments.size()) {
                if (!block_command.arguments[idx].parts.empty()
                    && std::holds_alternative<std::string>(block_command.arguments[idx].parts[0])) {
                    block_block.propagate_vars.push_back(std::get<std::string>(block_command.arguments[idx].parts[0]));
                } else {
                    return std::unexpected(ParseError{block_command.row, block_command.col, block_command.offset, block_command.length,
                                                      "block(PROPAGATE) variable names must be simple identifiers"});
                }
                idx++;
            }
            break; // PROPAGATE consumes all remaining arguments
        } else {
            return std::unexpected(ParseError{block_command.row, block_command.col, block_command.offset, block_command.length,
                                              "block() expected SCOPE_FOR or PROPAGATE keyword"});
        }
    }

    // Parse the body
    auto body_or_error = parse_block({"endblock"});
    if (!body_or_error) { return std::unexpected(body_or_error.error()); }
    block_block.body = std::move(body_or_error.value());

    // Consume endblock
    auto endblock_command_or_error = parse_command_invocation();
    if (!endblock_command_or_error) { return std::unexpected(endblock_command_or_error.error()); }

    if (!ascii_ci_equal(endblock_command_or_error.value().identifier, "endblock")) {
        return std::unexpected(ParseError{row_, col_, pos_, 8, "Expected 'endblock'"});
    }

    block_block.length = pos_ - block_block.offset;
    return block_block;
}

std::expected<std::vector<AstNode>, ParseError> Parser::parse() {
    // BOM handling: accept UTF-8, reject UTF-16/UTF-32 with a clear message
    // instead of letting them cascade into "expected identifier" noise.
    if (content_.length() >= 2) {
        auto b0 = static_cast<unsigned char>(content_[0]);
        auto b1 = static_cast<unsigned char>(content_[1]);
        if (content_.length() >= 3 && b0 == 0xEF && b1 == 0xBB && static_cast<unsigned char>(content_[2]) == 0xBF) {
            pos_ = 3;
        } else if (b0 == 0xFF && b1 == 0xFE) {
            if (content_.length() >= 4 && content_[2] == 0 && content_[3] == 0) {
                return std::unexpected(ParseError{1, 1, 0, 4, "File starts with a UTF-32 LE byte-order mark; only UTF-8 is supported"});
            }
            return std::unexpected(ParseError{1, 1, 0, 2, "File starts with a UTF-16 LE byte-order mark; only UTF-8 is supported"});
        } else if (b0 == 0xFE && b1 == 0xFF) {
            return std::unexpected(ParseError{1, 1, 0, 2, "File starts with a UTF-16 BE byte-order mark; only UTF-8 is supported"});
        } else if (content_.length() >= 4 && b0 == 0 && b1 == 0 && static_cast<unsigned char>(content_[2]) == 0xFE
                   && static_cast<unsigned char>(content_[3]) == 0xFF) {
            return std::unexpected(ParseError{1, 1, 0, 4, "File starts with a UTF-32 BE byte-order mark; only UTF-8 is supported"});
        }
    }

    // Reject embedded NUL bytes; they almost always indicate a binary file
    // misfed to the parser, and silently truncating identifiers downstream
    // produces baffling errors. One up-front scan is cheaper than the parse.
    if (size_t nul = content_.find('\0'); nul != std::string_view::npos) {
        auto [r, c] = locate(content_, nul);
        return std::unexpected(ParseError{r, c, nul, 1, "Null byte in source file (binary content?)"});
    }

    return parse_block({});
}

void Parser::consume_whitespace() {
    while (pos_ < content_.length()) {
        if (is_space(content_[pos_])) {
            if (content_[pos_] == '\n') {
                row_++;
                col_ = 1;
            } else {
                col_++;
            }
            pos_++;
        } else if (content_[pos_] == '#') {
            if (pos_ + 1 < content_.length() && content_[pos_ + 1] == '[') {
                // Bracket comment
                pos_ += 2; // #[
                col_ += 2;

                size_t equals_count = 0;
                while (pos_ < content_.length() && content_[pos_] == '=') {
                    equals_count++;
                    pos_++;
                    col_++;
                }

                if (pos_ >= content_.length() || content_[pos_] != '[') {
                    // Not a valid bracket comment, treat as line comment
                    while (pos_ < content_.length() && content_[pos_] != '\n') {
                        pos_++;
                        col_++;
                    }
                } else {
                    pos_++; // [
                    col_++;

                    while (pos_ + equals_count + 1 < content_.length()) {
                        if (content_[pos_] == ']' && match_equals(content_, pos_ + 1, equals_count)
                            && content_[pos_ + equals_count + 1] == ']') {
                            pos_ += equals_count + 2;
                            col_ += equals_count + 2;
                            break;
                        }
                        if (content_[pos_] == '\n') {
                            row_++;
                            col_ = 1;
                        } else {
                            col_++;
                        }
                        pos_++;
                    }
                }
            } else {
                // Line comment
                while (pos_ < content_.length() && content_[pos_] != '\n') {
                    pos_++;
                    col_++;
                }
            }
        } else {
            break;
        }
    }
}

std::expected<CommandInvocation, ParseError> Parser::parse_command_invocation() {
    consume_whitespace();

    size_t cmd_row = row_;
    size_t cmd_col = col_;
    size_t cmd_offset = pos_;

    // Parse identifier
    size_t start_pos = pos_;
    while (pos_ < content_.length() && is_ident_char(content_[pos_])) {
        pos_++;
        col_++;
    }
    std::string identifier(content_.substr(start_pos, pos_ - start_pos));

    if (identifier.empty()) { return std::unexpected(ParseError{row_, col_, pos_, 0, "Expected an identifier"}); }

    return parse_command_body(std::move(identifier), cmd_row, cmd_col, cmd_offset);
}

std::expected<CommandInvocation, ParseError> Parser::parse_command_body(std::string identifier, size_t cmd_row, size_t cmd_col,
                                                                        size_t cmd_offset) {

    consume_whitespace();

    // Parse opening parenthesis
    if (pos_ >= content_.length() || content_[pos_] != '(') {
        return std::unexpected(ParseError{row_, col_, pos_, 1, "Expected '(' after identifier"});
    }
    size_t open_paren_row = row_;
    size_t open_paren_col = col_;
    size_t open_paren_offset = pos_;
    pos_++;
    col_++;

    CommandInvocation cmd_inv{std::move(identifier), {}, cmd_row, cmd_col, cmd_offset, 0};

    // Parse arguments, tracking genex balance across all arguments.
    // Multi-line genex like $<$<BOOL:TRUE>:\n-Wall\n> split into fragments
    // that are individually unbalanced but balanced across the command.
    int nesting = 0;
    int genex_depth = 0;
    size_t genex_start_row = 0, genex_start_col = 0, genex_start_offset = 0;

    // Separation tracking. CMake warns when two arguments are not separated
    // by whitespace ("foo""bar"), and errors when a bracket argument touches
    // any neighbor. Paren-as-arg tokens (when nesting > 0) don't require
    // separation from their neighbors - they're structural.
    enum class ArgKind { Unquoted, Quoted, Bracket, Paren };
    ArgKind prev_kind = ArgKind::Unquoted;
    bool has_prev = false;

    while (pos_ < content_.length()) {
        size_t ws_start = pos_;
        consume_whitespace();
        bool whitespace_seen = pos_ > ws_start;
        if (pos_ >= content_.length()) {
            break; // EOF reached - will be caught by closing paren check below
        }
        if (content_[pos_] == ')' && nesting == 0) { break; }

        // Record position before parsing this argument (for genex tracking)
        size_t arg_start_row = row_, arg_start_col = col_, arg_start_offset = pos_;

        // Classify the upcoming argument so we can diagnose adjacent tokens
        // before parsing consumes them.
        ArgKind cur_kind;
        {
            char ch = content_[pos_];
            if (ch == '"')
                cur_kind = ArgKind::Quoted;
            else if (ch == '[' && pos_ + 1 < content_.length() && (content_[pos_ + 1] == '[' || content_[pos_ + 1] == '='))
                cur_kind = ArgKind::Bracket;
            else if (ch == '(' || ch == ')')
                cur_kind = ArgKind::Paren;
            else
                cur_kind = ArgKind::Unquoted;
        }

        if (has_prev && !whitespace_seen && prev_kind != ArgKind::Paren && cur_kind != ArgKind::Paren) {
            bool is_error = (prev_kind == ArgKind::Bracket || cur_kind == ArgKind::Bracket);
            std::string msg = "Argument not separated from preceding token by whitespace";
            if (is_error) { return std::unexpected(ParseError{arg_start_row, arg_start_col, arg_start_offset, 1, std::move(msg)}); }
            kiln::print_diagnostic(std::cerr, DiagnosticSeverity::Warning, msg, filename_, arg_start_row, arg_start_col, arg_start_offset,
                                   1, {}, std::string(content_), "");
        }

        auto arg_or_error = parse_argument();
        if (arg_or_error) {
            const auto& arg = arg_or_error.value();
            if (!arg.quoted && arg.parts.size() == 1 && std::holds_alternative<std::string>(arg.parts[0])) {
                const auto& s = std::get<std::string>(arg.parts[0]);
                if (s == "(")
                    nesting++;
                else if (s == ")")
                    nesting--;
            }

            // Track genex balance across all unquoted arguments
            if (!arg.quoted) {
                for (const auto& part : arg.parts) {
                    if (auto* lit = std::get_if<std::string>(&part)) {
                        for (size_t i = 0; i < lit->size(); ++i) {
                            if ((*lit)[i] == '$' && i + 1 < lit->size() && (*lit)[i + 1] == '<') {
                                if (genex_depth == 0) {
                                    genex_start_row = arg_start_row;
                                    genex_start_col = arg_start_col;
                                    genex_start_offset = arg_start_offset;
                                }
                                genex_depth++;
                                i++; // skip '<'
                            } else if ((*lit)[i] == '>' && genex_depth > 0) {
                                genex_depth--;
                            }
                        }
                    }
                }
            }

            cmd_inv.arguments.push_back(std::move(arg_or_error.value()));
            prev_kind = cur_kind;
            has_prev = true;
        } else {
            return std::unexpected(arg_or_error.error());
        }
    }

    // Warn about truly unbalanced genex (unbalanced across entire command)
    if (genex_depth > 0) {
        kiln::print_diagnostic(std::cerr, DiagnosticSeverity::Warning, "Unbalanced generator expression (missing closing '>')", filename_,
                               genex_start_row, genex_start_col, genex_start_offset, pos_ - genex_start_offset, {}, std::string(content_),
                               "Accepting as-is - CMake treats generator expressions as strings at parse time.");
    }

    // Parse closing parenthesis
    if (pos_ >= content_.length() || content_[pos_] != ')') {
        return std::unexpected(
            ParseError{open_paren_row, open_paren_col, open_paren_offset, 1, "This '(' is not closed before end of file"});
    }
    pos_++;
    col_++;

    cmd_inv.length = pos_ - cmd_offset;

    // Parse-time fast-path classification. Each classifier returns nullopt
    // unless the invocation matches its tight recognized shape; runtime then
    // skips arg expansion, dispatch, and the per-builtin parser overhead.
    {
        const auto& id = cmd_inv.identifier;
        if (id.size() == 4 && (id[0] == 'm' || id[0] == 'M') && (id[1] == 'a' || id[1] == 'A') && (id[2] == 't' || id[2] == 'T')
            && (id[3] == 'h' || id[3] == 'H')) {
            cmd_inv.pre_parsed_math = classify_math(cmd_inv.arguments);
        } else if (id.size() == 6 && (id[0] == 's' || id[0] == 'S') && (id[1] == 't' || id[1] == 'T') && (id[2] == 'r' || id[2] == 'R')
                   && (id[3] == 'i' || id[3] == 'I') && (id[4] == 'n' || id[4] == 'N') && (id[5] == 'g' || id[5] == 'G')) {
            cmd_inv.pre_parsed_substring = classify_substring(cmd_inv.arguments);
        }
    }

    return cmd_inv;
}

std::expected<Argument, ParseError> Parser::parse_argument() {
    if (pos_ >= content_.length()) { return std::unexpected(ParseError{row_, col_, pos_, 0, "Unexpected end of input"}); }

    if (content_[pos_] == '"') {
        auto value_or_error = parse_quoted_argument_value();
        if (!value_or_error) { return std::unexpected(value_or_error.error()); }
        return Argument{std::move(value_or_error.value()), true};
    } else if (content_[pos_] == '[') {
        // Check if this is a bracket argument ([[...]] or [=[...]=]) or a registry key ([HKEY_...])
        // Bracket arguments require '[' or '=' after the opening '['
        if (pos_ + 1 < content_.length() && (content_[pos_ + 1] == '[' || content_[pos_ + 1] == '=')) {
            auto value_or_error = parse_bracket_argument();
            if (!value_or_error) { return std::unexpected(value_or_error.error()); }
            return Argument{{std::move(value_or_error.value())}, true};
        }
        // Check for Windows registry key syntax: [HKEY_...]
        // On Linux, these resolve to empty strings since there's no registry
        if (pos_ + 5 < content_.length() && content_.substr(pos_ + 1, 5) == "HKEY_") {
            // Consume everything up to and including the closing ']'
            size_t end_pos = content_.find(']', pos_);
            if (end_pos != std::string::npos) {
                // Skip the entire registry key including brackets
                size_t len = end_pos - pos_ + 1;
                pos_ = end_pos + 1;
                col_ += len;
                // Return empty - registry keys resolve to nothing on non-Windows
                return Argument{{std::string("")}, false};
            }
        }
        // Otherwise treat as unquoted argument
        auto value_or_error = parse_unquoted_argument_value();
        if (!value_or_error) { return std::unexpected(value_or_error.error()); }
        return Argument{std::move(value_or_error.value()), false};
    } else {
        auto value_or_error = parse_unquoted_argument_value();
        if (!value_or_error) { return std::unexpected(value_or_error.error()); }
        return Argument{std::move(value_or_error.value()), false};
    }
}

std::expected<std::vector<ArgumentPart>, ParseError> Parser::parse_unquoted_argument_value() {
    std::vector<ArgumentPart> parts;
    size_t start_pos = pos_;

    // Handle '(' or ')' as single-character unquoted arguments.
    // In CMake, these are separate tokens in if() conditions.
    if (pos_ < content_.length() && (content_[pos_] == '(' || content_[pos_] == ')')) {
        parts.emplace_back(std::string(1, content_[pos_]));
        pos_++;
        col_++;
        return parts;
    }

    std::string current_literal;

    while (pos_ < content_.length()) {
        char current = content_[pos_];

        // Stop at whitespace, parens, or comments.
        // CMake ALWAYS splits on whitespace, even inside generator expressions.
        // Multi-line genex become separate arguments joined with semicolons by commands.
        if (is_space(current) || current == '(' || current == ')' || current == '#') { break; }

        // Continue with normal parsing logic for special constructs
        // Handle embedded quoted strings within unquoted arguments
        // E.g., LAGRANGE_APP_VERSION="${PROJECT_VERSION}" is one argument
        if (content_[pos_] == '"') {
            // Save accumulated literal before the quote
            if (start_pos < pos_) { current_literal += content_.substr(start_pos, pos_ - start_pos); }
            if (!current_literal.empty()) {
                parts.emplace_back(std::move(current_literal));
                current_literal.clear();
            }

            // Parse the quoted part
            auto quoted_value = parse_quoted_argument_value();
            if (!quoted_value) { return std::unexpected(quoted_value.error()); }
            // Embedded quotes in unquoted args are preserved as literal characters.
            // This is needed for compile definitions like MY_VERSION="${VAR}" which
            // should become -DMY_VERSION="value" (quotes are C string delimiters).
            // For COMMAND args, the build system strips these quotes at execution time.
            parts.emplace_back(std::string("\""));
            for (auto& part : *quoted_value) { parts.emplace_back(std::move(part)); }
            parts.emplace_back(std::string("\""));

            start_pos = pos_;
            continue;
        }

        // Handle bracket arguments embedded in unquoted arguments
        if (content_[pos_] == '[' && pos_ + 1 < content_.length() && (content_[pos_ + 1] == '[' || content_[pos_ + 1] == '=')) {
            // Save accumulated literal
            if (start_pos < pos_) { current_literal += content_.substr(start_pos, pos_ - start_pos); }
            if (!current_literal.empty()) {
                parts.emplace_back(std::move(current_literal));
                current_literal.clear();
            }

            // Parse bracket argument
            auto bracket_value = parse_bracket_argument();
            if (!bracket_value) { return std::unexpected(bracket_value.error()); }
            parts.emplace_back(std::move(*bracket_value));

            start_pos = pos_;
            continue;
        }
        if (content_[pos_] == '\\' && pos_ + 1 < content_.length()) {
            // Save any accumulated text before the escape
            if (start_pos < pos_) { current_literal += content_.substr(start_pos, pos_ - start_pos); }

            // Process escape sequence
            pos_++; // Skip backslash
            col_++;
            char escaped_char = content_[pos_];

            // Interpret CMake escape sequences
            switch (escaped_char) {
            case 'n':
                current_literal += '\n';
                break;
            case 't':
                current_literal += '\t';
                break;
            case 'r':
                current_literal += '\r';
                break;
            case '\\':
                current_literal += '\\';
                break;
            case '"':
                current_literal += '"';
                break;
            case ')':
                current_literal += ')';
                break;
            case '(':
                current_literal += '(';
                break;
            case '#':
                current_literal += '#';
                break;
            case ' ':
                current_literal += ' ';
                break;
            case ';':
                current_literal += '\\';
                current_literal += ';';
                break;
            case '\n':
                // Backslash-newline continues the line
                row_++;
                col_ = 1;
                pos_++;
                start_pos = pos_;
                continue;
            default:
                // Unknown escape, keep the backslash
                current_literal += '\\';
                current_literal += escaped_char;
                break;
            }

            pos_++;
            if (escaped_char != '\n') { col_++; }
            start_pos = pos_;
            continue;
        }
        // Check for variable reference: ${VAR}, $ENV{VAR}, or $CACHE{VAR}
        if (content_[pos_] == '$' && pos_ + 1 < content_.length()) {
            bool is_var_ref = false;

            // Check for ${
            if (content_[pos_ + 1] == '{') {
                is_var_ref = true;
            }
            // Check for $ENV{ or $CACHE{
            else if (is_alpha(content_[pos_ + 1]) || content_[pos_ + 1] == '_') {
                // Peek ahead to see if this is ENV{ or CACHE{
                size_t peek_pos = pos_ + 1;
                while (peek_pos < content_.length() && is_ident_char(content_[peek_pos])) { peek_pos++; }

                // Check if the identifier is followed by '{'
                if (peek_pos < content_.length() && content_[peek_pos] == '{') {
                    std::string_view prefix(content_.data() + pos_ + 1, peek_pos - pos_ - 1);
                    if (ascii_ci_equal(prefix, "ENV") || ascii_ci_equal(prefix, "CACHE")) { is_var_ref = true; }
                }
            }

            if (is_var_ref) {
                // Save accumulated text (from both unescaped and escaped content)
                if (start_pos < pos_) { current_literal += content_.substr(start_pos, pos_ - start_pos); }
                if (!current_literal.empty()) {
                    parts.emplace_back(std::move(current_literal));
                    current_literal.clear();
                }

                pos_++; // Move past '$'
                col_++;

                auto var_ref = parse_variable_reference(false);
                if (!var_ref) { return std::unexpected(var_ref.error()); }
                parts.emplace_back(std::move(var_ref.value()));
                start_pos = pos_;
                continue;
            }
        }

        // Handle newlines in position tracking
        if (content_[pos_] == '\n') {
            row_++;
            col_ = 1;
        } else {
            col_++;
        }
        pos_++;
    }

    // Collect any remaining text
    if (start_pos < pos_) { current_literal += content_.substr(start_pos, pos_ - start_pos); }
    if (!current_literal.empty()) { parts.emplace_back(std::move(current_literal)); }

    if (parts.empty()) { return std::unexpected(ParseError{row_, col_, pos_, 0, "Expected an unquoted argument"}); }

    return parts;
}

std::expected<std::vector<ArgumentPart>, ParseError> Parser::parse_quoted_argument_value() {
    size_t quote_start = pos_;
    pos_++; // Consume opening quote
    col_++;

    std::vector<ArgumentPart> parts;
    std::string current_literal;
    bool escaped = false;

    while (pos_ < content_.length()) {
        char current = content_[pos_];
        if (escaped) {
            // Interpret CMake escape sequences
            switch (current) {
            case '\n':
                // Line continuation: backslash-newline is dropped entirely
                // (CMake collapses \ + newline, keeping any leading whitespace
                // on the continuation line as-is)
                break;
            case 'n':
                current_literal += '\n';
                break;
            case 't':
                current_literal += '\t';
                break;
            case 'r':
                current_literal += '\r';
                break;
            case '\\':
                current_literal += '\\';
                break;
            case '"':
                current_literal += '"';
                break;
            case ')':
                current_literal += ')';
                break;
            case '(':
                current_literal += '(';
                break;
            case '#':
                current_literal += '#';
                break;
            case ' ':
                current_literal += ' ';
                break;
            case ';':
                current_literal += '\\';
                current_literal += ';';
                break;
            default:
                current_literal += current;
                break; // Unknown escape, keep as-is
            }
            escaped = false;
            pos_++;
            if (current == '\n') {
                row_++;
                col_ = 1;
            } else {
                col_++;
            }
            continue;
        }
        if (current == '\\') {
            escaped = true;
            pos_++;
            col_++;
            continue;
        }
        if (current == '"') {
            pos_++; // Consume closing quote
            col_++;
            if (!current_literal.empty()) { parts.emplace_back(current_literal); }
            return parts;
        }
        // Check for variable reference: ${VAR}, $ENV{VAR}, or $CACHE{VAR}
        if (current == '$' && pos_ + 1 < content_.length()) {
            bool is_var_ref = false;

            // Check for ${
            if (content_[pos_ + 1] == '{') {
                is_var_ref = true;
            }
            // Check for $ENV{ or $CACHE{
            else if (is_alpha(content_[pos_ + 1]) || content_[pos_ + 1] == '_') {
                // Peek ahead to see if this is ENV{ or CACHE{
                size_t peek_pos = pos_ + 1;
                while (peek_pos < content_.length() && is_ident_char(content_[peek_pos])) { peek_pos++; }

                // Check if the identifier is followed by '{'
                if (peek_pos < content_.length() && content_[peek_pos] == '{') {
                    std::string_view prefix(content_.data() + pos_ + 1, peek_pos - pos_ - 1);
                    if (ascii_ci_equal(prefix, "ENV") || ascii_ci_equal(prefix, "CACHE")) { is_var_ref = true; }
                }
            }

            if (is_var_ref) {
                if (!current_literal.empty()) {
                    parts.emplace_back(current_literal);
                    current_literal.clear();
                }

                pos_++; // Move past '$'
                col_++;

                auto var_ref = parse_variable_reference(true);
                if (!var_ref) { return std::unexpected(var_ref.error()); }
                parts.emplace_back(std::move(var_ref.value()));
                continue;
            }
        }

        // CRLF normalization: drop '\r' when it's the first half of '\r\n'.
        // Windows checkouts otherwise smuggle stray CR into string literals.
        if (current == '\r' && pos_ + 1 < content_.length() && content_[pos_ + 1] == '\n') {
            pos_++;
            // col not advanced; the '\n' branch below will reset it
            continue;
        }

        current_literal += current;
        pos_++;
        if (current == '\n') {
            row_++;
            col_ = 1;
        } else {
            col_++;
        }
    }

    return std::unexpected(ParseError{row_, col_, quote_start, pos_ - quote_start, "Unterminated quoted argument"});
}

std::expected<std::string, ParseError> Parser::parse_bracket_argument() {
    size_t start_pos_outer = pos_;
    pos_++; // Consume opening bracket
    col_++;

    size_t equals_count = 0;
    while (pos_ < content_.length() && content_[pos_] == '=') {
        equals_count++;
        pos_++;
        col_++;
    }

    if (pos_ >= content_.length() || content_[pos_] != '[') {
        return std::unexpected(ParseError{row_, col_, start_pos_outer, pos_ - start_pos_outer, "Invalid bracket argument"});
    }
    pos_++; // Consume opening bracket
    col_++;

    size_t start_pos = pos_;
    while (pos_ + equals_count + 1 < content_.length()) {
        if (content_[pos_] == ']' && match_equals(content_, pos_ + 1, equals_count) && content_[pos_ + equals_count + 1] == ']') {
            std::string_view raw = content_.substr(start_pos, pos_ - start_pos);
            std::string value;
            // CRLF normalization: collapse '\r\n' -> '\n'. Reserve to skip
            // reallocation; the common case has no '\r' so we just copy.
            value.reserve(raw.size());
            for (size_t i = 0; i < raw.size(); ++i) {
                if (raw[i] == '\r' && i + 1 < raw.size() && raw[i + 1] == '\n') continue;
                value += raw[i];
            }
            pos_ += equals_count + 2;
            col_ += equals_count + 2;
            return value;
        }
        if (content_[pos_] == '\n') {
            row_++;
            col_ = 1;
        } else {
            col_++;
        }
        pos_++;
    }

    return std::unexpected(ParseError{row_, col_, start_pos_outer, pos_ - start_pos_outer, "Unterminated bracket argument"});
}

std::expected<VariableReference, ParseError> Parser::parse_variable_reference(bool inside_quotes) {
    // We're positioned right after '$'
    // For now, we only support ${...}, not $ENV{...} or $CACHE{...}

    std::string namespace_prefix;
    size_t ref_start_pos = pos_ - 1; // Position of '$'

    // Check what comes after '$'
    if (pos_ >= content_.length()) { return std::unexpected(ParseError{row_, col_, pos_, 0, "Unexpected end after '$'"}); }

    // Detect namespace prefix: $ENV{...}, $CACHE{...}, or ${...}
    if (content_[pos_] == '{') {
        // Regular variable: ${...}
        namespace_prefix = "";
    } else if (is_alpha(content_[pos_]) || content_[pos_] == '_') {
        // Parse identifier (namespace prefix)
        size_t prefix_start = pos_;
        while (pos_ < content_.length() && is_ident_char(content_[pos_])) {
            pos_++;
            col_++;
        }

        std::string prefix = kiln::to_upper(content_.substr(prefix_start, pos_ - prefix_start));

        // Expect '{'
        if (pos_ >= content_.length() || content_[pos_] != '{') {
            return std::unexpected(ParseError{row_, col_, pos_, 1, "Expected '{' after '$" + prefix + "'"});
        }

        namespace_prefix = std::move(prefix);
    } else {
        return std::unexpected(ParseError{row_, col_, pos_, 1, "Expected '{' or identifier after '$'"});
    }

    // Consume '{'
    pos_++;
    col_++;

    // Parse the content inside ${...} as argument parts
    std::vector<ArgumentPart> name_parts;
    std::string current_literal;

    while (pos_ < content_.length()) {
        char c = content_[pos_];

        // Check for closing brace
        if (c == '}') {
            // Save any accumulated literal
            if (!current_literal.empty()) {
                name_parts.emplace_back(std::move(current_literal));
                current_literal.clear();
            }
            pos_++; // Consume '}'
            col_++;
            return VariableReference{namespace_prefix, std::move(name_parts)};
        }

        // Check for nested variable reference
        if (c == '$' && pos_ + 1 < content_.length() && content_[pos_ + 1] == '{') {
            // Save any accumulated literal
            if (!current_literal.empty()) {
                name_parts.emplace_back(std::move(current_literal));
                current_literal.clear();
            }

            pos_++; // Move past '$'
            col_++;

            // Recursively parse nested variable reference
            auto nested_ref = parse_variable_reference(inside_quotes);
            if (!nested_ref) { return std::unexpected(nested_ref.error()); }
            name_parts.emplace_back(std::move(nested_ref.value()));
            continue;
        }

        // Handle quotes inside variable references (for quoted argument context)
        if (inside_quotes && c == '"') {
            return std::unexpected(ParseError{row_, col_, pos_, 1, "Unexpected '\"' inside variable reference"});
        }

        // CRLF normalization
        if (c == '\r' && pos_ + 1 < content_.length() && content_[pos_ + 1] == '\n') {
            pos_++;
            continue;
        }

        // Regular character - add to literal
        current_literal += c;
        pos_++;
        if (c == '\n') {
            row_++;
            col_ = 1;
        } else {
            col_++;
        }
    }

    return std::unexpected(ParseError{row_, col_, ref_start_pos, pos_ - ref_start_pos, "Unterminated variable reference"});
}

namespace {

bool operator==(const VariableReference& lhs, const VariableReference& rhs);

bool operator==(const ArgumentPart& lhs, const ArgumentPart& rhs) {
    if (lhs.index() != rhs.index()) return false;
    if (std::holds_alternative<std::string>(lhs)) {
        return std::get<std::string>(lhs) == std::get<std::string>(rhs);
    } else {
        return std::get<VariableReference>(lhs) == std::get<VariableReference>(rhs);
    }
}

bool operator==(const VariableReference& lhs, const VariableReference& rhs) {
    if (lhs.namespace_prefix != rhs.namespace_prefix) return false;
    if (lhs.name_parts.size() != rhs.name_parts.size()) return false;
    for (size_t i = 0; i < lhs.name_parts.size(); ++i) {
        if (!(lhs.name_parts[i] == rhs.name_parts[i])) return false;
    }
    return true;
}

bool operator==(const Argument& lhs, const Argument& rhs) {
    if (lhs.quoted != rhs.quoted) return false;
    if (lhs.parts.size() != rhs.parts.size()) return false;
    for (size_t i = 0; i < lhs.parts.size(); ++i) {
        if (!(lhs.parts[i] == rhs.parts[i])) return false;
    }
    return true;
}

bool arguments_equal(const std::vector<Argument>& lhs, const std::vector<Argument>& rhs) {
    if (lhs.size() != rhs.size()) return false;
    for (size_t i = 0; i < lhs.size(); ++i) {
        if (!(lhs[i] == rhs[i])) return false;
    }
    return true;
}

} // namespace

void Parser::check_old_style(const CommandInvocation& cmd, const std::vector<Argument>& expected_args) {
    if (ascii_ci_equal(cmd.identifier, "endif") || ascii_ci_equal(cmd.identifier, "else") || ascii_ci_equal(cmd.identifier, "endforeach")
        || ascii_ci_equal(cmd.identifier, "endwhile") || ascii_ci_equal(cmd.identifier, "endmacro")
        || ascii_ci_equal(cmd.identifier, "endfunction")) {
        if (!cmd.arguments.empty() && !arguments_equal(cmd.arguments, expected_args)) {
            std::string msg = "old style CMake syntax: '" + cmd.identifier + "(...)' has arguments that do not match the opening command.";
            kiln::print_diagnostic(std::cerr, DiagnosticSeverity::Warning, msg, filename_, cmd.row, cmd.col, cmd.offset, cmd.length, {},
                                   std::string(content_), "These arguments are ignored by kiln.");
        }
    }
}

} // namespace kiln
