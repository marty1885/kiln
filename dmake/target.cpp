#include "target.hpp"
#include "build_system.hpp"
#include "language.hpp"
#include "toolchain.hpp"
#include "module_scanner.hpp"
#include "genex_evaluator.hpp"
#include "interperter.hpp"
#include "compile_features.hpp"
#include "printing.hpp"
#include "CMakeArray.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <algorithm>
#include "container_utils.hpp"
#include "utils.hpp"
#include <functional>
#include <set>
#include <unordered_set>
#include <unordered_map>
#include <cassert>
#include <iostream>

namespace dmake {

// Compute the effective language standard, avoiding unnecessary -std= flags when the
// compiler default already satisfies the requirement (e.g. don't emit -std=gnu++11
// when the compiler defaults to C++17).
//
// explicit_standard: from target's CXX_STANDARD/C_STANDARD (e.g. "17", or "" if unset)
// features_required: minimum standard required by compile features (e.g. 11, or 0)
// compiler_default:  compiler's default standard (e.g. 17, or 0 if unknown)
//
// Returns the standard string to use (e.g. "17"), or "" if no flag needed.
static std::string compute_effective_standard(
    const std::string& explicit_standard,
    int features_required,
    int compiler_default)
{
    int explicit_val = explicit_standard.empty() ? 0 : std::stoi(explicit_standard);
    int effective = std::max(explicit_val, features_required);

    if (effective <= 0) return explicit_standard;  // nothing required

    // If no explicit standard was set (only features drove the requirement),
    // and the compiler default already meets it, don't add a flag.
    if (explicit_val == 0 && compiler_default > 0 && effective <= compiler_default)
        return "";

    return std::to_string(effective);
}

// --- Generic Property Implementation ---

void Target::set_property(const std::string& name, const std::string& value) {
    properties_[name] = value;
}

std::string Target::get_property(const std::string& name) const {
    if (properties_.contains(name)) {
        return properties_.at(name);
    }
    return "";
}

void Target::append_property(const std::string& name, const std::vector<std::string>& values, PropertyVisibility visibility) {
    auto& list = list_properties_[name][visibility];
    for (const auto& v : values) {
        for (auto item : CMakeArrayView(v)) {
            if (!item.empty()) list.emplace_back(item);
        }
    }
}

void Target::append_property_from_string(const std::string& name, const std::string& value, PropertyVisibility visibility) {
    CMakeArray list(value);  // Always split by semicolons
    append_property(name, list.to_vector(), visibility);
}

void Target::prepend_property(const std::string& name, const std::vector<std::string>& values, PropertyVisibility visibility) {
    auto& list = list_properties_[name][visibility];
    std::vector<std::string> split;
    for (const auto& v : values) {
        for (auto item : CMakeArrayView(v)) {
            if (!item.empty()) split.emplace_back(item);
        }
    }
    list.insert(list.begin(), split.begin(), split.end());
}

const std::vector<std::string>& Target::get_property_list(const std::string& name, PropertyVisibility visibility) const {
    static const std::vector<std::string> empty;
    auto prop_it = list_properties_.find(name);
    if (prop_it == list_properties_.end()) return empty;

    auto vis_it = prop_it->second.find(visibility);
    return (vis_it != prop_it->second.end()) ? vis_it->second : empty;
}

std::vector<std::string> Target::get_property_list(const std::string& name, std::initializer_list<PropertyVisibility> visibilities) const {
    std::vector<std::string> result;
    for (auto vis : visibilities) {
        const auto& vals = get_property_list(name, vis);
        result.insert(result.end(), vals.begin(), vals.end());
    }
    return result;
}

std::vector<std::string> Target::get_property_list(const std::string& name, TargetPropertyScope scope) const {
    switch (scope) {
    case TargetPropertyScope::BUILD:
        return get_property_list(name, {PropertyVisibility::PRIVATE, PropertyVisibility::PUBLIC});
    case TargetPropertyScope::INTERFACE:
        return get_property_list(name, {PropertyVisibility::PUBLIC, PropertyVisibility::INTERFACE});
    }
    __builtin_unreachable();
}

std::string Target::get_property_combined(const std::string& name) const {
    // Check visibility-based list properties first
    auto prop_it = list_properties_.find(name);
    if (prop_it != list_properties_.end()) {
        std::string result;
        for (auto vis : {PropertyVisibility::PUBLIC, PropertyVisibility::PRIVATE, PropertyVisibility::INTERFACE}) {
            auto vis_it = prop_it->second.find(vis);
            if (vis_it != prop_it->second.end()) {
                for (const auto& v : vis_it->second) {
                    if (!result.empty()) result += ';';
                    result += v;
                }
            }
        }
        if (!result.empty()) return result;
    }
    // Fall back to generic properties
    return get_property(name);
}

const std::vector<std::string>& Target::get_resolved_property(const std::string& name) const {
    static const std::vector<std::string> empty;
    auto it = resolved_properties_.find(name);
    return (it != resolved_properties_.end()) ? it->second : empty;
}

const std::vector<std::string>& Target::get_resolved_interface_property(const std::string& name) const {
    static const std::vector<std::string> empty;
    auto it = resolved_interface_properties_.find(name);
    return (it != resolved_interface_properties_.end()) ? it->second : empty;
}

std::string Target::generate_dump_info() const {
    // Helper to print a vector
    auto print_vec = [](const std::string& indent, const std::vector<std::string>& vec) -> std::string {
        if (vec.empty()) return indent + "(empty)";
        return join(vec, "", [&](const std::string& item) {
            return indent + "- " + item + "\n";
        });
    };

    // Helper to get type name
    auto type_name = [](TargetType type) -> std::string {
        switch(type) {
            case TargetType::EXECUTABLE: return "EXECUTABLE";
            case TargetType::SHARED_LIBRARY: return "SHARED_LIBRARY";
            case TargetType::STATIC_LIBRARY: return "STATIC_LIBRARY";
            case TargetType::OBJECT_LIBRARY: return "OBJECT_LIBRARY";
            case TargetType::INTERFACE_LIBRARY: return "INTERFACE_LIBRARY";
            case TargetType::CUSTOM: return "CUSTOM";
            default: return "UNKNOWN";
        }
    };

    // Print basic info
    std::string output;
    output += "=== Target: " + name_ + " ===\n";
    output += "Type: " + type_name(type_) + "\n";
    output += "Source Dir: " + source_dir_ + "\n";
    output += "Binary Dir: " + binary_dir_ + "\n";
    output += "Output Path: " + get_output_path() + "\n";
    output += "Imported: " + std::string(is_imported_ ? "yes" : "no") + "\n";
    if (is_imported_) {
        output += "Imported Location: " + imported_location_ + "\n";
    }
    output += "\n";

    // Print unresolved properties (derived from shared metadata table)
    std::vector<std::string> prop_names;
    for (const auto& meta : kListProperties) {
        prop_names.emplace_back(meta.name);
    }

    output += "--- Unresolved Properties ---\n";
    for (const auto& prop : prop_names) {
        const auto& priv = get_property_list(prop, PropertyVisibility::PRIVATE);
        const auto& pub = get_property_list(prop, PropertyVisibility::PUBLIC);
        const auto& iface = get_property_list(prop, PropertyVisibility::INTERFACE);

        if (!priv.empty() || !pub.empty() || !iface.empty()) {
            output += "\n" + prop + ":\n";
            if (!priv.empty()) {
                output += "  PRIVATE:\n";
                output += print_vec("    ", priv);
            }
            if (!pub.empty()) {
                output += "  PUBLIC:\n";
                output += print_vec("    ", pub);
            }
            if (!iface.empty()) {
                output += "  INTERFACE:\n";
                output += print_vec("    ", iface);
            }
        }
    }

    // Print resolved properties (if resolved)
    output += "\n--- Resolved Properties (after dependency resolution) ---\n";
    for (const auto& prop : prop_names) {
        const auto& resolved = get_resolved_property(prop);
        if (!resolved.empty()) {
            output += "\n" + prop + " (for building this target):\n";
            output += print_vec("  ", resolved);
        }
    }

    output += "\n--- Resolved Interface Properties (propagated to dependents) ---\n";
    for (const auto& prop : prop_names) {
        const auto& resolved_iface = get_resolved_interface_property(prop);
        if (!resolved_iface.empty()) {
            output += "\n" + prop + " (for dependents):\n";
            output += print_vec("  ", resolved_iface);
        }
    }

    output += "\n";
    return output;
}

// --- File Set Support ---

void Target::add_file_set(FileSet file_set) {
    file_sets_.push_back(std::move(file_set));
}

bool Target::is_in_cxx_modules_file_set(const std::string& source) const {
    for (const auto& fs : file_sets_) {
        if (fs.type == "CXX_MODULES") {
            for (const auto& file : fs.files) {
                // Check if source matches (could be absolute or relative)
                std::filesystem::path src_path(source);
                std::filesystem::path fs_path(file);

                // Try direct match
                if (src_path == fs_path) return true;

                // Try filename match
                if (src_path.filename() == fs_path.filename()) return true;

                // Try resolved paths match
                std::filesystem::path src_abs = src_path.is_absolute() ?
                    src_path : (std::filesystem::path(source_dir_) / src_path);
                std::filesystem::path fs_abs = fs_path.is_absolute() ?
                    fs_path : (std::filesystem::path(source_dir_) / fs_path);

                if (src_abs.lexically_normal() == fs_abs.lexically_normal()) return true;
            }
        }
    }
    return false;
}

// --- Specific Property Helpers (Legacy Wrappers) ---

void Target::set_output_name(std::string output_name) {
    output_name_ = std::move(output_name);
}

const std::string& Target::get_output_name() const {
    return output_name_.empty() ? name_ : output_name_;
}

void Target::set_language_standard(Language lang, std::string standard) {
    standards_[lang] = std::move(standard);
}

const std::string& Target::get_language_standard(Language lang) const {
    static const std::string empty;
    auto it = standards_.find(lang);
    return (it != standards_.end()) ? it->second : empty;
}

void Target::set_language_extensions(Language lang, bool enabled) {
    extensions_enabled_[lang] = enabled;
}

bool Target::get_language_extensions(Language lang) const {
    auto it = extensions_enabled_.find(lang);
    // Default to true (extensions enabled) to match CMake behavior
    return (it != extensions_enabled_.end()) ? it->second : true;
}

void Target::add_language_flags(Language lang, const std::vector<std::string>& flags) {
    auto& target_flags = language_flags_[lang];
    target_flags.insert(target_flags.end(), flags.begin(), flags.end());
}

const std::vector<std::string>& Target::get_language_flags(Language lang) const {
    static const std::vector<std::string> empty;
    auto it = language_flags_.find(lang);
    return (it != language_flags_.end()) ? it->second : empty;
}

// --- Resolution Logic ---

GenexEvaluationContext Target::make_genex_context(
    const Target* current_target,
    const Interpreter& interp,
    const std::map<std::string, std::shared_ptr<Target>>& all_targets,
    std::optional<Language> compile_language,
    bool allow_deferred)
{
    GenexEvaluationContext ctx;
    ctx.build_type = interp.get_variable("CMAKE_BUILD_TYPE");
    ctx.system_name = interp.get_variable("CMAKE_SYSTEM_NAME");
    ctx.cxx_compiler_id = interp.get_variable("CMAKE_CXX_COMPILER_ID");
    ctx.c_compiler_id = interp.get_variable("CMAKE_C_COMPILER_ID");
    ctx.cxx_compiler_version = interp.get_variable("CMAKE_CXX_COMPILER_VERSION");
    ctx.c_compiler_version = interp.get_variable("CMAKE_C_COMPILER_VERSION");
    ctx.all_targets = &all_targets;
    ctx.target_aliases = &interp.get_target_aliases();
    ctx.current_target = current_target;
    ctx.install_prefix = interp.get_variable("CMAKE_INSTALL_PREFIX");
    ctx.phase = GenexEvaluationContext::Phase::BUILD;
    ctx.compile_language = compile_language;
    ctx.allow_deferred_compile_language = allow_deferred;
    ctx.source_properties = &interp.get_source_properties();
    return ctx;
}

// Order-preserving dedup merge: append items from source into target,
// skipping duplicates already present in target.
namespace {
void merge_dedup(std::vector<std::string>& target,
                 const std::vector<std::string>& source) {
    std::unordered_set<std::string> seen(target.begin(), target.end());
    for (const auto& s : source) {
        if (seen.insert(s).second) {
            target.push_back(s);
        }
    }
}
} // anonymous namespace

std::vector<Target::PropInfo> Target::build_props_to_resolve() {
    std::vector<PropInfo> result;
    for (const auto& meta : kListProperties) {
        if (meta.transitive && meta.name != "LINK_LIBRARIES") {
            result.push_back({std::string(meta.name), meta.is_path});
        }
    }
    return result;
}

std::string Target::resolve_to_absolute_path(const std::string& p) const {
    std::string_view sv = p;
    if (sv.starts_with("-I")) sv.remove_prefix(2);
    std::filesystem::path path(sv);
    if (path.is_absolute()) return path.string();
    return (std::filesystem::path(source_dir_) / path).lexically_normal().string();
}

void Target::initialize_local_properties(
    const std::vector<PropInfo>& props_to_resolve,
    GenexEvaluator& evaluator)
{
    for (const auto& info : props_to_resolve) {
        auto& res = resolved_properties_[info.name];
        auto& res_iface = resolved_interface_properties_[info.name];

        auto process_local = [&](PropertyVisibility vis, std::vector<std::string>& out) {
            const auto& val = get_property_list(info.name, vis);
            auto eval_result = evaluator.evaluate_property_list(val);
            if (!eval_result) {
                std::ostringstream oss;
                oss << "Error during build graph generation:\n"
                    << "  Target: '" << name_ << "'\n"
                    << "  Property: '" << info.name << "'\n"
                    << "  Error: " << eval_result.error();
                throw std::runtime_error(oss.str());
            }
            if (info.is_path) {
                for (const auto& p : *eval_result) {
                    if (!p.empty()) {
                        out.push_back(resolve_to_absolute_path(p));
                    }
                }
            } else {
                merge_dedup(out, *eval_result);
            }
        };

        // Own build properties: PRIVATE + PUBLIC
        process_local(PropertyVisibility::PRIVATE, res);
        process_local(PropertyVisibility::PUBLIC, res);

        // Interface properties: PUBLIC + INTERFACE
        process_local(PropertyVisibility::PUBLIC, res_iface);
        process_local(PropertyVisibility::INTERFACE, res_iface);
    }
}

const std::vector<std::string>& Target::collect_link_deps(const Target& dep) {
    if (dep.get_type() == TargetType::STATIC_LIBRARY && !dep.is_imported()) {
        return dep.get_resolved_property("LINK_LIBRARIES");
    }
    return dep.get_resolved_interface_property("LINK_LIBRARIES");
}

void Target::propagate_from_dependency(
    const Target& dep,
    const std::vector<PropInfo>& props_to_resolve,
    std::map<std::string, std::vector<std::string>>& output_props,
    bool skip_non_link)
{
    // 1. Merge non-link interface properties (includes, definitions, options, etc.)
    if (!skip_non_link) {
        for (const auto& info : props_to_resolve) {
            const auto& dep_iface = dep.get_resolved_interface_property(info.name);
            if (dep_iface.empty()) continue;
            if (dep.is_imported() && info.name == "INCLUDE_DIRECTORIES") {
                merge_dedup(output_props["SYSTEM_INCLUDE_DIRECTORIES"], dep_iface);
            } else {
                merge_dedup(output_props[info.name], dep_iface);
            }
        }
    }

    // 2. Add dependency's own artifact + transitive link deps to LINK_LIBRARIES
    auto& output_libs = output_props["LINK_LIBRARIES"];
    std::string dep_path = dep.get_output_path();
    if (!dep_path.empty()) {
        output_libs.push_back(std::move(dep_path));
    }
    merge_dedup(output_libs, collect_link_deps(dep));
}

void Target::handle_circular_dep(
    const Target& dep,
    bool is_public, bool is_interface_only,
    std::vector<std::string>& res_libs,
    std::vector<std::string>& res_iface_libs)
{
    if (dep.get_type() != TargetType::STATIC_LIBRARY &&
        dep.get_type() != TargetType::OBJECT_LIBRARY) {
        throw std::runtime_error(
            "Circular dependency detected involving target: " + dep.get_name());
    }
    dmake::print_message(std::cerr, "WARNING",
        "Circular dependency between static libraries '" + name_ +
        "' and '" + dep.get_name() + "'. Consider restructuring.");
    std::string dep_path = dep.get_output_path();
    if (!dep_path.empty()) {
        if (!is_interface_only) res_libs.push_back(dep_path);
        if (is_public || is_interface_only) res_iface_libs.push_back(dep_path);
    }
    deferred_circular_deps_.push_back(dep.get_name());
}

void Target::resolve_deferred_circular_deps(
    const std::map<std::string, std::shared_ptr<Target>>& all_targets)
{
    if (deferred_circular_deps_.empty()) return;

    auto props = build_props_to_resolve();
    for (const auto& dep_name : deferred_circular_deps_) {
        auto it = all_targets.find(dep_name);
        if (it == all_targets.end() || !it->second->is_resolved()) continue;
        const auto& dep = *it->second;
        for (const auto& info : props) {
            const auto& dep_iface = dep.get_resolved_interface_property(info.name);
            if (dep_iface.empty()) continue;
            if (dep.is_imported() && info.name == "INCLUDE_DIRECTORIES") {
                merge_dedup(resolved_properties_["SYSTEM_INCLUDE_DIRECTORIES"], dep_iface);
            } else {
                merge_dedup(resolved_properties_[info.name], dep_iface);
            }
        }
    }
    deferred_circular_deps_.clear();
}

void Target::resolve(const std::map<std::string, std::shared_ptr<Target>>& all_targets, const Interpreter& interp) {
    if (resolved_) return;
    if (visiting_) throw std::runtime_error("Circular dependency detected involving target: " + name_);
    visiting_ = true;

    auto props_to_resolve = build_props_to_resolve();
    auto genex_ctx = make_genex_context(this, interp, all_targets, std::nullopt, true);
    GenexEvaluator evaluator(genex_ctx);

    // Phase 1: Own properties
    initialize_local_properties(props_to_resolve, evaluator);

    // Phase 2: Walk dependency graph via LINK_LIBRARIES
    auto& res_libs = resolved_properties_["LINK_LIBRARIES"];
    auto& res_iface_libs = resolved_interface_properties_["LINK_LIBRARIES"];

    auto process_one_dep = [&](const std::string& raw_lib_name,
                               bool is_public, bool is_interface_only,
                               bool link_only) {
        std::string lib_name = interp.resolve_target_alias(raw_lib_name);
        auto dep_it = all_targets.find(lib_name);

        if (dep_it == all_targets.end()) {
            // System library or raw file path
            if (!is_interface_only) res_libs.push_back(lib_name);
            if (is_public || is_interface_only) res_iface_libs.push_back(lib_name);
            return;
        }

        auto& dep = dep_it->second;

        // Skip self-references (e.g. link_libraries(Threads::Threads) applied back
        // to Threads::Threads itself via finalize_directory_targets)
        if (dep.get() == this) return;

        resolved_target_deps_.push_back(lib_name);

        if (dep->is_visiting()) {
            handle_circular_dep(*dep, is_public, is_interface_only,
                                res_libs, res_iface_libs);
            return;
        }

        dep->resolve(all_targets, interp);

        // For building THIS target (PRIVATE or PUBLIC dep)
        if (!is_interface_only) {
            propagate_from_dependency(*dep, props_to_resolve,
                resolved_properties_, /*skip_non_link=*/link_only);
            if (dep->get_type() == TargetType::OBJECT_LIBRARY) {
                resolved_object_lib_deps_.push_back(lib_name);
            }
            if (dep->get_type() == TargetType::STATIC_LIBRARY && !dep->is_imported()) {
                merge_dedup(resolved_object_lib_deps_, dep->resolved_object_lib_deps_);
            }
        }

        // For propagating to OUR dependents (PUBLIC or INTERFACE dep)
        if (is_public || is_interface_only) {
            propagate_from_dependency(*dep, props_to_resolve,
                resolved_interface_properties_, /*skip_non_link=*/link_only);
        }
    };

    auto walk_link_libs = [&](PropertyVisibility vis, bool is_public, bool is_interface_only) {
        for (const auto& lib : get_property_list("LINK_LIBRARIES", vis)) {
            auto eval_result = evaluator.evaluate_link_library(lib);
            if (!eval_result) {
                throw std::runtime_error("Error evaluating LINK_LIBRARIES for target '"
                    + name_ + "': " + eval_result.error());
            }
            if (!eval_result->value.empty()) {
                process_one_dep(eval_result->value, is_public,
                               is_interface_only, eval_result->link_only);
            }
        }
    };
    walk_link_libs(PropertyVisibility::PUBLIC, true, false);
    walk_link_libs(PropertyVisibility::PRIVATE, false, false);
    walk_link_libs(PropertyVisibility::INTERFACE, false, true);

    // Phase 2b: Discover OBJECT library deps from $<TARGET_OBJECTS:X> in SOURCES.
    // DuckDB (and others) pass object libraries this way instead of via
    // target_link_libraries(), so we must resolve them here to ensure their
    // compile tasks get generated.
    {
        static constexpr std::string_view prefix = "$<TARGET_OBJECTS:";
        for (auto vis : {PropertyVisibility::PRIVATE, PropertyVisibility::PUBLIC, PropertyVisibility::INTERFACE}) {
            for (const auto& src : get_property_list("SOURCES", vis)) {
                std::string_view sv(src);
                std::string_view::size_type pos = 0;
                while ((pos = sv.find(prefix, pos)) != std::string_view::npos) {
                    auto name_start = pos + prefix.size();
                    auto name_end = sv.find('>', name_start);
                    if (name_end == std::string_view::npos) break;
                    std::string obj_target_name(sv.substr(name_start, name_end - name_start));
                    pos = name_end + 1;

                    std::string resolved_name = interp.resolve_target_alias(obj_target_name);
                    auto dep_it = all_targets.find(resolved_name);
                    if (dep_it == all_targets.end()) continue;

                    auto& dep = dep_it->second;
                    resolved_target_deps_.push_back(resolved_name);
                    dep->resolve(all_targets, interp);
                    resolved_object_lib_deps_.push_back(resolved_name);
                }
            }
        }
    }

    // Phase 3: Deduplicate non-link properties
    for (const auto& info : props_to_resolve) {
        remove_duplicates(resolved_properties_[info.name]);
        remove_duplicates(resolved_interface_properties_[info.name]);
    }

    visiting_ = false;
    resolved_ = true;
}

// --- Task Generation ---

std::string Target::get_output_path() const {
    // Interface libraries have no linkable output - they only propagate properties
    if (type_ == TargetType::INTERFACE_LIBRARY) {
        return "";
    }

    if (is_imported_ && !imported_location_.empty()) {
        return imported_location_;
    }

    std::string out_name = get_output_name();
    std::filesystem::path path;

    // Determine output directory: per-target property overrides binary_dir_.
    // CMake mapping: EXECUTABLE → RUNTIME, STATIC → ARCHIVE, SHARED → LIBRARY.
    std::string output_dir;
    if (type_ == TargetType::EXECUTABLE) {
        output_dir = get_property("RUNTIME_OUTPUT_DIRECTORY");
    } else if (type_ == TargetType::STATIC_LIBRARY) {
        output_dir = get_property("ARCHIVE_OUTPUT_DIRECTORY");
    } else if (type_ == TargetType::SHARED_LIBRARY) {
        output_dir = get_property("LIBRARY_OUTPUT_DIRECTORY");
    }
    const auto& dir = output_dir.empty() ? binary_dir_ : output_dir;

    // Determine prefix and suffix, respecting target properties
    std::string prefix_prop = get_property("PREFIX");
    std::string suffix_prop = get_property("SUFFIX");
    bool has_prefix = !prefix_prop.empty() || properties_.count("PREFIX");
    bool has_suffix = !suffix_prop.empty() || properties_.count("SUFFIX");

    if (type_ == TargetType::EXECUTABLE) {
        std::string prefix = has_prefix ? prefix_prop : "";
        std::string suffix = has_suffix ? suffix_prop : "";
        path = std::filesystem::path(dir) / (prefix + out_name + suffix);
    } else if (type_ == TargetType::SHARED_LIBRARY) {
        std::string prefix = has_prefix ? prefix_prop : "lib";
        std::string suffix = has_suffix ? suffix_prop : ".so";
        path = std::filesystem::path(dir) / (prefix + out_name + suffix);
    } else if (type_ == TargetType::STATIC_LIBRARY) {
        std::string prefix = has_prefix ? prefix_prop : "lib";
        std::string suffix = has_suffix ? suffix_prop : ".a";
        path = std::filesystem::path(dir) / (prefix + out_name + suffix);
    } else {
        return "";
    }

    return dir.empty() ? path.string() : path.lexically_normal().string();
}

// Strip trailing slashes to normalize include paths for comparison.
// Prevents mismatches like "/usr/include" vs "/usr/include/" when filtering implicit includes.
static std::string normalize_include(const std::string& dir) {
    auto end = dir.size();
    while (end > 1 && dir[end - 1] == '/') --end;
    return dir.substr(0, end);
}

std::string get_obj_path(const std::string& binary_dir, const std::string& target_name, const std::string& source_path) {
    std::filesystem::path src(source_path);
    std::filesystem::path obj_suffix;

    if (src.is_absolute()) {
        // Use full path structure (minus root) to avoid collisions between
        // files with same name in different directories (e.g. posix/file.c vs file.c)
        obj_suffix = src.relative_path();
    } else {
        obj_suffix = src;
    }

    std::filesystem::path obj = std::filesystem::path(binary_dir) / "objs" / target_name / obj_suffix;
    obj += ".o";
    return binary_dir.empty() ? obj.string() : obj.lexically_normal().string();
}

// Resolve executable target names in the first argument of COMMAND clauses.
// CMake replaces bare target names with the built binary path and adds an implicit dependency.
static void resolve_command_target_references(
    std::vector<std::vector<std::string>>& commands,
    BuildTask& task,
    const std::map<std::string, std::shared_ptr<Target>>& all_targets)
{
    for (auto& cmd : commands) {
        if (cmd.empty()) continue;

        std::shared_ptr<Target> resolved;

        // Direct target name lookup (executables only)
        auto it = all_targets.find(cmd[0]);
        if (it != all_targets.end() && it->second->get_type() == TargetType::EXECUTABLE) {
            resolved = it->second;
        }

        // Fall back: executable whose OUTPUT_NAME or output path matches the command
        if (!resolved) {
            static const std::map<std::string, std::shared_ptr<Target>>* cached_source = nullptr;
            static std::unordered_map<std::string, std::shared_ptr<Target>> output_name_map;
            static std::unordered_map<std::string, std::shared_ptr<Target>> output_path_map;
            if (&all_targets != cached_source) {
                cached_source = &all_targets;
                output_name_map.clear();
                output_path_map.clear();
                for (const auto& [name, target] : all_targets) {
                    if (target->get_type() == TargetType::EXECUTABLE) {
                        output_name_map.emplace(target->get_output_name(), target);
                        auto path = target->get_output_path();
                        if (!path.empty()) output_path_map.emplace(path, target);
                    }
                }
            }
            auto oit = output_name_map.find(cmd[0]);
            if (oit != output_name_map.end())
                resolved = oit->second;
            if (!resolved) {
                auto pit = output_path_map.find(cmd[0]);
                if (pit != output_path_map.end())
                    resolved = pit->second;
            }
        }

        if (resolved) {
            std::string output = resolved->get_output_path();
            if (!output.empty()) {
                cmd[0] = output;
                task.explicit_deps.push_back(output);
            }
        }
    }
}

// Helper to generate a task for a custom command rule.
// Recursively generates tasks for any deps that are themselves custom command outputs.
static void generate_custom_command_task(GraphTransaction& txn, const CustomCommandRule& rule,
                                         const std::map<std::string, std::shared_ptr<Target>>& all_targets,
                                         const std::map<std::string, std::shared_ptr<CustomCommandRule>>& custom_rules,
                                         std::set<std::string>& generated) {
    if (generated.count(rule.outputs[0]) || txn.has_task(rule.outputs[0]))
        return;
    generated.insert(rule.outputs[0]);

    BuildTask task;
    task.id = rule.outputs[0];
    task.kind = CustomCommandTask{};
    task.working_dir = rule.working_dir;

    for (const auto& cmd : rule.commands) {
        task.commands.push_back(cmd);
    }

    resolve_command_target_references(task.commands, task, all_targets);

    for (const auto& out : rule.outputs) {
        task.outputs.push_back(out);
    }

    for (const auto& dep : rule.depends) {
        auto dep_it = all_targets.find(dep);
        if (dep_it != all_targets.end()) {
            std::string dep_out = dep_it->second->get_output_path();
            if (!dep_out.empty()) {
                task.explicit_deps.push_back(dep_out);
                task.inputs.push_back(dep_out);
            } else {
                task.explicit_deps.push_back(dep);
            }
        } else {
            std::filesystem::path p(dep);
            std::string normalized;
            if (p.is_absolute()) {
                normalized = p.lexically_normal().string();
            } else {
                // Try source dir first, then binary dir (custom command outputs are in binary dir)
                normalized = (std::filesystem::path(rule.source_dir) / dep).lexically_normal().string();
            }

            // Check if a custom command rule produces this file
            auto cc_it = custom_rules.find(normalized);
            if (cc_it == custom_rules.end() && !p.is_absolute()) {
                // Fallback: check binary dir (custom command outputs are registered there)
                auto bin_normalized = (std::filesystem::path(rule.binary_dir) / dep).lexically_normal().string();
                cc_it = custom_rules.find(bin_normalized);
                if (cc_it != custom_rules.end()) {
                    normalized = bin_normalized;
                }
            }
            if (cc_it != custom_rules.end()) {
                generate_custom_command_task(txn, *cc_it->second, all_targets, custom_rules, generated);
                task.explicit_deps.push_back(cc_it->second->outputs[0]);
            }
            task.inputs.push_back(normalized);
        }
    }

    txn.add(std::move(task));
}

void Target::generate_object_tasks(GraphTransaction& txn, const Toolchain& toolchain, std::vector<std::string>& obj_files,
                                      const std::string& pch_gch_path, const std::string& pch_include_arg,
                                      bool is_shared, bool is_pie, const std::map<std::string, std::shared_ptr<Target>>& all_targets,
                                      GenexEvaluator& evaluator, const Interpreter& interp,
                                      const std::string& pre_build_task_id,
                                      const std::string& module_mapper_path,
                                      std::set<std::string>& generated_custom_tasks,
                                      const std::set<std::string>& implicit_includes) {
    // --- Hoist all loop-invariant computations ---

    bool target_has_modules = has_module_sources();

    const auto& source_props = interp.get_source_properties();
    const auto& custom_rules = interp.get_custom_command_rules();

    // Cache isatty result (syscall)
    const bool color_diag = isatty(STDOUT_FILENO);

    // Pre-compute compile features standard requirement (same for all sources)
    const auto& compile_features = get_resolved_property("COMPILE_FEATURES");
    int cxx_required_std = 0;
    int c_required_std = 0;
    if (!compile_features.empty()) {
        const auto& features_db = CompileFeatures::instance();
        cxx_required_std = features_db.get_required_standard(compile_features, Language::CXX);
        c_required_std = features_db.get_required_standard(compile_features, Language::C);
    }

    // Read compiler default standards (to suppress unnecessary -std= flags)
    int cxx_default_std = 0;
    int c_default_std = 0;
    {
        const std::string& cxx_def = interp.get_variable("CMAKE_CXX_STANDARD_DEFAULT");
        if (!cxx_def.empty()) cxx_default_std = std::stoi(cxx_def);
        const std::string& c_def = interp.get_variable("CMAKE_C_STANDARD_DEFAULT");
        if (!c_def.empty()) c_default_std = std::stoi(c_def);
    }

    // Compute effective standard per language (hoisted out of per-source loop)
    auto effective_standard = [&](Language lang) -> std::string {
        if (lang == Language::ASM) return "";
        int required = (lang == Language::CXX) ? cxx_required_std : c_required_std;
        int compiler_default = (lang == Language::CXX) ? cxx_default_std : c_default_std;
        return compute_effective_standard(get_language_standard(lang), required, compiler_default);
    };

    // Pre-resolve manual dependencies (same for every source)
    struct ResolvedDep { std::string id; };
    std::vector<ResolvedDep> resolved_manual_deps;
    for (const auto& dep_name : manually_added_dependencies_) {
        auto it = all_targets.find(dep_name);
        if (it != all_targets.end()) {
            std::string dep_out = it->second->get_output_path();
            resolved_manual_deps.push_back({dep_out.empty() ? dep_name : std::move(dep_out)});
        }
    }

    // Pre-build CXX_MODULES file set for O(1) lookups instead of O(N*M) per-source
    std::unordered_set<std::string> cxx_module_files;
    for (const auto& fs : file_sets_) {
        if (fs.type == "CXX_MODULES") {
            for (const auto& file : fs.files) {
                std::filesystem::path fs_path(file);
                std::filesystem::path fs_abs = fs_path.is_absolute() ?
                    fs_path : (std::filesystem::path(source_dir_) / fs_path);
                cxx_module_files.insert(fs_abs.lexically_normal().string());
                // Also add filename for filename-only matches
                cxx_module_files.insert(fs_path.filename().string());
            }
        }
    }

    // Pre-check whether any resolved property contains deferred COMPILE_LANG genex.
    // If not (common case), we skip per-source evaluation and vector copies entirely.
    const auto& resolved_includes = get_resolved_property("INCLUDE_DIRECTORIES");
    const auto& resolved_sys_includes = get_resolved_property("SYSTEM_INCLUDE_DIRECTORIES");
    const auto& resolved_definitions = get_resolved_property("COMPILE_DEFINITIONS");
    const auto& resolved_options = get_resolved_property("COMPILE_OPTIONS");

    auto has_deferred_genex = [](const std::vector<std::string>& vals) {
        return std::any_of(vals.begin(), vals.end(), [](const std::string& v) {
            return v.find("$<COMPILE_LANG") != std::string::npos;
        });
    };
    bool needs_per_lang_eval = has_deferred_genex(resolved_includes) ||
                               has_deferred_genex(resolved_sys_includes) ||
                               has_deferred_genex(resolved_definitions) ||
                               has_deferred_genex(resolved_options);

    // If we have COMPILE_LANG genex, pre-evaluate for C and CXX once (not per-source)
    struct PerLangProperties {
        std::vector<std::string> includes;
        std::vector<std::string> system_includes;
        std::vector<std::string> definitions;
        std::vector<std::string> options;
    };
    std::map<Language, PerLangProperties> per_lang_props;

    // Base genex context for per-source evaluator (used for source-property genex)
    auto source_genex_base = make_genex_context(this, interp, all_targets);

    if (needs_per_lang_eval) {
        auto eval_for_lang = [&](const std::vector<std::string>& values, GenexEvaluator& lang_eval) {
            std::vector<std::string> result;
            for (const auto& val : values) {
                if (val.find("$<COMPILE_LANG") != std::string::npos) {
                    auto eval_result = lang_eval.evaluate(val);
                    if (eval_result && !eval_result->empty()) {
                        // Genex may produce semicolon-separated lists
                        for (auto sv : CMakeArrayView(*eval_result)) {
                            result.emplace_back(sv);
                        }
                    }
                } else {
                    result.push_back(val);
                }
            }
            return result;
        };

        for (Language lang : {Language::C, Language::CXX, Language::ASM}) {
            GenexEvaluationContext lang_ctx = source_genex_base;
            lang_ctx.compile_language = lang;
            GenexEvaluator lang_evaluator(lang_ctx);

            auto& props = per_lang_props[lang];
            props.includes = eval_for_lang(resolved_includes, lang_evaluator);
            props.system_includes = eval_for_lang(resolved_sys_includes, lang_evaluator);
            props.definitions = eval_for_lang(resolved_definitions, lang_evaluator);
            props.options = eval_for_lang(resolved_options, lang_evaluator);
        }
    }

    // Module mapper path (computed once, used if target has modules)
    std::string module_mapper = target_has_modules ? get_module_mapper_path() : std::string{};

    // --- Evaluate genex in source paths (single pass over sources) ---
    auto own_sources = get_property_list("SOURCES", TargetPropertyScope::BUILD);
    auto evaluated_sources_result = evaluator.evaluate_property_list(own_sources);
    if (!evaluated_sources_result) {
        throw std::runtime_error("Error evaluating genex in SOURCES for target '" + name_ + "': " + evaluated_sources_result.error());
    }

    // Pre-scan: discover custom command dependencies from all sources (including
    // generated headers like bison .hh files) before emitting compilation tasks.
    // This ensures that even if a generated header appears after a .cc source in
    // the source list, its custom command is wired as a dependency for all tasks.
    for (const auto& src : *evaluated_sources_result) {
        if (src.empty()) continue;
        std::filesystem::path src_path(src);
        std::string norm_src, norm_bin;
        if (src_path.is_absolute()) {
            norm_src = norm_bin = src_path.lexically_normal().string();
        } else {
            norm_src = (std::filesystem::path(source_dir_) / src_path).lexically_normal().string();
            norm_bin = (std::filesystem::path(binary_dir_) / src_path).lexically_normal().string();
        }
        auto cc_it = custom_rules.find(norm_src);
        if (cc_it == custom_rules.end()) cc_it = custom_rules.find(norm_bin);
        if (cc_it != custom_rules.end()) {
            if (!generated_custom_tasks.count(cc_it->second->outputs[0])) {
                generate_custom_command_task(txn, *cc_it->second, all_targets, custom_rules, generated_custom_tasks);
            }
            resolved_manual_deps.push_back({cc_it->second->outputs[0]});
        }
    }

    std::unordered_set<std::string> seen_sources;
    for (const auto& src : *evaluated_sources_result) {
        if (src.empty()) continue;

        // Handle pre-compiled object files
        std::filesystem::path src_path(src);
        if (src_path.extension() == ".o" || src_path.extension() == ".obj") {
            obj_files.push_back(src);
            continue;
        }

        // Determine if source is relative to source_dir or binary_dir
        std::filesystem::path src_abs;
        std::string src_normalized;
        // For custom command discovery, we need both normalizations of relative paths
        std::string normalized_src_dir, normalized_bin_dir;

        if (src_path.is_absolute()) {
            src_abs = src_path;
            src_normalized = src_path.lexically_normal().string();
            normalized_src_dir = src_normalized;
            normalized_bin_dir = src_normalized;
        } else {
            normalized_bin_dir = (std::filesystem::path(binary_dir_) / src_path).lexically_normal().string();
            normalized_src_dir = (std::filesystem::path(source_dir_) / src_path).lexically_normal().string();

            if (custom_rules.find(normalized_bin_dir) != custom_rules.end()) {
                src_abs = std::filesystem::path(binary_dir_) / src_path;
                src_normalized = normalized_bin_dir;
            } else {
                src_abs = std::filesystem::path(source_dir_) / src_path;
                src_normalized = normalized_src_dir;
            }
        }

        // Deduplicate sources that resolve to the same file
        // (e.g. same file added as both relative and absolute path)
        if (!seen_sources.insert(src_normalized).second) continue;

        // Look up per-source properties
        auto sp_it = source_props.find(src_normalized);

        // Check HEADER_FILE_ONLY
        if (sp_it != source_props.end()) {
            auto hfo_it = sp_it->second.find("HEADER_FILE_ONLY");
            if (hfo_it != sp_it->second.end() && !Interpreter::is_falsy(hfo_it->second)) {
                continue;
            }
        }

        auto lang_info = LanguageClassifier::from_path(src);
        if (lang_info.is_header) continue;

        // Check LANGUAGE property override
        if (sp_it != source_props.end()) {
            auto lang_it = sp_it->second.find("LANGUAGE");
            if (lang_it != sp_it->second.end()) {
                if (lang_it->second == "CXX" || lang_it->second == "C++") {
                    lang_info.lang = Language::CXX;
                    lang_info.is_header = false;
                } else if (lang_it->second == "C") {
                    lang_info.lang = Language::C;
                    lang_info.is_header = false;
                } else if (lang_it->second == "ASM") {
                    lang_info.lang = Language::ASM;
                    lang_info.is_header = false;
                }
            }
        }

        if (lang_info.lang == Language::UNKNOWN) continue;

        // Check CXX_MODULES using pre-built set (O(1) vs O(N*M))
        if (!cxx_module_files.empty()) {
            if (cxx_module_files.count(src_normalized) ||
                cxx_module_files.count(src_path.filename().string())) {
                lang_info.is_module_interface = true;
            }
        }

        const Compiler* compiler = toolchain.get_compiler_ptr(lang_info.lang);
        if (!compiler) {
            throw std::runtime_error("No compiler available for language " + std::string(lang_info.name) + " in target '" + name_ + "'");
        }

        std::string obj = get_obj_path(binary_dir_, name_, src);
        obj_files.push_back(obj);

        std::string src_abs_str = src_abs.string();

        CompileContext ctx;
        ctx.source = src_abs_str;
        ctx.output = obj;
        ctx.is_shared = is_shared;
        ctx.is_pie = is_pie;
        ctx.pch_include = (lang_info.lang == Language::ASM) ? "" : pch_include_arg;
        ctx.standard = effective_standard(lang_info.lang);
        ctx.extensions_enabled = get_language_extensions(lang_info.lang);
        ctx.color_diagnostics = color_diag;

        // Visibility preset (per-language property)
        {
            std::string vis = get_property(std::string(lang_info.name) + "_VISIBILITY_PRESET");
            if (!vis.empty()) ctx.visibility_preset = vis;

            // Inline visibility hiding (only meaningful for C++/ObjC++)
            if (lang_info.lang == Language::CXX) {
                std::string vih_val = get_property("VISIBILITY_INLINES_HIDDEN");
                if (!vih_val.empty() && !Interpreter::is_falsy(vih_val)) ctx.visibility_inlines_hidden = true;
            }
        }

        if (lang_info.lang != Language::ASM && (lang_info.is_module_interface || target_has_modules)) {
            ctx.is_module_source = true;
            ctx.module_mapper_file = module_mapper;
        }

        for (const auto& opt : get_language_flags(lang_info.lang)) ctx.options.push_back(opt);

        // Apply resolved properties — no copies in the common case (no COMPILE_LANG genex)
        if (needs_per_lang_eval) {
            auto pl_it = per_lang_props.find(lang_info.lang);
            if (pl_it != per_lang_props.end()) {
                for (const auto& dir : pl_it->second.includes) {
                    if (!implicit_includes.contains(normalize_include(dir))) ctx.includes.push_back(dir);
                }
                for (const auto& dir : pl_it->second.system_includes) {
                    if (!implicit_includes.contains(normalize_include(dir))) ctx.system_includes.push_back(dir);
                }
                for (const auto& def : pl_it->second.definitions) ctx.definitions.push_back(def);
                for (const auto& opt : pl_it->second.options) ctx.options.push_back(opt);
            }
        } else {
            for (const auto& dir : resolved_includes) {
                if (!implicit_includes.contains(normalize_include(dir))) ctx.includes.push_back(dir);
            }
            for (const auto& dir : resolved_sys_includes) {
                if (!implicit_includes.contains(normalize_include(dir))) ctx.system_includes.push_back(dir);
            }
            for (const auto& def : resolved_definitions) ctx.definitions.push_back(def);
            for (const auto& opt : resolved_options) ctx.options.push_back(opt);
        }

        // Apply target-level COMPILE_FLAGS property (deprecated but still used)
        {
            std::string target_cf = get_property("COMPILE_FLAGS");
            if (!target_cf.empty()) {
                std::istringstream iss(target_cf);
                std::string flag;
                while (iss >> flag) {
                    ctx.options.push_back(flag);
                }
            }
        }

        // Apply per-source properties
        if (sp_it != source_props.end()) {
            auto cf_it = sp_it->second.find("COMPILE_FLAGS");
            if (cf_it != sp_it->second.end()) {
                std::istringstream iss(cf_it->second);
                std::string flag;
                while (iss >> flag) {
                    ctx.options.push_back(flag);
                }
            }

            auto co_it = sp_it->second.find("COMPILE_OPTIONS");
            if (co_it != sp_it->second.end()) {
                // Lazily construct per-source evaluator only if genex present
                std::optional<GenexEvaluator> src_eval_storage;
                for (auto sv : CMakeArrayView(co_it->second)) {
                    std::string opt(sv);
                    if (GenexParser::contains_genex(opt)) {
                        if (!src_eval_storage) {
                            GenexEvaluationContext ctx_copy = source_genex_base;
                            ctx_copy.compile_language = lang_info.lang;
                            src_eval_storage.emplace(ctx_copy);
                        }
                        auto result = src_eval_storage->evaluate(opt);
                        if (result && !result->empty()) {
                            // Genex may produce semicolon-separated lists
                            for (auto sv : CMakeArrayView(*result)) {
                                ctx.options.emplace_back(sv);
                            }
                        }
                    } else {
                        ctx.options.push_back(std::move(opt));
                    }
                }
            }

            auto cd_it = sp_it->second.find("COMPILE_DEFINITIONS");
            if (cd_it != sp_it->second.end()) {
                for (auto sv : CMakeArrayView(cd_it->second)) {
                    ctx.definitions.emplace_back(sv);
                }
            }

            auto id_it = sp_it->second.find("INCLUDE_DIRECTORIES");
            if (id_it != sp_it->second.end()) {
                for (auto sv : CMakeArrayView(id_it->second)) {
                    ctx.includes.emplace_back(sv);
                }
            }
        }

        BuildTask task;
        task.id = obj;
        task.kind = CompileTask{src_abs_str, lang_info.lang};
        task.parent_target = this;
        task.commands.push_back(compiler->get_compile_command(ctx));
        task.inputs.push_back(src_abs_str);
        task.outputs.push_back(obj);
        task.outputs.push_back(obj + ".d");

        if (!pch_gch_path.empty()) {
            task.explicit_deps.push_back(pch_gch_path);
            task.inputs.push_back(pch_gch_path);
        }

        // Pre-resolved manual dependencies (hoisted)
        for (const auto& dep : resolved_manual_deps) {
            task.explicit_deps.push_back(dep.id);
        }

        // OBJECT_DEPENDS
        if (sp_it != source_props.end()) {
            auto od_it = sp_it->second.find("OBJECT_DEPENDS");
            if (od_it != sp_it->second.end()) {
                for (auto sv : CMakeArrayView(od_it->second)) {
                    std::filesystem::path dep_path(sv);
                    if (!dep_path.is_absolute()) {
                        dep_path = std::filesystem::path(source_dir_) / dep_path;
                    }
                    task.inputs.push_back(dep_path.lexically_normal().string());
                }
            }
        }

        // --- Dependency wiring (merged from generate_tasks) ---

        // Depend on PRE_BUILD task
        if (!pre_build_task_id.empty()) {
            task.explicit_deps.push_back(pre_build_task_id);
        }

        // Depend on custom command that generates this source
        {
            auto cc_it = custom_rules.find(src_normalized);
            if (cc_it != custom_rules.end()) {
                task.explicit_deps.push_back(cc_it->second->outputs[0]);
                task.inputs.push_back(cc_it->second->outputs[0]);
            }
        }

        // Module mapper dependency
        if (!module_mapper_path.empty()) {
            task.explicit_deps.push_back(module_mapper_path);
            task.inputs.push_back(module_mapper_path);
        }

        txn.add(std::move(task));
    }
}

static std::pair<std::string, std::string> generate_pch_task(
    GraphTransaction& txn,
    const Toolchain& toolchain,
    const Target* target,
    bool is_shared,
    const std::vector<std::string>& includes,
    const std::vector<std::string>& system_includes,
    const std::vector<std::string>& definitions,
    const std::vector<std::string>& options,
    int compiler_default_standard) {

    // Using PCH property name "PRECOMPILE_HEADERS"
    auto own_pchs = target->get_property_list("PRECOMPILE_HEADERS", TargetPropertyScope::BUILD);

    if (own_pchs.empty()) {
        return {"", ""};
    }

    Language pch_lang = Language::CXX;
    const Compiler* compiler = toolchain.get_compiler_ptr(pch_lang);
    if (!compiler) {
        throw std::runtime_error("No compiler available for PCH generation in target '" + target->get_name() + "'");
    }

    auto pch_path = std::filesystem::path(target->get_binary_dir()) / "objs" / (target->get_name() + "_pch.hpp");
    std::string pch_wrapper = target->get_binary_dir().empty() ? pch_path.string() : pch_path.lexically_normal().string();
    std::string pch_gch_path = pch_wrapper + ".gch";
    std::string pch_include_arg = " -include " + pch_wrapper;

    std::ostringstream wrapper_content;
    for (const auto& hdr : own_pchs) wrapper_content << "#include \"" << hdr << "\"\n";
    std::string content = wrapper_content.str();

    bool needs_write = true;
    if (std::filesystem::exists(pch_wrapper)) {
        std::ifstream existing(pch_wrapper);
        if (existing) {
            std::string existing_content((std::istreambuf_iterator<char>(existing)), std::istreambuf_iterator<char>());
            if (existing_content == content) {
                needs_write = false;
            }
        }
    }

    if (needs_write) {
        std::filesystem::create_directories(std::filesystem::path(pch_wrapper).parent_path());
        std::ofstream wrapper_file(pch_wrapper);
        if (!wrapper_file) {
            throw std::runtime_error("Failed to create PCH wrapper file: " + pch_wrapper);
        }
        wrapper_file << content;
    }

    BuildTask pch_task;
    pch_task.id = pch_gch_path;
    pch_task.kind = PCHTask{pch_wrapper};
    pch_task.parent_target = const_cast<Target*>(target);

    CompileContext ctx;
    ctx.source = pch_wrapper;
    ctx.output = pch_gch_path;
    ctx.is_shared = is_shared;

    // Determine standard: use highest of explicit standard or required by compile features
    {
        int required_std = 0;
        const auto& cf = target->get_resolved_property("COMPILE_FEATURES");
        if (!cf.empty()) {
            required_std = CompileFeatures::instance().get_required_standard(cf, pch_lang);
        }
        ctx.standard = compute_effective_standard(
            target->get_language_standard(pch_lang), required_std, compiler_default_standard);
    }
    ctx.extensions_enabled = target->get_language_extensions(pch_lang);
    ctx.color_diagnostics = isatty(STDOUT_FILENO);
    ctx.options.push_back("-x");
    ctx.options.push_back("c++-header");

    for (const auto& opt : target->get_language_flags(pch_lang)) ctx.options.push_back(opt);
    for (const auto& opt : options) ctx.options.push_back(opt);
    for (const auto& def : definitions) ctx.definitions.push_back(def);

    // PCH wrapper includes headers relative to source_dir, so we need source_dir in the include path
    ctx.includes.push_back(target->get_source_dir());

    for (const auto& dir : includes)
        ctx.includes.push_back(dir);
    for (const auto& dir : system_includes)
        ctx.system_includes.push_back(dir);

    pch_task.commands.push_back(compiler->get_compile_command(ctx));
    pch_task.inputs.push_back(pch_wrapper);

    for (const auto& hdr : own_pchs) {
        auto hdr_abs = std::filesystem::path(hdr).is_absolute() ?
            std::filesystem::path(hdr) :
            std::filesystem::path(target->get_source_dir()) / hdr;
        pch_task.inputs.push_back(hdr_abs.lexically_normal().string());
    }

    pch_task.outputs.push_back(pch_gch_path);

    txn.add(std::move(pch_task));

    return {pch_gch_path, pch_include_arg};
}

void Target::generate_tasks(GraphTransaction& txn, const Toolchain& toolchain, const std::map<std::string, std::shared_ptr<Target>>& all_targets, const Interpreter& interp, const std::vector<std::string>& exe_linker_flags, const std::vector<std::string>& shared_linker_flags) {
    if (type_ == TargetType::INTERFACE_LIBRARY || is_imported_) return;

    resolve(all_targets, interp);

    // Set up genex evaluator for SOURCES
    auto genex_ctx = make_genex_context(this, interp, all_targets);
    GenexEvaluator evaluator(genex_ctx);

    // Get implicit include directories to filter out (CMake doesn't pass these to compiler)
    // These are directories already in the compiler's default search path
    std::set<std::string> implicit_includes;
    for (const auto& dir : CMakeArray(interp.get_variable("CMAKE_C_IMPLICIT_INCLUDE_DIRECTORIES"))) {
        implicit_includes.emplace(normalize_include(dir));
    }
    for (const auto& dir : CMakeArray(interp.get_variable("CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES"))) {
        implicit_includes.emplace(normalize_include(dir));
    }

    std::vector<std::string> obj_files;
    bool is_shared = (type_ == TargetType::SHARED_LIBRARY);
    bool is_pie = false;

    // Check POSITION_INDEPENDENT_CODE property
    std::string pic_prop = get_property("POSITION_INDEPENDENT_CODE");
    if (!pic_prop.empty() && !Interpreter::is_falsy(pic_prop)) {
        if (type_ == TargetType::EXECUTABLE) {
            is_pie = true;   // Executables use -fPIE
        } else {
            is_shared = true; // Libraries use -fPIC
        }
    }

    // Track which custom command tasks we've created
    std::set<std::string> generated_custom_tasks;

    // Generate PRE_BUILD task if we have any pre-build commands
    std::string pre_build_task_id;
    if (!pre_build_commands_.empty()) {
        pre_build_task_id = name_ + "_pre_build";
        BuildTask pre_build;
        pre_build.id = pre_build_task_id;
        pre_build.kind = PreBuildTask{};
        pre_build.parent_target = this;
        pre_build.always_run = true;
        pre_build.working_dir = binary_dir_;

        for (const auto& cmd : pre_build_commands_) {
            pre_build.commands.push_back(cmd.command);
            if (!cmd.working_dir.empty()) {
                pre_build.working_dir = cmd.working_dir;
            }
        }

        resolve_command_target_references(pre_build.commands, pre_build, all_targets);

        txn.add(std::move(pre_build));
    }

    // Read compiler default standards (for suppressing unnecessary -std= flags)
    int cxx_default_std = 0;
    int c_default_std = 0;
    {
        const std::string& cxx_def = interp.get_variable("CMAKE_CXX_STANDARD_DEFAULT");
        if (!cxx_def.empty()) cxx_default_std = std::stoi(cxx_def);
        const std::string& c_def = interp.get_variable("CMAKE_C_STANDARD_DEFAULT");
        if (!c_def.empty()) c_default_std = std::stoi(c_def);
    }

    // C++20 modules: generate scanner tasks first (they have no dependencies)
    bool has_modules = generate_module_scanner_tasks(txn, toolchain, cxx_default_std);
    std::string module_mapper_path = has_modules ? get_module_mapper_path() : std::string{};

    // Create CXX-specific evaluator for PCH (PCH is always C++)
    auto pch_genex_ctx = make_genex_context(this, interp, all_targets, Language::CXX);
    GenexEvaluator pch_evaluator(pch_genex_ctx);

    // Helper to evaluate deferred COMPILE_LANGUAGE genex for PCH
    auto evaluate_for_pch = [&](const std::vector<std::string>& values) -> std::vector<std::string> {
        std::vector<std::string> result;
        for (const auto& val : values) {
            if (val.find("$<COMPILE_LANG") != std::string::npos) {
                auto eval_result = pch_evaluator.evaluate(val);
                if (eval_result && !eval_result->empty()) {
                    // Genex may produce semicolon-separated lists
                    for (auto sv : CMakeArrayView(*eval_result)) {
                        result.emplace_back(sv);
                    }
                }
            } else {
                result.push_back(val);
            }
        }
        return result;
    };

    // Filter out implicit includes for PCH
    auto filter_implicit = [&](const std::vector<std::string>& dirs) {
        std::vector<std::string> result;
        for (const auto& dir : dirs) {
            if (!implicit_includes.contains(normalize_include(dir))) result.push_back(dir);
        }
        return result;
    };

    auto [pch_gch_path, pch_include_arg] = generate_pch_task(txn, toolchain, this, is_shared,
        filter_implicit(evaluate_for_pch(get_resolved_property("INCLUDE_DIRECTORIES"))),
        filter_implicit(evaluate_for_pch(get_resolved_property("SYSTEM_INCLUDE_DIRECTORIES"))),
        evaluate_for_pch(get_resolved_property("COMPILE_DEFINITIONS")),
        evaluate_for_pch(get_resolved_property("COMPILE_OPTIONS")),
        cxx_default_std);

    // Single pass: evaluates sources, discovers custom commands, generates compile tasks,
    // and wires dependencies (PRE_BUILD, custom commands, module mapper) inline.
    generate_object_tasks(txn, toolchain, obj_files, pch_gch_path, pch_include_arg, is_shared, is_pie,
                          all_targets, evaluator, interp,
                          pre_build_task_id, module_mapper_path, generated_custom_tasks,
                          implicit_includes);

    if (type_ == TargetType::OBJECT_LIBRARY) return;

    // Collect object files from OBJECT library dependencies.
    // resolve() populates resolved_object_lib_deps_ with all OBJECT library
    // target names (transitively through static libs), so we just need to
    // gather their .o files here.
    {
        std::set<std::string> seen;
        for (const auto& obj_lib_name : resolved_object_lib_deps_) {
            if (!seen.insert(obj_lib_name).second) continue;
            auto it = all_targets.find(obj_lib_name);
            if (it == all_targets.end()) continue;
            auto& dep = it->second;
            const auto& source_props = interp.get_source_properties();
            for (const auto& src : dep->get_property_list("SOURCES", TargetPropertyScope::BUILD)) {
                auto lang_info = LanguageClassifier::from_path(src);
                if (lang_info.lang != Language::UNKNOWN && !lang_info.is_header) {
                    // Skip sources marked HEADER_FILE_ONLY (e.g. unity build originals)
                    std::filesystem::path sp(src);
                    std::string abs = sp.is_absolute() ? sp.lexically_normal().string()
                        : (std::filesystem::path(dep->get_source_dir()) / sp).lexically_normal().string();
                    auto sp_it = source_props.find(abs);
                    if (sp_it != source_props.end()) {
                        auto hfo = sp_it->second.find("HEADER_FILE_ONLY");
                        if (hfo != sp_it->second.end() && !Interpreter::is_falsy(hfo->second)) {
                            continue;
                        }
                    }
                    obj_files.push_back(get_obj_path(dep->get_binary_dir(), dep->get_name(), src));
                }
            }
        }
    }

    // Deduplicate object files (CMake compatibility). This can happen when
    // $<TARGET_OBJECTS:X> in SOURCES and target_link_libraries(... X) both
    // pull in the same object library's files.
    {
        std::unordered_set<std::string> seen;
        auto before = obj_files.size();
        std::erase_if(obj_files, [&](const std::string& o) {
            return !seen.insert(o).second;
        });
        if (obj_files.size() < before) {
            dmake::print_message(std::cerr, "WARNING", "target '" + name_ + "' has " + std::to_string(before - obj_files.size())
                      + " duplicate object file(s), likely from the same OBJECT library "
                      + "appearing in both $<TARGET_OBJECTS:> and target_link_libraries(). "
                      + "Duplicates were removed automatically for compatibility. "
                      + "Consider removing the redundant reference to clean this up.");
        }
    }

    auto sources_list = get_property_list("SOURCES", TargetPropertyScope::BUILD);
    Language linker_lang = std::any_of(sources_list.begin(), sources_list.end(),
        [](const std::string& src) { return LanguageClassifier::from_path(src).lang == Language::CXX; })
        ? Language::CXX : Language::C;

    std::string output_path = get_output_path();
    BuildTask link;
    link.id = output_path;
    link.kind = LinkTask{};
    link.parent_target = this;

    // Use resolved LINK_LIBRARIES directly — resolve() already flattened
    // transitive deps. Circular static lib deps (e.g. MariaDB) are handled
    // at link time via --start-group/--end-group in the linker command.
    std::vector<std::string> full_link_libs;
    if (type_ != TargetType::STATIC_LIBRARY) {
        // Deduplicate while preserving order.
        std::unordered_set<std::string> seen;
        for (const auto& lib : get_resolved_property("LINK_LIBRARIES")) {
            if (seen.insert(lib).second) {
                full_link_libs.push_back(lib);
            }
        }

        // If the target is C but links against libraries containing C++ code,
        // upgrade to g++ for linking so C++ runtime symbols resolve.
        if (linker_lang == Language::C) {
            auto has_cxx = [&]() {
                for (const auto& [name, tgt] : all_targets) {
                    if (tgt.get() == this) continue;
                    std::string tgt_output = tgt->get_output_path();
                    if (tgt_output.empty()) continue;
                    bool is_linked = std::any_of(full_link_libs.begin(), full_link_libs.end(),
                        [&](const std::string& lib) { return lib == tgt_output; });
                    if (!is_linked) continue;
                    for (const auto& src : tgt->get_property_list("SOURCES", TargetPropertyScope::BUILD)) {
                        if (LanguageClassifier::from_path(src).lang == Language::CXX)
                            return true;
                    }
                }
                return false;
            };
            if (has_cxx()) linker_lang = Language::CXX;
        }
    }

    const Compiler* linker = toolchain.get_compiler_ptr(linker_lang);
    if (!linker) {
        throw std::runtime_error("No linker available for language in target '" + name_ + "'");
    }

    if (type_ == TargetType::STATIC_LIBRARY) {
        link.commands.push_back(linker->get_archive_command(output_path, obj_files));
    } else {
        LinkContext ctx;
        ctx.output = output_path;
        ctx.objects = obj_files;
        ctx.is_shared = is_shared;

        // Determine standard: use highest of explicit standard or required by compile features
        {
            int required_std = 0;
            const auto& cf = get_resolved_property("COMPILE_FEATURES");
            if (!cf.empty()) {
                required_std = CompileFeatures::instance().get_required_standard(cf, linker_lang);
            }
            int compiler_default = (linker_lang == Language::CXX) ? cxx_default_std : c_default_std;
            ctx.standard = compute_effective_standard(
                get_language_standard(linker_lang), required_std, compiler_default);
        }
        ctx.extensions_enabled = get_language_extensions(linker_lang);
        ctx.color_diagnostics = isatty(STDOUT_FILENO);
        ctx.linker_flags = is_shared ? shared_linker_flags : exe_linker_flags;

        // CMake passes CMAKE_<LANG>_FLAGS to both compile AND link commands.
        // Flags like -fsanitize=address require this to work correctly.
        for (const auto& opt : get_language_flags(linker_lang)) {
            ctx.linker_flags.push_back(opt);
        }

        // Add target-specific link options (from target_link_options)
        for (const auto& opt : get_resolved_property("LINK_OPTIONS")) {
            ctx.linker_flags.push_back(opt);
        }

        // Apply LINK_FLAGS property (deprecated but still used by many projects)
        {
            std::string link_flags = get_property("LINK_FLAGS");
            if (!link_flags.empty()) {
                for (auto& flag : dmake::shell_split(link_flags)) {
                    ctx.linker_flags.push_back(std::move(flag));
                }
            }
        }

        for (const auto& lib : full_link_libs) {
             // Some Find modules set LIBRARIES as a space-separated string of
             // flags (e.g. "-L/usr/lib -lmbedtls -lmbedx509"). Split these into
             // individual arguments so they don't get passed as one quoted token.
             if (lib.find(' ') != std::string::npos) {
                 for (auto& part : dmake::shell_split(lib)) {
                     if (part.starts_with("-L")) {
                         ctx.lib_dirs.push_back(part.substr(2));
                     } else {
                         ctx.libs.push_back(std::move(part));
                     }
                 }
             } else if (lib.starts_with("/") || lib.starts_with("./") || lib.starts_with("../") ||
                 lib.find(".so") != std::string::npos ||
                 lib.find(".a") != std::string::npos) {
                 ctx.objects.push_back(lib);
             } else {
                 ctx.libs.push_back(lib);
             }
        }

        for (const auto& dir : get_resolved_property("LINK_DIRECTORIES")) {
            ctx.lib_dirs.push_back(dir);
        }

        link.commands.push_back(linker->get_link_command(ctx));
    }

    for (const auto& obj : obj_files) {
        link.inputs.push_back(obj);
        link.explicit_deps.push_back(obj);
    }

    // Static libraries are just .o archives — ar doesn't resolve symbols against
    // other libraries. Only executables/shared libraries need link-library deps.
    if (type_ != TargetType::STATIC_LIBRARY) {
        for (const auto& lib : full_link_libs) {
             if (lib.starts_with("/") || lib.starts_with("./") || lib.starts_with("../")) {
                 link.inputs.push_back(lib);

                 // Only add to dependencies if a non-imported target produces this.
                 // System libraries (from find_library) and IMPORTED target outputs
                 // are pre-existing - we don't build them.
                 for (const auto& [name, target] : all_targets) {
                     if (target->get_output_path() == lib && !target->is_imported()) {
                         link.explicit_deps.push_back(lib);
                         break;
                     }
                 }
             }
        }
    }

    // Propagate imported library dependencies to consumer link task.
    // When linking against an imported library (e.g., mylib with IMPORTED_LOCATION),
    // we need to inherit its manually_added_dependencies (e.g., mylib_ep EP target).
    // This ensures the EP sentinel runs before we try to link against EP outputs.
    for (const auto& [name, target] : all_targets) {
        if (!target->is_imported()) continue;
        std::string imported_out = target->get_output_path();
        if (imported_out.empty()) continue;

        // Check if we're linking against this imported library's output
        bool is_linked = std::any_of(full_link_libs.begin(), full_link_libs.end(),
            [&](const std::string& lib) { return lib == imported_out; });
        if (!is_linked) continue;

        // Inherit the imported library's manually_added_dependencies
        for (const auto& dep_name : target->get_manually_added_dependencies()) {
            auto dep_it = all_targets.find(dep_name);
            if (dep_it != all_targets.end()) {
                std::string dep_out = dep_it->second->get_output_path();
                if (!dep_out.empty()) {
                    link.explicit_deps.push_back(dep_out);
                } else {
                    // Custom/EP targets may not have output paths - use target name as task ID
                    link.explicit_deps.push_back(dep_name);
                }
            }
        }
    }

    // Add manually added dependencies (from add_dependencies command)
    for (const auto& dep_name : manually_added_dependencies_) {
        auto dep_it = all_targets.find(dep_name);
        if (dep_it != all_targets.end()) {
            std::string dep_out = dep_it->second->get_output_path();
            if (!dep_out.empty()) {
                link.explicit_deps.push_back(dep_out);
            } else {
                link.explicit_deps.push_back(dep_name);
            }
        }
    }

    // Add PRE_LINK commands before the actual link command
    if (!pre_link_commands_.empty()) {
        // Save the link command(s)
        auto link_cmds = std::move(link.commands);
        link.commands.clear();

        // Add PRE_LINK commands first
        for (const auto& cmd : pre_link_commands_) {
            link.commands.push_back(cmd.command);
        }

        resolve_command_target_references(link.commands, link, all_targets);

        // Then add the actual link command(s)
        for (auto& cmd : link_cmds) {
            link.commands.push_back(std::move(cmd));
        }
    }

    link.outputs.push_back(output_path);
    txn.add(std::move(link));

    // Generate POST_BUILD task if we have any post-build commands
    if (!post_build_commands_.empty()) {
        std::string post_build_task_id = name_ + "_post_build";
        BuildTask post_build;
        post_build.id = post_build_task_id;
        post_build.kind = PostBuildTask{};
        post_build.parent_target = this;
        post_build.always_run = true;
        post_build.working_dir = binary_dir_;

        for (const auto& cmd : post_build_commands_) {
            post_build.commands.push_back(cmd.command);
            if (!cmd.working_dir.empty()) {
                post_build.working_dir = cmd.working_dir;
            }
        }

        resolve_command_target_references(post_build.commands, post_build, all_targets);

        // POST_BUILD depends on the link task completing
        post_build.explicit_deps.push_back(output_path);
        post_build.inputs.push_back(output_path);

        txn.add(std::move(post_build));
    }
}

void CustomTarget::generate_tasks(GraphTransaction& txn, const Toolchain&, const std::map<std::string, std::shared_ptr<Target>>& all_targets, const Interpreter& interp, const std::vector<std::string>&, const std::vector<std::string>&) {
    BuildTask task;
    task.id = name_;
    task.kind = CustomTargetTask{};
    task.parent_target = this;
    task.always_run = true;
    task.working_dir = binary_dir_;

    const auto& custom_rules = interp.get_custom_command_rules();
    std::set<std::string> generated_cc_tasks;

    for (const auto& custom_cmd : custom_commands_) {
        task.commands.push_back(custom_cmd.command);
        if (!custom_cmd.working_dir.empty()) {
            task.working_dir = custom_cmd.working_dir;
        }
    }

    resolve_command_target_references(task.commands, task, all_targets);

    // Handle DEPENDS from add_custom_target
    for (const auto& dep_name : custom_depends_) {
        // Skip self-dependencies (CMake silently ignores these)
        if (dep_name == name_) {
            dmake::print_message(std::cerr, "WARNING",
                "Target '" + name_ + "' has itself in DEPENDS - ignoring self-dependency. "
                "If you meant to depend on a file named '" + dep_name + "', use an absolute path.");
            continue;
        }

        auto dep_it = all_targets.find(dep_name);
        if (dep_it != all_targets.end()) {
            std::string dep_out = dep_it->second->get_output_path();
            if (!dep_out.empty()) {
                task.explicit_deps.push_back(dep_out);
                task.inputs.push_back(dep_out);
            } else {
                task.explicit_deps.push_back(dep_name);
            }
        } else {
            std::filesystem::path p(dep_name);
            std::string normalized;
            if (p.is_absolute()) {
                normalized = p.lexically_normal().string();
            } else {
                normalized = (std::filesystem::path(source_dir_) / p).lexically_normal().string();
            }

            // Check if a custom command rule produces this file
            auto cc_it = custom_rules.find(normalized);
            if (cc_it == custom_rules.end() && !p.is_absolute()) {
                // Fallback: check binary dir (custom command outputs are registered there)
                auto bin_normalized = (std::filesystem::path(binary_dir_) / p).lexically_normal().string();
                cc_it = custom_rules.find(bin_normalized);
                if (cc_it != custom_rules.end()) {
                    normalized = bin_normalized;
                }
            }
            if (cc_it != custom_rules.end()) {
                generate_custom_command_task(txn, *cc_it->second, all_targets, custom_rules, generated_cc_tasks);
                task.explicit_deps.push_back(cc_it->second->outputs[0]);
            }
            task.inputs.push_back(normalized);
        }
    }

    // Handle manually added dependencies (from add_dependencies command)
    for (const auto& dep_name : manually_added_dependencies_) {
        auto dep_it = all_targets.find(dep_name);
        if (dep_it != all_targets.end()) {
            std::string dep_out = dep_it->second->get_output_path();
            if (!dep_out.empty()) {
                task.explicit_deps.push_back(dep_out);
            } else {
                task.explicit_deps.push_back(dep_name);
            }
        }
    }

    // Support SOURCES in custom targets just in case
    for (const auto& src : get_property_list("SOURCES", TargetPropertyScope::BUILD)) {
         std::filesystem::path p(src);
         if (!p.is_absolute()) p = std::filesystem::path(source_dir_) / p;
         task.inputs.push_back(p.string());
    }

    txn.add(std::move(task));
}

// --- C++20 Modules Support ---

std::string Target::get_module_mapper_path() const {
    return (std::filesystem::path(binary_dir_) / (name_ + ".module-mapper")).lexically_normal().string();
}

bool Target::has_module_sources() const {
    if (modules_detected_) return has_modules_;

    modules_detected_ = true;
    has_modules_ = false;

    // Check regular SOURCES for module interface files by extension
    for (const auto& src : get_property_list("SOURCES", TargetPropertyScope::BUILD)) {
        auto lang_info = LanguageClassifier::from_path(src);
        if (lang_info.is_module_interface) {
            has_modules_ = true;
            return true;
        }
    }

    // Check CXX_MODULES file sets
    for (const auto& fs : file_sets_) {
        if (fs.type == "CXX_MODULES" && !fs.files.empty()) {
            has_modules_ = true;
            return true;
        }
    }

    return false;
}

bool Target::generate_module_scanner_tasks(GraphTransaction& txn, const Toolchain& toolchain, int cxx_default_std) {
    std::vector<std::string> scanner_ids;

    for (const auto& src : get_property_list("SOURCES", TargetPropertyScope::BUILD)) {
        auto lang_info = LanguageClassifier::from_path(src);

        // Override module interface detection if file is in CXX_MODULES file set
        if (is_in_cxx_modules_file_set(src)) {
            lang_info.is_module_interface = true;
        }

        // Only scan module interface files (*.ixx, *.cppm, etc.)
        // Regular .cpp files that might import modules will have their
        // dependencies resolved through the collator
        if (!lang_info.is_module_interface) continue;
        if (lang_info.lang != Language::CXX) continue;

        const Compiler* compiler = toolchain.get_compiler_ptr(lang_info.lang);
        if (!compiler) continue;

        std::filesystem::path src_abs = std::filesystem::path(source_dir_) / src;
        std::string ddi_path = get_ddi_path(binary_dir_, src);

        ModuleScanContext ctx;
        ctx.source = src_abs.string();
        ctx.output = ddi_path;

        // Determine standard: use highest of explicit standard or required by compile features
        {
            int required_std = 0;
            const auto& cf = get_resolved_property("COMPILE_FEATURES");
            if (!cf.empty()) {
                required_std = CompileFeatures::instance().get_required_standard(cf, lang_info.lang);
            }
            ctx.standard = compute_effective_standard(
                get_language_standard(lang_info.lang), required_std, cxx_default_std);
        }
        ctx.extensions_enabled = get_language_extensions(lang_info.lang);
        ctx.color_diagnostics = isatty(STDOUT_FILENO);

        // Note: Do NOT automatically add source_dir - only add what's explicitly in INCLUDE_DIRECTORIES
        for (const auto& dir : get_resolved_property("INCLUDE_DIRECTORIES")) {
            ctx.includes.push_back(dir);
        }
        for (const auto& dir : get_resolved_property("SYSTEM_INCLUDE_DIRECTORIES")) {
            ctx.system_includes.push_back(dir);
        }
        for (const auto& def : get_resolved_property("COMPILE_DEFINITIONS")) {
            ctx.definitions.push_back(def);
        }

        BuildTask scanner;
        scanner.id = ddi_path;
        scanner.kind = ModuleScannerTask{src_abs.string()};
        scanner.parent_target = this;
        scanner.commands.push_back(compiler->get_module_scan_command(ctx));
        scanner.inputs.push_back(src_abs.string());
        scanner.outputs.push_back(ddi_path);

        txn.add(std::move(scanner));
        scanner_ids.push_back(ddi_path);
    }

    if (!scanner_ids.empty()) {
        generate_module_collator_task(txn, scanner_ids);
        return true;
    }

    return false;
}

void Target::generate_module_collator_task(GraphTransaction& txn, const std::vector<std::string>& scanner_task_ids) {
    std::string mapper_path = get_module_mapper_path();

    BuildTask collator;
    collator.id = mapper_path;
    collator.kind = ModuleCollatorTask{};
    collator.parent_target = this;

    // Collator depends on all scanner tasks
    for (const auto& scanner_id : scanner_task_ids) {
        collator.explicit_deps.push_back(scanner_id);
        collator.inputs.push_back(scanner_id);
    }

    collator.outputs.push_back(mapper_path);

    // Collator has no commands - it's executed in-process by the build graph
    // The actual work happens in BuildGraph::execute() when it detects a collator task

    txn.add(std::move(collator));
}

} // namespace dmake
