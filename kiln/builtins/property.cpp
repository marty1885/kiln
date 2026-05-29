#include "registry.hpp"
#include "../interperter.hpp"
#include "../target.hpp"
#include "../command_parser.hpp"
#include "../container_utils.hpp"
#include <filesystem>
#include <sstream>
#include <algorithm>

namespace kiln {

// Helper to parse property scope from string
// Note: CMake inconsistently uses "CACHED_VARIABLE" for define_property() but "CACHE" for set/get_property()
static std::optional<PropertyScope> parse_property_scope(const std::string& scope_str) {
    if (scope_str == "GLOBAL") return PropertyScope::GLOBAL;
    if (scope_str == "DIRECTORY") return PropertyScope::DIRECTORY;
    if (scope_str == "TARGET") return PropertyScope::TARGET;
    if (scope_str == "SOURCE") return PropertyScope::SOURCE;
    if (scope_str == "TEST") return PropertyScope::TEST;
    if (scope_str == "VARIABLE") return PropertyScope::VARIABLE;
    if (scope_str == "CACHED_VARIABLE") return PropertyScope::CACHED_VARIABLE;
    if (scope_str == "CACHE") return PropertyScope::CACHED_VARIABLE; // Alias for set/get_property
    if (scope_str == "INSTALL") return PropertyScope::INSTALL;
    return std::nullopt;
}

// Helper to get target by name (used in multiple places)
static Target* get_target_from_name(Interpreter& interp, const std::string& name, const std::string& cmd_name) {
    auto* target = interp.find_target(name);
    if (!target) { interp.set_fatal_error(cmd_name + "() called on unknown target '" + name + "'"); }
    return target;
}

// Helper to get test by name
static TestDefinition* get_test_from_name(Interpreter& interp, const std::string& name) {
    auto& tests = interp.get_tests();
    for (auto& test : tests) {
        if (test.name == name) { return &test; }
    }
    return nullptr;
}

// Helper to resolve source file path to absolute
static std::string resolve_source_path(Interpreter& interp, const std::string& source, const std::string* opt_directory,
                                       const std::string* opt_target_directory) {
    std::filesystem::path source_path(source);

    if (source_path.is_absolute()) { return source_path.string(); }

    // Determine the base directory
    std::string base_dir;
    if (opt_target_directory) {
        auto target = get_target_from_name(interp, *opt_target_directory, "resolve_source_path");
        if (!target) return "";
        base_dir = target->get_source_dir();
    } else if (opt_directory) {
        std::filesystem::path dir_path(*opt_directory);
        if (dir_path.is_absolute()) {
            base_dir = *opt_directory;
        } else {
            base_dir = (std::filesystem::path(interp.get_variable("CMAKE_CURRENT_SOURCE_DIR")) / dir_path).string();
        }
    } else {
        base_dir = interp.get_variable("CMAKE_CURRENT_SOURCE_DIR");
    }

    return (std::filesystem::path(base_dir) / source_path).lexically_normal().string();
}

void register_property_builtins(Interpreter& interp) {

    // get_cmake_property() - Get global CMake properties
    // Syntax: get_cmake_property(<var> <property>)
    interp.add_builtin("get_cmake_property", [](Interpreter& interp, const std::vector<std::string>& args) {
        if (args.size() != 2) {
            interp.set_fatal_error("get_cmake_property() requires exactly 2 arguments: <var> <property>");
            return;
        }

        const std::string& var_name = args[0];
        const std::string& property = args[1];

        std::string result;

        if (property == "VARIABLES") {
            // Return semicolon-separated list of all defined variables
            auto names = interp.get_variables().get_all_names();
            std::sort(names.begin(), names.end());
            for (size_t i = 0; i < names.size(); ++i) {
                if (i > 0) result += ";";
                result += names[i];
            }
        } else if (property == "CACHE_VARIABLES") {
            // Return semicolon-separated list of all cache variables
            auto& cache_vars = interp.get_cache_variables();
            std::vector<std::string> names;
            for (const auto& [name, value] : cache_vars) {
                // Skip internal property storage keys
                if (name.find(".__property__.") == std::string::npos) { names.push_back(name); }
            }
            std::sort(names.begin(), names.end());
            for (size_t i = 0; i < names.size(); ++i) {
                if (i > 0) result += ";";
                result += names[i];
            }
        } else if (property == "COMPONENTS") {
            // Return semicolon-separated list of install components
            std::set<std::string> components;
            for (const auto& rule : interp.get_install_rules()) {
                if (rule->targets_rule) {
                    if (!rule->targets_rule->archive_dest.component.empty()) components.insert(rule->targets_rule->archive_dest.component);
                    if (!rule->targets_rule->library_dest.component.empty()) components.insert(rule->targets_rule->library_dest.component);
                    if (!rule->targets_rule->runtime_dest.component.empty()) components.insert(rule->targets_rule->runtime_dest.component);
                }
                if (rule->files_rule) {
                    if (!rule->files_rule->destination.component.empty()) components.insert(rule->files_rule->destination.component);
                }
                if (rule->script_rule) {
                    if (!rule->script_rule->component.empty()) components.insert(rule->script_rule->component);
                }
                if (rule->export_rule) {
                    if (!rule->export_rule->component.empty()) components.insert(rule->export_rule->component);
                }
            }
            result = join(components, ";");
        } else if (property == "MACROS") {
            // CMake allows querying defined macros
            // We don't currently expose user_macros_, so return empty for now
            result = "";
        } else if (property == "COMMANDS") {
            // CMake allows querying defined commands
            // We don't currently expose the full command list easily
            result = "";
        } else {
            // Check global properties
            auto& global_properties = interp.get_global_properties();
            auto it = global_properties.find(property);
            if (it != global_properties.end()) {
                result = it->second;
            } else {
                result = property + "-NOTFOUND";
            }
        }

        interp.set_variable(var_name, result);
    });

    // define_property() - Define a new property with metadata
    interp.add_builtin("define_property", [](Interpreter& interp, const std::vector<std::string>& args) {
        if (args.size() < 3) {
            interp.set_fatal_error("define_property() requires at least 3 arguments: <scope> PROPERTY <name>");
            return;
        }

        std::string scope_str = args[0];
        auto scope_opt = parse_property_scope(scope_str);
        if (!scope_opt) {
            interp.set_fatal_error("define_property() invalid scope: " + scope_str);
            return;
        }
        PropertyScope scope = *scope_opt;

        if (args[1] != "PROPERTY") {
            interp.set_fatal_error("define_property() expected PROPERTY keyword, got: " + args[1]);
            return;
        }

        std::string prop_name = args[2];

        PropertyDefinition def;
        def.scope = scope;
        def.name = prop_name;

        // Parse optional arguments
        for (size_t i = 3; i < args.size(); ++i) {
            if (args[i] == "INHERITED") {
                def.inherited = true;
            } else if (args[i] == "BRIEF_DOCS" && i + 1 < args.size()) {
                // Collect all args until next keyword
                ++i;
                while (i < args.size() && args[i] != "FULL_DOCS" && args[i] != "INITIALIZE_FROM_VARIABLE") {
                    if (!def.brief_docs.empty()) def.brief_docs += " ";
                    def.brief_docs += args[i];
                    ++i;
                }
                --i; // Back up one since loop will increment
            } else if (args[i] == "FULL_DOCS" && i + 1 < args.size()) {
                ++i;
                while (i < args.size() && args[i] != "BRIEF_DOCS" && args[i] != "INITIALIZE_FROM_VARIABLE") {
                    if (!def.full_docs.empty()) def.full_docs += " ";
                    def.full_docs += args[i];
                    ++i;
                }
                --i;
            } else if (args[i] == "INITIALIZE_FROM_VARIABLE" && i + 1 < args.size()) {
                ++i;
                def.initialize_from_variable = args[i];
            }
        }

        // Check if property is already defined for this scope
        auto& property_definitions = interp.get_property_definitions();
        if (property_definitions[scope].count(prop_name)) {
            // Silently ignore redefinition attempts (CMake behavior)
            return;
        }

        // Store the definition
        property_definitions[scope][prop_name] = def;
    });

    // set_property() - Set property values for various scopes
    interp.add_builtin("set_property", [](Interpreter& interp, const std::vector<std::string>& args) {
        if (args.size() < 3) {
            interp.set_fatal_error("set_property() requires at least 3 arguments: <scope> PROPERTY <name> [values...]");
            return;
        }

        std::string scope_str = args[0];
        auto scope_opt = parse_property_scope(scope_str);
        if (!scope_opt) {
            interp.set_fatal_error("set_property() invalid scope: " + scope_str);
            return;
        }
        PropertyScope scope = *scope_opt;

        // Parse items, APPEND/APPEND_STRING flags, and find PROPERTY keyword
        std::vector<std::string> items;
        bool append = false;
        bool append_string = false;
        size_t property_idx = 0;

        for (size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "APPEND") {
                append = true;
            } else if (args[i] == "APPEND_STRING") {
                append_string = true;
            } else if (args[i] == "PROPERTY") {
                property_idx = i;
                break;
            } else {
                items.push_back(args[i]);
            }
        }

        if (property_idx == 0 || property_idx + 1 >= args.size()) {
            interp.set_fatal_error("set_property() missing PROPERTY keyword or property name");
            return;
        }

        std::string property_name = args[property_idx + 1];
        std::vector<std::string> property_values;
        for (size_t i = property_idx + 2; i < args.size(); ++i) { property_values.push_back(args[i]); }

        // Build the value string
        std::string value;
        if (append_string) {
            // APPEND_STRING: concatenate as single string
            for (const auto& v : property_values) { value += v; }
        } else {
            // Regular or APPEND: semicolon-separated list
            for (size_t i = 0; i < property_values.size(); ++i) {
                if (i > 0) value += ";";
                value += property_values[i];
            }
        }

        // Handle each scope
        auto& global_properties = interp.get_global_properties();
        auto& source_properties = interp.get_source_properties();
        auto& cache_variables = interp.get_cache_variables();

        switch (scope) {
        case PropertyScope::GLOBAL: {
            if (!items.empty()) {
                interp.set_fatal_error("set_property(GLOBAL ...) does not accept item names");
                return;
            }
            // Read-only global properties
            if (property_name == "GENERATOR_IS_MULTI_CONFIG") {
                interp.set_fatal_error("set_property: GENERATOR_IS_MULTI_CONFIG is a read-only property");
                return;
            }
            if (append || append_string) {
                std::string old_val = global_properties[property_name];
                if (!old_val.empty() && !value.empty()) {
                    if (append_string) {
                        global_properties[property_name] = old_val + value;
                    } else {
                        global_properties[property_name] = old_val + ";" + value;
                    }
                } else if (!value.empty()) {
                    global_properties[property_name] = value;
                }
            } else {
                global_properties[property_name] = value;
            }
            break;
        }

        case PropertyScope::DIRECTORY: {
            // Items are directory paths (optional)
            std::vector<DirectoryContext*> target_contexts;
            if (items.empty()) {
                // Current directory
                target_contexts.push_back(&interp.get_current_directory_context());
            } else {
                for (const auto& dir_path : items) {
                    // Check if it's the current directory
                    std::string current_source_dir = interp.get_variable("CMAKE_CURRENT_SOURCE_DIR");
                    if (dir_path == "." || dir_path == current_source_dir) {
                        target_contexts.push_back(&interp.get_current_directory_context());
                    } else {
                        // Look up the directory context for the specified directory
                        DirectoryContext* dir_ctx = interp.get_directory_context(dir_path);
                        if (dir_ctx) {
                            target_contexts.push_back(dir_ctx);
                        } else {
                            interp.set_fatal_error("set_property(DIRECTORY ...) unknown directory: " + dir_path);
                            return;
                        }
                    }
                }
            }

            for (auto* target_ctx : target_contexts) {
                auto& dir_props = target_ctx->properties;
                if (append || append_string) {
                    std::string old_val = dir_props[property_name];
                    if (!old_val.empty() && !value.empty()) {
                        if (append_string) {
                            dir_props[property_name] = old_val + value;
                        } else {
                            dir_props[property_name] = old_val + ";" + value;
                        }
                    } else if (!value.empty()) {
                        dir_props[property_name] = value;
                    }
                } else {
                    dir_props[property_name] = value;
                }

                // Sync with accumulated map (used by include_directories(), etc.)
                // In CMake these are the same property; in kiln they're stored separately.
                auto acc_it = target_ctx->accumulated.find(property_name);
                if (acc_it != target_ctx->accumulated.end() || property_values.empty()) {
                    if (append) {
                        for (const auto& v : property_values) { target_ctx->accumulated[property_name].push_back(v); }
                    } else {
                        // Replace: clear and set new values
                        target_ctx->accumulated[property_name].clear();
                        for (const auto& v : property_values) { target_ctx->accumulated[property_name].push_back(v); }
                    }
                }
            }
            break;
        }

        case PropertyScope::TARGET: {
            // Items are target names
            if (items.empty()) {
                interp.set_fatal_error("set_property(TARGET ...) requires at least one target name");
                return;
            }

            // Handle list properties generically using the shared property metadata table
            auto handle_list_property = [](Target* target, const std::string& prop_name, const std::vector<std::string>& values,
                                           bool append_mode) -> bool {
                std::string base_prop;
                PropertyVisibility visibility = PropertyVisibility::PRIVATE;

                // Check INTERFACE_ prefix first, then plain name
                if (auto* iface_meta = find_interface_list_property(prop_name)) {
                    base_prop = std::string(iface_meta->name);
                    visibility = PropertyVisibility::INTERFACE;
                } else if (auto* meta = find_list_property(prop_name)) {
                    base_prop = std::string(meta->name);
                } else {
                    return false;
                }

                std::vector<std::string> items;
                for (const auto& v : values) {
                    std::istringstream ss(v);
                    std::string item;
                    while (std::getline(ss, item, ';')) {
                        if (!item.empty()) items.push_back(item);
                    }
                }

                if (!items.empty()) { target->append_property(base_prop, items, visibility); }
                return true;
            };
            auto handle_interface_property = handle_list_property;

            for (const auto& target_name : items) {
                auto target = get_target_from_name(interp, target_name, "set_property");
                if (!target) continue;

                // Try to handle as INTERFACE property first
                if (handle_interface_property(target, property_name, property_values, append)) { continue; }

                // Special handling for language standard properties
                if (property_name == "C_STANDARD") {
                    target->set_language_standard(Language::C, value);
                    continue;
                } else if (property_name == "CXX_STANDARD") {
                    target->set_language_standard(Language::CXX, value);
                    continue;
                } else if (property_name == "C_EXTENSIONS") {
                    bool enabled = !interp.is_falsy(value);
                    target->set_language_extensions(Language::C, enabled);
                    continue;
                } else if (property_name == "CXX_EXTENSIONS") {
                    bool enabled = !interp.is_falsy(value);
                    target->set_language_extensions(Language::CXX, enabled);
                    continue;
                }

                // IMPORTED_LOCATION or IMPORTED_LOCATION_<CONFIG> → update dedicated field
                if (property_name == "IMPORTED_LOCATION" || property_name.starts_with("IMPORTED_LOCATION_")) {
                    target->set_imported_location(value);
                }

                // OUTPUT_NAME → update dedicated field (used by get_output_path())
                if (property_name == "OUTPUT_NAME") { target->set_output_name(value); }

                // Generic property handling
                if (append || append_string) {
                    std::string old_val = target->get_property(property_name);
                    if (!old_val.empty() && !value.empty()) {
                        if (append_string) {
                            target->set_property(property_name, old_val + value);
                        } else {
                            target->set_property(property_name, old_val + ";" + value);
                        }
                    } else if (!value.empty()) {
                        target->set_property(property_name, value);
                    }
                } else {
                    target->set_property(property_name, value);
                }
            }
            break;
        }

        case PropertyScope::SOURCE: {
            // Items are source file paths
            if (items.empty()) {
                interp.set_fatal_error("set_property(SOURCE ...) requires at least one source file");
                return;
            }

            // Parse optional DIRECTORY or TARGET_DIRECTORY
            std::string opt_directory;
            std::string opt_target_directory;

            // Re-scan args for sub-options (after items, before PROPERTY)
            for (size_t i = 1; i < property_idx; ++i) {
                if (args[i] == "DIRECTORY" && i + 1 < property_idx) {
                    opt_directory = args[++i];
                } else if (args[i] == "TARGET_DIRECTORY" && i + 1 < property_idx) {
                    opt_target_directory = args[++i];
                }
            }

            for (const auto& source : items) {
                std::string abs_source = resolve_source_path(interp, source, opt_directory.empty() ? nullptr : &opt_directory,
                                                             opt_target_directory.empty() ? nullptr : &opt_target_directory);
                if (abs_source.empty()) continue;

                if (append || append_string) {
                    std::string old_val = source_properties[abs_source][property_name];
                    if (!old_val.empty() && !value.empty()) {
                        if (append_string) {
                            source_properties[abs_source][property_name] = old_val + value;
                        } else {
                            source_properties[abs_source][property_name] = old_val + ";" + value;
                        }
                    } else if (!value.empty()) {
                        source_properties[abs_source][property_name] = value;
                    }
                } else {
                    source_properties[abs_source][property_name] = value;
                }
            }
            break;
        }

        case PropertyScope::TEST: {
            // Items are test names
            if (items.empty()) {
                interp.set_fatal_error("set_property(TEST ...) requires at least one test name");
                return;
            }

            for (const auto& test_name : items) {
                auto* test = get_test_from_name(interp, test_name);
                if (!test) {
                    interp.set_fatal_error("set_property(TEST ...) unknown test: " + test_name);
                    return;
                }

                if (append || append_string) {
                    std::string old_val = test->properties[property_name];
                    if (!old_val.empty() && !value.empty()) {
                        if (append_string) {
                            test->properties[property_name] = old_val + value;
                        } else {
                            test->properties[property_name] = old_val + ";" + value;
                        }
                    } else if (!value.empty()) {
                        test->properties[property_name] = value;
                    }
                } else {
                    test->properties[property_name] = value;
                }
            }
            break;
        }

        case PropertyScope::CACHED_VARIABLE: {
            // Items are cache entry names
            if (items.empty()) {
                interp.set_fatal_error("set_property(CACHE ...) requires at least one cache entry name");
                return;
            }

            for (const auto& entry_name : items) {
                // Setting the VALUE property updates the actual cache variable
                if (property_name == "VALUE") {
                    if (append || append_string) {
                        std::string old_val = cache_variables[entry_name];
                        if (!old_val.empty() && !value.empty()) {
                            if (append_string) {
                                cache_variables[entry_name] = old_val + value;
                            } else {
                                cache_variables[entry_name] = old_val + ";" + value;
                            }
                        } else if (!value.empty()) {
                            cache_variables[entry_name] = value;
                        }
                    } else {
                        cache_variables[entry_name] = value;
                    }
                } else {
                    // Other properties (TYPE, STRINGS, etc.) use metadata storage
                    std::string cache_prop_key = entry_name + ".__property__." + property_name;

                    if (append || append_string) {
                        std::string old_val = cache_variables[cache_prop_key];
                        if (!old_val.empty() && !value.empty()) {
                            if (append_string) {
                                cache_variables[cache_prop_key] = old_val + value;
                            } else {
                                cache_variables[cache_prop_key] = old_val + ";" + value;
                            }
                        } else if (!value.empty()) {
                            cache_variables[cache_prop_key] = value;
                        }
                    } else {
                        cache_variables[cache_prop_key] = value;
                    }
                }
            }
            break;
        }

        case PropertyScope::VARIABLE: {
            // VARIABLE scope is only for documentation
            interp.set_fatal_error("set_property(VARIABLE ...) is only for documentation, cannot set values");
            return;
        }

        case PropertyScope::INSTALL: {
            // Items are installed file paths
            if (items.empty()) {
                interp.set_fatal_error("set_property(INSTALL ...) requires at least one installed file path");
                return;
            }

            auto& install_properties = interp.get_install_properties();
            for (const auto& install_path : items) {
                // Normalize the install path
                std::filesystem::path normalized = std::filesystem::path(install_path).lexically_normal();
                std::string path_key = normalized.string();

                if (append || append_string) {
                    std::string old_val = install_properties[path_key][property_name];
                    if (!old_val.empty() && !value.empty()) {
                        if (append_string) {
                            install_properties[path_key][property_name] = old_val + value;
                        } else {
                            install_properties[path_key][property_name] = old_val + ";" + value;
                        }
                    } else if (!value.empty()) {
                        install_properties[path_key][property_name] = value;
                    }
                } else {
                    install_properties[path_key][property_name] = value;
                }
            }
            break;
        }
        }
    });

