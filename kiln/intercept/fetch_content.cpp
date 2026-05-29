#include "fetch_content.hpp"
#include "download_utils.hpp"
#include "../interperter.hpp"
#include "../command_parser.hpp"
#include "../utils.hpp"
#include "../profiler.hpp"
#include <filesystem>
#include <algorithm>
#include <fstream>
#include <unordered_set>
#include <unordered_map>

namespace kiln {

namespace fs = std::filesystem;

// Internal helper to populate a dependency (download + patch).
// Sets <name>_POPULATED, <name>_SOURCE_DIR, <name>_BINARY_DIR in the interpreter.
static void do_populate(Interpreter& interp, const std::string& name) {
    std::string lower_name = name;
    std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), [](unsigned char c) { return std::tolower(c); });
    std::string upper_name = name;
    std::transform(upper_name.begin(), upper_name.end(), upper_name.begin(), [](unsigned char c) { return std::toupper(c); });

    // Check if already populated
    auto& global_props = interp.get_global_properties();
    std::string populated_key = "_FetchContent_" + lower_name + "_populated";
    if (global_props.count(populated_key)) { return; }

    // Read stored declaration details
    std::string details_key = "_FetchContent_" + lower_name + "_savedDetails";
    auto it = global_props.find(details_key);
    if (it == global_props.end()) {
        interp.set_fatal_error("FetchContent_Populate: no declaration for '" + name + "'");
        return;
    }

    // Parse the saved declaration (semicolon-separated tokens)
    // Tokens are: KEYWORD val1 val2 ... KEYWORD val1 ...
    // Some keywords (like PATCH_COMMAND) take multiple values, so we can't
    // assume strict key-value pairs. Instead, collect values per keyword
    // until the next known keyword.
    static const std::unordered_set<std::string> known_keywords = {
        "URL",
        "URL_HASH",
        "URL_MD5",
        "GIT_REPOSITORY",
        "GIT_TAG",
        "GIT_SHALLOW",
        "GIT_CONFIG",
        "GIT_REMOTE_NAME",
        "GIT_SUBMODULES",
        "GIT_SUBMODULES_RECURSE",
        "GIT_PROGRESS",
        "SOURCE_DIR",
        "BINARY_DIR",
        "SOURCE_SUBDIR",
        "PATCH_COMMAND",
        "UPDATE_COMMAND",
        "CONFIGURE_COMMAND",
        "BUILD_COMMAND",
        "INSTALL_COMMAND",
        "TEST_COMMAND",
        "DOWNLOAD_COMMAND",
        "CMAKE_ARGS",
        "CMAKE_CACHE_ARGS",
        "DOWNLOAD_NO_EXTRACT",
        "DOWNLOAD_EXTRACT_TIMESTAMP",
        "FIND_PACKAGE_ARGS",
        "OVERRIDE_FIND_PACKAGE",
        "HTTPHEADER",
        "HTTP_HEADER",
        "TIMEOUT",
        "PREFIX",
        "TMP_DIR",
        "STAMP_DIR",
        "DOWNLOAD_DIR",
        "LOG_DOWNLOAD",
        "LOG_CONFIGURE",
        "LOG_BUILD",
        "LOG_INSTALL",
        "LOG_UPDATE",
        "LOG_PATCH",
        "LOG_TEST",
        "LOG_MERGED_STDOUTERR",
        "LOG_OUTPUT_ON_FAILURE",
        "USES_TERMINAL_DOWNLOAD",
        "USES_TERMINAL_CONFIGURE",
        "USES_TERMINAL_BUILD",
        "USES_TERMINAL_INSTALL",
        "BUILD_IN_SOURCE",
        "EXCLUDE_FROM_ALL",
        "BUILD_ALWAYS",
        "DEPENDS",
        "BUILD_BYPRODUCTS",
        "LIST_SEPARATOR",
    };

    std::vector<std::string> detail_parts;
    {
        std::string_view details = it->second;
        size_t start = 0;
        while (start < details.size()) {
            auto end = details.find(';', start);
            if (end == std::string_view::npos) end = details.size();
            detail_parts.emplace_back(details.substr(start, end - start));
            start = end + 1;
        }
    }

    // Build keyword -> list of values map
    std::unordered_map<std::string, std::vector<std::string>> detail_map;
    std::string current_key;
    for (const auto& part : detail_parts) {
        if (known_keywords.count(part)) {
            current_key = part;
            if (!detail_map.count(current_key)) { detail_map[current_key] = {}; }
        } else if (!current_key.empty()) {
            detail_map[current_key].push_back(part);
        }
    }

    auto get_detail = [&](const std::string& key) -> std::string {
        auto map_it = detail_map.find(key);
        if (map_it != detail_map.end() && !map_it->second.empty()) { return map_it->second[0]; }
        return "";
    };

    auto get_detail_flag = [&](const std::string& key) -> bool { return detail_map.count(key) > 0; };

    // Get all values for a keyword, joined with spaces (for shell commands)
    auto get_detail_command = [&](const std::string& key) -> std::string {
        auto map_it = detail_map.find(key);
        if (map_it == detail_map.end() || map_it->second.empty()) { return ""; }
        std::string result;
        for (const auto& v : map_it->second) {
            if (!result.empty()) result += ' ';
            result += v;
        }
        return result;
    };

    // Resolve source/binary dirs
    // Source dir: use KILN_BUILD_ROOT/_deps (shared across configs)
    // Binary dir: use CMAKE_BINARY_DIR/_deps (config-specific)
    std::string build_root = interp.get_variable("KILN_BUILD_ROOT");
    std::string binary_base = interp.get_variable("CMAKE_BINARY_DIR");

    // Check for user override via FETCHCONTENT_BASE_DIR
    // When set, it's the directory containing <name>-src and <name>-build directly
    std::string base_dir = interp.get_variable("FETCHCONTENT_BASE_DIR");
    bool use_base_dir_directly = !base_dir.empty();
    if (use_base_dir_directly) {
        // User/system set FETCHCONTENT_BASE_DIR - use it directly (no extra _deps)
        build_root = base_dir;
        binary_base = base_dir;
    } else {
        // Use shared source dir under build root
        if (build_root.empty()) {
            build_root = binary_base; // Fallback
        }
    }

    std::string source_dir = get_detail("SOURCE_DIR");
    if (source_dir.empty()) {
        // If FETCHCONTENT_BASE_DIR is set, it already points to the _deps directory
        fs::path src_path =
            use_base_dir_directly ? fs::path(build_root) / (lower_name + "-src") : fs::path(build_root) / "_deps" / (lower_name + "-src");
        source_dir = src_path.string();
    }

    std::string binary_dir = get_detail("BINARY_DIR");
    if (binary_dir.empty()) {
        fs::path bin_path = use_base_dir_directly ? fs::path(binary_base) / (lower_name + "-build")
                                                  : fs::path(binary_base) / "_deps" / (lower_name + "-build");
        binary_dir = bin_path.string();
    }

    // Check if source dir already has content
    bool has_content = false;
    if (std::filesystem::exists(source_dir)) {
        auto dir_it = std::filesystem::directory_iterator(source_dir);
        has_content = (dir_it != std::filesystem::directory_iterator{});
    }

    if (!has_content) {
        std::string url = get_detail("URL");
        std::string git_repository = get_detail("GIT_REPOSITORY");

        if (!url.empty()) {
            // URL download (or local file path)
            std::string url_hash = get_detail("URL_HASH");
            std::string url_md5 = get_detail("URL_MD5");
            std::string hash_algo, hash_value;

            if (!url_hash.empty()) {
                auto eq_pos = url_hash.find('=');
                if (eq_pos != std::string::npos) {
                    hash_algo = url_hash.substr(0, eq_pos);
                    hash_value = url_hash.substr(eq_pos + 1);
                    std::transform(hash_algo.begin(), hash_algo.end(), hash_algo.begin(), [](unsigned char c) { return std::toupper(c); });
                }
            } else if (!url_md5.empty()) {
                hash_algo = "MD5";
                hash_value = url_md5;
            }

            // Determine if this is a local file or remote URL
            bool is_local = std::filesystem::exists(url);
            if (!is_local && url.starts_with("file://")) {
                // file:// URL scheme — strip prefix
                url = url.substr(7);
                is_local = true;
            }

            std::string archive_path;
            if (is_local) {
                // Local file: use directly, no download needed
                archive_path = url;
                interp.print_message("STATUS", "Extracting " + name + " from local archive " + archive_path + "...");
            } else {
                // Remote URL: download first
                std::string filename = fs::path(url).filename().string();
                auto qpos = filename.find('?');
                if (qpos != std::string::npos) filename = filename.substr(0, qpos);
                fs::path download_dir = fs::path(build_root) / (lower_name + "-subbuild");
                archive_path = (download_dir / filename).string();

                interp.print_message("STATUS", "Fetching " + name + " from " + url + "...");
                auto dl_result = download_url(url, archive_path, hash_algo, hash_value, interp.error_stream());
                if (!dl_result) {
                    interp.set_fatal_error("FetchContent: download failed for " + name + ": " + dl_result.error());
                    return;
                }
            }

            auto ex_result = extract_archive(archive_path, source_dir);
            if (!ex_result) {
                interp.set_fatal_error("FetchContent: extraction failed for " + name + ": " + ex_result.error());
                return;
            }

            // Handle single top-level directory (common for tarballs)
            std::vector<std::filesystem::directory_entry> entries;
            for (const auto& e : std::filesystem::directory_iterator(source_dir)) { entries.push_back(e); }
            if (entries.size() == 1 && entries[0].is_directory()) {
                std::string top_dir = entries[0].path().string();
                std::string temp_dir = source_dir + ".__tmp_move";
                std::filesystem::rename(top_dir, temp_dir);
                for (const auto& e : std::filesystem::directory_iterator(temp_dir)) {
                    std::filesystem::rename(e.path(), std::filesystem::path(source_dir) / e.path().filename());
                }
                std::filesystem::remove_all(temp_dir);
            }
        } else if (!git_repository.empty()) {
            std::string git_tag = get_detail("GIT_TAG");
            bool git_shallow = get_detail_flag("GIT_SHALLOW");

            interp.print_message("STATUS", "Fetching " + name + " from " + git_repository + "...");
            auto git_result = git_clone(git_repository, source_dir, git_tag, git_shallow);
            if (!git_result) {
                interp.set_fatal_error("FetchContent: git clone failed for " + name + ": " + git_result.error());
                return;
            }
        } else {
            interp.set_fatal_error("FetchContent_Populate(" + name + "): no URL or GIT_REPOSITORY declared");
            return;
        }

        // Execute patch command(s) if declared (only on fresh download)
        // PATCH_COMMAND may contain multiple commands separated by "COMMAND" tokens
        if (get_detail_flag("PATCH_COMMAND")) {
            auto map_it = detail_map.find("PATCH_COMMAND");
            if (map_it != detail_map.end() && !map_it->second.empty()) {
                // Split on "COMMAND" tokens to get individual commands
                std::vector<std::string> commands;
                std::string current_cmd;
                for (const auto& token : map_it->second) {
                    if (token == "COMMAND") {
                        if (!current_cmd.empty()) {
                            commands.push_back(std::move(current_cmd));
                            current_cmd.clear();
                        }
                    } else {
                        if (!current_cmd.empty()) current_cmd += ' ';
                        current_cmd += token;
                    }
                }
                if (!current_cmd.empty()) { commands.push_back(std::move(current_cmd)); }

                for (const auto& cmd : commands) {
                    auto result = run_command(cmd, source_dir);
                    if (result.exit_code != 0) {
                        interp.set_fatal_error("FetchContent: patch failed for " + name + ":\n" + result.output);
                        return;
                    }
                }
            }
        }
    }

    // Mark as populated
    global_props[populated_key] = "TRUE";
    global_props["_FetchContent_" + lower_name + "_sourceDir"] = source_dir;
    global_props["_FetchContent_" + lower_name + "_binaryDir"] = binary_dir;

    // Set variables as cache variables so they survive block() scopes (matching CMake behavior)
    interp.set_cache_variable(lower_name + "_POPULATED", "TRUE");
    interp.set_cache_variable(lower_name + "_SOURCE_DIR", source_dir);
    interp.set_cache_variable(lower_name + "_BINARY_DIR", binary_dir);
    if (name != lower_name) {
        interp.set_cache_variable(name + "_POPULATED", "TRUE");
        interp.set_cache_variable(name + "_SOURCE_DIR", source_dir);
        interp.set_cache_variable(name + "_BINARY_DIR", binary_dir);
    }
}

