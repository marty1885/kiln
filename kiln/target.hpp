#pragma once

#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <set>
#include <memory>
#include <string_view>
#include <algorithm>
#include <optional>
#include "language.hpp"
#include "shadow_map.hpp"

namespace kiln {

class Target;
struct CustomCommandRule;
using TargetMap = std::unordered_map<std::string, std::shared_ptr<Target>, TransparentStringHash, TransparentStringEqual>;

enum class TargetType { EXECUTABLE, SHARED_LIBRARY, STATIC_LIBRARY, OBJECT_LIBRARY, INTERFACE_LIBRARY, CUSTOM };

// Per-language precompiled header info for task generation
struct PchInfo {
    std::string gch_path;
    std::string include_arg;
};
enum class PropertyVisibility { PRIVATE, INTERFACE, PUBLIC };

// Semantic scopes for querying properties. Maps to visibility combinations.
// Use this instead of manually combining PropertyVisibility values.
enum class TargetPropertyScope {
    BUILD,     // PRIVATE + PUBLIC — what this target uses to build itself
    INTERFACE, // PUBLIC + INTERFACE — what consumers of this target inherit
};

// Single source of truth for all list properties that use visibility (PUBLIC/PRIVATE/INTERFACE).
// Used by resolve(), generate_dump_info(), set_target_properties(), set_property(TARGET).
struct PropertyMeta {
    std::string_view name;
    bool is_path;    // needs absolutification during resolve
    bool transitive; // resolved across dependencies
};

inline constexpr PropertyMeta kListProperties[] = {
    {"INCLUDE_DIRECTORIES", true, true},  {"SYSTEM_INCLUDE_DIRECTORIES", true, true},
    {"COMPILE_DEFINITIONS", false, true}, {"COMPILE_OPTIONS", false, true},
    {"COMPILE_FEATURES", false, true},    {"LINK_LIBRARIES", false, true},
    {"LINK_DIRECTORIES", true, true},     {"LINK_OPTIONS", false, true},
    {"PRECOMPILE_HEADERS", false, true},  {"SOURCES", false, false},
};

// Check if a property name is a known list property (returns pointer or nullptr)
inline const PropertyMeta* find_list_property(std::string_view name) {
    for (const auto& meta : kListProperties) {
        if (meta.name == name) return &meta;
    }
    return nullptr;
}

// Check if a property name is an INTERFACE_ prefixed version of a known list property.
// If so, returns pointer to the base property meta. E.g. "INTERFACE_COMPILE_OPTIONS" → &COMPILE_OPTIONS meta.
inline const PropertyMeta* find_interface_list_property(std::string_view name) {
    if (!name.starts_with("INTERFACE_")) return nullptr;
    return find_list_property(name.substr(10)); // len("INTERFACE_") == 10
}

struct FileSet {
    std::string name;
    std::string type; // "HEADERS" or "CXX_MODULES"
    PropertyVisibility visibility;
    std::vector<std::string> base_dirs;
    std::vector<std::string> files;
};

struct CustomCommand {
    std::vector<std::string> command;
    std::string comment;
    std::string working_dir;
};

class BuildGraph;
class GraphTransaction;
class Toolchain;
struct GenexEvaluationContext;

class Target {
public:
    Target(std::string name, TargetType type, std::string source_dir, std::string binary_dir)
        : name_(std::move(name)), type_(type), source_dir_(std::move(source_dir)), binary_dir_(std::move(binary_dir)), is_imported_(false) {
    }
    virtual ~Target() = default;

    const std::string& get_name() const { return name_; }
    TargetType get_type() const { return type_; }
    const std::string& get_source_dir() const { return source_dir_; }
    const std::string& get_binary_dir() const { return binary_dir_; }

    void set_output_name(std::string output_name);
    const std::string& get_output_name() const;

    // Active build configuration captured at target-definition time
    // (CMAKE_BUILD_TYPE). Used by get_output_path() to apply
    // <CONFIG>_POSTFIX/DEBUG_POSTFIX without threading interpreter state.
    void set_build_type(std::string bt) { build_type_ = std::move(bt); }
    const std::string& get_build_type() const { return build_type_; }

    void set_imported(bool imported) { is_imported_ = imported; }
    bool is_imported() const { return is_imported_; }

