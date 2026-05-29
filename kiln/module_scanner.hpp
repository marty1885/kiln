#pragma once

#include <string>
#include <vector>
#include <expected>
#include <optional>
#include <filesystem>

namespace kiln {

// Module map entry - maps module names to their BMI paths
struct ModuleMapEntry {
    std::string module_name;
    std::string bmi_path;
    std::string source_path;
    std::string object_task_id;  // Task ID that produces this module's BMI
    bool is_header_unit = false; // module_name is the header's resolved path
};

// Generate module mapper file content for g++ -fmodule-mapper=<file>
// Maps module names to BMI file paths
std::string generate_module_mapper_content(const std::vector<ModuleMapEntry>& entries);

// Parse module mapper file
std::expected<std::vector<ModuleMapEntry>, std::string> parse_module_mapper_file(const std::string& path);

// Get the BMI (Binary Module Interface) path for a given module name
std::string get_bmi_path(const std::string& binary_dir, const std::string& module_name);

// Get the BMI path for a header unit, derived from the header's absolute
// source path. Header-unit BMIs live under <binary_dir>/bmis/header_units/
// keyed by a path-mangled form of the source so they don't collide with
// named-module BMIs.
std::string get_header_unit_bmi_path(const std::string& binary_dir, const std::string& source_path);

// Get the DDI file path for a given source file
std::string get_ddi_path(const std::string& binary_dir, const std::string& source_path);

// --- P1689r5 ---
// Schema: https://wg21.link/p1689r5 — the JSON format emitted by GCC's
// -fdeps-format=p1689r5, clang-scan-deps --format=p1689, and MSVC
// /scanDependencies. One file per scanned TU; rules[] usually has one entry.

struct P1689Provide {
    std::string logical_name;                        // Module name, e.g. "Math" or "Math:Part"
    std::optional<std::string> source_path;          // Source path of the providing TU
    std::optional<std::string> compiled_module_path; // Hint for BMI output path
    bool is_interface = true;                        // True for interface units, false for impl
};

struct P1689Require {
    std::string logical_name;               // Module name or header-unit name
    std::string lookup_method = "by-name";  // "by-name" | "include-angle" | "include-quote"
    std::optional<std::string> source_path; // Header path (for include-* methods)
};

struct P1689Rule {
    std::string primary_output; // The compile output (.o) this scan describes
    std::vector<P1689Provide> provides;
    std::vector<P1689Require> requires_; // Trailing _ avoids C++ keyword
};

struct P1689File {
    int version = 0;
    int revision = 0;
    std::vector<P1689Rule> rules;
};

// Parse a P1689r5 JSON file produced by the compiler's scanner.
std::expected<P1689File, std::string> parse_p1689_file(const std::string& path);

// Same, but from an in-memory string. Used by tests.
std::expected<P1689File, std::string> parse_p1689_string(const std::string& json);

// --- Module export manifest ---
// Per-target manifest of modules a target makes visible to consumers, written
// by that target's ModuleCollatorTask. Consumer collators read manifests from
// transitive PUBLIC/INTERFACE link deps to resolve cross-target imports.

struct ModuleManifestEntry {
    std::string logical_name;   // e.g. "Foo" or "Foo:Part"
    std::string bmi_path;       // Absolute path to the BMI (.gcm)
    std::string primary_output; // The producing compile task's id (its .o path)
    std::string source_path;    // The provider's source TU; used in error messages
    std::string visibility;     // "PUBLIC" | "INTERFACE" — PRIVATE entries are not written
};

struct ModuleManifest {
    std::vector<ModuleManifestEntry> entries;
};

std::expected<ModuleManifest, std::string> read_module_manifest(const std::string& path);
std::expected<void, std::string> write_module_manifest(const std::string& path, const ModuleManifest& manifest);

// String-form parse, exposed for unit tests.
std::expected<ModuleManifest, std::string> parse_module_manifest_string(const std::string& json);

// --- libstdc++.modules.json (GCC ≥15) ---
// Toolchain-shipped manifest describing the std module units. The path is
// "libstdc++.modules.json" relative to the compiler's library dir (queryable
// via `g++ -print-file-name=`). source-path entries are relative to the
// directory containing modules.json.

struct LibstdcxxModuleEntry {
    std::string logical_name; // "std" or "std.compat"
    std::string source_path;  // Relative to modules.json's directory
    bool is_std_library = false;
};

struct LibstdcxxModulesJson {
    int version = 0;
    int revision = 0;
    std::vector<LibstdcxxModuleEntry> modules;
};

std::expected<LibstdcxxModulesJson, std::string> parse_libstdcxx_modules_json_string(const std::string& json);
std::expected<LibstdcxxModulesJson, std::string> parse_libstdcxx_modules_json_file(const std::string& path);

} // namespace kiln
