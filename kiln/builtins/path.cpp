#include "registry.hpp"
#include "../interperter.hpp"
#include "../command_parser.hpp"
#include "../path.hpp"
#include "../utils.hpp"
#include <filesystem>
#include <algorithm>

namespace kiln {

namespace {

// Helper to convert path to CMake format (forward slashes)
std::string to_cmake_path(const std::filesystem::path& p) {
    std::string s = p.string();
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

// Helper to convert path to native format
std::string to_native_path(const std::filesystem::path& p) {
    std::filesystem::path native = p;
    return native.make_preferred().string();
}

// Dotfile helper: CMake treats filenames starting with '.' as having no extension/stem
bool is_dotfile(std::string_view name) {
    return !name.empty() && name[0] == '.';
}

// GET subcommands
void handle_get(Interpreter& interp, const std::vector<std::string>& args) {
    if (args.size() < 4) {
        interp.set_fatal_error("cmake_path(GET) requires at least 4 arguments");
        return;
    }

    // args[1] is a variable name, dereference it to get the path value
    Path path(interp.get_variable(args[1]));
    const auto& component = args[2];
    std::string result;

    // Check for LAST_ONLY flag - if present, out_var is at args[4], otherwise args[3]
    bool last_only = (args.size() > 4 && ci_equals(args[3], "LAST_ONLY"));
    std::string out_var = last_only ? args[4] : args[3];

    if (ci_equals(component, "ROOT_NAME")) {
        // On Unix, root_name is always empty
        result = "";
    } else if (ci_equals(component, "ROOT_DIRECTORY")) {
        result = path.is_absolute() ? "/" : "";
    } else if (ci_equals(component, "ROOT_PATH")) {
        result = path.is_absolute() ? "/" : "";
    } else if (ci_equals(component, "FILENAME")) {
        result = std::string(path.filename());
    } else if (ci_equals(component, "EXTENSION")) {
        auto fname = path.filename();

        if (!fname.empty() && fname != "." && fname != "..") {
            if (is_dotfile(fname)) {
                result = "";
            } else {
                size_t dot_pos = last_only ? fname.find_last_of('.') : fname.find_first_of('.', 1);
                if (dot_pos != std::string::npos && dot_pos > 0) { result = std::string(fname.substr(dot_pos)); }
            }
        }
    } else if (ci_equals(component, "STEM")) {
        auto fname = path.filename();

        if (!fname.empty() && fname != "." && fname != "..") {
            if (is_dotfile(fname)) {
                result = "";
            } else {
                size_t dot_pos = last_only ? fname.find_last_of('.') : fname.find_first_of('.', 1);
                if (dot_pos != std::string::npos && dot_pos > 0) {
                    result = std::string(fname.substr(0, dot_pos));
                } else {
                    result = std::string(fname);
                }
            }
        }
    } else if (ci_equals(component, "RELATIVE_PART")) {
        result = std::string(path.relative_path());
    } else if (ci_equals(component, "PARENT_PATH")) {
        result = std::string(path.parent_path());
    } else {
        interp.set_fatal_error("cmake_path(GET): unknown component '" + args[2] + "'");
        return;
    }

    interp.set_variable(out_var, result);
}

// HAS_* query subcommands
// cmake_path(HAS_FILENAME <path-var> <out-var>)
void handle_has(Interpreter& interp, const std::vector<std::string>& args) {
    if (args.size() < 3) {
        interp.set_fatal_error("cmake_path(HAS_*) requires at least 3 arguments");
        return;
    }

    // args[0] is the subcommand (HAS_FILENAME, etc.)
    // args[1] is a variable name, dereference it to get the path value
    // args[2] is the output variable
    const auto& subcommand = args[0];
    Path path(interp.get_variable(args[1]));
    std::string out_var = args[2];
    bool result = false;

    if (ci_equals(subcommand, "HAS_ROOT_NAME")) {
        result = false; // Unix: never has root name
    } else if (ci_equals(subcommand, "HAS_ROOT_DIRECTORY")) {
        result = path.is_absolute();
    } else if (ci_equals(subcommand, "HAS_ROOT_PATH")) {
        result = path.is_absolute();
    } else if (ci_equals(subcommand, "HAS_FILENAME")) {
        result = !path.filename().empty();
    } else if (ci_equals(subcommand, "HAS_EXTENSION")) {
        result = path.has_extension();
    } else if (ci_equals(subcommand, "HAS_STEM")) {
        result = !path.stem().empty();
    } else if (ci_equals(subcommand, "HAS_RELATIVE_PATH")) {
        result = !path.relative_path().empty();
    } else if (ci_equals(subcommand, "HAS_PARENT_PATH")) {
        // Root path (/) has no parent path in CMake
        auto pp = path.parent_path();
        if (path.is_absolute() && (pp.empty() || pp == "/")) {
            result = false;
        } else {
            result = !pp.empty();
        }
    } else {
        interp.set_fatal_error("cmake_path: unknown query '" + args[0] + "'");
        return;
    }

    interp.set_variable(out_var, result ? "ON" : "OFF");
}

// IS_ABSOLUTE, IS_RELATIVE, IS_PREFIX
// cmake_path(IS_ABSOLUTE <path-var> <out-var>)
// cmake_path(IS_PREFIX <path-var> <input> <out-var> [NORMALIZE])
void handle_is(Interpreter& interp, const std::vector<std::string>& args) {
    if (args.size() < 3) {
        interp.set_fatal_error("cmake_path(IS_*) requires at least 3 arguments");
        return;
    }

    // args[0] is the subcommand (IS_ABSOLUTE, IS_RELATIVE, IS_PREFIX)
    // args[1] is a variable name, dereference it to get the path value
    // args[2] is the output variable
    const auto& subcommand = args[0];
    std::string out_var = args[2];
    bool result = false;

    if (ci_equals(subcommand, "IS_ABSOLUTE")) {
        result = Path(interp.get_variable(args[1])).is_absolute();
    } else if (ci_equals(subcommand, "IS_RELATIVE")) {
        result = Path(interp.get_variable(args[1])).is_relative();
    } else if (ci_equals(subcommand, "IS_PREFIX")) {
        // cmake_path(IS_PREFIX <path-var> <input> [NORMALIZE] <out-var>)
        if (args.size() < 4) {
            interp.set_fatal_error("cmake_path(IS_PREFIX) requires 4 arguments");
            return;
        }
        // args[1] = path variable name, args[2] = input path.
        // The optional NORMALIZE keyword may appear either before or after the
        // out-var; CMake docs place it before, but accept both for robustness.
        std::string input = args[2];
        bool normalize = false;
        if (args.size() > 4 && ci_equals(args[3], "NORMALIZE")) {
            normalize = true;
            out_var = args[4];
        } else if (args.size() > 4 && ci_equals(args[4], "NORMALIZE")) {
            normalize = true;
            out_var = args[3];
        } else {
            out_var = args[3];
        }

        // CMake's IS_PREFIX uses logical path prefix matching.
        // Strip trailing separators that confuse std::filesystem::path iteration
        // (trailing '/' adds an empty component).
        auto strip_trailing_sep = [](std::string_view p) -> std::string {
            while (p.size() > 1 && p.back() == '/') p.remove_suffix(1);
            return std::string(p);
        };
        std::filesystem::path prefix_path(strip_trailing_sep(interp.get_variable(args[1])));
        std::filesystem::path other(strip_trailing_sep(input));
        if (normalize) {
            prefix_path = prefix_path.lexically_normal();
            other = other.lexically_normal();
        }

        auto path_it = prefix_path.begin();
        auto other_it = other.begin();

        result = true;
        while (path_it != prefix_path.end() && other_it != other.end()) {
            if (*path_it != *other_it) {
                result = false;
                break;
            }
            ++path_it;
            ++other_it;
        }

        if (path_it != prefix_path.end()) { result = false; }
    } else {
        interp.set_fatal_error("cmake_path: unknown query '" + args[2] + "'");
        return;
    }

    interp.set_variable(out_var, result ? "ON" : "OFF");
}

// COMPARE
void handle_compare(Interpreter& interp, const std::vector<std::string>& args) {
    if (args.size() < 5) {
        interp.set_fatal_error("cmake_path(COMPARE) requires at least 5 arguments");
        return;
    }

    // args[1] and args[3] are variable names, dereference them
    const std::string& path1 = interp.get_variable(args[1]);
    const auto& op = args[2];
    const std::string& path2 = interp.get_variable(args[3]);
    std::string out_var = args[4];
    bool result = false;

    if (ci_equals(op, "EQUAL")) {
        result = (path1 == path2);
    } else if (ci_equals(op, "NOT_EQUAL")) {
        result = (path1 != path2);
    } else {
        interp.set_fatal_error("cmake_path(COMPARE): unknown operator '" + args[2] + "'");
        return;
    }

    interp.set_variable(out_var, result ? "ON" : "OFF");
}

// SET
void handle_set(Interpreter& interp, const std::vector<std::string>& args) {
    if (args.size() < 3) {
        interp.set_fatal_error("cmake_path(SET) requires at least 3 arguments");
        return;
    }

    std::string path_var = args[1];
    std::string input = args[2];
    bool normalize = false;

    // Check for NORMALIZE flag
    for (size_t i = 3; i < args.size(); ++i) {
        if (ci_equals(args[i], "NORMALIZE")) { normalize = true; }
    }

    if (normalize) {
        interp.set_variable(path_var, Path(input).lexically_normal().str());
    } else {
        interp.set_variable(path_var, input);
    }
}

// APPEND
void handle_append(Interpreter& interp, std::vector<std::string> args) {
    if (args.size() < 3) {
        interp.set_fatal_error("cmake_path(APPEND) requires at least 3 arguments");
        return;
    }

    std::string path_var = args[1];
    Path path(interp.get_variable(path_var));

    // Check for OUTPUT_VARIABLE
    std::string output_var = path_var;
    for (size_t i = 2; i < args.size(); ++i) {
        if (ci_equals(args[i], "OUTPUT_VARIABLE") && i + 1 < args.size()) {
            output_var = args[i + 1];
            // Remove OUTPUT_VARIABLE and its value from processing
            std::vector<std::string> new_args(args.begin(), args.begin() + i);
            new_args.insert(new_args.end(), args.begin() + i + 2, args.end());
            args = new_args;
            break;
        }
    }

    // Append all remaining arguments as path components
    for (size_t i = 2; i < args.size(); ++i) { path = path / args[i]; }

    interp.set_variable(output_var, path.str());
}

// APPEND_STRING
void handle_append_string(Interpreter& interp, std::vector<std::string> args) {
    if (args.size() < 3) {
        interp.set_fatal_error("cmake_path(APPEND_STRING) requires at least 3 arguments");
        return;
    }

    std::string path_var = args[1];
    std::string path_str = interp.get_variable(path_var);

    // Check for OUTPUT_VARIABLE
    std::string output_var = path_var;
    for (size_t i = 2; i < args.size(); ++i) {
        if (ci_equals(args[i], "OUTPUT_VARIABLE") && i + 1 < args.size()) {
            output_var = args[i + 1];
            // Remove OUTPUT_VARIABLE and its value from processing
            std::vector<std::string> new_args(args.begin(), args.begin() + i);
            new_args.insert(new_args.end(), args.begin() + i + 2, args.end());
            args = new_args;
            break;
        }
    }

    // Append all remaining arguments as strings
    for (size_t i = 2; i < args.size(); ++i) { path_str += args[i]; }

    interp.set_variable(output_var, path_str);
}

// REMOVE_FILENAME
void handle_remove_filename(Interpreter& interp, const std::vector<std::string>& args) {
    if (args.size() < 2) {
        interp.set_fatal_error("cmake_path(REMOVE_FILENAME) requires at least 2 arguments");
        return;
    }

    std::string path_var = args[1];
    Path path(interp.get_variable(path_var));

    // Check for OUTPUT_VARIABLE
    std::string output_var = path_var;
    if (args.size() > 2 && ci_equals(args[2], "OUTPUT_VARIABLE") && args.size() > 3) { output_var = args[3]; }

    // parent_path gives the directory part; add trailing slash to match CMake behavior
    auto pp = path.parent_path();
    std::string result;
    if (!pp.empty()) {
        result = std::string(pp);
        if (result.back() != '/') result += '/';
    }
    interp.set_variable(output_var, result);
}

// REPLACE_FILENAME
void handle_replace_filename(Interpreter& interp, const std::vector<std::string>& args) {
    if (args.size() < 3) {
        interp.set_fatal_error("cmake_path(REPLACE_FILENAME) requires at least 3 arguments");
        return;
    }

    std::string path_var = args[1];
    std::string new_filename = args[2];
    Path path(interp.get_variable(path_var));

    // Check for OUTPUT_VARIABLE
    std::string output_var = path_var;
    if (args.size() > 3 && ci_equals(args[3], "OUTPUT_VARIABLE") && args.size() > 4) { output_var = args[4]; }

    // Replace filename: parent_path / new_filename
    interp.set_variable(output_var, Path::join(path.parent_path(), new_filename));
}

// REMOVE_EXTENSION
void handle_remove_extension(Interpreter& interp, const std::vector<std::string>& args) {
    if (args.size() < 2) {
        interp.set_fatal_error("cmake_path(REMOVE_EXTENSION) requires at least 2 arguments");
        return;
    }

    std::string path_var = args[1];
    Path path(interp.get_variable(path_var));
    bool last_only = false;

    // Check for LAST_ONLY and OUTPUT_VARIABLE
    std::string output_var = path_var;
    for (size_t i = 2; i < args.size(); ++i) {
        if (ci_equals(args[i], "LAST_ONLY")) {
            last_only = true;
        } else if (ci_equals(args[i], "OUTPUT_VARIABLE") && i + 1 < args.size()) {
            output_var = args[i + 1];
        }
    }

    auto fname = path.filename();
    std::string stem;

    if (!fname.empty() && fname != "." && fname != "..") {
        if (is_dotfile(fname)) {
            stem = "";
        } else if (last_only) {
            size_t dot_pos = fname.find_last_of('.');
            stem = (dot_pos != std::string::npos && dot_pos > 0) ? std::string(fname.substr(0, dot_pos)) : std::string(fname);
        } else {
            size_t dot_pos = fname.find_first_of('.', 1);
            stem = (dot_pos != std::string::npos) ? std::string(fname.substr(0, dot_pos)) : std::string(fname);
        }
    } else {
        stem = std::string(fname);
    }

    interp.set_variable(output_var, Path::join(path.parent_path(), stem));
}

// REPLACE_EXTENSION
void handle_replace_extension(Interpreter& interp, const std::vector<std::string>& args) {
    if (args.size() < 3) {
        interp.set_fatal_error("cmake_path(REPLACE_EXTENSION) requires at least 3 arguments");
        return;
    }

    std::string path_var = args[1];
    std::string new_ext = args[2];
    Path path(interp.get_variable(path_var));
    bool last_only = false;

    // Check for LAST_ONLY and OUTPUT_VARIABLE
    std::string output_var = path_var;
    for (size_t i = 3; i < args.size(); ++i) {
        if (ci_equals(args[i], "LAST_ONLY")) {
            last_only = true;
        } else if (ci_equals(args[i], "OUTPUT_VARIABLE") && i + 1 < args.size()) {
            output_var = args[i + 1];
        }
    }

    auto fname = path.filename();
    std::string stem;

    if (!fname.empty() && fname != "." && fname != "..") {
        if (is_dotfile(fname)) {
            stem = "";
        } else if (last_only) {
            size_t dot_pos = fname.find_last_of('.');
            stem = (dot_pos != std::string::npos && dot_pos > 0) ? std::string(fname.substr(0, dot_pos)) : std::string(fname);
        } else {
            size_t dot_pos = fname.find_first_of('.', 1);
            stem = (dot_pos != std::string::npos) ? std::string(fname.substr(0, dot_pos)) : std::string(fname);
        }
    } else {
        stem = std::string(fname);
    }

    // Ensure extension starts with '.' if not empty
    if (!new_ext.empty() && new_ext[0] != '.') { new_ext = "." + new_ext; }

    interp.set_variable(output_var, Path::join(path.parent_path(), stem + new_ext));
}

// NORMAL_PATH
void handle_normal_path(Interpreter& interp, const std::vector<std::string>& args) {
    if (args.size() < 2) {
        interp.set_fatal_error("cmake_path(NORMAL_PATH) requires at least 2 arguments");
        return;
    }

    std::string path_var = args[1];
    std::string out_var = path_var;

    // Check for OUTPUT_VARIABLE
    for (size_t i = 2; i < args.size(); ++i) {
        if (ci_equals(args[i], "OUTPUT_VARIABLE") && i + 1 < args.size()) {
            out_var = args[i + 1];
            break;
        }
    }

    interp.set_variable(out_var, Path(interp.get_variable(path_var)).lexically_normal().str());
}

// RELATIVE_PATH
void handle_relative_path(Interpreter& interp, const std::vector<std::string>& args) {
    if (args.size() < 2) {
        interp.set_fatal_error("cmake_path(RELATIVE_PATH) requires at least 2 arguments");
        return;
    }

    std::string path_var = args[1];
    std::string out_var = path_var;
    std::filesystem::path path(interp.get_variable(path_var));

    // Check for BASE_DIRECTORY and OUTPUT_VARIABLE
    std::filesystem::path base_dir(interp.get_variable("CMAKE_CURRENT_SOURCE_DIR"));
    for (size_t i = 2; i < args.size(); ++i) {
        if (ci_equals(args[i], "BASE_DIRECTORY") && i + 1 < args.size()) {
            base_dir = std::filesystem::path(args[i + 1]);
        } else if (ci_equals(args[i], "OUTPUT_VARIABLE") && i + 1 < args.size()) {
            out_var = args[i + 1];
        }
    }

    std::filesystem::path result = path.lexically_relative(base_dir);
    interp.set_variable(out_var, to_cmake_path(result));
}

// ABSOLUTE_PATH
void handle_absolute_path(Interpreter& interp, const std::vector<std::string>& args) {
    if (args.size() < 2) {
        interp.set_fatal_error("cmake_path(ABSOLUTE_PATH) requires at least 2 arguments");
        return;
    }

    std::string path_var = args[1];
    std::string out_var = path_var;
    Path path(interp.get_variable(path_var));
    bool normalize = false;

    // Check for BASE_DIRECTORY, NORMALIZE, and OUTPUT_VARIABLE
    std::string base_dir = interp.get_variable("CMAKE_CURRENT_SOURCE_DIR");
    for (size_t i = 2; i < args.size(); ++i) {
        if (ci_equals(args[i], "BASE_DIRECTORY") && i + 1 < args.size()) {
            base_dir = args[i + 1];
        } else if (ci_equals(args[i], "NORMALIZE")) {
            normalize = true;
        } else if (ci_equals(args[i], "OUTPUT_VARIABLE") && i + 1 < args.size()) {
            out_var = args[i + 1];
        }
    }

    Path result = path.is_absolute() ? path : Path(Path::join(base_dir, path.str()));

    if (normalize) { result = result.lexically_normal(); }

    interp.set_variable(out_var, result.str());
}

// NATIVE_PATH
void handle_native_path(Interpreter& interp, const std::vector<std::string>& args) {
    if (args.size() < 2) {
        interp.set_fatal_error("cmake_path(NATIVE_PATH) requires at least 2 arguments");
        return;
    }

    std::string path_var = args[1];
    std::string out_var = path_var;
    std::string path_str = interp.get_variable(path_var);

    // Check for NORMALIZE and OUTPUT_VARIABLE
    bool normalize = false;
    for (size_t i = 2; i < args.size(); ++i) {
        if (ci_equals(args[i], "NORMALIZE")) {
            normalize = true;
        } else if (ci_equals(args[i], "OUTPUT_VARIABLE") && i + 1 < args.size()) {
            out_var = args[i + 1];
        }
    }

    if (normalize) { path_str = Path(path_str).lexically_normal().str(); }

    // On Unix, native path == cmake path (forward slashes)
    interp.set_variable(out_var, path_str);
}

// CONVERT
void handle_convert(Interpreter& interp, const std::vector<std::string>& args) {
    if (args.size() < 4) {
        interp.set_fatal_error("cmake_path(CONVERT) requires at least 4 arguments");
        return;
    }

    std::string input = args[1];
    const auto& mode = args[2];
    std::string out_var = args[3];

    if (ci_equals(mode, "TO_CMAKE_PATH_LIST")) {
        // Convert native path list to CMake format
        std::string result;

#ifdef _WIN32
        // Windows: semicolon-separated
        size_t start = 0;
        size_t end = input.find(';');
        while (end != std::string::npos) {
            std::filesystem::path p(input.substr(start, end - start));
            if (!result.empty()) result += ";";
            result += to_cmake_path(p);
            start = end + 1;
            end = input.find(';', start);
        }
        std::filesystem::path p(input.substr(start));
        if (!result.empty()) result += ";";
        result += to_cmake_path(p);
#else
        // Unix: colon-separated
        size_t start = 0;
        size_t end = input.find(':');
        while (end != std::string::npos) {
            std::filesystem::path p(input.substr(start, end - start));
            if (!result.empty()) result += ";";
            result += to_cmake_path(p);
            start = end + 1;
            end = input.find(':', start);
        }
        std::filesystem::path p(input.substr(start));
        if (!result.empty()) result += ";";
        result += to_cmake_path(p);
#endif

        interp.set_variable(out_var, result);
    } else if (ci_equals(mode, "TO_NATIVE_PATH_LIST")) {
        // Convert CMake list to native path list
        std::string result;

        size_t start = 0;
        size_t end = input.find(';');
        while (end != std::string::npos) {
            std::filesystem::path p(input.substr(start, end - start));
            if (!result.empty()) {
#ifdef _WIN32
                result += ";";
#else
                result += ":";
#endif
            }
            result += to_native_path(p);
            start = end + 1;
            end = input.find(';', start);
        }
        std::filesystem::path p(input.substr(start));
        if (!result.empty()) {
#ifdef _WIN32
            result += ";";
#else
            result += ":";
#endif
        }
        result += to_native_path(p);

        interp.set_variable(out_var, result);
    } else {
        interp.set_fatal_error("cmake_path(CONVERT): unknown mode '" + args[2] + "'");
    }
}

// HASH
void handle_hash(Interpreter& interp, const std::vector<std::string>& args) {
    if (args.size() < 3) {
        interp.set_fatal_error("cmake_path(HASH) requires at least 3 arguments");
        return;
    }

    std::string path_var = args[1];
    std::string out_var = args[2];
    std::filesystem::path path(interp.get_variable(path_var));

    // Normalize the path before hashing for consistency
    path = path.lexically_normal();

    // Use std::hash
    size_t hash_value = std::filesystem::hash_value(path);

    interp.set_variable(out_var, std::to_string(hash_value));
}

} // anonymous namespace

void register_path_builtins(Interpreter& interp) {
    interp.add_builtin("get_filename_component", [](Interpreter& interp, const std::vector<std::string>& args) {
        CommandParser parser("get_filename_component");
        std::string var_name;
        std::string filename;
        std::string mode;
        std::string base_dir;
        bool cache = false;

        parser.positional(var_name, "variable name");
        parser.positional(filename, "filename");
        parser.positional(mode, "mode");
        parser.value("BASE_DIR", base_dir);
        parser.flag("CACHE", cache);

        PARSE_OR_RETURN(parser, interp, args);

        Path path(filename);
        std::string result;

        if (mode == "DIRECTORY" || mode == "PATH") {
            result = std::string(path.parent_path());
        } else if (mode == "NAME") {
            result = std::string(path.filename());
        } else if (mode == "EXT") {
            auto name = path.filename();
            if (!name.empty() && !is_dotfile(name)) {
                size_t first_dot = name.find_first_of('.', 1);
                if (first_dot != std::string::npos) { result = std::string(name.substr(first_dot)); }
            }
        } else if (mode == "NAME_WE") {
            auto name = path.filename();
            if (!name.empty()) {
                if (is_dotfile(name)) {
                    result = std::string(name);
                } else {
                    size_t first_dot = name.find_first_of('.', 1);
                    if (first_dot != std::string::npos) {
                        result = std::string(name.substr(0, first_dot));
                    } else {
                        result = std::string(name);
                    }
                }
            }
        } else if (mode == "LAST_EXT") {
            auto name = path.filename();
            if (!name.empty() && !is_dotfile(name)) {
                size_t last_dot = name.find_last_of('.');
                if (last_dot != std::string::npos && last_dot > 0) { result = std::string(name.substr(last_dot)); }
            }
        } else if (mode == "NAME_WLE") {
            auto name = path.filename();
            if (!name.empty()) {
                if (is_dotfile(name)) {
                    result = std::string(name);
                } else {
                    size_t last_dot = name.find_last_of('.');
                    if (last_dot != std::string::npos && last_dot > 0) {
                        result = std::string(name.substr(0, last_dot));
                    } else {
                        result = std::string(name);
                    }
                }
            }
        } else if (mode == "ABSOLUTE" || mode == "REALPATH") {
            Path abs_path = path;
            if (!path.is_absolute()) {
                std::string base = !base_dir.empty() ? base_dir : interp.get_variable("CMAKE_CURRENT_SOURCE_DIR");
                abs_path = Path(Path::join(base, path.str()));
            }

            if (mode == "REALPATH") {
                result = interp.cached_weakly_canonical(abs_path.str());
            } else {
                result = abs_path.lexically_normal().str();
            }
        } else {
            interp.set_fatal_error("Invalid mode '" + mode + "' for get_filename_component()");
            return;
        }

        interp.set_variable(var_name, result);
    });

    interp.add_builtin("cmake_path", [](Interpreter& interp, const std::vector<std::string>& args) {
        if (args.empty()) {
            interp.set_fatal_error("cmake_path() requires at least one argument");
            return;
        }

        const auto& subcmd = args[0];

        // GET subcommands
        if (ci_equals(subcmd, "GET")) {
            handle_get(interp, args);
        }
        // HAS_* queries
        else if (ci_equals(subcmd, "HAS_ROOT_NAME") || ci_equals(subcmd, "HAS_ROOT_DIRECTORY") || ci_equals(subcmd, "HAS_ROOT_PATH")
                 || ci_equals(subcmd, "HAS_FILENAME") || ci_equals(subcmd, "HAS_EXTENSION") || ci_equals(subcmd, "HAS_STEM")
                 || ci_equals(subcmd, "HAS_RELATIVE_PATH") || ci_equals(subcmd, "HAS_PARENT_PATH")) {
            handle_has(interp, args);
        }
        // IS_* queries
        else if (ci_equals(subcmd, "IS_ABSOLUTE") || ci_equals(subcmd, "IS_RELATIVE") || ci_equals(subcmd, "IS_PREFIX")) {
            handle_is(interp, args);
        }
        // COMPARE
        else if (ci_equals(subcmd, "COMPARE")) {
            handle_compare(interp, args);
        }
        // SET
        else if (ci_equals(subcmd, "SET")) {
            handle_set(interp, args);
        }
        // APPEND
        else if (ci_equals(subcmd, "APPEND")) {
            handle_append(interp, args);
        }
        // APPEND_STRING
        else if (ci_equals(subcmd, "APPEND_STRING")) {
            handle_append_string(interp, args);
        }
        // REMOVE_FILENAME
        else if (ci_equals(subcmd, "REMOVE_FILENAME")) {
            handle_remove_filename(interp, args);
        }
        // REPLACE_FILENAME
        else if (ci_equals(subcmd, "REPLACE_FILENAME")) {
            handle_replace_filename(interp, args);
        }
        // REMOVE_EXTENSION
        else if (ci_equals(subcmd, "REMOVE_EXTENSION")) {
            handle_remove_extension(interp, args);
        }
        // REPLACE_EXTENSION
        else if (ci_equals(subcmd, "REPLACE_EXTENSION")) {
            handle_replace_extension(interp, args);
        }
        // NORMAL_PATH
        else if (ci_equals(subcmd, "NORMAL_PATH")) {
            handle_normal_path(interp, args);
        }
        // RELATIVE_PATH
        else if (ci_equals(subcmd, "RELATIVE_PATH")) {
            handle_relative_path(interp, args);
        }
        // ABSOLUTE_PATH
        else if (ci_equals(subcmd, "ABSOLUTE_PATH")) {
            handle_absolute_path(interp, args);
        }
        // NATIVE_PATH
        else if (ci_equals(subcmd, "NATIVE_PATH")) {
            handle_native_path(interp, args);
        }
        // CONVERT
        else if (ci_equals(subcmd, "CONVERT")) {
            handle_convert(interp, args);
        }
        // HASH
        else if (ci_equals(subcmd, "HASH")) {
            handle_hash(interp, args);
        } else {
            interp.set_fatal_error("cmake_path: unknown subcommand '" + args[0] + "'");
        }
    });
}

} // namespace kiln
