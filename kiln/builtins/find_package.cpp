#include "registry.hpp"
#include "find_helpers.hpp"
#include "../interperter.hpp"
#include "../policies.hpp"
#include "../command_parser.hpp"
#include "../CMakeArray.hpp"
#include "../utils.hpp"
#include <filesystem>
#include <algorithm>
#include <vector>
#include <string>
#include <cctype>
#include <cstdlib>

namespace kiln {

namespace {

// Check if a directory name matches a package name (case-insensitive prefix match)
// e.g., "Botan-3.10.0" matches package "Botan" or "botan"
bool directory_matches_package(const std::string& dir_name, const std::string& package_name) {
    std::string lower_dir = kiln::to_lower(dir_name);
    std::string lower_pkg = kiln::to_lower(package_name);

    // Exact match
    if (lower_dir == lower_pkg) return true;

    // Prefix match followed by a separator (-, _, or digit for versioned dirs)
    if (lower_dir.length() > lower_pkg.length() && lower_dir.substr(0, lower_pkg.length()) == lower_pkg) {
        char next_char = lower_dir[lower_pkg.length()];
        if (next_char == '-' || next_char == '_' || std::isdigit(next_char)) { return true; }
    }

    return false;
}

struct VersionComponents {
    std::string full;
    std::string major;
    std::string minor;
    std::string patch;
    std::string tweak;
    std::string count;
};

VersionComponents parse_version(const std::string& version) {
    VersionComponents v;
    v.full = version;

    if (version.empty()) {
        v.count = "0";
        return v;
    }

    std::vector<std::string> parts;
    size_t start = 0;
    size_t end = version.find('.');
    while (end != std::string::npos) {
        parts.push_back(version.substr(start, end - start));
        start = end + 1;
        end = version.find('.', start);
    }
    parts.push_back(version.substr(start));

    v.count = std::to_string(parts.size());
    if (parts.size() > 0) v.major = parts[0];
    if (parts.size() > 1) v.minor = parts[1];
    if (parts.size() > 2) v.patch = parts[2];
    if (parts.size() > 3) v.tweak = parts[3];

    return v;
}

} // namespace

void register_find_package_builtins(Interpreter& interp) {
    interp.add_builtin("find_package", [](Interpreter& interp, const std::vector<std::string>& args) {
        CommandParser parser("find_package");
        std::string package_name;
        std::string version;
        bool required = false;
        bool config = false;
        bool module_only = false;
        bool no_module = false;
        bool quiet = false;
        std::vector<std::string> components;
        std::vector<std::string> bare_components;
        std::vector<std::string> optional_components;
        std::vector<std::string> hints;
        std::vector<std::string> paths;
        std::vector<std::string> path_suffixes;
        std::vector<std::string> names;
        std::vector<std::string> configs;
        bool no_default_path = false;
        bool no_package_root_path = false;
        bool no_cmake_path = false;
        bool no_cmake_environment_path = false;
        bool no_system_environment_path = false;
        bool no_cmake_package_registry = false;
        bool no_cmake_system_path = false;
        bool no_cmake_system_package_registry = false;
        bool no_cmake_install_prefix = false;
        bool exact = false;
        bool global_flag = false;

        // Silently accepted but ignored flags
        bool no_policy_scope = false;
        bool bypass_provider = false;
        bool cmake_find_root_path_both = false;
        bool only_cmake_find_root_path = false;
        bool no_cmake_find_root_path = false;
        std::string registry_view; // VALUE keyword, silently ignored

        parser.positional(package_name, "package name", true);
        parser.positional(version, "version", false);
        parser.flag("REQUIRED", required);
        parser.flag("CONFIG", config);
        parser.flag("MODULE", module_only);
        parser.flag("NO_MODULE", no_module);
        parser.flag("QUIET", quiet);
        parser.flag("NO_DEFAULT_PATH", no_default_path);
        parser.flag("NO_PACKAGE_ROOT_PATH", no_package_root_path);
        parser.flag("NO_CMAKE_PATH", no_cmake_path);
        parser.flag("NO_CMAKE_ENVIRONMENT_PATH", no_cmake_environment_path);
        parser.flag("NO_SYSTEM_ENVIRONMENT_PATH", no_system_environment_path);
        parser.flag("NO_CMAKE_PACKAGE_REGISTRY", no_cmake_package_registry);
        parser.flag("NO_CMAKE_SYSTEM_PATH", no_cmake_system_path);
        parser.flag("NO_CMAKE_SYSTEM_PACKAGE_REGISTRY", no_cmake_system_package_registry);
        parser.flag("NO_CMAKE_INSTALL_PREFIX", no_cmake_install_prefix);
        parser.flag("EXACT", exact);
        parser.flag("GLOBAL", global_flag);
        parser.flag("NO_POLICY_SCOPE", no_policy_scope);
        parser.flag("BYPASS_PROVIDER", bypass_provider);
        parser.flag("CMAKE_FIND_ROOT_PATH_BOTH", cmake_find_root_path_both);
        parser.flag("ONLY_CMAKE_FIND_ROOT_PATH", only_cmake_find_root_path);
        parser.flag("NO_CMAKE_FIND_ROOT_PATH", no_cmake_find_root_path);
        parser.list("COMPONENTS", components);
        parser.list("OPTIONAL_COMPONENTS", optional_components);
        parser.list("HINTS", hints);
        parser.list("PATHS", paths);
        parser.list("PATH_SUFFIXES", path_suffixes);
        parser.list("NAMES", names);
        parser.list("CONFIGS", configs);
        parser.value("REGISTRY_VIEW", registry_view);
        // CMake treats bare args as implicit components
        // e.g. find_package(Qt5 REQUIRED Gui Widgets QUIET)
        parser.unparsed(bare_components);

        PARSE_OR_RETURN(parser, interp, args);

        // Merge bare components into components list
        for (auto& c : bare_components) { components.push_back(std::move(c)); }

        // --- Step 1: CMAKE_DISABLE_FIND_PACKAGE_<Name> ---
        std::string disable_var = interp.get_variable("CMAKE_DISABLE_FIND_PACKAGE_" + package_name);
        if (!disable_var.empty() && !interp.is_falsy(disable_var)) {
            if (required) {
                interp.set_fatal_error("find_package(" + package_name + ") is REQUIRED but CMAKE_DISABLE_FIND_PACKAGE_" + package_name
                                       + " is TRUE");
                return;
            }
            // Silently skip
            interp.set_variable(package_name + "_FOUND", "OFF");
            return;
        }

        // --- Step 2: CMAKE_REQUIRE_FIND_PACKAGE_<Name> / CMAKE_FIND_REQUIRED ---
        std::string require_var = interp.get_variable("CMAKE_REQUIRE_FIND_PACKAGE_" + package_name);
        if (!require_var.empty() && !interp.is_falsy(require_var)) { required = true; }
        std::string find_required_var = interp.get_variable("CMAKE_FIND_REQUIRED");
        if (!find_required_var.empty() && !interp.is_falsy(find_required_var)) { required = true; }

        std::string found_var = package_name + "_FOUND";

        // --- Step 3: FindVarGuard ---
        // Save and restore _FIND_* input variables so they don't leak between
        // repeated find_package calls for the same package.
        struct FindVarGuard {
            Interpreter& interp;
            std::string pkg;
            std::vector<std::pair<std::string, std::string>> saved;

            FindVarGuard(Interpreter& i, const std::string& p) : interp(i), pkg(p) {
                for (const char* suffix : {"_FIND_REQUIRED", "_FIND_QUIETLY", "_FIND_COMPONENTS"}) {
                    std::string name = pkg + suffix;
                    saved.emplace_back(name, interp.get_variable(name));
                }
                saved.emplace_back("CMAKE_FIND_PACKAGE_NAME", interp.get_variable("CMAKE_FIND_PACKAGE_NAME"));
                // Save per-component FIND_REQUIRED vars
                std::string comps = interp.get_variable(pkg + "_FIND_COMPONENTS");
                if (!comps.empty()) {
                    for (auto c : CMakeArrayIterator(comps)) {
                        std::string n = std::string(pkg) + "_FIND_REQUIRED_" + std::string(c);
                        saved.emplace_back(n, interp.get_variable(n));
                    }
                }
            }

            ~FindVarGuard() {
                for (const auto& [name, value] : saved) {
                    if (value.empty()) {
                        interp.unset_variable(name);
                    } else {
                        interp.set_variable(name, value);
                    }
                }
            }
        } find_var_guard(interp, package_name);

        // Set CMAKE_FIND_PACKAGE_NAME so Find modules know which package is being searched
        interp.set_variable("CMAKE_FIND_PACKAGE_NAME", package_name);

        // Set component-related variables before calling Find module or Config file
        std::vector<std::string> all_components = components;
        all_components.insert(all_components.end(), optional_components.begin(), optional_components.end());

        {
            std::string components_str;
            for (size_t i = 0; i < all_components.size(); ++i) {
                if (i > 0) components_str += ";";
                components_str += all_components[i];
            }
            interp.set_variable(package_name + "_FIND_COMPONENTS", components_str);

            for (const auto& comp : components) { interp.set_variable(package_name + "_FIND_REQUIRED_" + comp, "TRUE"); }
        }

        if (required) { interp.set_variable(package_name + "_FIND_REQUIRED", "TRUE"); }
        if (quiet) { interp.set_variable(package_name + "_FIND_QUIETLY", "TRUE"); }

        // Set version-related variables
        VersionComponents v_req = parse_version(version);
        if (!version.empty()) {
            interp.set_variable(package_name + "_FIND_VERSION", v_req.full);
            interp.set_variable(package_name + "_FIND_VERSION_MAJOR", v_req.major);
            interp.set_variable(package_name + "_FIND_VERSION_MINOR", v_req.minor);
            interp.set_variable(package_name + "_FIND_VERSION_PATCH", v_req.patch);
            interp.set_variable(package_name + "_FIND_VERSION_TWEAK", v_req.tweak);
            interp.set_variable(package_name + "_FIND_VERSION_COUNT", v_req.count);
            interp.set_variable(package_name + "_FIND_VERSION_EXACT", exact ? "TRUE" : "FALSE");
        }

        // Helper to validate that all required components were found
        auto validate_components = [&]() -> bool {
            if (components.empty()) { return true; }

            std::string pkg_found = interp.get_variable(found_var);
            if (interp.is_falsy(pkg_found)) { return false; }

            std::vector<std::string> missing_components;
            for (const auto& comp : components) {
                std::string comp_found_var = package_name + "_" + comp + "_FOUND";
                std::string comp_found = interp.get_variable(comp_found_var);

                if (!comp_found.empty() && interp.is_falsy(comp_found)) { missing_components.push_back(comp); }
            }

            if (!missing_components.empty()) {
                std::string missing_str;
                for (size_t i = 0; i < missing_components.size(); ++i) {
                    if (i > 0) missing_str += " ";
                    missing_str += missing_components[i];
                }

                if (required) {
                    interp.set_fatal_error("Could not find package " + package_name + " (missing required components: " + missing_str
                                           + ")");
                } else if (!quiet) {
                    interp.print_message("STATUS", "Package " + package_name + " missing components: " + missing_str);
                }

                interp.set_variable(found_var, "OFF");
                return false;
            }

            return true;
        };

        // --- Determine search mode ---
        // CMP0167: Boost uses config-first (BoostConfig.cmake) instead of
        // the legacy FindBoost.cmake module. All other packages use module-first.
        bool prefer_config = (package_name == "Boost" && interp.get_policy(CMakePolicy::CMP0167) == PolicyState::NEW);

        // CMAKE_FIND_PACKAGE_PREFER_CONFIG overrides for all packages
        std::string prefer_config_var = interp.get_variable("CMAKE_FIND_PACKAGE_PREFER_CONFIG");
        if (!prefer_config_var.empty() && !interp.is_falsy(prefer_config_var)) { prefer_config = true; }

        bool try_module = !config && !no_module;
        bool try_config = !module_only;

        // --- Module mode search helpers ---
        auto try_find_module = [&](const std::filesystem::path& found_module) -> bool {
            auto res = interp.include_file(found_module.string());
            if (!res) {
                interp.set_fatal_error(res.error());
                return true;
            }

            std::string found = interp.get_variable(found_var);
            // CMake also checks <PACKAGENAME>_FOUND (uppercase) as a fallback
            if (interp.is_falsy(found)) {
                std::string upper_name = package_name;
                for (auto& c : upper_name) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                std::string upper_found_var = upper_name + "_FOUND";
                if (upper_found_var != found_var) {
                    std::string upper_found = interp.get_variable(upper_found_var);
                    if (!interp.is_falsy(upper_found)) {
                        found = upper_found;
                        interp.set_variable(found_var, found);
                    }
                }
            }
            if (interp.is_falsy(found)) {
                if (required) {
                    interp.set_fatal_error("Could not find package " + package_name + " (missing: " + package_name + "_FOUND)");
                } else if (!quiet) {
                    interp.print_message("STATUS", "Could not find package " + package_name + " (Module mode)");
                }
                return true;
            }

            validate_components();
            return true;
        };

        // Search CMAKE_MODULE_PATH for user-provided Find modules
        auto try_user_find_modules = [&]() -> bool {
            if (!try_module) return false;
            std::string module_path_var = interp.get_variable("CMAKE_MODULE_PATH");
            if (module_path_var.empty()) return false;

            std::string module_filename = "Find" + package_name + ".cmake";
            for (auto path_sv : CMakeArrayIterator(module_path_var)) {
                std::filesystem::path path(path_sv);
                if (interp.cached_file_exists(path.string(), module_filename)) {
                    if (try_find_module(path / module_filename)) return true;
                }
            }
            return false;
        };

        // Search system paths for Find modules
        auto try_system_find_modules = [&]() -> bool {
            if (!try_module) return false;
            std::vector<std::string> system_modules = {
                "/usr/share/cmake/Modules",
                "/usr/local/share/cmake/Modules",
                "/usr/lib/cmake/Modules",
            };
            if (auto& extra = cmake_extra_modules_root(); !extra.empty()) system_modules.push_back(extra + "/Modules");
            if (auto& triplet = gnu_arch_triplet(); !triplet.empty()) {
                system_modules.push_back("/usr/lib/" + triplet + "/cmake/Modules");
            }

            std::string module_filename = "Find" + package_name + ".cmake";
            for (const auto& path : system_modules) {
                if (interp.cached_file_exists(path, module_filename)) {
                    if (try_find_module(std::filesystem::path(path) / module_filename)) return true;
                }
            }
            return false;
        };

        // --- Module mode: user CMAKE_MODULE_PATH always first ---
        if (try_user_find_modules()) return;

        // --- Module mode before config (unless prefer_config) ---
        if (!prefer_config && try_system_find_modules()) return;

        // --- Config Mode ---
        std::string dir_var = package_name + "_DIR";
        std::filesystem::path found_path;

        if (try_config) {
            // Build the list of search names
            // If NAMES given, use those. Otherwise use package_name.
            std::vector<std::string> search_names;
            if (!names.empty()) {
                search_names = names;
            } else {
                search_names.push_back(package_name);
            }

            // Build config file candidates for each name
            // If CONFIGS given, use those exact filenames. Otherwise generate standard patterns.
            struct Candidate {
                std::string config;
                std::string version;
            };

            auto make_candidates = [&]() -> std::vector<Candidate> {
                std::vector<Candidate> candidates;
                if (!configs.empty()) {
                    // Use exact config filenames provided
                    for (const auto& cfg : configs) {
                        // Derive version filename: replace .cmake with Version.cmake
                        std::string ver = cfg;
                        if (ver.size() > 6 && ver.substr(ver.size() - 6) == ".cmake") {
                            ver = ver.substr(0, ver.size() - 6) + "Version.cmake";
                        }
                        candidates.push_back({cfg, ver});
                    }
                } else {
                    for (const auto& name : search_names) {
                        std::string lower = kiln::to_lower(name);
                        candidates.push_back({name + "Config.cmake", name + "ConfigVersion.cmake"});
                        candidates.push_back({lower + "-config.cmake", lower + "-config-version.cmake"});
                    }
                }
                return candidates;
            };

            auto candidates = make_candidates();

            // Track considered configs/versions
            std::vector<std::string> considered_configs;
            std::vector<std::string> considered_versions;

            // Collect search prefixes in proper CMake 9-step order
            std::vector<std::filesystem::path> search_paths;

            auto add_with_suffixes = [&](const std::filesystem::path& base) {
                search_paths.push_back(base);
                for (const auto& suffix : path_suffixes) { search_paths.push_back(base / suffix); }
            };

            // Expand a prefix to UNIX convention subdirectories
            auto expand_prefix = [&](const std::filesystem::path& prefix) {
                for (const auto& name : search_names) {
                    std::string lower = kiln::to_lower(name);
                    // <prefix>/<name> (direct subdirectory)
                    add_with_suffixes(prefix / name);
                    if (lower != name) add_with_suffixes(prefix / lower);
                    // <prefix>/<name>/cmake
                    add_with_suffixes(prefix / name / "cmake");
                    if (lower != name) add_with_suffixes(prefix / lower / "cmake");
                    // <prefix>/lib/cmake/<name>
                    add_with_suffixes(prefix / "lib" / "cmake" / name);
                    if (lower != name) add_with_suffixes(prefix / "lib" / "cmake" / lower);
                    // <prefix>/lib/<name>
                    add_with_suffixes(prefix / "lib" / name);
                    if (lower != name) add_with_suffixes(prefix / "lib" / lower);
                    // <prefix>/share/cmake/<name>
                    add_with_suffixes(prefix / "share" / "cmake" / name);
                    if (lower != name) add_with_suffixes(prefix / "share" / "cmake" / lower);
                    // <prefix>/share/<name>
                    add_with_suffixes(prefix / "share" / name);
                    if (lower != name) add_with_suffixes(prefix / "share" / lower);
                }
                // Also check the prefix root itself
                add_with_suffixes(prefix);
            };

            // Step 0: <PackageName>_DIR hint (always checked, even with NO_DEFAULT_PATH)
            {
                std::string hint_dir = interp.get_variable(dir_var);
                if (!hint_dir.empty()) { add_with_suffixes(hint_dir); }
            }

            // Step 1: Package root paths (<PackageName>_ROOT, <PACKAGENAME>_ROOT)
            {
                bool use_package_root = !no_default_path && !no_package_root_path;
                if (use_package_root) {
                    std::string use_var = interp.get_variable("CMAKE_FIND_USE_PACKAGE_ROOT_PATH");
                    if (!use_var.empty() && interp.is_falsy(use_var)) { use_package_root = false; }
                }
                if (use_package_root) {
                    std::string upper_name = package_name;
                    for (auto& c : upper_name) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

                    std::vector<std::string> root_prefixes;
                    auto maybe_add = [&](const std::string& val) {
                        if (!val.empty()) root_prefixes.push_back(val);
                    };
                    maybe_add(interp.get_variable(package_name + "_ROOT"));
                    if (upper_name != package_name) maybe_add(interp.get_variable(upper_name + "_ROOT"));
                    if (const char* env = std::getenv((package_name + "_ROOT").c_str())) maybe_add(env);
                    if (upper_name != package_name) {
                        if (const char* env = std::getenv((upper_name + "_ROOT").c_str())) maybe_add(env);
                    }

                    for (const auto& root : root_prefixes) { expand_prefix(root); }
                }
            }

            // Step 2: CMAKE_PREFIX_PATH (variable), CMAKE_FRAMEWORK_PATH, CMAKE_APPBUNDLE_PATH
            if (!no_default_path && !no_cmake_path) {
                std::string use_var = interp.get_variable("CMAKE_FIND_USE_CMAKE_PATH");
                bool use = use_var.empty() || !interp.is_falsy(use_var);
                if (use) {
                    std::string prefix_path = interp.get_variable("CMAKE_PREFIX_PATH");
                    for (auto p : CMakeArrayIterator(prefix_path)) { expand_prefix(std::string(p)); }
                }
            }

            // Step 3: CMAKE_PREFIX_PATH (environment), <PackageName>_DIR (environment)
            if (!no_default_path && !no_cmake_environment_path) {
                std::string use_var = interp.get_variable("CMAKE_FIND_USE_CMAKE_ENVIRONMENT_PATH");
                bool use = use_var.empty() || !interp.is_falsy(use_var);
                if (use) {
                    if (const char* env = std::getenv("CMAKE_PREFIX_PATH")) {
                        // Environment CMAKE_PREFIX_PATH is colon-separated on UNIX
                        std::string env_str(env);
                        size_t start = 0;
                        size_t end;
                        while ((end = env_str.find(':', start)) != std::string::npos) {
                            std::string p = env_str.substr(start, end - start);
                            if (!p.empty()) expand_prefix(p);
                            start = end + 1;
                        }
                        std::string p = env_str.substr(start);
                        if (!p.empty()) expand_prefix(p);
                    }

                    // <PackageName>_DIR from environment
                    if (const char* env = std::getenv((package_name + "_DIR").c_str())) { add_with_suffixes(env); }
                }
            }

            // Step 4: HINTS
            for (const auto& h : hints) { add_with_suffixes(h); }

            // Step 5: System environment PATH (convert /bin, /sbin to parent prefix)
            if (!no_default_path && !no_system_environment_path) {
                std::string use_var = interp.get_variable("CMAKE_FIND_USE_SYSTEM_ENVIRONMENT_PATH");
                bool use = use_var.empty() || !interp.is_falsy(use_var);
                if (use) {
                    if (const char* env_path = std::getenv("PATH")) {
                        std::string path_str(env_path);
                        size_t start = 0;
                        size_t end;
                        std::vector<std::string> seen_prefixes;
                        auto process_path_entry = [&](const std::string& entry) {
                            if (entry.empty()) return;
                            std::filesystem::path p(entry);
                            std::string filename = p.filename().string();
                            // Convert /bin and /sbin to parent prefix
                            std::filesystem::path prefix;
                            if (filename == "bin" || filename == "sbin") {
                                prefix = p.parent_path();
                            } else {
                                prefix = p;
                            }
                            std::string prefix_str = prefix.string();
                            // Deduplicate
                            if (std::find(seen_prefixes.begin(), seen_prefixes.end(), prefix_str) == seen_prefixes.end()) {
                                seen_prefixes.push_back(prefix_str);
                                expand_prefix(prefix);
                            }
                        };
                        while ((end = path_str.find(':', start)) != std::string::npos) {
                            process_path_entry(path_str.substr(start, end - start));
                            start = end + 1;
                        }
                        process_path_entry(path_str.substr(start));
                    }
                }
            }

            // Step 6: Package registry (skipped - we don't implement it)
            // NO_CMAKE_PACKAGE_REGISTRY silently accepted above

            // Step 7: System paths (CMAKE_SYSTEM_PREFIX_PATH, standard system roots)
            if (!no_default_path && !no_cmake_system_path) {
                std::string use_var = interp.get_variable("CMAKE_FIND_USE_CMAKE_SYSTEM_PATH");
                bool use = use_var.empty() || !interp.is_falsy(use_var);
                if (use) {
                    // CMAKE_SYSTEM_PREFIX_PATH if set
                    std::string sys_prefix = interp.get_variable("CMAKE_SYSTEM_PREFIX_PATH");
                    if (!sys_prefix.empty()) {
                        for (auto p : CMakeArrayIterator(sys_prefix)) { expand_prefix(std::string(p)); }
                    }

                    // CMAKE_INSTALL_PREFIX (unless NO_CMAKE_INSTALL_PREFIX)
                    if (!no_cmake_install_prefix) {
                        std::string install_prefix = interp.get_variable("CMAKE_INSTALL_PREFIX");
                        if (!install_prefix.empty()) { expand_prefix(install_prefix); }
                    }

                    // Standard system roots
                    std::vector<std::string> system_prefixes = {
                        "/usr",
                        "/usr/local",
                    };
                    for (const auto& prefix : system_prefixes) { expand_prefix(prefix); }

                    // Also check arch-specific paths directly
                    if (auto& triplet = gnu_arch_triplet(); !triplet.empty()) {
                        std::filesystem::path arch_cmake = std::filesystem::path("/usr/lib") / triplet / "cmake";
                        add_with_suffixes(arch_cmake);
                        for (const auto& name : search_names) {
                            std::string lower = kiln::to_lower(name);
                            add_with_suffixes(arch_cmake / name);
                            if (lower != name) add_with_suffixes(arch_cmake / lower);
                        }
                    }
                }
            }

            // Step 8: System package registry (skipped)
            // NO_CMAKE_SYSTEM_PACKAGE_REGISTRY silently accepted above

            // Step 9: PATHS (hard-coded guesses, always last)
            for (const auto& p : paths) { add_with_suffixes(p); }

            // --- Search all collected paths for config files ---
            auto check_directory_for_config = [&](const std::filesystem::path& path) -> bool {
                for (const auto& cand : candidates) {
                    if (!interp.cached_file_exists(path.string(), cand.config)) continue;
                    std::filesystem::path config_path = path / cand.config;

                    // Track considered configs
                    considered_configs.push_back(config_path.string());

                    // Run version file if it exists
                    if (interp.cached_file_exists(path.string(), cand.version)) {
                        std::filesystem::path version_path = path / cand.version;
                        considered_versions.push_back(interp.get_variable("PACKAGE_VERSION"));

                        interp.set_variable("PACKAGE_FIND_NAME", package_name);
                        interp.set_variable("PACKAGE_FIND_VERSION", v_req.full);
                        interp.set_variable("PACKAGE_FIND_VERSION_MAJOR", v_req.major);
                        interp.set_variable("PACKAGE_FIND_VERSION_MINOR", v_req.minor);
                        interp.set_variable("PACKAGE_FIND_VERSION_PATCH", v_req.patch);
                        interp.set_variable("PACKAGE_FIND_VERSION_TWEAK", v_req.tweak);
                        interp.set_variable("PACKAGE_FIND_VERSION_COUNT", v_req.count);
                        interp.set_variable("PACKAGE_FIND_VERSION_EXACT", exact ? "TRUE" : "FALSE");

                        auto res = interp.include_file(version_path.string());
                        if (!res) {
                            interp.print_message("WARN",
                                                 "Error processing version file " + version_path.string() + ": " + res.error().message);
                            continue;
                        }

                        // Map PACKAGE_VERSION* -> <Package>_VERSION*
                        std::string pkg_version = interp.get_variable("PACKAGE_VERSION");
                        if (!pkg_version.empty()) {
                            VersionComponents v_found = parse_version(pkg_version);
                            interp.set_variable(package_name + "_VERSION", v_found.full);
                            interp.set_variable(package_name + "_VERSION_MAJOR", v_found.major);
                            interp.set_variable(package_name + "_VERSION_MINOR", v_found.minor);
                            interp.set_variable(package_name + "_VERSION_PATCH", v_found.patch);
                            interp.set_variable(package_name + "_VERSION_TWEAK", v_found.tweak);
                            interp.set_variable(package_name + "_VERSION_COUNT", v_found.count);
                        }

                        // Update considered_versions with actual version found
                        if (!considered_versions.empty()) { considered_versions.back() = pkg_version; }

                        // Check version compatibility if a version was requested
                        if (!version.empty()) {
                            std::string compatible = interp.get_variable("PACKAGE_VERSION_COMPATIBLE");
                            std::string exact_match = interp.get_variable("PACKAGE_VERSION_EXACT");

                            interp.set_variable("PACKAGE_VERSION_COMPATIBLE", "");
                            interp.set_variable("PACKAGE_VERSION_EXACT", "");

                            if (interp.is_falsy(compatible)) { continue; }
                            if (exact && interp.is_falsy(exact_match)) { continue; }
                        }
                    } else if (!version.empty()) {
                        // Version requested but no version file found - skip
                        considered_versions.push_back("");
                        if (!quiet) {
                            interp.print_message("STATUS",
                                                 "Checking " + config_path.string() + ": No version file found, assuming incompatible.");
                        }
                        continue;
                    }

                    found_path = config_path;
                    return true;
                }
                return false;
            };

            // Apply CMAKE_FIND_ROOT_PATH re-rooting before scanning. Without
            // this, a cross build with CMAKE_FIND_ROOT_PATH=/sysroot would
            // still hit host paths first and resolve to host packages.
            // Mode resolution follows the find_program/library/path family:
            // per-call flag wins, else CMAKE_FIND_ROOT_PATH_MODE_PACKAGE,
            // else BOTH (effectively no-op when CMAKE_FIND_ROOT_PATH unset).
            {
                std::string root_mode;
                if (no_cmake_find_root_path)
                    root_mode = "NEVER";
                else if (only_cmake_find_root_path)
                    root_mode = "ONLY";
                else if (cmake_find_root_path_both)
                    root_mode = "BOTH";
                else {
                    std::string mode = interp.get_variable("CMAKE_FIND_ROOT_PATH_MODE_PACKAGE");
                    root_mode = (mode == "NEVER" || mode == "ONLY" || mode == "BOTH") ? mode : "BOTH";
                }
                search_paths = apply_find_root_path(interp, search_paths, root_mode);
            }

            // First pass: check explicit search paths
            for (const auto& path : search_paths) {
                auto path_parent = path.parent_path();
                auto path_name = path.filename().string();
                if (!path_parent.empty() && !path_name.empty()) {
                    if (!interp.cached_file_exists(path_parent.string(), path_name)) continue;
                }
                if (!interp.cached_is_directory(path.string())) continue;

                if (check_directory_for_config(path)) break;
            }

            // Second pass: scan directories for subdirs matching package name pattern
            // This handles cases like boost_system-1.89.0/ for package "boost_system"
            if (found_path.empty()) {
                // Collect all root dirs to scan (search_paths that are cmake/lib/share roots)
                auto scan_root_for_package = [&](const std::string& root) -> bool {
                    auto* entries = interp.get_directory_listing(root);
                    if (!entries) return false;

                    for (const auto& name : search_names) {
                        for (const auto& entry_name : *entries) {
                            if (!directory_matches_package(entry_name, name)) continue;

                            std::filesystem::path subdir = std::filesystem::path(root) / entry_name;
                            if (!interp.cached_is_directory(subdir.string())) continue;

                            if (check_directory_for_config(subdir)) return true;
                        }
                    }
                    return false;
                };

                // Scan HINTS directories
                for (const auto& h : hints) {
                    if (scan_root_for_package(h)) break;
                }

                // Scan system cmake roots
                if (found_path.empty()) {
                    std::vector<std::string> system_roots = {
                        "/usr/lib/cmake",
                        "/usr/share/cmake",
                        "/usr/local/lib/cmake",
                        "/usr/local/share/cmake",
                    };
                    if (auto& extra = cmake_extra_modules_root(); !extra.empty()) system_roots.push_back(extra);
                    if (auto& triplet = gnu_arch_triplet(); !triplet.empty()) { system_roots.push_back("/usr/lib/" + triplet + "/cmake"); }
                    for (const auto& root : system_roots) {
                        if (scan_root_for_package(root)) break;
                    }
                }
            }

            // Set CONSIDERED_CONFIGS and CONSIDERED_VERSIONS
            {
                std::string cc_str, cv_str;
                for (size_t i = 0; i < considered_configs.size(); ++i) {
                    if (i > 0) {
                        cc_str += ";";
                        cv_str += ";";
                    }
                    cc_str += considered_configs[i];
                    if (i < considered_versions.size()) cv_str += considered_versions[i];
                }
                interp.set_variable(package_name + "_CONSIDERED_CONFIGS", cc_str);
                interp.set_variable(package_name + "_CONSIDERED_VERSIONS", cv_str);
            }
        } // if (try_config)

        if (!found_path.empty()) {
            interp.set_variable(found_var, "ON");
            interp.set_variable(dir_var, found_path.parent_path().string());
            interp.set_variable(package_name + "_CONFIG", found_path.string());

            auto res = interp.include_file(found_path.string());
            if (!res) {
                interp.set_fatal_error(res.error());
                return;
            }

            // The config file may have set <Package>_FOUND to FALSE
            // (e.g. Qt6Config.cmake does this when a required component is missing)
            std::string post_found = interp.get_variable(found_var);
            if (interp.is_falsy(post_found)) {
                if (required) {
                    std::string not_found_msg = interp.get_variable(package_name + "_NOT_FOUND_MESSAGE");
                    std::string error_msg = "Could not find package " + package_name;
                    if (!not_found_msg.empty()) { error_msg += ": " + not_found_msg; }
                    interp.set_fatal_error(error_msg);
                } else if (!quiet) {
                    interp.print_message("STATUS", "Could not find package " + package_name + " (optional)");
                }
                return;
            }

            validate_components();
        } else {
            // For config-preferred packages, fall back to system Find modules
            if (prefer_config && try_system_find_modules()) return;

            // Nothing found
            interp.set_variable(found_var, "OFF");

            auto build_not_found_message = [&]() -> std::string {
                std::string msg = "Could not find \"" + package_name + "\"";
                if (!version.empty()) msg += " version " + version;
                msg += ": no Find" + package_name
                       + ".cmake under CMAKE_MODULE_PATH "
                         "and no "
                       + package_name
                       + "Config.cmake found. "
                         "Set "
                       + dir_var + " or CMAKE_PREFIX_PATH accordingly.";
                return msg;
            };

            if (required) {
                interp.set_fatal_error(build_not_found_message());
            } else if (!quiet) {
                interp.print_warning_with_context(build_not_found_message());
            }
        }
    });
}

} // namespace kiln