    // Imported targets are directory-scoped by default — visible only in the
    // dir that created them and its subdirs. The GLOBAL keyword promotes them
    // to project-wide visibility. Non-imported targets are always global.
    void set_imported_global(bool g) { imported_global_ = g; }
    bool is_imported_global() const { return imported_global_; }
    bool is_visiting() const { return visiting_; }
    bool is_resolved() const { return resolved_; }

    void set_imported_location(std::string location) { imported_location_ = std::move(location); }
    const std::string& get_imported_location() const { return imported_location_; }

    void set_language_standard(Language lang, std::string standard);
    const std::string& get_language_standard(Language lang) const;

    void set_language_extensions(Language lang, bool enabled);
    bool get_language_extensions(Language lang) const;

    void add_language_flags(Language lang, const std::vector<std::string>& flags);
    const std::vector<std::string>& get_language_flags(Language lang) const;

    // Generic Property Access
    void set_property(const std::string& name, const std::string& value);
    std::string get_property(const std::string& name) const;

    // Generic List Property Access (for properties that accumulate like SOURCES, DEFINITIONS)
    // Note: values should already be split - append_property() asserts no semicolons in debug builds
    void append_property(const std::string& name, const std::vector<std::string>& values, PropertyVisibility visibility);
    void prepend_property(const std::string& name, const std::vector<std::string>& values, PropertyVisibility visibility);
    const std::vector<std::string>& get_property_list(const std::string& name, PropertyVisibility visibility) const;
    std::vector<std::string> get_property_list(const std::string& name, std::initializer_list<PropertyVisibility> visibilities) const;

    // Get property values for a semantic scope (merges visibility buckets, returns by value).
    std::vector<std::string> get_property_list(const std::string& name, TargetPropertyScope scope) const;

    // Get combined property value as semicolon-separated string (all visibilities).
    // Checks list_properties_ first, then falls back to generic properties_.
    std::string get_property_combined(const std::string& name) const;

    // Helper for raw string inputs: splits by semicolon then appends
    void append_property_from_string(const std::string& name, const std::string& value, PropertyVisibility visibility);

    // File Set Support
    void add_file_set(FileSet file_set);
    const std::vector<FileSet>& get_file_sets() const { return file_sets_; }
    bool is_in_cxx_modules_file_set(const std::string& source) const;

    // Look up the visibility of the cxx_modules FILE_SET that contains `source`,
    // if any. Returns nullopt if not in any cxx_modules file set (treat as PRIVATE
    // for export purposes — those modules stay internal to this target).
    std::optional<PropertyVisibility> file_set_visibility_for_source(const std::string& source) const;

    // Manually added dependencies (add_dependencies command)
    void add_dependency(const std::string& dep) { manually_added_dependencies_.push_back(dep); }
    const std::vector<std::string>& get_manually_added_dependencies() const { return manually_added_dependencies_; }

    // Compiler-scope capture. Stores the values of CMAKE_<LANG>_COMPILER /
    // CMAKE_SYSROOT / CMAKE_<LANG>_COMPILER_TARGET as they were in the
    // current variable scope when this target was defined. At
    // generate_tasks time the build system uses these to look up a
    // per-target Compiler* from the Toolchain registry, so a subsequent
    // set(CMAKE_CXX_COMPILER ...) in a different scope won't change which
    // compiler this target uses. Empty values mean "fall back to the
    // toolchain default for this language."
    void capture_compiler_var(const std::string& var, std::string value) { compiler_var_snapshot_[var] = std::move(value); }
    const std::string& captured_compiler_var(const std::string& var) const {
        static const std::string empty;
        auto it = compiler_var_snapshot_.find(var);
        return it == compiler_var_snapshot_.end() ? empty : it->second;
    }

    // Resolve the compiler this target should use for `lang`. If the target
    // captured a non-empty CMAKE_<LANG>_COMPILER at definition time, the
    // Toolchain registry returns the (possibly registered-on-demand) Compiler*
    // matching that exact (binary, sysroot, target) tuple. Otherwise we fall
    // back to the toolchain's default compiler for the language. Returns
    // nullptr if no compiler is available at all.
    const class Compiler* resolve_compiler(Language lang, const class Toolchain& toolchain) const;

