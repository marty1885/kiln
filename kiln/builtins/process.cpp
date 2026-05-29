#include "registry.hpp"
#include "../interperter.hpp"
#include "../command_parser.hpp"
#include "../profiler.hpp"
#include "../utils.hpp"
#include "../parse_number.hpp"
#include <iostream>
#include <sstream>
#include <sys/stat.h>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>

namespace kiln {

// Per-session validated Python binaries: maps "invoked_path:canonical_path" → validated -V output
static std::unordered_map<std::string, std::string> s_python_validated;

// Per-session Python sys.path cache: maps binary_key → list of search directories
static std::unordered_map<std::string, std::vector<std::string>> s_python_import_dirs;

// Helper: Get file mtime or nullopt if doesn't exist
static std::optional<int64_t> get_file_mtime(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) == 0) { return st.st_mtime; }
    return std::nullopt;
}

// Helper: Get directory mtime or nullopt if doesn't exist
static std::optional<int64_t> get_dir_mtime(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) { return st.st_mtime; }
    return std::nullopt;
}

// Helper: Check if command is pkg-config/pkgconf
static bool is_pkgconfig_command(const std::string& cmd) {
    // Check for pkg-config or pkgconf in command name
    std::filesystem::path cmd_path(cmd);
    std::string filename = cmd_path.filename().string();
    return filename == "pkg-config" || filename == "pkgconf";
}

// Helper: Append ':'-separated entries from an env var to dirs (skipping empties).
static void append_path_env(const char* var, std::vector<std::string>& dirs) {
    const char* val = std::getenv(var);
    if (!val) return;
    std::istringstream iss(val);
    std::string dir;
    while (std::getline(iss, dir, ':')) {
        if (!dir.empty()) dirs.push_back(std::move(dir));
    }
}

// Helper: Get standard pkgconfig directories
static std::vector<std::string> get_pkgconfig_search_dirs() {
    std::vector<std::string> dirs = {
        "/usr/lib/pkgconfig",       "/usr/lib64/pkgconfig",       "/usr/share/pkgconfig",
        "/usr/local/lib/pkgconfig", "/usr/local/lib64/pkgconfig", "/usr/local/share/pkgconfig",
    };
    append_path_env("PKG_CONFIG_PATH", dirs);
    append_path_env("PKG_CONFIG_LIBDIR", dirs); // overrides default paths if set
    return dirs;
}

// Helper: Compute cache signature for external command (pkg-config, etc.)
static std::string compute_command_signature(const std::vector<std::vector<std::string>>& commands, const ProcessOptions& options) {
    std::ostringstream oss;

    // Include command and all arguments
    for (size_t i = 0; i < commands.size(); ++i) {
        if (i > 0) oss << "||";
        for (size_t j = 0; j < commands[i].size(); ++j) {
            if (j > 0) oss << " ";
            oss << commands[i][j];
        }
    }
    oss << "|";

    // Include working directory
    if (!options.working_dir.empty()) { oss << "wd:" << options.working_dir << "|"; }

    // Include relevant environment variables
    if (const char* pkg_path = std::getenv("PKG_CONFIG_PATH")) { oss << "PKG_CONFIG_PATH:" << pkg_path << "|"; }
    if (const char* sysroot = std::getenv("PKG_CONFIG_SYSROOT_DIR")) { oss << "PKG_CONFIG_SYSROOT_DIR:" << sysroot << "|"; }
    if (const char* libdir = std::getenv("PKG_CONFIG_LIBDIR")) { oss << "PKG_CONFIG_LIBDIR:" << libdir << "|"; }

    return oss.str();
}

// Helper: Check if command is a Python interpreter
static bool is_python_interpreter(const std::string& cmd) {
    std::filesystem::path cmd_path(cmd);
    std::string filename = cmd_path.filename().string();
    // Match python, python3, python3.14, etc.
    if (!filename.starts_with("python")) return false;
    // "python" alone is fine
    if (filename.size() == 6) return true;
    // After "python" must be a digit (python3, python3.14, etc.)
    return std::isdigit(static_cast<unsigned char>(filename[6]));
}

// Helper: Extract the script text from a Python "<py> -c <script> ..." command line.
// Returns empty string if -c is absent or has no following argument.
static std::string find_python_c_script(const std::vector<std::string>& cmd) {
    for (size_t i = 1; i + 1 < cmd.size(); ++i) {
        if (cmd[i] == "-c") return cmd[i + 1];
    }
    return {};
}

