#include "registry.hpp"
#include "../interperter.hpp"
#include "../policies.hpp"
#include "../command_parser.hpp"
#include "../utils.hpp"
#include "../parse_number.hpp"
#include <algorithm>
#include <unistd.h>
#include <limits.h>

namespace kiln {

void register_variable_builtins(Interpreter& interp) {
    interp.add_builtin("set", [](Interpreter& interp, const std::vector<std::string>& args) {
        if (args.empty()) {
            interp.set_fatal_error("set() requires at least one argument");
            return;
        }

        std::string var_name = args[0];

        // INVALID: set(CACHE{VAR} value) - reject this (case-insensitive)
        if (var_name.size() > 7 && ci_equals(std::string_view(var_name).substr(0, 6), "CACHE{") && var_name.back() == '}') {
            interp.set_fatal_error("set(CACHE{...} ...) is invalid. Use: set(VAR value CACHE TYPE \"doc\")");
            return;
        }

        // VALID: set(ENV{VAR} value) - case insensitive
        if (var_name.size() > 5 && ci_equals(std::string_view(var_name).substr(0, 4), "ENV{") && var_name.back() == '}') {
            // Extract variable name (preserve original case from inside {})
            std::string env_var_name = var_name.substr(4, var_name.size() - 5);

            if (args.size() == 1) {
                // set(ENV{VAR}) with no value = unset
                unsetenv(env_var_name.c_str());
            } else {
                // Combine remaining arguments into value
                std::vector<std::string> value_args(args.begin() + 1, args.end());
                CMakeArray value_list(value_args);
                setenv(env_var_name.c_str(), value_list.to_string().c_str(), 1);
            }
            return;
        }

        // Check for CACHE keyword (case-insensitive)
        auto cache_it = std::find_if(args.begin() + 1, args.end(), [](const std::string& s) { return ci_equals(s, "CACHE"); });

        // Check for PARENT_SCOPE keyword (case-insensitive)
        auto parent_it = std::find_if(args.begin() + 1, args.end(), [](const std::string& s) { return ci_equals(s, "PARENT_SCOPE"); });

        // Cannot use both CACHE and PARENT_SCOPE
        if (cache_it != args.end() && parent_it != args.end()) {
            interp.set_fatal_error("set() cannot use both CACHE and PARENT_SCOPE");
            return;
        }

        // Handle: set(VAR value CACHE TYPE "doc" [FORCE])
        if (cache_it != args.end()) {
            // Value is everything between var_name and CACHE keyword
            std::vector<std::string> value_args(args.begin() + 1, cache_it);
            CMakeArray value_list(value_args);
            std::string value = value_list.to_string();

            // Parse TYPE and docstring after CACHE
            auto type_it = cache_it + 1;
            std::string cache_type;
            if (type_it != args.end()) { cache_type = *type_it; }

            // Check for FORCE keyword
            bool force = false;
            for (auto it = cache_it + 1; it != args.end(); ++it) {
                if (ci_equals(*it, "FORCE")) {
                    force = true;
                    break;
                }
            }

            // INTERNAL type always implies FORCE
            if (ci_equals(cache_type, "INTERNAL")) { force = true; }

            auto* root = interp.get_root();

            // Only set cache if entry doesn't exist or FORCE is given
            bool cache_existed = root->cache_variables_.find(var_name) != root->cache_variables_.end();
            if (force || !cache_existed) { root->cache_variables_[var_name] = value; }

            // CMP0126 OLD behavior: remove the normal variable of the
            // same name ONLY when the cache entry was just created, or
            // FORCE/INTERNAL was used.  When the cache entry already
            // existed (and no FORCE), the normal variable is preserved.
            KILN_POLICY_OLD(CMP0126);
            if (interp.get_policy(CMakePolicy::CMP0126) == PolicyState::OLD) {
                if (force || !cache_existed) { interp.unset_variable(var_name); }
            }

            return;
        }

        // Handle: set(VAR value PARENT_SCOPE)
        if (parent_it != args.end()) {
            // Value is everything between var_name and PARENT_SCOPE keyword
            std::vector<std::string> value_args(args.begin() + 1, parent_it);
            CMakeArray value_list(value_args);
            std::string value = value_list.to_string();

            // Set in parent scope (handles both function and subdirectory contexts)
            auto result = interp.set_variable_parent_scope(var_name, value);
            if (!result) {
                // CMake issues a dev warning (not fatal error) when no parent scope exists
                interp.print_message("AUTHOR_WARNING", "Cannot set \"" + var_name + "\": current scope has no parent.", false);
            }
            return;
        }

        // Warn if trying to modify CMAKE_BUILD_TYPE during script execution
        if (var_name == "CMAKE_BUILD_TYPE") {
            interp.print_message("WARN",
                                 "Modifying CMAKE_BUILD_TYPE in CMakeLists.txt is NOT RECOMMENDED. "
                                 "Use --config flag to set the build configuration.",
                                 false);
        }

        // Validate CMAKE_LINKER_TYPE
        if (var_name == "CMAKE_LINKER_TYPE" && args.size() > 1) {
            const auto& linker_type = args[1];

            if (!ci_equals(linker_type, "BFD") && !ci_equals(linker_type, "GOLD") && !ci_equals(linker_type, "MOLD")
                && !ci_equals(linker_type, "LLD")) {
                interp.set_fatal_error("Invalid CMAKE_LINKER_TYPE: " + linker_type + ". Must be one of: BFD, GOLD, MOLD, LLD");
                return;
            }
        }

        // Regular: set(VAR value)
        // Special case: set(VAR) with no value is equivalent to unset(VAR)
        if (args.size() == 1) {
            interp.unset_variable(var_name);
            return;
        }

        std::vector<std::string> value_args(args.begin() + 1, args.end());
        CMakeArray value_list(value_args);
        std::string value = value_list.to_string();

        interp.set_variable(var_name, value);
    });

    interp.add_builtin("unset", [](Interpreter& interp, const std::vector<std::string>& args) {
        if (args.empty()) {
            interp.set_fatal_error("unset() requires at least one argument");
            return;
        }

        std::string var_name = args[0];

        // INVALID: unset(CACHE{VAR}) - reject this (case-insensitive)
        if (var_name.size() > 7 && ci_equals(std::string_view(var_name).substr(0, 6), "CACHE{") && var_name.back() == '}') {
            interp.set_fatal_error("unset(CACHE{...}) is invalid. Use: unset(VAR CACHE)");
            return;
        }

        // VALID: unset(ENV{VAR}) - case insensitive
        if (var_name.size() > 5 && ci_equals(std::string_view(var_name).substr(0, 4), "ENV{") && var_name.back() == '}') {
            // Extract variable name (preserve original case from inside {})
            std::string env_var_name = var_name.substr(4, var_name.size() - 5);
            unsetenv(env_var_name.c_str());
            return;
        }

        // Check for CACHE keyword (case-insensitive)
        auto cache_it = std::find_if(args.begin() + 1, args.end(), [](const std::string& s) { return ci_equals(s, "CACHE"); });

        // Check for PARENT_SCOPE keyword (case-insensitive)
        auto parent_it = std::find_if(args.begin() + 1, args.end(), [](const std::string& s) { return ci_equals(s, "PARENT_SCOPE"); });

        // Cannot use both CACHE and PARENT_SCOPE
        if (cache_it != args.end() && parent_it != args.end()) {
            interp.set_fatal_error("unset() cannot use both CACHE and PARENT_SCOPE");
            return;
        }

        // Handle: unset(VAR CACHE)
        if (cache_it != args.end()) {
            interp.get_root()->cache_variables_.erase(var_name);
            return;
        }

        // Handle: unset(VAR PARENT_SCOPE)
        if (parent_it != args.end()) {
            auto result = interp.unset_variable_parent_scope(var_name);
            if (!result) {
                // CMake issues a dev warning (not fatal error) when no parent scope exists
                interp.print_message("AUTHOR_WARNING", "Cannot unset \"" + var_name + "\": current scope has no parent.", false);
            }
            return;
        }

        // Regular: unset(VAR)
        interp.unset_variable(var_name);
    });

    interp.add_builtin("option", [](Interpreter& interp, const std::vector<std::string>& args) {
        CommandParser parser("option");
        std::string option_name;
        std::string help_message;
        std::string initial_value;
        parser.positional(option_name, "option name");
        parser.positional(help_message, "help message");
        parser.positional(initial_value, "initial value", false);
        PARSE_OR_RETURN(parser, interp, args);

        std::string value = Interpreter::is_truthy(initial_value) ? "ON" : "OFF";
        // option() creates a cache variable (BOOL type). Only set if not already
        // in the cache — matching CMake's behavior where option() checks the cache,
        // not normal variables. Normal variables do NOT prevent option() from
        // creating its cache entry.
        if (interp.get_cache_variables().find(option_name) == interp.get_cache_variables().end()) {
            interp.set_cache_variable(option_name, value);
        }
    });

    interp.add_builtin("cmake_parse_arguments", [](Interpreter& interp, const std::vector<std::string>& args) {
        // Parse two forms:
        // 1. PARSE_ARGV <N> <prefix> <options> <one_value> <multi_value>
        // 2. <prefix> <options> <one_value> <multi_value> <args>...

        if (args.empty()) {
            interp.set_fatal_error("cmake_parse_arguments() requires at least 1 argument");
            return;
        }

        std::string prefix;
        CMakeArray options, one_value_keywords, multi_value_keywords;
        std::vector<std::string> to_parse;

        // Determine which form we're using
        bool is_parse_argv = (args[0] == "PARSE_ARGV");

        if (is_parse_argv) {
            // PARSE_ARGV form: PARSE_ARGV <N> <prefix> <options> <one_value> <multi_value>
            if (args.size() < 6) {
                interp.set_fatal_error("cmake_parse_arguments(PARSE_ARGV ...) requires 6 arguments");
                return;
            }

            // Parse N (starting index)
            auto start_idx_opt = parse_number<int>(args[1]);
            if (!start_idx_opt) {
                interp.set_fatal_error("cmake_parse_arguments(PARSE_ARGV): index must be a number");
                return;
            }
            int start_idx = *start_idx_opt;

            if (start_idx < 0) {
                interp.set_fatal_error("cmake_parse_arguments(PARSE_ARGV): index cannot be negative");
                return;
            }

            prefix = args[2];
            options = CMakeArray(args[3]);
            one_value_keywords = CMakeArray(args[4]);
            multi_value_keywords = CMakeArray(args[5]);

            // Read arguments from ARGV variables
            std::string argc_str = interp.get_variable("ARGC");
            int argc = 0;
            if (!argc_str.empty()) { argc = parse_number<int>(argc_str).value_or(0); }

            // Collect arguments from ARGV{start_idx} to ARGV{argc-1}
            for (int i = start_idx; i < argc; ++i) {
                std::string argv_var = "ARGV" + std::to_string(i);
                std::string value = interp.get_variable(argv_var);
                // ARGV variables preserve semicolons, so we add them as-is
                to_parse.push_back(value);
            }
        } else {
            // Standard form: <prefix> <options> <one_value> <multi_value> <args>...
            if (args.size() < 4) {
                interp.set_fatal_error("cmake_parse_arguments() requires at least 4 arguments");
                return;
            }

            prefix = args[0];
            options = CMakeArray(args[1]);
            one_value_keywords = CMakeArray(args[2]);
            multi_value_keywords = CMakeArray(args[3]);

            // Remaining arguments are what we parse. Each arg may itself be a
            // CMake list (e.g. "${ARGN}" expands to "FOO;BAR;BAZ" as one arg);
            // flatten on ';' so keyword matching works as in CMake.
            for (auto it = args.begin() + 4; it != args.end(); ++it) {
                if (it->empty()) continue;
                size_t start = 0;
                for (size_t i = 0; i <= it->size(); ++i) {
                    if (i == it->size() || (*it)[i] == ';') {
                        to_parse.push_back(it->substr(start, i - start));
                        start = i + 1;
                    }
                }
            }
        }

        // Initialize all option variables to FALSE
        for (const auto& opt : options) {
            if (!opt.empty()) { interp.set_variable(prefix + "_" + opt, "FALSE"); }
        }

        // Unset all one-value keyword variables (undefined if not provided)
        for (const auto& kw : one_value_keywords) {
            if (!kw.empty()) { interp.unset_variable(prefix + "_" + kw); }
        }

        // Unset all multi-value keyword variables (undefined if not provided)
        for (const auto& kw : multi_value_keywords) {
            if (!kw.empty()) { interp.unset_variable(prefix + "_" + kw); }
        }

        // Track unparsed arguments and keywords missing values
        std::vector<std::string> unparsed;
        std::vector<std::string> keywords_missing_values;

        // Parse the arguments
        for (size_t i = 0; i < to_parse.size();) {
            std::string arg = to_parse[i];

            // Check if it's an option (boolean flag)
            if (options.contains(arg)) {
                interp.set_variable(prefix + "_" + arg, "TRUE");
                i++;
                continue;
            }

            // Check if it's a one-value keyword
            if (one_value_keywords.contains(arg)) {
                if (i + 1 < to_parse.size()) {
                    // Check if next argument is also a keyword - if so, this keyword has no value
                    const std::string& next_arg = to_parse[i + 1];
                    if (options.contains(next_arg) || one_value_keywords.contains(next_arg) || multi_value_keywords.contains(next_arg)) {
                        // Next arg is a keyword, so this one-value keyword has no value
                        interp.set_variable(prefix + "_" + arg, "");
                        keywords_missing_values.push_back(arg);
                        i++;
                    } else {
                        // Next argument is the value
                        interp.set_variable(prefix + "_" + arg, to_parse[i + 1]);
                        i += 2;
                    }
                } else {
                    // Keyword at end with no value
                    interp.set_variable(prefix + "_" + arg, "");
                    keywords_missing_values.push_back(arg);
                    i++;
                }
                continue;
            }

            // Check if it's a multi-value keyword
            if (multi_value_keywords.contains(arg)) {
                std::string keyword = arg;
                i++; // Move past the keyword

                std::vector<std::string> values;
                // Collect values until we hit another keyword or end
                while (i < to_parse.size() && !options.contains(to_parse[i]) && !one_value_keywords.contains(to_parse[i])
                       && !multi_value_keywords.contains(to_parse[i])) {
                    values.push_back(to_parse[i]);
                    i++;
                }

                // Append to existing values (CMake appends on duplicate keywords)
                std::string var_name = prefix + "_" + keyword;
                std::string existing = interp.get_variable(var_name);
                if (!existing.empty() && !values.empty()) {
                    CMakeArray combined(existing);
                    for (auto& v : values) combined.append(std::move(v));
                    interp.set_variable(var_name, combined.to_string());
                } else {
                    CMakeArray value_list(values);
                    interp.set_variable(var_name, value_list.to_string());
                }
                continue;
            }

            // Not a keyword - add to unparsed
            unparsed.push_back(arg);
            i++;
        }

        // Set special output variables - only define when non-empty (CMake behavior)
        if (!unparsed.empty()) {
            CMakeArray unparsed_list(unparsed);
            interp.set_variable(prefix + "_UNPARSED_ARGUMENTS", unparsed_list.to_string());
        } else {
            interp.unset_variable(prefix + "_UNPARSED_ARGUMENTS");
        }

        if (!keywords_missing_values.empty()) {
            CMakeArray missing_list(keywords_missing_values);
            interp.set_variable(prefix + "_KEYWORDS_MISSING_VALUES", missing_list.to_string());
        } else {
            interp.unset_variable(prefix + "_KEYWORDS_MISSING_VALUES");
        }
    });

    interp.add_builtin("variable_watch", [](Interpreter& interp, const std::vector<std::string>& args) {
        if (args.empty()) {
            interp.set_fatal_error("variable_watch() requires at least one argument (variable name)");
            return;
        }
        if (args.size() > 2) {
            interp.set_fatal_error("variable_watch() takes at most 2 arguments (variable name, optional callback)");
            return;
        }

        std::optional<std::string> callback;
        if (args.size() == 2) { callback = args[1]; }
        interp.add_variable_watch(args[0], std::move(callback));
    });

    interp.add_builtin("site_name", [](Interpreter& interp, const std::vector<std::string>& args) {
        if (args.size() != 1) {
            interp.set_fatal_error("site_name() requires exactly one argument");
            return;
        }

        std::string var_name = args[0];
        std::string hostname;

        // Get hostname using gethostname()
#ifdef __FreeBSD__
        char buffer[_POSIX_HOST_NAME_MAX + 1];
#else
        char buffer[HOST_NAME_MAX + 1];
#endif
        if (gethostname(buffer, sizeof(buffer)) == 0) {
            hostname = buffer;
        } else {
            // Fallback: try HOSTNAME environment variable
            if (const char* env_hostname = std::getenv("HOSTNAME")) { hostname = env_hostname; }
            // If both fail, hostname remains empty
        }

        interp.set_variable(var_name, hostname);
    });
}

} // namespace kiln
