#include "registry.hpp"
#include "../interperter.hpp"
#include "../command_parser.hpp"
#include "../profiler.hpp"
#include "../CMakeArray.hpp"
#include "../utils.hpp"
#include <filesystem>
#include <functional>
#include <optional>
#include <cstdlib>
#include <sys/stat.h>
#include <algorithm>
#include <sstream>
#include <iostream>
#include "find_helpers.hpp"

namespace kiln {
namespace { // Anonymous namespace for internal helpers

// Parsed command options
struct FindOptions {
    std::string var_name;
    std::vector<std::string> names;
    std::vector<std::string> hints;
    std::vector<std::string> paths;
    std::vector<std::string> path_suffixes;
    std::string doc;
    std::string validator; // User-provided validator function name
    bool required = false;
    bool no_default_path = false;
    bool no_package_root_path = false;
    bool no_cache = false;
    bool names_per_dir = false;
    bool no_cmake_find_root_path = false;
    bool only_cmake_find_root_path = false;
};

// Determines the find root path mode for a given command.
// Per-call flags override the per-variable mode.
// Returns: "NEVER", "ONLY", or "BOTH"
std::string get_find_root_path_mode(Interpreter& interp, const FindOptions& opts, const std::string& command_name) {
    // Per-call flags take precedence
    if (opts.no_cmake_find_root_path) return "NEVER";
    if (opts.only_cmake_find_root_path) return "ONLY";

    // Check per-command mode variable
    std::string mode_var;
    if (command_name == "find_program") {
        mode_var = "CMAKE_FIND_ROOT_PATH_MODE_PROGRAM";
    } else if (command_name == "find_library") {
        mode_var = "CMAKE_FIND_ROOT_PATH_MODE_LIBRARY";
    } else {
        // find_path, find_file
        mode_var = "CMAKE_FIND_ROOT_PATH_MODE_INCLUDE";
    }

    std::string mode = interp.get_variable(mode_var);
    if (mode == "NEVER" || mode == "ONLY" || mode == "BOTH") return mode;

    // Default: BOTH if CMAKE_FIND_ROOT_PATH is set, effectively no-op if not
    return "BOTH";
}

} // namespace

// Apply CMAKE_FIND_ROOT_PATH re-rooting to a list of search paths.
// Returns the modified search path list based on the mode.
// Public — declared in find_helpers.hpp, also used by find_package.cpp.
std::vector<std::filesystem::path> apply_find_root_path(Interpreter& interp, const std::vector<std::filesystem::path>& search_paths,
                                                        const std::string& mode) {
    if (mode == "NEVER") return search_paths;

    std::string root_path_str = interp.get_variable("CMAKE_FIND_ROOT_PATH");
    if (root_path_str.empty()) return search_paths;

    // Parse CMAKE_FIND_ROOT_PATH (semicolon-separated)
    std::vector<std::filesystem::path> root_paths;
    size_t start = 0;
    size_t end = root_path_str.find(';');
    while (end != std::string::npos) {
        if (end > start) root_paths.emplace_back(root_path_str.substr(start, end - start));
        start = end + 1;
        end = root_path_str.find(';', start);
    }
    if (start < root_path_str.size()) root_paths.emplace_back(root_path_str.substr(start));

    if (root_paths.empty()) return search_paths;

    std::vector<std::filesystem::path> result;

    // Re-rooted paths: prepend each root to each search path
    for (const auto& search_path : search_paths) {
        for (const auto& root : root_paths) { result.push_back(root / search_path.relative_path()); }
    }

    if (mode == "BOTH") {
        // Also include original paths after re-rooted ones
        result.insert(result.end(), search_paths.begin(), search_paths.end());
    }
    // mode == "ONLY": only re-rooted paths, already done

    return result;
}

namespace { // re-open anonymous namespace for the rest of the helpers

// Parse a path argument that could be literal or "ENV VAR_NAME"
std::filesystem::path parse_path_arg(const std::string& arg) {
    if (arg.starts_with("ENV ")) {
        std::string env_var = arg.substr(4);
        const char* value = std::getenv(env_var.c_str());
        return value ? std::filesystem::path(value) : std::filesystem::path();
    }
    return std::filesystem::path(arg);
}

// Split PATH-like environment variable by colons
std::vector<std::filesystem::path> split_env_path(const char* env_value) {
    std::vector<std::filesystem::path> result;
    if (!env_value) return result;

    std::string value(env_value);
    size_t start = 0;
    size_t end = value.find(':');

    while (end != std::string::npos) {
        if (end > start) { result.emplace_back(value.substr(start, end - start)); }
        start = end + 1;
        end = value.find(':', start);
    }

    if (start < value.length()) { result.emplace_back(value.substr(start)); }

    return result;
}

// Collect <PackageName>_ROOT prefixes from CMake vars and env vars.
// Returns empty vector if disabled by flags or CMAKE_FIND_USE_PACKAGE_ROOT_PATH.
std::vector<std::filesystem::path> collect_package_root_prefixes(Interpreter& interp, bool no_default_path, bool no_package_root_path) {
    std::vector<std::filesystem::path> roots;

    if (no_default_path || no_package_root_path) return roots;

    // Check global disable variable
    std::string use_var = interp.get_variable("CMAKE_FIND_USE_PACKAGE_ROOT_PATH");
    if (!use_var.empty() && interp.is_falsy(use_var)) return roots;

    // Get current package name (set by find_package before calling Find modules)
    std::string pkg = interp.get_variable("CMAKE_FIND_PACKAGE_NAME");
    if (pkg.empty()) return roots;

    std::string upper_pkg = pkg;
    for (auto& c : upper_pkg) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    auto maybe_add = [&](const std::string& val) {
        if (!val.empty()) roots.emplace_back(val);
    };

    // CMake variable <PackageName>_ROOT, then <PACKAGENAME>_ROOT
    maybe_add(interp.get_variable(pkg + "_ROOT"));
    if (upper_pkg != pkg) maybe_add(interp.get_variable(upper_pkg + "_ROOT"));

    // Environment variable <PackageName>_ROOT, then <PACKAGENAME>_ROOT
    if (const char* env = std::getenv((pkg + "_ROOT").c_str())) maybe_add(env);
    if (upper_pkg != pkg) {
        if (const char* env = std::getenv((upper_pkg + "_ROOT").c_str())) maybe_add(env);
    }

    return roots;
}

// Build the complete search path list following CMake's specification
std::vector<std::filesystem::path> build_search_paths(Interpreter& interp, const FindOptions& opts,
                                                      const std::vector<std::filesystem::path>& default_paths,
                                                      const std::string& command_name) {
    std::vector<std::filesystem::path> search_paths;

    if (opts.no_default_path) {
        // Only use HINTS and PATHS when NO_DEFAULT_PATH is set
        for (const auto& hint : opts.hints) {
            auto path = parse_path_arg(hint);
            if (!path.empty()) search_paths.push_back(path);
        }
        for (const auto& path_str : opts.paths) {
            auto path = parse_path_arg(path_str);
            if (!path.empty()) search_paths.push_back(path);
        }
        return search_paths;
    }

    // 1. Package-specific roots from <PackageName>_ROOT
    auto root_prefixes = collect_package_root_prefixes(interp, opts.no_default_path, opts.no_package_root_path);
    for (const auto& root : root_prefixes) {
        if (command_name == "find_path" || command_name == "find_file") {
            search_paths.push_back(root / "include");
        } else if (command_name == "find_library") {
            search_paths.push_back(root / "lib");
            search_paths.push_back(root / "lib64");
        } else if (command_name == "find_program") {
            search_paths.push_back(root / "bin");
        }
        search_paths.push_back(root);
    }

    // 2. CMake-specific paths from CMAKE_PREFIX_PATH (variable, then environment)
    auto append_prefix_paths = [&](const std::vector<std::filesystem::path>& paths) {
        for (const auto& prefix : paths) {
            // For find_path and find_file, also add <prefix>/include
            // For find_library, also add <prefix>/lib
            // For find_program, also add <prefix>/bin
            if (command_name == "find_path" || command_name == "find_file") {
                search_paths.push_back(prefix / "include");
            } else if (command_name == "find_library") {
                search_paths.push_back(prefix / "lib");
                search_paths.push_back(prefix / "lib64");
            } else if (command_name == "find_program") {
                search_paths.push_back(prefix / "bin");
            }
            // Always add the prefix itself
            search_paths.push_back(prefix);
        }
    };
    auto split_cmake_list = [](const std::string& s) {
        std::vector<std::filesystem::path> result;
        size_t start = 0;
        for (size_t i = 0; i <= s.size(); ++i) {
            if (i == s.size() || s[i] == ';') {
                if (i > start) result.emplace_back(s.substr(start, i - start));
                start = i + 1;
            }
        }
        return result;
    };
    std::string prefix_path = interp.get_variable("CMAKE_PREFIX_PATH");
    if (!prefix_path.empty()) { append_prefix_paths(split_cmake_list(prefix_path)); }
    if (const char* env_prefix = std::getenv("CMAKE_PREFIX_PATH")) {
        if (*env_prefix) append_prefix_paths(split_env_path(env_prefix));
    }

    // 3. HINTS paths (system introspection)
    for (const auto& hint : opts.hints) {
        auto path = parse_path_arg(hint);
        if (!path.empty()) search_paths.push_back(path);
    }

    // 4. System environment variables (already included in default_paths)

    // 5. CMake system paths and default paths
    for (const auto& path : default_paths) { search_paths.push_back(path); }

    // 6. PATHS (hard-coded guesses)
    for (const auto& path_str : opts.paths) {
        auto path = parse_path_arg(path_str);
        if (!path.empty()) search_paths.push_back(path);
    }

    return search_paths;
}

// Result type for search_for_file - contains both the file path and the base search directory
struct SearchResult {
    bool found = false;                     // Whether file was found
    std::filesystem::path file_path;        // Full path to the found file (empty if not found)
    std::filesystem::path search_dir;       // Base search directory where it was found (empty if not found)
    std::vector<std::string> searched_dirs; // All directories checked (in order, up to hit OR end)
};

// Compute cache signature for find command
std::string compute_find_signature(const std::string& cmd_name, const FindOptions& opts,
                                   const std::vector<std::filesystem::path>& search_paths,
                                   // Toolchain context. Most of this is already reflected in search_paths
                                   // (CMAKE_FIND_ROOT_PATH re-rooting changes the resolved paths), but
                                   // mixing them in explicitly defends against edge cases where a toolchain
                                   // swap leaves the visible paths unchanged but the underlying meaning
                                   // differs (e.g. NO_CMAKE_FIND_ROOT_PATH per-call disables re-rooting,
                                   // so the host result must not be served to a cross build).
                                   const std::string& sysroot = {}, const std::string& find_root_path = {},
                                   const std::string& root_mode = {}) {
    std::ostringstream oss;
    oss << "cmd:" << cmd_name << "|";

    // Names (sorted for consistency)
    std::vector<std::string> sorted_names = opts.names;
    std::sort(sorted_names.begin(), sorted_names.end());
    for (const auto& name : sorted_names) { oss << "name:" << name << "|"; }

    // Search paths (in order - matters!)
    for (const auto& path : search_paths) { oss << "path:" << path.string() << "|"; }

    // Suffixes (in order)
    for (const auto& suffix : opts.path_suffixes) { oss << "suffix:" << suffix << "|"; }

    // Flags
    oss << "names_per_dir:" << opts.names_per_dir << "|";

    // Toolchain context — only emit when set so we don't churn existing
    // host-build cache entries.
    if (!sysroot.empty()) oss << "sysroot:" << sysroot << "|";
    if (!find_root_path.empty()) oss << "find_root:" << find_root_path << "|";
    if (!root_mode.empty()) oss << "root_mode:" << root_mode << "|";

    return oss.str();
}

// Validate cached find result by checking directory mtimes
bool validate_find_cache_entry(Interpreter& interp, const FindResultCacheEntry& entry) {
    for (const auto& [dir, cached_mtime] : entry.searched_dirs) {
        auto current_mtime = interp.get_dir_mtime_cached(dir);

        // Check state change
        if (!cached_mtime.has_value() && current_mtime.has_value()) {
            // Directory didn't exist before, exists now -> invalidate
            return false;
        }
        if (cached_mtime.has_value() && !current_mtime.has_value()) {
            // Directory existed, now gone -> continue checking (might still be valid)
            continue;
        }
        if (cached_mtime.has_value() && current_mtime.has_value()) {
            if (*cached_mtime != *current_mtime) {
                // Directory mtime changed -> invalidate
                return false;
            }
        }
    }
    return true;
}

// Core search engine - supports both default and NAMES_PER_DIR algorithms
SearchResult search_for_file(Interpreter& interp, const FindOptions& opts, const std::vector<std::filesystem::path>& default_paths,
                             std::vector<std::string> (*name_variants)(Interpreter&, const std::string&),
                             bool (*builtin_validator)(const std::filesystem::path&), const std::string& command_name) {
    auto search_paths = build_search_paths(interp, opts, default_paths, command_name);

    // Apply CMAKE_FIND_ROOT_PATH re-rooting
    std::string root_mode = get_find_root_path_mode(interp, opts, command_name);
    search_paths = apply_find_root_path(interp, search_paths, root_mode);

    // Track searched directories for caching
    std::vector<std::string> searched_dirs;

    // Prepare suffix list (empty string first for no suffix)
    std::vector<std::string> suffixes = {""};
    suffixes.insert(suffixes.end(), opts.path_suffixes.begin(), opts.path_suffixes.end());

    if (opts.names_per_dir) {
        // NAMES_PER_DIR: Try all names in one directory before moving to next
        for (const auto& search_path : search_paths) {
            for (const auto& suffix : suffixes) {
                std::filesystem::path check_dir = suffix.empty() ? search_path : search_path / suffix;

                // Check if directory exists using cache
                auto parent = check_dir.parent_path();
                auto dirname = check_dir.filename().string();
                if (!dirname.empty() && !interp.cached_file_exists(parent.string(), dirname)) {
                    searched_dirs.push_back(check_dir.string());
                    continue;
                }
                // Verify it's actually a directory
                if (!interp.cached_is_directory(check_dir.string())) {
                    searched_dirs.push_back(check_dir.string());
                    continue;
                }

                searched_dirs.push_back(check_dir.string());

                for (const auto& name : opts.names) {
                    auto variants = name_variants(interp, name);
                    for (const auto& variant : variants) {
                        std::filesystem::path full_path = check_dir / variant;
                        // For names with path components (e.g., X11/X.h), use single-parameter check
                        bool exists = (variant.find('/') != std::string::npos) ? interp.cached_file_exists(full_path.string())
                                                                               : interp.cached_file_exists(check_dir.string(), variant);
                        bool valid = exists && builtin_validator(full_path);

                        // Check user-provided VALIDATOR function if specified
                        if (valid && !opts.validator.empty()) {
                            // Set the variable to the candidate path before calling validator
                            interp.set_variable(opts.var_name, full_path.string());

                            // Call user's validator function with the candidate path as argument
                            if (!interp.call_user_function(opts.validator, {full_path.string()})) {
                                // Function doesn't exist or failed - reject candidate
                                valid = false;
                            } else {
                                // Check if the variable was cleared by the validator (CMake convention for invalid)
                                std::string result = interp.get_variable(opts.var_name);
                                if (result.empty()) { valid = false; }
                            }
                        }

                        // DEBUG: Uncomment to trace search

                        if (valid) {
                            std::error_code ec;
                            return SearchResult{true, // found
                                                std::filesystem::absolute(full_path, ec).lexically_normal(),
                                                check_dir, // Return the directory where the file was found (includes suffix)
                                                searched_dirs};
                        }
                    }
                }
            }
        }
    } else {
        // Default: Try all directories for each name before moving to next name
        for (const auto& name : opts.names) {
            auto variants = name_variants(interp, name);

            for (const auto& search_path : search_paths) {
                for (const auto& suffix : suffixes) {
                    std::filesystem::path check_dir = suffix.empty() ? search_path : search_path / suffix;

                    // DEBUG

                    // Check if directory exists using cache
                    auto parent = check_dir.parent_path();
                    auto dirname = check_dir.filename().string();
                    if (!dirname.empty() && !interp.cached_file_exists(parent.string(), dirname)) {
                        searched_dirs.push_back(check_dir.string());
                        continue;
                    }
                    // Verify it's actually a directory
                    if (!interp.cached_is_directory(check_dir.string())) {
                        searched_dirs.push_back(check_dir.string());
                        continue;
                    }

                    searched_dirs.push_back(check_dir.string());

                    for (const auto& variant : variants) {
                        std::filesystem::path full_path = check_dir / variant;
                        // For names with path components (e.g., X11/X.h), use single-parameter check
                        bool exists = (variant.find('/') != std::string::npos) ? interp.cached_file_exists(full_path.string())
                                                                               : interp.cached_file_exists(check_dir.string(), variant);
                        bool valid = exists && builtin_validator(full_path);

                        // Check user-provided VALIDATOR function if specified
                        if (valid && !opts.validator.empty()) {
                            // Set the variable to the candidate path before calling validator
                            interp.set_variable(opts.var_name, full_path.string());

                            // Call user's validator function with the candidate path as argument
                            if (!interp.call_user_function(opts.validator, {full_path.string()})) {
                                // Function doesn't exist or failed - reject candidate
                                valid = false;
                            } else {
                                // Check if the variable was cleared by the validator (CMake convention for invalid)
                                std::string result = interp.get_variable(opts.var_name);
                                if (result.empty()) { valid = false; }
                            }
                        }

                        if (valid) {
                            std::error_code ec;
                            return SearchResult{true, // found
                                                std::filesystem::absolute(full_path, ec).lexically_normal(),
                                                check_dir, // Return the directory where the file was found (includes suffix)
                                                searched_dirs};
                        }
                    }
                }
            }
        }
    }

    // Not found - return with all searched directories
    return SearchResult{false,                   // not found
                        std::filesystem::path(), // empty path
                        std::filesystem::path(), // empty search_dir
                        searched_dirs};
}

// Validate that a file is an executable program
bool validate_program(const std::filesystem::path& p) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(p, ec)) { return false; }