// Helper: Check if a Python -c script consists entirely of import statements.
// These are pure availability checks (exit code 0 = installed, non-zero = not).
// Safe to cache because they have no output side effects.
static bool is_import_only_script(const std::vector<std::string>& cmd) {
    if (cmd.size() < 3) return false;
    std::string script = find_python_c_script(cmd);
    if (script.empty()) return false;

    // Tokenize by ';' and '\n', verify each non-empty statement is an import
    size_t pos = 0;
    bool found_any = false;
    while (pos < script.size()) {
        // Find end of statement
        size_t end = script.find_first_of(";\n", pos);
        if (end == std::string::npos) end = script.size();

        // Extract and strip whitespace
        size_t start = pos;
        while (start < end && (script[start] == ' ' || script[start] == '\t' || script[start] == '\r')) ++start;
        size_t back = end;
        while (back > start && (script[back - 1] == ' ' || script[back - 1] == '\t' || script[back - 1] == '\r')) --back;

        std::string_view stmt(script.data() + start, back - start);
        pos = end + 1;

        // Skip empty statements and comments
        if (stmt.empty() || stmt[0] == '#') continue;

        // Must start with "import " or "from "
        if (stmt.starts_with("import ")) {
            // Validate: "import X" / "import X, Y" / "import X.Y.Z"
            // After "import ", only allow identifiers, dots, commas, whitespace
            std::string_view rest = stmt.substr(7);
            for (char c : rest) {
                if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '.' && c != ',' && c != ' ' && c != '\t') {
                    return false;
                }
            }
            // Must have at least one identifier character
            bool has_ident = false;
            for (char c : rest) {
                if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
                    has_ident = true;
                    break;
                }
            }
            if (!has_ident) return false;
        } else if (stmt.starts_with("from ")) {
            // Validate: "from X import Y" / "from X import *" / "from X import (Y, Z)"
            // After "from ", allow identifiers, dots, commas, whitespace, *, (, ), and "import" keyword
            std::string_view rest = stmt.substr(5);
            // Must contain " import "
            auto imp_pos = rest.find(" import ");
            if (imp_pos == std::string_view::npos && !rest.ends_with(" import")) {
                // Also check for "import\t" etc
                imp_pos = rest.find(" import\t");
                if (imp_pos == std::string_view::npos) return false;
            }
            for (char c : rest) {
                if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '.' && c != ',' && c != ' ' && c != '\t' && c != '*'
                    && c != '(' && c != ')') {
                    return false;
                }
            }
        } else {
            return false; // Not an import statement
        }
        found_any = true;
    }

    return found_any;
}

// Helper: Get Python sys.path directories for a given binary (cached per session)
static const std::vector<std::string>& get_python_import_dirs(const std::string& binary_key, const std::string& invoked_path) {
    auto it = s_python_import_dirs.find(binary_key);
    if (it != s_python_import_dirs.end()) return it->second;

    std::vector<std::string> dirs;
    ProcessOptions opts;
    opts.output_quiet = true;
    opts.error_quiet = true;
    std::string out;
    opts.output_variable = &out;
    std::string dummy;
    opts.error_variable = &dummy;
    std::vector<std::vector<std::string>> cmd = {{invoked_path, "-c", "import sys; print('\\n'.join(sys.path))"}};
    auto res = execute_pipeline(cmd, opts);
    if (!res.exit_codes.empty() && res.exit_codes[0] == 0) {
        std::istringstream iss(res.captured_stdout);
        std::string line;
        while (std::getline(iss, line)) {
            auto stripped = rstrip(line);
            if (!stripped.empty()) dirs.emplace_back(stripped); // Skip cwd placeholder
        }
    }

    auto [inserted, _] = s_python_import_dirs.emplace(binary_key, std::move(dirs));
    return inserted->second;
}

// Helper: Compute cache signature for a Python import-only script
// Includes env vars that affect Python's import resolution
static std::string compute_python_import_signature(const std::string& binary_key, const std::string& script,
                                                   const std::string& working_dir) {
    std::string sig = "python_import:" + binary_key + "|" + script + "|cwd:";
    if (!working_dir.empty()) {
        sig += working_dir;
    } else {
        sig += std::filesystem::current_path().string();
    }
    // Include env vars that affect import resolution
    static const char* env_vars[] = {"PYTHONPATH", "VIRTUAL_ENV", "PYTHONHOME", "PYTHONUSERBASE"};
    for (const char* var : env_vars) {
        sig += '|';
        sig += var;
        sig += ':';
        if (const char* val = std::getenv(var)) { sig += val; }
    }
    return sig;
}

