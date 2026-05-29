#pragma once
#include <string>
#include <vector>
#include <map>
#include <expected>
#include <filesystem>

namespace kiln {

// Resolved configure preset — the flat shape consumers actually want after
// inherits/include chains and ${var}/$env{}/$penv{} expansion are applied.
struct ResolvedPreset {
    std::string name;
    std::string generator;
    std::string binary_dir;
    std::string toolchain_file;
    // Cache variables: name -> value. Type annotations (BOOL/STRING/PATH) are
    // dropped here — kiln treats all -D values as strings.
    std::map<std::string, std::string> cache_variables;
    // Environment: name -> value. Caller is responsible for setenv-ing these
    // before forking child processes (so vcpkg, find_package etc. see them).
    std::map<std::string, std::string> environment;
    std::string build_type; // CMAKE_BUILD_TYPE shortcut, mirrors cache var if present
};

// Load CMakePresets.json (and optional CMakeUserPresets.json) from project_dir
// and resolve the named configure preset. Returns the flattened preset, or an
// error string describing what went wrong (file missing, unknown preset, cycle
// in inherits, condition false, etc.).
std::expected<ResolvedPreset, std::string> load_configure_preset(const std::filesystem::path& project_dir, const std::string& preset_name);

// List the names of all non-hidden configure presets in the project. Useful
// for a `--list-presets`-style flag and for friendlier error messages.
std::expected<std::vector<std::string>, std::string> list_configure_presets(const std::filesystem::path& project_dir);

} // namespace kiln
