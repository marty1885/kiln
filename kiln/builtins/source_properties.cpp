#include "registry.hpp"
#include "../interperter.hpp"
#include "../command_parser.hpp"
#include "../CMakeArray.hpp"
#include "../path.hpp"

namespace kiln {

void register_source_properties_builtins(Interpreter& interp) {

    // set_source_files_properties - Set properties on source files
    // CMake signature:
    // set_source_files_properties(<files>...
    //     [DIRECTORY <dirs>...] [TARGET_DIRECTORY <targets>...]
    //     PROPERTIES <prop1> <value1> [<prop2> <value2>] ...)
    interp.add_builtin("set_source_files_properties", [](Interpreter& interp, const std::vector<std::string>& args) {
        if (args.empty()) {
            interp.set_fatal_error("set_source_files_properties() requires arguments");
            return;
        }

        // Parse files until we hit a keyword
        std::vector<std::string> files;
        std::vector<std::string> directories;
        std::vector<std::string> target_directories;
        size_t i = 0;

        // Collect files until DIRECTORY, TARGET_DIRECTORY, or PROPERTIES
        while (i < args.size() && args[i] != "DIRECTORY" && args[i] != "TARGET_DIRECTORY" && args[i] != "PROPERTIES") {
            files.push_back(args[i]);
            ++i;
        }

        if (files.empty()) {
            // Undocumented CMake behavior: the signature requires <files>..., but
            // CMake silently accepts zero files (e.g. when a variable expands to
            // empty) and treats it as a no-op. We match that behavior.
            // interp.print_message("WARNING",
            //     "set_source_files_properties() called with no source files "
            //     "(undocumented CMake behavior, accepted for compatibility)", true);
            return;
        }

        // Parse optional DIRECTORY and TARGET_DIRECTORY
        while (i < args.size() && args[i] != "PROPERTIES") {
            if (args[i] == "DIRECTORY") {
                ++i;
                while (i < args.size() && args[i] != "TARGET_DIRECTORY" && args[i] != "PROPERTIES" && args[i] != "DIRECTORY") {
                    directories.push_back(args[i]);
                    ++i;
                }
            } else if (args[i] == "TARGET_DIRECTORY") {
                ++i;
                while (i < args.size() && args[i] != "DIRECTORY" && args[i] != "PROPERTIES" && args[i] != "TARGET_DIRECTORY") {
                    target_directories.push_back(args[i]);
                    ++i;
                }
            } else {
                ++i;
            }
        }

        if (i >= args.size() || args[i] != "PROPERTIES") {
            interp.set_fatal_error("set_source_files_properties called with illegal arguments, maybe missing a PROPERTIES specifier?");
            return;
        }
        ++i;

        // Parse property key-value pairs
        if ((args.size() - i) % 2 != 0) {
            interp.set_fatal_error("set_source_files_properties() PROPERTIES must be key-value pairs");
            return;
        }

        std::vector<std::pair<std::string, std::string>> properties;
        while (i + 1 < args.size()) {
            properties.emplace_back(args[i], args[i + 1]);
            i += 2;
        }

        if (properties.empty()) {
            interp.set_fatal_error("set_source_files_properties() requires at least one property");
            return;
        }

        // Determine base directories for resolving relative paths
        std::vector<std::string> base_dirs;

        if (!target_directories.empty()) {
            // Use target directories
            for (const auto& target_name : target_directories) {
                auto* target = interp.find_target(target_name);
                if (!target) {
                    interp.set_fatal_error("set_source_files_properties() unknown target: " + target_name);
                    return;
                }
                base_dirs.push_back(target->get_source_dir());
            }
        } else if (!directories.empty()) {
            // Use specified directories
            std::string current_source_dir = interp.get_variable("CMAKE_CURRENT_SOURCE_DIR");
            for (const auto& dir : directories) { base_dirs.push_back(Path::make_absolute_and_normal(current_source_dir, dir)); }
        } else {
            // Default to current source directory
            base_dirs.push_back(interp.get_variable("CMAKE_CURRENT_SOURCE_DIR"));
        }

        // Apply properties to each file in each base directory
        auto& source_properties = interp.get_source_properties();

        // CMP0118 (CMake 3.20+): When GENERATED TRUE is being set, relative
        // paths resolve against CMAKE_CURRENT_BINARY_DIR instead of SOURCE_DIR.
        // The GENERATED property is then globally visible (which we get for free
        // since source_properties is a flat map keyed by absolute path).
        bool setting_generated = false;
        for (const auto& [prop_name, prop_value] : properties) {
            if (prop_name == "GENERATED" && !Interpreter::is_falsy(prop_value)) {
                setting_generated = true;
                break;
            }
        }

        for (const auto& base_dir : base_dirs) {
            for (const auto& file : files) {
                std::string abs_path;
                if (setting_generated && !Path(file).is_absolute()) {
                    // CMP0118: generated files resolve against binary dir
                    std::string binary_dir = interp.get_variable("CMAKE_CURRENT_BINARY_DIR");
                    abs_path = Path::make_absolute_and_normal(binary_dir, file);
                } else {
                    abs_path = Path::make_absolute_and_normal(base_dir, file);
                }

                for (const auto& [prop_name, prop_value] : properties) { source_properties[abs_path][prop_name] = prop_value; }
            }
        }
    });

    // get_source_file_property - Get a property from a source file
    // CMake signature:
    // get_source_file_property(<variable> <file>
    //     [DIRECTORY <dir> | TARGET_DIRECTORY <target>]
    //     <property>)
    interp.add_builtin("get_source_file_property", [](Interpreter& interp, const std::vector<std::string>& args) {
        if (args.size() < 3) {
            interp.set_fatal_error("get_source_file_property() requires at least 3 arguments: <variable> <file> <property>");
            return;
        }

        std::string variable = args[0];
        std::string file = args[1];
        std::string property_name;
        std::string opt_directory;
        std::string opt_target_directory;

        size_t i = 2;
        while (i < args.size()) {
            if (args[i] == "DIRECTORY" && i + 1 < args.size()) {
                opt_directory = args[++i];
                ++i;
            } else if (args[i] == "TARGET_DIRECTORY" && i + 1 < args.size()) {
                opt_target_directory = args[++i];
                ++i;
            } else {
                // Must be the property name
                property_name = args[i];
                ++i;
                break;
            }
        }

        if (property_name.empty()) {
            interp.set_fatal_error("get_source_file_property() requires a property name");
            return;
        }

        // Determine base directory
        std::string base_dir;
        if (!opt_target_directory.empty()) {
            auto* target = interp.find_target(opt_target_directory);
            if (!target) {
                interp.set_fatal_error("get_source_file_property() unknown target: " + opt_target_directory);
                return;
            }
            base_dir = target->get_source_dir();
        } else if (!opt_directory.empty()) {
            base_dir = Path::make_absolute_and_normal(interp.get_variable("CMAKE_CURRENT_SOURCE_DIR"), opt_directory);
        } else {
            base_dir = interp.get_variable("CMAKE_CURRENT_SOURCE_DIR");
        }

        // Resolve file path
        std::string abs_path = Path::make_absolute_and_normal(base_dir, file);

        // Look up the property
        const auto& source_properties = interp.get_source_properties();
        auto source_it = source_properties.find(abs_path);
        if (source_it != source_properties.end()) {
            auto prop_it = source_it->second.find(property_name);
            if (prop_it != source_it->second.end()) {
                interp.set_variable(variable, prop_it->second);
                return;
            }
        }

        // CMP0118: GENERATED is globally visible and stored under binary-dir paths.
        // If source-dir lookup missed and path is relative, try binary dir.
        if (property_name == "GENERATED" && !Path(file).is_absolute()) {
            std::string binary_dir = interp.get_variable("CMAKE_CURRENT_BINARY_DIR");
            std::string binary_abs = Path::make_absolute_and_normal(binary_dir, file);
            if (binary_abs != abs_path) {
                auto bin_it = source_properties.find(binary_abs);
                if (bin_it != source_properties.end()) {
                    auto prop_it = bin_it->second.find(property_name);
                    if (prop_it != bin_it->second.end()) {
                        interp.set_variable(variable, prop_it->second);
                        return;
                    }
                }
            }
        }

        // Property not found - CMake returns literal "NOTFOUND" for
        // get_source_file_property (unlike get_target_property which uses
        // <PROP>-NOTFOUND).
        interp.set_variable(variable, "NOTFOUND");
    });
}

} // namespace kiln
