#pragma once

#include <expected>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace kiln {

// Hard runtime gate: throws if a string that is about to leave the build system
// (as a shell command, file path, or install output) still contains unevaluated
// generator expressions.  This is always a bug — genex must be resolved before
// they reach the shell.
inline void assert_no_genex(std::string_view value, std::string_view context) {
    if (value.find("$<") != std::string_view::npos) {
        throw std::runtime_error(std::string("Unevaluated generator expression in ") + std::string(context) + ": " + std::string(value));
    }
}

// Generator expression node types
enum class GenexNodeType {
    LITERAL,                      // Plain text, no genex
    BUILD_INTERFACE,              // $<BUILD_INTERFACE:...>
    INSTALL_INTERFACE,            // $<INSTALL_INTERFACE:...>
    LINK_ONLY,                    // $<LINK_ONLY:...> - link but don't propagate INTERFACE properties
    CONFIG,                       // $<CONFIG:cfg>
    BOOL,                         // $<BOOL:string>
    IF,                           // $<IF:cond,true_val,false_val>
    AND,                          // $<AND:...>
    OR,                           // $<OR:...>
    NOT,                          // $<NOT:...>
    STREQUAL,                     // $<STREQUAL:a,b>
    EQUAL,                        // $<EQUAL:a,b> - numeric equality
    VERSION_LESS,                 // $<VERSION_LESS:v1,v2>
    VERSION_GREATER,              // $<VERSION_GREATER:v1,v2>
    VERSION_EQUAL,                // $<VERSION_EQUAL:v1,v2>
    VERSION_LESS_EQUAL,           // $<VERSION_LESS_EQUAL:v1,v2>
    VERSION_GREATER_EQUAL,        // $<VERSION_GREATER_EQUAL:v1,v2>
    TARGET_EXISTS,                // $<TARGET_EXISTS:target>
    TARGET_NAME,                  // $<TARGET_NAME:target> - target name (resolves aliases)
    TARGET_NAME_IF_EXISTS,        // $<TARGET_NAME_IF_EXISTS:target> - target name if exists, empty otherwise
    TARGET_FILE,                  // $<TARGET_FILE:target> - full path to target output
    TARGET_FILE_NAME,             // $<TARGET_FILE_NAME:target> - filename of target output
    TARGET_FILE_DIR,              // $<TARGET_FILE_DIR:target> - directory of target output
    TARGET_LINKER_FILE,           // $<TARGET_LINKER_FILE:target> - full path to linker file
    TARGET_LINKER_FILE_NAME,      // $<TARGET_LINKER_FILE_NAME:target> - filename of linker file
    TARGET_LINKER_FILE_DIR,       // $<TARGET_LINKER_FILE_DIR:target> - directory of linker file
    TARGET_OBJECTS,               // $<TARGET_OBJECTS:target> - object files from OBJECT_LIBRARY
    TARGET_PROPERTY,              // $<TARGET_PROPERTY:tgt,prop> or $<TARGET_PROPERTY:prop>
    TARGET_FILE_BASE_NAME,        // $<TARGET_FILE_BASE_NAME:target> - base name without prefix/suffix
    TARGET_FILE_PREFIX,           // $<TARGET_FILE_PREFIX:target> - prefix (e.g. "lib")
    TARGET_FILE_SUFFIX,           // $<TARGET_FILE_SUFFIX:target> - suffix (e.g. ".a", ".so")
    TARGET_LINKER_FILE_BASE_NAME, // $<TARGET_LINKER_FILE_BASE_NAME:target>
    TARGET_LINKER_FILE_PREFIX,    // $<TARGET_LINKER_FILE_PREFIX:target>
    TARGET_LINKER_FILE_SUFFIX,    // $<TARGET_LINKER_FILE_SUFFIX:target>
    GENEX_EVAL,                   // $<GENEX_EVAL:expr> - evaluate string as genex
    TARGET_GENEX_EVAL,            // $<TARGET_GENEX_EVAL:target,expr> - evaluate with target context
    JOIN,                         // $<JOIN:list,glue> - join list with separator
    REMOVE_DUPLICATES,            // $<REMOVE_DUPLICATES:list> - remove duplicate entries
    FILTER,                       // $<FILTER:list,INCLUDE|EXCLUDE,regex> - filter list
    IN_LIST,                      // $<IN_LIST:value,list> - check if value is in list
    LOWER_CASE,                   // $<LOWER_CASE:string> - convert to lowercase
    UPPER_CASE,                   // $<UPPER_CASE:string> - convert to uppercase
    COMPILE_LANGUAGE,             // $<COMPILE_LANGUAGE:lang>
    COMPILE_LANG_AND_ID,          // $<COMPILE_LANG_AND_ID:lang,id1,id2,...>
    LINK_LANGUAGE,                // $<LINK_LANGUAGE:lang[,lang2,...]>
    LINK_GROUP,                   // $<LINK_GROUP:feature,libs...> — group libs with linker feature
    PLATFORM_ID,                  // $<PLATFORM_ID:platform>
    CXX_COMPILER_ID,              // $<CXX_COMPILER_ID:id>
    C_COMPILER_ID,                // $<C_COMPILER_ID:id>
    CXX_COMPILER_VERSION,         // $<CXX_COMPILER_VERSION> - CXX compiler version string
    C_COMPILER_VERSION,           // $<C_COMPILER_VERSION> - C compiler version string
    INSTALL_PREFIX,               // $<INSTALL_PREFIX> - install prefix path
    CONDITIONAL,                  // $<cond:text> where cond is a genex
    UNSUPPORTED                   // Unknown or unsupported genex type
};

