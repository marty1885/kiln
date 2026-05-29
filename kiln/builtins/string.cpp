#include "registry.hpp"
#include "../interperter.hpp"
#include "../command_parser.hpp"
#include "../utils.hpp"
#include "../container_utils.hpp"
#include "../parse_number.hpp"
#include <algorithm>
#include <cctype>
#include "../regex.hpp"
#include "../clock_cache.hpp"
#include <iomanip>
#include <random>
#include <ctime>
#include <chrono>
#include <sys/stat.h>
#include <filesystem>
#include <glaze/json/read.hpp>
#include <glaze/json/write.hpp>
#include <glaze/json/generic.hpp>

namespace kiln {

namespace {

// Helper to convert string to lowercase
std::string to_lower(const std::string& str) {
    return kiln::to_lower(str);
}

// Helper to strip leading and trailing whitespace
std::string strip(const std::string& str) {
    return std::string(kiln::strip(str));
}

// Helper to escape regex special characters
std::string regex_quote(const std::string& str) {
    static const std::string special_chars = "\\^$.|?*+()[]{}-";
    std::string result;
    result.reserve(str.size() * 2);

    for (char c : str) {
        if (special_chars.find(c) != std::string::npos) { result += '\\'; }
        result += c;
    }

    return result;
}

// Helper to strip generator expressions
std::string genex_strip(const std::string& str) {
    std::string result;
    int depth = 0;
    size_t i = 0;

    while (i < str.size()) {
        if (i + 1 < str.size() && str[i] == '$' && str[i + 1] == '<') {
            depth++;
            i += 2;
            continue;
        }

        if (depth > 0 && str[i] == '>') {
            depth--;
            i++;
            continue;
        }

        if (depth == 0) { result += str[i]; }
        i++;
    }

    return result;
}

// Helper to make a valid C identifier
std::string make_c_identifier(const std::string& str) {
    std::string result;
    result.reserve(str.size());

    for (char c : str) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            result += c;
        } else {
            result += '_';
        }
    }

    return result;
}

// Helper to convert byte to hex
std::string byte_to_hex(unsigned char byte) {
    std::stringstream ss;
    ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    return ss.str();
}

// Helper to perform configure-style variable substitution
std::string configure_string(const std::string& input, Interpreter& interp, bool at_only, bool escape_quotes) {
    std::string result;
    size_t i = 0;

    while (i < input.size()) {
        // Handle ${VAR} syntax
        if (!at_only && i + 2 < input.size() && input[i] == '$' && input[i + 1] == '{') {
            size_t end = input.find('}', i + 2);
            if (end != std::string::npos) {
                std::string var_name = input.substr(i + 2, end - i - 2);
                std::string value = interp.get_variable(var_name);
                if (escape_quotes) {
                    for (char c : value) {
                        if (c == '"') result += '\\';
                        result += c;
                    }
                } else {
                    result += value;
                }
                i = end + 1;
                continue;
            }
        }

        // Handle @VAR@ syntax
        if (i + 1 < input.size() && input[i] == '@') {
            size_t end = input.find('@', i + 1);
            if (end != std::string::npos) {
                std::string var_name = input.substr(i + 1, end - i - 1);
                std::string value = interp.get_variable(var_name);
                if (escape_quotes) {
                    for (char c : value) {
                        if (c == '"') result += '\\';
                        result += c;
                    }
                } else {
                    result += value;
                }
                i = end + 1;
                continue;
            }
        }

        result += input[i];
        i++;
    }

    return result;
}

} // anonymous namespace

// Parse arguments in UNIX_COMMAND mode
std::vector<std::string> parse_unix_command(const std::string& input) {
    std::vector<std::string> result;
    std::string current;
    bool in_single_quote = false;
    bool in_double_quote = false;
    bool escape_next = false;

    for (size_t i = 0; i < input.size(); ++i) {
        char c = input[i];

        if (escape_next) {
            // Backslash escapes the next literal character
            current += c;
            escape_next = false;
            continue;
        }

        if (c == '\\') {
            escape_next = true;
            continue;
        }

        if (c == '\'' && !in_double_quote) {
            in_single_quote = !in_single_quote;
            continue;
        }

        if (c == '"' && !in_single_quote) {
            in_double_quote = !in_double_quote;
            continue;
        }

        if (std::isspace(c) && !in_single_quote && !in_double_quote) {
            if (!current.empty()) {
                result.push_back(current);
                current.clear();
            }
            continue;
        }

        current += c;
    }

    if (!current.empty()) { result.push_back(current); }

    return result;
}

// Parse arguments in WINDOWS_COMMAND mode
std::vector<std::string> parse_windows_command(const std::string& input) {
    std::vector<std::string> result;
    std::string current;
    bool in_quote = false;
    size_t i = 0;

    while (i < input.size()) {
        // Count consecutive backslashes
        size_t backslash_count = 0;
        while (i < input.size() && input[i] == '\\') {
            backslash_count++;
            i++;
        }

        if (i < input.size() && input[i] == '"') {
            // Backslashes before quote
            // Even number: half the backslashes in output, quote is delimiter
            // Odd number: half the backslashes (rounded down) in output, quote is literal
            current += std::string(backslash_count / 2, '\\');

            if (backslash_count % 2 == 0) {
                // Quote is not escaped
                // Check for "" (double quote within quoted string = literal quote)
                if (in_quote && i + 1 < input.size() && input[i + 1] == '"') {
                    current += '"';
                    i += 2; // Skip both quotes
                } else {
                    // Toggle quote mode
                    in_quote = !in_quote;
                    i++;
                }
            } else {
                // Quote is escaped, add literal quote
                current += '"';
                i++;
            }
        } else {
            // Backslashes not followed by quote are literal
            current += std::string(backslash_count, '\\');

            if (i < input.size()) {
                char c = input[i];

                if (std::isspace(static_cast<unsigned char>(c)) && !in_quote) {
                    if (!current.empty()) {
                        result.push_back(current);
                        current.clear();
                    }
                    i++;
                } else {
                    current += c;
                    i++;
                }
            }
        }
    }

    if (!current.empty()) { result.push_back(current); }

    return result;
}

