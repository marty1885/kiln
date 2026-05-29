#include "registry.hpp"
#include "../interperter.hpp"
#include "../policies.hpp"
#include "../command_parser.hpp"
#include "../utils.hpp"
#include "../language.hpp"
#include "../CMakeArray.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include "../regex.hpp"
#include <cstdlib>
#include <cstring>

namespace kiln {

namespace {

// Enable a single language in the interpreter and toolchain.
// Returns empty string on success, or an error message on failure.
std::string enable_language_impl(Interpreter& interp, const std::string& lang) {
    if (lang == "NONE") return {};
    return interp.enable_compiler_for_language(lang);
}

// Check if a value is falsy for #cmakedefine purposes
// This is simpler than Interpreter::is_falsy - it only checks known false constants,
// without the buggy "invalid number" logic that treats "-Wall" as falsy
bool is_falsy_for_cmakedefine(const std::string& val) {
    if (val.empty()) return true;

    // False constants (case-insensitive)
    if (val == "0" || ci_equals(val, "OFF") || ci_equals(val, "NO") || ci_equals(val, "FALSE") || ci_equals(val, "N")
        || ci_equals(val, "IGNORE") || ci_equals(val, "NOTFOUND")
        || (val.size() >= 9 && ci_equals(std::string_view(val).substr(val.size() - 9), "-NOTFOUND"))) {
        return true;
    }

    return false;
}

// Substitute @VAR@ patterns in content
std::string substitute_at_vars(Interpreter& interp, const std::string& content, bool escape_quotes) {
    std::string result;
    result.reserve(content.size());

    size_t i = 0;
    while (i < content.size()) {
        if (content[i] == '@') {
            size_t end = content.find('@', i + 1);
            if (end != std::string::npos) {
                std::string var_name = content.substr(i + 1, end - i - 1);
                // Variable names must be valid identifiers
                bool valid = !var_name.empty() && (std::isalpha(static_cast<unsigned char>(var_name[0])) || var_name[0] == '_');
                for (size_t j = 1; valid && j < var_name.size(); ++j) {
                    valid = std::isalnum(static_cast<unsigned char>(var_name[j])) || var_name[j] == '_';
                }
                if (valid) {
                    std::string value = interp.get_variable(var_name);
                    if (escape_quotes) {
                        for (char c : value) {
                            if (c == '"')
                                result += "\\\"";
                            else
                                result += c;
                        }
                    } else {
                        result += value;
                    }
                    i = end + 1;
                    continue;
                }
            }
        }
        result += content[i++];
    }

    return result;
}

// Substitute $<prefix>{<name>} patterns
// prefix can be empty (regular var), ENV, or CACHE
std::string substitute_dollar_vars(Interpreter& interp, const std::string& content, bool escape_quotes) {
    std::string result;
    result.reserve(content.size());

    size_t i = 0;
    while (i < content.size()) {
        if (content[i] == '$' && i + 1 < content.size()) {
            // Find the opening brace
            size_t brace_pos = content.find('{', i + 1);
            if (brace_pos != std::string::npos) {
                std::string prefix = content.substr(i + 1, brace_pos - i - 1);
                // Valid prefixes: empty, ENV, CACHE
                if (prefix.empty() || prefix == "ENV" || prefix == "CACHE") {
                    size_t end = content.find('}', brace_pos + 1);
                    if (end != std::string::npos) {
                        std::string var_name = content.substr(brace_pos + 1, end - brace_pos - 1);
                        std::string value;

                        if (prefix == "ENV") {
                            const char* env_val = std::getenv(var_name.c_str());
                            value = env_val ? env_val : "";
                        } else {
                            // Both empty prefix and CACHE use get_variable
                            value = interp.get_variable(var_name);
                        }

                        if (escape_quotes) {
                            for (char c : value) {
                                if (c == '"')
                                    result += "\\\"";
                                else
                                    result += c;
                            }
                        } else {
                            result += value;
                        }
                        i = end + 1;
                        continue;
                    }
                }
            }
        }
        result += content[i++];
    }

    return result;
}

// Process #cmakedefine and #cmakedefine01 directives
std::string process_cmakedefine(Interpreter& interp, const std::string& content) {
    static auto cmakedefine_re = Regex::compile_match(R"((\s*)#(\s*)cmakedefine\s+(\w+)(.*))").value();
    static auto cmakedefine01_re = Regex::compile_match(R"((\s*)#(\s*)cmakedefine01\s+(\w+)\s*)").value();

    std::istringstream stream(content);
    std::ostringstream result;
    std::string line;

    bool first_line = true;
    while (std::getline(stream, line)) {
        if (!first_line) result << '\n';
        first_line = false;

        std::vector<std::string> captures;

        if (cmakedefine01_re.match(line, captures)) {
            std::string leading = captures[1];
            std::string hash_space = captures[2];
            std::string var_name = captures[3];

            std::string value = interp.get_variable(var_name);
            bool is_true = !is_falsy_for_cmakedefine(value);

            result << leading << "#" << hash_space << "define " << var_name << " " << (is_true ? "1" : "0");
        } else if (cmakedefine_re.match(line, captures)) {
            std::string leading = captures[1];
            std::string hash_space = captures[2];
            std::string var_name = captures[3];
            std::string rest = captures[4];

            auto opt_value = interp.get_optional_variable(var_name);
            std::string value = opt_value.value_or("");
            // Variable is "defined" for #cmakedefine if it exists AND is truthy
            bool is_defined = opt_value.has_value() && !is_falsy_for_cmakedefine(value);

            if (is_defined) {
                if (kiln::strip(rest).empty()) {
                    // No value after variable name
                    if (value.empty() || value == "ON" || value == "TRUE" || value == "YES" || value == "1") {
                        result << leading << "#" << hash_space << "define " << var_name;
                    } else {
                        result << leading << "#" << hash_space << "define " << var_name << " " << value;
                    }
                } else {
                    result << leading << "#" << hash_space << "define " << var_name << rest;
                }
            } else {
                result << "/* " << leading << "#" << hash_space << "undef " << var_name << " */";
            }
        } else {
            result << line;
        }
    }

    return result.str();
}

// Convert line endings
std::string convert_newlines(const std::string& content, const std::string& style) {
    std::string result;
    result.reserve(content.size());

    bool use_crlf = (style == "DOS" || style == "WIN32" || style == "CRLF");

    for (size_t i = 0; i < content.size(); ++i) {
        if (content[i] == '\r') {
            if (i + 1 < content.size() && content[i + 1] == '\n') {
                continue; // Skip \r before \n
            }
            // Standalone \r
            if (use_crlf)
                result += "\r\n";
            else
                result += '\n';
        } else if (content[i] == '\n') {
            if (use_crlf)
                result += "\r\n";
            else
                result += '\n';
        } else {
            result += content[i];
        }
    }

    return result;
}

} // anonymous namespace

void register_project_builtins(Interpreter& interp) {
    interp.add_builtin("cmake_minimum_required", [](Interpreter& interp, const std::vector<std::string>& args) {
        for (size_t i = 0; i < args.size(); ++i) {
            if (args[i] == "VERSION" && i + 1 < args.size()) {
                std::string version = args[i + 1];
                // Handle VERSION x...y range syntax — use the minimum
                if (auto dots = version.find("..."); dots != std::string::npos) { version = version.substr(0, dots); }
                interp.set_variable("CMAKE_MINIMUM_REQUIRED_VERSION", version);

                // Check that the requested version doesn't exceed what we support
                auto our_version = interp.get_variable("CMAKE_VERSION");
                if (!our_version.empty() && compare_versions(version, our_version) > 0) {
                    interp.set_fatal_error("CMake " + version + " or higher is required.  You are running version "
                                           + std::string(our_version));
                    return;
                }

                // Set policy defaults based on the requested version.
                interp.set_policies_for_version(version);
                break;
            }
        }
    });
    interp.add_builtin("source_group", [](Interpreter&, const std::vector<std::string>&) {});

    interp.add_builtin("project", [](Interpreter& interp, const std::vector<std::string>& args) {
        if (args.empty()) {
            interp.set_fatal_error("project() requires at least one argument");
            return;
        }

        interp.mark_project_called();

        std::string project_name = args[0];
        interp.set_variable("PROJECT_NAME", project_name);

        // CMAKE_PROJECT_NAME tracks the top-level project name
        if (interp.get_variable("CMAKE_PROJECT_NAME").empty()) { interp.set_variable("CMAKE_PROJECT_NAME", project_name); }
        interp.set_variable("PROJECT_SOURCE_DIR", interp.get_variable("CMAKE_CURRENT_SOURCE_DIR"));
        interp.set_variable("PROJECT_BINARY_DIR", interp.get_variable("CMAKE_CURRENT_BINARY_DIR"));

        // CMake also sets <ProjectName>_SOURCE_DIR and <ProjectName>_BINARY_DIR
        interp.set_variable(project_name + "_SOURCE_DIR", interp.get_variable("CMAKE_CURRENT_SOURCE_DIR"));
        interp.set_variable(project_name + "_BINARY_DIR", interp.get_variable("CMAKE_CURRENT_BINARY_DIR"));

        // PROJECT_IS_TOP_LEVEL / <ProjectName>_IS_TOP_LEVEL: TRUE iff this
        // project() call is in the top source directory (i.e. not under
        // add_subdirectory or FetchContent). Modern projects key install/test
        // rules on this so they no-op when consumed as a subproject.
        const std::string tl_value = interp.in_top_source_dir() ? "TRUE" : "FALSE";
        interp.set_variable("PROJECT_IS_TOP_LEVEL", tl_value);
        interp.set_variable(project_name + "_IS_TOP_LEVEL", tl_value);

        std::vector<std::string> languages;
        std::string version;
        std::string description;
        std::string homepage_url;

        // Parse arguments
        // NOTE: CMake docs say VERSION/DESCRIPTION/HOMEPAGE_URL require values,
        // but in practice CMake only warns when a keyword's value expands to
        // nothing (e.g. VERSION ${SOME_EMPTY_VAR}). We match that behavior.
        for (size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "VERSION") {
                if (i + 1 < args.size() && args[i + 1] != "LANGUAGES" && args[i + 1] != "DESCRIPTION" && args[i + 1] != "HOMEPAGE_URL") {
                    version = args[++i];
                } else {
                    interp.print_message("WARNING",
                                         "VERSION keyword not followed by a value or was followed by a value that expanded to nothing.");
                }
            } else if (args[i] == "LANGUAGES") {
                // Collect all languages until next keyword
                ++i;
                while (i < args.size() && args[i] != "VERSION" && args[i] != "DESCRIPTION" && args[i] != "HOMEPAGE_URL") {
                    languages.push_back(args[i++]);
                }
                --i; // Adjust for loop increment
            } else if (args[i] == "DESCRIPTION") {
                if (i + 1 < args.size() && args[i + 1] != "LANGUAGES" && args[i + 1] != "VERSION" && args[i + 1] != "HOMEPAGE_URL") {
                    description = args[++i];
                } else {
                    interp.print_message(
                        "WARNING", "DESCRIPTION keyword not followed by a value or was followed by a value that expanded to nothing.");
                }
            } else if (args[i] == "HOMEPAGE_URL") {
                if (i + 1 < args.size() && args[i + 1] != "LANGUAGES" && args[i + 1] != "VERSION" && args[i + 1] != "DESCRIPTION") {
                    homepage_url = args[++i];
                } else {
                    interp.print_message(
                        "WARNING", "HOMEPAGE_URL keyword not followed by a value or was followed by a value that expanded to nothing.");
                }
            }
            // If no keyword, treat as language (for backward compatibility)
            // e.g. project(test C CXX) without the LANGUAGES keyword
            else if (args[i] != "VERSION" && args[i] != "DESCRIPTION" && args[i] != "HOMEPAGE_URL" && args[i] != "LANGUAGES") {
                languages.push_back(args[i]);
            }
        }

        // Validate languages (default is C and CXX if not specified)
        if (languages.empty()) { languages = {"C", "CXX"}; }

        // CMAKE_PROJECT_<NAME>_INCLUDE_BEFORE: included after project name vars
        // are set but before toolchain/language work. ladybird uses this to
        // populate VCPKG_TARGET_TRIPLET (and pick the dynamic triplet on Linux)
        // before vcpkg's toolchain file gets a chance to lock in a default.
        auto include_hook = [&](const std::string& var) {
            std::string file = interp.get_variable(var);
            if (file.empty()) return;
            auto res = interp.include_file(file);
            if (!res) { interp.set_fatal_error("project() failed to load " + var + ": " + file + ": " + res.error().message); }
        };
        include_hook("CMAKE_PROJECT_" + project_name + "_INCLUDE_BEFORE");

        // Load CMAKE_TOOLCHAIN_FILE once, before any language enablement, so
        // it can set CMAKE_<LANG>_COMPILER, CMAKE_SYSROOT, etc. Relative
        // paths are resolved against CMAKE_SOURCE_DIR (top-level), matching
        // CMake's behavior.
        if (!interp.toolchain_file_loaded()) {
            std::string tc = interp.get_variable("CMAKE_TOOLCHAIN_FILE");
            if (!tc.empty()) {
                std::filesystem::path tc_path(tc);
                if (!tc_path.is_absolute()) {
                    std::string src = interp.get_variable("CMAKE_SOURCE_DIR");
                    if (src.empty()) src = interp.get_variable("CMAKE_CURRENT_SOURCE_DIR");
                    tc_path = std::filesystem::path(src) / tc_path;
                }
                interp.mark_toolchain_file_loaded();
                auto res = interp.include_file(tc_path.lexically_normal().string());
                if (!res) {
                    interp.set_fatal_error("project() failed to load CMAKE_TOOLCHAIN_FILE: " + tc + ": " + res.error().message);
                    return;
                }
            } else {
                // Mark loaded even when empty so we don't re-check on every
                // nested project() call.
                interp.mark_toolchain_file_loaded();
            }
        }

        // Enable each language
        for (const auto& lang : languages) {
            auto err = enable_language_impl(interp, lang);
            if (!err.empty()) {
                interp.set_fatal_error("project() " + err);
                return;
            }
        }

        // Merge newly enabled languages into the global property
        // (a child project() must not remove languages enabled by the parent)
        {
            CMakeArray enabled_langs(interp.get_global_properties()["ENABLED_LANGUAGES"]);
            for (const auto& lang : languages) {
                if (!enabled_langs.contains(lang)) { enabled_langs.append(lang); }
            }
            interp.get_global_properties()["ENABLED_LANGUAGES"] = enabled_langs.to_string();
        }

        // Process VERSION - CMake always sets version variables, even when
        // VERSION is not provided (in which case they are set to empty string)
        bool is_top_level = (interp.get_variable("CMAKE_PROJECT_NAME") == project_name);

        if (!version.empty()) {
            // Parse version components: major[.minor[.patch[.tweak]]]
            std::vector<std::string> components;
            std::string component;
            for (char c : version) {
                if (c == '.') {
                    components.push_back(component);
                    component.clear();
                } else if (std::isdigit(static_cast<unsigned char>(c))) {
                    component += c;
                } else {
                    interp.set_fatal_error("project() VERSION contains invalid character: " + version);
                    return;
                }
            }
            if (!component.empty()) { components.push_back(component); }

            if (components.empty() || components.size() > 4) {
                interp.set_fatal_error("project() VERSION must have 1-4 components: " + version);
                return;
            }

            // Set version variables
            interp.set_variable("PROJECT_VERSION", version);
            interp.set_variable(project_name + "_VERSION", version);
            if (is_top_level) { interp.set_variable("CMAKE_PROJECT_VERSION", version); }

            const char* suffixes[] = {"_MAJOR", "_MINOR", "_PATCH", "_TWEAK"};
            for (size_t i = 0; i < 4; ++i) {
                std::string value = (i < components.size()) ? components[i] : "";
                interp.set_variable("PROJECT_VERSION" + std::string(suffixes[i]), value);
                interp.set_variable(project_name + "_VERSION" + std::string(suffixes[i]), value);
                if (is_top_level) { interp.set_variable("CMAKE_PROJECT_VERSION" + std::string(suffixes[i]), value); }
            }
        } else {
            // No VERSION provided - set all version variables to empty string
            interp.set_variable("PROJECT_VERSION", "");
            interp.set_variable(project_name + "_VERSION", "");
            if (is_top_level) { interp.set_variable("CMAKE_PROJECT_VERSION", ""); }

            const char* suffixes[] = {"_MAJOR", "_MINOR", "_PATCH", "_TWEAK"};
            for (size_t i = 0; i < 4; ++i) {
                interp.set_variable("PROJECT_VERSION" + std::string(suffixes[i]), "");
                interp.set_variable(project_name + "_VERSION" + std::string(suffixes[i]), "");
                if (is_top_level) { interp.set_variable("CMAKE_PROJECT_VERSION" + std::string(suffixes[i]), ""); }
            }
        }

        // Set DESCRIPTION variables
        interp.set_variable("PROJECT_DESCRIPTION", description);
        interp.set_variable(project_name + "_DESCRIPTION", description);
        if (is_top_level) { interp.set_variable("CMAKE_PROJECT_DESCRIPTION", description); }

        // Set HOMEPAGE_URL variables
        interp.set_variable("PROJECT_HOMEPAGE_URL", homepage_url);
        interp.set_variable(project_name + "_HOMEPAGE_URL", homepage_url);
        if (is_top_level) { interp.set_variable("CMAKE_PROJECT_HOMEPAGE_URL", homepage_url); }

        // CMAKE_PROJECT_<NAME>_INCLUDE / CMAKE_PROJECT_INCLUDE: late hooks,
        // after languages and version vars are populated. Per-name first.
        include_hook("CMAKE_PROJECT_" + project_name + "_INCLUDE");
        include_hook("CMAKE_PROJECT_INCLUDE");
    });

