#include "kiln/presets.hpp"

#include <glaze/json/read.hpp>
#include <glaze/json/generic.hpp>

#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <cstdlib>
#include <sys/utsname.h>

namespace kiln {

namespace {

struct RawPreset {
    // The file that contained this preset definition; used as ${fileDir}.
    std::filesystem::path source_file;
    glz::generic data; // The original JSON object node, for field lookups
};

struct PresetIndex {
    // Flat map of preset name -> raw definition. Hidden presets are included
    // here so inherits can resolve to them.
    std::unordered_map<std::string, RawPreset> presets;
    std::filesystem::path root_dir; // dir of root CMakePresets.json
};

std::string expand_fully(const std::string& s, const std::filesystem::path& source_dir, const std::filesystem::path& file_dir,
                         const std::string& preset_name, const std::map<std::string, std::string>& local_env);

std::string read_file(const std::filesystem::path& p) {
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::expected<void, std::string> load_file_into_index(const std::filesystem::path& file, PresetIndex& idx,
                                                      std::unordered_set<std::string>& visited) {
    auto canon = std::filesystem::weakly_canonical(file).string();
    if (!visited.insert(canon).second) return {}; // already loaded

    if (!std::filesystem::exists(file)) { return std::unexpected("preset file not found: " + file.string()); }

    std::string content = read_file(file);
    glz::generic root;
    if (auto ec = glz::read_json(root, content); ec) {
        return std::unexpected("parse error in " + file.string() + ": " + glz::format_error(ec, content));
    }

    if (!root.is_object()) { return std::unexpected(file.string() + ": top-level must be a JSON object"); }
    auto& obj = root.get<glz::generic::object_t>();

    // Recurse into includes first so that local definitions can override.
    if (auto it = obj.find("include"); it != obj.end() && it->second.is_array()) {
        for (const auto& inc : it->second.get<glz::generic::array_t>()) {
            if (!inc.is_string()) continue;
            // CMake allows macro expansion in include paths (notably
            // ${hostSystemName} for OS-specific preset files).
            std::map<std::string, std::string> empty_env;
            std::string raw = inc.get<std::string>();
            std::string expanded = expand_fully(raw, idx.root_dir, file.parent_path(), "", empty_env);
            std::filesystem::path inc_path = expanded;
            if (inc_path.is_relative()) inc_path = file.parent_path() / inc_path;
            if (auto r = load_file_into_index(inc_path, idx, visited); !r) return r;
        }
    }

    if (auto it = obj.find("configurePresets"); it != obj.end() && it->second.is_array()) {
        for (const auto& p : it->second.get<glz::generic::array_t>()) {
            if (!p.is_object()) continue;
            const auto& po = p.get<glz::generic::object_t>();
            auto nit = po.find("name");
            if (nit == po.end() || !nit->second.is_string()) continue;
            std::string name = nit->second.get<std::string>();
            // Includes were processed above, so this assignment lets the
            // current file override anything inherited from them.
            idx.presets[name] = RawPreset{file, p};
        }
    }
    return {};
}

// Walk inherits chain producing an ordered list of presets to merge.
// CMake rule: "If multiple presets in the inherits list provide conflicting
// values for the same field, the earlier preset in the list will be preferred."
// And the preset itself overrides anything from inherits.
// We produce a list in *application order*: ancestors first, self last, so
// later writes win.
std::expected<std::vector<const RawPreset*>, std::string> flatten_inherits(const PresetIndex& idx, const std::string& name,
                                                                           std::unordered_set<std::string>& on_stack,
                                                                           std::unordered_set<std::string>& seen) {
    if (on_stack.count(name)) { return std::unexpected("inherits cycle involving preset '" + name + "'"); }
    auto it = idx.presets.find(name);
    if (it == idx.presets.end()) { return std::unexpected("preset not found: '" + name + "'"); }
    on_stack.insert(name);
    std::vector<const RawPreset*> out;
    const auto& obj = it->second.data.get<glz::generic::object_t>();
    if (auto iit = obj.find("inherits"); iit != obj.end()) {
        // inherits may be a string or array of strings.
        std::vector<std::string> parents;
        if (iit->second.is_string())
            parents.push_back(iit->second.get<std::string>());
        else if (iit->second.is_array()) {
            for (const auto& v : iit->second.get<glz::generic::array_t>()) {
                if (v.is_string()) parents.push_back(v.get<std::string>());
            }
        }
        // CMake: earlier in inherits list wins. Apply later parents first so
        // earlier ones override (since we use last-write-wins merge).
        for (auto pit = parents.rbegin(); pit != parents.rend(); ++pit) {
            auto sub = flatten_inherits(idx, *pit, on_stack, seen);
            if (!sub) return sub;
            for (auto* rp : *sub) out.push_back(rp);
        }
    }
    on_stack.erase(name);
    if (seen.insert(name).second) { out.push_back(&it->second); }
    return out;
}

std::string host_system_name() {
    struct utsname u{};
    if (uname(&u) == 0) return u.sysname;
    return {};
}

// Expand ${var} / $env{VAR} / $penv{VAR} in s.
// `local_env` is the merged environment from the preset chain (so $env{} can
// see preset-defined vars even before they're exported).
std::string expand_macros(const std::string& s, const std::filesystem::path& source_dir, const std::filesystem::path& file_dir,
                          const std::string& preset_name, const std::map<std::string, std::string>& local_env) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        char c = s[i];
        if (c == '$' && i + 1 < s.size()) {
            // ${var}
            if (s[i + 1] == '{') {
                size_t end = s.find('}', i + 2);
                if (end != std::string::npos) {
                    std::string var = s.substr(i + 2, end - (i + 2));
                    if (var == "sourceDir")
                        out += source_dir.string();
                    else if (var == "sourceParentDir")
                        out += source_dir.parent_path().string();
                    else if (var == "sourceDirName")
                        out += source_dir.filename().string();
                    else if (var == "fileDir")
                        out += file_dir.string();
                    else if (var == "presetName")
                        out += preset_name;
                    else if (var == "hostSystemName")
                        out += host_system_name();
                    else if (var == "dollar")
                        out += '$';
                    else if (var == "pathListSep")
                        out += ':';
                    else if (var == "generator") { /* unknown here, leave empty */
                    } else {
                        // Unknown ${var} — leave as-is for visibility.
                        out.append(s, i, end - i + 1);
                    }
                    i = end + 1;
                    continue;
                }
            }
            // $env{VAR} — preset-defined env first, then process env
            else if (s.compare(i, 5, "$env{") == 0) {
                size_t end = s.find('}', i + 5);
                if (end != std::string::npos) {
                    std::string var = s.substr(i + 5, end - (i + 5));
                    if (auto it = local_env.find(var); it != local_env.end()) {
                        out += it->second;
                    } else if (const char* v = std::getenv(var.c_str())) {
                        out += v;
                    }
                    i = end + 1;
                    continue;
                }
            }
            // $penv{VAR} — only process env, never preset-defined
            else if (s.compare(i, 6, "$penv{") == 0) {
                size_t end = s.find('}', i + 6);
                if (end != std::string::npos) {
                    std::string var = s.substr(i + 6, end - (i + 6));
                    if (const char* v = std::getenv(var.c_str())) out += v;
                    i = end + 1;
                    continue;
                }
            }
        }
        out += c;
        ++i;
    }
    return out;
}

// Recursively expand until fixed point (env vars may reference each other,
// e.g. VCPKG_ROOT references LADYBIRD_SOURCE_DIR).
std::string expand_fully(const std::string& s, const std::filesystem::path& source_dir, const std::filesystem::path& file_dir,
                         const std::string& preset_name, const std::map<std::string, std::string>& local_env) {
    std::string cur = s;
    for (int i = 0; i < 16; ++i) {
        std::string next = expand_macros(cur, source_dir, file_dir, preset_name, local_env);
        if (next == cur) break;
        cur = std::move(next);
    }
    return cur;
}

// Evaluate a preset condition object. CMake supports many operators; we
// implement the common subset Ladybird uses (equals/notEquals).
// Returns true (condition met / no condition), false (skip), or error.
std::expected<bool, std::string> eval_condition(const glz::generic& cond, const std::filesystem::path& source_dir,
                                                const std::filesystem::path& file_dir, const std::string& preset_name,
                                                const std::map<std::string, std::string>& local_env) {
    if (cond.holds<std::nullptr_t>()) return true;
    if (cond.holds<bool>()) return cond.get<bool>();
    if (!cond.is_object()) return true;
    const auto& o = cond.get<glz::generic::object_t>();
    auto tit = o.find("type");
    if (tit == o.end() || !tit->second.is_string()) return true;
    std::string type = tit->second.get<std::string>();

    auto get_str = [&](const char* key) -> std::string {
        auto it = o.find(key);
        if (it == o.end() || !it->second.is_string()) return "";
        return expand_fully(it->second.get<std::string>(), source_dir, file_dir, preset_name, local_env);
    };

    if (type == "const") {
        auto it = o.find("value");
        return it != o.end() && it->second.holds<bool>() && it->second.get<bool>();
    }
    if (type == "equals" || type == "notEquals") {
        bool eq = get_str("lhs") == get_str("rhs");
        return type == "equals" ? eq : !eq;
    }
    if (type == "inList" || type == "notInList") {
        std::string s = get_str("string");
        auto lit = o.find("list");
        bool found = false;
        if (lit != o.end() && lit->second.is_array()) {
            for (const auto& v : lit->second.get<glz::generic::array_t>()) {
                if (v.is_string() && expand_fully(v.get<std::string>(), source_dir, file_dir, preset_name, local_env) == s) {
                    found = true;
                    break;
                }
            }
        }
        return type == "inList" ? found : !found;
    }
    if (type == "matches" || type == "notMatches") {
        // Skip regex; treat as true to avoid blocking common presets.
        return true;
    }
    if (type == "anyOf" || type == "allOf") {
        auto cit = o.find("conditions");
        if (cit == o.end() || !cit->second.is_array()) return true;
        bool any_of = (type == "anyOf");
        for (const auto& sub : cit->second.get<glz::generic::array_t>()) {
            auto r = eval_condition(sub, source_dir, file_dir, preset_name, local_env);
            if (!r) return r;
            if (any_of && *r) return true;
            if (!any_of && !*r) return false;
        }
        return !any_of;
    }
    if (type == "not") {
        auto cit = o.find("condition");
        if (cit == o.end()) return true;
        auto r = eval_condition(cit->second, source_dir, file_dir, preset_name, local_env);
        if (!r) return r;
        return !*r;
    }
    return true;
}

std::expected<PresetIndex, std::string> build_index(const std::filesystem::path& project_dir) {
    PresetIndex idx;
    idx.root_dir = project_dir;
    std::unordered_set<std::string> visited;
    for (const char* name : {"CMakePresets.json", "CMakeUserPresets.json"}) {
        auto path = project_dir / name;
        if (!std::filesystem::exists(path)) continue;
        if (auto r = load_file_into_index(path, idx, visited); !r) return std::unexpected(r.error());
    }
    if (idx.presets.empty()) { return std::unexpected("no CMakePresets.json found in " + project_dir.string()); }
    return idx;
}

} // namespace

