#include "external_project.hpp"
#include "external_project_target.hpp"
#include "download_utils.hpp"
#include "../interperter.hpp"
#include "../command_parser.hpp"
#include "../utils.hpp"
#include "../genex_evaluator.hpp"
#include "../genex_parser.hpp"
#include <filesystem>
#include <algorithm>

namespace kiln {

// Helper to create an EPStepCommand from a flat vector of strings,
// splitting on "COMMAND" tokens to support multiple commands per step.
static EPStepCommand make_step_command(const std::vector<std::string>& args) {
    EPStepCommand result;
    if (args.size() == 1 && args[0].empty()) {
        result.is_empty = true;
        return result;
    }
    // Split on COMMAND tokens
    std::vector<std::string> current;
    for (const auto& token : args) {
        if (token == "COMMAND") {
            if (!current.empty()) {
                result.commands.push_back(std::move(current));
                current.clear();
            }
        } else {
            current.push_back(token);
        }
    }
    if (!current.empty()) { result.commands.push_back(std::move(current)); }
    return result;
}

void register_external_project_builtins(Interpreter& interp) {

    interp.add_builtin("externalproject_add", [](Interpreter& interp, const std::vector<std::string>& args) {
        CommandParser parser("ExternalProject_Add");
        std::string name;
        // Directory options
        std::string prefix, source_dir, binary_dir, install_dir, tmp_dir, stamp_dir, download_dir;
        // URL download
        std::vector<std::string> url_list;
        std::string url_hash, url_md5;
        // Git download
        std::string git_repository, git_tag;
        bool git_shallow = false;
        // Build options
        std::vector<std::string> cmake_args, cmake_cache_args;
        std::string list_separator;
        // Step commands
        std::vector<std::string> configure_command, build_command, install_command;
        std::vector<std::string> patch_command, update_command, test_command;
        // Misc
        std::vector<std::string> depends, build_byproducts;
        bool build_in_source = false, exclude_from_all = false, build_always = false;
        std::string download_no_extract_str;
        std::vector<std::string> download_command;
        std::string source_subdir;

        parser.positional(name, "name");
        parser.value("PREFIX", prefix);
        parser.value("SOURCE_DIR", source_dir);
        parser.value("BINARY_DIR", binary_dir);
        parser.value("INSTALL_DIR", install_dir);
        parser.value("TMP_DIR", tmp_dir);
        parser.value("STAMP_DIR", stamp_dir);
        parser.value("DOWNLOAD_DIR", download_dir);
        parser.value("SOURCE_SUBDIR", source_subdir);

        parser.list("URL", url_list);
        parser.value("URL_HASH", url_hash);
        parser.value("URL_MD5", url_md5);

        parser.value("GIT_REPOSITORY", git_repository);
        parser.value("GIT_TAG", git_tag);
        std::string git_shallow_str;
        parser.value("GIT_SHALLOW", git_shallow_str);

        parser.list("CMAKE_ARGS", cmake_args);
        parser.list("CMAKE_CACHE_ARGS", cmake_cache_args);
        parser.value("LIST_SEPARATOR", list_separator);

        parser.list("CONFIGURE_COMMAND", configure_command);
        parser.list("BUILD_COMMAND", build_command);
        parser.list("INSTALL_COMMAND", install_command);
        parser.list("PATCH_COMMAND", patch_command);
        parser.list("UPDATE_COMMAND", update_command);
        parser.list("TEST_COMMAND", test_command);
        parser.list("DOWNLOAD_COMMAND", download_command);

        parser.list("DEPENDS", depends);
        parser.list("BUILD_BYPRODUCTS", build_byproducts);
        std::string build_in_source_str, exclude_from_all_str, build_always_str;
        parser.value("BUILD_IN_SOURCE", build_in_source_str);
        parser.value("EXCLUDE_FROM_ALL", exclude_from_all_str);
        parser.value("BUILD_ALWAYS", build_always_str);
        parser.value("DOWNLOAD_NO_EXTRACT", download_no_extract_str);

        // Ignored options (accepted but not used)
        std::string log_download, log_configure, log_build, log_install;
        std::string log_update, log_patch, log_test, log_merged_stdouterr, log_output_on_failure;
        bool uses_terminal_download = false, uses_terminal_configure = false;
        bool uses_terminal_build = false, uses_terminal_install = false;
        std::vector<std::string> git_config;
        std::string git_remote_name, git_submodules, git_submodules_recurse;
        std::string git_progress;
        std::string timeout, download_extract_timestamp;
        std::vector<std::string> http_header;

        parser.value("LOG_DOWNLOAD", log_download);
        parser.value("LOG_CONFIGURE", log_configure);
        parser.value("LOG_BUILD", log_build);
        parser.value("LOG_INSTALL", log_install);
        parser.value("LOG_UPDATE", log_update);
        parser.value("LOG_PATCH", log_patch);
        parser.value("LOG_TEST", log_test);
        parser.value("LOG_MERGED_STDOUTERR", log_merged_stdouterr);
        parser.value("LOG_OUTPUT_ON_FAILURE", log_output_on_failure);
        parser.flag("USES_TERMINAL_DOWNLOAD", uses_terminal_download);
        parser.flag("USES_TERMINAL_CONFIGURE", uses_terminal_configure);
        parser.flag("USES_TERMINAL_BUILD", uses_terminal_build);
        parser.flag("USES_TERMINAL_INSTALL", uses_terminal_install);
        parser.list("GIT_CONFIG", git_config);
        parser.value("GIT_REMOTE_NAME", git_remote_name);
        parser.value("GIT_SUBMODULES", git_submodules);
        parser.value("GIT_SUBMODULES_RECURSE", git_submodules_recurse);
        parser.value("GIT_PROGRESS", git_progress);
        parser.value("TIMEOUT", timeout);
        parser.value("DOWNLOAD_EXTRACT_TIMESTAMP", download_extract_timestamp);
        parser.list("HTTPHEADER", http_header);
        parser.list("HTTP_HEADER", http_header);
        std::string cmake_generator;
        parser.value("CMAKE_GENERATOR", cmake_generator);

        PARSE_OR_RETURN(parser, interp, args);

        git_shallow = !git_shallow_str.empty() && !Interpreter::is_falsy(git_shallow_str);
        build_in_source = !build_in_source_str.empty() && !Interpreter::is_falsy(build_in_source_str);
        exclude_from_all = !exclude_from_all_str.empty() && !Interpreter::is_falsy(exclude_from_all_str);
        build_always = !build_always_str.empty() && !Interpreter::is_falsy(build_always_str);

        // Warn about ignored options that were actually specified
        {
            std::vector<std::string> ignored_used;
            auto check_str = [&](const std::string& val, const char* name) {
                if (!val.empty()) ignored_used.push_back(name);
            };
            auto check_bool = [&](bool val, const char* name) {
                if (val) ignored_used.push_back(name);
            };
            auto check_list = [&](const std::vector<std::string>& val, const char* name) {
                if (!val.empty()) ignored_used.push_back(name);
            };

            check_str(log_download, "LOG_DOWNLOAD");
            check_str(log_configure, "LOG_CONFIGURE");
            check_str(log_build, "LOG_BUILD");
            check_str(log_install, "LOG_INSTALL");
            check_str(log_update, "LOG_UPDATE");
            check_str(log_patch, "LOG_PATCH");
            check_str(log_test, "LOG_TEST");
            check_str(log_merged_stdouterr, "LOG_MERGED_STDOUTERR");
            check_str(log_output_on_failure, "LOG_OUTPUT_ON_FAILURE");
            check_bool(uses_terminal_download, "USES_TERMINAL_DOWNLOAD");
            check_bool(uses_terminal_configure, "USES_TERMINAL_CONFIGURE");
            check_bool(uses_terminal_build, "USES_TERMINAL_BUILD");
            check_bool(uses_terminal_install, "USES_TERMINAL_INSTALL");
            check_list(git_config, "GIT_CONFIG");
            check_str(git_remote_name, "GIT_REMOTE_NAME");
            check_str(git_submodules, "GIT_SUBMODULES");
            check_str(git_submodules_recurse, "GIT_SUBMODULES_RECURSE");
            check_str(git_progress, "GIT_PROGRESS");
            check_str(timeout, "TIMEOUT");
            check_str(download_extract_timestamp, "DOWNLOAD_EXTRACT_TIMESTAMP");
            check_list(http_header, "HTTP_HEADER");
            check_str(cmake_generator, "CMAKE_GENERATOR");

            if (!ignored_used.empty()) {
                std::string opts;
                for (size_t i = 0; i < ignored_used.size(); ++i) {
                    if (i > 0) opts += ", ";
                    opts += ignored_used[i];
                }
                interp.print_message("WARNING", "ExternalProject_Add(" + name
                                                    + "): The following options are not yet implemented and will be ignored: " + opts);
            }
        }

        if (name.empty()) {
            interp.set_fatal_error("ExternalProject_Add requires a name");
            return;
        }

        // === Resolve directories ===
        // Use shared source dir under build root (KILN_BUILD_ROOT/_deps/<name>-src)
        // so sources are shared across configs
        std::string build_root = interp.get_variable("KILN_BUILD_ROOT");
        std::string bin_dir = interp.get_variable("CMAKE_CURRENT_BINARY_DIR");

        // Fallback if KILN_BUILD_ROOT not set (shouldn't happen in normal builds)
        if (build_root.empty()) { build_root = interp.get_variable("CMAKE_BINARY_DIR"); }

        // Resolve prefix (config-specific)
        if (prefix.empty()) { prefix = bin_dir + "/" + name + "-prefix"; }
        if (!std::filesystem::path(prefix).is_absolute()) { prefix = bin_dir + "/" + prefix; }

        // Source dir: shared across configs
        if (source_dir.empty()) {
            source_dir = build_root + "/_deps/" + name + "-src";
        } else if (!std::filesystem::path(source_dir).is_absolute()) {
            source_dir = bin_dir + "/" + source_dir;
        }

        // Binary dir: config-specific EP build artifacts
        if (binary_dir.empty()) {
            if (build_in_source) {
                binary_dir = source_dir;
            } else {
                binary_dir = bin_dir + "/_ep/" + name;
            }
        } else if (!std::filesystem::path(binary_dir).is_absolute()) {
            binary_dir = bin_dir + "/" + binary_dir;
        }

        // Install dir: config-specific
        if (install_dir.empty())
            install_dir = prefix;
        else if (!std::filesystem::path(install_dir).is_absolute())
            install_dir = bin_dir + "/" + install_dir;

        if (tmp_dir.empty()) tmp_dir = prefix + "/tmp";
        if (stamp_dir.empty()) stamp_dir = prefix + "/src/" + name + "-stamp";
        if (download_dir.empty()) download_dir = build_root + "/_deps"; // Shared download dir

        // Apply SOURCE_SUBDIR
        std::string effective_source_dir = source_dir;
        if (!source_subdir.empty()) { effective_source_dir = source_dir + "/" + source_subdir; }

        // Token replacements for step commands
        std::vector<std::pair<std::string, std::string>> tokens = {
            {"<SOURCE_DIR>", source_dir}, {"<SOURCE_SUBDIR>", effective_source_dir},
            {"<BINARY_DIR>", binary_dir}, {"<INSTALL_DIR>", install_dir},
            {"<TMP_DIR>", tmp_dir},
        };

        // === Create ExternalProjectTarget ===
        std::string src_dir_cmake = interp.get_variable("CMAKE_CURRENT_SOURCE_DIR");
        auto target = std::make_shared<ExternalProjectTarget>(name, src_dir_cmake, bin_dir);
        target->set_build_by_default(!exclude_from_all);

        // Store EP directories
        target->set_ep_source_dir(source_dir);
        target->set_ep_binary_dir(binary_dir);
        target->set_ep_install_dir(install_dir);
        target->set_ep_prefix(prefix);
        target->set_ep_tmp_dir(tmp_dir);
        target->set_ep_stamp_dir(stamp_dir);
        target->set_ep_download_dir(download_dir);
        target->set_ep_source_subdir(source_subdir);

        // Set up genex evaluator using parent interp context so $<CONFIG>, etc.
        // get resolved before being forwarded into the EP's interpreter.
        auto genex_ctx = GenexEvaluationContext::from_interpreter(interp, interp.get_targets());
        GenexEvaluator genex_eval(genex_ctx);
        auto eval_genex = [&](std::string s) -> std::string {
            if (!GenexParser::contains_genex(s)) return s;
            auto r = genex_eval.evaluate(s);
            return r ? *r : s;
        };

        // Store CMAKE_ARGS and CMAKE_CACHE_ARGS
        for (const auto& arg : cmake_args) {
            // Apply token replacements
            std::string processed = arg;
            if (!list_separator.empty()) { processed = kiln::replace_all(std::move(processed), list_separator, ";"); }
            for (const auto& [token, value] : tokens) { processed = kiln::replace_all(std::move(processed), token, value); }
            target->add_cmake_arg(eval_genex(std::move(processed)));
        }
        for (const auto& arg : cmake_cache_args) {
            std::string processed = arg;
            if (!list_separator.empty()) { processed = kiln::replace_all(std::move(processed), list_separator, ";"); }
            for (const auto& [token, value] : tokens) { processed = kiln::replace_all(std::move(processed), token, value); }
            target->add_cmake_cache_arg(eval_genex(std::move(processed)));
        }
        target->set_list_separator(list_separator);

        // Store step commands (for non-cmake EPs)
        // Apply token replacements (and genex eval) to commands
        auto apply_tokens = [&tokens, &eval_genex](std::vector<std::string>& cmd) {
            for (auto& arg : cmd) {
                for (const auto& [token, value] : tokens) { arg = kiln::replace_all(std::move(arg), token, value); }
                arg = eval_genex(std::move(arg));
            }
        };
        if (!configure_command.empty()) {
            apply_tokens(configure_command);
            target->set_configure_command(make_step_command(configure_command));
        }
        if (!build_command.empty()) {
            apply_tokens(build_command);
            target->set_build_command(make_step_command(build_command));
        }
        if (!install_command.empty()) {
            apply_tokens(install_command);
            target->set_install_command(make_step_command(install_command));
        }

        // Store build options
        target->set_build_in_source(build_in_source);
        target->set_build_always(build_always);

        // Store all directories as target properties for ExternalProject_Get_Property
        target->set_property("_EP_SOURCE_DIR", source_dir);
        target->set_property("_EP_BINARY_DIR", binary_dir);
        target->set_property("_EP_INSTALL_DIR", install_dir);
        target->set_property("_EP_PREFIX", prefix);
        target->set_property("_EP_TMP_DIR", tmp_dir);
        target->set_property("_EP_STAMP_DIR", stamp_dir);
        target->set_property("_EP_DOWNLOAD_DIR", download_dir);
        if (!source_subdir.empty()) target->set_property("_EP_SOURCE_SUBDIR", source_subdir);

        // Create install directory structure at configure time (CMake does this)
        if (!install_dir.empty()) {
            std::string libdir = interp.get_variable("CMAKE_INSTALL_LIBDIR");
            std::string includedir = interp.get_variable("CMAKE_INSTALL_INCLUDEDIR");
            std::string bindir = interp.get_variable("CMAKE_INSTALL_BINDIR");
            if (libdir.empty()) libdir = "lib";
            if (includedir.empty()) includedir = "include";
            if (bindir.empty()) bindir = "bin";
            std::filesystem::create_directories(install_dir + "/" + libdir);
            std::filesystem::create_directories(install_dir + "/" + includedir);
            std::filesystem::create_directories(install_dir + "/" + bindir);
        }

        // Add DEPENDS
        for (const auto& dep : depends) { target->add_custom_dependency(dep); }

        // === Download + Patch at CONFIGURE time ===
        // This runs now, not at build time, because:
        // 1. Source dir is shared across configs - download once
        // 2. Patch commands may be config-agnostic
        // 3. Build-time interpreter needs sources to exist

        interp.print_message("STATUS", "ExternalProject_Add: " + name);

        bool source_dir_has_content = false;
        if (std::filesystem::exists(source_dir)) {
            auto it = std::filesystem::directory_iterator(source_dir);
            source_dir_has_content = (it != std::filesystem::directory_iterator{});
        }

        if (!source_dir_has_content) {
            if (!download_command.empty()) {
                // Custom download command
                apply_tokens(download_command);
                interp.print_message("STATUS", "  Downloading " + name + " (custom command)...");
                auto result = run_steps({download_command});
                if (!result) {
                    interp.set_fatal_error("ExternalProject_Add(" + name + ") download failed: " + result.error());
                    return;
                }
            } else if (!url_list.empty()) {
                // URL download (or local file)
                std::string url = url_list[0];

                // Parse hash
                std::string hash_algo, hash_value;
                if (!url_hash.empty()) {
                    auto eq_pos = url_hash.find('=');
                    if (eq_pos != std::string::npos) {
                        hash_algo = url_hash.substr(0, eq_pos);
                        hash_value = url_hash.substr(eq_pos + 1);
                        std::transform(hash_algo.begin(), hash_algo.end(), hash_algo.begin(),
                                       [](unsigned char c) { return std::toupper(c); });
                    }
                } else if (!url_md5.empty()) {
                    hash_algo = "MD5";
                    hash_value = url_md5;
                }

                // Detect local file vs remote URL
                bool is_local = std::filesystem::exists(url);
                if (!is_local && url.starts_with("file://")) {
                    url = url.substr(7);
                    is_local = true;
                }

                std::string archive_path;
                if (is_local) {
                    archive_path = url;
                    interp.print_message("STATUS", "  Using local archive for " + name + ": " + archive_path);
                } else {
                    std::string filename = std::filesystem::path(url).filename().string();
                    auto qpos = filename.find('?');
                    if (qpos != std::string::npos) filename = filename.substr(0, qpos);
                    archive_path = download_dir + "/" + filename;

                    interp.print_message("STATUS", "  Downloading " + name + " from " + url + "...");
                    auto dl_result = download_url(url, archive_path, hash_algo, hash_value, interp.error_stream());
                    if (!dl_result) {
                        interp.set_fatal_error("ExternalProject_Add(" + name + ") download failed: " + dl_result.error());
                        return;
                    }
                }

                // Extract unless DOWNLOAD_NO_EXTRACT
                bool no_extract = !download_no_extract_str.empty() && !Interpreter::is_falsy(download_no_extract_str);
                if (!no_extract) {
                    interp.print_message("STATUS", "  Extracting " + name + "...");
                    auto ex_result = extract_archive(archive_path, source_dir);
                    if (!ex_result) {
                        interp.set_fatal_error("ExternalProject_Add(" + name + ") extraction failed: " + ex_result.error());
                        return;
                    }
                }
            } else if (!git_repository.empty()) {
                interp.print_message("STATUS", "  Cloning " + name + " from " + git_repository + "...");
                auto git_result = git_clone(git_repository, source_dir, git_tag, git_shallow);
                if (!git_result) {
                    interp.set_fatal_error("ExternalProject_Add(" + name + ") git clone failed: " + git_result.error());
                    return;
                }
            }
        } else {
            interp.print_message("STATUS", "  " + name + " source directory already exists, skipping download.");
        }

        // Flatten single top-level directory (common in archives)
        if (std::filesystem::exists(source_dir)) {
            std::vector<std::filesystem::directory_entry> dirs;
            for (const auto& e : std::filesystem::directory_iterator(source_dir)) {
                if (e.is_directory()) dirs.push_back(e);
            }
            if (dirs.size() == 1) {
                std::string top_dir = dirs[0].path().string();
                std::string temp_dir = source_dir + ".__tmp_move";
                std::filesystem::rename(top_dir, temp_dir);
                for (const auto& e : std::filesystem::directory_iterator(temp_dir)) {
                    std::filesystem::rename(e.path(), std::filesystem::path(source_dir) / e.path().filename());
                }
                std::filesystem::remove_all(temp_dir);
            }
        }

        // Patch step (only on fresh download)
        if (!patch_command.empty() && !source_dir_has_content) {
            apply_tokens(patch_command);
            interp.print_message("STATUS", "  Patching " + name + "...");

            auto patch_step = make_step_command(patch_command);
            for (const auto& cmd : patch_step.commands) {
                auto result = run_command(cmd, source_dir);
                if (result.exit_code != 0) {
                    interp.set_fatal_error("ExternalProject_Add(" + name + ") patch failed:\n" + result.output);
                    return;
                }
            }
        }

        // === Register the target ===
        // Configure/Build/Install will happen at BUILD TIME via the orchestrator task
        interp.get_targets()[name] = target;
        interp.get_current_directory_context().owned_targets.push_back(target);

        interp.print_message("STATUS", "ExternalProject_Add: " + name + " registered (will build at build time).");
    });

    interp.add_builtin("externalproject_get_property", [](Interpreter& interp, const std::vector<std::string>& args) {
        // ExternalProject_Get_Property(<name> <prop1> [<prop2>...])
        if (args.size() < 2) {
            interp.set_fatal_error("ExternalProject_Get_Property requires a target name and at least one property");
            return;
        }

        std::string name = args[0];
        auto* target = interp.find_target(name);
        if (!target) {
            interp.set_fatal_error("ExternalProject_Get_Property: target '" + name + "' not found");
            return;
        }
        for (size_t i = 1; i < args.size(); ++i) {
            std::string prop_name = args[i];
            std::string upper_prop = prop_name;
            std::transform(upper_prop.begin(), upper_prop.end(), upper_prop.begin(), [](unsigned char c) { return std::toupper(c); });

            std::string value = target->get_property("_EP_" + upper_prop);

            // Set variable with the lowercase property name (CMake convention)
            std::string var_name = prop_name;
            std::transform(var_name.begin(), var_name.end(), var_name.begin(), [](unsigned char c) { return std::tolower(c); });
            interp.set_variable(var_name, value);
        }
    });

    // Stub for ExternalProject_Add_Step (just ignore)
    interp.add_builtin("externalproject_add_step", [](Interpreter&, const std::vector<std::string>&) {
        // Silently ignore - steps are not used in our model
    });

    // Stub for ExternalProject_Add_StepTargets
    interp.add_builtin("externalproject_add_steptargets", [](Interpreter&, const std::vector<std::string>&) {
        // Silently ignore
    });

    // Stub for ExternalProject_Add_StepDependencies
    interp.add_builtin("externalproject_add_stepdependencies", [](Interpreter&, const std::vector<std::string>&) {
        // Silently ignore
    });
}

} // namespace kiln