    // Build event commands (TARGET form of add_custom_command)
    void add_pre_build_command(CustomCommand cmd) { pre_build_commands_.push_back(std::move(cmd)); }
    void add_pre_link_command(CustomCommand cmd) { pre_link_commands_.push_back(std::move(cmd)); }
    void add_post_build_command(CustomCommand cmd) { post_build_commands_.push_back(std::move(cmd)); }
    const std::vector<CustomCommand>& get_pre_build_commands() const { return pre_build_commands_; }
    const std::vector<CustomCommand>& get_pre_link_commands() const { return pre_link_commands_; }
    const std::vector<CustomCommand>& get_post_build_commands() const { return post_build_commands_; }

    // Deprecated helpers for C++ (mapped to generic properties)
    void set_cxx_standard(const std::string& standard) { set_language_standard(Language::CXX, standard); }
    const std::string& get_cxx_standard() const { return get_language_standard(Language::CXX); }

    virtual std::string get_output_path(class GenexEvaluator* evaluator = nullptr) const;

    // The core task generation logic
    virtual std::expected<void, std::string> generate_tasks(GraphTransaction& txn, const Toolchain& toolchain, const TargetMap& all_targets,
                                                            const class Interpreter& interp,
                                                            const std::vector<std::string>& exe_linker_flags = {},
                                                            const std::vector<std::string>& shared_linker_flags = {});

    // Resolves transitive properties (includes, definitions, options, libraries).
    // Must be called before generating tasks. Safe to call multiple times (cached).
    void resolve(const TargetMap& all_targets, const class Interpreter& interp);

    // Access resolved properties (after resolve() is called)
    const std::vector<std::string>& get_resolved_property(const std::string& name) const;
    const std::vector<std::string>& get_resolved_interface_property(const std::string& name) const;

    // Like get_resolved_property() but with $<COMPILE_LANGUAGE:...> deferred
    // genex evaluated for the given language. Use this when feeding tools
    // that consume per-language flags (compilers, moc, etc.).
    std::vector<std::string> get_resolved_property_for_language(const std::string& name, Language lang, const class Interpreter& interp,
                                                                const TargetMap& all_targets) const;

    // Linker language used by $<LINK_LANGUAGE>. Returns LINKER_LANGUAGE
    // property if set, else inferred from this target's own sources
    // (any C++ source -> CXX, else C). Cached after first call.
    Language get_linker_language() const;

    // Second pass after all targets are resolved: pick up interface properties
    // from circular dependencies that were unavailable during the DFS.
    void resolve_deferred_circular_deps(const TargetMap& all_targets);

    // All direct target dependencies (canonical names) discovered during resolve().
    // Single source of truth for dependency discovery.
    const std::vector<std::string>& get_resolved_target_deps() const { return resolved_target_deps_; }

    // Generate a formatted dump of target info (for kiln_dump_target_info)
    std::string generate_dump_info() const;

    // Get the module mapper file path for this target
    std::string get_module_mapper_path() const;

    // Get the path of this target's exported-modules manifest file. Written by
    // the target's collator with one entry per PUBLIC/INTERFACE provided module;
    // read by consumers' collators to resolve cross-target imports.
    std::string get_module_manifest_path() const;

    // Qt autogen helpers — called by generate_autogen_tasks()
    void inject_autogen_include(const std::string& dir);
    void inject_autogen_source(const std::string& path);
    void inject_autogen_dep(const std::string& task_id);
    const std::vector<std::string>& get_autogen_deps() const { return autogen_deps_; }
    void remove_source(const std::string& path);

    // Factory for GenexEvaluationContext. Populates from interpreter variables.
    static GenexEvaluationContext make_genex_context(const Target* current_target, const class Interpreter& interp,
                                                     const TargetMap& all_targets, std::optional<Language> compile_language = std::nullopt,
                                                     bool allow_deferred = false);

protected:
    // Metadata for a transitive property during resolve().
    struct PropInfo {
        std::string name;
        bool is_path;
    };
    static const std::vector<PropInfo>& build_props_to_resolve();

    // Resolve a possibly-relative path to absolute against source_dir_.
    std::string resolve_to_absolute_path(const std::string& p) const;

    // Phase 1 of resolve(): populate resolved_properties_ and resolved_interface_properties_
    // from this target's own property values. Evaluates generator expressions, resolves paths.
    void initialize_local_properties(const std::vector<PropInfo>& props_to_resolve, class GenexEvaluator& evaluator);