    // Check if file has executable permission (owner, group, or others)
    struct stat st;
    if (stat(p.c_str(), &st) != 0) { return false; }

    return (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0;
}

// Validate that a file exists and is a regular file
bool validate_file(const std::filesystem::path& p) {
    std::error_code ec;
    return std::filesystem::is_regular_file(p, ec);
}

// Generate name variants for programs (no transformation on Linux)
std::vector<std::string> program_variants(Interpreter& interp, const std::string& name) {
    // TODO: On Windows, would add .exe, .com, .bat extensions
    return {name};
}

// Generate name variants for libraries (lib prefix and .so/.a suffixes)
std::vector<std::string> library_variants(Interpreter& interp, const std::string& name) {
    std::vector<std::string> variants;

    // Get custom prefixes/suffixes if set
    std::string prefixes_str = interp.get_variable("CMAKE_FIND_LIBRARY_PREFIXES");
    std::string suffixes_str = interp.get_variable("CMAKE_FIND_LIBRARY_SUFFIXES");

    std::vector<std::string> prefixes;
    std::vector<std::string> suffixes;

    if (!prefixes_str.empty()) {
        // Split by semicolon
        size_t start = 0;
        size_t end = prefixes_str.find(';');
        while (end != std::string::npos) {
            prefixes.push_back(prefixes_str.substr(start, end - start));
            start = end + 1;
            end = prefixes_str.find(';', start);
        }
        prefixes.push_back(prefixes_str.substr(start));
    } else {
        // Linux defaults
        prefixes = {"lib", ""};
    }

    if (!suffixes_str.empty()) {
        // Split by semicolon
        size_t start = 0;
        size_t end = suffixes_str.find(';');
        while (end != std::string::npos) {
            suffixes.push_back(suffixes_str.substr(start, end - start));
            start = end + 1;
            end = suffixes_str.find(';', start);
        }
        suffixes.push_back(suffixes_str.substr(start));
    } else {
        // Linux defaults
        suffixes = {".so", ".a", ""};
    }

    // Try name as-is first (might already have lib prefix or suffix)
    variants.push_back(name);

    // Then try all combinations
    for (const auto& prefix : prefixes) {
        for (const auto& suffix : suffixes) {
            std::string variant = prefix + name + suffix;
            if (variant != name) { // Don't duplicate
                variants.push_back(variant);
            }
        }
    }

    return variants;
}

// Generate name variants for files (exact match only)
std::vector<std::string> file_variants(Interpreter& interp, const std::string& name) {
    return {name};
}

// Get default search paths for programs
std::vector<std::filesystem::path> get_program_default_paths(Interpreter& interp) {
    std::vector<std::filesystem::path> paths;

    // PATH environment variable
    const char* path_env = std::getenv("PATH");
    auto path_dirs = split_env_path(path_env);
    paths.insert(paths.end(), path_dirs.begin(), path_dirs.end());

    // Standard system paths
    paths.emplace_back("/usr/local/bin");
    paths.emplace_back("/usr/bin");
    paths.emplace_back("/bin");

    return paths;
}

// Get default search paths for libraries
std::vector<std::filesystem::path> get_library_default_paths(Interpreter& interp) {
    std::vector<std::filesystem::path> paths;

    // LD_LIBRARY_PATH environment variable
    const char* ld_path = std::getenv("LD_LIBRARY_PATH");
    auto ld_dirs = split_env_path(ld_path);
    paths.insert(paths.end(), ld_dirs.begin(), ld_dirs.end());

    // Standard system library paths
    paths.emplace_back("/usr/local/lib");
    paths.emplace_back("/usr/local/lib64");
    paths.emplace_back("/usr/lib");
    paths.emplace_back("/usr/lib64");
    paths.emplace_back("/lib");
    paths.emplace_back("/lib64");

    // Architecture-specific paths (common on Debian/Ubuntu)
    if (auto& triplet = gnu_arch_triplet(); !triplet.empty()) {
        paths.emplace_back("/usr/lib/" + triplet);
        paths.emplace_back("/lib/" + triplet);
    }

    return paths;
}

// Get default search paths for files (typically headers)
std::vector<std::filesystem::path> get_file_default_paths(Interpreter& interp) {
    std::vector<std::filesystem::path> paths;

    // Standard include paths
    paths.emplace_back("/usr/local/include");
    paths.emplace_back("/usr/include");

    return paths;
}

// Get default search paths for find_path (includes CMAKE_INCLUDE_PATH)
std::vector<std::filesystem::path> get_path_default_paths(Interpreter& interp) {
    std::vector<std::filesystem::path> paths;

    // CMAKE_INCLUDE_PATH variable
    std::string include_path = interp.get_variable("CMAKE_INCLUDE_PATH");
    if (!include_path.empty()) {
        auto include_dirs = split_env_path(include_path.c_str());
        paths.insert(paths.end(), include_dirs.begin(), include_dirs.end());
    }

    // INCLUDE environment variable
    const char* include_env = std::getenv("INCLUDE");
    auto include_env_dirs = split_env_path(include_env);
    paths.insert(paths.end(), include_env_dirs.begin(), include_env_dirs.end());

    // Standard include paths
    paths.emplace_back("/usr/local/include");
    paths.emplace_back("/usr/include");

    return paths;
}

// Generic registration helper for all find commands
void register_find_command(Interpreter& interp, const std::string& cmd_name,
                           std::vector<std::filesystem::path> (*get_defaults)(Interpreter&),
                           std::vector<std::string> (*get_variants)(Interpreter&, const std::string&),
                           bool (*validator)(const std::filesystem::path&),
                           bool return_directory = false // find_path returns directory, not file
) {
    interp.add_builtin(cmd_name, [=](Interpreter& interp, const std::vector<std::string>& args) {
        CommandParser parser(cmd_name);

        FindOptions opts;
        std::vector<std::string> default_args;

        parser.positional(opts.var_name, "variable name", true);
        parser.positionals(default_args, "args");
        parser.list("NAMES", opts.names);
        parser.list("HINTS", opts.hints);
        parser.list("PATHS", opts.paths);
        parser.list("PATH_SUFFIXES", opts.path_suffixes);
        parser.value("DOC", opts.doc);
        parser.value("VALIDATOR", opts.validator);
        parser.flag("REQUIRED", opts.required);
        parser.flag("NO_DEFAULT_PATH", opts.no_default_path);
        parser.flag("NO_PACKAGE_ROOT_PATH", opts.no_package_root_path);
        parser.flag("NO_CACHE", opts.no_cache);
        parser.flag("NO_CMAKE_FIND_ROOT_PATH", opts.no_cmake_find_root_path);
        parser.flag("ONLY_CMAKE_FIND_ROOT_PATH", opts.only_cmake_find_root_path);
        parser.flag("NAMES_PER_DIR", opts.names_per_dir);

        PARSE_OR_RETURN(parser, interp, args);

        // CMAKE_FIND_REQUIRED (CMake >= 3.22) promotes any find_* call to REQUIRED.
        if (!opts.required) {
            std::string find_required_var = interp.get_variable("CMAKE_FIND_REQUIRED");
            if (!find_required_var.empty() && !interp.is_falsy(find_required_var)) { opts.required = true; }
        }

        // Handle short-hand syntax: find_program(VAR name [path1 path2...])
        if (opts.names.empty() && !default_args.empty()) {
            // First item is always the name to search for
            opts.names.push_back(default_args[0]);

            // Rest are paths (if provided)
            for (size_t i = 1; i < default_args.size(); ++i) { opts.paths.push_back(default_args[i]); }
        }

        if (opts.names.empty()) {
            // CMake silently fails when no names are provided (e.g. from empty variable expansion)
            interp.set_variable(opts.var_name, opts.var_name + "-NOTFOUND");
            return;
        }

        // Before searching, check if variable already contains a non-NOTFOUND value
        std::string existing = interp.get_variable(opts.var_name);
        if (!existing.empty() && existing.find("-NOTFOUND") == std::string::npos) {
            // Already have a valid value, skip search
            return;
        }

        // Profiling setup
        int64_t profile_start = 0;
        bool profiling = g_profiling_enabled.load(std::memory_order_relaxed);
        if (profiling) profile_start = Profiler::instance().now_us();

        // Check persistent cache
        auto default_paths = get_defaults(interp);
        auto search_paths = build_search_paths(interp, opts, default_paths, cmd_name);

        // Apply CMAKE_FIND_ROOT_PATH re-rooting for cache signature
        std::string root_mode = get_find_root_path_mode(interp, opts, cmd_name);
        search_paths = apply_find_root_path(interp, search_paths, root_mode);

        std::string signature = compute_find_signature(cmd_name, opts, search_paths, interp.get_variable("CMAKE_SYSROOT"),
                                                       interp.get_variable("CMAKE_FIND_ROOT_PATH"), root_mode);

        auto& cache = interp.get_cache_store();
        auto cached = cache.lookup<CacheSubsystem::FindResult>(signature);

        if (cached) {
            // Validate cache entry
            if (validate_find_cache_entry(interp, *cached)) {
                // Cache hit - no filesystem scan needed
                if (profiling) {
                    auto dur = Profiler::instance().now_us() - profile_start;
                    Profiler::instance().add_complete(cmd_name + " " + opts.names[0] + " (cached)", "configure", profile_start, dur);
                }
                if (!cached->found_path.empty()) {
                    interp.set_variable(opts.var_name, cached->found_path);
                    if (!opts.no_cache) { interp.set_cache_variable(opts.var_name, cached->found_path); }
                } else {
                    // Cached negative result
                    std::string notfound = opts.var_name + "-NOTFOUND";
                    interp.set_variable(opts.var_name, notfound);
                    if (!opts.no_cache) { interp.set_cache_variable(opts.var_name, notfound); }
                }
                return;
            }
        }

        // Cache miss - perform the search
        auto result = search_for_file(interp, opts, default_paths, get_variants, validator, cmd_name);

        // Store in cache with searched directory mtimes
        FindResultCacheEntry cache_entry;
        if (result.found) {
            // Found - store result path
            std::string result_str;
            if (return_directory) {
                result_str = result.search_dir.string();
            } else {
                result_str = result.file_path.string();
            }
            cache_entry.found_path = result_str;
        } else {
            // Not found - store empty path
            cache_entry.found_path = "";
        }

        // Record searched directories with their mtimes
        for (const auto& dir : result.searched_dirs) {
            auto mtime = interp.get_dir_mtime_cached(dir);
            cache_entry.searched_dirs.push_back({dir, mtime});
        }

        cache.insert<CacheSubsystem::FindResult>(signature, cache_entry);

        if (result.found) {
            // Found it
            std::string result_str;
            if (return_directory) {
                // find_path returns the base search directory where the file was found
                // For "fontconfig/fontconfig.h" found in /usr/include, return /usr/include
                result_str = result.search_dir.string();
            } else {
                // find_file, find_program, find_library return full path
                result_str = result.file_path.string();
            }
            interp.set_variable(opts.var_name, result_str);

            if (!opts.no_cache) {
                // Also store in cache
                interp.set_cache_variable(opts.var_name, result_str);
                // we don't do docs
            }
        } else {
            // Not found
            std::string notfound = opts.var_name + "-NOTFOUND";
            interp.set_variable(opts.var_name, notfound);

            if (!opts.no_cache) { interp.set_cache_variable(opts.var_name, notfound); }

            if (opts.required) {
                std::string err = "Could not find required " + cmd_name + ": " + opts.names[0];
                if (opts.names.size() > 1) {
                    err += " (or " + std::to_string(opts.names.size() - 1) + " alternative";
                    if (opts.names.size() > 2) err += "s";
                    err += ")";
                }
                interp.set_fatal_error(err);
            }
        }

        if (profiling) {
            auto dur = Profiler::instance().now_us() - profile_start;
            Profiler::instance().add_complete(cmd_name + " " + opts.names[0], "configure", profile_start, dur);
        }
    });
}

} // anonymous namespace

// Public registration function
void register_find_commands_builtins(Interpreter& interp) {
    register_find_command(interp, "find_program", get_program_default_paths, program_variants, validate_program);

    register_find_command(interp, "find_library", get_library_default_paths, library_variants, validate_file);

    register_find_command(interp, "find_file", get_file_default_paths, file_variants, validate_file);

    // find_path returns the directory containing the file, not the file itself
    register_find_command(interp, "find_path", get_path_default_paths, file_variants, validate_file, true);
}

} // namespace kiln