// Helper: add_subdirectory logic for a fetched dependency
static void add_subdirectory_for_dep(Interpreter& interp, const std::string& source_dir, const std::string& binary_dir) {
    std::filesystem::path cmake_file = std::filesystem::path(source_dir) / "CMakeLists.txt";

    if (!std::filesystem::exists(cmake_file)) {
        return; // No CMakeLists.txt, nothing to add
    }

    std::ifstream file(cmake_file);
    if (!file) {
        interp.set_fatal_error("Could not read " + cmake_file.string());
        return;
    }
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    std::string saved_file = interp.get_current_file();

    std::filesystem::path abs_source = std::filesystem::absolute(source_dir).lexically_normal();
    std::filesystem::path abs_binary = std::filesystem::absolute(binary_dir).lexically_normal();

    interp.get_variables().push_scope();
    interp.push_directory(abs_source.string(), abs_binary.string());

    interp.set_variable("CMAKE_CURRENT_SOURCE_DIR", abs_source.string());
    interp.set_variable("CMAKE_CURRENT_BINARY_DIR", abs_binary.string());
    interp.set_variable("CMAKE_CURRENT_LIST_FILE", cmake_file.string());
    interp.set_variable("CMAKE_CURRENT_LIST_DIR", abs_source.string());
    interp.set_current_file(cmake_file.string());

    ProfileScope parse_profile("parse " + cmake_file.filename().string(), "parse");
    Parser parser(content, cmake_file.string());
    auto ast = parser.parse();
    parse_profile.stop();

    {
        ReturnGuard rg(interp);

        if (ast) {
            ProfileScope interpret_profile("interpret " + cmake_file.filename().string(), "interpret");
            auto res = interp.interpret(ast.value());
            interpret_profile.stop();
            if (!res) {
                interp.set_fatal_error(res.error());
            } else {
                interp.finalize_directory_targets();
            }
        } else {
            interp.set_fatal_error(InterpreterError{
                cmake_file.string(), ast.error().row, ast.error().col, ast.error().offset, ast.error().length, ast.error().reason, {}});
        }
    }

    interp.pop_directory();
    interp.get_variables().pop_scope();
    interp.set_current_file(saved_file);
}