    // Get the link library dependencies that should propagate from a dependency.
    // Static (non-imported): ALL link deps; Shared/imported/interface: only INTERFACE.
    static const std::vector<std::string>& collect_link_deps(const Target& dep);

    // Merge a resolved dependency's INTERFACE properties into an output property map.
    // If the dependency has deferred genex (single-arg $<TARGET_PROPERTY:>), re-evaluates
    // them with the provided evaluator (which has the consumer as current_target).
    void propagate_from_dependency(const Target& dep, const std::vector<PropInfo>& props_to_resolve,
                                   std::map<std::string, std::vector<std::string>>& output_props, bool skip_non_link,
                                   class GenexEvaluator& evaluator);

    // Handle a circular dependency detected during resolve().
    void handle_circular_dep(const Target& dep, bool is_public, bool is_interface_only, std::vector<std::string>& res_libs,
                             std::vector<std::string>& res_iface_libs);

    // Circular static lib deps recorded during resolve() for deferred property pickup.
    std::vector<std::string> deferred_circular_deps_;

    // Helper methods for task generation
    std::expected<void, std::string>
    generate_object_tasks(GraphTransaction& txn, const Toolchain& toolchain, std::vector<std::string>& obj_files,
                          const std::map<Language, PchInfo>& pch_per_lang, bool is_shared, bool is_pie, const TargetMap& all_targets,
                          class GenexEvaluator& evaluator, const class Interpreter& interp, const std::string& pre_build_task_id,
                          const std::string& module_mapper_path, std::set<std::string>& generated_custom_tasks,
                          const std::set<std::string>& implicit_includes, std::vector<struct ResolvedDep> resolved_manual_deps);

    // C++20 modules task generation
    // Returns true if module pipeline (scanner+collator) was generated for this target
    std::expected<bool, std::string> generate_module_scanner_tasks(GraphTransaction& txn, const Toolchain& toolchain,
                                                                   const TargetMap& all_targets, const class Interpreter& interp,
                                                                   int cxx_default_std = 0);

    // Generate the collator task that depends on all scanner tasks plus the
    // module-export manifests of every module-providing transitive PUBLIC/INTERFACE
    // link dep, so cross-target imports resolve.
    std::expected<void, std::string> generate_module_collator_task(GraphTransaction& txn, const std::vector<std::string>& scanner_task_ids,
                                                                   const TargetMap& all_targets,
                                                                   const std::string& std_module_manifest_path,
                                                                   const class Compiler* cxx_compiler, const std::string& cxx_standard,
                                                                   bool cxx_extensions_enabled);

    // Check if target has any module sources of its own
    bool has_module_sources() const;

    // Transitive PUBLIC/INTERFACE link deps that have module sources. Walks
    // the resolved LINK_LIBRARIES closure. Used to: (a) decide whether this
    // target needs a module pipeline even without own interface units, and
    // (b) collect manifest inputs for the collator.
    std::vector<Target*> transitive_module_providing_deps(const TargetMap& all_targets) const;

    // True iff this target needs the module compile pipeline (scanner, collator,
    // -fmodules-ts, module mapper). Triggered by own modules OR by cross-target
    // imports from a module-providing link dep.
    bool participates_in_modules(const TargetMap& all_targets) const;

    // Cheap textual peek for `import` directives in CXX sources. Lets us
    // opt targets that import std (or any module) into the module pipeline
    // even when they have no own interface units / module-providing deps.
    bool target_imports_anything() const;

    // Targeted variant: matches only `import std;` / `import std.compat;`.
    // Used to gate eager scheduling of the toolchain-level std module
    // compile so projects that don't `import std;` pay no cost.
    bool target_imports_std() const;

    std::string name_;
    std::string output_name_;
    std::string build_type_;
    TargetType type_;
    std::string source_dir_;
    std::string binary_dir_;

    bool is_imported_;
    bool imported_global_ = false;
    std::string imported_location_;

    std::map<Language, std::string> standards_;
    std::map<Language, bool> extensions_enabled_; // Whether GNU extensions are enabled (gnu11 vs c11)
    std::map<Language, std::vector<std::string>> language_flags_;

    // Generic Storage
    // Scalar properties (e.g., OUTPUT_NAME, CXX_STANDARD)
    std::map<std::string, std::string> properties_;