// Helper: Check if a Python -c script only imports safe stdlib modules
static bool is_safe_python_script(const std::vector<std::string>& cmd) {
    if (cmd.size() < 2) return false;

    // Check for -V flag anywhere in args
    for (size_t i = 1; i < cmd.size(); ++i) {
        if (cmd[i] == "-V" || cmd[i] == "--version") return true;
    }

    // Only cache -c <script> invocations
    if (cmd.size() < 3) return false;
    std::string script = find_python_c_script(cmd);
    if (script.empty()) return false;

    // Step 1: Blocklist rejection — block non-deterministic or dangerous operations.
    // os.getcwd() and os.path pure functions are safe (cwd is included in cache key).
    static const std::array<std::string_view, 10> blocklist = {
        "__import__", "exec(",   "eval(",    "open(", "subprocess",
        "environ",                           // os.environ — depends on env vars
        "listdir",    "scandir", "os.walk(", // filesystem enumeration
        "os.stat(",                          // filesystem metadata
    };
    for (auto bl : blocklist) {
        if (script.find(bl) != std::string::npos) return false;
    }

    // Step 2: Extract all imported module names
    static const std::unordered_set<std::string> safe_modules = {
        "sys", "sysconfig", "distutils", "struct",
        "re",  "importlib", "os",        "pathlib", // safe with cwd in cache key; dangerous ops caught by blocklist
    };

    // Scan for import statements
    // We look for "import" as a token (preceded by whitespace, start-of-string, or semicolon)
    std::string_view sv(script);
    size_t pos = 0;
    while (pos < sv.size()) {
        // Find next "import"
        auto imp = sv.find("import", pos);
        if (imp == std::string_view::npos) break;

        // Check it's a token boundary (preceded by whitespace/start/semicolon)
        if (imp > 0) {
            char before = sv[imp - 1];
            if (before != ' ' && before != '\t' && before != '\n' && before != '\r' && before != ';') {
                pos = imp + 6;
                continue;
            }
        }
        // Check followed by whitespace (not part of a larger word)
        size_t after = imp + 6;
        if (after >= sv.size() || (sv[after] != ' ' && sv[after] != '\t')) {
            pos = after;
            continue;
        }

        // Check if this is "from X import ..." by looking back for "from"
        bool is_from_import = false;
        if (imp >= 2) {
            // Scan backwards over whitespace to find "from"
            auto back = imp - 1;
            while (back > 0 && (sv[back] == ' ' || sv[back] == '\t')) --back;
            // Check if there's a module name before this, preceded by "from"
            // Find the start of the module name
            auto mod_end = back + 1;
            while (back > 0 && sv[back] != ' ' && sv[back] != '\t' && sv[back] != '\n' && sv[back] != ';') --back;
            if (back > 0 || sv[0] == 'f') {
                auto from_start = (sv[back] == ' ' || sv[back] == '\t' || sv[back] == '\n' || sv[back] == ';') ? back + 1 : back;
                // The word before the module name should be "from"
                // Actually, "from X import Y" — we need to check if we see "from <mod> import"
                // Let's look backwards past the module name to find "from"
                auto mod_start = (sv[back] == ' ' || sv[back] == '\t' || sv[back] == '\n' || sv[back] == ';') ? back + 1 : back;
                std::string_view mod_name = sv.substr(mod_start, mod_end - mod_start);

                // Now check if "from" precedes mod_name
                if (mod_start >= 5) {
                    auto from_check = mod_start - 1;
                    while (from_check > 0 && (sv[from_check] == ' ' || sv[from_check] == '\t')) --from_check;
                    if (from_check >= 3) {
                        auto fw = from_check - 3;
                        if (sv.substr(fw, 4) == "from") {
                            // Verify "from" is at token boundary
                            if (fw == 0 || sv[fw - 1] == ' ' || sv[fw - 1] == '\t' || sv[fw - 1] == '\n' || sv[fw - 1] == ';') {
                                // Extract top-level module from dotted name
                                auto dot = mod_name.find('.');
                                std::string top_mod(dot != std::string_view::npos ? mod_name.substr(0, dot) : mod_name);
                                if (!safe_modules.contains(top_mod)) return false;
                                is_from_import = true;
                            }
                        }
                    }
                }
            }
        }

        if (!is_from_import) {
            // Plain "import X, Y, Z" or "import X.Y.Z"
            auto module_start = after;
            while (module_start < sv.size() && (sv[module_start] == ' ' || sv[module_start] == '\t')) ++module_start;

            // Parse comma-separated module names
            auto p = module_start;
            while (p < sv.size()) {
                // Skip whitespace
                while (p < sv.size() && (sv[p] == ' ' || sv[p] == '\t')) ++p;
                if (p >= sv.size()) break;

                // Read module name (may be dotted)
                auto name_start = p;
                while (p < sv.size() && sv[p] != ' ' && sv[p] != '\t' && sv[p] != ',' && sv[p] != '\n' && sv[p] != '\r' && sv[p] != ';'
                       && sv[p] != '#')
                    ++p;
                if (p == name_start) break;

                std::string_view full_name = sv.substr(name_start, p - name_start);
                auto dot = full_name.find('.');
                std::string top_mod(dot != std::string_view::npos ? full_name.substr(0, dot) : full_name);
                if (!safe_modules.contains(top_mod)) return false;

                // Skip whitespace after module name
                while (p < sv.size() && (sv[p] == ' ' || sv[p] == '\t')) ++p;
                // If comma, continue; otherwise done with this import
                if (p < sv.size() && sv[p] == ',') {
                    ++p;
                    continue;
                }
                break;
            }
        }

        pos = after;
    }

    return true;
}

