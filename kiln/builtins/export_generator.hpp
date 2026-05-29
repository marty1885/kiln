#pragma once

#include "../target.hpp"
#include <string>
#include <unordered_map>
#include <vector>

namespace kiln {

// Context for generating export files
struct ExportContext {
    bool for_install;                 // true = install(EXPORT), false = export()
    std::string namespace_prefix;     // e.g., "LLVM::"
    std::string destination;          // Install destination (for computing _IMPORT_PREFIX depth)
    std::string install_prefix;       // CMAKE_INSTALL_PREFIX
    std::string build_type;           // CMAKE_BUILD_TYPE
    std::string config;               // Current build config for per-config properties (e.g., "Debug", "Release")
    std::string system_name;          // CMAKE_SYSTEM_NAME
    std::string cxx_compiler_id;      // CMAKE_CXX_COMPILER_ID
    std::string c_compiler_id;        // CMAKE_C_COMPILER_ID
    std::string cxx_compiler_version; // CMAKE_CXX_COMPILER_VERSION
    std::string c_compiler_version;   // CMAKE_C_COMPILER_VERSION

    // Pointers to target maps (not owned)
    const TargetMap* all_targets = nullptr;
    const std::unordered_map<std::string, std::string>* target_aliases = nullptr;

    // Per-target install destinations from install(TARGETS ... EXPORT). Empty
    // strings mean "fall back to GNUInstallDirs defaults" (lib/, bin/). Keyed
    // by the unprefixed target name.
    struct InstallDests {
        std::string archive_dest;
        std::string library_dest;
        std::string runtime_dest;
    };
    std::unordered_map<std::string, InstallDests> target_install_dests;
};

// Generate the content of a CMake export file (.cmake file with IMPORTED targets)
// Returns the complete file content as a string
std::string generate_export_content(const ExportContext& ctx, const std::vector<Target*>& targets);

// Generate the per-config export file content (e.g., MyLibTargets-release.cmake)
// This contains IMPORTED_LOCATION_<CONFIG> and other per-configuration properties
std::string generate_config_export_content(const ExportContext& ctx, const std::vector<Target*>& targets,
                                           const std::string& install_prefix);

// Helper to find a target by name, resolving aliases
Target* find_target_in_context(const ExportContext& ctx, const std::string& name);

} // namespace kiln