    interp.add_builtin("cmake_policy", [](Interpreter& interp, const std::vector<std::string>& args) {
        if (args.empty()) {
            interp.set_fatal_error("cmake_policy() requires at least one argument");
            return;
        }

        if (args[0] == "SET") {
            if (args.size() < 3) {
                interp.set_fatal_error("cmake_policy(SET) requires CMP#### and OLD|NEW");
                return;
            }
            auto policy = parse_cmake_policy(args[1]);
            if (!policy) return; // Unknown policies silently ignored
            if (args[2] == "OLD") {
                interp.set_policy(*policy, PolicyState::OLD);
            } else if (args[2] == "NEW") {
                interp.set_policy(*policy, PolicyState::NEW);
            }
        } else if (args[0] == "GET") {
            if (args.size() < 3) {
                interp.set_fatal_error("cmake_policy(GET) requires CMP#### and <variable>");
                return;
            }
            auto policy = parse_cmake_policy(args[1]);
            if (policy) {
                interp.set_variable(args[2], interp.get_policy(*policy) == PolicyState::NEW ? "NEW" : "OLD");
            } else {
                // Unknown policy: default to NEW (kiln defaults all policies to NEW
                // except ones explicitly known to need OLD behavior)
                interp.set_variable(args[2], "NEW");
            }
        } else if (args[0] == "PUSH") {
            interp.push_policies();
        } else if (args[0] == "POP") {
            interp.pop_policies();
        }
        // VERSION is silently accepted as no-op
    });
    interp.add_builtin("mark_as_advanced", [](Interpreter&, const std::vector<std::string>&) {});
    interp.add_builtin("include_regular_expression", [](Interpreter&, const std::vector<std::string>& args) {
        if (args.empty()) throw std::runtime_error("include_regular_expression requires at least 1 argument");
        if (args[0] != "^.*$") throw std::runtime_error("include_regular_expression: only the default \"^.*$\" pattern is supported");
    });