// Helper: Make a cache key for a Python binary
static std::string make_python_binary_key(const std::string& invoked_path) {
    std::error_code ec;
    auto canonical = std::filesystem::canonical(invoked_path, ec);
    std::string canon_str = ec ? invoked_path : canonical.string();
    return invoked_path + ":" + canon_str;
}

// Helper: Validate a Python binary for this session by checking -V output
// Returns the version string if validated, or empty string on failure
static std::string validate_python_binary(const std::string& invoked_path, CacheStore& cache) {
    std::string binary_key = make_python_binary_key(invoked_path);

    // Already validated this session?
    auto it = s_python_validated.find(binary_key);
    if (it != s_python_validated.end()) return it->second;

    // Run python -V to get current version
    ProcessOptions vopts;
    vopts.output_quiet = true;
    vopts.error_quiet = true;
    std::string dummy;
    vopts.output_variable = &dummy;
    vopts.error_variable = &dummy;
    std::vector<std::vector<std::string>> vcmd = {{invoked_path, "-V"}};
    auto vres = execute_pipeline(vcmd, vopts);
    if (vres.exit_codes.empty() || vres.exit_codes[0] != 0) return "";

    // Python 2 prints to stderr, Python 3 to stdout
    std::string current_version = std::string(rstrip(vres.captured_stdout.empty() ? vres.captured_stderr : vres.captured_stdout));

    // Check stored version from disk cache
    std::string version_key = "python_version:" + binary_key;
    auto stored = cache.lookup<CacheSubsystem::ExternalCommand>(version_key);
    if (stored && stored->stdout_output == current_version) {
        // Version matches — all cached entries are valid
        s_python_validated[binary_key] = current_version;
        return current_version;
    }

    // Version mismatch or no stored version — invalidate stale entries
    // We can't selectively clear entries by prefix, so just store the new version
    // and let individual lookups fail naturally (they won't exist or signature won't match)
    ExternalCommandCacheEntry version_entry;
    version_entry.stdout_output = current_version;
    version_entry.exit_code = 0;
    cache.insert<CacheSubsystem::ExternalCommand>(version_key, version_entry);

    s_python_validated[binary_key] = current_version;
    return current_version;
}

// Helper: Compute cache signature for a Python command
// Includes effective cwd so os.getcwd()/os.path operations are correctly keyed
static std::string compute_python_cache_signature(const std::string& binary_key, const std::vector<std::string>& cmd,
                                                  const std::string& working_dir) {
    std::string sig = "python:" + binary_key + "|";
    for (size_t i = 1; i < cmd.size(); ++i) {
        if (i > 1) sig += " ";
        sig += cmd[i];
    }
    // Include working directory (or actual cwd if none specified) so that
    // os.getcwd() and relative path operations produce correct cached results
    sig += "|cwd:";
    if (!working_dir.empty()) {
        sig += working_dir;
    } else {
        sig += std::filesystem::current_path().string();
    }
    return sig;
}

// Helper: Get mtimes for directories to track (based on command type)
// For pkg-config: tracks standard pkg-config directories
static std::map<std::string, std::optional<int64_t>> get_tracked_dir_mtimes(const std::string& cmd) {
    std::map<std::string, std::optional<int64_t>> mtimes;

    if (is_pkgconfig_command(cmd)) {
        auto dirs = get_pkgconfig_search_dirs();
        for (const auto& dir : dirs) { mtimes[dir] = get_dir_mtime(dir); }
    }
    // Future: Add tracking for other command types (compilers, etc.)

    return mtimes;
}

// Helper: Validate external command cache entry by checking tracked directories.
// Any change (appeared, disappeared, mtime differs) invalidates the entry.
static bool validate_command_cache(const ExternalCommandCacheEntry& entry) {
    for (const auto& [dir, cached_mtime] : entry.tracked_dir_mtimes) {
        if (cached_mtime != get_dir_mtime(dir)) return false;
    }
    return true;
}

// Helper: Run commands, but serve from the ExternalCommand cache when possible.
// On miss, executes and stores. `validate_dirs` enables tracked-dir mtime checking
// against an existing entry. `populate_extras` (optional) is called with a fresh
// entry before insert — typically to fill tracked_dir_mtimes for the new entry.
static PipelineResult cached_or_execute(CacheStore& cache, const std::string& signature, bool validate_dirs,
                                        const std::function<void(ExternalCommandCacheEntry&)>& populate_extras,
                                        const std::vector<std::vector<std::string>>& commands, const ProcessOptions& options) {
    PipelineResult res;
    auto cached = cache.lookup<CacheSubsystem::ExternalCommand>(signature);
    if (cached && (!validate_dirs || validate_command_cache(*cached))) {
        res.captured_stdout = cached->stdout_output;
        res.captured_stderr = cached->stderr_output;
        res.exit_codes.push_back(cached->exit_code);
        return res;
    }
    res = execute_pipeline(commands, options);
    ExternalCommandCacheEntry entry;
    entry.stdout_output = res.captured_stdout;
    entry.stderr_output = res.captured_stderr;
    entry.exit_code = res.exit_codes.empty() ? -1 : res.exit_codes.back();
    if (populate_extras) populate_extras(entry);
    cache.insert<CacheSubsystem::ExternalCommand>(signature, entry);
    return res;
}

