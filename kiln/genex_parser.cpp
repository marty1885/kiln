#include "genex_parser.hpp"
#include <algorithm>
#include <functional>
#include <sstream>
#include <map>

namespace kiln {

GenexNodeType GenexParser::classify_genex_type(const std::string& keyword) const {
    // Check if keyword is itself a genex (conditional pattern)
    if (keyword.starts_with("$<")) { return GenexNodeType::CONDITIONAL; }

    if (keyword == "BUILD_INTERFACE") return GenexNodeType::BUILD_INTERFACE;
    if (keyword == "BUILD_LOCAL_INTERFACE")
        return GenexNodeType::BUILD_INTERFACE; // CMake 3.26+, same as BUILD_INTERFACE (no export in kiln)
    if (keyword == "INSTALL_INTERFACE") return GenexNodeType::INSTALL_INTERFACE;
    if (keyword == "INSTALL_LOCAL_INTERFACE")
        return GenexNodeType::INSTALL_INTERFACE; // CMake 3.26+, same as INSTALL_INTERFACE (no export in kiln)
    if (keyword == "LINK_ONLY") return GenexNodeType::LINK_ONLY;
    if (keyword == "CONFIG") return GenexNodeType::CONFIG;
    if (keyword == "BOOL") return GenexNodeType::BOOL;
    if (keyword == "IF") return GenexNodeType::IF;
    if (keyword == "AND") return GenexNodeType::AND;
    if (keyword == "OR") return GenexNodeType::OR;
    if (keyword == "NOT") return GenexNodeType::NOT;
    if (keyword == "STREQUAL") return GenexNodeType::STREQUAL;
    if (keyword == "EQUAL") return GenexNodeType::EQUAL;
    if (keyword == "VERSION_LESS") return GenexNodeType::VERSION_LESS;
    if (keyword == "VERSION_GREATER") return GenexNodeType::VERSION_GREATER;
    if (keyword == "VERSION_EQUAL") return GenexNodeType::VERSION_EQUAL;
    if (keyword == "VERSION_LESS_EQUAL") return GenexNodeType::VERSION_LESS_EQUAL;
    if (keyword == "VERSION_GREATER_EQUAL") return GenexNodeType::VERSION_GREATER_EQUAL;
    if (keyword == "TARGET_EXISTS") return GenexNodeType::TARGET_EXISTS;
    if (keyword == "TARGET_NAME") return GenexNodeType::TARGET_NAME;
    if (keyword == "TARGET_NAME_IF_EXISTS") return GenexNodeType::TARGET_NAME_IF_EXISTS;
    if (keyword == "TARGET_FILE") return GenexNodeType::TARGET_FILE;
    if (keyword == "TARGET_FILE_NAME") return GenexNodeType::TARGET_FILE_NAME;
    if (keyword == "TARGET_FILE_DIR") return GenexNodeType::TARGET_FILE_DIR;
    if (keyword == "TARGET_LINKER_FILE") return GenexNodeType::TARGET_LINKER_FILE;
    if (keyword == "TARGET_LINKER_FILE_NAME") return GenexNodeType::TARGET_LINKER_FILE_NAME;
    if (keyword == "TARGET_LINKER_FILE_DIR") return GenexNodeType::TARGET_LINKER_FILE_DIR;
    if (keyword == "TARGET_OBJECTS") return GenexNodeType::TARGET_OBJECTS;
    if (keyword == "TARGET_PROPERTY") return GenexNodeType::TARGET_PROPERTY;
    if (keyword == "TARGET_FILE_BASE_NAME") return GenexNodeType::TARGET_FILE_BASE_NAME;
    if (keyword == "TARGET_FILE_PREFIX") return GenexNodeType::TARGET_FILE_PREFIX;
    if (keyword == "TARGET_FILE_SUFFIX") return GenexNodeType::TARGET_FILE_SUFFIX;
    if (keyword == "TARGET_LINKER_FILE_BASE_NAME") return GenexNodeType::TARGET_LINKER_FILE_BASE_NAME;
    if (keyword == "TARGET_LINKER_FILE_PREFIX") return GenexNodeType::TARGET_LINKER_FILE_PREFIX;
    if (keyword == "TARGET_LINKER_FILE_SUFFIX") return GenexNodeType::TARGET_LINKER_FILE_SUFFIX;
    if (keyword == "GENEX_EVAL") return GenexNodeType::GENEX_EVAL;
    if (keyword == "TARGET_GENEX_EVAL") return GenexNodeType::TARGET_GENEX_EVAL;
    if (keyword == "JOIN") return GenexNodeType::JOIN;
    if (keyword == "REMOVE_DUPLICATES") return GenexNodeType::REMOVE_DUPLICATES;
    if (keyword == "FILTER") return GenexNodeType::FILTER;
    if (keyword == "IN_LIST") return GenexNodeType::IN_LIST;
    if (keyword == "LOWER_CASE") return GenexNodeType::LOWER_CASE;
    if (keyword == "UPPER_CASE") return GenexNodeType::UPPER_CASE;
    if (keyword == "COMPILE_LANGUAGE") return GenexNodeType::COMPILE_LANGUAGE;
    if (keyword == "COMPILE_LANG_AND_ID") return GenexNodeType::COMPILE_LANG_AND_ID;
    if (keyword == "LINK_LANGUAGE") return GenexNodeType::LINK_LANGUAGE;
    if (keyword == "LINK_GROUP") return GenexNodeType::LINK_GROUP;
    if (keyword == "PLATFORM_ID") return GenexNodeType::PLATFORM_ID;
    if (keyword == "CXX_COMPILER_ID") return GenexNodeType::CXX_COMPILER_ID;
    if (keyword == "C_COMPILER_ID") return GenexNodeType::C_COMPILER_ID;
    if (keyword == "CXX_COMPILER_VERSION") return GenexNodeType::CXX_COMPILER_VERSION;
    if (keyword == "C_COMPILER_VERSION") return GenexNodeType::C_COMPILER_VERSION;

    // Constant literal genexes ($<SEMICOLON>, $<COMMA>, etc.)
    // are handled specially in parse_genex_node(), not here.

    return GenexNodeType::UNSUPPORTED;
}

std::expected<std::string, std::string> GenexParser::parse_genex_content() {
    // Parse content until we hit the matching '>', balancing only $< ... > pairs.
    // Bare '<' is NOT special — only '$<' opens a nesting level, matching CMake's
    // lexer which tokenizes '$<' as BeginExpression but treats bare '<' as text.
    std::string content;
    int depth = 1; // We're already inside one '$<'
    size_t start_pos = pos_;

    while (!at_end() && depth > 0) {
        char c = peek();

        if (c == '$' && pos_ + 1 < input_.size() && input_[pos_ + 1] == '<') {
            // $< opens a new nesting level
            ++depth;
            content += '$';
            advance();
            content += '<';
            advance();
        } else if (c == '>') {
            --depth;
            if (depth > 0) { content += c; }
            advance();
        } else {
            content += c;
            advance();
        }
    }

    if (depth != 0) { return std::unexpected("Unmatched '$<' in generator expression at position " + std::to_string(start_pos)); }

    return content;
}

std::vector<std::string> GenexParser::split_genex_args(const std::string& content) {
    // Split on commas, but only at depth 0.  Only '$<' opens a nesting level
    // (bare '<' is plain text), matching CMake's tokenizer behaviour.
    std::vector<std::string> args;
    std::string current;
    int depth = 0;

    for (size_t i = 0; i < content.size(); ++i) {
        char c = content[i];

        if (c == '$' && i + 1 < content.size() && content[i + 1] == '<') {
            ++depth;
            current += '$';
            current += '<';
            ++i; // skip the '<'
        } else if (c == '>') {
            --depth;
            current += c;
        } else if (c == ',' && depth == 0) {
            args.push_back(current);
            current.clear();
        } else {
            current += c;
        }
    }

    // Always add the last part, even if empty (important for cases like "a," or ",")
    args.push_back(current);

    return args;
}

std::expected<std::shared_ptr<GenexNode>, std::string> GenexParser::parse_genex() {
    size_t start = pos_;

    // Expect "$<"
    if (peek() != '$') { return std::unexpected("Expected '$' at position " + std::to_string(pos_)); }
    advance();

    if (peek() != '<') { return std::unexpected("Expected '<' after '$' at position " + std::to_string(pos_)); }
    advance();

    // Parse until we find ':' or '>' (but balance $<...> pairs for nested genex).
    // Only '$<' opens a nesting level — bare '<' is plain text, matching CMake.
    std::string keyword;
    int depth = 0;
    while (!at_end()) {
        char c = peek();
        if (c == '$' && pos_ + 1 < input_.size() && input_[pos_ + 1] == '<') {
            depth++;
            keyword += '$';
            advance();
            keyword += '<';
            advance();
        } else if (c == '>') {
            if (depth > 0) {
                depth--;
                keyword += c;
                advance();
            } else {
                // This is the closing '>' for our genex
                break;
            }
        } else if (c == ':' && depth == 0) {
            // Found the separator at our level
            break;
        } else {
            keyword += c;
            advance();
        }
    }

    if (at_end()) { return std::unexpected("Unexpected end of input in generator expression"); }

    GenexNodeType type = classify_genex_type(keyword);
    auto node = std::make_shared<GenexNode>(type);
    node->start_pos = start;

    // Handle CONDITIONAL type (keyword is itself a genex)
    if (type == GenexNodeType::CONDITIONAL) {
        // keyword contains the condition genex (e.g., "$<CONFIG:Debug>")
        // Parse it as a nested genex
        GenexParser cond_parser;
        auto cond_result = cond_parser.parse(keyword);
        if (!cond_result) { return std::unexpected(cond_result.error()); }
        // Store condition in children (first set of nodes)
        node->children = cond_result->nodes;

        // Now parse the value part after ':'
        if (peek() == ':') {
            advance();
            auto content_result = parse_genex_content();
            if (!content_result) { return std::unexpected(content_result.error()); }

            // Parse the content as potentially containing more genex
            GenexParser value_parser;
            auto value_result = value_parser.parse(*content_result);
            if (!value_result) { return std::unexpected(value_result.error()); }
            // Append value nodes to children
            node->children.insert(node->children.end(), value_result->nodes.begin(), value_result->nodes.end());
            node->raw_content = *content_result;
        } else if (peek() == '>') {
            advance();
            // No value part, just condition
        }
        node->end_pos = pos_;
        return node;
    }

    // Handle constant literal genexes: $<SEMICOLON>, $<COMMA>, $<ANGLE-R>, $<QUOTE>
    if (type == GenexNodeType::UNSUPPORTED && peek() == '>') {
        static const std::map<std::string, std::string> constant_genexes = {
            {"SEMICOLON", ";"}, {"COMMA", ","},         {"ANGLE-R", ">"},
            {"QUOTE", "\""},    {"INSTALL_PREFIX", ""}, // Placeholder, resolved in evaluator
        };
        auto cit = constant_genexes.find(keyword);
        if (cit != constant_genexes.end()) {
            advance(); // consume '>'
            if (keyword == "INSTALL_PREFIX") {
                // Needs runtime resolution — use a dedicated type
                node->type = GenexNodeType::INSTALL_PREFIX;
            } else {
                node->type = GenexNodeType::LITERAL;
            }
            node->raw_content = cit->second;
            node->end_pos = pos_;
            return node;
        }
    }

    // If it's UNSUPPORTED but the keyword is a simple literal boolean (0/1),
    // treat as CONDITIONAL. This handles $<1:text> and $<0:text>.
    // Other unknown keywords with ':' remain UNSUPPORTED for proper error reporting.
    auto is_literal_bool = [](const std::string& kw) { return kw == "0" || kw == "1"; };
    if (type == GenexNodeType::UNSUPPORTED && peek() == ':' && is_literal_bool(keyword)) {
        node->type = GenexNodeType::CONDITIONAL;
        // Store the keyword as a literal condition child
        auto cond_node = std::make_shared<GenexNode>(GenexNodeType::LITERAL, keyword);
        node->children.push_back(cond_node);

        advance(); // consume ':'
        auto content_result = parse_genex_content();
        if (!content_result) { return std::unexpected(content_result.error()); }

        // Parse the value part
        GenexParser value_parser;
        auto value_result = value_parser.parse(*content_result);
        if (!value_result) { return std::unexpected(value_result.error()); }
        node->children.insert(node->children.end(), value_result->nodes.begin(), value_result->nodes.end());
        node->raw_content = *content_result;
        node->end_pos = pos_;
        return node;
    }

    // If it's UNSUPPORTED, store the original string for error reporting
    if (type == GenexNodeType::UNSUPPORTED) {
        // Need to capture the full genex string
        size_t content_start = pos_;
        if (peek() == ':') {
            advance();
            auto content_result = parse_genex_content();
            if (!content_result) { return std::unexpected(content_result.error()); }
        } else if (peek() == '>') {
            advance();
        }
        node->end_pos = pos_;
        node->raw_content = "$<" + keyword + ">"; // Simplified representation
        return node;
    }

    // Parse content if present
    if (peek() == ':') {
        advance();
        auto content_result = parse_genex_content();
        if (!content_result) { return std::unexpected(content_result.error()); }

        std::string content = *content_result;
        node->raw_content = content;

        // For certain types, parse arguments recursively
        if (type == GenexNodeType::BUILD_INTERFACE || type == GenexNodeType::INSTALL_INTERFACE || type == GenexNodeType::LINK_ONLY
            || type == GenexNodeType::NOT) {
            // Single argument that may contain nested genex
            GenexParser inner_parser;
            inner_parser.input_ = content;
            inner_parser.pos_ = 0;
            auto inner_result = inner_parser.parse(content);
            if (!inner_result) { return std::unexpected(inner_result.error()); }
            node->children = inner_result->nodes;
        } else if (type == GenexNodeType::IF) {
            // Three comma-separated arguments
            auto args = split_genex_args(content);
            if (args.size() != 3) { return std::unexpected("$<IF:...> requires exactly 3 arguments (condition,true_value,false_value)"); }

            for (const auto& arg : args) {
                GenexParser inner_parser;
                auto inner_result = inner_parser.parse(arg);
                if (!inner_result) { return std::unexpected(inner_result.error()); }
                node->children.insert(node->children.end(), inner_result->nodes.begin(), inner_result->nodes.end());
            }
        } else if (type == GenexNodeType::AND || type == GenexNodeType::OR) {
            // Multiple comma-separated arguments
            auto args = split_genex_args(content);
            for (const auto& arg : args) {
                GenexParser inner_parser;
                auto inner_result = inner_parser.parse(arg);
                if (!inner_result) { return std::unexpected(inner_result.error()); }
                node->children.insert(node->children.end(), inner_result->nodes.begin(), inner_result->nodes.end());
            }
        } else if (type == GenexNodeType::STREQUAL || type == GenexNodeType::EQUAL || type == GenexNodeType::VERSION_LESS
                   || type == GenexNodeType::VERSION_GREATER || type == GenexNodeType::VERSION_EQUAL
                   || type == GenexNodeType::VERSION_LESS_EQUAL || type == GenexNodeType::VERSION_GREATER_EQUAL) {
            // Two comma-separated arguments
            auto args = split_genex_args(content);
            if (args.size() != 2) {
                std::string genex_name;
                if (type == GenexNodeType::STREQUAL)
                    genex_name = "STREQUAL";
                else if (type == GenexNodeType::EQUAL)
                    genex_name = "EQUAL";
                else if (type == GenexNodeType::VERSION_LESS)
                    genex_name = "VERSION_LESS";
                else if (type == GenexNodeType::VERSION_GREATER)
                    genex_name = "VERSION_GREATER";
                else if (type == GenexNodeType::VERSION_EQUAL)
                    genex_name = "VERSION_EQUAL";
                else if (type == GenexNodeType::VERSION_LESS_EQUAL)
                    genex_name = "VERSION_LESS_EQUAL";
                else if (type == GenexNodeType::VERSION_GREATER_EQUAL)
                    genex_name = "VERSION_GREATER_EQUAL";
                return std::unexpected("$<" + genex_name + ":...> requires exactly 2 arguments");
            }

            for (const auto& arg : args) {
                GenexParser inner_parser;
                auto inner_result = inner_parser.parse(arg);
                if (!inner_result) { return std::unexpected(inner_result.error()); }
                node->children.insert(node->children.end(), inner_result->nodes.begin(), inner_result->nodes.end());
            }
        } else if (type == GenexNodeType::TARGET_PROPERTY) {
            // One or two comma-separated arguments: target,property or just property
            auto args = split_genex_args(content);
            if (args.size() != 1 && args.size() != 2) { return std::unexpected("$<TARGET_PROPERTY:...> requires 1 or 2 arguments"); }

            for (const auto& arg : args) {
                GenexParser inner_parser;
                auto inner_result = inner_parser.parse(arg);
                if (!inner_result) { return std::unexpected(inner_result.error()); }
                node->children.insert(node->children.end(), inner_result->nodes.begin(), inner_result->nodes.end());
            }
        } else if (type == GenexNodeType::GENEX_EVAL || type == GenexNodeType::REMOVE_DUPLICATES || type == GenexNodeType::LOWER_CASE
                   || type == GenexNodeType::UPPER_CASE) {
            // Single argument that may contain nested genex
            GenexParser inner_parser;
            auto inner_result = inner_parser.parse(content);
            if (!inner_result) { return std::unexpected(inner_result.error()); }
            node->children = inner_result->nodes;
        } else if (type == GenexNodeType::TARGET_GENEX_EVAL || type == GenexNodeType::JOIN || type == GenexNodeType::IN_LIST) {
            // Two comma-separated arguments
            auto args = split_genex_args(content);
            for (const auto& arg : args) {
                GenexParser inner_parser;
                auto inner_result = inner_parser.parse(arg);
                if (!inner_result) { return std::unexpected(inner_result.error()); }
                node->children.insert(node->children.end(), inner_result->nodes.begin(), inner_result->nodes.end());
            }
        }
        // For simple types (CONFIG, BOOL, etc.), raw_content is sufficient
    } else if (peek() == '>') {
        advance();
        // Empty genex like $<AND> or $<OR> - valid for some types
    } else {
        return std::unexpected("Expected ':' or '>' in generator expression at position " + std::to_string(pos_));
    }

    node->end_pos = pos_;
    return node;
}

std::expected<GenexParseResult, std::string> GenexParser::parse(const std::string& input) {
    input_ = input;
    pos_ = 0;

    GenexParseResult result;
    std::string literal;

    while (!at_end()) {
        // Look for "$<"
        if (peek() == '$' && pos_ + 1 < input_.size() && input_[pos_ + 1] == '<') {
            // Save any accumulated literal
            if (!literal.empty()) {
                auto lit_node = std::make_shared<GenexNode>(GenexNodeType::LITERAL, literal);
                result.nodes.push_back(lit_node);
                literal.clear();
            }

            // Parse genex
            size_t saved_pos = pos_;
            auto genex_result = parse_genex();
            if (!genex_result) {
                if (allow_recovery_) {
                    // Recovery mode: unmatched $< degrades to literal "$<" text
                    // (matches CMake behaviour for structurally-unbalanced expressions)
                    pos_ = saved_pos + 1; // skip past '$', re-parse '<' as text
                    literal += '$';
                    continue;
                }
                return std::unexpected(genex_result.error());
            }

            result.nodes.push_back(*genex_result);
            result.has_genex = true;
        } else {
            literal += peek();
            advance();
        }
    }

    // Save any remaining literal
    if (!literal.empty()) {
        auto lit_node = std::make_shared<GenexNode>(GenexNodeType::LITERAL, literal);
        result.nodes.push_back(lit_node);
    }

    // If no nodes, add empty literal
    if (result.nodes.empty()) {
        auto lit_node = std::make_shared<GenexNode>(GenexNodeType::LITERAL, "");
        result.nodes.push_back(lit_node);
    }

    return result;
}

std::expected<void, std::string> GenexParser::validate_genex_support(const std::string& input) {
    GenexParser parser;
    auto result = parser.parse(input);
    if (!result) {
        return std::unexpected(result.error()); // Syntax error
    }

    // Check for unsupported types recursively
    std::function<std::expected<void, std::string>(const GenexNode&)> check_node =
        [&](const GenexNode& node) -> std::expected<void, std::string> {
        if (node.type == GenexNodeType::UNSUPPORTED) { return std::unexpected("Unsupported generator expression: " + node.raw_content); }
        for (const auto& child : node.children) {
            auto child_result = check_node(*child);
            if (!child_result) { return child_result; }
        }
        return {};
    };

    for (const auto& node : result->nodes) {
        auto node_result = check_node(*node);
        if (!node_result) { return node_result; }
    }

    return {}; // All genex are supported
}

std::expected<std::set<GenexNodeType>, std::string> GenexParser::extract_genex_types(const std::string& input) {
    GenexParser parser;
    auto result = parser.parse(input);
    if (!result) { return std::unexpected(result.error()); }

    std::set<GenexNodeType> types;

    std::function<void(const GenexNode&)> collect_types = [&](const GenexNode& node) {
        if (node.type != GenexNodeType::LITERAL) { types.insert(node.type); }
        for (const auto& child : node.children) { collect_types(*child); }
    };

    for (const auto& node : result->nodes) { collect_types(*node); }

    return types;
}

} // namespace kiln