    // set_directory_properties() - Set properties on the current directory
    // Syntax: set_directory_properties(PROPERTIES prop1 value1 [prop2 value2] ...)
    interp.add_builtin("set_directory_properties", [](Interpreter& interp, const std::vector<std::string>& args) {
        if (args.empty()) {
            interp.set_fatal_error("set_directory_properties() requires at least PROPERTIES keyword");
            return;
        }

        // Find PROPERTIES keyword
        size_t props_idx = 0;
        for (size_t i = 0; i < args.size(); ++i) {
            if (args[i] == "PROPERTIES") {
                props_idx = i;
                break;
            }
        }

        if (props_idx == 0 && args[0] != "PROPERTIES") {
            interp.set_fatal_error("set_directory_properties() expected PROPERTIES keyword, got: " + args[0]);
            return;
        }

        // Property-value pairs must come after PROPERTIES
        size_t pair_start = props_idx + 1;
        size_t num_pairs = args.size() - pair_start;

        if (num_pairs == 0) {
            // No properties to set - this is valid (no-op)
            return;
        }

        if (num_pairs % 2 != 0) {
            interp.set_fatal_error(
                "set_directory_properties() requires an even number of arguments after PROPERTIES (property-value pairs)");
            return;
        }

        // Get current directory context
        auto& dir_context = interp.get_current_directory_context();

        // Set each property
        for (size_t i = pair_start; i < args.size(); i += 2) {
            const std::string& prop_name = args[i];
            const std::string& prop_value = args[i + 1];
            dir_context.properties[prop_name] = prop_value;
        }
    });

