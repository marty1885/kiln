#include "registry.hpp"
#include "../interperter.hpp"
#include "../target.hpp"
#include "../command_parser.hpp"
#include "../genex_parser.hpp"
#include "../compile_features.hpp"
#include "../CMakeArray.hpp"
#include "../path.hpp"
#include "../language.hpp"
#include "../compiler.hpp"
#include "../toolchain.hpp"
#include <sstream>
#include <algorithm>
#include <set>

namespace kiln {

// Target names reserved by the build system. Users cannot create targets with these names
// because they collide with built-in build targets (e.g. "make all", "make test").
static constexpr std::string_view kBannedTargetNames[] = {
    "all", "test", "clean", "install", "package", "rebuild_cache", "edit_cache",
};

static bool is_banned_target_name(std::string_view name) {
    for (auto banned : kBannedTargetNames) {
        if (name == banned) return true;
    }
    return false;
}

// Returns true if the caller should abort (target name is reserved and unusable).
// Gated by CMake policy CMP0037:
//   NEW: reserved names are a hard error.
//   OLD: emit a warning and allow the target.
// "test" gets a further exemption per CMP0037: it is only reserved after
// enable_testing() / include(CTest). Even when allowed under NEW, we warn that
// shadowing the conventional `make test` target is a footgun.
static bool reject_reserved_target_name(Interpreter& interp, std::string_view command, const std::string& name) {
    if (!is_banned_target_name(name)) return false;

    // CMP0037's `test` carve-out keys on whether the *project* called
    // enable_testing(), not BUILD_TESTING — the latter may be flipped by the
    // kiln CLI without the project ever intending to define test targets.
    bool test_allowed_by_cmp0037 = (name == "test" && !interp.project_enabled_testing());
    bool policy_new = interp.get_policy(CMakePolicy::CMP0037) == PolicyState::NEW;

    if (test_allowed_by_cmp0037) {
        interp.print_warning_with_context(std::string(command)
                                          + "() target name 'test' shadows the conventional "
                                            "test target. This is allowed because enable_testing() has not been "
                                            "called, but is discouraged - calling enable_testing() later will "
                                            "collide with this target.");
        return false;
    }

    std::string msg = std::string(command) + "() target name '" + name + "' is reserved by the build system";
    if (!policy_new) {
        KILN_POLICY_OLD(CMP0037);
        interp.print_warning_with_context(msg + " (CMP0037 is set to OLD; treating as a warning)");
        return false;
    }
    interp.set_fatal_error(msg);
    return true;
}

// Warn once when a target-defining command runs without a prior project()
// call. Without project(), no language has been enabled and the build will
// later fail with a confusing "no compiler available" error. Mirrors CMake's
// "No project() command is present" diagnostic.
static void warn_if_no_project(Interpreter& interp, std::string_view command) {
    if (interp.project_called()) return;
    if (interp.no_project_warned()) return;
    interp.mark_no_project_warned();
    interp.print_warning_with_context(std::string(command)
                                      + "() called without a prior project() call. "
                                        "The top-level CMakeLists.txt should contain a call to project() "
                                        "before defining any targets, otherwise no compiler is selected and "
                                        "the build will fail with 'no compiler available'.");
}

void register_target_builtins(Interpreter& interp) {
    // Helper to configure common target properties (C/C++ standards, flags, inherited directories)
    auto configure_target = [](Interpreter& interp, const std::shared_ptr<Target>& target) {
        auto configure_lang = [&](Language lang, const std::string& lang_prefix) {
            // Set standard from CMAKE_<LANG>_STANDARD
            std::string std_var = "CMAKE_" + lang_prefix + "_STANDARD";
            std::string lang_std = interp.get_variable(std_var);
            if (!lang_std.empty()) { target->set_language_standard(lang, lang_std); }

            // Set extensions from CMAKE_<LANG>_EXTENSIONS (default: ON)
            std::string ext_var = "CMAKE_" + lang_prefix + "_EXTENSIONS";
            std::string extensions = interp.get_variable(ext_var);
            if (!extensions.empty()) {
                // CMake truthiness: anything not explicitly falsy is true
                bool enabled = !interp.is_falsy(extensions);
                target->set_language_extensions(lang, enabled);
            }
            // else: default to true (handled in get_language_extensions)

            // Set visibility preset from CMAKE_<LANG>_VISIBILITY_PRESET
            std::string vis_var = "CMAKE_" + lang_prefix + "_VISIBILITY_PRESET";
            std::string vis_preset = interp.get_variable(vis_var);
            if (!vis_preset.empty()) { target->set_property(lang_prefix + "_VISIBILITY_PRESET", vis_preset); }

            // Apply CMAKE_<LANG>_FLAGS and CMAKE_<LANG>_FLAGS_<CONFIG>
            auto get_flags = [&](const std::string& var_name) -> std::vector<std::string> {
                std::string flags = interp.get_variable(var_name);
                std::vector<std::string> flag_list;
                if (!flags.empty()) {
                    std::istringstream iss(flags);
                    std::string flag;
                    while (iss >> flag) flag_list.push_back(flag);
                }
                return flag_list;
            };

            target->add_language_flags(lang, get_flags("CMAKE_" + lang_prefix + "_FLAGS"));

            std::string build_type = interp.get_variable("CMAKE_BUILD_TYPE");
            if (!build_type.empty()) {
                std::string upper_type = kiln::to_upper(build_type);
                target->add_language_flags(lang, get_flags("CMAKE_" + lang_prefix + "_FLAGS_" + upper_type));
            }

            // Snapshot the compiler-scope vars at target-definition time so
            // that a later set(CMAKE_<LANG>_COMPILER ...) in a different
            // scope doesn't change which compiler this target uses.
            // Empty values flow through and are interpreted as "use default".
            target->capture_compiler_var("CMAKE_" + lang_prefix + "_COMPILER", interp.get_variable("CMAKE_" + lang_prefix + "_COMPILER"));
            target->capture_compiler_var("CMAKE_" + lang_prefix + "_COMPILER_TARGET",
                                         interp.get_variable("CMAKE_" + lang_prefix + "_COMPILER_TARGET"));
            target->capture_compiler_var("CMAKE_" + lang_prefix + "_COMPILER_ID",
                                         interp.get_variable("CMAKE_" + lang_prefix + "_COMPILER_ID"));
        };

        configure_lang(Language::CXX, "CXX");
        configure_lang(Language::C, "C");
        if (!interp.get_variable("CMAKE_ASM_COMPILER_LOADED").empty()) { configure_lang(Language::ASM, "ASM"); }

        // CMAKE_SYSROOT is global, not per-language.
        target->capture_compiler_var("CMAKE_SYSROOT", interp.get_variable("CMAKE_SYSROOT"));

        // Snapshot the active build configuration so get_output_path() can
        // apply <CONFIG>_POSTFIX without threading the interpreter through.
        target->set_build_type(interp.get_variable("CMAKE_BUILD_TYPE"));

        // Set VISIBILITY_INLINES_HIDDEN from CMAKE_VISIBILITY_INLINES_HIDDEN
        std::string vih = interp.get_variable("CMAKE_VISIBILITY_INLINES_HIDDEN");
        if (!vih.empty() && !interp.is_falsy(vih)) { target->set_property("VISIBILITY_INLINES_HIDDEN", "ON"); }

        // Set POSITION_INDEPENDENT_CODE from CMAKE_POSITION_INDEPENDENT_CODE
        std::string pic = interp.get_variable("CMAKE_POSITION_INDEPENDENT_CODE");
        if (!pic.empty() && !interp.is_falsy(pic)) { target->set_property("POSITION_INDEPENDENT_CODE", "ON"); }

        // Set output directory properties from CMAKE_ globals
        for (const auto& [cmake_var, prop] : {
                 std::pair{"CMAKE_RUNTIME_OUTPUT_DIRECTORY", "RUNTIME_OUTPUT_DIRECTORY"},
                 std::pair{"CMAKE_ARCHIVE_OUTPUT_DIRECTORY", "ARCHIVE_OUTPUT_DIRECTORY"},
                 std::pair{"CMAKE_LIBRARY_OUTPUT_DIRECTORY", "LIBRARY_OUTPUT_DIRECTORY"},
             }) {
            std::string val = interp.get_variable(cmake_var);
            if (!val.empty()) target->set_property(prop, val);
        }

        // Set AUTOMOC/AUTOUIC/AUTORCC from CMAKE_ globals
        for (const auto& prop : {"AUTOMOC", "AUTOUIC", "AUTORCC"}) {
            std::string val = interp.get_variable(std::string("CMAKE_") + prop);
            if (!val.empty() && !interp.is_falsy(val)) { target->set_property(prop, "ON"); }
        }

        // Note: Accumulated directory properties are applied retroactively via
        // finalize_directory_targets() to match CMake's behavior where directory-level
        // commands like add_definitions() affect all targets in the directory,
        // including those created before the command was executed.
    };

    // Helper for adding sources to a target with validation
    // Skips existence check for files that are outputs of add_custom_command or have GENERATED property
    // Source validation is deferred to build graph construction (generate_object_tasks),
    // matching CMake behavior. Custom commands may be registered after add_library/add_executable.
    auto add_sources_to_target = [](Interpreter& interp, const std::shared_ptr<Target>& target, const std::string&,
                                    const std::vector<std::string>& sources) -> bool {
        // Reject C++ module sources at interpretation time when the toolchain
        // can't emit P1689r5. Failing here yields a stack trace pointing at
        // the offending add_executable/add_library/target_sources call.
        bool has_module_iface = false;
        for (const auto& s : sources) {
            if (LanguageClassifier::from_path(s).is_module_interface) {
                has_module_iface = true;
                break;
            }
        }
        if (has_module_iface && !target->is_imported()) {
            // If no CXX compiler is configured yet, defer: the existing
            // "no compiler available" diagnostic at build-graph time covers
            // it, and project() may not have been called yet (e.g. in unit
            // tests that exercise just the parser). When a compiler IS set,
            // we hard-reject unsupported toolchains here so the user sees
            // the failure with a stack trace pointing at this call.
            const Compiler* cxx = interp.get_toolchain().get_compiler(Language::CXX);
            if (cxx && !cxx->supports_p1689()) {
                interp.set_fatal_error("target '" + target->get_name()
                                       + "' has a C++ module-interface source, but the configured "
                                         "C++ compiler ('"
                                       + cxx->binary()
                                       + "') does not support P1689r5 dependency scanning. "
                                         "kiln requires GCC >=14 (-fdeps-format=p1689r5) or Clang with clang-scan-deps "
                                         "for C++ modules.");
                return false;
            }
        }
        target->append_property("SOURCES", sources, PropertyVisibility::PRIVATE);
        return true;
    };

    interp.add_builtin("add_executable", [&](Interpreter& interp, const std::vector<std::string>& args) {
        warn_if_no_project(interp, "add_executable");
        CommandParser parser("add_executable");
        std::string name;
        bool imported = false;
        bool is_alias = false;
        bool win32 = false;
        bool macosx_bundle = false;
        bool exclude_from_all = false;
        bool imported_global = false;
        std::vector<std::string> sources;
        parser.positional(name, "target name");
        parser.flag("IMPORTED", imported);
        parser.flag("GLOBAL", imported_global);
        parser.flag("ALIAS", is_alias);
        parser.flag("WIN32", win32);
        parser.flag("MACOSX_BUNDLE", macosx_bundle);
        parser.flag("EXCLUDE_FROM_ALL", exclude_from_all);
        parser.positionals(sources, "sources");
        PARSE_OR_RETURN(parser, interp, args);

        if (reject_reserved_target_name(interp, "add_executable", name)) return;

        // Handle ALIAS targets
        if (is_alias) {
            if (sources.size() != 1) {
                interp.set_fatal_error("add_executable() ALIAS requires exactly one target name");
                return;
            }
            std::string alias_target = sources[0];

            // Validate that the aliased target exists
            std::string resolved = interp.resolve_target_alias(alias_target);
            auto& targets = interp.get_root()->targets_;
            if (targets.find(resolved) == targets.end()) {
                interp.set_fatal_error("add_executable() ALIAS target '" + alias_target + "' does not exist");
                return;
            }

            // Validate that the target is an executable
            auto target = targets[resolved];
            if (target->get_type() != TargetType::EXECUTABLE) {
                interp.set_fatal_error("add_executable() ALIAS target '" + alias_target + "' is not an executable");
                return;
            }

            // Store the alias
            interp.get_target_aliases()[name] = resolved;
            return;
        }

        std::string src_dir = interp.get_variable("CMAKE_CURRENT_SOURCE_DIR");
        std::string bin_dir = interp.get_variable("CMAKE_CURRENT_BINARY_DIR");

        auto target = std::make_shared<Target>(name, TargetType::EXECUTABLE, src_dir, bin_dir);
        if (imported) {
            target->set_imported(true);
            target->set_imported_global(imported_global);
        } else {
            configure_target(interp, target);
            if (!add_sources_to_target(interp, target, src_dir, sources)) return;
        }
        // Set WIN32_EXECUTABLE, MACOSX_BUNDLE, and EXCLUDE_FROM_ALL properties
        if (win32) { target->set_property("WIN32_EXECUTABLE", "TRUE"); }
        if (macosx_bundle) { target->set_property("MACOSX_BUNDLE", "TRUE"); }
        if (exclude_from_all) { target->set_property("EXCLUDE_FROM_ALL", "TRUE"); }
        interp.get_targets()[name] = target;
        interp.get_current_directory_context().owned_targets.push_back(target); // Track ownership for this directory
    });

    interp.add_builtin("add_library", [&](Interpreter& interp, const std::vector<std::string>& args) {
        warn_if_no_project(interp, "add_library");
        CommandParser parser("add_library");
        std::string name;
        bool shared = false, static_lib = false, module_lib = false, object_lib = false, interface_lib = false, imported = false,
             imported_global = false, is_alias = false;
        std::vector<std::string> sources;

        parser.positional(name, "target name");
        parser.flag("SHARED", shared);
        parser.flag("STATIC", static_lib);
        parser.flag("MODULE", module_lib);
        parser.flag("OBJECT", object_lib);
        parser.flag("INTERFACE", interface_lib);
        parser.flag("IMPORTED", imported);
        parser.flag("GLOBAL", imported_global);
        parser.flag("ALIAS", is_alias);
        parser.positionals(sources, "sources");
        PARSE_OR_RETURN(parser, interp, args);

        if (reject_reserved_target_name(interp, "add_library", name)) return;

        // Handle ALIAS targets
        if (is_alias) {
            if (sources.size() != 1) {
                interp.set_fatal_error("add_library() ALIAS requires exactly one target name");
                return;
            }
            std::string alias_target = sources[0];

            // Validate that the aliased target exists
            std::string resolved = interp.resolve_target_alias(alias_target);
            auto& targets = interp.get_root()->targets_;
            if (targets.find(resolved) == targets.end()) {
                interp.set_fatal_error("add_library() ALIAS target '" + alias_target + "' does not exist");
                return;
            }

            // Validate that the target is a library
            auto target = targets[resolved];
            auto type = target->get_type();
            if (type != TargetType::SHARED_LIBRARY && type != TargetType::STATIC_LIBRARY && type != TargetType::OBJECT_LIBRARY
                && type != TargetType::INTERFACE_LIBRARY) {
                interp.set_fatal_error("add_library() ALIAS target '" + alias_target + "' is not a library");
                return;
            }

            // Store the alias
            interp.get_target_aliases()[name] = resolved;
            return;
        }

        int type_count = (shared ? 1 : 0) + (static_lib ? 1 : 0) + (module_lib ? 1 : 0) + (object_lib ? 1 : 0) + (interface_lib ? 1 : 0);
        if (type_count > 1) {
            interp.set_fatal_error("add_library() called with multiple conflicting types");
            return;
        }

        TargetType type;
        if (shared || module_lib)
            type = TargetType::SHARED_LIBRARY;
        else if (static_lib)
            type = TargetType::STATIC_LIBRARY;
        else if (object_lib)
            type = TargetType::OBJECT_LIBRARY;
        else if (interface_lib)
            type = TargetType::INTERFACE_LIBRARY;
        else {
            // No explicit type — check BUILD_SHARED_LIBS (CMake behavior)
            auto bsl = interp.get_variable("BUILD_SHARED_LIBS");
            type = Interpreter::is_truthy(bsl) ? TargetType::SHARED_LIBRARY : TargetType::STATIC_LIBRARY;
        }

        std::string src_dir = interp.get_variable("CMAKE_CURRENT_SOURCE_DIR");
        std::string bin_dir = interp.get_variable("CMAKE_CURRENT_BINARY_DIR");

        auto target = std::make_shared<Target>(name, type, src_dir, bin_dir);
        if (imported) {
            target->set_imported(true);
            target->set_imported_global(imported_global);
        } else {
            configure_target(interp, target);
            if (!add_sources_to_target(interp, target, src_dir, sources)) return;
        }
        interp.get_targets()[name] = target;
        interp.get_current_directory_context().owned_targets.push_back(target); // Track ownership for this directory
    });

    // add_custom_command - two forms:
    // 1. OUTPUT form: add_custom_command(OUTPUT outputs... COMMAND cmd... [DEPENDS deps...] [WORKING_DIRECTORY dir] [COMMENT comment])
    //    Also: add_custom_command(APPEND OUTPUT outputs... COMMAND cmd... [DEPENDS deps...])
    // 2. TARGET form: add_custom_command(TARGET target PRE_BUILD|PRE_LINK|POST_BUILD COMMAND cmd...)
    interp.add_builtin("add_custom_command", [&](Interpreter& interp, const std::vector<std::string>& args) {
        if (args.empty()) {
            interp.set_fatal_error("add_custom_command() requires arguments");
            return;
        }

        std::string src_dir = interp.get_variable("CMAKE_CURRENT_SOURCE_DIR");
        std::string bin_dir = interp.get_variable("CMAKE_CURRENT_BINARY_DIR");

        // Handle APPEND as first keyword - it's the OUTPUT form with append=true
        // CMake allows: add_custom_command(APPEND OUTPUT ... COMMAND ... DEPENDS ...)
        bool append_first = (args[0] == "APPEND");
        const std::vector<std::string>* parse_args = &args;
        std::vector<std::string> args_without_append;
        if (append_first) {
            args_without_append.assign(args.begin() + 1, args.end());
            parse_args = &args_without_append;
            if (parse_args->empty() || (*parse_args)[0] != "OUTPUT") {
                interp.set_fatal_error("add_custom_command(APPEND) requires OUTPUT keyword");
                return;
            }
        }

        // Detect which form based on first keyword.
        // TARGET form requires TARGET as first keyword.
        // OUTPUT form accepts keywords in any order (DEPENDS, OUTPUT, COMMAND, etc.)
        if ((*parse_args)[0] != "TARGET") {
            // OUTPUT form - generates files
            // Strip deprecated ARGS keyword - in CMake it's a no-op that means
            // "the following are arguments to the preceding COMMAND". We remove it
            // so its arguments stay part of the current COMMAND's multi_list entry.
            std::vector<std::string> filtered_args;
            filtered_args.reserve(parse_args->size());
            for (const auto& a : *parse_args) {
                if (a != "ARGS") filtered_args.push_back(a);
            }

            CommandParser parser("add_custom_command");
            std::vector<std::string> outputs;
            std::vector<std::vector<std::string>> commands;
            std::vector<std::string> depends;
            std::vector<std::string> byproducts;
            std::string working_dir;
            std::vector<std::string> comment_parts;
            std::string main_dependency;
            std::string depfile;
            std::string job_pool;
            std::vector<std::string> implicit_depends;
            bool append_flag = false;
            bool verbatim = false;
            bool command_expand_lists = false;
            bool uses_terminal = false;
            bool codegen = false;
            bool depends_explicit_only = false;
            bool job_server_aware = false;

            parser.list("OUTPUT", outputs);
            parser.multi_list("COMMAND", commands);
            parser.list("DEPENDS", depends);
            parser.list("BYPRODUCTS", byproducts); // Parsed but treated same as OUTPUT
            parser.value("WORKING_DIRECTORY", working_dir);
            parser.list("COMMENT", comment_parts);
            parser.value("MAIN_DEPENDENCY", main_dependency);
            parser.value("DEPFILE", depfile);
            parser.value("JOB_POOL", job_pool);
            parser.list("IMPLICIT_DEPENDS", implicit_depends);
            parser.flag("APPEND", append_flag);
            parser.flag("VERBATIM", verbatim); // Ignored (we always quote properly)
            parser.flag("COMMAND_EXPAND_LISTS", command_expand_lists);
            parser.flag("USES_TERMINAL", uses_terminal);
            parser.flag("CODEGEN", codegen);
            parser.flag("DEPENDS_EXPLICIT_ONLY", depends_explicit_only);
            parser.flag("JOB_SERVER_AWARE", job_server_aware);
            PARSE_OR_RETURN(parser, interp, filtered_args);

            // APPEND can be first keyword or a flag within the command
            bool append = append_first || append_flag;

            if (command_expand_lists) {
                for (auto& cmd_args : commands) {
                    std::vector<std::string> expanded_args;
                    for (const auto& arg : cmd_args) {
                        // Use CMakeArray to split by semicolons while respecting generator expressions
                        CMakeArray list(arg);
                        auto parts = list.to_vector();
                        expanded_args.insert(expanded_args.end(), parts.begin(), parts.end());
                    }
                    cmd_args = std::move(expanded_args);
                }
            }
            // CMake drops empty arguments from COMMAND (e.g. Qt's tool-wrapper
            // path is empty on non-Windows hosts and prefixes the real exe).
            for (auto& cmd_args : commands) { std::erase(cmd_args, std::string()); }

            // MAIN_DEPENDENCY is treated as an additional dependency
            if (!main_dependency.empty()) { depends.push_back(main_dependency); }

            if (outputs.empty()) {
                interp.set_fatal_error("add_custom_command(OUTPUT) requires at least one output file");
                return;
            }

            if (commands.empty() && !append) {
                interp.set_fatal_error("add_custom_command(OUTPUT) requires at least one COMMAND");
                return;
            }

            // Normalize output paths (relative to binary dir)
            std::vector<std::string> normalized_outputs;
            for (const auto& out : outputs) { normalized_outputs.push_back(Path::make_absolute_and_normal(bin_dir, out)); }

            // Also treat BYPRODUCTS as outputs
            for (const auto& out : byproducts) { normalized_outputs.push_back(Path::make_absolute_and_normal(bin_dir, out)); }

            // Default working directory to binary dir
            if (working_dir.empty()) {
                working_dir = bin_dir;
            } else if (Path(working_dir).is_relative()) {
                working_dir = Path(Path::join(bin_dir, working_dir)).lexically_normal().str();
            }

            auto& rules = interp.get_custom_command_rules();

            if (append) {
                // APPEND: add commands to existing rule
                // Find existing rule for any of our outputs
                std::shared_ptr<CustomCommandRule> existing_rule;
                for (const auto& out : normalized_outputs) {
                    auto it = rules.find(out);
                    if (it != rules.end()) {
                        existing_rule = it->second;
                        break;
                    }
                }

                if (!existing_rule) {
                    interp.set_fatal_error("add_custom_command(APPEND) requires an existing rule for the output");
                    return;
                }

                // Append commands
                for (const auto& cmd : commands) { existing_rule->commands.push_back(cmd); }
                // Append depends
                for (const auto& dep : depends) { existing_rule->depends.push_back(dep); }
            } else {
                // Create new rule
                auto rule = std::make_shared<CustomCommandRule>();
                rule->outputs = normalized_outputs;
                rule->commands = commands;
                rule->depends = depends;
                rule->working_dir = working_dir;
                // Join comment parts with spaces (CMake collects all args after COMMENT until next keyword)
                for (size_t i = 0; i < comment_parts.size(); ++i) {
                    if (i > 0) rule->comment += " ";
                    rule->comment += comment_parts[i];
                }
                rule->source_dir = src_dir;
                rule->binary_dir = bin_dir;

                // Register rule for each output and mark as GENERATED
                auto& source_props = interp.get_source_properties();
                for (const auto& out : normalized_outputs) {
                    rules[out] = rule;
                    source_props[out]["GENERATED"] = "TRUE";
                }
            }
        } else {
            // TARGET form - build events
            // Syntax: add_custom_command(TARGET <target>
            //           [PRE_BUILD | PRE_LINK | POST_BUILD]
            //           COMMAND command1 [ARGS] [args1...]
            //           [COMMAND command2 ...] [BYPRODUCTS files...]
            //           [COMMENT comment] [WORKING_DIRECTORY dir]
            //           [VERBATIM] [USES_TERMINAL] [COMMAND_EXPAND_LISTS])
            // Timing is optional and defaults to POST_BUILD (matching CMake behavior).
            if (parse_args->size() < 2) {
                interp.set_fatal_error("add_custom_command(TARGET) requires a target name");
                return;
            }

            std::string target_name = (*parse_args)[1];

            // Check if args[2] is a timing keyword; if not, default to POST_BUILD
            std::string timing = "POST_BUILD";
            size_t remaining_start = 2;
            if (parse_args->size() > 2) {
                const std::string& candidate = (*parse_args)[2];
                if (candidate == "PRE_BUILD" || candidate == "PRE_LINK" || candidate == "POST_BUILD") {
                    timing = candidate;
                    remaining_start = 3;
                }
            }

            // Find the target
            auto* target = interp.find_target(target_name);
            if (!target) {
                interp.set_fatal_error("add_custom_command(TARGET) target '" + target_name + "' does not exist");
                return;
            }

            // Parse remaining arguments
            // Strip deprecated ARGS keyword (see OUTPUT form above)
            std::vector<std::string> remaining_args;
            for (size_t ri = remaining_start; ri < parse_args->size(); ++ri) {
                if ((*parse_args)[ri] != "ARGS") remaining_args.push_back((*parse_args)[ri]);
            }

            CommandParser parser("add_custom_command");
            std::vector<std::vector<std::string>> commands;
            std::vector<std::string> byproducts;
            std::vector<std::string> ignored_depends;
            std::string working_dir;
            std::vector<std::string> comment_parts;
            bool verbatim = false;
            bool uses_terminal = false;
            bool command_expand_lists = false;

            parser.multi_list("COMMAND", commands);
            parser.list("BYPRODUCTS", byproducts);
            // CMake's TARGET form ignores DEPENDS (build events run on every
            // build of the target by definition), but real CMake accepts it
            // silently — some projects pass it. Accept and discard so we
            // don't reject otherwise-valid CMakeLists.
            parser.list("DEPENDS", ignored_depends);
            parser.value("WORKING_DIRECTORY", working_dir);
            parser.list("COMMENT", comment_parts);
            parser.flag("VERBATIM", verbatim);
            parser.flag("USES_TERMINAL", uses_terminal);
            parser.flag("COMMAND_EXPAND_LISTS", command_expand_lists);

            PARSE_OR_RETURN(parser, interp, remaining_args);

            if (commands.empty()) {
                interp.set_fatal_error("add_custom_command(TARGET) requires at least one COMMAND");
                return;
            }

            if (command_expand_lists) {
                for (auto& cmd_args : commands) {
                    std::vector<std::string> expanded_args;
                    for (const auto& arg : cmd_args) {
                        CMakeArray list(arg);
                        auto parts = list.to_vector();
                        expanded_args.insert(expanded_args.end(), parts.begin(), parts.end());
                    }
                    cmd_args = std::move(expanded_args);
                }
            }
            for (auto& cmd_args : commands) { std::erase(cmd_args, std::string()); }

            // Default working directory
            if (working_dir.empty()) {
                working_dir = bin_dir;
            } else if (Path(working_dir).is_relative()) {
                working_dir = Path(Path::join(bin_dir, working_dir)).lexically_normal().str();
            }

            // Join comment parts with spaces (CMake collects all args after COMMENT until next keyword)
            std::string comment;
            for (size_t i = 0; i < comment_parts.size(); ++i) {
                if (i > 0) comment += " ";
                comment += comment_parts[i];
            }

            // Add commands to target
            for (const auto& cmd_args : commands) {
                CustomCommand cmd;
                cmd.command = cmd_args;
                cmd.comment = comment;
                cmd.working_dir = working_dir;

                if (timing == "PRE_BUILD") {
                    target->add_pre_build_command(std::move(cmd));
                } else if (timing == "PRE_LINK") {
                    target->add_pre_link_command(std::move(cmd));
                } else { // POST_BUILD
                    target->add_post_build_command(std::move(cmd));
                }
            }
        }
    });

    interp.add_builtin("add_custom_target", [&](Interpreter& interp, const std::vector<std::string>& args) {
        // CMake allows an implicit command after the target name (and optional ALL):
        //   add_custom_target(name [ALL] cmd args... [DEPENDS ...])
        // We normalize this to the explicit COMMAND form before parsing.
        //
        // Undocumented CMake behaviour: implicit command args can be mixed with
        // explicit COMMAND keywords. The implicit args become the first command
        // and each COMMAND keyword adds another. MariaDB's build relies on this:
        //   add_custom_target(coverage_report
        //     lcov --capture ...
        //     COMMAND genhtml ...)
        static const std::set<std::string> keywords = {"COMMAND",    "DEPENDS",  "WORKING_DIRECTORY",    "COMMENT",
                                                       "SOURCES",    "VERBATIM", "COMMAND_EXPAND_LISTS", "ALL",
                                                       "BYPRODUCTS", "JOB_POOL", "USES_TERMINAL"};

        std::vector<std::string> normalized_args;
        if (!args.empty()) {
            size_t i = 0;
            // First arg is target name
            normalized_args.push_back(args[i++]);
            // Skip ALL if present
            if (i < args.size() && args[i] == "ALL") { normalized_args.push_back(args[i++]); }
            // Collect non-keyword arguments as implicit command
            std::vector<std::string> implicit_cmd;
            while (i < args.size() && keywords.find(args[i]) == keywords.end()) { implicit_cmd.push_back(args[i++]); }
            if (!implicit_cmd.empty()) {
                // Warn if mixing implicit args with explicit COMMAND (undocumented CMake behaviour)
                bool has_explicit_command = false;
                for (size_t j = i; j < args.size(); ++j) {
                    if (args[j] == "COMMAND") {
                        has_explicit_command = true;
                        break;
                    }
                }
                if (has_explicit_command) {
                    interp.print_message("WARNING", "add_custom_target(" + args[0]
                                                        + "): mixing implicit command arguments "
                                                          "with explicit COMMAND keywords is undocumented CMake behaviour. "
                                                          "Prefer using COMMAND for all commands.");
                }
                normalized_args.push_back("COMMAND");
                normalized_args.insert(normalized_args.end(), implicit_cmd.begin(), implicit_cmd.end());
            }
            // Copy remaining arguments
            while (i < args.size()) { normalized_args.push_back(args[i++]); }
        }

        CommandParser parser("add_custom_target");
        std::string name;
        bool all = false;
        bool command_expand_lists = false;
        std::vector<std::vector<std::string>> commands;
        std::vector<std::string> depends;
        std::vector<std::string> byproducts;
        std::string working_dir;
        std::vector<std::string> comment_parts;
        std::vector<std::string> sources;
        bool verbatim = false;
        bool uses_terminal = false;
        std::string job_pool;

        parser.positional(name, "target name");
        parser.flag("ALL", all);
        parser.flag("COMMAND_EXPAND_LISTS", command_expand_lists);
        parser.multi_list("COMMAND", commands);
        parser.list("DEPENDS", depends);
        parser.list("BYPRODUCTS", byproducts);
        parser.value("WORKING_DIRECTORY", working_dir);
        parser.list("COMMENT", comment_parts);
        parser.list("SOURCES", sources);
        parser.flag("VERBATIM", verbatim);
        parser.flag("USES_TERMINAL", uses_terminal);
        parser.value("JOB_POOL", job_pool);
        PARSE_OR_RETURN(parser, interp, normalized_args);

        if (reject_reserved_target_name(interp, "add_custom_target", name)) return;

        std::string src_dir = interp.get_variable("CMAKE_CURRENT_SOURCE_DIR");
        std::string bin_dir = interp.get_variable("CMAKE_CURRENT_BINARY_DIR");

        auto target = std::make_shared<CustomTarget>(name, src_dir, bin_dir);
        target->set_build_by_default(all);

        // Join comment parts with spaces (CMake collects all args after COMMENT until next keyword)
        std::string comment;
        for (size_t i = 0; i < comment_parts.size(); ++i) {
            if (i > 0) comment += " ";
            comment += comment_parts[i];
        }
        if (!comment.empty()) { target->set_property("COMMENT", comment); }

        // Expand lists in command arguments if COMMAND_EXPAND_LISTS is set
        if (command_expand_lists) {
            for (auto& cmd_args : commands) {
                std::vector<std::string> expanded_args;
                for (const auto& arg : cmd_args) {
                    // Use CMakeArray to split by semicolons while respecting generator expressions
                    CMakeArray list(arg);
                    auto parts = list.to_vector();
                    expanded_args.insert(expanded_args.end(), parts.begin(), parts.end());
                }
                cmd_args = std::move(expanded_args);
            }
        }
        for (auto& cmd_args : commands) { std::erase(cmd_args, std::string()); }

        for (const auto& cmd_args : commands) {
            CustomCommand cmd;
            cmd.command = cmd_args;
            cmd.comment = comment;
            cmd.working_dir = working_dir;
            target->add_custom_command(std::move(cmd));
        }

        for (const auto& dep : depends) {
            // DEPENDS items may contain semicolon-separated lists (e.g. from "${LIST_VAR}")
            for (auto item : CMakeArrayIterator(dep)) { target->add_custom_dependency(std::string(item)); }
        }

        auto& source_props = interp.get_source_properties();
        for (const auto& bp : byproducts) {
            std::string abs = Path::make_absolute_and_normal(bin_dir, bp);
            target->add_byproduct(abs);
            // Match add_custom_command: BYPRODUCTS are GENERATED. Lets later
            // target_sources() / add_executable() consumers know the file is
            // produced by this build and must order after its producer.
            source_props[abs]["GENERATED"] = "TRUE";
        }

        if (!sources.empty()) { target->append_property("SOURCES", sources, PropertyVisibility::PRIVATE); }

        interp.get_targets()[name] = target;
        interp.get_current_directory_context().owned_targets.push_back(target); // Track ownership for this directory
    });

    auto get_target_from_name = [](Interpreter& interp, const std::string& name, const std::string& cmd_name) -> Target* {
        auto* target = interp.find_target(name);
        if (!target) { interp.set_fatal_error(cmd_name + "() called on unknown target '" + name + "'"); }
        return target;
    };

    // Generic handler generator for target_* commands
    auto make_target_command = [get_target_from_name](std::string cmd_name, std::string prop_name) {
        return [get_target_from_name, cmd_name, prop_name](Interpreter& interp, const std::vector<std::string>& args) {
            CommandParser parser(cmd_name);
            std::string name;
            bool before = false;
            std::vector<std::string> pub, priv, inter;
            parser.positional(name, "target name");
            parser.flag("BEFORE", before);
            parser.list("PUBLIC", pub);
            parser.list("PRIVATE", priv);
            parser.list("INTERFACE", inter);
            PARSE_OR_RETURN(parser, interp, args);

            auto target = get_target_from_name(interp, name, cmd_name);
            if (!target) return;

            // EARLY VALIDATION (Layer 1) - validate genex support before storing.
            // CMake splits multi-line genex into separate arguments joined by semicolons,
            // so we must validate the joined string (individual items may be fragments).
            auto validate_values = [&](const std::vector<std::string>& values, const char* visibility) -> bool {
                std::string joined;
                for (size_t i = 0; i < values.size(); ++i) {
                    if (i > 0) joined += ';';
                    joined += values[i];
                }
                auto validation = GenexParser::validate_genex_support(joined);
                if (!validation) {
                    interp.set_fatal_error(cmd_name + ": " + validation.error() + " in " + visibility + " scope");
                    return false;
                }
                return true;
            };

            if (!pub.empty() && !validate_values(pub, "PUBLIC")) return;
            if (!priv.empty() && !validate_values(priv, "PRIVATE")) return;
            if (!inter.empty() && !validate_values(inter, "INTERFACE")) return;

            // Store validated values (prepend if BEFORE, otherwise append)
            if (before) {
                if (!pub.empty()) target->prepend_property(prop_name, pub, PropertyVisibility::PUBLIC);
                if (!priv.empty()) target->prepend_property(prop_name, priv, PropertyVisibility::PRIVATE);
                if (!inter.empty()) target->prepend_property(prop_name, inter, PropertyVisibility::INTERFACE);
            } else {
                if (!pub.empty()) target->append_property(prop_name, pub, PropertyVisibility::PUBLIC);
                if (!priv.empty()) target->append_property(prop_name, priv, PropertyVisibility::PRIVATE);
                if (!inter.empty()) target->append_property(prop_name, inter, PropertyVisibility::INTERFACE);
            }
        };
    };

    // target_include_directories - with SYSTEM and BEFORE/AFTER support
    interp.add_builtin("target_include_directories", [get_target_from_name](Interpreter& interp, const std::vector<std::string>& args) {
        CommandParser parser("target_include_directories");
        std::string name;
        bool is_system = false;
        bool before = false;
        bool after = false; // Consume but ignore (AFTER is default)
        std::vector<std::string> pub, priv, inter;

        parser.positional(name, "target name");
        parser.flag("SYSTEM", is_system);
        parser.flag("BEFORE", before);
        parser.flag("AFTER", after);
        parser.list("PUBLIC", pub);
        parser.list("PRIVATE", priv);
        parser.list("INTERFACE", inter);
        PARSE_OR_RETURN(parser, interp, args);

        auto target = get_target_from_name(interp, name, "target_include_directories");
        if (!target) return;

        // EARLY VALIDATION (Layer 1) - validate genex support before storing.
        // Validate the joined string since multi-line genex become fragments across items.
        auto validate_values = [&](const std::vector<std::string>& values, const char* visibility) -> bool {
            std::string joined;
            for (size_t i = 0; i < values.size(); ++i) {
                if (i > 0) joined += ';';
                joined += values[i];
            }
            auto validation = GenexParser::validate_genex_support(joined);
            if (!validation) {
                interp.set_fatal_error("target_include_directories: " + validation.error() + " in " + visibility + " scope");
                return false;
            }
            return true;
        };

        if (!pub.empty() && !validate_values(pub, "PUBLIC")) return;
        if (!priv.empty() && !validate_values(priv, "PRIVATE")) return;
        if (!inter.empty() && !validate_values(inter, "INTERFACE")) return;

        // Choose property name based on SYSTEM flag
        std::string prop_name = is_system ? "SYSTEM_INCLUDE_DIRECTORIES" : "INCLUDE_DIRECTORIES";

        // Store with prepend/append based on BEFORE flag
        if (before) {
            if (!pub.empty()) target->prepend_property(prop_name, pub, PropertyVisibility::PUBLIC);
            if (!priv.empty()) target->prepend_property(prop_name, priv, PropertyVisibility::PRIVATE);
            if (!inter.empty()) target->prepend_property(prop_name, inter, PropertyVisibility::INTERFACE);
        } else {
            if (!pub.empty()) target->append_property(prop_name, pub, PropertyVisibility::PUBLIC);
            if (!priv.empty()) target->append_property(prop_name, priv, PropertyVisibility::PRIVATE);
            if (!inter.empty()) target->append_property(prop_name, inter, PropertyVisibility::INTERFACE);
        }
    });

    interp.add_builtin("target_compile_definitions", make_target_command("target_compile_definitions", "COMPILE_DEFINITIONS"));
    interp.add_builtin("target_compile_options", make_target_command("target_compile_options", "COMPILE_OPTIONS"));
    interp.add_builtin("target_precompile_headers", [get_target_from_name](Interpreter& interp, const std::vector<std::string>& args) {
        if (args.size() < 3) {
            interp.set_fatal_error("target_precompile_headers() requires at least 3 arguments");
            return;
        }

        auto target = get_target_from_name(interp, args[0], "target_precompile_headers");
        if (!target) return;

        // REUSE_FROM mode: target_precompile_headers(<target> REUSE_FROM <provider>)
        // Just stores the property; validation happens at generate time (order-independent).
        if (args[1] == "REUSE_FROM") {
            if (args.size() != 3) {
                interp.set_fatal_error("target_precompile_headers(REUSE_FROM) expects exactly one provider target name");
                return;
            }
            target->set_property("PRECOMPILE_HEADERS_REUSE_FROM", args[2]);
            return;
        }

        // Standard mode: parse BEFORE/PUBLIC/PRIVATE/INTERFACE
        CommandParser parser("target_precompile_headers");
        std::string name;
        bool before = false;
        std::vector<std::string> pub, priv, inter;
        parser.positional(name, "target name");
        parser.flag("BEFORE", before);
        parser.list("PUBLIC", pub);
        parser.list("PRIVATE", priv);
        parser.list("INTERFACE", inter);
        PARSE_OR_RETURN(parser, interp, args);

        auto validate_values = [&](const std::vector<std::string>& values, const char* visibility) -> bool {
            std::string joined;
            for (size_t i = 0; i < values.size(); ++i) {
                if (i > 0) joined += ';';
                joined += values[i];
            }
            auto validation = GenexParser::validate_genex_support(joined);
            if (!validation) {
                interp.set_fatal_error("target_precompile_headers: " + validation.error() + " in " + visibility + " scope");
                return false;
            }
            return true;
        };

        if (!pub.empty() && !validate_values(pub, "PUBLIC")) return;
        if (!priv.empty() && !validate_values(priv, "PRIVATE")) return;
        if (!inter.empty() && !validate_values(inter, "INTERFACE")) return;

        if (before) {
            if (!pub.empty()) target->prepend_property("PRECOMPILE_HEADERS", pub, PropertyVisibility::PUBLIC);
            if (!priv.empty()) target->prepend_property("PRECOMPILE_HEADERS", priv, PropertyVisibility::PRIVATE);
            if (!inter.empty()) target->prepend_property("PRECOMPILE_HEADERS", inter, PropertyVisibility::INTERFACE);
        } else {
            if (!pub.empty()) target->append_property("PRECOMPILE_HEADERS", pub, PropertyVisibility::PUBLIC);
            if (!priv.empty()) target->append_property("PRECOMPILE_HEADERS", priv, PropertyVisibility::PRIVATE);
            if (!inter.empty()) target->append_property("PRECOMPILE_HEADERS", inter, PropertyVisibility::INTERFACE);
        }
    });

    // target_compile_features - specify compiler features required for a target
    interp.add_builtin("target_compile_features", [get_target_from_name](Interpreter& interp, const std::vector<std::string>& args) {
        CommandParser parser("target_compile_features");
        std::string name;
        std::vector<std::string> pub, priv, inter;
        parser.positional(name, "target name");
        parser.list("PUBLIC", pub);
        parser.list("PRIVATE", priv);
        parser.list("INTERFACE", inter);
        PARSE_OR_RETURN(parser, interp, args);

        auto target = get_target_from_name(interp, name, "target_compile_features");
        if (!target) return;

        // Validate features against known list
        const auto& features_db = CompileFeatures::instance();
        auto validate_features = [&](const std::vector<std::string>& features, const char* visibility) -> bool {
            for (const auto& feature : features) {
                if (GenexParser::contains_genex(feature)) continue;
                if (!features_db.is_known_feature(feature)) {
                    interp.set_fatal_error("target_compile_features: Unknown compile feature '" + feature + "' in " + visibility
                                           + " scope for target '" + name + "'");
                    return false;
                }
            }
            return true;
        };

        if (!pub.empty() && !validate_features(pub, "PUBLIC")) return;
        if (!priv.empty() && !validate_features(priv, "PRIVATE")) return;
        if (!inter.empty() && !validate_features(inter, "INTERFACE")) return;

        // Store validated features
        if (!pub.empty()) target->append_property("COMPILE_FEATURES", pub, PropertyVisibility::PUBLIC);
        if (!priv.empty()) target->append_property("COMPILE_FEATURES", priv, PropertyVisibility::PRIVATE);
        if (!inter.empty()) target->append_property("COMPILE_FEATURES", inter, PropertyVisibility::INTERFACE);
    });

    // target_link_options - similar to other target_* commands but with BEFORE support
    interp.add_builtin("target_link_options", [get_target_from_name](Interpreter& interp, const std::vector<std::string>& args) {
        CommandParser parser("target_link_options");
        std::string name;
        bool before = false;
        std::vector<std::string> pub, priv, inter;

        parser.positional(name, "target name");
        parser.flag("BEFORE", before);
        parser.list("PUBLIC", pub);
        parser.list("PRIVATE", priv);
        parser.list("INTERFACE", inter);
        PARSE_OR_RETURN(parser, interp, args);

        auto target = get_target_from_name(interp, name, "target_link_options");
        if (!target) return;

        // EARLY VALIDATION (Layer 1) - validate genex support before storing.
        // Validate the joined string since multi-line genex become fragments across items.
        auto validate_values = [&](const std::vector<std::string>& values, const char* visibility) -> bool {
            std::string joined;
            for (size_t i = 0; i < values.size(); ++i) {
                if (i > 0) joined += ';';
                joined += values[i];
            }
            auto validation = GenexParser::validate_genex_support(joined);
            if (!validation) {
                interp.set_fatal_error("target_link_options: " + validation.error() + " in " + visibility + " scope");
                return false;
            }
            return true;
        };

        if (!pub.empty() && !validate_values(pub, "PUBLIC")) return;
        if (!priv.empty() && !validate_values(priv, "PRIVATE")) return;
        if (!inter.empty() && !validate_values(inter, "INTERFACE")) return;

        // Store validated values (prepend if BEFORE is specified)
        if (before) {
            if (!pub.empty()) target->prepend_property("LINK_OPTIONS", pub, PropertyVisibility::PUBLIC);
            if (!priv.empty()) target->prepend_property("LINK_OPTIONS", priv, PropertyVisibility::PRIVATE);
            if (!inter.empty()) target->prepend_property("LINK_OPTIONS", inter, PropertyVisibility::INTERFACE);
        } else {
            if (!pub.empty()) target->append_property("LINK_OPTIONS", pub, PropertyVisibility::PUBLIC);
            if (!priv.empty()) target->append_property("LINK_OPTIONS", priv, PropertyVisibility::PRIVATE);
            if (!inter.empty()) target->append_property("LINK_OPTIONS", inter, PropertyVisibility::INTERFACE);
        }
    });

    interp.add_builtin("target_sources", [get_target_from_name](Interpreter& interp, const std::vector<std::string>& args) {
        if (args.empty()) {
            interp.set_fatal_error("target_sources() requires at least a target name");
            return;
        }

        std::string target_name = args[0];
        auto target = get_target_from_name(interp, target_name, "target_sources");
        if (!target) return;

        std::string src_dir = interp.get_variable("CMAKE_CURRENT_SOURCE_DIR");
        std::string bin_dir = interp.get_variable("CMAKE_CURRENT_BINARY_DIR");
        bool is_custom = (target->get_type() == TargetType::CUSTOM);

        // Source validation is deferred to build graph construction, matching CMake behavior.
        auto validate_sources = [&](const std::vector<std::string>&) -> bool { return true; };

        // Parse arguments manually to handle complex FILE_SET syntax
        PropertyVisibility current_visibility = PropertyVisibility::PRIVATE;
        std::vector<std::string> current_sources;

        auto flush_current_sources = [&]() {
            if (!current_sources.empty()) {
                if (is_custom && current_visibility != PropertyVisibility::PRIVATE) {
                    interp.set_fatal_error("target_sources() on custom target '" + target_name + "' only supports PRIVATE scope");
                    return false;
                }
                if (!validate_sources(current_sources)) { return false; }
                target->append_property("SOURCES", current_sources, current_visibility);
                current_sources.clear();
            }
            return true;
        };

        size_t i = 1;
        while (i < args.size()) {
            const std::string& arg = args[i];

            // Check for visibility keywords
            if (arg == "PRIVATE") {
                if (!flush_current_sources()) return;
                current_visibility = PropertyVisibility::PRIVATE;
                i++;
            } else if (arg == "PUBLIC") {
                if (!flush_current_sources()) return;
                current_visibility = PropertyVisibility::PUBLIC;
                i++;
            } else if (arg == "INTERFACE") {
                if (!flush_current_sources()) return;
                current_visibility = PropertyVisibility::INTERFACE;
                i++;
            } else if (arg == "FILE_SET") {
                // Flush any pending sources
                if (!flush_current_sources()) return;

                // Parse FILE_SET
                i++;
                if (i >= args.size()) {
                    interp.set_fatal_error("target_sources() FILE_SET requires a name");
                    return;
                }

                FileSet fs;
                fs.name = args[i++];
                fs.visibility = current_visibility;

                // Parse optional TYPE, BASE_DIRS, FILES
                while (i < args.size()) {
                    const std::string& keyword = args[i];

                    if (keyword == "TYPE") {
                        i++;
                        if (i >= args.size()) {
                            interp.set_fatal_error("target_sources() FILE_SET TYPE requires a value");
                            return;
                        }
                        fs.type = args[i++];

                        // Validate type
                        if (fs.type != "HEADERS" && fs.type != "CXX_MODULES") {
                            interp.set_fatal_error("target_sources() FILE_SET TYPE must be HEADERS or CXX_MODULES, got: " + fs.type);
                            return;
                        }

                        // Validate CXX_MODULES can't be INTERFACE unless imported
                        if (fs.type == "CXX_MODULES" && fs.visibility == PropertyVisibility::INTERFACE && !target->is_imported()) {
                            interp.set_fatal_error(
                                "target_sources() FILE_SET TYPE CXX_MODULES cannot use INTERFACE scope on non-imported targets");
                            return;
                        }

                        // Reject CXX_MODULES file sets when a CXX compiler is
                        // configured but lacks P1689r5. If no compiler is set
                        // yet (e.g. parser-only tests), defer to the build-time
                        // "no compiler available" path.
                        if (fs.type == "CXX_MODULES" && !target->is_imported()) {
                            const Compiler* cxx = interp.get_toolchain().get_compiler(Language::CXX);
                            if (cxx && !cxx->supports_p1689()) {
                                interp.set_fatal_error("target_sources() FILE_SET TYPE CXX_MODULES on '" + target->get_name()
                                                       + "' requires a compiler with P1689r5 support; configured C++ compiler '"
                                                       + cxx->binary()
                                                       + "' does not. kiln requires GCC >=14 or Clang with "
                                                         "clang-scan-deps installed.");
                                return;
                            }
                        }
                    } else if (keyword == "BASE_DIRS") {
                        i++;
                        while (i < args.size() && args[i] != "FILES" && args[i] != "FILE_SET" && args[i] != "PRIVATE" && args[i] != "PUBLIC"
                               && args[i] != "INTERFACE") {
                            fs.base_dirs.push_back(args[i++]);
                        }
                    } else if (keyword == "FILES") {
                        i++;
                        while (i < args.size() && args[i] != "TYPE" && args[i] != "BASE_DIRS" && args[i] != "FILE_SET"
                               && args[i] != "PRIVATE" && args[i] != "PUBLIC" && args[i] != "INTERFACE") {
                            fs.files.push_back(args[i++]);
                        }
                        break; // FILES is typically last in a FILE_SET block
                    } else {
                        // Hit a new visibility keyword or FILE_SET, break out
                        break;
                    }
                }

                // Default BASE_DIRS to CMAKE_CURRENT_SOURCE_DIR if not specified
                if (fs.base_dirs.empty()) { fs.base_dirs.push_back(src_dir); }

                // Validate files exist
                if (!validate_sources(fs.files)) { return; }

                // Add files to SOURCES property with appropriate visibility
                target->append_property("SOURCES", fs.files, fs.visibility);

                // Store the file set
                target->add_file_set(std::move(fs));
            } else {
                // Regular source file
                current_sources.push_back(arg);
                i++;
            }
        }

        // Flush any remaining sources
        flush_current_sources();
    });

    interp.add_builtin("target_link_libraries", [get_target_from_name](Interpreter& interp, const std::vector<std::string>& args) {
        CommandParser parser("target_link_libraries");
        std::string name;
        std::vector<std::string> pub, priv, inter, def;
        parser.positional(name, "target name");
        parser.list("PUBLIC", pub);
        parser.list("PRIVATE", priv);
        parser.list("LINK_PUBLIC", pub);   // Legacy alias for PUBLIC
        parser.list("LINK_PRIVATE", priv); // Legacy alias for PRIVATE
        parser.list("INTERFACE", inter);
        parser.positionals(def, "libraries");
        PARSE_OR_RETURN(parser, interp, args);

        auto target = get_target_from_name(interp, name, "target_link_libraries");
        if (!target) return;

        // EARLY VALIDATION (Layer 1) - validate genex support before resolving.
        // Validate the joined string since multi-line genex become fragments across items.
        auto validate_values = [&](const std::vector<std::string>& values, const char* visibility) -> bool {
            std::string joined;
            for (size_t i = 0; i < values.size(); ++i) {
                if (i > 0) joined += ';';
                joined += values[i];
            }
            auto validation = GenexParser::validate_genex_support(joined);
            if (!validation) {
                interp.set_fatal_error("target_link_libraries: " + validation.error() + " in " + visibility + " scope");
                return false;
            }
            return true;
        };

        if (!pub.empty() && !validate_values(pub, "PUBLIC")) return;
        if (!priv.empty() && !validate_values(priv, "PRIVATE")) return;
        if (!inter.empty() && !validate_values(inter, "INTERFACE")) return;
        if (!def.empty() && !validate_values(def, "default")) return;

        // Helper to resolve aliases in library list
        auto resolve_lib_aliases = [&](std::vector<std::string>& libs) {
            for (auto& lib : libs) { lib = interp.resolve_target_alias(lib); }
        };

        resolve_lib_aliases(pub);
        resolve_lib_aliases(priv);
        resolve_lib_aliases(inter);
        resolve_lib_aliases(def);

        if (!pub.empty()) target->append_property("LINK_LIBRARIES", pub, PropertyVisibility::PUBLIC);
        if (!priv.empty()) target->append_property("LINK_LIBRARIES", priv, PropertyVisibility::PRIVATE);
        if (!inter.empty()) target->append_property("LINK_LIBRARIES", inter, PropertyVisibility::INTERFACE);
        // Default (no keyword) libraries are treated as PUBLIC for non-INTERFACE targets
        // and INTERFACE for INTERFACE targets (CMake compatibility)
        if (!def.empty()) {
            if (target->get_type() == TargetType::INTERFACE_LIBRARY) {
                target->append_property("LINK_LIBRARIES", def, PropertyVisibility::INTERFACE);
            } else {
                target->append_property("LINK_LIBRARIES", def, PropertyVisibility::PUBLIC);
            }
        }
    });

    interp.add_builtin("set_target_properties", [get_target_from_name](Interpreter& interp, const std::vector<std::string>& args) {
        // CMake syntax: set_target_properties(target1 target2 ... PROPERTIES prop1 value1 prop2 value2 ...)
        // Find PROPERTIES keyword to split target names from properties
        if (args.empty()) {
            interp.set_fatal_error("set_target_properties() requires at least a target name");
            return;
        }

        auto props_it = std::find(args.begin(), args.end(), "PROPERTIES");
        if (props_it == args.end()) {
            interp.set_fatal_error("set_target_properties() requires PROPERTIES keyword");
            return;
        }

        // Extract target names (everything before PROPERTIES)
        std::vector<std::string> target_names(args.begin(), props_it);
        if (target_names.empty()) {
            // CMake silently accepts this when a variable expands to empty
            interp.print_message("WARNING",
                                 "set_target_properties() called with no targets "
                                 "(undocumented CMake behavior, accepted for compatibility)",
                                 true);
            return;
        }

        // Extract properties (everything after PROPERTIES)
        std::vector<std::string> props(props_it + 1, args.end());
        if (props.size() % 2 != 0) {
            interp.set_fatal_error("set_target_properties() PROPERTIES must be key-value pairs");
            return;
        }

        // Get all target objects and validate they exist
        std::vector<Target*> targets;
        for (const auto& name : target_names) {
            auto* target = get_target_from_name(interp, name, "set_target_properties");
            if (!target) return;
            targets.push_back(target);
        }

        // Apply properties to each target
        for (auto* target : targets) {
            // Helper to validate genex and append as INTERFACE property (uses append_property_from_string for splitting)
            auto parse_and_append_interface = [&](const std::string& base_prop_name, const std::string& value) {
                // EARLY VALIDATION (Layer 1) - validate genex support.
                // Validate the full joined value since multi-line genex become fragments.
                auto validation = GenexParser::validate_genex_support(value);
                if (!validation) {
                    interp.set_fatal_error("set_target_properties: " + validation.error() + " in INTERFACE_" + base_prop_name
                                           + " property");
                    return;
                }

                if (!value.empty()) { target->append_property_from_string(base_prop_name, value, PropertyVisibility::INTERFACE); }
            };

            for (size_t i = 0; i < props.size(); i += 2) {
                std::string prop_name = props[i];
                std::string prop_value = props[i + 1];

                // Special-case: scalar properties with dedicated setters
                if (prop_name == "IMPORTED_LOCATION" || prop_name.starts_with("IMPORTED_LOCATION_")) {
                    target->set_imported_location(prop_value);
                    target->set_property(prop_name, prop_value);
                } else if (prop_name == "OUTPUT_NAME") {
                    target->set_output_name(prop_value);
                    target->set_property(prop_name, prop_value);
                } else if (prop_name == "CXX_STANDARD") {
                    target->set_language_standard(Language::CXX, prop_value);
                    target->set_property(prop_name, prop_value);
                } else if (prop_name == "C_STANDARD") {
                    target->set_language_standard(Language::C, prop_value);
                    target->set_property(prop_name, prop_value);
                } else if (prop_name == "CXX_EXTENSIONS") {
                    target->set_language_extensions(Language::CXX, !Interpreter::is_falsy(prop_value));
                    target->set_property(prop_name, prop_value);
                } else if (prop_name == "C_EXTENSIONS") {
                    target->set_language_extensions(Language::C, !Interpreter::is_falsy(prop_value));
                    target->set_property(prop_name, prop_value);
                } else if (auto* meta = find_list_property(prop_name); meta && meta->name != "LINK_LIBRARIES" && meta->name != "SOURCES") {
                    // Known list property → append with PRIVATE visibility
                    // (LINK_LIBRARIES and SOURCES are excluded - they use dedicated commands)
                    target->append_property_from_string(prop_name, prop_value, PropertyVisibility::PRIVATE);
                } else if (auto* iface_meta = find_interface_list_property(prop_name)) {
                    // INTERFACE_ prefix of a known list property → append with INTERFACE visibility
                    parse_and_append_interface(std::string(iface_meta->name), prop_value);
                } else {
                    // Fallback: Generic scalar property
                    target->set_property(prop_name, prop_value);
                }
            }
        }
    });

    // get_target_property is a simpler form: get_target_property(<var> <target> <property>)
    interp.add_builtin("get_target_property", [get_target_from_name](Interpreter& interp, const std::vector<std::string>& args) {
        if (args.size() != 3) {
            interp.set_fatal_error("get_target_property() requires exactly 3 arguments: <variable> <target> <property>");
            return;
        }

        std::string variable = args[0];
        std::string target_name = args[1];
        std::string property_name = args[2];

        auto target = get_target_from_name(interp, target_name, "get_target_property");
        if (!target) return;

        // Helper to convert TargetType to string
        auto target_type_to_string = [](TargetType type) -> std::string {
            switch (type) {
            case TargetType::EXECUTABLE:
                return "EXECUTABLE";
            case TargetType::SHARED_LIBRARY:
                return "SHARED_LIBRARY";
            case TargetType::STATIC_LIBRARY:
                return "STATIC_LIBRARY";
            case TargetType::OBJECT_LIBRARY:
                return "OBJECT_LIBRARY";
            case TargetType::INTERFACE_LIBRARY:
                return "INTERFACE_LIBRARY";
            case TargetType::CUSTOM:
                return "UTILITY";
            default:
                return "";
            }
        };

        std::string result;

        // Handle special built-in properties
        if (property_name == "TYPE") {
            result = target_type_to_string(target->get_type());
        } else if (property_name == "NAME") {
            result = target->get_name();
        } else if (property_name == "SOURCE_DIR") {
            result = target->get_source_dir();
        } else if (property_name == "BINARY_DIR") {
            result = target->get_binary_dir();
        } else if (property_name == "IMPORTED") {
            result = target->is_imported() ? "TRUE" : "FALSE";
        } else if (property_name == "IMPORTED_LOCATION") {
            result = target->get_imported_location();
        } else if (property_name.starts_with("INTERFACE_")) {
            // INTERFACE_<PROP> returns PUBLIC + INTERFACE visibilities of the base property.
            // In CMake, PUBLIC items appear in both the target's own property and the
            // INTERFACE property (they are visible to consumers).
            std::string base_prop = property_name.substr(10); // strlen("INTERFACE_")
            auto vals = target->get_property_list(base_prop, {PropertyVisibility::PUBLIC, PropertyVisibility::INTERFACE});
            if (!vals.empty()) {
                // CMake absolutifies source paths for INTERFACE_SOURCES
                if (base_prop == "SOURCES") {
                    std::string src_dir = target->get_source_dir();
                    for (auto& v : vals) {
                        if (!v.empty() && v[0] != '/' && v[0] != '$') { v = src_dir + "/" + v; }
                    }
                }
                result = CMakeArray(vals).to_string();
            } else {
                // Fall back to generic scalar properties (custom INTERFACE_ properties
                // set via set_target_properties are stored under the full name)
                result = target->get_property(property_name);
                if (result.empty()) { result = property_name + "-NOTFOUND"; }
            }
        } else {
            // Non-INTERFACE_ properties: return PUBLIC + PRIVATE only.
            // INTERFACE visibility is only accessible via the INTERFACE_ prefix
            // (handled above). This matches CMake's get_target_property behavior.
            auto vals = target->get_property_list(property_name, {PropertyVisibility::PRIVATE, PropertyVisibility::PUBLIC});
            if (!vals.empty()) {
                result = CMakeArray(vals).to_string();
            } else {
                // Fall back to scalar properties
                result = target->get_property(property_name);
                if (result.empty()) { result = property_name + "-NOTFOUND"; }
            }
        }

        interp.set_variable(variable, result);
    });

    interp.add_builtin("include_directories", [](Interpreter& interp, const std::vector<std::string>& args) {
        if (args.empty()) { return; }

        bool before = false;
        std::string src_dir = interp.get_variable("CMAKE_CURRENT_SOURCE_DIR");
        auto& dirs = interp.get_current_directory_context().accumulated["INCLUDE_DIRECTORIES"];

        std::vector<std::string> resolved_dirs;
        for (const auto& arg : args) {
            // Handle keywords
            if (arg == "BEFORE") {
                before = true;
                continue;
            }
            if (arg == "AFTER") { continue; }  // AFTER is default
            if (arg == "SYSTEM") { continue; } // SYSTEM ignored for now

            // Generator expressions are resolved later; store as-is
            if (arg.find("$<") != std::string::npos) {
                resolved_dirs.push_back(arg);
            } else {
                resolved_dirs.push_back(Path(arg).is_absolute() ? arg : Path::join(src_dir, arg));
            }
        }

        if (before) {
            dirs.insert(dirs.begin(), resolved_dirs.begin(), resolved_dirs.end());
        } else {
            dirs.insert(dirs.end(), resolved_dirs.begin(), resolved_dirs.end());
        }
    });

    interp.add_builtin("target_link_directories", [get_target_from_name](Interpreter& interp, const std::vector<std::string>& args) {
        CommandParser parser("target_link_directories");
        std::string name;
        bool before = false;
        bool after = false;
        std::vector<std::string> pub, priv, inter;

        parser.positional(name, "target name");
        parser.flag("BEFORE", before);
        parser.flag("AFTER", after);
        parser.list("PUBLIC", pub);
        parser.list("PRIVATE", priv);
        parser.list("INTERFACE", inter);
        PARSE_OR_RETURN(parser, interp, args);

        auto target = get_target_from_name(interp, name, "target_link_directories");
        if (!target) return;

        // Resolve paths relative to source directory
        std::string src_dir = interp.get_variable("CMAKE_CURRENT_SOURCE_DIR");
        auto resolve_dirs = [&](std::vector<std::string>& dirs) {
            for (auto& dir : dirs) {
                if (Path(dir).is_relative()) { dir = Path::join(src_dir, dir); }
            }
        };

        resolve_dirs(pub);
        resolve_dirs(priv);
        resolve_dirs(inter);

        // Store with prepend/append based on BEFORE flag
        if (before) {
            if (!pub.empty()) target->prepend_property("LINK_DIRECTORIES", pub, PropertyVisibility::PUBLIC);
            if (!priv.empty()) target->prepend_property("LINK_DIRECTORIES", priv, PropertyVisibility::PRIVATE);
            if (!inter.empty()) target->prepend_property("LINK_DIRECTORIES", inter, PropertyVisibility::INTERFACE);
        } else {
            if (!pub.empty()) target->append_property("LINK_DIRECTORIES", pub, PropertyVisibility::PUBLIC);
            if (!priv.empty()) target->append_property("LINK_DIRECTORIES", priv, PropertyVisibility::PRIVATE);
            if (!inter.empty()) target->append_property("LINK_DIRECTORIES", inter, PropertyVisibility::INTERFACE);
        }
    });

    interp.add_builtin("link_directories", [](Interpreter& interp, const std::vector<std::string>& args) {
        if (args.empty()) { return; }

        std::string src_dir = interp.get_variable("CMAKE_CURRENT_SOURCE_DIR");
        auto& dirs = interp.get_current_directory_context().accumulated["LINK_DIRECTORIES"];

        for (const auto& arg : args) {
            // Split semicolons - CMake treats all args as lists
            for (const auto& item : CMakeArrayIterator(arg)) {
                if (item.empty()) continue;
                dirs.push_back(Path(item).is_absolute() ? std::string(item) : Path::join(src_dir, item));
            }
        }
    });

    interp.add_builtin("add_definitions", [](Interpreter& interp, const std::vector<std::string>& args) {
        if (args.empty()) { return; }

        auto& defs = interp.get_current_directory_context().accumulated["COMPILE_DEFINITIONS"];
        auto& opts = interp.get_current_directory_context().accumulated["COMPILE_OPTIONS"];
        bool next_is_option_value = false;
        for (const auto& arg : args) {
            if (next_is_option_value) {
                // Argument to a preceding flag (e.g. "stdint.h" after "-include")
                opts.push_back(arg);
                next_is_option_value = false;
            } else if (arg.size() >= 2 && arg[0] == '-' && arg[1] == 'D') {
                // -Dfoo -> definition "foo"
                defs.push_back(arg.substr(2));
            } else if (!arg.empty() && arg[0] == '-') {
                // Non-definition flags (e.g. -Wall) go to compile options
                opts.push_back(arg);
                // HACK: some projects pass flags like "-include stdint.h" through
                // add_definitions(). CMake keeps the argument with its flag; we do
                // the same by consuming the next token as an option value.
                if (arg == "-include" || arg == "-isystem" || arg == "-I" || arg == "-D" || arg == "-U" || arg == "-F"
                    || arg == "-iframework") {
                    next_is_option_value = true;
                }
            } else if (!arg.empty()) {
                // Bare name (e.g. HAS_SOCKLEN_T) -> definition
                defs.push_back(arg);
            }
        }
    });

    interp.add_builtin("remove_definitions", [](Interpreter& interp, const std::vector<std::string>& args) {
        if (args.empty()) { return; }

        // Build set of items to remove (args may be semicolon-separated lists)
        std::set<std::string> to_remove;
        for (const auto& arg : args) {
            for (auto sv : CMakeArrayIterator(arg)) {
                std::string def(sv);
                // Strip -D prefix if present (CMake compatibility)
                if (def.size() >= 2 && def[0] == '-' && def[1] == 'D') { def = def.substr(2); }
                to_remove.insert(def);
            }
        }

        // Flatten accumulated defs (may contain semicolon-separated entries), filter, store back
        auto& defs = interp.get_current_directory_context().accumulated["COMPILE_DEFINITIONS"];
        CMakeArray all_defs;
        for (const auto& d : defs) { all_defs.append(d); }

        std::vector<std::string> filtered;
        for (const auto& def : all_defs) {
            if (to_remove.find(def) == to_remove.end()) { filtered.push_back(def); }
        }
        defs = std::move(filtered);
    });

    interp.add_builtin("add_compile_definitions", [](Interpreter& interp, const std::vector<std::string>& args) {
        if (args.empty()) { return; }

        auto& defs = interp.get_current_directory_context().accumulated["COMPILE_DEFINITIONS"];
        // No -D prefix stripping (modern CMake style)
        defs.insert(defs.end(), args.begin(), args.end());
    });

    interp.add_builtin("add_compile_options", [](Interpreter& interp, const std::vector<std::string>& args) {
        if (args.empty()) { return; }

        auto& opts = interp.get_current_directory_context().accumulated["COMPILE_OPTIONS"];
        opts.insert(opts.end(), args.begin(), args.end());
    });

    interp.add_builtin("add_link_options", [](Interpreter& interp, const std::vector<std::string>& args) {
        if (args.empty()) { return; }

        auto& opts = interp.get_current_directory_context().accumulated["LINK_OPTIONS"];
        opts.insert(opts.end(), args.begin(), args.end());
    });

    interp.add_builtin("link_libraries", [](Interpreter& interp, const std::vector<std::string>& args) {
        if (args.empty()) { return; }

        // Resolve target aliases (same as target_link_libraries does)
        std::vector<std::string> resolved_libs;
        for (const auto& lib : args) { resolved_libs.push_back(interp.resolve_target_alias(lib)); }

        auto& libs = interp.get_current_directory_context().accumulated["LINK_LIBRARIES"];
        libs.insert(libs.end(), resolved_libs.begin(), resolved_libs.end());
    });

    interp.add_builtin("add_dependencies", [get_target_from_name](Interpreter& interp, const std::vector<std::string>& args) {
        if (args.empty()) {
            interp.set_fatal_error("add_dependencies() requires at least a target name");
            return;
        }

        std::string target_name = args[0];

        // Resolve alias to real target name
        std::string resolved_name = interp.resolve_target_alias(target_name);

        auto target = get_target_from_name(interp, resolved_name, "add_dependencies");
        if (!target) return;

        // Add each dependency (CMake 4.1+ allows no dependencies, so skip if args.size() == 1)
        for (size_t i = 1; i < args.size(); ++i) {
            std::string dep = interp.resolve_target_alias(args[i]);

            // CMake allows forward references to targets not yet defined. This is a bad idea
            // as it makes build order dependent on definition order, but we allow it with a warning.
            auto& targets = interp.get_root()->targets_;
            if (targets.find(dep) == targets.end()) {
                // interp.print_message("WARNING", "add_dependencies() references target \"" + args[i]
                //           + "\" which does not exist yet. This is a bad idea - consider "
                //           + "reordering your CMakeLists.txt to define targets before referencing them.");
            }

            target->add_dependency(dep);
        }
    });

    interp.add_builtin("kiln_dump_target_info", [get_target_from_name](Interpreter& interp, const std::vector<std::string>& args) {
        CommandParser parser("kiln_dump_target_info");
        std::string name;
        bool at_build = false;
        parser.positional(name, "target name", true);
        parser.flag("AT_BUILD", at_build);
        PARSE_OR_RETURN(parser, interp, args);

        auto target = get_target_from_name(interp, name, "kiln_dump_target_info");
        if (!target) return;

        if (at_build) {
            // Defer printing until after target resolution during build
            interp.add_target_to_dump_at_build(target->get_name());
        } else {
            // Print immediately
            interp.print_message("", target->generate_dump_info());
        }
    });
}

} // namespace kiln