// AST node for generator expressions
struct GenexNode {
    GenexNodeType type;
    std::string raw_content; // For literals, or original genex string if UNSUPPORTED
    std::vector<std::shared_ptr<GenexNode>> children;
    size_t start_pos = 0;
    size_t end_pos = 0;

    GenexNode(GenexNodeType t, std::string content = "") : type(t), raw_content(std::move(content)) {}
};

// Result of parsing a property value
struct GenexParseResult {
    std::vector<std::shared_ptr<GenexNode>> nodes; // Mix of literals + genex
    bool has_genex = false;                        // Optimization: skip evaluation if false
};

// Parser for CMake generator expressions
class GenexParser {
public:
    GenexParser() = default;

    // Enable recovery mode: unmatched $< degrades to literal "$<" text
    // instead of returning an error. Use this when evaluating expressions
    // that may contain structurally-unbalanced $< (e.g. Qt's $<ANGLE-R>
    // patterns create $</$> imbalances that CMake handles via recovery).
    void set_recovery(bool enabled) { allow_recovery_ = enabled; }

    // Quick check if string contains any generator expression (without parsing)
    // Use this to skip validation/processing for strings that can't contain genex
    static bool contains_genex(const std::string& input) { return input.find("$<") != std::string::npos; }

    // Parse a property value that may contain genex
    std::expected<GenexParseResult, std::string> parse(const std::string& input);

    // Validation API (shared by both error handling layers)
    // Returns error if input contains unsupported genex types or malformed syntax
    static std::expected<void, std::string> validate_genex_support(const std::string& input);

    // Extract all genex types found in input (for introspection)
    static std::expected<std::set<GenexNodeType>, std::string> extract_genex_types(const std::string& input);

    // Split comma-separated arguments, respecting nested genex (public for evaluator)
    std::vector<std::string> split_genex_args(const std::string& content);

private:
    std::string input_;
    size_t pos_ = 0;
    bool allow_recovery_ = false;

    // Parse a single genex starting at current position (expects to be at '$<')
    std::expected<std::shared_ptr<GenexNode>, std::string> parse_genex();

    // Parse the interior of a genex (after the keyword and ':')
    // Returns the balanced content up to the closing '>'
    std::expected<std::string, std::string> parse_genex_content();

    // Determine genex type from keyword
    GenexNodeType classify_genex_type(const std::string& keyword) const;

    // Check if we're at end of input
    bool at_end() const { return pos_ >= input_.size(); }

    // Peek at current character
    char peek() const { return at_end() ? '\0' : input_[pos_]; }

    // Advance position
    void advance() {
        if (!at_end()) ++pos_;
    }

    // Get substring from start to current position
    std::string substr_from(size_t start) const { return input_.substr(start, pos_ - start); }
};

} // namespace kiln