// Helper: Check if an execute_process command is invoking CMAKE_COMMAND (kiln itself)
// with CMake-style CLI arguments, and rewrite them to kiln semantics.
// e.g. "cmake -G 'Unix Makefiles' ." → "kiln -C ."
//      "cmake --build ."             → "kiln -C ."
//      "cmake --build . --target foo" → "kiln -C . foo"
static bool translate_cmake_self_invocation(std::vector<std::string>& cmd, const Interpreter& interp) {
    if (cmd.empty()) return false;

    std::string cmake_command = interp.get_variable("CMAKE_COMMAND");
    if (cmake_command.empty()) return false;

    // Normalize paths for comparison
    std::error_code ec;
    auto cmd_canonical = std::filesystem::canonical(cmd[0], ec);
    if (ec) return false;
    auto cmake_canonical = std::filesystem::canonical(cmake_command, ec);
    if (ec) return false;

    if (cmd_canonical != cmake_canonical) return false;

    // It's invoking kiln with cmake-style args — translate
    std::string project_dir;
    std::string build_dir;
    std::string config;
    std::vector<std::string> definitions;
    std::vector<std::string> targets;

    // Don't translate tool mode (-E) or script mode (-P) invocations
    for (size_t j = 1; j < cmd.size(); ++j) {
        if (cmd[j] == "-E" || cmd[j] == "-P") return false;
    }

    size_t i = 1;
    while (i < cmd.size()) {
        if (cmd[i] == "--build") {
            // cmake --build <dir> — dir is the project directory for kiln
            ++i;
            if (i < cmd.size()) {
                project_dir = cmd[i];
                ++i;
            }
        } else if (cmd[i] == "--target") {
            ++i;
            if (i < cmd.size()) {
                targets.push_back(cmd[i]);
                ++i;
            }
        } else if (cmd[i] == "--config") {
            ++i;
            if (i < cmd.size()) {
                config = cmd[i];
                ++i;
            }
        } else if (cmd[i] == "-G") {
            // Skip generator and its value (kiln has no generators)
            ++i;
            if (i < cmd.size()) ++i;
        } else if (cmd[i] == "-S") {
            ++i;
            if (i < cmd.size()) {
                project_dir = cmd[i];
                ++i;
            }
        } else if (cmd[i] == "-B") {
            ++i;
            if (i < cmd.size()) {
                build_dir = cmd[i];
                ++i;
            }
        } else if (cmd[i].starts_with("-D")) {
            // Pass through definitions
            definitions.push_back(cmd[i]);
            ++i;
        } else if (!cmd[i].starts_with("-")) {
            // Positional argument — source directory in cmake configure mode
            project_dir = cmd[i];
            ++i;
        } else {
            // Unknown flag — skip
            ++i;
        }
    }

    // Rebuild as kiln command
    std::vector<std::string> new_cmd;
    new_cmd.push_back(cmd[0]);

    if (!project_dir.empty()) {
        new_cmd.push_back("-C");
        new_cmd.push_back(project_dir);
    }
    if (!build_dir.empty()) {
        new_cmd.push_back("-B");
        new_cmd.push_back(build_dir);
    }
    if (!config.empty()) {
        new_cmd.push_back("--config");
        new_cmd.push_back(config);
    }
    for (const auto& def : definitions) { new_cmd.push_back(def); }
    for (const auto& target : targets) { new_cmd.push_back(target); }

    cmd = std::move(new_cmd);
    return true;
}