void register_fetch_content_builtins(Interpreter& interp) {

    interp.add_builtin("fetchcontent_declare", [](Interpreter& interp, const std::vector<std::string>& args) {
        if (args.empty()) {
            interp.set_fatal_error("FetchContent_Declare requires a name");
            return;
        }

        // Initialize FETCHCONTENT_BASE_DIR if not set (matches CMake's FetchContent module)
        // This must happen before args are processed since they may reference this variable
        if (interp.get_variable("FETCHCONTENT_BASE_DIR").empty()) {
            std::string build_root = interp.get_variable("KILN_BUILD_ROOT");
            if (build_root.empty()) { build_root = interp.get_variable("CMAKE_BINARY_DIR"); }
            if (!build_root.empty()) { interp.set_cache_variable("FETCHCONTENT_BASE_DIR", build_root + "/_deps"); }
        }

        std::string name = args[0];
        std::string lower_name = name;
        std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), [](unsigned char c) { return std::tolower(c); });

        // First-to-declare wins
        std::string details_key = "_FetchContent_" + lower_name + "_savedDetails";
        auto& global_props = interp.get_global_properties();
        if (global_props.count(details_key)) { return; }

        // Store all remaining args as semicolon-separated pairs
        // We store them as: KEY1;VALUE1;KEY2;VALUE2;...
        // For flags without values, store as: FLAG_NAME;__FLAG__
        std::string details;
        for (size_t i = 1; i < args.size(); ++i) {
            if (!details.empty()) details += ';';
            details += args[i];
        }

        global_props[details_key] = details;

        // Store the original name
        global_props["_FetchContent_" + lower_name + "_originalName"] = name;
    });

    interp.add_builtin("fetchcontent_populate", [](Interpreter& interp, const std::vector<std::string>& args) {
        if (args.empty()) {
            interp.set_fatal_error("FetchContent_Populate requires a name");
            return;
        }
        do_populate(interp, args[0]);
    });

    interp.add_builtin("fetchcontent_makeavailable", [](Interpreter& interp, const std::vector<std::string>& args) {
        for (const auto& name : args) {
            std::string lower_name = to_lower(name);
            std::string upper_name = to_upper(name);

            // Check for local override
            std::string override_dir = interp.get_variable("FETCHCONTENT_SOURCE_DIR_" + upper_name);
            if (!override_dir.empty()) {
                // Binary dir computation - if FETCHCONTENT_BASE_DIR is set, use it directly
                std::string base_dir = interp.get_variable("FETCHCONTENT_BASE_DIR");
                fs::path bin_path;
                if (!base_dir.empty()) {
                    bin_path = fs::path(base_dir) / (lower_name + "-build");
                } else {
                    bin_path = fs::path(interp.get_variable("CMAKE_BINARY_DIR")) / "_deps" / (lower_name + "-build");
                }
                std::string binary_dir = bin_path.string();

                interp.set_cache_variable(lower_name + "_POPULATED", "TRUE");
                interp.set_cache_variable(lower_name + "_SOURCE_DIR", override_dir);
                interp.set_cache_variable(lower_name + "_BINARY_DIR", binary_dir);
                if (name != lower_name) {
                    interp.set_cache_variable(name + "_POPULATED", "TRUE");
                    interp.set_cache_variable(name + "_SOURCE_DIR", override_dir);
                    interp.set_cache_variable(name + "_BINARY_DIR", binary_dir);
                }

                add_subdirectory_for_dep(interp, override_dir, binary_dir);
                continue;
            }

            // Check if FIND_PACKAGE_ARGS was declared
            auto& global_props = interp.get_global_properties();
            std::string details_key = "_FetchContent_" + lower_name + "_savedDetails";
            auto it = global_props.find(details_key);
            if (it != global_props.end()) {
                // Check for FIND_PACKAGE_ARGS in the details
                bool has_find_package_args = (it->second.find("FIND_PACKAGE_ARGS") != std::string::npos);
                if (has_find_package_args) {
                    // Try find_package first — if the target already exists from a system install,
                    // we skip the fetch. We use a simple heuristic: call find_package in quiet mode.
                    // But this is tricky since we'd need to parse the args. For now, skip this
                    // optimization and always fetch.
                }
            }

            // Populate (download)
            do_populate(interp, name);
            if (interp.has_accumulated_errors()) return;

            // Get source and binary dirs
            std::string source_dir = interp.get_variable(lower_name + "_SOURCE_DIR");
            std::string binary_dir = interp.get_variable(lower_name + "_BINARY_DIR");

            // Check for SOURCE_SUBDIR in the declaration
            if (it != global_props.end()) {
                std::string_view details = it->second;
                size_t pos = details.find("SOURCE_SUBDIR;");
                if (pos != std::string_view::npos) {
                    size_t start = pos + 14; // length of "SOURCE_SUBDIR;"
                    size_t end = details.find(';', start);
                    std::string subdir(details.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
                    if (!subdir.empty()) { source_dir = (fs::path(source_dir) / subdir).string(); }
                }
            }

            // add_subdirectory if CMakeLists.txt exists
            add_subdirectory_for_dep(interp, source_dir, binary_dir);
        }
    });

    interp.add_builtin("fetchcontent_getproperties", [](Interpreter& interp, const std::vector<std::string>& args) {
        if (args.empty()) {
            interp.set_fatal_error("FetchContent_GetProperties requires a name");
            return;
        }

        std::string name = args[0];
        std::string lower_name = name;
        std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), [](unsigned char c) { return std::tolower(c); });

        auto& global_props = interp.get_global_properties();

        // Parse optional keyword arguments
        std::string populated_var, source_dir_var, binary_dir_var;
        for (size_t i = 1; i + 1 < args.size(); i += 2) {
            if (args[i] == "POPULATED")
                populated_var = args[i + 1];
            else if (args[i] == "SOURCE_DIR")
                source_dir_var = args[i + 1];
            else if (args[i] == "BINARY_DIR")
                binary_dir_var = args[i + 1];
        }

        // If no keyword args, use default variable names
        if (populated_var.empty() && source_dir_var.empty() && binary_dir_var.empty()) {
            // Default: set <lowername>_POPULATED, <lowername>_SOURCE_DIR, <lowername>_BINARY_DIR
            std::string populated_key = "_FetchContent_" + lower_name + "_populated";
            bool is_populated = global_props.count(populated_key) > 0;

            interp.set_variable(lower_name + "_POPULATED", is_populated ? "TRUE" : "FALSE");
            if (is_populated) {
                auto src_it = global_props.find("_FetchContent_" + lower_name + "_sourceDir");
                auto bin_it = global_props.find("_FetchContent_" + lower_name + "_binaryDir");
                if (src_it != global_props.end()) interp.set_variable(lower_name + "_SOURCE_DIR", src_it->second);
                if (bin_it != global_props.end()) interp.set_variable(lower_name + "_BINARY_DIR", bin_it->second);
            }
        } else {
            if (!populated_var.empty()) {
                std::string populated_key = "_FetchContent_" + lower_name + "_populated";
                interp.set_variable(populated_var, global_props.count(populated_key) ? "TRUE" : "FALSE");
            }
            if (!source_dir_var.empty()) {
                auto it = global_props.find("_FetchContent_" + lower_name + "_sourceDir");
                interp.set_variable(source_dir_var, it != global_props.end() ? it->second : "");
            }
            if (!binary_dir_var.empty()) {
                auto it = global_props.find("_FetchContent_" + lower_name + "_binaryDir");
                interp.set_variable(binary_dir_var, it != global_props.end() ? it->second : "");
            }
        }
    });

    interp.add_builtin("fetchcontent_setpopulated", [](Interpreter& interp, const std::vector<std::string>& args) {
        // FetchContent_SetPopulated(<name> [SOURCE_DIR <dir>] [BINARY_DIR <dir>])
        // Used by CPM to mark packages as populated without actually fetching
        if (args.empty()) {
            interp.set_fatal_error("FetchContent_SetPopulated requires a name");
            return;
        }

        std::string name = args[0];
        std::string lower_name = name;
        std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), [](unsigned char c) { return std::tolower(c); });

        auto& global_props = interp.get_global_properties();
        global_props["_FetchContent_" + lower_name + "_populated"] = "TRUE";

        // Parse optional SOURCE_DIR/BINARY_DIR
        for (size_t i = 1; i + 1 < args.size(); i += 2) {
            if (args[i] == "SOURCE_DIR") {
                global_props["_FetchContent_" + lower_name + "_sourceDir"] = args[i + 1];
                interp.set_variable(lower_name + "_SOURCE_DIR", args[i + 1]);
            } else if (args[i] == "BINARY_DIR") {
                global_props["_FetchContent_" + lower_name + "_binaryDir"] = args[i + 1];
                interp.set_variable(lower_name + "_BINARY_DIR", args[i + 1]);
            }
        }

        interp.set_variable(lower_name + "_POPULATED", "TRUE");
    });
}

} // namespace kiln
