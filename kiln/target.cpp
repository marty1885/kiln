#include "target.hpp"
#include "build_system.hpp"
#include "autogen.hpp"
#include "language.hpp"
#include "toolchain.hpp"
#include "module_scanner.hpp"
#include "gnu_compiler.hpp"
#include "genex_evaluator.hpp"
#include "interperter.hpp"
#include "compile_features.hpp"
#include "printing.hpp"
#include "CMakeArray.hpp"
#include "parse_number.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <algorithm>
#include "container_utils.hpp"
#include "utils.hpp"
#include "path.hpp"
#include <functional>
#include <set>
#include <unordered_set>
#include <unordered_map>
#include <cassert>
#include <iostream>

namespace kiln {

// Compute the effective language standard, avoiding unnecessary -std= flags when the
// compiler default already satisfies the requirement (e.g. don't emit -std=gnu++11
// when the compiler defaults to C++17).
//
// explicit_standard: from target's CXX_STANDARD/C_STANDARD (e.g. "17", or "" if unset)
// features_required: minimum standard required by compile features (e.g. 11, or 0)
// compiler_default:  compiler's default standard (e.g. 17, or 0 if unknown)
//
// Returns the standard string to use (e.g. "17"), or "" if no flag needed.
static std::string compute_effective_standard(const std::string& explicit_standard, int features_required, int compiler_default) {
    int explicit_val = explicit_standard.empty() ? 0 : parse_number<int>(explicit_standard).value_or(0);
    int effective = std::max(explicit_val, features_required);

    if (effective <= 0) return explicit_standard; // nothing required

    // If no explicit standard was set (only features drove the requirement),
    // and the compiler default already meets it, don't add a flag.
    if (explicit_val == 0 && compiler_default > 0 && effective <= compiler_default) return "";

    return std::to_string(effective);
}

// CMake's <LANG>_COMPILER_LAUNCHER / <LANG>_LINKER_LAUNCHER: target property
// (semicolon list) overrides the corresponding CMAKE_<LANG>_..._LAUNCHER cache
// variable. Tokens are prepended verbatim to the compiler/linker argv. Empty
// result = no launcher.
static std::vector<std::string> resolve_launcher(const Target& target, const Interpreter& interp, Language lang,
                                                 const std::string& kind /* "COMPILER" or "LINKER" */) {
    std::string lang_str(language_name(lang));
    std::string prop = lang_str + "_" + kind + "_LAUNCHER";
    std::string val = target.get_property(prop);
    if (val.empty()) { val = interp.get_variable("CMAKE_" + prop); }
    std::vector<std::string> out;
    if (val.empty()) return out;
    for (auto sv : CMakeArrayIterator(val)) {
        if (!sv.empty()) out.emplace_back(sv);
    }
    return out;
}

// Prepend launcher tokens onto a CompilerCommand's argv (and signature_argv).
// Launcher identity is part of the signature so that swapping
// ccache↔none reruns the compile.
static void apply_launcher(CompilerCommand& cc, const std::vector<std::string>& launcher) {
    if (launcher.empty()) return;
    cc.argv.insert(cc.argv.begin(), launcher.begin(), launcher.end());
    cc.signature_argv.insert(cc.signature_argv.begin(), launcher.begin(), launcher.end());
}

static void apply_launcher(std::vector<std::string>& argv, const std::vector<std::string>& launcher) {
    if (launcher.empty()) return;
    argv.insert(argv.begin(), launcher.begin(), launcher.end());
}

// --- Generic Property Implementation ---

void Target::set_property(const std::string& name, const std::string& value) {
    properties_[name] = value;
}

std::string Target::get_property(const std::string& name) const {
    if (properties_.contains(name)) { return properties_.at(name); }
    return "";
}

void Target::append_property(const std::string& name, const std::vector<std::string>& values, PropertyVisibility visibility) {
    auto& list = list_properties_[name][visibility];
    for (const auto& v : values) {
        for (auto item : CMakeArrayIterator(v)) {
            if (!item.empty()) list.emplace_back(item);
        }
    }
}

void Target::append_property_from_string(const std::string& name, const std::string& value, PropertyVisibility visibility) {
    CMakeArray list(value); // Always split by semicolons
    append_property(name, list.to_vector(), visibility);
}

void Target::prepend_property(const std::string& name, const std::vector<std::string>& values, PropertyVisibility visibility) {
    auto& list = list_properties_[name][visibility];
    std::vector<std::string> split;
    for (const auto& v : values) {
        for (auto item : CMakeArrayIterator(v)) {
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

        // In CMake, INCLUDE_DIRECTORIES contains ALL includes (both regular and SYSTEM).
        // kiln stores SYSTEM includes separately, so merge them here for correctness
        // when queried via $<TARGET_PROPERTY:tgt,INCLUDE_DIRECTORIES> or get_property().
        if (name == "INCLUDE_DIRECTORIES") {
            auto sys_it = list_properties_.find("SYSTEM_INCLUDE_DIRECTORIES");
            if (sys_it != list_properties_.end()) {
                for (auto vis : {PropertyVisibility::PUBLIC, PropertyVisibility::PRIVATE, PropertyVisibility::INTERFACE}) {
                    auto vis_it = sys_it->second.find(vis);
                    if (vis_it != sys_it->second.end()) {
                        for (const auto& v : vis_it->second) {
                            if (!result.empty()) result += ';';
                            result += v;
                        }
                    }
                }
            }
        }

        if (!result.empty()) return result;
    }

    // CMake visibility-prefixed property access:
    //   INTERFACE_X → X[PUBLIC] + X[INTERFACE]
    // This is how CMake exposes visibility-scoped properties via genex like
    // $<TARGET_PROPERTY:tgt,INTERFACE_INCLUDE_DIRECTORIES>
    auto try_visibility_prefix = [&](std::string_view prefix, std::initializer_list<PropertyVisibility> scopes) -> std::string {
        if (!name.starts_with(prefix)) return {};
        std::string base_name(name.substr(prefix.size()));
        auto base_it = list_properties_.find(base_name);
        if (base_it == list_properties_.end()) return {};
        std::string result;
        for (auto vis : scopes) {
            auto vis_it = base_it->second.find(vis);
            if (vis_it != base_it->second.end()) {
                for (const auto& v : vis_it->second) {
                    if (!result.empty()) result += ';';
                    result += v;
                }
            }
        }
        return result;
    };

    if (auto r = try_visibility_prefix("INTERFACE_", {PropertyVisibility::PUBLIC, PropertyVisibility::INTERFACE}); !r.empty()) return r;

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

std::vector<std::string> Target::get_resolved_property_for_language(const std::string& name, Language lang, const Interpreter& interp,
                                                                    const TargetMap& all_targets) const {
    const auto& base = get_resolved_property(name);

    // Fast path: nothing depends on language.
    bool needs_lang =
        std::any_of(base.begin(), base.end(), [](const std::string& v) { return v.find("$<COMPILE_LANG") != std::string::npos; });
    if (!needs_lang) return base;

    GenexEvaluationContext ctx = GenexEvaluationContext::from_interpreter(interp, all_targets);
    ctx.current_target = this;
    ctx.compile_language = lang;
    GenexEvaluator evaluator(ctx);

    std::vector<std::string> out;
    out.reserve(base.size());
    for (const auto& val : base) {
        if (val.find("$<COMPILE_LANG") == std::string::npos) {
            out.push_back(val);
            continue;
        }
        auto r = evaluator.evaluate(val);
        if (!r || r->empty()) continue;
        // Result may be a CMake list — split on top-level ';'
        for (auto sv : CMakeArrayIterator(*r)) { out.emplace_back(sv); }
    }
    return out;
}

Language Target::get_linker_language() const {
    if (cached_linker_language_) return *cached_linker_language_;

    // Explicit LINKER_LANGUAGE property wins (CMake semantics).
    {
        std::string explicit_lang = get_property("LINKER_LANGUAGE");
        if (!explicit_lang.empty()) {
            Language lang = Language::CXX;
            if (explicit_lang == "C")
                lang = Language::C;
            else if (explicit_lang == "ASM")
                lang = Language::ASM;
            // Fall back to CXX for unknown — keeps us linking with the most
            // permissive driver instead of erroring.
            cached_linker_language_ = lang;
            return lang;
        }
    }

    // Else infer from this target's own sources: any C++ source promotes to CXX.
    Language lang = Language::C;
    auto sources = get_property_list("SOURCES", TargetPropertyScope::BUILD);
    for (const auto& src : sources) {
        auto info = LanguageClassifier::from_path(src);
        if (info.lang == Language::CXX) {
            lang = Language::CXX;
            break;
        }
        if (info.lang == Language::UNKNOWN) {
            // Extensionless source: probe filesystem for a C++ companion
            std::string abs = Path::make_absolute_and_normal(source_dir_, src);
            for (auto ext : {".cpp", ".cc", ".cxx", ".C", ".c++"}) {
                if (std::filesystem::exists(abs + ext)) {
                    lang = Language::CXX;
                    break;
                }
            }
            if (lang == Language::CXX) break;
        }
    }
    cached_linker_language_ = lang;
    return lang;
}

std::string Target::generate_dump_info() const {
    // Helper to print a vector
    auto print_vec = [](const std::string& indent, const std::vector<std::string>& vec) -> std::string {
        if (vec.empty()) return indent + "(empty)";
        return join(vec, "", [&](const std::string& item) { return indent + "- " + item + "\n"; });
    };

    // Helper to get type name
    auto type_name = [](TargetType type) -> std::string {
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
            return "CUSTOM";
        default:
            return "UNKNOWN";
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
    if (is_imported_) { output += "Imported Location: " + imported_location_ + "\n"; }
    output += "\n";

    // Print unresolved properties (derived from shared metadata table)
    std::vector<std::string> prop_names;
    for (const auto& meta : kListProperties) { prop_names.emplace_back(meta.name); }

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
    return file_set_visibility_for_source(source).has_value();
}

std::optional<PropertyVisibility> Target::file_set_visibility_for_source(const std::string& source) const {
    for (const auto& fs : file_sets_) {
        if (fs.type != "CXX_MODULES") continue;
        for (const auto& file : fs.files) {
            Path src_path(source);
            Path fs_path(file);
            if (src_path == fs_path) return fs.visibility;
            if (src_path.filename() == fs_path.filename()) return fs.visibility;
            auto src_norm = Path::make_absolute_and_normal(source_dir_, source);
            auto fs_norm = Path::make_absolute_and_normal(source_dir_, file);
            if (src_norm == fs_norm) return fs.visibility;
        }
    }
    return std::nullopt;
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

const Compiler* Target::resolve_compiler(Language lang, const Toolchain& toolchain) const {
    if (!language_has_compiler(lang)) { return toolchain.get_compiler_ptr(lang); }
    std::string_view name = language_name(lang);
    std::string binary_var = "CMAKE_" + std::string(name) + "_COMPILER";
    const std::string& captured_binary = captured_compiler_var(binary_var);
    if (!captured_binary.empty()) {
        std::string target_var = "CMAKE_" + std::string(name) + "_COMPILER_TARGET";
        std::string id_var = "CMAKE_" + std::string(name) + "_COMPILER_ID";
        const std::string& captured_sysroot = captured_compiler_var("CMAKE_SYSROOT");
        const std::string& captured_target = captured_compiler_var(target_var);
        const std::string& captured_id = captured_compiler_var(id_var);
        // CMAKE_<LANG>_COMPILER_TARGET only makes sense for drivers that
        // honor --target= (real CMake's Clang Compiler module is what turns
        // it into --target=). GCC errors on the flag.
        const std::string effective_target = compiler_honors_target_flag(captured_id) ? captured_target : std::string{};
        // The default compiler is also in the registry (registered via
        // enable_language). If the captured tuple matches it, get_or_register
        // returns the same pointer; otherwise it lazily registers a fresh one.
        return toolchain.get_or_register(lang, captured_id, captured_binary, captured_sysroot, effective_target);
    }
    return toolchain.get_compiler_ptr(lang);
}

// --- Resolution Logic ---

GenexEvaluationContext Target::make_genex_context(const Target* current_target, const Interpreter& interp, const TargetMap& all_targets,
                                                  std::optional<Language> compile_language, bool allow_deferred) {
    auto ctx = GenexEvaluationContext::from_interpreter(interp, all_targets);
    ctx.current_target = current_target;
    ctx.compile_language = compile_language;
    ctx.allow_deferred_compile_language = allow_deferred;
    return ctx;
}

// Order-preserving dedup merge: append items from source into target,
// skipping duplicates already present in target.
namespace {
void merge_dedup(std::vector<std::string>& target, const std::vector<std::string>& source) {
    std::unordered_set<std::string> seen(target.begin(), target.end());
    for (const auto& s : source) {
        if (seen.insert(s).second) { target.push_back(s); }
    }
}
// Detect if a string contains genex that can only be resolved in a per-consumer,
// per-source context — values that must be propagated to consumers as raw genex
// rather than evaluated at the dep's resolve time:
//
//   $<TARGET_PROPERTY:X>             — no comma at top depth: refers to the
//                                      consuming target's property.
//   $<COMPILE_LANGUAGE:...>          — depends on the source file's language.
//   $<COMPILE_LANG_AND_ID:...>       — same.
//
// When these appear nested inside another genex (e.g. Qt6's
// `$<$<NOT:$<COMPILE_LANGUAGE:Swift>>:-mno-direct-extern-access>`), partial
// evaluation at the dep would resolve the inner placeholder to a literal
// string the outer genex can't reason about, silently dropping the value.
// Instead we mark the whole string as deferred and forward it intact.
bool contains_consumer_target_property_genex(const std::string& s) {
    if (s.find("$<COMPILE_LANGUAGE:") != std::string::npos) return true;
    if (s.find("$<COMPILE_LANG_AND_ID:") != std::string::npos) return true;
    static constexpr std::string_view marker = "$<TARGET_PROPERTY:";
    size_t pos = 0;
    while ((pos = s.find(marker, pos)) != std::string::npos) {
        size_t content_start = pos + marker.size();
        // Scan for the matching '>' counting nested $< >
        int depth = 1;
        bool found_comma_at_depth1 = false;
        for (size_t i = content_start; i < s.size() && depth > 0; ++i) {
            if (s[i] == '$' && i + 1 < s.size() && s[i + 1] == '<') {
                depth++;
                i++; // skip '<'
            } else if (s[i] == '>') {
                depth--;
                if (depth == 0) break;
            } else if (s[i] == ',' && depth == 1) {
                found_comma_at_depth1 = true;
                break;
            }
        }
        if (!found_comma_at_depth1) return true;
        pos = content_start;
    }
    return false;
}

} // anonymous namespace

const std::vector<Target::PropInfo>& Target::build_props_to_resolve() {
    static const std::vector<PropInfo> instance = [] {
        std::vector<PropInfo> result;
        for (const auto& meta : kListProperties) {
            if (meta.transitive && meta.name != "LINK_LIBRARIES") { result.push_back({std::string(meta.name), meta.is_path}); }
        }
        return result;
    }();
    return instance;
}

std::string Target::resolve_to_absolute_path(const std::string& p) const {
    std::string_view sv = p;
    if (sv.starts_with("-I")) sv.remove_prefix(2);
    Path path(sv);
    if (path.is_absolute()) return path.str();
    return (Path(source_dir_) / sv).lexically_normal().str();
}

void Target::initialize_local_properties(const std::vector<PropInfo>& props_to_resolve, GenexEvaluator& evaluator) {
    for (const auto& info : props_to_resolve) {
        auto& res = resolved_properties_[info.name];
        auto& res_iface = resolved_interface_properties_[info.name];

        auto process_local = [&](PropertyVisibility vis, std::vector<std::string>& out) {
            const auto& val = get_property_list(info.name, vis);
            if (val.empty()) return;
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
                    if (!p.empty()) { out.push_back(resolve_to_absolute_path(p)); }
                }
            } else {
                out.insert(out.end(), eval_result->begin(), eval_result->end());
            }
        };

        // Own build properties: PRIVATE + PUBLIC
        process_local(PropertyVisibility::PRIVATE, res);
        process_local(PropertyVisibility::PUBLIC, res);

        // Interface properties: PUBLIC + INTERFACE
        // Check for single-arg $<TARGET_PROPERTY:prop> that needs deferred
        // evaluation per consumer (the property refers to the consuming target).
        auto process_interface = [&](PropertyVisibility vis) {
            const auto& val = get_property_list(info.name, vis);
            if (val.empty()) return;

            // Check if any value contains consumer-dependent genex
            bool has_deferred = false;
            for (const auto& v : val) {
                if (contains_consumer_target_property_genex(v)) {
                    has_deferred = true;
                    break;
                }
            }

            if (has_deferred) {
                // Store raw values for re-evaluation per consumer
                auto& deferred = deferred_interface_genex_[info.name];
                deferred.insert(deferred.end(), val.begin(), val.end());
            } else {
                process_local(vis, res_iface);
            }
        };

        process_interface(PropertyVisibility::PUBLIC);
        process_interface(PropertyVisibility::INTERFACE);
    }
}

const std::vector<std::string>& Target::collect_link_deps(const Target& dep) {
    if (dep.get_type() == TargetType::STATIC_LIBRARY && !dep.is_imported()) { return dep.get_resolved_property("LINK_LIBRARIES"); }
    return dep.get_resolved_interface_property("LINK_LIBRARIES");
}

void Target::propagate_from_dependency(const Target& dep, const std::vector<PropInfo>& props_to_resolve,
                                       std::map<std::string, std::vector<std::string>>& output_props, bool skip_non_link,
                                       GenexEvaluator& evaluator) {
    // 1. Append non-link interface properties (includes, definitions, options, etc.)
    // Deduplication is deferred to Phase 3 of resolve() via remove_duplicates().
    if (!skip_non_link) {
        for (const auto& info : props_to_resolve) {
            auto& dest = (dep.is_imported() && info.name == "INCLUDE_DIRECTORIES") ? output_props["SYSTEM_INCLUDE_DIRECTORIES"]
                                                                                   : output_props[info.name];

            // Re-evaluate deferred genex with the consumer's context.
            // If *this* target is also just an interface library propagating
            // onward, re-defer instead of evaluating — the genex refers to
            // properties of the ultimate consuming target, not intermediaries.
            auto deferred_it = dep.deferred_interface_genex_.find(info.name);
            if (deferred_it != dep.deferred_interface_genex_.end()) {
                bool is_interface_passthrough = (type_ == TargetType::INTERFACE_LIBRARY);
                if (is_interface_passthrough) {
                    // Forward raw genex to our own deferred set
                    auto& our_deferred = deferred_interface_genex_[info.name];
                    our_deferred.insert(our_deferred.end(), deferred_it->second.begin(), deferred_it->second.end());
                } else {
                    // Split deferred values into:
                    //   - per-source genex (COMPILE_LANGUAGE / COMPILE_LANG_AND_ID):
                    //     forward raw to dest. The consumer's per-source compile
                    //     evaluates with that source's language in context.
                    //     Evaluating here would resolve the per-source genex to
                    //     a placeholder string the surrounding genex can't reason
                    //     about, silently dropping the value (Qt6's
                    //     `$<$<NOT:$<COMPILE_LANGUAGE:Swift>>:-mno-direct-extern-access>`).
                    //   - the rest ($<TARGET_PROPERTY:X>): evaluate now with the
                    //     consumer as current_target.
                    std::vector<std::string> needs_eval;
                    for (const auto& v : deferred_it->second) {
                        if (v.find("$<COMPILE_LANGUAGE:") != std::string::npos || v.find("$<COMPILE_LANG_AND_ID:") != std::string::npos) {
                            dest.push_back(v);
                        } else {
                            needs_eval.push_back(v);
                        }
                    }
                    if (!needs_eval.empty()) {
                        auto eval_result = evaluator.evaluate_property_list(needs_eval);
                        if (eval_result) {
                            if (info.is_path) {
                                for (const auto& p : *eval_result) {
                                    if (!p.empty()) dest.push_back(resolve_to_absolute_path(p));
                                }
                            } else {
                                dest.insert(dest.end(), eval_result->begin(), eval_result->end());
                            }
                        }
                    }
                }
            }

            const auto& dep_iface = dep.get_resolved_interface_property(info.name);
            if (!dep_iface.empty()) { dest.insert(dest.end(), dep_iface.begin(), dep_iface.end()); }
        }
    }

    // 2. Add dependency's own artifact + transitive link deps to LINK_LIBRARIES
    auto& output_libs = output_props["LINK_LIBRARIES"];
    std::string dep_path = dep.get_output_path();
    if (!dep_path.empty()) { output_libs.push_back(std::move(dep_path)); }
    merge_dedup(output_libs, collect_link_deps(dep));
}

void Target::handle_circular_dep(const Target& dep, bool is_public, bool is_interface_only, std::vector<std::string>& res_libs,
                                 std::vector<std::string>& res_iface_libs) {
    if (dep.get_type() != TargetType::STATIC_LIBRARY && dep.get_type() != TargetType::OBJECT_LIBRARY) {
        throw std::runtime_error("Circular dependency: " + name_ + " -> " + dep.get_name());
    }
    kiln::print_message(std::cerr, "WARNING",
                        "Circular dependency between static libraries '" + name_ + "' and '" + dep.get_name()
                            + "'. Consider restructuring.");
    std::string dep_path = dep.get_output_path();
    if (!dep_path.empty()) {
        if (!is_interface_only) res_libs.push_back(dep_path);
        if (is_public || is_interface_only) res_iface_libs.push_back(dep_path);
    }
    deferred_circular_deps_.push_back(dep.get_name());
}

void Target::resolve_deferred_circular_deps(const TargetMap& all_targets) {
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

void Target::resolve(const TargetMap& all_targets, const Interpreter& interp) {
    if (resolved_) return;
    if (visiting_) throw std::runtime_error("Circular dependency: " + name_);
    visiting_ = true;

    auto props_to_resolve = build_props_to_resolve();
    auto genex_ctx = make_genex_context(this, interp, all_targets, std::nullopt, true);
    GenexEvaluator evaluator(genex_ctx);

    // Phase 1: Own properties
    initialize_local_properties(props_to_resolve, evaluator);

    // Phase 2: Walk dependency graph via LINK_LIBRARIES
    auto& res_libs = resolved_properties_["LINK_LIBRARIES"];
    auto& res_iface_libs = resolved_interface_properties_["LINK_LIBRARIES"];

    auto process_one_dep = [&](const std::string& raw_lib_name, bool is_public, bool is_interface_only, bool link_only) {
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
            handle_circular_dep(*dep, is_public, is_interface_only, res_libs, res_iface_libs);
            return;
        }

        try {
            dep->resolve(all_targets, interp);
        } catch (std::runtime_error& e) {
            std::string_view msg = e.what();
            constexpr std::string_view prefix = "Circular dependency: ";
            if (msg.starts_with(prefix)) {
                // Build the chain as the stack unwinds: each frame prepends itself.
                // Seed is "X", first catch makes "C -> X", next "B -> C -> X", etc.
                std::string chain(msg.substr(prefix.size()));
                throw std::runtime_error(std::string(prefix) + name_ + " -> " + chain);
            }
            throw;
        }

        // For building THIS target (PRIVATE or PUBLIC dep)
        if (!is_interface_only) {
            propagate_from_dependency(*dep, props_to_resolve, resolved_properties_, /*skip_non_link=*/link_only, evaluator);
            // CMake 3.21+: OBJECT libraries on the right side of
            // target_link_libraries() have "their object files included in
            // the link too" (cmake.org docs).  But OBJECT libraries on the
            // LEFT side only receive usage requirements — there is no link
            // step to include objects in.
            if (dep->get_type() == TargetType::OBJECT_LIBRARY && type_ != TargetType::OBJECT_LIBRARY) {
                resolved_object_lib_deps_.push_back(lib_name);
            }
            // Don't merge resolved_object_lib_deps_ from STATIC library deps:
            // those objects are already archived inside the .a file and will be
            // picked up by the linker from the archive.  Merging would cause
            // the same .o files to appear both in the archive AND directly on
            // the link line, leading to "multiple definition" errors.
        }

        // For propagating to OUR dependents (PUBLIC or INTERFACE dep)
        if (is_public || is_interface_only) {
            propagate_from_dependency(*dep, props_to_resolve, resolved_interface_properties_, /*skip_non_link=*/link_only, evaluator);
        }
    };

    auto walk_link_libs = [&](PropertyVisibility vis, bool is_public, bool is_interface_only) {
        const auto& raw_libs = get_property_list("LINK_LIBRARIES", vis);

        // Reassemble fragmented genex: semicolons inside $<...> cause the
        // interpreter to split a single genex like $<BUILD_INTERFACE:a;b;c>
        // into separate list items. Rejoin them before evaluation.
        std::vector<std::string> libs;
        std::string pending;
        int depth = 0;
        for (const auto& lib : raw_libs) {
            if (depth > 0) {
                pending += ';';
                pending += lib;
            } else {
                pending = lib;
            }
            for (size_t i = 0; i < lib.size(); ++i) {
                if (lib[i] == '$' && i + 1 < lib.size() && lib[i + 1] == '<') {
                    ++depth;
                    ++i;
                } else if (lib[i] == '>' && depth > 0) {
                    --depth;
                }
            }
            if (depth == 0) {
                libs.push_back(std::move(pending));
                pending.clear();
            }
        }
        if (!pending.empty()) { libs.push_back(std::move(pending)); }

        for (const auto& lib : libs) {
            auto eval_result = evaluator.evaluate_link_library(lib);
            if (!eval_result) {
                throw std::runtime_error("Error evaluating LINK_LIBRARIES for target '" + name_ + "': " + eval_result.error()
                                         + "\n  Value: " + lib);
            }
            if (!eval_result->value.empty()) {
                for (auto part : CMakeArrayIterator(eval_result->value)) {
                    process_one_dep(std::string(part), is_public, is_interface_only, eval_result->link_only);
                }
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

std::string Target::get_output_path(GenexEvaluator* evaluator) const {
    // Interface libraries have no linkable output - they only propagate properties
    if (type_ == TargetType::INTERFACE_LIBRARY) { return ""; }

    if (is_imported_ && !imported_location_.empty()) { return imported_location_; }

    // Helper: read a property, evaluating genex if an evaluator is available
    auto eval_prop = [&](const std::string& prop) -> std::string {
        return evaluator ? evaluator->evaluate_target_property(*this, prop) : get_property(prop);
    };

    // Per-artifact OUTPUT_NAME override (RUNTIME/ARCHIVE/LIBRARY) takes
    // precedence over OUTPUT_NAME, which itself overrides target name.
    std::string out_name;
    const char* per_type_name_prop = nullptr;
    if (type_ == TargetType::EXECUTABLE)
        per_type_name_prop = "RUNTIME_OUTPUT_NAME";
    else if (type_ == TargetType::STATIC_LIBRARY)
        per_type_name_prop = "ARCHIVE_OUTPUT_NAME";
    else if (type_ == TargetType::SHARED_LIBRARY)
        per_type_name_prop = "LIBRARY_OUTPUT_NAME";
    if (per_type_name_prop) out_name = eval_prop(per_type_name_prop);
    if (out_name.empty()) out_name = get_output_name();

    // <CONFIG>_POSTFIX (and DEBUG_POSTFIX) — appended for libraries only,
    // matching CMake. Single-config: build_type_ is captured at definition.
    if (type_ == TargetType::SHARED_LIBRARY || type_ == TargetType::STATIC_LIBRARY) {
        if (!build_type_.empty()) {
            std::string upper;
            upper.reserve(build_type_.size());
            for (char c : build_type_) upper.push_back(std::toupper((unsigned char) c));
            std::string postfix = eval_prop(upper + "_POSTFIX");
            if (!postfix.empty()) out_name += postfix;
        }
    }

    // Determine output directory: per-target property overrides binary_dir_.
    // CMake mapping: EXECUTABLE → RUNTIME, STATIC → ARCHIVE, SHARED → LIBRARY.
    std::string output_dir;
    if (type_ == TargetType::EXECUTABLE) {
        output_dir = eval_prop("RUNTIME_OUTPUT_DIRECTORY");
    } else if (type_ == TargetType::STATIC_LIBRARY) {
        output_dir = eval_prop("ARCHIVE_OUTPUT_DIRECTORY");
    } else if (type_ == TargetType::SHARED_LIBRARY) {
        output_dir = eval_prop("LIBRARY_OUTPUT_DIRECTORY");
    }
    const auto& dir = output_dir.empty() ? binary_dir_ : output_dir;

    // Determine prefix and suffix, respecting target properties
    std::string prefix_prop = eval_prop("PREFIX");
    std::string suffix_prop = eval_prop("SUFFIX");
    bool has_prefix = !prefix_prop.empty() || properties_.count("PREFIX");
    bool has_suffix = !suffix_prop.empty() || properties_.count("SUFFIX");

    std::string filename;
    if (type_ == TargetType::EXECUTABLE) {
        std::string prefix = has_prefix ? prefix_prop : "";
        std::string suffix = has_suffix ? suffix_prop : "";
        filename = prefix + out_name + suffix;
    } else if (type_ == TargetType::SHARED_LIBRARY) {
        std::string prefix = has_prefix ? prefix_prop : "lib";
        std::string suffix = has_suffix ? suffix_prop : ".so";
        filename = prefix + out_name + suffix;
        // VERSION/SOVERSION → real on-disk file is libfoo.so.<VERSION>
        // (or .<SOVERSION> if only SOVERSION is set), with symlinks created
        // by the link task. Matches CMake.
        if (!has_suffix) {
            std::string ver = eval_prop("VERSION");
            if (ver.empty()) ver = eval_prop("SOVERSION");
            if (!ver.empty()) filename += "." + ver;
        }
    } else if (type_ == TargetType::STATIC_LIBRARY) {
        std::string prefix = has_prefix ? prefix_prop : "lib";
        std::string suffix = has_suffix ? suffix_prop : ".a";
        filename = prefix + out_name + suffix;
    } else {
        return "";
    }

    Path path = Path(dir) / filename;
    return dir.empty() ? path.str() : path.lexically_normal().str();
}

// --- Qt Autogen Helpers ---

void Target::inject_autogen_include(const std::string& dir) {
    // Prepend to resolved INCLUDE_DIRECTORIES so autogen headers are found first
    auto& resolved = resolved_properties_["INCLUDE_DIRECTORIES"];
    if (std::find(resolved.begin(), resolved.end(), dir) == resolved.end()) { resolved.insert(resolved.begin(), dir); }
}

void Target::inject_autogen_dep(const std::string& task_id) {
    autogen_deps_.push_back(task_id);
}

void Target::inject_autogen_source(const std::string& path) {
    append_property("SOURCES", {path}, PropertyVisibility::PRIVATE);
}

void Target::remove_source(const std::string& path) {
    for (auto vis : {PropertyVisibility::PRIVATE, PropertyVisibility::PUBLIC, PropertyVisibility::INTERFACE}) {
        auto prop_it = list_properties_.find("SOURCES");
        if (prop_it == list_properties_.end()) continue;
        auto vis_it = prop_it->second.find(vis);
        if (vis_it == prop_it->second.end()) continue;
        std::erase_if(vis_it->second, [&](const std::string& s) {
            // Match by exact path or by normalized absolute path
            if (s == path) return true;
            std::string abs = Path::make_absolute_and_normal(source_dir_, s);
            return abs == path;
        });
    }
}

// Strip trailing slashes to normalize include paths for comparison.
// Prevents mismatches like "/usr/include" vs "/usr/include/" when filtering implicit includes.
static std::string normalize_include(const std::string& dir) {
    auto end = dir.size();
    while (end > 1 && dir[end - 1] == '/') --end;
    return dir.substr(0, end);
}

std::string get_obj_path(const std::string& binary_dir, const std::string& target_name, const std::string& source_path) {
    Path src(source_path);
    std::string_view obj_suffix;

    if (src.is_absolute()) {
        // Use full path structure (minus root) to avoid collisions between
        // files with same name in different directories (e.g. posix/file.c vs file.c)
        obj_suffix = src.relative_path();
    } else {
        obj_suffix = src.view();
    }

    // Replace ".." segments with "__" so a source like "../src/foo.c" doesn't
    // walk out of the per-target objs/<target> directory after normalization
    // and collide with other targets compiling the same source.
    std::string sanitized(obj_suffix);
    for (size_t i = 0; i < sanitized.size();) {
        size_t end = sanitized.find('/', i);
        size_t len = (end == std::string::npos ? sanitized.size() : end) - i;
        if (len == 2 && sanitized[i] == '.' && sanitized[i + 1] == '.') {
            sanitized[i] = '_';
            sanitized[i + 1] = '_';
        }
        if (end == std::string::npos) break;
        i = end + 1;
    }

    Path obj = (Path(binary_dir) / "objs") / target_name / sanitized;
    std::string obj_str = obj.str() + ".o";
    return binary_dir.empty() ? obj_str : Path(obj_str).lexically_normal().str();
}

// Resolve executable target names in the first argument of COMMAND clauses.
// CMake replaces bare target names with the built binary path and adds an implicit dependency.
static void resolve_command_target_references(std::vector<std::vector<std::string>>& commands, BuildTask& task,
                                              const TargetMap& all_targets,
                                              const std::unordered_map<std::string, std::string>& target_aliases = {}) {
    for (auto& cmd : commands) {
        if (cmd.empty()) continue;

        std::shared_ptr<Target> resolved;

        // Resolve alias first (e.g. Qt6::syncqt -> syncqt)
        const std::string& cmd_name = [&]() -> const std::string& {
            auto alias_it = target_aliases.find(cmd[0]);
            return (alias_it != target_aliases.end()) ? alias_it->second : cmd[0];
        }();

        // Direct target name lookup (executables only)
        auto it = all_targets.find(cmd_name);
        if (it != all_targets.end() && it->second->get_type() == TargetType::EXECUTABLE) { resolved = it->second; }

        // Fall back: executable whose OUTPUT_NAME or output path matches the command
        if (!resolved) {
            // thread_local: parallel EP orchestrators each pass a different
            // all_targets map; sharing this cache across threads races.
            thread_local const TargetMap* cached_source = nullptr;
            thread_local std::unordered_map<std::string, std::shared_ptr<Target>> output_name_map;
            thread_local std::unordered_map<std::string, std::shared_ptr<Target>> output_path_map;
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
            if (oit != output_name_map.end()) resolved = oit->second;
            if (!resolved) {
                auto pit = output_path_map.find(cmd[0]);
                if (pit != output_path_map.end()) resolved = pit->second;
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
static std::expected<void, std::string>
generate_custom_command_task(GraphTransaction& txn, const CustomCommandRule& rule, const TargetMap& all_targets,
                             const std::map<std::string, std::shared_ptr<CustomCommandRule>>& custom_rules,
                             std::set<std::string>& generated, const std::unordered_map<std::string, std::string>& target_aliases);

std::expected<void, std::string>
generate_custom_command_task_for_rule(GraphTransaction& txn, const CustomCommandRule& rule, const TargetMap& all_targets,
                                      const std::map<std::string, std::shared_ptr<CustomCommandRule>>& custom_rules,
                                      std::set<std::string>& generated,
                                      const std::unordered_map<std::string, std::string>& target_aliases) {
    return generate_custom_command_task(txn, rule, all_targets, custom_rules, generated, target_aliases);
}

static std::expected<void, std::string>
generate_custom_command_task(GraphTransaction& txn, const CustomCommandRule& rule, const TargetMap& all_targets,
                             const std::map<std::string, std::shared_ptr<CustomCommandRule>>& custom_rules,
                             std::set<std::string>& generated, const std::unordered_map<std::string, std::string>& target_aliases) {
    if (generated.count(rule.outputs[0]) || txn.has_task(rule.outputs[0])) return {};
    generated.insert(rule.outputs[0]);

    BuildTask task;
    task.id = rule.outputs[0];
    task.kind = CustomCommandTask{};
    task.working_dir = rule.working_dir;

    for (const auto& cmd : rule.commands) { task.commands.push_back(cmd); }

    resolve_command_target_references(task.commands, task, all_targets, target_aliases);

    for (const auto& out : rule.outputs) { task.outputs.push_back(out); }

    for (const auto& dep : rule.depends) {
        // If dependency contains genex, store raw for evaluation at graph gen time
        if (GenexParser::contains_genex(dep)) {
            task.inputs.push_back(dep);
            continue;
        }

        // Resolve target aliases (e.g. unicode::ucd -> unicode)
        auto alias_it = target_aliases.find(dep);
        const std::string& resolved_dep = (alias_it != target_aliases.end()) ? alias_it->second : dep;
        auto dep_it = all_targets.find(resolved_dep);
        if (dep_it != all_targets.end()) {
            std::string dep_out = dep_it->second->get_output_path();
            if (!dep_out.empty()) {
                task.explicit_deps.push_back(dep_out);
                task.inputs.push_back(dep_out);
            } else {
                task.explicit_deps.push_back(dep);
            }
        } else {
            Path p(dep);
            std::string normalized;
            if (p.is_absolute()) {
                normalized = p.lexically_normal().str();
            } else {
                // Try source dir first, then binary dir (custom command outputs are in binary dir)
                normalized = Path::make_absolute_and_normal(rule.source_dir, dep);
            }

            // Check if a custom command rule produces this file
            auto cc_it = custom_rules.find(normalized);
            if (cc_it == custom_rules.end() && p.is_relative()) {
                // Fallback: check binary dir (custom command outputs are registered there)
                auto bin_normalized = Path::make_absolute_and_normal(rule.binary_dir, dep);
                cc_it = custom_rules.find(bin_normalized);
                if (cc_it != custom_rules.end()) { normalized = bin_normalized; }
            }
            if (cc_it != custom_rules.end()) {
                if (auto r = generate_custom_command_task(txn, *cc_it->second, all_targets, custom_rules, generated, target_aliases); !r)
                    return std::unexpected(std::move(r.error()));
                task.explicit_deps.push_back(cc_it->second->outputs[0]);
            }
            task.inputs.push_back(normalized);
        }
    }

    if (auto r = txn.add(std::move(task)); !r) return std::unexpected(std::move(r.error()));
    return {};
}

struct ResolvedDep {
    std::string id;
};

std::expected<void, std::string>
Target::generate_object_tasks(GraphTransaction& txn, const Toolchain& toolchain, std::vector<std::string>& obj_files,
                              const std::map<Language, PchInfo>& pch_per_lang, bool is_shared, bool is_pie, const TargetMap& all_targets,
                              GenexEvaluator& evaluator, const Interpreter& interp, const std::string& pre_build_task_id,
                              const std::string& module_mapper_path, std::set<std::string>& generated_custom_tasks,
                              const std::set<std::string>& implicit_includes, std::vector<ResolvedDep> resolved_manual_deps) {
    // --- Hoist all loop-invariant computations ---

    // True when this target needs the module compile pipeline. Includes the
    // cross-target consumer case where the target imports a foreign module
    // but provides none of its own.
    bool target_has_modules = participates_in_modules(all_targets);

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
        if (!cxx_def.empty()) cxx_default_std = parse_number<int>(cxx_def).value_or(0);
        const std::string& c_def = interp.get_variable("CMAKE_C_STANDARD_DEFAULT");
        if (!c_def.empty()) c_default_std = parse_number<int>(c_def).value_or(0);
    }

    // Compute effective standard per language (hoisted out of per-source loop)
    auto effective_standard = [&](Language lang) -> std::string {
        if (lang == Language::ASM) return "";
        int required = (lang == Language::CXX) ? cxx_required_std : c_required_std;
        int compiler_default = (lang == Language::CXX) ? cxx_default_std : c_default_std;
        return compute_effective_standard(get_language_standard(lang), required, compiler_default);
    };

    // Pre-build CXX_MODULES file set for O(1) lookups instead of O(N*M) per-source
    std::unordered_set<std::string> cxx_module_files;
    for (const auto& fs : file_sets_) {
        if (fs.type == "CXX_MODULES") {
            for (const auto& file : fs.files) {
                Path fs_path(file);
                cxx_module_files.insert(Path::make_absolute_and_normal(source_dir_, file));
                // Also add filename for filename-only matches
                cxx_module_files.insert(std::string(fs_path.filename()));
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
        return std::any_of(vals.begin(), vals.end(), [](const std::string& v) { return v.find("$<COMPILE_LANG") != std::string::npos; });
    };
    bool needs_per_lang_eval = has_deferred_genex(resolved_includes) || has_deferred_genex(resolved_sys_includes)
                               || has_deferred_genex(resolved_definitions) || has_deferred_genex(resolved_options);

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
                        for (auto sv : CMakeArrayIterator(*eval_result)) { result.emplace_back(sv); }
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

    // CMake auto-defines <target>_EXPORTS for shared/module libraries.
    // DEFINE_SYMBOL property overrides the default. Empty DEFINE_SYMBOL suppresses it.
    std::string exports_define;
    if (type_ == TargetType::SHARED_LIBRARY) {
        std::string ds = get_property("DEFINE_SYMBOL");
        if (ds.empty()) {
            // Default: <name>_EXPORTS with non-alnum replaced by _
            exports_define = name_;
            for (auto& ch : exports_define) {
                if (!std::isalnum(static_cast<unsigned char>(ch))) ch = '_';
            }
            exports_define += "_EXPORTS";
        } else {
            exports_define = std::move(ds);
        }
    }

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
        Path src_path(src);
        std::string norm_src, norm_bin;
        if (src_path.is_absolute()) {
            norm_src = norm_bin = src_path.lexically_normal().str();
        } else {
            norm_src = Path::make_absolute_and_normal(source_dir_, src);
            norm_bin = Path::make_absolute_and_normal(binary_dir_, src);
        }
        auto cc_it = custom_rules.find(norm_src);
        if (cc_it == custom_rules.end()) cc_it = custom_rules.find(norm_bin);
        if (cc_it != custom_rules.end()) {
            if (!generated_custom_tasks.count(cc_it->second->outputs[0])) {
                if (auto r = generate_custom_command_task(txn, *cc_it->second, all_targets, custom_rules, generated_custom_tasks,
                                                          interp.get_target_aliases());
                    !r)
                    return std::unexpected(std::move(r.error()));
            }
            resolved_manual_deps.push_back({cc_it->second->outputs[0]});
        }
    }

    // Ninja-style: compile tasks depend on custom command outputs from linked library targets.
    // When target A links target B (transitively), A's compilations must wait for B's custom
    // commands that produce generated files (e.g. headers from mktables.py), but NOT for B's
    // full build. Uses get_resolved_property("LINK_LIBRARIES") which includes transitive deps.
    {
        // Cached maps: output_path → Target*, and Target* → custom command primary outputs.
        // Rebuilt when all_targets changes. Precomputation is O(T + S_total), lookup is O(1).
        //
        // thread_local, not static: EP orchestrators run generate_build_graph
        // concurrently on worker threads, each with its own all_targets map.
        // A plain static raced — different threads cleared/rewrote the same
        // hash table, corrupting heap headers (surfaced as glibc
        // "free(): invalid pointer" deep inside an EP configure).
        thread_local const TargetMap* cached_cc_map_source = nullptr;
        thread_local std::unordered_map<std::string, Target*> lib_to_target;
        thread_local std::unordered_map<Target*, std::vector<std::string>> target_cc_outputs;

        if (&all_targets != cached_cc_map_source) {
            cached_cc_map_source = &all_targets;
            lib_to_target.clear();
            target_cc_outputs.clear();

            for (const auto& [name, target] : all_targets) {
                if (target->is_imported()) continue;
                std::string out = target->get_output_path();
                if (!out.empty()) lib_to_target[out] = target.get();

                // Precompute custom command outputs for this target's sources
                std::vector<std::string> cc_keys;
                for (const auto& src : target->get_property_list("SOURCES", TargetPropertyScope::BUILD)) {
                    if (src.empty()) continue;
                    Path src_path(src);
                    std::string norm;
                    if (src_path.is_absolute()) {
                        norm = src_path.lexically_normal().str();
                    } else {
                        norm = Path::make_absolute_and_normal(target->get_source_dir(), src);
                    }
                    auto cc_it = custom_rules.find(norm);
                    if (cc_it == custom_rules.end() && !src_path.is_absolute()) {
                        norm = Path::make_absolute_and_normal(target->get_binary_dir(), src);
                        cc_it = custom_rules.find(norm);
                    }
                    if (cc_it != custom_rules.end()) { cc_keys.push_back(cc_it->second->outputs[0]); }
                }
                if (!cc_keys.empty()) { target_cc_outputs[target.get()] = std::move(cc_keys); }
            }
        }

        // For each transitively linked library, add its custom command deps — O(L) per target
        std::unordered_set<Target*> visited;
        for (const auto& lib : get_resolved_property("LINK_LIBRARIES")) {
            auto tt = lib_to_target.find(lib);
            if (tt == lib_to_target.end()) continue;
            if (!visited.insert(tt->second).second) continue;

            auto cc_it = target_cc_outputs.find(tt->second);
            if (cc_it == target_cc_outputs.end()) continue;
            for (const auto& key : cc_it->second) {
                if (!generated_custom_tasks.count(key)) {
                    // Look up the rule by its primary output to generate the task
                    auto rule_it = custom_rules.find(key);
                    if (rule_it != custom_rules.end()) {
                        if (auto r = generate_custom_command_task(txn, *rule_it->second, all_targets, custom_rules, generated_custom_tasks,
                                                                  interp.get_target_aliases());
                            !r)
                            return std::unexpected(std::move(r.error()));
                    }
                }
                resolved_manual_deps.push_back({key});
            }
        }
    }

    std::unordered_set<std::string> seen_sources;
    std::vector<std::string> generated_header_inputs;

    // Pre-scan for GENERATED headers in the source list so every compile
    // task (including ones whose source appears before the header) gets
    // the order-only dep. Without this, the .h listed at the end of the
    // source list would only attach to nothing (it's filtered as a header)
    // and the build would race the producer.
    for (const auto& src : *evaluated_sources_result) {
        if (src.empty()) continue;
        auto li = LanguageClassifier::from_path(src);
        if (!li.is_header) continue;
        Path sp(src);
        std::string norm = sp.is_absolute() ? sp.lexically_normal().str() : Path::make_absolute_and_normal(binary_dir_, src);
        auto sp_it2 = source_props.find(norm);
        if (sp_it2 == source_props.end()) continue;
        auto g_it = sp_it2->second.find("GENERATED");
        if (g_it == sp_it2->second.end() || Interpreter::is_falsy(g_it->second)) continue;
        generated_header_inputs.push_back(std::move(norm));
    }

    for (const auto& src : *evaluated_sources_result) {
        if (src.empty()) continue;

        // Handle pre-compiled object files
        Path src_path(src);
        auto ext = src_path.extension();
        if (ext == ".o" || ext == ".obj") {
            obj_files.push_back(src);
            continue;
        }

        // Determine if source is relative to source_dir or binary_dir
        std::string src_abs_str;
        std::string src_normalized;
        // For custom command discovery, we need both normalizations of relative paths
        std::string normalized_src_dir, normalized_bin_dir;

        if (src_path.is_absolute()) {
            src_abs_str = src_path.lexically_normal().str();
            src_normalized = src_abs_str;
            normalized_src_dir = src_normalized;
            normalized_bin_dir = src_normalized;
        } else {
            normalized_bin_dir = Path::make_absolute_and_normal(binary_dir_, src);
            normalized_src_dir = Path::make_absolute_and_normal(source_dir_, src);

            if (custom_rules.find(normalized_bin_dir) != custom_rules.end()) {
                src_abs_str = Path::join(binary_dir_, src);
                src_normalized = normalized_bin_dir;
            } else {
                src_abs_str = Path::join(source_dir_, src);
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
            if (hfo_it != sp_it->second.end() && !Interpreter::is_falsy(hfo_it->second)) { continue; }
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

        // CMake allows sources without extensions (e.g. "src/test" for "src/test.cpp").
        // Try appending known source extensions to find the actual file.
        if (lang_info.lang == Language::UNKNOWN) {
            static constexpr std::string_view try_extensions[] = {
                ".cpp", ".cc", ".cxx", ".C", ".c++", ".c", ".cu", ".s", ".S", ".asm",
            };
            bool resolved = false;
            for (auto try_ext : try_extensions) {
                std::string candidate = src_abs_str + std::string(try_ext);
                if (std::filesystem::exists(candidate)) {
                    src_abs_str = std::move(candidate);
                    lang_info = LanguageClassifier::from_extension(try_ext);
                    resolved = true;
                    break;
                }
            }
            if (!resolved) continue;
        }

        // Check CXX_MODULES using pre-built set (O(1) vs O(N*M))
        if (!cxx_module_files.empty()) {
            if (cxx_module_files.count(src_normalized) || cxx_module_files.count(std::string(src_path.filename()))) {
                lang_info.is_module_interface = true;
            }
        }

        const Compiler* compiler = resolve_compiler(lang_info.lang, toolchain);
        if (!compiler) {
            throw std::runtime_error("No compiler available for language " + std::string(lang_info.name) + " in target '" + name_ + "'");
        }

        std::string obj = get_obj_path(binary_dir_, name_, src);
        obj_files.push_back(obj);

        // src_abs_str already computed above

        CompileContext ctx;
        ctx.source = src_abs_str;
        ctx.output = obj;
        ctx.is_shared = is_shared;
        ctx.is_pie = is_pie;
        // Apply PCH only for languages that have a PCH generated,
        // and not for sources with SKIP_PRECOMPILE_HEADERS property
        auto pch_it = pch_per_lang.find(lang_info.lang);
        bool skip_pch = (pch_it == pch_per_lang.end());
        if (!skip_pch && sp_it != source_props.end()) {
            auto skip_it = sp_it->second.find("SKIP_PRECOMPILE_HEADERS");
            if (skip_it != sp_it->second.end() && !Interpreter::is_falsy(skip_it->second)) skip_pch = true;
        }
        ctx.pch_include = skip_pch ? "" : pch_it->second.include_arg;
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

        // CMake auto-defines <target>_EXPORTS for shared/module libraries
        if (!exports_define.empty()) ctx.definitions.push_back(exports_define);

        // Apply target-level COMPILE_FLAGS property (deprecated but still used)
        {
            std::string target_cf = get_property("COMPILE_FLAGS");
            if (!target_cf.empty()) {
                std::istringstream iss(target_cf);
                std::string flag;
                while (iss >> flag) { ctx.options.push_back(flag); }
            }
        }

        // Apply per-source properties
        if (sp_it != source_props.end()) {
            auto cf_it = sp_it->second.find("COMPILE_FLAGS");
            if (cf_it != sp_it->second.end()) {
                std::istringstream iss(cf_it->second);
                std::string flag;
                while (iss >> flag) { ctx.options.push_back(flag); }
            }

            auto co_it = sp_it->second.find("COMPILE_OPTIONS");
            if (co_it != sp_it->second.end()) {
                // Lazily construct per-source evaluator only if genex present
                std::optional<GenexEvaluator> src_eval_storage;
                for (auto sv : CMakeArrayIterator(co_it->second)) {
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
                            for (auto sv : CMakeArrayIterator(*result)) { ctx.options.emplace_back(sv); }
                        }
                    } else {
                        ctx.options.push_back(std::move(opt));
                    }
                }
            }

            auto cd_it = sp_it->second.find("COMPILE_DEFINITIONS");
            if (cd_it != sp_it->second.end()) {
                for (auto sv : CMakeArrayIterator(cd_it->second)) { ctx.definitions.emplace_back(sv); }
            }

            auto id_it = sp_it->second.find("INCLUDE_DIRECTORIES");
            if (id_it != sp_it->second.end()) {
                for (auto sv : CMakeArrayIterator(id_it->second)) { ctx.includes.emplace_back(sv); }
            }
        }

        BuildTask task;
        task.id = obj;
        task.kind = CompileTask{src_abs_str, lang_info.lang};
        task.parent_target = this;
        {
            auto cc = compiler->get_compile_command(ctx);
            apply_launcher(cc, resolve_launcher(*this, interp, lang_info.lang, "COMPILER"));
            task.commands.push_back(std::move(cc.argv));
            task.signature_commands.push_back(std::move(cc.signature_argv));
        }
        task.inputs.push_back(src_abs_str);
        task.outputs.push_back(obj);
        task.outputs.push_back(obj + ".d");

        if (pch_it != pch_per_lang.end()) {
            task.explicit_deps.push_back(pch_it->second.gch_path);
            task.inputs.push_back(pch_it->second.gch_path);
        }

        // Pre-resolved manual dependencies (hoisted)
        for (const auto& dep : resolved_manual_deps) { task.explicit_deps.push_back(dep.id); }

        // Autogen deps (UIC outputs): ui_*.h can be included transitively via headers
        for (const auto& dep : autogen_deps_) { task.explicit_deps.push_back(dep); }

        // GENERATED headers listed in target sources: route through task.inputs
        // so the file-dep resolver maps them to the producing custom target's
        // task (via output_to_task_). Putting them in explicit_deps would only
        // resolve task IDs, not file paths.
        for (const auto& gh : generated_header_inputs) { task.inputs.push_back(gh); }

        // OBJECT_DEPENDS
        if (sp_it != source_props.end()) {
            auto od_it = sp_it->second.find("OBJECT_DEPENDS");
            if (od_it != sp_it->second.end()) {
                for (auto sv : CMakeArrayIterator(od_it->second)) {
                    std::string dep_normalized = Path::make_absolute_and_normal(source_dir_, sv);
                    task.inputs.push_back(dep_normalized);

                    // If this dependency is produced by a custom command, ensure
                    // the task for it is generated (e.g. qt6_generate_moc outputs)
                    auto od_cc_it = custom_rules.find(dep_normalized);
                    if (od_cc_it != custom_rules.end()) {
                        if (!generated_custom_tasks.count(od_cc_it->second->outputs[0])) {
                            if (auto r = generate_custom_command_task(txn, *od_cc_it->second, all_targets, custom_rules,
                                                                      generated_custom_tasks, interp.get_target_aliases());
                                !r)
                                return std::unexpected(std::move(r.error()));
                        }
                        task.explicit_deps.push_back(od_cc_it->second->outputs[0]);
                    }
                }
            }
        }

        // --- Dependency wiring (merged from generate_tasks) ---

        // Depend on PRE_BUILD task
        if (!pre_build_task_id.empty()) { task.explicit_deps.push_back(pre_build_task_id); }

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

        if (auto r = txn.add(std::move(task)); !r) return std::unexpected(std::move(r.error()));
    }
    return {};
}

static std::expected<PchInfo, std::string>
generate_pch_task(GraphTransaction& txn, const Toolchain& toolchain, const Target* target, const Interpreter& interp, Language pch_lang,
                  const std::vector<std::string>& pch_headers, bool is_shared, const std::vector<std::string>& includes,
                  const std::vector<std::string>& system_includes, const std::vector<std::string>& definitions,
                  const std::vector<std::string>& options, int compiler_default_standard,
                  const std::vector<ResolvedDep>& manual_deps = {}) {

    const Compiler* compiler = target->resolve_compiler(pch_lang, toolchain);
    if (!compiler) {
        std::string lang_name = (pch_lang == Language::C) ? "C" : "CXX";
        throw std::runtime_error("No " + lang_name + " compiler available for PCH generation in target '" + target->get_name()
                                 + "' (target has PRECOMPILE_HEADERS for " + lang_name + " but project() does not enable that language)");
    }

    // CMake uses cmake_pch.h for C and cmake_pch.hxx for CXX; we use similar naming
    std::string ext = (pch_lang == Language::C) ? "_pch.h" : "_pch.hxx";
    Path pch_path = (Path(target->get_binary_dir()) / "objs") / (target->get_name() + ext);
    std::string pch_wrapper = target->get_binary_dir().empty() ? pch_path.str() : pch_path.lexically_normal().str();
    std::string pch_gch_path = pch_wrapper + ".gch";
    std::string pch_include_arg = " -include " + pch_wrapper;

    auto is_angle_bracketed = [](const std::string& h) { return h.size() >= 2 && h.front() == '<' && h.back() == '>'; };
    auto strip_quotes = [](const std::string& h) {
        if (h.size() >= 2 && h.front() == '"' && h.back() == '"') return h.substr(1, h.size() - 2);
        return h;
    };

    std::ostringstream wrapper_content;
    for (const auto& hdr : pch_headers) {
        if (is_angle_bracketed(hdr)) {
            wrapper_content << "#include " << hdr << "\n";
        } else {
            wrapper_content << "#include \"" << strip_quotes(hdr) << "\"\n";
        }
    }
    std::string content = wrapper_content.str();

    bool needs_write = true;
    if (std::filesystem::exists(pch_wrapper)) {
        std::ifstream existing(pch_wrapper);
        if (existing) {
            std::string existing_content((std::istreambuf_iterator<char>(existing)), std::istreambuf_iterator<char>());
            if (existing_content == content) { needs_write = false; }
        }
    }

    if (needs_write) {
        std::filesystem::create_directories(std::string(Path(pch_wrapper).parent_path()));
        std::ofstream wrapper_file(pch_wrapper);
        if (!wrapper_file) { throw std::runtime_error("Failed to create PCH wrapper file: " + pch_wrapper); }
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
        if (!cf.empty()) { required_std = CompileFeatures::instance().get_required_standard(cf, pch_lang); }
        ctx.standard = compute_effective_standard(target->get_language_standard(pch_lang), required_std, compiler_default_standard);
    }
    ctx.extensions_enabled = target->get_language_extensions(pch_lang);
    ctx.color_diagnostics = isatty(STDOUT_FILENO);
    ctx.options.push_back("-x");
    ctx.options.push_back(pch_lang == Language::C ? "c-header" : "c++-header");

    for (const auto& opt : target->get_language_flags(pch_lang)) ctx.options.push_back(opt);
    for (const auto& opt : options) ctx.options.push_back(opt);
    for (const auto& def : definitions) ctx.definitions.push_back(def);

    // PCH wrapper includes headers relative to source_dir, so we need source_dir in the include path
    ctx.includes.push_back(target->get_source_dir());

    for (const auto& dir : includes) ctx.includes.push_back(dir);
    for (const auto& dir : system_includes) ctx.system_includes.push_back(dir);

    {
        auto cc = compiler->get_compile_command(ctx);
        apply_launcher(cc, resolve_launcher(*target, interp, pch_lang, "COMPILER"));
        pch_task.commands.push_back(std::move(cc.argv));
        pch_task.signature_commands.push_back(std::move(cc.signature_argv));
    }
    pch_task.inputs.push_back(pch_wrapper);

    for (const auto& hdr : pch_headers) {
        // Angle-bracketed headers are system includes, not files on disk
        if (is_angle_bracketed(hdr)) continue;
        std::string h = strip_quotes(hdr);
        // Quoted-include headers ("foo/bar.h") are resolved against the include search path
        // at compile time, not the source dir — we can't reliably locate them as inputs here,
        // so skip adding them to task inputs (the .gch dependency on the wrapper is enough).
        if (!Path(h).is_absolute() && hdr != h) continue;
        pch_task.inputs.push_back(Path::make_absolute_and_normal(target->get_source_dir(), h));
    }

    pch_task.outputs.push_back(pch_gch_path);

    for (const auto& dep : manual_deps) { pch_task.explicit_deps.push_back(dep.id); }

    if (auto r = txn.add(std::move(pch_task)); !r) return std::unexpected(std::move(r.error()));

    return PchInfo{pch_gch_path, pch_include_arg};
}

std::expected<void, std::string> Target::generate_tasks(GraphTransaction& txn, const Toolchain& toolchain, const TargetMap& all_targets,
                                                        const Interpreter& interp, const std::vector<std::string>& exe_linker_flags,
                                                        const std::vector<std::string>& shared_linker_flags) {
    if (type_ == TargetType::INTERFACE_LIBRARY || is_imported_) return {};

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
            is_pie = true; // Executables use -fPIE
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
            if (!cmd.working_dir.empty()) { pre_build.working_dir = cmd.working_dir; }
        }

        resolve_command_target_references(pre_build.commands, pre_build, all_targets, interp.get_target_aliases());

        if (auto r = txn.add(std::move(pre_build)); !r) return std::unexpected(std::move(r.error()));
    }

    // Pre-resolve manual dependencies for autogen, PCH, and compile tasks.
    // Moved early so autogen (MOC/UIC/RCC) tasks can also depend on these.
    std::vector<ResolvedDep> resolved_manual_deps;
    {
        std::unordered_set<std::string> seen_dep_outputs;
        auto add_manual_deps = [&](const std::vector<std::string>& deps) {
            for (const auto& dep_name : deps) {
                auto it = all_targets.find(dep_name);
                if (it != all_targets.end()) {
                    std::string dep_out = it->second->get_output_path();
                    std::string id = dep_out.empty() ? dep_name : std::move(dep_out);
                    if (seen_dep_outputs.insert(id).second) { resolved_manual_deps.push_back({id}); }
                }
            }
        };
        add_manual_deps(manually_added_dependencies_);
        // Transitive walk: a custom target attached via add_dependencies to an
        // imported library (e.g. add_dependencies(libjs_rust libjs_rust-build))
        // must also order before consumers' compile tasks when those consumers
        // include headers the custom target produces. The deps reach us through
        // multiple link levels (js → LibJS → libjs_rust → libjs_rust-build).
        std::unordered_set<std::string> visited;
        std::vector<std::string> stack(resolved_target_deps_.begin(), resolved_target_deps_.end());
        while (!stack.empty()) {
            std::string cur = std::move(stack.back());
            stack.pop_back();
            if (!visited.insert(cur).second) continue;
            auto it = all_targets.find(cur);
            if (it == all_targets.end()) continue;
            add_manual_deps(it->second->get_manually_added_dependencies());
            for (const auto& d : it->second->get_resolved_target_deps()) stack.push_back(d);
        }
    }

    // Qt autogen: generate moc/uic/rcc tasks before compilation.
    // This scans sources/headers for Qt macros, creates moc/uic/rcc tasks,
    // injects generated sources into SOURCES, and adds the autogen include dir.
    if (!Interpreter::is_falsy(get_property("AUTOMOC")) || !Interpreter::is_falsy(get_property("AUTOUIC"))
        || !Interpreter::is_falsy(get_property("AUTORCC"))) {
        std::vector<std::string> manual_dep_ids;
        manual_dep_ids.reserve(resolved_manual_deps.size());
        for (const auto& d : resolved_manual_deps) manual_dep_ids.push_back(d.id);
        if (auto r = generate_autogen_tasks(*this, txn, const_cast<Interpreter&>(interp), all_targets, pre_build_task_id, manual_dep_ids);
            !r)
            return std::unexpected(std::move(r.error()));
    }

    // Read compiler default standards (for suppressing unnecessary -std= flags)
    int cxx_default_std = 0;
    int c_default_std = 0;
    {
        const std::string& cxx_def = interp.get_variable("CMAKE_CXX_STANDARD_DEFAULT");
        if (!cxx_def.empty()) cxx_default_std = parse_number<int>(cxx_def).value_or(0);
        const std::string& c_def = interp.get_variable("CMAKE_C_STANDARD_DEFAULT");
        if (!c_def.empty()) c_default_std = parse_number<int>(c_def).value_or(0);
    }

    // C++20 modules: generate scanner tasks first (they have no dependencies)
    auto has_modules_result = generate_module_scanner_tasks(txn, toolchain, all_targets, interp, cxx_default_std);
    if (!has_modules_result) return std::unexpected(std::move(has_modules_result.error()));
    bool has_modules = *has_modules_result;
    std::string module_mapper_path = has_modules ? get_module_mapper_path() : std::string{};

    // PCH: generate per-language precompiled headers (C gets _pch.h, CXX gets _pch.hxx)
    // CMake only generates PCH for languages the target actually uses in its sources,
    // and only when at least one source of that language participates in PCH (i.e.,
    // does not have SKIP_PRECOMPILE_HEADERS set).
    std::set<Language> target_source_languages;
    {
        const auto& source_props = interp.get_source_properties();
        auto srcs = get_property_list("SOURCES", TargetPropertyScope::BUILD);
        for (const auto& src : srcs) {
            auto info = LanguageClassifier::from_path(src);
            if (info.lang == Language::UNKNOWN || info.is_header) continue;

            std::string src_normalized =
                Path(src).is_absolute() ? Path(src).lexically_normal().str() : Path::make_absolute_and_normal(source_dir_, src);
            auto sp_it = source_props.find(src_normalized);
            if (sp_it != source_props.end()) {
                auto skip_it = sp_it->second.find("SKIP_PRECOMPILE_HEADERS");
                if (skip_it != sp_it->second.end() && !Interpreter::is_falsy(skip_it->second)) { continue; }
            }
            target_source_languages.insert(info.lang);
        }
    }

    std::map<Language, PchInfo> pch_per_lang;
    std::string reuse_from = get_property("PRECOMPILE_HEADERS_REUSE_FROM");
    if (!reuse_from.empty()) {
        // REUSE_FROM mode: use provider's PCH artifact
        auto provider_it = all_targets.find(reuse_from);
        if (provider_it == all_targets.end()) {
            throw std::runtime_error("target_precompile_headers(REUSE_FROM): provider target '" + reuse_from + "' not found");
        }
        auto provider = provider_it->second;
        // Follow the REUSE_FROM chain to the ultimate provider (the one with actual PCH headers).
        std::set<std::string> visited{reuse_from};
        while (true) {
            std::string next = provider->get_property("PRECOMPILE_HEADERS_REUSE_FROM");
            if (next.empty()) break;
            if (!visited.insert(next).second) {
                throw std::runtime_error("target_precompile_headers(REUSE_FROM): cycle detected via '" + next + "'");
            }
            auto next_it = all_targets.find(next);
            if (next_it == all_targets.end()) {
                throw std::runtime_error("target_precompile_headers(REUSE_FROM): provider target '" + next + "' not found");
            }
            provider = next_it->second;
        }
        // Compute provider's PCH paths per language (same formula as generate_pch_task).
        // If the provider has no PRECOMPILE_HEADERS, this is a silent no-op — matches CMake,
        // where REUSE_FROM against a provider with conditionally-empty PCH simply produces
        // no PCH for the consumer rather than failing configuration.
        auto own_pchs_raw = provider->get_property_list("PRECOMPILE_HEADERS", TargetPropertyScope::BUILD);
        for (Language lang : {Language::C, Language::CXX}) {
            if (!target_source_languages.contains(lang)) continue;
            auto lang_ctx = make_genex_context(provider.get(), interp, all_targets, lang);
            GenexEvaluator lang_eval(lang_ctx);
            auto result = lang_eval.evaluate_property_list(own_pchs_raw);
            if (result && !result->empty()) {
                std::string ext = (lang == Language::C) ? "_pch.h" : "_pch.hxx";
                Path provider_pch_path = (Path(provider->get_binary_dir()) / "objs") / (provider->get_name() + ext);
                std::string pch_wrapper =
                    provider->get_binary_dir().empty() ? provider_pch_path.str() : provider_pch_path.lexically_normal().str();
                pch_per_lang[lang] = {pch_wrapper + ".gch", " -include " + pch_wrapper};
            }
        }
    } else {
        // Standard PCH generation: generate one PCH task per language that has headers
        // Compute exports define here so it's included in PCH compilation (must match sources)
        std::string pch_exports_define;
        if (type_ == TargetType::SHARED_LIBRARY) {
            std::string ds = get_property("DEFINE_SYMBOL");
            if (ds.empty()) {
                pch_exports_define = name_;
                for (auto& ch : pch_exports_define) {
                    if (!std::isalnum(static_cast<unsigned char>(ch))) ch = '_';
                }
                pch_exports_define += "_EXPORTS";
            } else {
                pch_exports_define = std::move(ds);
            }
        }

        auto own_pchs_raw = get_property_list("PRECOMPILE_HEADERS", TargetPropertyScope::BUILD);
        if (!own_pchs_raw.empty()) {
            // Filter out implicit includes for PCH
            auto filter_implicit = [&](const std::vector<std::string>& dirs) {
                std::vector<std::string> result;
                for (const auto& dir : dirs) {
                    if (!implicit_includes.contains(normalize_include(dir))) result.push_back(dir);
                }
                return result;
            };

            for (Language lang : {Language::C, Language::CXX}) {
                // Skip languages where the target has no source files
                if (!target_source_languages.contains(lang)) continue;
                // Skip languages where no compiler is available
                if (!resolve_compiler(lang, toolchain)) continue;

                // Evaluate genex with this language's context
                auto lang_ctx = make_genex_context(this, interp, all_targets, lang);
                GenexEvaluator lang_eval(lang_ctx);
                auto result = lang_eval.evaluate_property_list(own_pchs_raw);
                if (!result || result->empty()) continue;

                // Helper to evaluate deferred COMPILE_LANGUAGE genex for this language's PCH
                auto evaluate_for_pch = [&](const std::vector<std::string>& values) -> std::vector<std::string> {
                    std::vector<std::string> out;
                    for (const auto& val : values) {
                        if (val.find("$<COMPILE_LANG") != std::string::npos) {
                            auto eval_result = lang_eval.evaluate(val);
                            if (eval_result && !eval_result->empty()) {
                                for (auto sv : CMakeArrayIterator(*eval_result)) { out.emplace_back(sv); }
                            }
                        } else {
                            out.push_back(val);
                        }
                    }
                    return out;
                };

                int default_std = (lang == Language::C) ? c_default_std : cxx_default_std;
                auto pch_defs = evaluate_for_pch(get_resolved_property("COMPILE_DEFINITIONS"));
                if (!pch_exports_define.empty()) { pch_defs.push_back(pch_exports_define); }
                auto info =
                    generate_pch_task(txn, toolchain, this, interp, lang, *result, is_shared,
                                      filter_implicit(evaluate_for_pch(get_resolved_property("INCLUDE_DIRECTORIES"))),
                                      filter_implicit(evaluate_for_pch(get_resolved_property("SYSTEM_INCLUDE_DIRECTORIES"))), pch_defs,
                                      evaluate_for_pch(get_resolved_property("COMPILE_OPTIONS")), default_std, resolved_manual_deps);
                if (!info) return std::unexpected(std::move(info.error()));
                pch_per_lang[lang] = std::move(*info);
            }
        }
    }

    // Single pass: evaluates sources, discovers custom commands, generates compile tasks,
    // and wires dependencies (PRE_BUILD, custom commands, module mapper) inline.
    if (auto r =
            generate_object_tasks(txn, toolchain, obj_files, pch_per_lang, is_shared, is_pie, all_targets, evaluator, interp,
                                  pre_build_task_id, module_mapper_path, generated_custom_tasks, implicit_includes, resolved_manual_deps);
        !r)
        return std::unexpected(std::move(r.error()));

    if (type_ == TargetType::OBJECT_LIBRARY) return {};

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
                    std::string abs = Path::make_absolute_and_normal(dep->get_source_dir(), src);
                    auto sp_it = source_props.find(abs);
                    if (sp_it != source_props.end()) {
                        auto hfo = sp_it->second.find("HEADER_FILE_ONLY");
                        if (hfo != sp_it->second.end() && !Interpreter::is_falsy(hfo->second)) { continue; }
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
        std::erase_if(obj_files, [&](const std::string& o) { return !seen.insert(o).second; });
        if (obj_files.size() < before) {
            kiln::print_message(std::cerr, "WARNING",
                                "target '" + name_ + "' has " + std::to_string(before - obj_files.size())
                                    + " duplicate object file(s), likely from the same OBJECT library "
                                    + "appearing in both $<TARGET_OBJECTS:> and target_link_libraries(). "
                                    + "Duplicates were removed automatically for compatibility. "
                                    + "Consider removing the redundant reference to clean this up.");
        }
    }

    auto sources_list = get_property_list("SOURCES", TargetPropertyScope::BUILD);
    // Determine linker language. If any source is C++, use g++ for linking.
    // For extensionless sources, check if a C++ file exists with that base name.
    Language linker_lang = Language::C;
    for (const auto& src : sources_list) {
        auto info = LanguageClassifier::from_path(src);
        if (info.lang == Language::CXX) {
            linker_lang = Language::CXX;
            break;
        }
        if (info.lang == Language::UNKNOWN) {
            // Try resolving extensionless source (CMake compat)
            std::string abs = Path::make_absolute_and_normal(source_dir_, src);
            for (auto ext : {".cpp", ".cc", ".cxx", ".C", ".c++"}) {
                if (std::filesystem::exists(abs + ext)) {
                    linker_lang = Language::CXX;
                    break;
                }
            }
            if (linker_lang == Language::CXX) break;
        }
    }

    std::string output_path = get_output_path(&evaluator);
    BuildTask link;
    link.id = output_path;
    link.kind = LinkTask{};
    link.parent_target = this;
    // Run linker from the target's binary dir so relative paths in linker
    // flags (e.g. -Map=foo.map, response files) resolve where the project
    // expects, matching CMake.
    link.working_dir = binary_dir_;

    // Use resolved LINK_LIBRARIES directly — resolve() already flattened
    // transitive deps. Circular static lib deps (e.g. MariaDB) are handled
    // at link time via --start-group/--end-group in the linker command.
    std::vector<std::string> full_link_libs;
    if (type_ != TargetType::STATIC_LIBRARY) {
        // Deduplicate while preserving order.
        // Filter out NOTFOUND values — CMake errors on these at generate time,
        // but we warn and skip to avoid passing garbage like -lFOO-NOTFOUND to ld.
        std::unordered_set<std::string> seen;
        for (const auto& lib : get_resolved_property("LINK_LIBRARIES")) {
            if (lib.ends_with("-NOTFOUND")) {
                kiln::print_message(std::cerr, "WARNING", "target '" + name_ + "' links '" + lib + "' which was not found - skipping");
                continue;
            }
            if (seen.insert(lib).second) { full_link_libs.push_back(lib); }
        }

        // If the target is C but links against libraries containing C++ code,
        // upgrade to g++ for linking so C++ runtime symbols resolve.
        if (linker_lang == Language::C) {
            // Cached map: output_path → has_cxx_sources (rebuilt when all_targets changes).
            // thread_local: parallel EP orchestrators each pass a different
            // all_targets map; sharing this cache across threads races.
            thread_local const TargetMap* cached_source = nullptr;
            thread_local std::unordered_map<std::string, bool> output_has_cxx;
            if (&all_targets != cached_source) {
                cached_source = &all_targets;
                output_has_cxx.clear();
                for (const auto& [name, tgt] : all_targets) {
                    std::string tgt_output = tgt->get_output_path();
                    if (tgt_output.empty()) continue;
                    bool has_cxx = false;
                    for (const auto& src : tgt->get_property_list("SOURCES", TargetPropertyScope::BUILD)) {
                        if (LanguageClassifier::from_path(src).lang == Language::CXX) {
                            has_cxx = true;
                            break;
                        }
                    }
                    output_has_cxx[tgt_output] = has_cxx;
                }
            }
            for (const auto& lib : full_link_libs) {
                auto it = output_has_cxx.find(lib);
                if (it != output_has_cxx.end() && it->second) {
                    linker_lang = Language::CXX;
                    break;
                }
            }
        }
    }

    const Compiler* linker = resolve_compiler(linker_lang, toolchain);
    if (!linker) { throw std::runtime_error("No linker available for language in target '" + name_ + "'"); }

    if (type_ == TargetType::STATIC_LIBRARY) {
        auto archive_cmd = linker->get_archive_command(output_path, obj_files);
        // Static archives don't run a compiler/linker; CMake doesn't apply
        // launchers here either, so leave the argv untouched.
        link.commands.push_back(std::move(archive_cmd));
    } else {
        LinkContext ctx;
        ctx.output = output_path;
        ctx.objects = obj_files;
        ctx.is_shared = is_shared;
        ctx.is_pie = is_pie;

        // Determine standard: use highest of explicit standard or required by compile features
        {
            int required_std = 0;
            const auto& cf = get_resolved_property("COMPILE_FEATURES");
            if (!cf.empty()) { required_std = CompileFeatures::instance().get_required_standard(cf, linker_lang); }
            int compiler_default = (linker_lang == Language::CXX) ? cxx_default_std : c_default_std;
            ctx.standard = compute_effective_standard(get_language_standard(linker_lang), required_std, compiler_default);
        }
        ctx.extensions_enabled = get_language_extensions(linker_lang);
        ctx.color_diagnostics = isatty(STDOUT_FILENO);
        ctx.linker_flags = is_shared ? shared_linker_flags : exe_linker_flags;

        // CMake passes CMAKE_<LANG>_FLAGS to both compile AND link commands.
        // Flags like -fsanitize=address require this to work correctly.
        for (const auto& opt : get_language_flags(linker_lang)) { ctx.linker_flags.push_back(opt); }

        // Add target-specific link options (from target_link_options)
        for (const auto& opt : get_resolved_property("LINK_OPTIONS")) { ctx.linker_flags.push_back(opt); }

        // Apply LINK_FLAGS property (deprecated but still used by many projects)
        {
            std::string link_flags = get_property("LINK_FLAGS");
            if (!link_flags.empty()) {
                for (auto& flag : kiln::shell_split(link_flags)) { ctx.linker_flags.push_back(std::move(flag)); }
            }
        }

        for (const auto& lib : full_link_libs) {
            // Some Find modules set LIBRARIES as a space-separated string of
            // flags (e.g. "-L/usr/lib -lmbedtls -lmbedx509"). Split these into
            // individual arguments so they don't get passed as one quoted token.
            if (lib.find(' ') != std::string::npos) {
                for (auto& part : kiln::shell_split(lib)) {
                    if (part.starts_with("-L")) {
                        ctx.lib_dirs.push_back(part.substr(2));
                    } else {
                        ctx.libs.push_back(std::move(part));
                    }
                }
            } else if (lib.starts_with("/") || lib.starts_with("./") || lib.starts_with("../") || lib.find(".so") != std::string::npos
                       || lib.find(".a") != std::string::npos) {
                ctx.objects.push_back(lib);
            } else {
                ctx.libs.push_back(lib);
            }
        }

        for (const auto& dir : get_resolved_property("LINK_DIRECTORIES")) { ctx.lib_dirs.push_back(dir); }

        {
            const char* implicit_var =
                (linker_lang == Language::C) ? "CMAKE_C_IMPLICIT_LINK_DIRECTORIES" : "CMAKE_CXX_IMPLICIT_LINK_DIRECTORIES";
            for (const auto& dir : CMakeArray(interp.get_variable(implicit_var))) { ctx.implicit_link_dirs.push_back(dir); }
        }

        // SKIP_BUILD_RPATH: per-target property overrides the variable default.
        {
            std::string skip = get_property("SKIP_BUILD_RPATH");
            if (skip.empty()) skip = interp.get_variable("CMAKE_SKIP_BUILD_RPATH");
            ctx.skip_build_rpath = !skip.empty() && !Interpreter::is_falsy(skip);
        }

        // BUILD_WITH_INSTALL_RPATH / BUILD_RPATH / INSTALL_RPATH.
        // Per-target property overrides the corresponding CMAKE_* variable.
        std::string bwir = get_property("BUILD_WITH_INSTALL_RPATH");
        if (bwir.empty()) bwir = interp.get_variable("CMAKE_BUILD_WITH_INSTALL_RPATH");
        ctx.build_with_install_rpath = !bwir.empty() && !Interpreter::is_falsy(bwir);

        auto split_rpath = [](std::string val, std::vector<std::string>& out) {
            if (val.empty()) return;
            for (auto sv : CMakeArrayIterator(val)) {
                if (!sv.empty()) out.emplace_back(sv);
            }
        };
        std::string br = get_property("BUILD_RPATH");
        if (br.empty()) br = interp.get_variable("CMAKE_BUILD_RPATH");
        split_rpath(br, ctx.build_rpath);

        std::string ir = get_property("INSTALL_RPATH");
        if (ir.empty()) ir = interp.get_variable("CMAKE_INSTALL_RPATH");
        split_rpath(ir, ctx.install_rpath);

        // VERSION / SOVERSION → DT_SONAME for shared libraries.
        // CMake: SOVERSION defaults to VERSION when only VERSION is set.
        // The actual file is named libfoo.so.<VERSION> (see get_output_path);
        // sibling symlinks libfoo.so.<SOVERSION> and libfoo.so are created
        // after the link command below.
        if (type_ == TargetType::SHARED_LIBRARY) {
            std::string soversion = get_property("SOVERSION");
            if (soversion.empty()) soversion = get_property("VERSION");
            if (!soversion.empty()) { ctx.soname = "lib" + get_output_name() + ".so." + soversion; }
        }

        {
            auto lc = linker->get_link_command(ctx);
            apply_launcher(lc, resolve_launcher(*this, interp, linker_lang, "LINKER"));
            link.commands.push_back(std::move(lc.argv));
            link.signature_commands.push_back(std::move(lc.signature_argv));
        }
    }

    for (const auto& obj : obj_files) {
        link.inputs.push_back(obj);
        link.explicit_deps.push_back(obj);
    }

    // Static libraries are just .o archives — ar doesn't resolve symbols against
    // other libraries. Only executables/shared libraries need link-library deps.
    if (type_ != TargetType::STATIC_LIBRARY) {
        // Cached maps: output_path → non-imported target, output_path → imported target.
        // Rebuilt when all_targets changes (pointer identity check).
        // thread_local: parallel EP orchestrators each pass a different
        // all_targets map; sharing this cache across threads races.
        thread_local const TargetMap* cached_link_source = nullptr;
        thread_local std::unordered_set<std::string> non_imported_outputs;
        thread_local std::unordered_map<std::string, Target*> imported_by_output;
        if (&all_targets != cached_link_source) {
            cached_link_source = &all_targets;
            non_imported_outputs.clear();
            imported_by_output.clear();
            for (const auto& [name, target] : all_targets) {
                std::string out = target->get_output_path();
                if (out.empty()) continue;
                if (target->is_imported()) {
                    imported_by_output[out] = target.get();
                } else {
                    non_imported_outputs.insert(out);
                }
            }
        }

        for (const auto& lib : full_link_libs) {
            if (lib.starts_with("/") || lib.starts_with("./") || lib.starts_with("../")) {
                link.inputs.push_back(lib);

                // Only add to dependencies if a non-imported target produces this.
                if (non_imported_outputs.count(lib)) { link.explicit_deps.push_back(lib); }
            }
        }

        // Propagate imported library dependencies to consumer link task.
        // When linking against an imported library (e.g., mylib with IMPORTED_LOCATION),
        // we need to inherit its manually_added_dependencies (e.g., mylib_ep EP target).
        for (const auto& lib : full_link_libs) {
            auto imp_it = imported_by_output.find(lib);
            if (imp_it == imported_by_output.end()) continue;

            for (const auto& dep_name : imp_it->second->get_manually_added_dependencies()) {
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
        for (const auto& cmd : pre_link_commands_) { link.commands.push_back(cmd.command); }

        resolve_command_target_references(link.commands, link, all_targets, interp.get_target_aliases());

        // Then add the actual link command(s)
        for (auto& cmd : link_cmds) { link.commands.push_back(std::move(cmd)); }
    }

    link.outputs.push_back(output_path);

    // Shared library VERSION/SOVERSION symlinks.
    // Real file (output_path) is libfoo.so.<VERSION>. Create:
    //   libfoo.so.<SOVERSION> -> libfoo.so.<VERSION>   (matches DT_SONAME)
    //   libfoo.so             -> libfoo.so.<SOVERSION> (or directly to real)
    // Symlink contents are basenames so the chain is relocatable.
    if (type_ == TargetType::SHARED_LIBRARY) {
        std::string version = get_property("VERSION");
        std::string soversion = get_property("SOVERSION");
        if (soversion.empty()) soversion = version;
        if (!version.empty() || !soversion.empty()) {
            Path real_path(output_path);
            std::string real_name(real_path.filename());
            std::string dir(real_path.parent_path());
            std::string base = "lib" + get_output_name() + ".so";

            std::string soversion_name = base + (soversion.empty() ? "" : "." + soversion);
            std::string unversioned_name = base;

            auto add_symlink = [&](const std::string& target_name, const std::string& link_name) {
                if (link_name == real_name) return;
                std::string link_abs = dir.empty() ? link_name : (Path(dir) / link_name).str();
                link.commands.push_back({"ln", "-sf", target_name, link_abs});
                link.outputs.push_back(link_abs);
            };

            add_symlink(real_name, soversion_name);
            if (unversioned_name != soversion_name) { add_symlink(soversion_name, unversioned_name); }
        }
    }

    // Register linker side-effect outputs (e.g. -Map= map files,
    // --out-implib= MinGW import libraries). The driver parses its own
    // argv; we just normalize paths against the link working directory.
    // Without this, custom_target DEPENDS pointing at e.g. a -Map file
    // have no producer in the graph and the build stalls.
    {
        const std::string& wd = link.working_dir.empty() ? binary_dir_ : link.working_dir;
        for (const auto& cmd : link.commands) {
            for (auto& raw : linker->get_link_side_effect_outputs(cmd)) {
                if (raw.empty()) continue;
                Path pp(raw);
                link.outputs.push_back(pp.is_absolute() ? pp.lexically_normal().str() : Path::make_absolute_and_normal(wd, raw));
            }
        }
    }
    if (auto r = txn.add(std::move(link)); !r) return std::unexpected(std::move(r.error()));

    // Generate POST_BUILD task if we have any post-build commands
    if (!post_build_commands_.empty()) {
        std::string post_build_task_id = name_ + "_post_build";
        BuildTask post_build;
        post_build.id = post_build_task_id;
        post_build.kind = PostBuildTask{};
        post_build.parent_target = this;
        post_build.working_dir = binary_dir_;

        for (const auto& cmd : post_build_commands_) {
            post_build.commands.push_back(cmd.command);
            if (!cmd.working_dir.empty()) { post_build.working_dir = cmd.working_dir; }
        }

        resolve_command_target_references(post_build.commands, post_build, all_targets, interp.get_target_aliases());

        // POST_BUILD depends on the link task completing
        post_build.explicit_deps.push_back(output_path);
        post_build.inputs.push_back(output_path);

        if (auto r = txn.add(std::move(post_build)); !r) return std::unexpected(std::move(r.error()));
    }
    return {};
}

std::expected<void, std::string> CustomTarget::generate_tasks(GraphTransaction& txn, const Toolchain&, const TargetMap& all_targets,
                                                              const Interpreter& interp, const std::vector<std::string>&,
                                                              const std::vector<std::string>&) {
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
        if (!custom_cmd.working_dir.empty()) { task.working_dir = custom_cmd.working_dir; }
    }

    resolve_command_target_references(task.commands, task, all_targets, interp.get_target_aliases());

    // Handle DEPENDS from add_custom_target
    const auto& target_aliases = interp.get_target_aliases();
    for (const auto& dep_name : custom_depends_) {
        // Skip self-dependencies (CMake silently ignores these)
        if (dep_name == name_) {
            kiln::print_message(std::cerr, "WARNING",
                                "Target '" + name_
                                    + "' has itself in DEPENDS - ignoring self-dependency. "
                                      "If you meant to depend on a file named '"
                                    + dep_name + "', use an absolute path.");
            continue;
        }

        // Resolve target aliases (e.g. Qt6::cmake_automoc_parser -> cmake_automoc_parser)
        auto alias_it = target_aliases.find(dep_name);
        const std::string& resolved_dep = (alias_it != target_aliases.end()) ? alias_it->second : dep_name;
        auto dep_it = all_targets.find(resolved_dep);
        if (dep_it != all_targets.end()) {
            std::string dep_out = dep_it->second->get_output_path();
            if (!dep_out.empty()) {
                task.explicit_deps.push_back(dep_out);
                task.inputs.push_back(dep_out);
            } else {
                task.explicit_deps.push_back(dep_name);
            }
        } else {
            Path p(dep_name);
            std::string normalized;
            if (p.is_absolute()) {
                normalized = p.lexically_normal().str();
            } else {
                normalized = Path::make_absolute_and_normal(source_dir_, dep_name);
            }

            // Check if a custom command rule produces this file
            auto cc_it = custom_rules.find(normalized);
            if (cc_it == custom_rules.end() && p.is_relative()) {
                // Fallback: check binary dir (custom command outputs are registered there)
                auto bin_normalized = Path::make_absolute_and_normal(binary_dir_, dep_name);
                cc_it = custom_rules.find(bin_normalized);
                if (cc_it != custom_rules.end()) { normalized = bin_normalized; }
            }
            if (cc_it != custom_rules.end()) {
                if (auto r = generate_custom_command_task(txn, *cc_it->second, all_targets, custom_rules, generated_cc_tasks,
                                                          interp.get_target_aliases());
                    !r)
                    return std::unexpected(std::move(r.error()));
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

    // Register byproducts as outputs so the build graph knows this target produces them
    for (const auto& bp : byproducts_) { task.outputs.push_back(bp); }

    // Support SOURCES in custom targets just in case
    for (const auto& src : get_property_list("SOURCES", TargetPropertyScope::BUILD)) {
        task.inputs.push_back(Path(src).is_absolute() ? src : Path::join(source_dir_, src));
    }

    if (auto r = txn.add(std::move(task)); !r) return std::unexpected(std::move(r.error()));
    return {};
}

// --- C++20 Modules Support ---

std::string Target::get_module_mapper_path() const {
    return (Path(binary_dir_) / (name_ + ".module-mapper")).lexically_normal().str();
}

std::string Target::get_module_manifest_path() const {
    return (Path(binary_dir_) / (name_ + ".module-exports.json")).lexically_normal().str();
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

// Eagerly schedules the synthetic compile task for the libstdc++ std module
// unit and pre-writes the foreign manifest at config time so this target's
// collator can pick it up like any cross-target export. Idempotent — the
// task id (the std obj path) is shared across all targets, so subsequent
// callers no-op.
//
// Returns the manifest path on success, empty string when std isn't
// available (no CXX compiler, GCC<15, libstdc++.modules.json missing).
static std::expected<std::string, std::string> ensure_std_module_tasks(GraphTransaction& txn, const Compiler* cxx,
                                                                       const std::string& top_binary_dir, int cxx_default_std,
                                                                       bool extensions_enabled,
                                                                       const std::vector<std::string>& compiler_launcher) {
    if (!cxx || !cxx->supports_import_std()) return std::string{};
    std::string json_path = cxx->libstdcxx_modules_json_path();
    if (json_path.empty()) return std::string{};

    auto parsed = parse_libstdcxx_modules_json_file(json_path);
    if (!parsed) return std::string{};

    std::filesystem::path std_dir = std::filesystem::path(top_binary_dir) / "_kiln_std";
    std::string manifest_path = (std_dir / "std.module-exports.json").string();
    std::string mapper_path = (std_dir / "std.mapper").string();
    std::string obj_path = (std_dir / "std.o").string();
    std::string bmi_path = (std_dir / "bmis" / "std.gcm").string();

    std::error_code ec;
    std::filesystem::create_directories(std_dir, ec);
    std::filesystem::create_directories(std_dir / "bmis", ec);

    std::filesystem::path json_dir = std::filesystem::path(json_path).parent_path();
    std::string src_abs;
    for (const auto& mod : parsed->modules) {
        if (mod.logical_name != "std") continue;
        std::filesystem::path src_path = (json_dir / mod.source_path).lexically_normal();
        src_abs = std::filesystem::weakly_canonical(src_path, ec).string();
        if (ec) src_abs = src_path.string();
        break;
    }
    if (src_abs.empty()) return std::string{};

    if (!txn.has_task(obj_path)) {
        std::string standard = "23";
        if (cxx_default_std >= 23) standard = std::to_string(cxx_default_std);

        const auto* clang = dynamic_cast<const ClangCompiler*>(cxx);
        const bool clang_rsp = cxx->uses_per_task_module_rsp();
        std::string late_bound_path = clang_rsp ? (obj_path + ".modules.rsp") : mapper_path;

        CompileContext ctx;
        ctx.source = src_abs;
        ctx.output = obj_path;
        ctx.standard = standard;
        ctx.extensions_enabled = extensions_enabled;
        ctx.is_module_source = true;
        ctx.module_mapper_file = mapper_path; // Unused on clang; harmless.
        ctx.bmi_output = bmi_path;
        if (clang && clang->import_std_uses_libcxx()) { ctx.options.push_back("-stdlib=libc++"); }
        if (clang_rsp) {
            // libstdc++'s std module references reserved identifiers as part
            // of its implementation; clang warns by default. Silencing
            // matches what the libc++/libstdc++ build systems do.
            ctx.options.push_back("-Wno-reserved-module-identifier");
            ctx.options.push_back("-Wno-reserved-identifier");
        }
        auto cmd = cxx->get_compile_command(ctx);
        apply_launcher(cmd, compiler_launcher);

        // Pre-write the late-bound file the std compile reads:
        //   GCC:   mapper at mapper_path: `std <bmi>`
        //   Clang: rsp at <obj>.modules.rsp: `-fmodule-output=<bmi>`
        // Skip the write if content is unchanged so the file's mtime stays
        // stable; otherwise every config rerun bumps it and forces the std
        // compile to invalidate its signature.
        {
            std::string desired = clang_rsp ? ("-fmodule-output=" + bmi_path + "\n") : ("std " + bmi_path + "\n");
            std::ifstream existing(late_bound_path);
            std::string current((std::istreambuf_iterator<char>(existing)), std::istreambuf_iterator<char>());
            if (current != desired) {
                std::ofstream m(late_bound_path);
                m << desired;
            }
        }

        BuildTask task;
        task.id = obj_path;
        task.kind = CompileTask{src_abs, Language::CXX};
        task.commands.push_back(std::move(cmd.argv));
        task.signature_commands.push_back(std::move(cmd.signature_argv));
        task.inputs.push_back(src_abs);
        task.inputs.push_back(late_bound_path);
        task.outputs.push_back(obj_path);
        task.outputs.push_back(bmi_path);
        if (auto r = txn.add(std::move(task)); !r) return std::unexpected(std::move(r.error()));

        ModuleManifest manifest;
        ModuleManifestEntry me;
        me.logical_name = "std";
        me.bmi_path = bmi_path;
        me.primary_output = obj_path;
        me.source_path = src_abs;
        me.visibility = "PUBLIC";
        manifest.entries.push_back(std::move(me));
        (void) write_module_manifest(manifest_path, manifest);
    }

    return manifest_path;
}

std::expected<bool, std::string> Target::generate_module_scanner_tasks(GraphTransaction& txn, const Toolchain& toolchain,
                                                                       const TargetMap& all_targets, const Interpreter& interp,
                                                                       int cxx_default_std) {
    std::vector<std::string> scanner_ids;

    auto sources = get_property_list("SOURCES", TargetPropertyScope::BUILD);

    // Decide whether this target participates in modules. Two reasons it might:
    //   1. It has its own module-interface unit (it provides a module).
    //   2. It links a transitive PUBLIC/INTERFACE dep that provides modules,
    //      meaning some TU here may `import` that module. We can't know which
    //      TUs do without scanning, so scan all CXX TUs once we're committed.
    bool has_interface = false;
    for (const auto& src : sources) {
        auto info = LanguageClassifier::from_path(src);
        if (is_in_cxx_modules_file_set(src)) info.is_module_interface = true;
        if (info.lang == Language::CXX && info.is_module_interface) {
            has_interface = true;
            break;
        }
    }
    bool has_module_dep = !transitive_module_providing_deps(all_targets).empty();
    bool imports_something = !has_interface && !has_module_dep && target_imports_anything();
    if (!has_interface && !has_module_dep && !imports_something) return false;

    for (const auto& src : sources) {
        auto lang_info = LanguageClassifier::from_path(src);

        // Override module interface detection if file is in CXX_MODULES file set
        if (is_in_cxx_modules_file_set(src)) { lang_info.is_module_interface = true; }

        // Scan all CXX sources: interface units to discover what they `provide`,
        // regular .cpp files to discover what they `import`. Without scanning
        // importers, the collator can't wire compile-order edges from importer
        // to producer, leaving correctness up to the scheduler.
        if (lang_info.lang != Language::CXX) continue;
        if (lang_info.is_header) continue;

        const Compiler* compiler = resolve_compiler(lang_info.lang, toolchain);
        if (!compiler) continue;

        std::string src_abs = Path::join(source_dir_, src);
        std::string ddi_path = get_ddi_path(binary_dir_, src);
        // The obj path the compile task will produce. P1689 -fdeps-target
        // emits this as the rule's primary-output, which the collator uses
        // to re-join DDIs to obj task ids without path-relativizing.
        std::string obj_path = get_obj_path(binary_dir_, name_, src);

        ModuleScanContext ctx;
        ctx.source = src_abs;
        ctx.output = ddi_path;
        ctx.obj_path = obj_path;
        ctx.depfile = ddi_path + ".d";

        // Determine standard: use highest of explicit standard or required by compile features
        {
            int required_std = 0;
            const auto& cf = get_resolved_property("COMPILE_FEATURES");
            if (!cf.empty()) { required_std = CompileFeatures::instance().get_required_standard(cf, lang_info.lang); }
            ctx.standard = compute_effective_standard(get_language_standard(lang_info.lang), required_std, cxx_default_std);
        }
        ctx.extensions_enabled = get_language_extensions(lang_info.lang);
        ctx.color_diagnostics = isatty(STDOUT_FILENO);

        // Note: Do NOT automatically add source_dir - only add what's explicitly in INCLUDE_DIRECTORIES
        for (const auto& dir : get_resolved_property("INCLUDE_DIRECTORIES")) { ctx.includes.push_back(dir); }
        for (const auto& dir : get_resolved_property("SYSTEM_INCLUDE_DIRECTORIES")) { ctx.system_includes.push_back(dir); }
        for (const auto& def : get_resolved_property("COMPILE_DEFINITIONS")) { ctx.definitions.push_back(def); }

        BuildTask scanner;
        scanner.id = ddi_path;
        scanner.kind = ModuleScannerTask{src_abs};
        scanner.parent_target = this;
        {
            auto mc = compiler->get_module_scan_command(ctx);
            scanner.commands.push_back(std::move(mc.argv));
            scanner.signature_commands.push_back(std::move(mc.signature_argv));
        }
        scanner.inputs.push_back(src_abs);
        scanner.outputs.push_back(ddi_path);

        if (auto r = txn.add(std::move(scanner)); !r) return std::unexpected(std::move(r.error()));
        scanner_ids.push_back(ddi_path);
    }

    if (!scanner_ids.empty()) {
        const Compiler* cxx = resolve_compiler(Language::CXX, toolchain);
        bool cxx_ext = get_language_extensions(Language::CXX);

        // Schedule the toolchain-level std module compile and write its
        // foreign manifest, but only when this target's sources actually
        // contain `import std;` (cheap textual peek). Empty manifest path
        // when std is unavailable (GCC<15, libstdc++.modules.json missing)
        // — `import std;` then hard-fails at collate time.
        std::string std_manifest;
        if (target_imports_std()) {
            const std::string& top_binary = interp.get_variable("CMAKE_BINARY_DIR");
            auto sm = ensure_std_module_tasks(txn, cxx, top_binary.empty() ? binary_dir_ : top_binary, cxx_default_std, cxx_ext,
                                              resolve_launcher(*this, interp, Language::CXX, "COMPILER"));
            if (!sm) return std::unexpected(std::move(sm.error()));
            std_manifest = std::move(*sm);
        }

        // Pick the C++ standard the header-unit compile should use. Mirrors
        // the per-source standard resolution for normal compiles, but at the
        // target granularity — a single header-unit BMI is shared by all
        // importers in this target, so per-source variance can't be honored.
        int required_std = 0;
        const auto& cf = get_resolved_property("COMPILE_FEATURES");
        if (!cf.empty()) { required_std = CompileFeatures::instance().get_required_standard(cf, Language::CXX); }
        std::string cxx_std_str = compute_effective_standard(get_language_standard(Language::CXX), required_std, cxx_default_std);

        if (auto r = generate_module_collator_task(txn, scanner_ids, all_targets, std_manifest, cxx, cxx_std_str, cxx_ext); !r)
            return std::unexpected(std::move(r.error()));
        return true;
    }

    return false;
}

std::expected<void, std::string>
Target::generate_module_collator_task(GraphTransaction& txn, const std::vector<std::string>& scanner_task_ids, const TargetMap& all_targets,
                                      const std::string& std_module_manifest_path, const Compiler* cxx_compiler,
                                      const std::string& cxx_standard, bool cxx_extensions_enabled) {
    std::string mapper_path = get_module_mapper_path();
    std::string manifest_path = get_module_manifest_path();

    BuildTask collator;
    collator.id = mapper_path;
    ModuleCollatorTask collator_kind{};
    collator_kind.cxx_compiler = cxx_compiler;
    collator_kind.cxx_standard = cxx_standard;
    collator_kind.cxx_extensions_enabled = cxx_extensions_enabled;
    collator.kind = collator_kind;
    collator.parent_target = this;

    // Local: collator depends on every scanner in this target.
    for (const auto& scanner_id : scanner_task_ids) {
        collator.explicit_deps.push_back(scanner_id);
        collator.inputs.push_back(scanner_id);
    }

    // Cross-target: each module-providing dep emits a manifest. Reading that
    // manifest is how this collator learns about modules outside its own
    // scanned sources, so producer collators must finish first — declare an
    // explicit_deps edge on the dep's manifest output.
    for (Target* dep : transitive_module_providing_deps(all_targets)) {
        std::string dep_manifest = dep->get_module_manifest_path();
        collator.explicit_deps.push_back(dep_manifest);
        collator.inputs.push_back(dep_manifest);
    }

    // Toolchain-level std module: foreign manifest for `import std;`. The
    // collator picks it up via the same suffix-detection used for cross-
    // target manifests.
    if (!std_module_manifest_path.empty()) { collator.inputs.push_back(std_module_manifest_path); }

    // Both the per-target mapper (consumed by this target's compiles) and the
    // export manifest (consumed by downstream targets' collators) are graph-
    // visible outputs of this single in-process task.
    collator.outputs.push_back(mapper_path);
    collator.outputs.push_back(manifest_path);

    // Collator has no commands - it's executed in-process by the build graph
    // The actual work happens in BuildGraph::execute() when it detects a collator task

    if (auto r = txn.add(std::move(collator)); !r) return std::unexpected(std::move(r.error()));
    return {};
}

std::vector<Target*> Target::transitive_module_providing_deps(const TargetMap& all_targets) const {
    // Walk resolved LINK_LIBRARIES (already flattened by resolve()) and collect
    // every dep that has its own module-interface units. PRIVATE link deps are
    // already excluded from this property's transitive closure as resolved by
    // Target::resolve, so we don't need to re-filter visibility here.
    std::vector<Target*> result;
    std::unordered_set<Target*> seen;
    auto consider = [&](const std::string& lib) {
        auto it = all_targets.find(lib);
        if (it == all_targets.end()) return;
        Target* dep = it->second.get();
        if (dep == this) return;
        if (!seen.insert(dep).second) return;
        if (dep->has_module_sources()) result.push_back(dep);
    };
    for (const auto& lib : get_resolved_property("LINK_LIBRARIES")) consider(lib);
    // resolved_target_deps_ catches link-only / interface-only edges that
    // resolve() recorded but may have filtered out of res_libs.
    for (const auto& lib : resolved_target_deps_) consider(lib);
    return result;
}

// Cheap textual peek: does any CXX source in this target contain an
// `import` directive? Used to opt targets that don't otherwise look
// module-flavored (no .cppm, no module-providing dep) into the module
// pipeline so that `import std;` and similar resolve. Reads up to a few
// KB per source — full parsing is the scanner's job.
bool Target::target_imports_std() const {
    // Targeted peek: only matches `import std;` or `import std.<x>` to avoid
    // false positives from unrelated `import Foo;` lines.
    auto matches = [](const std::string& buf) {
        size_t pos = 0;
        while (pos < buf.size()) {
            while (pos < buf.size() && (buf[pos] == ' ' || buf[pos] == '\t')) ++pos;
            if (pos + 11 <= buf.size() && buf.compare(pos, 7, "import ") == 0) {
                size_t q = pos + 7;
                while (q < buf.size() && (buf[q] == ' ' || buf[q] == '\t')) ++q;
                if (q + 3 <= buf.size() && buf.compare(q, 3, "std") == 0) {
                    char after = q + 3 < buf.size() ? buf[q + 3] : '\0';
                    if (after == ';' || after == '.' || after == ' ' || after == '\t' || after == '\n') { return true; }
                }
            }
            while (pos < buf.size() && buf[pos] != '\n') ++pos;
            if (pos < buf.size()) ++pos;
        }
        return false;
    };
    for (const auto& src : get_property_list("SOURCES", TargetPropertyScope::BUILD)) {
        auto info = LanguageClassifier::from_path(src);
        if (info.lang != Language::CXX || info.is_header) continue;
        std::string abs = Path(src).is_absolute() ? src : Path::join(source_dir_, src);
        std::ifstream f(abs);
        if (!f) continue;
        std::string buf(8192, '\0');
        f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        buf.resize(static_cast<size_t>(f.gcount()));
        if (matches(buf)) return true;
    }
    return false;
}

bool Target::target_imports_anything() const {
    auto matches = [](const std::string& buf) {
        size_t pos = 0;
        while (pos < buf.size()) {
            // Skip whitespace at line start
            size_t line_start = pos;
            while (pos < buf.size() && (buf[pos] == ' ' || buf[pos] == '\t')) ++pos;
            if (pos + 7 <= buf.size() && buf.compare(pos, 7, "import ") == 0) return true;
            if (pos + 7 <= buf.size() && buf.compare(pos, 7, "import\t") == 0) return true;
            // Advance to next line
            while (pos < buf.size() && buf[pos] != '\n') ++pos;
            if (pos < buf.size()) ++pos;
            (void) line_start;
        }
        return false;
    };

    for (const auto& src : get_property_list("SOURCES", TargetPropertyScope::BUILD)) {
        auto info = LanguageClassifier::from_path(src);
        if (info.lang != Language::CXX || info.is_header) continue;
        std::string abs = Path(src).is_absolute() ? src : Path::join(source_dir_, src);
        std::ifstream f(abs);
        if (!f) continue;
        std::string buf(8192, '\0');
        f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        buf.resize(static_cast<size_t>(f.gcount()));
        if (matches(buf)) return true;
    }
    return false;
}

bool Target::participates_in_modules(const TargetMap& all_targets) const {
    if (has_module_sources()) return true;
    if (!transitive_module_providing_deps(all_targets).empty()) return true;
    return target_imports_anything();
}

} // namespace kiln