    interp.add_builtin("enable_language", [](Interpreter& interp, const std::vector<std::string>& args) {
        if (args.empty()) {
            interp.set_fatal_error("enable_language() requires at least one language argument");
            return;
        }

        // Parse arguments - look for OPTIONAL flag
        bool optional = false;
        std::vector<std::string> languages;

        for (const auto& arg : args) {
            if (arg == "OPTIONAL") {
                optional = true;
            } else {
                languages.push_back(arg);
            }
        }

        if (languages.empty()) {
            interp.set_fatal_error("enable_language() requires at least one language");
            return;
        }

        // Get current enabled languages
        std::string current = interp.get_global_properties()["ENABLED_LANGUAGES"];
        CMakeArray enabled_langs(current);

        // Enable each language
        for (const auto& lang : languages) {
            auto err = enable_language_impl(interp, lang);
            if (!err.empty()) {
                if (optional) continue;
                interp.set_fatal_error("enable_language() " + err);
                return;
            }
            if (!enabled_langs.contains(lang)) { enabled_langs.append(lang); }
        }

        // Update global property
        interp.get_global_properties()["ENABLED_LANGUAGES"] = enabled_langs.to_string();
    });

    interp.add_builtin("configure_file", [](Interpreter& interp, const std::vector<std::string>& args) {
        if (args.size() < 2) {
            interp.set_fatal_error("configure_file() requires input and output arguments");
            return;
        }

        // Parse arguments
        std::filesystem::path input_path = args[0];
        std::filesystem::path output_path = args[1];

        bool copyonly = false;
        bool at_only = false;
        bool escape_quotes = false;
        bool no_source_permissions = false;
        bool use_source_permissions = false;
        std::string newline_style;
        std::vector<std::string> file_permissions;

        for (size_t i = 2; i < args.size(); ++i) {
            if (args[i] == "COPYONLY") {
                copyonly = true;
            } else if (args[i] == "@ONLY") {
                at_only = true;
            } else if (args[i] == "ESCAPE_QUOTES") {
                escape_quotes = true;
            } else if (args[i] == "NO_SOURCE_PERMISSIONS") {
                no_source_permissions = true;
            } else if (args[i] == "USE_SOURCE_PERMISSIONS") {
                use_source_permissions = true;
            } else if (args[i] == "NEWLINE_STYLE") {
                if (i + 1 >= args.size()) {
                    interp.set_fatal_error("configure_file() NEWLINE_STYLE requires a value");
                    return;
                }
                newline_style = args[++i];
                if (newline_style != "UNIX" && newline_style != "DOS" && newline_style != "WIN32" && newline_style != "LF"
                    && newline_style != "CRLF") {
                    interp.set_fatal_error("configure_file() invalid NEWLINE_STYLE: " + newline_style);
                    return;
                }
            } else if (args[i] == "FILE_PERMISSIONS") {
                ++i;
                while (i < args.size() && args[i].find("_") != std::string::npos) { file_permissions.push_back(args[i++]); }
                --i; // Adjust for loop increment
            }
        }

        // Validate incompatible options
        if (copyonly && !newline_style.empty()) {
            interp.set_fatal_error("configure_file() COPYONLY and NEWLINE_STYLE are incompatible");
            return;
        }

        if ((no_source_permissions ? 1 : 0) + (use_source_permissions ? 1 : 0) + (!file_permissions.empty() ? 1 : 0) > 1) {
            interp.set_fatal_error("configure_file() permission options are mutually exclusive");
            return;
        }

        // Resolve paths
        if (!input_path.is_absolute()) { input_path = std::filesystem::path(interp.get_variable("CMAKE_CURRENT_SOURCE_DIR")) / input_path; }
        if (!output_path.is_absolute()) {
            output_path = std::filesystem::path(interp.get_variable("CMAKE_CURRENT_BINARY_DIR")) / output_path;
        }

        // Read input file
        std::ifstream infile(input_path, std::ios::binary);
        if (!infile) {
            interp.set_fatal_error("configure_file() could not open input file: " + input_path.string());
            return;
        }
        std::string content((std::istreambuf_iterator<char>(infile)), std::istreambuf_iterator<char>());
        infile.close();

        // Process content (unless COPYONLY)
        if (!copyonly) {
            // Process #cmakedefine directives first
            content = process_cmakedefine(interp, content);

            // Then substitute variables
            if (at_only) {
                content = substitute_at_vars(interp, content, escape_quotes);
            } else {
                content = substitute_at_vars(interp, content, escape_quotes);
                content = substitute_dollar_vars(interp, content, escape_quotes);
            }

            // Convert newlines if specified
            if (!newline_style.empty()) { content = convert_newlines(content, newline_style); }
        }

        // CMake's configure_file always ensures a trailing newline
        if (!content.empty() && content.back() != '\n') { content += '\n'; }

        // Create output directory if needed
        std::filesystem::create_directories(output_path.parent_path());

        // Determine target permissions
        namespace fs = std::filesystem;
        fs::perms target_perms;
        if (!file_permissions.empty()) {
            target_perms = fs::perms::none;
            for (const auto& p : file_permissions) {
                if (p == "OWNER_READ")
                    target_perms |= fs::perms::owner_read;
                else if (p == "OWNER_WRITE")
                    target_perms |= fs::perms::owner_write;
                else if (p == "OWNER_EXECUTE")
                    target_perms |= fs::perms::owner_exec;
                else if (p == "GROUP_READ")
                    target_perms |= fs::perms::group_read;
                else if (p == "GROUP_WRITE")
                    target_perms |= fs::perms::group_write;
                else if (p == "GROUP_EXECUTE")
                    target_perms |= fs::perms::group_exec;
                else if (p == "WORLD_READ")
                    target_perms |= fs::perms::others_read;
                else if (p == "WORLD_WRITE")
                    target_perms |= fs::perms::others_write;
                else if (p == "WORLD_EXECUTE")
                    target_perms |= fs::perms::others_exec;
            }
        } else if (no_source_permissions) {
            target_perms = fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read | fs::perms::others_read;
        } else {
            // USE_SOURCE_PERMISSIONS is the default
            target_perms = fs::status(input_path).permissions();
        }

        // Check if we need to write (content changed) or update permissions
        bool content_changed = true;
        bool perms_changed = true;
        if (fs::exists(output_path)) {
            // Check content
            std::ifstream existing_file(output_path, std::ios::binary);
            if (existing_file) {
                std::string existing_content((std::istreambuf_iterator<char>(existing_file)), std::istreambuf_iterator<char>());
                existing_file.close();

                Hash256 existing_hash = blake2b(existing_content);
                Hash256 new_hash = blake2b(content);
                content_changed = (memcmp(existing_hash.bytes, new_hash.bytes, 32) != 0);
            }

            // Check permissions
            auto existing_perms = fs::status(output_path).permissions();
            perms_changed = (existing_perms != target_perms);
        }

        // Only touch the file if content or permissions changed
        if (content_changed) {
            std::ofstream outfile(output_path, std::ios::binary);
            if (!outfile) {
                interp.set_fatal_error("configure_file() could not open output file: " + output_path.string());
                return;
            }
            outfile << content;
            outfile.close();
        }

        if (perms_changed || content_changed) { fs::permissions(output_path, target_perms); }

        // Mark output as generated source file
        auto& source_props = interp.get_source_properties();
        std::string normalized_output = output_path.lexically_normal().string();
        source_props[normalized_output]["GENERATED"] = "TRUE";
    });