    // get_directory_property() - Get property or variable from a directory
    // Syntax: get_directory_property(<variable> [DIRECTORY <dir>] <prop-name>)
    //         get_directory_property(<variable> [DIRECTORY <dir>] DEFINITION <var-name>)
    interp.add_builtin("get_directory_property", [](Interpreter& interp, const std::vector<std::string>& args) {
        if (args.size() < 2) {
            interp.set_fatal_error("get_directory_property() requires at least 2 arguments: <variable> <prop-name>");
            return;
        }

        std::string var_name = args[0];
        std::string directory_path;
        size_t prop_idx = 1;

        // Check for optional DIRECTORY keyword
        if (args.size() >= 3 && args[1] == "DIRECTORY") {
            if (args.size() < 4) {
                interp.set_fatal_error("get_directory_property() DIRECTORY requires a path argument");
                return;
            }
            directory_path = args[2];
            prop_idx = 3;
        }

        if (prop_idx >= args.size()) {
            interp.set_fatal_error("get_directory_property() missing property name");
            return;
        }

        // Get the target directory context
        DirectoryContext* target_ctx = nullptr;
        if (!directory_path.empty()) {
            std::string current_source_dir = interp.get_variable("CMAKE_CURRENT_SOURCE_DIR");
            if (directory_path == "." || directory_path == current_source_dir) {
                target_ctx = &interp.get_current_directory_context();
            } else {
                target_ctx = interp.get_directory_context(directory_path);
                if (!target_ctx) {
                    interp.set_fatal_error("get_directory_property() unknown directory: " + directory_path);
                    return;
                }
            }
        } else {
            target_ctx = &interp.get_current_directory_context();
        }

        // Check for DEFINITION keyword (get variable instead of property)
        if (args[prop_idx] == "DEFINITION") {
            if (prop_idx + 1 >= args.size()) {
                interp.set_fatal_error("get_directory_property() DEFINITION requires a variable name");
                return;
            }
            std::string def_var_name = args[prop_idx + 1];

            // Get variable from the directory's scope
            // For now, we only support getting variables from the current directory
            // A full implementation would need to track per-directory variable scopes
            if (directory_path.empty() || directory_path == "." || directory_path == interp.get_variable("CMAKE_CURRENT_SOURCE_DIR")) {
                interp.set_variable(var_name, interp.get_variable(def_var_name));
            } else {
                // For other directories, return empty (CMake would have the full scope)
                interp.set_variable(var_name, "");
            }
            return;
        }

        std::string prop_name = args[prop_idx];

        // Check for built-in directory properties
        if (prop_name == "PARENT_DIRECTORY") {
            interp.set_variable(var_name, target_ctx->parent_dir);
            return;
        }

        // Check accumulated properties first (INCLUDE_DIRECTORIES, etc.)
        // These are the "live" values used by include_directories(), set_property(), etc.
        auto acc_it = target_ctx->accumulated.find(prop_name);
        if (acc_it != target_ctx->accumulated.end()) {
            std::string value;
            for (size_t i = 0; i < acc_it->second.size(); ++i) {
                if (i > 0) value += ";";
                value += acc_it->second[i];
            }
            interp.set_variable(var_name, value);
            return;
        }

        // Check properties map (custom directory properties)
        auto it = target_ctx->properties.find(prop_name);
        if (it != target_ctx->properties.end()) {
            interp.set_variable(var_name, it->second);
            return;
        }

        // Property not found - return empty
        interp.set_variable(var_name, "");
    });