    // List properties with visibility (e.g., INCLUDE_DIRECTORIES[PUBLIC])
    // Structure: map<PropertyName, map<Visibility, vector<Value>>>
    std::map<std::string, std::map<PropertyVisibility, std::vector<std::string>>> list_properties_;

    // File Sets (CMake 3.23+)
    std::vector<FileSet> file_sets_;

    // Resolution State
    bool resolved_ = false;
    bool visiting_ = false;

    // Resolved Properties Cache (effective build requirements)
    // Map<PropertyName, ListOfValues>
    std::map<std::string, std::vector<std::string>> resolved_properties_;

    // Resolved Interface Properties Cache (usage requirements propagated to dependents)
    std::map<std::string, std::vector<std::string>> resolved_interface_properties_;

    // Raw INTERFACE property values containing single-arg $<TARGET_PROPERTY:prop> genex.
    // These must be re-evaluated per consumer with the consumer as current_target.
    // Map<PropertyName, vector<RawValue>>
    std::map<std::string, std::vector<std::string>> deferred_interface_genex_;

    // OBJECT library dependencies discovered during resolve() (target names, not aliases).
    // Used by generate_tasks() to inject .o files into the link command.
    std::vector<std::string> resolved_object_lib_deps_;

    // All direct target dependencies discovered during resolve() (canonical names, not aliases).
    // Single source of truth for dependency discovery — replaces raw LINK_LIBRARIES walking.
    std::vector<std::string> resolved_target_deps_;

    // C++20 modules state
    mutable bool modules_detected_ = false;
    mutable bool has_modules_ = false;

    // Manually added dependencies (from add_dependencies command)
    std::vector<std::string> manually_added_dependencies_;
    std::map<std::string, std::string> compiler_var_snapshot_;

    // Autogen (UIC/MOC/RCC) task IDs that all compile tasks must depend on
    std::vector<std::string> autogen_deps_;

    // Build event commands (TARGET form of add_custom_command)
    std::vector<CustomCommand> pre_build_commands_;
    std::vector<CustomCommand> pre_link_commands_;
    std::vector<CustomCommand> post_build_commands_;

    mutable std::optional<Language> cached_linker_language_;
};

class CustomTarget : public Target {
public:
    CustomTarget(std::string name, std::string source_dir, std::string binary_dir)
        : Target(std::move(name), TargetType::CUSTOM, std::move(source_dir), std::move(binary_dir)) {}

    void add_custom_command(CustomCommand cmd) { custom_commands_.push_back(std::move(cmd)); }
    void add_custom_dependency(std::string dep) { custom_depends_.push_back(std::move(dep)); }
    const std::vector<std::string>& get_custom_dependencies() const { return custom_depends_; }
    void add_byproduct(std::string path) { byproducts_.push_back(std::move(path)); }
    const std::vector<std::string>& get_byproducts() const { return byproducts_; }
    void set_build_by_default(bool b) { build_by_default_ = b; }
    bool is_build_by_default() const { return build_by_default_; }

    std::expected<void, std::string> generate_tasks(GraphTransaction& txn, const Toolchain& toolchain, const TargetMap& all_targets,
                                                    const class Interpreter& interp, const std::vector<std::string>& exe_linker_flags = {},
                                                    const std::vector<std::string>& shared_linker_flags = {}) override;

private:
    std::vector<CustomCommand> custom_commands_;
    std::vector<std::string> custom_depends_;
    std::vector<std::string> byproducts_;
    bool build_by_default_ = false;
};

// Compute the object file path for a source file within a target.
// Single source of truth — used by target task generation, genex evaluator, and module collator.
std::string get_obj_path(const std::string& binary_dir, const std::string& target_name, const std::string& source_path);

// Generate a task for an add_custom_command rule (and its custom-command DEPENDS chain).
// Used by the missing-dependency resolver when a target's input/dep is the OUTPUT of a
// stand-alone add_custom_command not pulled in via target_sources/DEPENDS.
std::expected<void, std::string>
generate_custom_command_task_for_rule(GraphTransaction& txn, const CustomCommandRule& rule, const TargetMap& all_targets,
                                      const std::map<std::string, std::shared_ptr<CustomCommandRule>>& custom_rules,
                                      std::set<std::string>& generated, const std::unordered_map<std::string, std::string>& target_aliases);

} // namespace kiln