void register_process_builtins(Interpreter& interp) {
    interp.add_builtin("execute_process", [](Interpreter& interp, const std::vector<std::string>& args) {
        CommandParser parser("execute_process");
        std::vector<std::vector<std::string>> commands;
        std::string working_dir;
        std::string timeout;
        std::string result_variable;
        std::string results_variable;
        std::string output_variable;
        std::string error_variable;
        std::string input_file;
        std::string output_file;
        std::string error_file;
        std::string command_echo;
        std::string encoding;
        bool output_quiet = false;
        bool error_quiet = false;
        bool output_strip_trailing_whitespace = false;
        bool error_strip_trailing_whitespace = false;
        bool echo_output_variable = false;
        bool echo_error_variable = false;
        std::string command_error_is_fatal_mode;
        bool command_error_is_fatal = false;

        parser.multi_list("COMMAND", commands);
        parser.value("WORKING_DIRECTORY", working_dir);
        parser.value("TIMEOUT", timeout);
        parser.value("RESULT_VARIABLE", result_variable);
        parser.value("RESULTS_VARIABLE", results_variable);
        parser.value("OUTPUT_VARIABLE", output_variable);
        parser.value("ERROR_VARIABLE", error_variable);
        parser.value("INPUT_FILE", input_file);
        parser.value("OUTPUT_FILE", output_file);
        parser.value("ERROR_FILE", error_file);
        parser.value("COMMAND_ECHO", command_echo);
        parser.value("ENCODING", encoding);
        parser.flag("OUTPUT_QUIET", output_quiet);
        parser.flag("ERROR_QUIET", error_quiet);
        parser.flag("OUTPUT_STRIP_TRAILING_WHITESPACE", output_strip_trailing_whitespace);
        parser.flag("ERROR_STRIP_TRAILING_WHITESPACE", error_strip_trailing_whitespace);
        parser.flag("ECHO_OUTPUT_VARIABLE", echo_output_variable);
        parser.flag("ECHO_ERROR_VARIABLE", echo_error_variable);
        parser.value("COMMAND_ERROR_IS_FATAL", command_error_is_fatal_mode);

        PARSE_OR_RETURN(parser, interp, args);

        // CMake supports COMMAND_ERROR_IS_FATAL with values ANY|LAST|NONE.
        // Treat ANY and LAST identically (fatal on any non-zero exit), NONE as no-op.
        if (ci_equals(command_error_is_fatal_mode, "ANY") || ci_equals(command_error_is_fatal_mode, "LAST")) {
            command_error_is_fatal = true;
        }

        if (commands.empty()) {
            interp.set_fatal_error("execute_process requires at least one COMMAND");
            return;
        }

        // Translate cmake-style self-invocations to kiln semantics
        // e.g. execute_process(COMMAND ${CMAKE_COMMAND} -G "..." .) → kiln -C .
        if (commands.size() == 1 && !commands[0].empty()) { translate_cmake_self_invocation(commands[0], interp); }

        int64_t profile_start = 0;
        bool profiling = g_profiling_enabled.load(std::memory_order_relaxed);
        std::string profile_name;
        std::string profile_cmd;
        if (profiling) {
            profile_start = Profiler::instance().now_us();
            const auto& cmd = commands[0][0];
            auto pos = cmd.rfind('/');
            profile_name = "execute_process " + (pos != std::string::npos ? cmd.substr(pos + 1) : cmd);
            for (const auto& pipeline : commands) {
                if (!profile_cmd.empty()) profile_cmd += " | ";
                profile_cmd += join_command(pipeline);
            }
        }

        ProcessOptions options;
        options.working_dir = working_dir;
        options.input_file = input_file;
        options.output_file = output_file;
        options.error_file = error_file;
        options.output_quiet = output_quiet;
        options.error_quiet = error_quiet;

        if (!timeout.empty()) {
            auto v = parse_double(timeout);
            if (!v) {
                interp.set_fatal_error("execute_process invalid TIMEOUT: " + timeout);
                return;
            }
            options.timeout = *v;
        }

        if (!output_variable.empty()) options.output_variable = &output_variable;
        if (!error_variable.empty()) options.error_variable = &error_variable;

        // Check if this is a cacheable command
        bool no_file_redirect = output_file.empty() && error_file.empty() && input_file.empty();
        bool captures_output = !output_variable.empty() || !result_variable.empty();
        bool is_pkgconfig = !commands.empty() && !commands[0].empty() && is_pkgconfig_command(commands[0][0]);
        bool is_single_python = !commands.empty() && !commands[0].empty() && commands.size() == 1 && // Single command, no pipes
                                is_python_interpreter(commands[0][0]);
        bool is_python_import = is_single_python && is_import_only_script(commands[0]);
        bool is_python = is_single_python && !is_python_import && is_safe_python_script(commands[0]);
        bool can_cache = (is_pkgconfig || is_python || is_python_import) && no_file_redirect && captures_output;

        PipelineResult res;

        if (can_cache && is_python_import) {
            // Import-only script: cache with sys.path dir mtime validation
            auto& cache = interp.get_cache_store();
            const auto& cmd = commands[0];
            std::string binary_key = make_python_binary_key(cmd[0]);

            if (validate_python_binary(cmd[0], cache).empty()) {
                res = execute_pipeline(commands, options);
            } else {
                std::string signature = compute_python_import_signature(binary_key, find_python_c_script(cmd), working_dir);
                res = cached_or_execute(
                    cache, signature, /*validate_dirs=*/true,
                    [&](ExternalCommandCacheEntry& e) {
                        // Lazily get sys.path dirs (once per binary per session)
                        for (const auto& dir : get_python_import_dirs(binary_key, cmd[0])) {
                            e.tracked_dir_mtimes[dir] = get_dir_mtime(dir);
                        }
                    },
                    commands, options);
            }
        } else if (can_cache && is_python) {
            auto& cache = interp.get_cache_store();
            const auto& cmd = commands[0];
            std::string binary_key = make_python_binary_key(cmd[0]);

            // Check for -V flag — serve from in-session state if already validated
            bool is_version_query = false;
            for (size_t i = 1; i < cmd.size(); ++i) {
                if (cmd[i] == "-V" || cmd[i] == "--version") {
                    is_version_query = true;
                    break;
                }
            }

            if (is_version_query) {
                auto vit = s_python_validated.find(binary_key);
                if (vit != s_python_validated.end()) {
                    // Serve directly from in-session cache
                    res.captured_stdout = vit->second + "\n";
                    res.exit_codes.push_back(0);
                } else {
                    // Not yet validated — run normally, then prime the session cache
                    res = execute_pipeline(commands, options);
                    if (!res.exit_codes.empty() && res.exit_codes[0] == 0) {
                        std::string ver = res.captured_stdout.empty() ? res.captured_stderr : res.captured_stdout;
                        ver = std::string(rstrip(ver));
                        s_python_validated[binary_key] = ver;
                        // Store version to disk cache for next session
                        ExternalCommandCacheEntry ve;
                        ve.stdout_output = ver;
                        ve.exit_code = 0;
                        cache.insert<CacheSubsystem::ExternalCommand>("python_version:" + binary_key, ve);
                    }
                }
            } else if (validate_python_binary(cmd[0], cache).empty()) {
                res = execute_pipeline(commands, options);
            } else {
                std::string signature = compute_python_cache_signature(binary_key, cmd, working_dir);
                res = cached_or_execute(cache, signature, /*validate_dirs=*/false, nullptr, commands, options);
            }
        } else if (can_cache && is_pkgconfig) {
            auto& cache = interp.get_cache_store();
            std::string signature = compute_command_signature(commands, options);
            res = cached_or_execute(
                cache, signature, /*validate_dirs=*/true,
                [&](ExternalCommandCacheEntry& e) { e.tracked_dir_mtimes = get_tracked_dir_mtimes(commands[0][0]); }, commands, options);
        } else {
            // Not cacheable - execute normally
            res = execute_pipeline(commands, options);
        }

        if (output_strip_trailing_whitespace) res.captured_stdout = std::string(rstrip(res.captured_stdout));
        if (error_strip_trailing_whitespace) res.captured_stderr = std::string(rstrip(res.captured_stderr));

        if (!res.setup_error.empty()) {
            interp.set_fatal_error("execute_process " + res.setup_error);
            return;
        }

        if (!output_variable.empty()) { interp.set_variable(output_variable, res.captured_stdout); }
        if (!error_variable.empty()) { interp.set_variable(error_variable, res.captured_stderr); }

        if (!result_variable.empty()) {
            interp.set_variable(result_variable, res.exit_codes.empty() ? "-1" : std::to_string(res.exit_codes.back()));
        }

        if (!results_variable.empty()) {
            std::string results;
            for (size_t i = 0; i < res.exit_codes.size(); ++i) {
                if (i > 0) results += ";";
                results += std::to_string(res.exit_codes[i]);
            }
            interp.set_variable(results_variable, results);
        }

        if (command_error_is_fatal) {
            for (int code : res.exit_codes) {
                if (code != 0) {
                    interp.set_fatal_error("execute_process failed with exit code " + std::to_string(code));
                    return;
                }
            }
        }

        if (profiling) {
            auto dur = Profiler::instance().now_us() - profile_start;
            Profiler::instance().add_complete(profile_name, "configure", profile_start, dur, Profiler::Args({{"cmd", profile_cmd}}));
        }
    });

    // build_command() - generate a command line to build the project
    // Primary: build_command(<variable> [CONFIGURATION <config>] [PARALLEL_LEVEL <parallel>] [TARGET <target>] [PROJECT_NAME <projname>])
    // Legacy:  build_command(<cachevariable> <makecommand>) - deprecated
    interp.add_builtin("build_command", [](Interpreter& interp, const std::vector<std::string>& args) {
        if (args.empty()) {
            interp.set_fatal_error("build_command requires at least one argument (variable name)");
            return;
        }

        std::string variable = args[0];
        std::string config;
        std::string parallel;
        std::vector<std::string> targets;
        bool has_project_name = false;

        // Check for legacy signature: build_command(<cachevariable> <makecommand>)
        // Legacy form has exactly 2 positional args and no keywords
        bool is_legacy = false;
        if (args.size() == 2) {
            // Check if second arg is not a keyword
            static const std::vector<std::string> keywords = {"CONFIGURATION", "PARALLEL_LEVEL", "TARGET", "PROJECT_NAME"};
            bool second_is_keyword = std::find(keywords.begin(), keywords.end(), args[1]) != keywords.end();
            if (!second_is_keyword) { is_legacy = true; }
        }

        if (is_legacy) {
            // Legacy signature - second argument (makecommand) is ignored
            // Output format: kiln (no --target option)
            interp.set_variable(variable, "kiln");
            return;
        }

        // Primary signature - parse keyword arguments
        size_t i = 1;
        while (i < args.size()) {
            if (args[i] == "CONFIGURATION") {
                ++i;
                if (i >= args.size()) {
                    interp.set_fatal_error("build_command: CONFIGURATION requires a value");
                    return;
                }
                config = args[i];
                ++i;
            } else if (args[i] == "PARALLEL_LEVEL") {
                ++i;
                if (i >= args.size()) {
                    interp.set_fatal_error("build_command: PARALLEL_LEVEL requires a value");
                    return;
                }
                parallel = args[i];
                ++i;
            } else if (args[i] == "TARGET") {
                ++i;
                if (i >= args.size()) {
                    interp.set_fatal_error("build_command: TARGET requires a value");
                    return;
                }
                targets.push_back(args[i]);
                ++i;
            } else if (args[i] == "PROJECT_NAME") {
                ++i;
                if (i >= args.size()) {
                    interp.set_fatal_error("build_command: PROJECT_NAME requires a value");
                    return;
                }
                // PROJECT_NAME is deprecated and ignored, but warn
                has_project_name = true;
                ++i;
            } else {
                interp.set_fatal_error("build_command: unknown argument '" + args[i] + "'");
                return;
            }
        }

        if (has_project_name) { interp.print_message("WARNING", "build_command: PROJECT_NAME is deprecated and has no effect"); }

        // Build command parts as vector, then join
        std::vector<std::string> cmd_parts;
        cmd_parts.push_back("kiln");

        if (!config.empty()) {
            cmd_parts.push_back("--config");
            cmd_parts.push_back(config);
        }

        if (!parallel.empty()) {
            cmd_parts.push_back("-j");
            cmd_parts.push_back(parallel);
        }

        for (const auto& target : targets) { cmd_parts.push_back(target); }

        interp.set_variable(variable, join_command(cmd_parts));
    });

    // Deprecated CMake command, still needed by projects like MariaDB
    interp.add_builtin("exec_program", [](Interpreter& interp, const std::vector<std::string>& args) {
        if (args.empty()) {
            interp.set_fatal_error("exec_program requires at least one argument (executable)");
            return;
        }

        // Parse: exec_program(exe [dir] [ARGS ...] [OUTPUT_VARIABLE var] [RETURN_VALUE var])
        // The first arg is the executable. The second arg is the working directory
        // ONLY if it's not a keyword.
        static const auto is_keyword = [](const std::string& s) { return s == "ARGS" || s == "OUTPUT_VARIABLE" || s == "RETURN_VALUE"; };

        std::string executable = args[0];
        std::string working_dir;
        std::string output_variable;
        std::string return_value_var;
        std::vector<std::string> exec_args;

        size_t i = 1;

        // Optional positional working directory
        if (i < args.size() && !is_keyword(args[i])) {
            working_dir = args[i];
            ++i;
        }

        // Parse keyword arguments
        while (i < args.size()) {
            if (args[i] == "ARGS") {
                ++i;
                while (i < args.size() && !is_keyword(args[i])) {
                    exec_args.push_back(args[i]);
                    ++i;
                }
            } else if (args[i] == "OUTPUT_VARIABLE") {
                ++i;
                if (i >= args.size()) {
                    interp.set_fatal_error("exec_program: OUTPUT_VARIABLE requires a variable name");
                    return;
                }
                output_variable = args[i];
                ++i;
            } else if (args[i] == "RETURN_VALUE") {
                ++i;
                if (i >= args.size()) {
                    interp.set_fatal_error("exec_program: RETURN_VALUE requires a variable name");
                    return;
                }
                return_value_var = args[i];
                ++i;
            } else {
                // Unknown argument - treat as end of known args
                // CMake silently ignores these
                ++i;
            }
        }

        // Build the command vector: executable + args
        std::vector<std::string> command;
        command.push_back(executable);
        command.insert(command.end(), exec_args.begin(), exec_args.end());

        ProcessOptions options;
        options.working_dir = working_dir;

        // When OUTPUT_VARIABLE is set, capture both stdout+stderr and suppress console output
        std::string dummy_output;
        if (!output_variable.empty()) {
            options.output_variable = &dummy_output;
            options.error_variable = &dummy_output;
            options.output_quiet = true;
            options.error_quiet = true;
        }

        std::vector<std::vector<std::string>> commands = {command};
        PipelineResult res = execute_pipeline(commands, options);

        if (!output_variable.empty()) {
            // exec_program combines stdout+stderr into one variable
            std::string combined = res.captured_stdout + res.captured_stderr;
            // Strip trailing newline like CMake does
            while (!combined.empty() && combined.back() == '\n') { combined.pop_back(); }
            interp.set_variable(output_variable, combined);
        }

        if (!return_value_var.empty()) {
            int exit_code = res.exit_codes.empty() ? -1 : res.exit_codes.back();
            interp.set_variable(return_value_var, std::to_string(exit_code));
        }
    });
}

} // namespace kiln