// Search for a program in PATH
std::string find_program_in_path(const std::string& program_name) {
    // If already an absolute path and executable, return as-is
    std::filesystem::path p(program_name);
    if (p.is_absolute()) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(p, ec)) {
            struct stat st;
            if (stat(p.c_str(), &st) == 0 && (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH))) { return program_name; }
        }
    }

    // Search in PATH
    const char* path_env = std::getenv("PATH");
    if (!path_env) return "";

    std::string path_str(path_env);
    size_t start = 0;

    while (start < path_str.size()) {
        size_t end = path_str.find(':', start);
        if (end == std::string::npos) { end = path_str.size(); }

        std::string dir = path_str.substr(start, end - start);
        if (!dir.empty()) {
            std::filesystem::path candidate = std::filesystem::path(dir) / program_name;
            std::error_code ec;

            if (std::filesystem::is_regular_file(candidate, ec)) {
                struct stat st;
                if (stat(candidate.c_str(), &st) == 0 && (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH))) {
                    return std::filesystem::absolute(candidate).string();
                }
            }
        }

        start = end + 1;
    }

    return "";
}

// ---------------------------------------------------------------------------
// Parse-time classification + runtime fast path for string(SUBSTRING ...).
// ---------------------------------------------------------------------------
//
// Recognized: string(SUBSTRING <input> <begin> <length> <out_var>) where
//   - <out_var> is a single parse-time string literal,
//   - <input>/<begin>/<length> are each either a parse-time string literal
//     OR a single bare ${VAR} reference with a literal name (no namespace,
//     no nested-name expansion).
// On the bench's hex-decode loop, 9 of every 10 SUBSTRING index args are
// static literals, so the runtime path skips parse_number entirely on those.

static std::optional<std::string_view> single_literal(const Argument& a) {
    if (a.parts.size() != 1) return std::nullopt;
    auto* s = std::get_if<std::string>(&a.parts[0]);
    if (!s) return std::nullopt;
    return std::string_view(*s);
}

// "Simple var ref" = exactly one Argument part, which is a VariableReference
// with no namespace and a name that is a single string literal.
static const std::string* single_simple_var_name(const Argument& a) {
    if (a.parts.size() != 1) return nullptr;
    auto* ref = std::get_if<VariableReference>(&a.parts[0]);
    if (!ref || !ref->namespace_prefix.empty()) return nullptr;
    if (ref->name_parts.size() != 1) return nullptr;
    return std::get_if<std::string>(&ref->name_parts[0]);
}

// Decode an Argument that's expected to be either a literal int or a simple
// var ref. Sets `*is_var` + `var_name` (when var) or `*literal` (when not).
// Returns false if neither shape fits.
static bool decode_index_arg(const Argument& a, bool& is_var, std::string& var_name, int64_t& literal) {
    if (auto vn = single_simple_var_name(a)) {
        is_var = true;
        var_name = *vn;
        return true;
    }
    auto lit = single_literal(a);
    if (!lit) return false;
    // Strip ASCII whitespace.
    std::string_view s = *lit;
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    if (s.empty()) return false;
    bool neg = false;
    if (s.front() == '-') {
        neg = true;
        s.remove_prefix(1);
    } else if (s.front() == '+') {
        s.remove_prefix(1);
    }
    if (s.empty()) return false;
    int64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    is_var = false;
    literal = neg ? -v : v;
    return true;
}

std::optional<PreParsedSubstring> classify_substring(const std::vector<Argument>& args) {
    // args[0] is the subcommand "SUBSTRING" (case-insensitive, parse-time literal).
    if (args.size() != 5) return std::nullopt;
    auto sub = single_literal(args[0]);
    if (!sub) return std::nullopt;
    if (!ci_equals(*sub, "SUBSTRING")) return std::nullopt;

    auto out_var = single_literal(args[4]);
    if (!out_var) return std::nullopt;

    PreParsedSubstring pp;
    pp.out_var = std::string(*out_var);

    // input: literal or simple var
    if (auto vn = single_simple_var_name(args[1])) {
        pp.input_is_var = true;
        pp.input = *vn;
    } else if (auto lit = single_literal(args[1])) {
        pp.input_is_var = false;
        pp.input = std::string(*lit);
    } else {
        return std::nullopt;
    }

    if (!decode_index_arg(args[2], pp.begin_is_var, pp.begin_var, pp.begin_literal)) { return std::nullopt; }
    if (!decode_index_arg(args[3], pp.length_is_var, pp.length_var, pp.length_literal)) { return std::nullopt; }
    return pp;
}

// Parse a CMake-style decimal int from a string_view holding a variable's
// value. Returns false on any malformation. Tight loop, no allocations.
static bool parse_index_value(std::string_view s, int64_t& out) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    if (s.empty()) return false;
    bool neg = false;
    if (s.front() == '-') {
        neg = true;
        s.remove_prefix(1);
    } else if (s.front() == '+') {
        s.remove_prefix(1);
    }
    if (s.empty()) return false;
    int64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    out = neg ? -v : v;
    return true;
}

bool try_execute_pre_parsed_substring(Interpreter& interp, const PreParsedSubstring& pp) {
    // Resolve input. When input is a var, view its storage directly.
    std::string_view input;
    std::string input_buf; // only used in the literal/copy branch (rare)
    if (pp.input_is_var) {
        auto v = interp.get_variable_view(pp.input);
        if (!v) {
            // Match the slow path: undefined vars expand to empty.
            input = std::string_view{};
        } else {
            input = *v;
        }
    } else {
        input = pp.input;
    }

    int64_t begin;
    if (pp.begin_is_var) {
        auto v = interp.get_variable_view(pp.begin_var);
        if (!v) return false;
        if (!parse_index_value(*v, begin)) return false;
    } else {
        begin = pp.begin_literal;
    }

    int64_t length;
    if (pp.length_is_var) {
        auto v = interp.get_variable_view(pp.length_var);
        if (!v) return false;
        if (!parse_index_value(*v, length)) return false;
    } else {
        length = pp.length_literal;
    }

    // Match the slow path's bounds policy: begin must be in [0, size]; out-of-
    // range is a fatal error, not a silent fallback. We could replicate the
    // fatal_error here, but it's simpler to drop to the slow path for the
    // failure case so error formatting stays in one place.
    if (begin < 0) return false;
    if (static_cast<size_t>(begin) > input.size()) return false;

    std::string result;
    if (length < 0) {
        result.assign(input.data() + begin, input.size() - static_cast<size_t>(begin));
    } else {
        size_t take = static_cast<size_t>(length);
        size_t avail = input.size() - static_cast<size_t>(begin);
        if (take > avail) take = avail;
        result.assign(input.data() + begin, take);
    }

    interp.set_variable(pp.out_var, result);
    return true;
}

