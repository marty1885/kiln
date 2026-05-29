#include "registry.hpp"
#include "export_generator.hpp"
#include "../interperter.hpp"
#include "../command_parser.hpp"
#include <filesystem>
#include <algorithm>
#include <fstream>

namespace kiln {

namespace {

// Map install TYPE keyword to default destination directory.
// Checks CMAKE_INSTALL_*DIR variables first, falling back to GNUInstallDirs defaults.
std::string resolve_install_type(Interpreter& interp, const std::string& type) {
    static const struct {
        const char* type;
        const char* var;
        const char* fallback;
    } mappings[] = {
        {"BIN", "CMAKE_INSTALL_BINDIR", "bin"},
        {"SBIN", "CMAKE_INSTALL_SBINDIR", "sbin"},
        {"LIB", "CMAKE_INSTALL_LIBDIR", "lib"},
        {"INCLUDE", "CMAKE_INSTALL_INCLUDEDIR", "include"},
        {"SYSCONF", "CMAKE_INSTALL_SYSCONFDIR", "etc"},
        {"SHAREDSTATE", "CMAKE_INSTALL_SHAREDSTATEDIR", "com"},
        {"LOCALSTATE", "CMAKE_INSTALL_LOCALSTATEDIR", "var"},
        {"RUNSTATE", "CMAKE_INSTALL_RUNSTATEDIR", "run"},
        {"DATA", "CMAKE_INSTALL_DATADIR", "share"},
        {"INFO", "CMAKE_INSTALL_INFODIR", "share/info"},
        {"LOCALE", "CMAKE_INSTALL_LOCALEDIR", "share/locale"},
        {"MAN", "CMAKE_INSTALL_MANDIR", "share/man"},
        {"DOC", "CMAKE_INSTALL_DOCDIR", "share/doc"},
    };
    for (const auto& m : mappings) {
        if (type == m.type) {
            auto val = interp.get_variable(m.var);
            return val.empty() ? std::string(m.fallback) : val;
        }
    }
    return {};
}

// Parse common destination properties
void parse_destination_properties(CommandParser& parser, InstallDestination& dest, const std::string& keyword_prefix = "") {
    std::string dest_keyword = keyword_prefix.empty() ? "DESTINATION" : keyword_prefix + "_DESTINATION";
    std::string perm_keyword = keyword_prefix.empty() ? "PERMISSIONS" : keyword_prefix + "_PERMISSIONS";

    parser.value(dest_keyword, dest.destination);
    parser.list(perm_keyword, dest.permissions);
    parser.value("COMPONENT", dest.component);
    parser.list("CONFIGURATIONS", dest.configurations);
    parser.flag("OPTIONAL", dest.optional);
    parser.flag("EXCLUDE_FROM_ALL", dest.exclude_from_all);
}

// Parse install(TARGETS ...) command
void parse_install_targets(Interpreter& interp, const std::vector<std::string>& args, const std::string& src_dir,
                           const std::string& bin_dir) {
    // Skip first argument (TARGETS keyword)
    std::vector<std::string> parse_args(args.begin() + 1, args.end());

    CommandParser parser("install(TARGETS)");

    auto rule = std::make_shared<InstallTargetsRule>();

    // Parse target list (all positional args before first keyword)
    size_t i = 0;
    std::string export_name;
    while (i < parse_args.size()) {
        const auto& arg = parse_args[i];
        // Stop at first keyword
        if (arg == "EXPORT" || arg == "ARCHIVE" || arg == "LIBRARY" || arg == "RUNTIME" || arg == "BUNDLE" || arg == "PUBLIC_HEADER"
            || arg == "PRIVATE_HEADER" || arg == "INCLUDES" || arg == "DESTINATION" || arg == "PERMISSIONS" || arg == "CONFIGURATIONS"
            || arg == "COMPONENT" || arg == "OPTIONAL" || arg == "EXCLUDE_FROM_ALL") {
            break;
        }
        rule->targets.push_back(arg);
        ++i;
    }

    if (rule->targets.empty()) {
        // CMake silently accepts install(TARGETS) with no targets
        // (e.g. when a variable expands to empty)
        interp.print_message("WARNING",
                             "install(TARGETS) called with no targets "
                             "(undocumented CMake behavior, accepted for compatibility)",
                             true);
        return;
    }

    // Validate all targets exist
    for (const auto& target_name : rule->targets) {
        if (!interp.find_target(target_name)) {
            interp.set_fatal_error("install(TARGETS) unknown target: " + target_name);
            return;
        }
    }

    // Parse destination-specific keywords
    std::vector<std::string> remaining_args(parse_args.begin() + i, parse_args.end());

    // Track which destination type we're currently parsing
    std::string current_dest_type;
    InstallDestination* current_dest = nullptr;

    // INCLUDES DESTINATION dirs — these are added to each target's
    // INTERFACE_INCLUDE_DIRECTORIES wrapped in $<INSTALL_INTERFACE:...> so
    // the install export propagates them. Distinct from header file install
    // (which uses install(FILES ...)).
    std::vector<std::string> includes_destinations;

    i = 0;
    while (i < remaining_args.size()) {
        const auto& arg = remaining_args[i];

        if (arg == "EXPORT") {
            // Handle EXPORT keyword - consume it and its value
            if (i + 1 >= remaining_args.size()) {
                interp.set_fatal_error("install(TARGETS) EXPORT requires a value");
                return;
            }
            export_name = remaining_args[i + 1];
            i += 2;
        } else if (arg == "ARCHIVE" || arg == "LIBRARY" || arg == "RUNTIME" || arg == "BUNDLE" || arg == "PUBLIC_HEADER"
                   || arg == "PRIVATE_HEADER") {
            current_dest_type = arg;
            if (arg == "ARCHIVE")
                current_dest = &rule->archive_dest;
            else if (arg == "LIBRARY")
                current_dest = &rule->library_dest;
            else if (arg == "RUNTIME")
                current_dest = &rule->runtime_dest;
            else if (arg == "BUNDLE")
                current_dest = &rule->bundle_dest;
            else if (arg == "PUBLIC_HEADER")
                current_dest = &rule->public_header_dest;
            else if (arg == "PRIVATE_HEADER")
                current_dest = &rule->private_header_dest;
            ++i;
        } else if (arg == "INCLUDES") {
            // Expect: INCLUDES DESTINATION dir1 [dir2 ...]
            if (i + 1 >= remaining_args.size() || remaining_args[i + 1] != "DESTINATION") {
                interp.set_fatal_error("install(TARGETS) INCLUDES must be followed by DESTINATION");
                return;
            }
            i += 2;
            // Consume destinations until next keyword
            while (i < remaining_args.size()) {
                const auto& a = remaining_args[i];
                if (a == "EXPORT" || a == "ARCHIVE" || a == "LIBRARY" || a == "RUNTIME" || a == "BUNDLE" || a == "PUBLIC_HEADER"
                    || a == "PRIVATE_HEADER" || a == "INCLUDES" || a == "DESTINATION" || a == "PERMISSIONS" || a == "CONFIGURATIONS"
                    || a == "COMPONENT" || a == "OPTIONAL" || a == "EXCLUDE_FROM_ALL") {
                    break;
                }
                includes_destinations.push_back(a);
                ++i;
            }
            // After INCLUDES DESTINATION, no per-type destination is active
            current_dest = nullptr;
            current_dest_type.clear();
        } else if (arg == "DESTINATION") {
            if (i + 1 >= remaining_args.size()) {
                interp.set_fatal_error("install(TARGETS) DESTINATION requires a value");
                return;
            }
            if (current_dest) {
                // Destination type was specified (e.g., ARCHIVE DESTINATION lib)
                current_dest->destination = remaining_args[i + 1];
            } else {
                // Shorthand form: DESTINATION applies to all applicable types
                // (ARCHIVE for static, LIBRARY for shared, RUNTIME for executables)
                rule->archive_dest.destination = remaining_args[i + 1];
                rule->library_dest.destination = remaining_args[i + 1];
                rule->runtime_dest.destination = remaining_args[i + 1];
            }
            i += 2;
        } else if (arg == "PERMISSIONS" && current_dest) {
            ++i;
            while (i < remaining_args.size() && remaining_args[i].find("_") != std::string::npos
                   && (remaining_args[i].find("OWNER_") == 0 || remaining_args[i].find("GROUP_") == 0
                       || remaining_args[i].find("WORLD_") == 0)) {
                current_dest->permissions.push_back(remaining_args[i]);
                ++i;
            }
        } else if (arg == "CONFIGURATIONS" && current_dest) {
            ++i;
            while (i < remaining_args.size() && remaining_args[i] != "EXPORT" && remaining_args[i] != "ARCHIVE"
                   && remaining_args[i] != "LIBRARY" && remaining_args[i] != "RUNTIME" && remaining_args[i] != "BUNDLE"
                   && remaining_args[i] != "PUBLIC_HEADER" && remaining_args[i] != "PRIVATE_HEADER" && remaining_args[i] != "DESTINATION"
                   && remaining_args[i] != "PERMISSIONS" && remaining_args[i] != "COMPONENT" && remaining_args[i] != "OPTIONAL"
                   && remaining_args[i] != "EXCLUDE_FROM_ALL") {
                current_dest->configurations.push_back(remaining_args[i]);
                ++i;
            }
        } else if (arg == "COMPONENT" && current_dest) {
            if (i + 1 >= remaining_args.size()) {
                interp.set_fatal_error("install(TARGETS) COMPONENT requires a value");
                return;
            }
            current_dest->component = remaining_args[i + 1];
            i += 2;
        } else if (arg == "OPTIONAL" && current_dest) {
            current_dest->optional = true;
            ++i;
        } else if (arg == "EXCLUDE_FROM_ALL" && current_dest) {
            current_dest->exclude_from_all = true;
            ++i;
        } else {
            ++i;
        }
    }

    // Apply default destinations from GNUInstallDirs when DESTINATION was not specified.
    // This matches CMake behavior: install(TARGETS foo RUNTIME) installs to ${CMAKE_INSTALL_BINDIR}
    // even with no explicit DESTINATION.
    auto var_or = [&](const char* var, const char* fallback) {
        auto v = interp.get_variable(var);
        return v.empty() ? std::string(fallback) : v;
    };
    if (rule->runtime_dest.destination.empty()) { rule->runtime_dest.destination = var_or("CMAKE_INSTALL_BINDIR", "bin"); }
    if (rule->library_dest.destination.empty()) { rule->library_dest.destination = var_or("CMAKE_INSTALL_LIBDIR", "lib"); }
    if (rule->archive_dest.destination.empty()) { rule->archive_dest.destination = var_or("CMAKE_INSTALL_LIBDIR", "lib"); }

    // Apply INCLUDES DESTINATION to each target's INTERFACE_INCLUDE_DIRECTORIES,
    // wrapped in $<INSTALL_INTERFACE:...> so the value only takes effect
    // through install exports — not in the build tree.
    if (!includes_destinations.empty()) {
        std::vector<std::string> wrapped;
        wrapped.reserve(includes_destinations.size());
        for (const auto& d : includes_destinations) { wrapped.push_back("$<INSTALL_INTERFACE:" + d + ">"); }
        for (const auto& target_name : rule->targets) {
            if (auto* t = interp.find_target(target_name)) {
                t->append_property("INCLUDE_DIRECTORIES", wrapped, PropertyVisibility::INTERFACE);
            }
        }
    }

    // Add targets to the export set
    if (!export_name.empty()) {
        for (const auto& target_name : rule->targets) {
            interp.add_to_export_set(export_name, target_name, src_dir, bin_dir, rule->archive_dest.destination,
                                     rule->library_dest.destination, rule->runtime_dest.destination);
        }
    }

    // Create install rule
    auto install_rule = std::make_shared<InstallRule>();
    install_rule->type = InstallRuleType::TARGETS;
    install_rule->source_dir = src_dir;
    install_rule->binary_dir = bin_dir;
    install_rule->targets_rule = rule;

    interp.get_install_rules().push_back(install_rule);
}

// Parse install(FILES ...) or install(PROGRAMS ...)
void parse_install_files(Interpreter& interp, const std::vector<std::string>& args, const std::string& src_dir, const std::string& bin_dir,
                         bool is_programs) {
    const char* mode = is_programs ? "PROGRAMS" : "FILES";

    // Skip first argument (FILES/PROGRAMS keyword)
    std::vector<std::string> parse_args(args.begin() + 1, args.end());

    auto rule = std::make_shared<InstallFilesRule>();
    rule->is_programs = is_programs;

    std::vector<std::string> raw_files;
    std::string type_str;

    CommandParser parser("install", mode);
    parser.positionals(raw_files, "files", false);
    parser.value("DESTINATION", rule->destination.destination);
    parser.value("TYPE", type_str);
    parser.list("PERMISSIONS", rule->destination.permissions);
    parser.list("CONFIGURATIONS", rule->destination.configurations);
    parser.value("COMPONENT", rule->destination.component);
    parser.value("RENAME", rule->rename);
    parser.flag("OPTIONAL", rule->destination.optional);
    parser.flag("EXCLUDE_FROM_ALL", rule->destination.exclude_from_all);
    PARSE_OR_RETURN(parser, interp, parse_args);

    // No files provided - CMake silently accepts this (undocumented behavior)
    if (raw_files.empty()) {
        interp.print_message("WARNING", "install(" + std::string(mode) + ") called with no files - ignoring (undocumented CMake behavior)",
                             true);
        return;
    }

    // RENAME only valid with a single file
    if (!rule->rename.empty() && raw_files.size() > 1) {
        interp.set_fatal_error("install(" + std::string(mode) + ") RENAME may only be used with one file");
        return;
    }

    // Resolve TYPE to destination
    if (!type_str.empty()) {
        auto dest = resolve_install_type(interp, type_str);
        if (dest.empty()) {
            interp.set_fatal_error("install(" + std::string(mode) + ") unknown TYPE: " + type_str);
            return;
        }
        rule->destination.destination = dest;
    }

    if (rule->destination.destination.empty()) {
        interp.set_fatal_error("install(" + std::string(mode) + ") requires DESTINATION or TYPE");
        return;
    }

    // Resolve file paths to absolute
    for (const auto& f : raw_files) {
        std::filesystem::path file_path = f;
        if (!file_path.is_absolute()) { file_path = std::filesystem::path(src_dir) / file_path; }
        rule->files.push_back(file_path.lexically_normal().string());
    }

    // Create install rule
    auto install_rule = std::make_shared<InstallRule>();
    install_rule->type = is_programs ? InstallRuleType::PROGRAMS : InstallRuleType::FILES;
    install_rule->source_dir = src_dir;
    install_rule->binary_dir = bin_dir;
    install_rule->files_rule = rule;

    interp.get_install_rules().push_back(install_rule);
}

// Parse install(DIRECTORY ...)
void parse_install_directory(Interpreter& interp, const std::vector<std::string>& args, const std::string& src_dir,
                             const std::string& bin_dir) {
    // Skip first argument (DIRECTORY keyword)
    std::vector<std::string> parse_args(args.begin() + 1, args.end());

    auto rule = std::make_shared<InstallDirectoryRule>();

    // Parse directory list
    size_t i = 0;
    while (i < parse_args.size()) {
        const auto& arg = parse_args[i];
        if (arg == "DESTINATION" || arg == "TYPE" || arg == "FILE_PERMISSIONS" || arg == "DIRECTORY_PERMISSIONS"
            || arg == "USE_SOURCE_PERMISSIONS" || arg == "FILES_MATCHING" || arg == "PATTERN" || arg == "CONFIGURATIONS"
            || arg == "COMPONENT" || arg == "OPTIONAL" || arg == "EXCLUDE_FROM_ALL") {
            break;
        }
        // Resolve relative paths
        std::filesystem::path dir_path = arg;
        if (!dir_path.is_absolute()) { dir_path = std::filesystem::path(src_dir) / dir_path; }
        rule->directories.push_back(dir_path.lexically_normal().string());
        ++i;
    }

    if (rule->directories.empty()) {
        // CMake silently accepts install(DIRECTORY) with no directories
        // (e.g. when a variable expands to empty)
        interp.print_message("WARNING",
                             "install(DIRECTORY) called with no directories "
                             "(undocumented CMake behavior, accepted for compatibility)",
                             true);
        return;
    }

    // Parse remaining arguments
    std::vector<std::string> remaining_args(parse_args.begin() + i, parse_args.end());

    i = 0;
    while (i < remaining_args.size()) {
        const auto& arg = remaining_args[i];

        if (arg == "DESTINATION") {
            if (i + 1 >= remaining_args.size()) {
                interp.set_fatal_error("install(DIRECTORY) DESTINATION requires a value");
                return;
            }
            rule->destination.destination = remaining_args[i + 1];
            i += 2;
        } else if (arg == "TYPE") {
            if (i + 1 >= remaining_args.size()) {
                interp.set_fatal_error("install(DIRECTORY) TYPE requires a value");
                return;
            }
            auto dest = resolve_install_type(interp, remaining_args[i + 1]);
            if (dest.empty()) {
                interp.set_fatal_error("install(DIRECTORY) unknown TYPE: " + remaining_args[i + 1]);
                return;
            }
            rule->destination.destination = dest;
            i += 2;
        } else if (arg == "USE_SOURCE_PERMISSIONS") {
            rule->use_source_permissions = true;
            ++i;
        } else if (arg == "PATTERN") {
            if (i + 1 >= remaining_args.size()) {
                interp.set_fatal_error("install(DIRECTORY) PATTERN requires a value");
                return;
            }
            ++i;
            std::string pattern = remaining_args[i];
            ++i;

            // Check if followed by EXCLUDE
            bool is_exclude = false;
            if (i < remaining_args.size() && remaining_args[i] == "EXCLUDE") {
                is_exclude = true;
                ++i;
            }

            if (is_exclude) {
                rule->exclude_patterns.push_back(pattern);
            } else {
                rule->file_patterns.push_back(pattern);
            }
        } else if (arg == "FILES_MATCHING") {
            ++i;
        } else if (arg == "COMPONENT") {
            if (i + 1 >= remaining_args.size()) {
                interp.set_fatal_error("install(DIRECTORY) COMPONENT requires a value");
                return;
            }
            rule->destination.component = remaining_args[i + 1];
            i += 2;
        } else if (arg == "CONFIGURATIONS") {
            ++i;
            while (i < remaining_args.size() && remaining_args[i] != "DESTINATION" && remaining_args[i] != "PATTERN"
                   && remaining_args[i] != "COMPONENT" && remaining_args[i] != "OPTIONAL" && remaining_args[i] != "EXCLUDE_FROM_ALL"
                   && remaining_args[i] != "USE_SOURCE_PERMISSIONS") {
                rule->destination.configurations.push_back(remaining_args[i]);
                ++i;
            }
        } else if (arg == "OPTIONAL") {
            rule->destination.optional = true;
            ++i;
        } else if (arg == "EXCLUDE_FROM_ALL") {
            rule->destination.exclude_from_all = true;
            ++i;
        } else {
            ++i;
        }
    }

    if (rule->destination.destination.empty()) {
        interp.set_fatal_error("install(DIRECTORY) requires DESTINATION or TYPE");
        return;
    }

    // Create install rule
    auto install_rule = std::make_shared<InstallRule>();
    install_rule->type = InstallRuleType::DIRECTORY;
    install_rule->source_dir = src_dir;
    install_rule->binary_dir = bin_dir;
    install_rule->directory_rule = rule;

    interp.get_install_rules().push_back(install_rule);
}

// Parse install(SCRIPT/CODE ...) - supports mixed SCRIPT and CODE entries in a single call.
// CMake syntax: install([[SCRIPT <file>] [CODE <code>]]...
//                       [ALL_COMPONENTS | COMPONENT <component>]
//                       [EXCLUDE_FROM_ALL])
void parse_install_script(Interpreter& interp, const std::vector<std::string>& args, const std::string& src_dir,
                          const std::string& bin_dir) {
    // Collect script/code entries and shared options
    struct Entry {
        bool is_script;
        std::string content;
    };
    std::vector<Entry> entries;
    std::string component;
    bool all_components = false;
    bool exclude_from_all = false;

    size_t i = 0;
    while (i < args.size()) {
        const auto& arg = args[i];
        if (arg == "SCRIPT") {
            if (i + 1 >= args.size()) {
                interp.set_fatal_error("install(SCRIPT) requires a file path");
                return;
            }
            entries.push_back({true, args[++i]});
        } else if (arg == "CODE") {
            if (i + 1 >= args.size()) {
                interp.set_fatal_error("install(CODE) requires a code string");
                return;
            }
            entries.push_back({false, args[++i]});
        } else if (arg == "COMPONENT") {
            if (i + 1 >= args.size()) {
                interp.set_fatal_error("install(SCRIPT/CODE) COMPONENT requires a value");
                return;
            }
            component = args[++i];
        } else if (arg == "ALL_COMPONENTS") {
            all_components = true;
        } else if (arg == "EXCLUDE_FROM_ALL") {
            exclude_from_all = true;
        } else {
            interp.set_fatal_error("install(SCRIPT/CODE): unexpected argument '" + arg + "'");
            return;
        }
        ++i;
    }

    if (entries.empty()) {
        interp.set_fatal_error("install(SCRIPT/CODE) requires at least one SCRIPT or CODE entry");
        return;
    }

    if (all_components && !component.empty()) {
        interp.set_fatal_error("install(SCRIPT/CODE): ALL_COMPONENTS and COMPONENT are mutually exclusive");
        return;
    }

    (void) exclude_from_all; // Stored but not used during interpretation

    // Create an install rule for each entry, sharing the same component
    for (const auto& entry : entries) {
        auto rule = std::make_shared<InstallScriptRule>();
        rule->component = component;

        if (entry.is_script) {
            std::filesystem::path script_path = entry.content;
            if (!script_path.is_absolute()) { script_path = std::filesystem::path(src_dir) / script_path; }
            rule->script_path = script_path.lexically_normal().string();
        } else {
            rule->code = entry.content;
        }

        auto install_rule = std::make_shared<InstallRule>();
        install_rule->type = entry.is_script ? InstallRuleType::SCRIPT : InstallRuleType::CODE;
        install_rule->source_dir = src_dir;
        install_rule->binary_dir = bin_dir;
        install_rule->script_rule = rule;

        interp.get_install_rules().push_back(install_rule);
    }
}

// Parse install(EXPORT ...)
void parse_install_export(Interpreter& interp, const std::vector<std::string>& args, const std::string& src_dir,
                          const std::string& bin_dir) {
    // Skip first argument (EXPORT keyword)
    std::vector<std::string> parse_args(args.begin() + 1, args.end());

    auto rule = std::make_shared<InstallExportRule>();

    CommandParser parser("install", "EXPORT");
    parser.positional(rule->export_name, "export_name", false);
    parser.value("FILE", rule->file_name);
    parser.value("NAMESPACE", rule->namespace_prefix);
    parser.value("DESTINATION", rule->destination);
    parser.value("COMPONENT", rule->component);
    PARSE_OR_RETURN(parser, interp, parse_args);

    if (rule->export_name.empty()) {
        // CMake silently accepts install(EXPORT) with no export name
        // (e.g. when a variable expands to empty)
        interp.print_message("WARNING",
                             "install(EXPORT) called with no export name "
                             "(undocumented CMake behavior, accepted for compatibility)",
                             true);
        return;
    }

    // Look up the export set
    const auto& export_sets = interp.get_export_sets();
    auto it = export_sets.find(rule->export_name);
    if (it == export_sets.end() || it->second.empty()) {
        interp.print_message("WARNING",
                             "install(EXPORT) export set '" + rule->export_name + "' has no targets - no export file will be generated");
        return;
    }

    // Collect the targets from the export set
    std::vector<Target*> targets_to_export;
    for (const auto& entry : it->second) {
        auto* target = interp.find_target(entry.target_name);
        if (target) {
            targets_to_export.push_back(target);
        } else {
            interp.print_message("WARNING",
                                 "install(EXPORT) target '" + entry.target_name + "' in export set '" + rule->export_name + "' not found");
        }
    }

    // Create install rule (actual file generation happens at install time)
    auto install_rule = std::make_shared<InstallRule>();
    install_rule->type = InstallRuleType::EXPORT;
    install_rule->source_dir = src_dir;
    install_rule->binary_dir = bin_dir;
    install_rule->export_rule = rule;

    interp.get_install_rules().push_back(install_rule);
}

} // anonymous namespace

void register_install_builtins(Interpreter& interp) {
    interp.add_builtin("install", [](Interpreter& interp, const std::vector<std::string>& args) {
        if (args.empty()) {
            interp.set_fatal_error("install() requires arguments");
            return;
        }

        std::string mode = args[0];
        std::string src_dir = interp.get_variable("CMAKE_CURRENT_SOURCE_DIR");
        std::string bin_dir = interp.get_variable("CMAKE_CURRENT_BINARY_DIR");

        if (mode == "TARGETS") {
            parse_install_targets(interp, args, src_dir, bin_dir);
        } else if (mode == "FILES") {
            parse_install_files(interp, args, src_dir, bin_dir, false);
        } else if (mode == "PROGRAMS") {
            parse_install_files(interp, args, src_dir, bin_dir, true);
        } else if (mode == "DIRECTORY") {
            parse_install_directory(interp, args, src_dir, bin_dir);
        } else if (mode == "SCRIPT" || mode == "CODE") {
            parse_install_script(interp, args, src_dir, bin_dir);
        } else if (mode == "EXPORT") {
            parse_install_export(interp, args, src_dir, bin_dir);
        } else if (mode == "IMPORTED_RUNTIME_ARTIFACTS") {
            // CMake 3.21+: install runtime artifacts of imported targets.
            // No-op for now; relevant only at install time, not for build.
            interp.print_warning_with_context("install(IMPORTED_RUNTIME_ARTIFACTS) is not implemented; ignoring");
        } else {
            interp.set_fatal_error("install() first argument must be TARGETS, FILES, PROGRAMS, DIRECTORY, SCRIPT, CODE, or EXPORT");
        }
    });

    interp.add_builtin("export", [](Interpreter& interp, const std::vector<std::string>& args) {
        // export(TARGETS t1 t2... FILE file.cmake [NAMESPACE ns::] [APPEND])
        // export(EXPORT export_name ...) - alternate form using install export set
        // export(PACKAGE name) - register package in user package registry (ignored for now)
        if (args.empty()) {
            interp.set_fatal_error("export() requires arguments");
            return;
        }

        std::string src_dir = interp.get_variable("CMAKE_CURRENT_SOURCE_DIR");
        std::string bin_dir = interp.get_variable("CMAKE_CURRENT_BINARY_DIR");

        // Handle export(PACKAGE) - just ignore it (package registry not supported)
        if (args[0] == "PACKAGE") {
            if (args.size() < 2) {
                interp.set_fatal_error("export(PACKAGE) requires a package name");
                return;
            }
            // Silently ignore - package registry is not commonly used
            return;
        }

        // Handle export(EXPORT export_name ...) - uses an install export set
        if (args[0] == "EXPORT") {
            if (args.size() < 2) {
                interp.set_fatal_error("export(EXPORT) requires an export set name");
                return;
            }

            std::string export_set_name = args[1];
            std::string file_path;
            std::string namespace_prefix;

            // Parse remaining arguments
            for (size_t i = 2; i < args.size(); ++i) {
                if (args[i] == "FILE" && i + 1 < args.size()) {
                    file_path = args[++i];
                } else if (args[i] == "NAMESPACE" && i + 1 < args.size()) {
                    namespace_prefix = args[++i];
                }
            }

            // Look up export set
            const auto& export_sets = interp.get_export_sets();
            auto it = export_sets.find(export_set_name);
            if (it == export_sets.end() || it->second.empty()) {
                interp.print_message("WARNING", "export(EXPORT) export set '" + export_set_name + "' has no targets");
                return;
            }

            // Collect targets
            std::vector<Target*> targets_to_export;
            for (const auto& entry : it->second) {
                auto* target = interp.find_target(entry.target_name);
                if (target) { targets_to_export.push_back(target); }
            }

            if (targets_to_export.empty()) {
                interp.print_message("WARNING", "export(EXPORT) no valid targets found in export set '" + export_set_name + "'");
                return;
            }

            // Default file name
            if (file_path.empty()) { file_path = export_set_name + ".cmake"; }

            // Resolve file path relative to binary dir
            std::filesystem::path output_file = file_path;
            if (!output_file.is_absolute()) { output_file = std::filesystem::path(bin_dir) / output_file; }

            // Generate export content
            ExportContext ctx;
            ctx.for_install = false; // Build-tree export
            ctx.namespace_prefix = namespace_prefix;
            ctx.destination = "";
            ctx.install_prefix = interp.get_variable("CMAKE_INSTALL_PREFIX");
            ctx.build_type = interp.get_variable("CMAKE_BUILD_TYPE");
            ctx.config = interp.get_variable("CMAKE_BUILD_TYPE"); // Per-config properties
            ctx.system_name = interp.get_variable("CMAKE_SYSTEM_NAME");
            ctx.cxx_compiler_id = interp.get_variable("CMAKE_CXX_COMPILER_ID");
            ctx.c_compiler_id = interp.get_variable("CMAKE_C_COMPILER_ID");
            ctx.cxx_compiler_version = interp.get_variable("CMAKE_CXX_COMPILER_VERSION");
            ctx.c_compiler_version = interp.get_variable("CMAKE_C_COMPILER_VERSION");
            ctx.all_targets = &interp.get_targets();
            ctx.target_aliases = &interp.get_target_aliases();

            std::string content = generate_export_content(ctx, targets_to_export);

            // Write to file
            std::filesystem::create_directories(output_file.parent_path());
            std::ofstream out(output_file);
            if (!out) {
                interp.set_fatal_error("export(EXPORT): failed to open file for writing: " + output_file.string());
                return;
            }
            out << content;
            return;
        }

        // Handle export(TARGETS ...) form
        if (args[0] != "TARGETS") {
            interp.set_fatal_error("export() first argument must be TARGETS, EXPORT, or PACKAGE");
            return;
        }

        // Parse TARGETS list and options
        std::vector<std::string> target_names;
        std::string file_path;
        std::string namespace_prefix;
        bool append = false;

        size_t i = 1; // Skip "TARGETS"
        while (i < args.size()) {
            const auto& arg = args[i];
            if (arg == "FILE") {
                if (i + 1 >= args.size()) {
                    interp.set_fatal_error("export(TARGETS) FILE requires a value");
                    return;
                }
                file_path = args[++i];
            } else if (arg == "NAMESPACE") {
                if (i + 1 >= args.size()) {
                    interp.set_fatal_error("export(TARGETS) NAMESPACE requires a value");
                    return;
                }
                namespace_prefix = args[++i];
            } else if (arg == "APPEND") {
                append = true;
            } else if (arg == "EXPORT_LINK_INTERFACE_LIBRARIES") {
                // Deprecated option, ignore
            } else {
                // Must be a target name
                target_names.push_back(arg);
            }
            ++i;
        }

        if (target_names.empty()) {
            interp.set_fatal_error("export(TARGETS) requires at least one target");
            return;
        }

        if (file_path.empty()) {
            interp.set_fatal_error("export(TARGETS) requires FILE argument");
            return;
        }

        // Validate and collect targets
        std::vector<Target*> targets_to_export;
        for (const auto& name : target_names) {
            auto* target = interp.find_target(name);
            if (!target) {
                interp.set_fatal_error("export(TARGETS) unknown target: " + name);
                return;
            }
            targets_to_export.push_back(target);
        }

        // Resolve file path relative to binary dir
        std::filesystem::path output_file = file_path;
        if (!output_file.is_absolute()) { output_file = std::filesystem::path(bin_dir) / output_file; }

        // Generate export content
        ExportContext ctx;
        ctx.for_install = false; // Build-tree export
        ctx.namespace_prefix = namespace_prefix;
        ctx.destination = "";
        ctx.install_prefix = interp.get_variable("CMAKE_INSTALL_PREFIX");
        ctx.build_type = interp.get_variable("CMAKE_BUILD_TYPE");
        ctx.config = interp.get_variable("CMAKE_BUILD_TYPE"); // Per-config properties
        ctx.system_name = interp.get_variable("CMAKE_SYSTEM_NAME");
        ctx.cxx_compiler_id = interp.get_variable("CMAKE_CXX_COMPILER_ID");
        ctx.c_compiler_id = interp.get_variable("CMAKE_C_COMPILER_ID");
        ctx.cxx_compiler_version = interp.get_variable("CMAKE_CXX_COMPILER_VERSION");
        ctx.c_compiler_version = interp.get_variable("CMAKE_C_COMPILER_VERSION");
        ctx.all_targets = &interp.get_targets();
        ctx.target_aliases = &interp.get_target_aliases();

        std::string content = generate_export_content(ctx, targets_to_export);

        // Write to file
        std::filesystem::create_directories(output_file.parent_path());
        std::ios_base::openmode mode = std::ios::out;
        if (append) { mode |= std::ios::app; }
        std::ofstream out(output_file, mode);
        if (!out) {
            interp.set_fatal_error("export(TARGETS): failed to open file for writing: " + output_file.string());
            return;
        }
        out << content;
    });
}

} // namespace kiln