std::expected<ResolvedPreset, std::string> load_configure_preset(const std::filesystem::path& project_dir, const std::string& preset_name) {
    auto idx_or = build_index(project_dir);
    if (!idx_or) return std::unexpected(idx_or.error());
    auto& idx = *idx_or;

    auto self_it = idx.presets.find(preset_name);
    if (self_it == idx.presets.end()) {
        std::string available;
        for (const auto& [n, p] : idx.presets) {
            const auto& obj = p.data.get<glz::generic::object_t>();
            auto hit = obj.find("hidden");
            if (hit != obj.end() && hit->second.holds<bool>() && hit->second.get<bool>()) continue;
            if (!available.empty()) available += ", ";
            available += n;
        }
        return std::unexpected("unknown preset '" + preset_name + "'. Available: " + available);
    }

    std::unordered_set<std::string> on_stack, seen;
    auto chain_or = flatten_inherits(idx, preset_name, on_stack, seen);
    if (!chain_or) return std::unexpected(chain_or.error());

    // Merge fields with last-write-wins semantics.
    ResolvedPreset r;
    r.name = preset_name;
    std::filesystem::path file_dir; // ${fileDir} = directory of the preset itself

    for (const auto* rp : *chain_or) {
        const auto& obj = rp->data.get<glz::generic::object_t>();
        file_dir = rp->source_file.parent_path();

        if (auto it = obj.find("generator"); it != obj.end() && it->second.is_string()) r.generator = it->second.get<std::string>();
        if (auto it = obj.find("binaryDir"); it != obj.end() && it->second.is_string()) r.binary_dir = it->second.get<std::string>();
        if (auto it = obj.find("toolchainFile"); it != obj.end() && it->second.is_string())
            r.toolchain_file = it->second.get<std::string>();

        if (auto it = obj.find("environment"); it != obj.end() && it->second.is_object()) {
            for (const auto& [k, v] : it->second.get<glz::generic::object_t>()) {
                if (v.is_string())
                    r.environment[k] = v.get<std::string>();
                else if (v.holds<std::nullptr_t>())
                    r.environment.erase(k);
            }
        }
        if (auto it = obj.find("cacheVariables"); it != obj.end() && it->second.is_object()) {
            for (const auto& [k, v] : it->second.get<glz::generic::object_t>()) {
                if (v.is_string()) {
                    r.cache_variables[k] = v.get<std::string>();
                } else if (v.holds<bool>()) {
                    r.cache_variables[k] = v.get<bool>() ? "ON" : "OFF";
                } else if (v.is_object()) {
                    // {"type": "...", "value": "..."} form
                    const auto& vo = v.get<glz::generic::object_t>();
                    auto vv = vo.find("value");
                    if (vv != vo.end()) {
                        if (vv->second.is_string())
                            r.cache_variables[k] = vv->second.get<std::string>();
                        else if (vv->second.holds<bool>())
                            r.cache_variables[k] = vv->second.get<bool>() ? "ON" : "OFF";
                    }
                } else if (v.holds<std::nullptr_t>()) {
                    r.cache_variables.erase(k);
                }
            }
        }
    }

    // Now expand macros. Environment may reference itself, so iterate.
    auto source_dir = std::filesystem::absolute(project_dir).lexically_normal();
    for (int pass = 0; pass < 8; ++pass) {
        bool changed = false;
        for (auto& [k, v] : r.environment) {
            auto nv = expand_fully(v, source_dir, file_dir, preset_name, r.environment);
            if (nv != v) {
                v = std::move(nv);
                changed = true;
            }
        }
        if (!changed) break;
    }
    for (auto& [k, v] : r.cache_variables) { v = expand_fully(v, source_dir, file_dir, preset_name, r.environment); }
    r.binary_dir = expand_fully(r.binary_dir, source_dir, file_dir, preset_name, r.environment);
    r.toolchain_file = expand_fully(r.toolchain_file, source_dir, file_dir, preset_name, r.environment);

    // Condition: check the *self* preset's condition (after expansion).
    const auto& self_obj = self_it->second.data.get<glz::generic::object_t>();
    if (auto cit = self_obj.find("condition"); cit != self_obj.end()) {
        auto cr = eval_condition(cit->second, source_dir, file_dir, preset_name, r.environment);
        if (!cr) return std::unexpected(cr.error());
        if (!*cr) return std::unexpected("preset '" + preset_name + "' condition is false on this host");
    }

    if (auto it = r.cache_variables.find("CMAKE_BUILD_TYPE"); it != r.cache_variables.end()) { r.build_type = it->second; }

    return r;
}

std::expected<std::vector<std::string>, std::string> list_configure_presets(const std::filesystem::path& project_dir) {
    auto idx_or = build_index(project_dir);
    if (!idx_or) return std::unexpected(idx_or.error());
    std::vector<std::string> names;
    for (const auto& [name, rp] : idx_or->presets) {
        const auto& obj = rp.data.get<glz::generic::object_t>();
        auto hit = obj.find("hidden");
        if (hit != obj.end() && hit->second.holds<bool>() && hit->second.get<bool>()) continue;
        names.push_back(name);
    }
    return names;
}

} // namespace kiln