void register_string_builtins(Interpreter& interp) {
    interp.add_builtin("string", [](Interpreter& interp, const std::vector<std::string>& args) {
        if (args.empty()) {
            interp.set_fatal_error("string() requires at least one argument");
            return;
        }

        const auto& operation = args[0];
        std::span<const std::string> sub_args(args.begin() + 1, args.end());

        // ========== Search and Replace ==========

        if (ci_equals(operation, "FIND")) {
            CommandParser parser("string", "FIND");
            std::string input, substring, out_var;
            bool reverse = false;

            parser.positional(input, "input string");
            parser.positional(substring, "substring");
            parser.positional(out_var, "output variable");
            parser.flag("REVERSE", reverse);

            PARSE_OR_RETURN(parser, interp, sub_args);

            size_t pos;
            if (reverse) {
                pos = input.rfind(substring);
            } else {
                pos = input.find(substring);
            }

            interp.set_variable(out_var, (pos == std::string::npos) ? "-1" : std::to_string(pos));

        } else if (ci_equals(operation, "REPLACE")) {
            CommandParser parser("string", "REPLACE");
            std::string match_str, replace_str, out_var;
            std::vector<std::string> inputs;

            parser.positional(match_str, "match string");
            parser.positional(replace_str, "replace string");
            parser.positional(out_var, "output variable");
            parser.positionals(inputs, "inputs");

            PARSE_OR_RETURN(parser, interp, sub_args);

            // Concatenate all inputs
            std::string input;
            for (const auto& s : inputs) { input += s; }

            // Replace all occurrences
            std::string result = kiln::replace_all(std::move(input), match_str, replace_str);

            interp.set_variable(out_var, result);

        } else if (ci_equals(operation, "REGEX")) {
            // REGEX operations require a subcommand
            if (sub_args.empty()) {
                interp.set_fatal_error("string(REGEX) requires a subcommand (MATCH, MATCHALL, REPLACE, QUOTE)");
                return;
            }

            const auto& regex_op_str = sub_args[0];
            std::span<const std::string> regex_args(sub_args.begin() + 1, sub_args.end());

            if (ci_equals(regex_op_str, "MATCH")) {
                CommandParser parser("string", "REGEX MATCH");
                std::string pattern, out_var;
                std::vector<std::string> inputs;

                parser.positional(pattern, "regex pattern");
                parser.positional(out_var, "output variable");
                parser.positionals(inputs, "inputs");

                PARSE_OR_RETURN(parser, interp, regex_args);

                // Concatenate all inputs
                std::string input;
                for (const auto& s : inputs) { input += s; }

                thread_local ClockCache<std::string, Regex> cache(8, [](const std::string& p) { return Regex::from_cmake_regex(p); });
                auto re = cache.get(pattern);
                if (!re) {
                    interp.set_fatal_error("string(REGEX MATCH) invalid regex: " + re.error());
                    return;
                }

                std::vector<std::string> captures;
                if ((*re)->search(input, captures)) {
                    interp.set_variable(out_var, captures[0]);
                    for (size_t i = 0; i < captures.size(); ++i) { interp.set_variable("CMAKE_MATCH_" + std::to_string(i), captures[i]); }
                    interp.set_variable("CMAKE_MATCH_COUNT", std::to_string(captures.size() - 1));
                } else {
                    interp.set_variable(out_var, "");
                    // Clear CMAKE_MATCH_* variables on non-match (CMake behavior)
                    interp.set_variable("CMAKE_MATCH_COUNT", "0");
                    interp.set_variable("CMAKE_MATCH_0", "");
                    for (int i = 1; i <= 9; ++i) { interp.set_variable("CMAKE_MATCH_" + std::to_string(i), ""); }
                }

            } else if (ci_equals(regex_op_str, "MATCHALL")) {
                CommandParser parser("string", "REGEX MATCHALL");
                std::string pattern, out_var;
                std::vector<std::string> inputs;

                parser.positional(pattern, "regex pattern");
                parser.positional(out_var, "output variable");
                parser.positionals(inputs, "inputs");

                PARSE_OR_RETURN(parser, interp, regex_args);

                // Concatenate all inputs
                std::string input;
                for (const auto& s : inputs) { input += s; }

                thread_local ClockCache<std::string, Regex> cache(8, [](const std::string& p) { return Regex::from_cmake_regex(p); });
                auto re = cache.get(pattern);
                if (!re) {
                    interp.set_fatal_error("string(REGEX MATCHALL) invalid regex: " + re.error());
                    return;
                }

                auto all_matches = (*re)->match_all(input);
                CMakeArray result;
                for (const auto& m : all_matches) {
                    if (!m.empty()) result.append(m[0]);
                }

                interp.set_variable(out_var, result.to_string());

                // Set CMAKE_MATCH_* from the last match (CMake behavior)
                if (!all_matches.empty()) {
                    const auto& last = all_matches.back();
                    for (size_t i = 0; i < last.size(); ++i) { interp.set_variable("CMAKE_MATCH_" + std::to_string(i), last[i]); }
                    interp.set_variable("CMAKE_MATCH_COUNT", std::to_string(last.size() - 1));
                } else {
                    interp.set_variable("CMAKE_MATCH_COUNT", "0");
                    interp.set_variable("CMAKE_MATCH_0", "");
                    for (int i = 1; i <= 9; ++i) { interp.set_variable("CMAKE_MATCH_" + std::to_string(i), ""); }
                }

            } else if (ci_equals(regex_op_str, "REPLACE")) {
                CommandParser parser("string", "REGEX REPLACE");
                std::string pattern, replacement, out_var;
                std::vector<std::string> inputs;

                parser.positional(pattern, "regex pattern");
                parser.positional(replacement, "replacement expression");
                parser.positional(out_var, "output variable");
                parser.positionals(inputs, "inputs");

                PARSE_OR_RETURN(parser, interp, regex_args);

                // Concatenate all inputs
                std::string input;
                for (const auto& s : inputs) { input += s; }

                thread_local ClockCache<std::string, Regex> cache(8, [](const std::string& p) { return Regex::from_cmake_regex(p); });
                auto re = cache.get(pattern);
                if (!re) {
                    interp.set_fatal_error("string(REGEX REPLACE) invalid regex: " + re.error());
                    return;
                }
                // replace_all handles CMake \1 \2 syntax natively
                // PCRE2 already doesn't match \n with . (no PCRE2_DOTALL),
                // matching CMake's behavior without needing line-by-line processing
                std::string result = (*re)->replace_all(input, replacement);
                interp.set_variable(out_var, result);

            } else if (ci_equals(regex_op_str, "QUOTE")) {
                CommandParser parser("string", "REGEX QUOTE");
                std::string out_var;
                std::vector<std::string> inputs;

                parser.positional(out_var, "output variable");
                parser.positionals(inputs, "inputs");

                PARSE_OR_RETURN(parser, interp, regex_args);

                // Concatenate and quote all inputs
                std::string input;
                for (const auto& s : inputs) { input += s; }

                interp.set_variable(out_var, regex_quote(input));

            } else {
                interp.set_fatal_error("string(REGEX) unknown subcommand: " + std::string(regex_op_str));
                return;
            }

            // ========== Manipulation ==========

        } else if (ci_equals(operation, "APPEND")) {
            CommandParser parser("string", "APPEND");
            std::string var_name;
            std::vector<std::string> inputs;

            parser.positional(var_name, "variable name");
            parser.positionals(inputs, "inputs");

            PARSE_OR_RETURN(parser, interp, sub_args);

            auto entry = interp.get_variables().entry(var_name);
            // Fast path: mutate the stored string in place when it already lives at
            // the current scope depth. This makes a tight string(APPEND) loop
            // amortized O(1) per call instead of O(current-length) per call.
            if (std::string* p = entry.mutable_at_current_depth()) {
                for (const auto& s : inputs) { p->append(s); }
                if (interp.has_variable_watches()) { interp.fire_variable_watch(var_name, "MODIFIED_ACCESS", *p); }
            } else {
                std::string current = entry.get();
                for (const auto& s : inputs) { current += s; }
                entry.set(std::move(current));
                if (interp.has_variable_watches()) { interp.fire_variable_watch(var_name, "MODIFIED_ACCESS", entry.get()); }
            }

        } else if (ci_equals(operation, "PREPEND")) {
            CommandParser parser("string", "PREPEND");
            std::string var_name;
            std::vector<std::string> inputs;

            parser.positional(var_name, "variable name");
            parser.positionals(inputs, "inputs");

            PARSE_OR_RETURN(parser, interp, sub_args);

            std::string prefix;
            for (const auto& s : inputs) { prefix += s; }

            std::string current = interp.get_variable(var_name);
            interp.set_variable(var_name, prefix + current);

        } else if (ci_equals(operation, "CONCAT")) {
            CommandParser parser("string", "CONCAT");
            std::string out_var;
            std::vector<std::string> inputs;

            parser.positional(out_var, "output variable");
            parser.positionals(inputs, "inputs");

            PARSE_OR_RETURN(parser, interp, sub_args);

            std::string result;
            for (const auto& s : inputs) { result += s; }

            interp.set_variable(out_var, result);

        } else if (ci_equals(operation, "JOIN")) {
            CommandParser parser("string", "JOIN");
            std::string glue, out_var;
            std::vector<std::string> inputs;

            parser.positional(glue, "glue string");
            parser.positional(out_var, "output variable");
            parser.positionals(inputs, "inputs");

            PARSE_OR_RETURN(parser, interp, sub_args);

            interp.set_variable(out_var, join(inputs, glue));

        } else if (ci_equals(operation, "TOLOWER")) {
            CommandParser parser("string", "TOLOWER");
            std::string input, out_var;

            parser.positional(input, "input string");
            parser.positional(out_var, "output variable");

            PARSE_OR_RETURN(parser, interp, sub_args);

            interp.set_variable(out_var, to_lower(input));

        } else if (ci_equals(operation, "TOUPPER")) {
            CommandParser parser("string", "TOUPPER");
            std::string input, out_var;

            parser.positional(input, "input string");
            parser.positional(out_var, "output variable");

            PARSE_OR_RETURN(parser, interp, sub_args);

            interp.set_variable(out_var, to_upper(input));

        } else if (ci_equals(operation, "LENGTH")) {
            CommandParser parser("string", "LENGTH");
            std::string input, out_var;

            parser.positional(input, "input string");
            parser.positional(out_var, "output variable");

            PARSE_OR_RETURN(parser, interp, sub_args);

            interp.set_variable(out_var, std::to_string(input.size()));

        } else if (ci_equals(operation, "SUBSTRING")) {
            CommandParser parser("string", "SUBSTRING");
            std::string input, begin_str, length_str, out_var;

            parser.positional(input, "input string");
            parser.positional(begin_str, "begin index");
            parser.positional(length_str, "length");
            parser.positional(out_var, "output variable");

            PARSE_OR_RETURN(parser, interp, sub_args);

            {
                auto begin_opt = parse_number<int>(begin_str);
                auto length_opt = parse_number<int>(length_str);
                if (!begin_opt || !length_opt) {
                    interp.set_fatal_error("string(SUBSTRING) invalid indices");
                    return;
                }
                int begin = *begin_opt;
                int length = *length_opt;

                if (begin < 0 || static_cast<size_t>(begin) > input.size()) {
                    interp.set_fatal_error("string(SUBSTRING) begin index out of range: " + begin_str);
                    return;
                }

                std::string result;
                if (length < 0) {
                    // -1 means to the end
                    result = input.substr(begin);
                } else {
                    result = input.substr(begin, length);
                }

                interp.set_variable(out_var, result);
                return;
            }

        } else if (ci_equals(operation, "STRIP")) {
            CommandParser parser("string", "STRIP");
            std::string input, out_var;

            parser.positional(input, "input string");
            parser.positional(out_var, "output variable");

            PARSE_OR_RETURN(parser, interp, sub_args);

            interp.set_variable(out_var, strip(input));

        } else if (ci_equals(operation, "GENEX_STRIP")) {
            CommandParser parser("string", "GENEX_STRIP");
            std::string input, out_var;

            parser.positional(input, "input string");
            parser.positional(out_var, "output variable");

            PARSE_OR_RETURN(parser, interp, sub_args);

            interp.set_variable(out_var, genex_strip(input));

        } else if (ci_equals(operation, "REPEAT")) {
            CommandParser parser("string", "REPEAT");
            std::string input, count_str, out_var;

            parser.positional(input, "input string");
            parser.positional(count_str, "repeat count");
            parser.positional(out_var, "output variable");

            PARSE_OR_RETURN(parser, interp, sub_args);

            {
                auto count_opt = parse_number<int>(count_str);
                if (!count_opt) {
                    interp.set_fatal_error("string(REPEAT) invalid count: " + count_str);
                    return;
                }
                int count = *count_opt;
                if (count < 0) {
                    interp.set_fatal_error("string(REPEAT) count must be non-negative");
                    return;
                }

                std::string result;
                for (int i = 0; i < count; ++i) { result += input; }

                interp.set_variable(out_var, result);
                return;
            }

            // ========== Comparison ==========

        } else if (ci_equals(operation, "COMPARE")) {
            if (sub_args.empty()) {
                interp.set_fatal_error("string(COMPARE) requires an operator");
                return;
            }

            const auto& op_str = sub_args[0];
            std::span<const std::string> cmp_args(sub_args.begin() + 1, sub_args.end());

            // Hold the assembled subcommand label in a named local — the
            // parser stores it as string_view, so the buffer must outlive it.
            std::string compare_label = "COMPARE " + std::string(op_str);
            CommandParser parser("string", compare_label);
            std::string str1, str2, out_var;

            parser.positional(str1, "string1");
            parser.positional(str2, "string2");
            parser.positional(out_var, "output variable");

            PARSE_OR_RETURN(parser, interp, cmp_args);

            bool result = false;

            if (ci_equals(op_str, "LESS")) {
                result = str1 < str2;
            } else if (ci_equals(op_str, "GREATER")) {
                result = str1 > str2;
            } else if (ci_equals(op_str, "EQUAL")) {
                result = str1 == str2;
            } else if (ci_equals(op_str, "NOTEQUAL")) {
                result = str1 != str2;
            } else if (ci_equals(op_str, "LESS_EQUAL")) {
                result = str1 <= str2;
            } else if (ci_equals(op_str, "GREATER_EQUAL")) {
                result = str1 >= str2;
            } else {
                interp.set_fatal_error("string(COMPARE) unknown operator: " + std::string(op_str));
                return;
            }

            interp.set_variable(out_var, result ? "1" : "0");

            // ========== Generation ==========

        } else if (ci_equals(operation, "ASCII")) {
            CommandParser parser("string", "ASCII");
            std::string out_var;
            std::vector<std::string> codes;

            parser.positionals(codes, "codes");

            PARSE_OR_RETURN(parser, interp, sub_args);

            if (codes.empty()) {
                interp.set_fatal_error("string(ASCII) requires at least one character code");
                return;
            }

            // Last argument is the output variable
            out_var = codes.back();
            codes.pop_back();

            std::string result;
            for (const auto& code_str : codes) {
                auto code_opt = parse_number<int>(code_str);
                if (!code_opt) {
                    interp.set_fatal_error("string(ASCII) invalid code: " + code_str);
                    return;
                }
                int code = *code_opt;
                // XXX: apparently CMake allows ASCII codes outside the range [0, 127]
                // if (code < 0 || code > 127) {
                //     interp.set_fatal_error("string(ASCII) code out of range [0, 127]: " + code_str);
                //     return;
                // }
                result += static_cast<char>(code);
            }

            interp.set_variable(out_var, result);

        } else if (ci_equals(operation, "HEX")) {
            CommandParser parser("string", "HEX");
            std::string input, out_var;

            parser.positional(input, "input string");
            parser.positional(out_var, "output variable");

            PARSE_OR_RETURN(parser, interp, sub_args);

            std::string result;
            for (unsigned char c : input) { result += byte_to_hex(c); }

            interp.set_variable(out_var, result);

        } else if (ci_equals(operation, "CONFIGURE")) {
            CommandParser parser("string", "CONFIGURE");
            std::string input, out_var;
            bool at_only = false;
            bool escape_quotes = false;

            parser.positional(input, "input string");
            parser.positional(out_var, "output variable");
            parser.flag("@ONLY", at_only);
            parser.flag("ESCAPE_QUOTES", escape_quotes);

            PARSE_OR_RETURN(parser, interp, sub_args);

            std::string result = configure_string(input, interp, at_only, escape_quotes);
            interp.set_variable(out_var, result);

        } else if (ci_equals(operation, "MAKE_C_IDENTIFIER")) {
            CommandParser parser("string", "MAKE_C_IDENTIFIER");
            std::string input, out_var;

            parser.positional(input, "input string");
            parser.positional(out_var, "output variable");

            PARSE_OR_RETURN(parser, interp, sub_args);

            interp.set_variable(out_var, make_c_identifier(input));

        } else if (ci_equals(operation, "RANDOM")) {
            // Manual parsing for RANDOM since it has complex argument structure
            std::string length_str;
            std::string alphabet;
            std::string seed_str;
            std::string out_var;

            // Parse arguments manually
            size_t i = 0;
            while (i < sub_args.size()) {
                const auto& arg = sub_args[i];

                if (ci_equals(arg, "LENGTH") && i + 1 < sub_args.size()) {
                    length_str = sub_args[i + 1];
                    i += 2;
                } else if (ci_equals(arg, "ALPHABET") && i + 1 < sub_args.size()) {
                    alphabet = sub_args[i + 1];
                    i += 2;
                } else if (ci_equals(arg, "RANDOM_SEED") && i + 1 < sub_args.size()) {
                    seed_str = sub_args[i + 1];
                    i += 2;
                } else {
                    // This should be the output variable (last positional arg)
                    out_var = sub_args[i];
                    i++;
                }
            }

            if (out_var.empty()) {
                interp.set_fatal_error("string(RANDOM) requires an output variable");
                return;
            }

            size_t length = 5; // Default length
            if (!length_str.empty()) {
                auto len_opt = parse_number<size_t>(length_str);
                if (!len_opt) {
                    interp.set_fatal_error("string(RANDOM) invalid LENGTH: " + length_str);
                    return;
                }
                length = *len_opt;
            }

            if (alphabet.empty()) { alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"; }

            if (alphabet.empty()) {
                interp.set_fatal_error("string(RANDOM) alphabet cannot be empty");
                return;
            }

            std::random_device rd;
            std::mt19937 gen(rd());

            if (!seed_str.empty()) {
                auto seed_opt = parse_number<unsigned int>(seed_str);
                if (!seed_opt) {
                    interp.set_fatal_error("string(RANDOM) invalid RANDOM_SEED: " + seed_str);
                    return;
                }
                gen.seed(*seed_opt);
            }

            std::uniform_int_distribution<size_t> dis(0, alphabet.size() - 1);

            std::string result;
            for (size_t i = 0; i < length; ++i) { result += alphabet[dis(gen)]; }

            interp.set_variable(out_var, result);

        } else if (ci_equals(operation, "TIMESTAMP")) {
            CommandParser parser("string", "TIMESTAMP");
            std::string out_var, format;
            bool utc = false;
            std::vector<std::string> remaining_args;

            parser.positional(out_var, "output variable");
            parser.positionals(remaining_args, "args"); // For format string
            parser.flag("UTC", utc);

            PARSE_OR_RETURN(parser, interp, sub_args);

            // Extract format string (everything between out_var and UTC flag if present)
            if (sub_args.size() > 1) {
                // Check if last arg is UTC
                bool has_utc_flag = !sub_args.empty() && ci_equals(sub_args.back(), "UTC");
                size_t format_end = has_utc_flag ? sub_args.size() - 1 : sub_args.size();

                for (size_t i = 1; i < format_end; ++i) {
                    if (i > 1) format += " ";
                    format += sub_args[i];
                }

                if (has_utc_flag) { utc = true; }
            }

            if (format.empty()) { format = "%Y-%m-%dT%H:%M:%S"; }

            auto now = std::chrono::system_clock::now();
            std::time_t now_c = std::chrono::system_clock::to_time_t(now);
            std::tm* timeinfo = utc ? std::gmtime(&now_c) : std::localtime(&now_c);

            char buffer[256];
            std::strftime(buffer, sizeof(buffer), format.c_str(), timeinfo);

            interp.set_variable(out_var, std::string(buffer));

        } else if (ci_equals(operation, "UUID")) {
            // string(UUID <output_variable> NAMESPACE <namespace> NAME <name> TYPE <MD5|SHA1> [UPPER])
            // Generates a UUID v3 (MD5) or v5 (SHA1) per RFC 4122.
            CommandParser parser("string", "UUID");
            std::string out_var, ns_uuid, name_str, type_str;
            bool upper = false;

            parser.positional(out_var, "output variable");
            parser.value("NAMESPACE", ns_uuid);
            parser.value("NAME", name_str);
            parser.value("TYPE", type_str);
            parser.flag("UPPER", upper);
            PARSE_OR_RETURN(parser, interp, sub_args);

            if (ns_uuid.empty() || name_str.empty() || type_str.empty()) {
                interp.set_fatal_error("string(UUID) requires NAMESPACE, NAME, and TYPE arguments");
                return;
            }

            // Parse the namespace UUID string into 16 bytes (RFC 4122 binary form)
            // Expected format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
            std::string ns_clean;
            for (char c : ns_uuid) {
                if (c != '-') ns_clean += c;
            }
            if (ns_clean.size() != 32) {
                interp.set_fatal_error("string(UUID): NAMESPACE is not a valid UUID: " + ns_uuid);
                return;
            }
            unsigned char ns_bytes[16];
            for (int i = 0; i < 16; ++i) {
                unsigned int byte;
                if (std::sscanf(ns_clean.c_str() + i * 2, "%02x", &byte) != 1) {
                    interp.set_fatal_error("string(UUID): NAMESPACE contains invalid hex: " + ns_uuid);
                    return;
                }
                ns_bytes[i] = static_cast<unsigned char>(byte);
            }

            // Concatenate namespace bytes + name bytes
            std::vector<unsigned char> input_data(ns_bytes, ns_bytes + 16);
            input_data.insert(input_data.end(), name_str.begin(), name_str.end());

            // Hash and build UUID
            unsigned char hash_bytes[20]; // SHA1 is 20 bytes, MD5 is 16
            int version;
            if (ci_equals(type_str, "MD5")) {
                auto h = md5(input_data.data(), input_data.size());
                std::memcpy(hash_bytes, h.bytes, 16);
                version = 3;
            } else if (ci_equals(type_str, "SHA1")) {
                auto h = sha1(input_data.data(), input_data.size());
                std::memcpy(hash_bytes, h.bytes, 20);
                version = 5;
            } else {
                interp.set_fatal_error("string(UUID): TYPE must be MD5 or SHA1, got: " + type_str);
                return;
            }

            // Set version bits (4 bits in byte 6, high nibble)
            hash_bytes[6] = static_cast<unsigned char>((hash_bytes[6] & 0x0F) | (version << 4));
            // Set variant bits (2 bits in byte 8, high bits = 10)
            hash_bytes[8] = static_cast<unsigned char>((hash_bytes[8] & 0x3F) | 0x80);

            // Format as UUID: 8-4-4-4-12
            char uuid_str[37];
            std::snprintf(uuid_str, sizeof(uuid_str), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", hash_bytes[0],
                          hash_bytes[1], hash_bytes[2], hash_bytes[3], hash_bytes[4], hash_bytes[5], hash_bytes[6], hash_bytes[7],
                          hash_bytes[8], hash_bytes[9], hash_bytes[10], hash_bytes[11], hash_bytes[12], hash_bytes[13], hash_bytes[14],
                          hash_bytes[15]);

            std::string result(uuid_str);
            if (upper) {
                std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return std::toupper(c); });
            }

            interp.set_variable(out_var, result);

        } else if (ci_equals(operation, "SHA1")) {
            CommandParser parser("string", "SHA1");
            std::string input, out_var;

            parser.positional(out_var, "output variable");
            parser.positional(input, "input string");

            PARSE_OR_RETURN(parser, interp, sub_args);
            interp.set_variable(out_var, sha1(input).to_string());

        } else if (ci_equals(operation, "SHA256")) {
            CommandParser parser("string", "SHA256");
            std::string input, out_var;

            parser.positional(out_var, "output variable");
            parser.positional(input, "input string");

            PARSE_OR_RETURN(parser, interp, sub_args);
            interp.set_variable(out_var, sha256(input).to_string());

        } else if (ci_equals(operation, "MD5")) {
            CommandParser parser("string", "MD5");
            std::string input, out_var;

            parser.positional(out_var, "output variable");
            parser.positional(input, "input string");

            PARSE_OR_RETURN(parser, interp, sub_args);
            interp.set_variable(out_var, md5(input).to_string());

        } else if (ci_equals(operation, "BLAKE2B")) {
            CommandParser parser("string", "BLAKE2B");
            std::string input, out_var, key;

            parser.positional(out_var, "output variable");
            parser.positional(input, "input string");
            parser.positional(key, "key", false);

            PARSE_OR_RETURN(parser, interp, sub_args);
            interp.set_variable(out_var, blake2b(input, key).to_string());

        } else if (ci_equals(operation, "JSON")) {
            // string(JSON <out> [ERROR_VARIABLE <err>] GET|TYPE|LENGTH|MEMBER|EQUAL|SET|REMOVE <json> <path>...)
            if (sub_args.size() < 3) {
                interp.set_fatal_error("string(JSON) requires at least 3 arguments");
                return;
            }

            std::string out_var = std::string(sub_args[0]);
            size_t idx = 1;

            // Check for ERROR_VARIABLE
            std::string error_var;
            if (idx < sub_args.size() && ci_equals(sub_args[idx], "ERROR_VARIABLE")) {
                idx++;
                if (idx >= sub_args.size()) {
                    interp.set_fatal_error("string(JSON) ERROR_VARIABLE requires a variable name");
                    return;
                }
                error_var = std::string(sub_args[idx]);
                idx++;
            }

            if (idx >= sub_args.size()) {
                interp.set_fatal_error("string(JSON) missing subcommand (GET, LENGTH, TYPE, MEMBER, SET, REMOVE, EQUAL)");
                return;
            }

            std::string subcmd = std::string(sub_args[idx]);
            idx++;

            // Helper to report errors: either set error_var or fatal error
            auto report_error = [&](const std::string& msg) {
                if (!error_var.empty()) {
                    interp.set_variable(error_var, msg);
                    interp.set_variable(out_var, "NOTFOUND");
                } else {
                    interp.set_fatal_error("string(JSON) " + msg);
                }
            };

            auto clear_error = [&]() {
                if (!error_var.empty()) { interp.set_variable(error_var, "NOTFOUND"); }
            };

            if (idx >= sub_args.size()) {
                interp.set_fatal_error("string(JSON) missing JSON string argument");
                return;
            }

            std::string json_str = std::string(sub_args[idx]);
            idx++;

            // Parse the JSON string
            glz::generic json;
            auto ec = glz::read_json(json, json_str);
            if (ec) {
                report_error("parse error: " + glz::format_error(ec, json_str));
                return;
            }

            // Navigate to the target element using remaining path components
            auto navigate = [&](glz::generic& root, size_t path_start) -> glz::generic* {
                glz::generic* current = &root;
                for (size_t i = path_start; i < sub_args.size(); ++i) {
                    const auto& key = sub_args[i];
                    if (current->is_object()) {
                        auto& obj = current->get<glz::generic::object_t>();
                        auto it = obj.find(std::string(key));
                        if (it == obj.end()) {
                            report_error("member '" + std::string(key) + "' not found");
                            return nullptr;
                        }
                        current = &it->second;
                    } else if (current->is_array()) {
                        auto num = parse_number<size_t>(key);
                        if (!num) {
                            report_error("expected array index, got '" + std::string(key) + "'");
                            return nullptr;
                        }
                        auto& arr = current->get<glz::generic::array_t>();
                        if (*num >= arr.size()) {
                            report_error("array index " + std::string(key) + " out of range (size " + std::to_string(arr.size()) + ")");
                            return nullptr;
                        }
                        current = &arr[*num];
                    } else {
                        report_error("cannot index into non-container value");
                        return nullptr;
                    }
                }
                return current;
            };

            // Convert a json_t value to a CMake string
            auto to_cmake_string = [](const glz::generic& val) -> std::string {
                if (val.is_string()) {
                    return val.get<std::string>();
                } else if (val.is_number()) {
                    std::string buf;
                    if (auto wec = glz::write_json(val, buf); wec) return "";
                    return buf;
                } else if (val.holds<bool>()) {
                    return val.get<bool>() ? "true" : "false";
                } else if (val.holds<std::nullptr_t>()) {
                    return "null";
                } else {
                    // Array or object: serialize as JSON
                    std::string buf;
                    if (auto wec = glz::write_json(val, buf); wec) return "";
                    return buf;
                }
            };

            if (ci_equals(subcmd, "GET")) {
                auto* target = navigate(json, idx);
                if (!target) return;
                clear_error();
                interp.set_variable(out_var, to_cmake_string(*target));

            } else if (ci_equals(subcmd, "LENGTH")) {
                auto* target = navigate(json, idx);
                if (!target) return;
                size_t len = 0;
                if (target->is_array()) {
                    len = target->get<glz::generic::array_t>().size();
                } else if (target->is_object()) {
                    len = target->get<glz::generic::object_t>().size();
                } else {
                    report_error("LENGTH requires an array or object");
                    return;
                }
                clear_error();
                interp.set_variable(out_var, std::to_string(len));

            } else if (ci_equals(subcmd, "TYPE")) {
                auto* target = navigate(json, idx);
                if (!target) return;
                std::string type;
                if (target->holds<std::nullptr_t>())
                    type = "NULL";
                else if (target->holds<bool>())
                    type = "BOOLEAN";
                else if (target->is_number())
                    type = "NUMBER";
                else if (target->is_string())
                    type = "STRING";
                else if (target->is_array())
                    type = "ARRAY";
                else if (target->is_object())
                    type = "OBJECT";
                else
                    type = "NULL";
                clear_error();
                interp.set_variable(out_var, type);

            } else if (ci_equals(subcmd, "MEMBER")) {
                // Navigate to container, then get the Nth key name
                if (sub_args.size() < idx + 1) {
                    interp.set_fatal_error("string(JSON MEMBER) requires at least an index argument");
                    return;
                }
                // Last arg is the index, rest are path
                size_t member_idx_arg = sub_args.size() - 1;
                auto member_idx = parse_number<size_t>(sub_args[member_idx_arg]);
                if (!member_idx) {
                    report_error("MEMBER index must be a number");
                    return;
                }
                auto* target = navigate(json, idx);
                // If we navigated with the index as part of the path, back up
                // Actually: path is everything between json_str and the last arg
                // Re-navigate without the last arg
                glz::generic* container = &json;
                for (size_t i = idx; i < member_idx_arg; ++i) {
                    const auto& key = sub_args[i];
                    if (container->is_object()) {
                        auto& obj = container->get<glz::generic::object_t>();
                        auto it = obj.find(std::string(key));
                        if (it == obj.end()) {
                            report_error("member '" + std::string(key) + "' not found");
                            return;
                        }
                        container = &it->second;
                    } else if (container->is_array()) {
                        auto num = parse_number<size_t>(key);
                        if (!num) {
                            report_error("expected array index, got '" + std::string(key) + "'");
                            return;
                        }
                        auto& arr = container->get<glz::generic::array_t>();
                        if (*num >= arr.size()) {
                            report_error("array index out of range");
                            return;
                        }
                        container = &arr[*num];
                    } else {
                        report_error("cannot index into non-container");
                        return;
                    }
                }

                if (!container->is_object()) {
                    report_error("MEMBER requires an object");
                    return;
                }
                auto& obj = container->get<glz::generic::object_t>();
                if (*member_idx >= obj.size()) {
                    report_error("MEMBER index " + std::to_string(*member_idx) + " out of range (size " + std::to_string(obj.size()) + ")");
                    return;
                }
                auto it = obj.begin();
                std::advance(it, *member_idx);
                clear_error();
                interp.set_variable(out_var, it->first);

            } else if (ci_equals(subcmd, "EQUAL")) {
                // string(JSON <out> EQUAL <json1> <json2>)
                // Compare two JSON values
                if (sub_args.size() < idx + 1) {
                    interp.set_fatal_error("string(JSON EQUAL) requires two JSON strings");
                    return;
                }
                std::string json_str2 = std::string(sub_args[idx]);
                glz::generic json2;
                auto ec2 = glz::read_json(json2, json_str2);
                if (ec2) {
                    report_error("EQUAL: parse error in second JSON: " + glz::format_error(ec2, json_str2));
                    return;
                }
                clear_error();
                // Serialize both and compare (simple deep equality)
                std::string s1, s2;
                if (auto wec = glz::write_json(json, s1); wec) {
                    report_error("EQUAL: failed to serialize first JSON");
                    return;
                }
                if (auto wec = glz::write_json(json2, s2); wec) {
                    report_error("EQUAL: failed to serialize second JSON");
                    return;
                }
                interp.set_variable(out_var, s1 == s2 ? "TRUE" : "FALSE");

            } else if (ci_equals(subcmd, "SET")) {
                // string(JSON <out> SET <json> <path>... <new_value>)
                // Not commonly needed for gtest, but implement for completeness
                report_error("string(JSON SET) is not yet implemented");
                return;

            } else if (ci_equals(subcmd, "REMOVE")) {
                report_error("string(JSON REMOVE) is not yet implemented");
                return;

            } else {
                interp.set_fatal_error("string(JSON) unknown subcommand: " + subcmd);
                return;
            }

        } else {
            interp.set_fatal_error("string() unknown operation: " + operation);
            return;
        }
    });

    // Register separate_arguments() command
    interp.add_builtin("separate_arguments", [](Interpreter& interp, const std::vector<std::string>& args) {
        if (args.empty()) {
            interp.set_fatal_error("separate_arguments() requires at least one argument");
            return;
        }

        // Detect which form: simple conversion or advanced parsing
        // Simple form: separate_arguments(<var>)
        // Advanced form: separate_arguments(<variable> <mode> [PROGRAM [SEPARATE_ARGS]] <args>)

        std::string var_name = args[0];

        // Check if this is the simple form (only one argument)
        if (args.size() == 1) {
            // Simple form: replace spaces with semicolons in the variable's value
            auto entry = interp.get_variables().entry(var_name);
            const std::string& value = entry.get();
            std::string result;

            for (char c : value) {
                if (std::isspace(c)) {
                    result += ';';
                } else {
                    result += c;
                }
            }

            entry.set(std::move(result));
            return;
        }

        // Advanced form: requires at least 3 arguments (var, mode, args)
        if (args.size() < 3) {
            interp.set_fatal_error("separate_arguments() requires at least 3 arguments for advanced form: <variable> <mode> <args>");
            return;
        }

        const auto& mode = args[1];

        // Validate mode
        if (!ci_equals(mode, "UNIX_COMMAND") && !ci_equals(mode, "WINDOWS_COMMAND") && !ci_equals(mode, "NATIVE_COMMAND")) {
            interp.set_fatal_error("separate_arguments() invalid mode: " + args[1]
                                   + " (expected UNIX_COMMAND, WINDOWS_COMMAND, or NATIVE_COMMAND)");
            return;
        }

        // Determine actual parsing mode
        std::string parse_mode;
        if (ci_equals(mode, "NATIVE_COMMAND")) {
#ifdef _WIN32
            parse_mode = "WINDOWS_COMMAND";
#else
            parse_mode = "UNIX_COMMAND";
#endif
        } else if (ci_equals(mode, "UNIX_COMMAND")) {
            parse_mode = "UNIX_COMMAND";
        } else {
            parse_mode = "WINDOWS_COMMAND";
        }

        // Check for PROGRAM flag
        bool has_program = false;
        bool has_separate_args = false;
        size_t args_start = 2;

        if (args.size() > 2 && ci_equals(args[2], "PROGRAM")) {
            has_program = true;
            args_start = 3;

            // Check for SEPARATE_ARGS flag
            if (args.size() > 3 && ci_equals(args[3], "SEPARATE_ARGS")) {
                has_separate_args = true;
                args_start = 4;
            }
        }

        if (args_start >= args.size()) {
            interp.set_fatal_error("separate_arguments() missing command string argument");
            return;
        }

        // Concatenate remaining arguments to form the command string
        std::string command_str;
        for (size_t i = args_start; i < args.size(); ++i) {
            if (i > args_start) command_str += " ";
            command_str += args[i];
        }

        // Parse the command string according to the mode
        std::vector<std::string> parsed_args;
        if (parse_mode == "UNIX_COMMAND") {
            parsed_args = parse_unix_command(command_str);
        } else {
            parsed_args = parse_windows_command(command_str);
        }

        // Handle PROGRAM option
        if (has_program) {
            if (parsed_args.empty()) {
                // No program found, set empty list
                interp.set_variable(var_name, "");
                return;
            }

            std::string program = parsed_args[0];
            std::string program_path = find_program_in_path(program);

            if (program_path.empty()) {
                // Program not found, set empty list
                interp.set_variable(var_name, "");
                return;
            }

            if (has_separate_args) {
                // Return [absolute_path, arg1, arg2, ...]
                CMakeArray result;
                result.append(program_path);
                for (size_t i = 1; i < parsed_args.size(); ++i) { result.append(parsed_args[i]); }
                interp.set_variable(var_name, result.to_string());
            } else {
                // Return [absolute_path, remaining_args_as_string]
                CMakeArray result;
                result.append(program_path);

                if (parsed_args.size() > 1) {
                    std::string remaining;
                    for (size_t i = 1; i < parsed_args.size(); ++i) {
                        if (i > 1) remaining += " ";
                        remaining += parsed_args[i];
                    }
                    result.append(remaining);
                }

                interp.set_variable(var_name, result.to_string());
            }
        } else {
            // No PROGRAM option, just return parsed args as list
            CMakeArray result;
            for (const auto& arg : parsed_args) { result.append(arg); }
            interp.set_variable(var_name, result.to_string());
        }
    });
}

} // namespace kiln