    // get_property() - Get property values with inheritance support
    interp.add_builtin("get_property", [](Interpreter& interp, const std::vector<std::string>& args) {
        if (args.size() < 4) {
            interp.set_fatal_error("get_property() requires at least 4 arguments: <variable> <scope> <item> PROPERTY <name>");
            return;
        }

        std::string var_name = args[0];
        std::string scope_str = args[1];

        auto scope_opt = parse_property_scope(scope_str);
        if (!scope_opt) {
            interp.set_fatal_error("get_property() invalid scope: " + scope_str);
            return;
        }
        PropertyScope scope = *scope_opt;

        // Parse the rest of the arguments
        std::string item_name;
        std::string property_name;
        bool get_set = false;
        bool get_defined = false;
        bool get_brief_docs = false;
        bool get_full_docs = false;

        size_t arg_idx = 2;

        // For DIRECTORY scope, item name is optional (defaults to current directory)
        // For SOURCE, TARGET, TEST, CACHED_VARIABLE - item name is required
        // For GLOBAL and VARIABLE - no item name
        if (scope == PropertyScope::DIRECTORY) {
            // Item name is optional for DIRECTORY
            // If next arg is not PROPERTY, it's an item name
            if (arg_idx < args.size() && args[arg_idx] != "PROPERTY") { item_name = args[arg_idx++]; }
        } else if (scope != PropertyScope::GLOBAL && scope != PropertyScope::VARIABLE) {
            // For other scopes (TARGET, SOURCE, TEST, CACHED_VARIABLE), item name is required
            if (arg_idx >= args.size()) {
                interp.set_fatal_error("get_property() missing item name for scope " + scope_str);
                return;
            }
            item_name = args[arg_idx++];
        }

        // Parse DIRECTORY sub-options for SOURCE and TEST
        std::string opt_directory;
        std::string opt_target_directory;

        while (arg_idx < args.size() && args[arg_idx] != "PROPERTY") {
            if (scope == PropertyScope::SOURCE) {
                if (args[arg_idx] == "DIRECTORY" && arg_idx + 1 < args.size()) {
                    opt_directory = args[++arg_idx];
                } else if (args[arg_idx] == "TARGET_DIRECTORY" && arg_idx + 1 < args.size()) {
                    opt_target_directory = args[++arg_idx];
                }
            } else if (scope == PropertyScope::TEST) {
                if (args[arg_idx] == "DIRECTORY" && arg_idx + 1 < args.size()) { opt_directory = args[++arg_idx]; }
            } else if (scope == PropertyScope::DIRECTORY && args[arg_idx] != "PROPERTY") {
                // Skip any unrecognized args before PROPERTY for DIRECTORY scope
                ++arg_idx;
                continue;
            }
            ++arg_idx;
        }

        // Find PROPERTY keyword
        if (arg_idx >= args.size() || args[arg_idx] != "PROPERTY") {
            interp.set_fatal_error("get_property() missing PROPERTY keyword");
            return;
        }
        ++arg_idx;

        if (arg_idx >= args.size()) {
            interp.set_fatal_error("get_property() missing property name");
            return;
        }
        property_name = args[arg_idx++];

        // CMake's state-machine parser allows PROPERTY to appear multiple times;
        // the last PROPERTY <name> wins (e.g. SDL uses this pattern)
        while (arg_idx + 1 < args.size() && args[arg_idx] == "PROPERTY") {
            ++arg_idx;
            property_name = args[arg_idx++];
        }

        // Parse optional query mode
        if (arg_idx < args.size()) {
            std::string query_mode = args[arg_idx];
            if (query_mode == "SET")
                get_set = true;
            else if (query_mode == "DEFINED")
                get_defined = true;
            else if (query_mode == "BRIEF_DOCS")
                get_brief_docs = true;
            else if (query_mode == "FULL_DOCS")
                get_full_docs = true;
            else {
                interp.set_fatal_error("get_property() invalid query mode: " + query_mode);
                return;
            }
        }

        auto& property_definitions = interp.get_property_definitions();
        auto& global_properties = interp.get_global_properties();
        auto& source_properties = interp.get_source_properties();
        auto& cache_variables = interp.get_cache_variables();
        auto& directory_properties = interp.get_directory_properties();

        // Handle query modes (DEFINED, BRIEF_DOCS, FULL_DOCS)
        if (get_defined) {
            bool defined = property_definitions[scope].count(property_name) > 0;
            interp.set_variable(var_name, defined ? "1" : "0");
            return;
        }

        if (get_brief_docs || get_full_docs) {
            auto def_it = property_definitions[scope].find(property_name);
            if (def_it == property_definitions[scope].end()) {
                interp.set_variable(var_name, property_name + "-NOTFOUND");
            } else {
                interp.set_variable(var_name, get_brief_docs ? def_it->second.brief_docs : def_it->second.full_docs);
            }
            return;
        }

        // Helper to check if property is inherited
        auto is_inherited = [&]() -> bool {
            auto def_it = property_definitions[scope].find(property_name);
            return def_it != property_definitions[scope].end() && def_it->second.inherited;
        };

        // Get the property value based on scope
        std::string value;
        bool value_found = false;

        switch (scope) {
        case PropertyScope::GLOBAL: {
            auto it = global_properties.find(property_name);
            if (it != global_properties.end()) {
                value = it->second;
                value_found = true;
            }
            break;
        }

        case PropertyScope::DIRECTORY: {
            // Determine which directory context to use
            DirectoryContext* target_ctx = nullptr;
            if (!item_name.empty()) {
                // Explicit directory path provided
                std::string current_source_dir = interp.get_variable("CMAKE_CURRENT_SOURCE_DIR");
                if (item_name == "." || item_name == current_source_dir) {
                    target_ctx = &interp.get_current_directory_context();
                } else {
                    target_ctx = interp.get_directory_context(item_name);
                    if (!target_ctx) {
                        // Unknown directory - return empty
                        interp.set_variable(var_name, "");
                        return;
                    }
                }
            } else {
                // No directory specified - use current directory
                target_ctx = &interp.get_current_directory_context();
            }

            // Check accumulated properties first (INCLUDE_DIRECTORIES, etc.)
            // These are the "live" values set by include_directories(), add_definitions(), etc.
            // Must be checked before target_ctx->properties so that get_property(DIRECTORY)
            // and get_directory_property() return the same values.
            auto acc_it = target_ctx->accumulated.find(property_name);
            if (acc_it != target_ctx->accumulated.end()) {
                for (size_t i = 0; i < acc_it->second.size(); ++i) {
                    if (i > 0) value += ";";
                    value += acc_it->second[i];
                }
                value_found = true;
            }

            if (!value_found) {
                // Check directory properties map
                auto it = target_ctx->properties.find(property_name);
                if (it != target_ctx->properties.end()) {
                    value = it->second;
                    value_found = true;
                }
            }

            // If not found and property is inherited, check parent directories
            if (!value_found && is_inherited()) {
                std::string parent_dir = target_ctx->parent_dir;
                while (!parent_dir.empty()) {
                    DirectoryContext* parent_ctx = interp.get_directory_context(parent_dir);
                    if (!parent_ctx) break;

                    auto parent_it = parent_ctx->properties.find(property_name);
                    if (parent_it != parent_ctx->properties.end()) {
                        value = parent_it->second;
                        value_found = true;
                        break;
                    }
                    parent_dir = parent_ctx->parent_dir;
                }

                // If still not found, check GLOBAL scope
                if (!value_found) {
                    auto global_it = global_properties.find(property_name);
                    if (global_it != global_properties.end()) {
                        value = global_it->second;
                        value_found = true;
                    }
                }
            }
            break;
        }

        case PropertyScope::TARGET: {
            auto target = get_target_from_name(interp, item_name, "get_property");
            if (!target) {
                interp.set_variable(var_name, "");
                return;
            }

            // Handle special built-in target properties
            if (property_name == "TYPE") {
                switch (target->get_type()) {
                case TargetType::EXECUTABLE:
                    value = "EXECUTABLE";
                    break;
                case TargetType::SHARED_LIBRARY:
                    value = "SHARED_LIBRARY";
                    break;
                case TargetType::STATIC_LIBRARY:
                    value = "STATIC_LIBRARY";
                    break;
                case TargetType::OBJECT_LIBRARY:
                    value = "OBJECT_LIBRARY";
                    break;
                case TargetType::INTERFACE_LIBRARY:
                    value = "INTERFACE_LIBRARY";
                    break;
                case TargetType::CUSTOM:
                    value = "UTILITY";
                    break;
                }
                value_found = true;
            } else if (property_name == "NAME") {
                value = target->get_name();
                value_found = true;
            } else if (property_name == "SOURCE_DIR") {
                value = target->get_source_dir();
                value_found = true;
            } else if (property_name == "BINARY_DIR") {
                value = target->get_binary_dir();
                value_found = true;
            } else if (property_name == "IMPORTED") {
                value = target->is_imported() ? "TRUE" : "FALSE";
                value_found = true;
            } else if (property_name == "IMPORTED_LOCATION") {
                value = target->get_imported_location();
                value_found = !value.empty();
            } else {
                // Try generic property
                value = target->get_property(property_name);
                value_found = !value.empty();
            }

            // If not found and inherited, check DIRECTORY scope
            if (!value_found && is_inherited()) {
                auto dir_it = directory_properties.find(property_name);
                if (dir_it != directory_properties.end()) {
                    value = dir_it->second;
                    value_found = true;
                }
            }
            break;
        }

        case PropertyScope::SOURCE: {
            std::string abs_source = resolve_source_path(interp, item_name, opt_directory.empty() ? nullptr : &opt_directory,
                                                         opt_target_directory.empty() ? nullptr : &opt_target_directory);
            if (abs_source.empty()) {
                interp.set_variable(var_name, "");
                return;
            }

            auto source_it = source_properties.find(abs_source);
            if (source_it != source_properties.end()) {
                auto prop_it = source_it->second.find(property_name);
                if (prop_it != source_it->second.end()) {
                    value = prop_it->second;
                    value_found = true;
                }
            }

            // If not found and inherited, check DIRECTORY scope
            if (!value_found && is_inherited()) {
                auto dir_it = directory_properties.find(property_name);
                if (dir_it != directory_properties.end()) {
                    value = dir_it->second;
                    value_found = true;
                }
            }
            break;
        }

        case PropertyScope::TEST: {
            auto* test = get_test_from_name(interp, item_name);
            if (!test) {
                interp.set_variable(var_name, "");
                return;
            }

            auto it = test->properties.find(property_name);
            if (it != test->properties.end()) {
                value = it->second;
                value_found = true;
            }

            // If not found and inherited, check DIRECTORY scope
            if (!value_found && is_inherited()) {
                auto dir_it = directory_properties.find(property_name);
                if (dir_it != directory_properties.end()) {
                    value = dir_it->second;
                    value_found = true;
                }
            }
            break;
        }

        case PropertyScope::CACHED_VARIABLE: {
            // Handle special built-in cache properties
            if (property_name == "TYPE") {
                // Check if the cache variable exists
                auto cache_it = cache_variables.find(item_name);
                if (cache_it != cache_variables.end()) {
                    // For now, all cache variables are treated as STRING type
                    // CMake has BOOL, FILEPATH, PATH, STRING, INTERNAL types
                    value = "STRING";
                    value_found = true;
                }
            } else if (property_name == "VALUE") {
                // Built-in VALUE property returns the cache variable's value
                auto cache_it = cache_variables.find(item_name);
                if (cache_it != cache_variables.end()) {
                    value = cache_it->second;
                    value_found = true;
                }
            } else if (property_name == "HELPSTRING") {
                // Built-in HELPSTRING property (we don't track this currently)
                value = "";
                value_found = true;
            } else if (property_name == "ADVANCED") {
                // Built-in ADVANCED property (we don't track this currently)
                value = "0";
                value_found = true;
            } else {
                // Custom cache property
                std::string cache_prop_key = item_name + ".__property__." + property_name;
                auto it = cache_variables.find(cache_prop_key);
                if (it != cache_variables.end()) {
                    value = it->second;
                    value_found = true;
                }
            }
            break;
        }

        case PropertyScope::VARIABLE: {
            // VARIABLE scope only has documentation
            interp.set_variable(var_name, "");
            return;
        }

        case PropertyScope::INSTALL: {
            // Item name is the installed file path
            if (item_name.empty()) {
                interp.set_fatal_error("get_property(INSTALL ...) requires an installed file path");
                return;
            }

            auto& install_properties = interp.get_install_properties();
            std::filesystem::path normalized = std::filesystem::path(item_name).lexically_normal();
            std::string path_key = normalized.string();

            auto path_it = install_properties.find(path_key);
            if (path_it != install_properties.end()) {
                auto prop_it = path_it->second.find(property_name);
                if (prop_it != path_it->second.end()) {
                    value = prop_it->second;
                    value_found = true;
                }
            }
            break;
        }
        }

        // Handle SET query mode
        if (get_set) {
            interp.set_variable(var_name, value_found ? "1" : "0");
            return;
        }

        // Return the value, or unset the variable if property was not found
        // (CMake leaves the variable undefined when the property doesn't exist)
        if (value_found) {
            interp.set_variable(var_name, value);
        } else {
            interp.unset_variable(var_name);
        }
    });
}

} // namespace kiln