    interp.add_builtin("aux_source_directory", [](Interpreter& interp, const std::vector<std::string>& args) {
        if (args.size() < 2) {
            interp.set_fatal_error("aux_source_directory() requires <dir> and <variable> arguments");
            return;
        }

        std::string directory = args[0];
        std::string variable = args[1];

        // Resolve directory path
        std::filesystem::path dir_path = directory;
        if (!dir_path.is_absolute()) { dir_path = std::filesystem::path(interp.get_variable("CMAKE_CURRENT_SOURCE_DIR")) / dir_path; }

        // Check if directory exists
        if (!std::filesystem::exists(dir_path)) {
            interp.set_fatal_error("aux_source_directory() directory does not exist: " + dir_path.string());
            return;
        }

        if (!std::filesystem::is_directory(dir_path)) {
            interp.set_fatal_error("aux_source_directory() path is not a directory: " + dir_path.string());
            return;
        }

        // Collect source files
        CMakeArray source_files;
        try {
            for (const auto& entry : std::filesystem::directory_iterator(dir_path)) {
                if (!entry.is_regular_file()) continue;

                auto lang_info = LanguageClassifier::from_path(entry.path().string());

                // Only include compileable source files (not headers, not unknown)
                if (lang_info.is_compileable && !lang_info.is_header) { source_files.append(entry.path().string()); }
            }
        } catch (const std::filesystem::filesystem_error& e) {
            interp.set_fatal_error("aux_source_directory() failed to read directory: " + std::string(e.what()));
            return;
        }

        interp.set_variable(variable, source_files.to_string());
    });
}

} // namespace kiln
