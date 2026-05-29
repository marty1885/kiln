#include "autogen.hpp"
#include "target.hpp"
#include "build_system.hpp"
#include "interperter.hpp"
#include "genex_evaluator.hpp"
#include "CMakeArray.hpp"
#include "printing.hpp"
#include <glaze/glaze.hpp>
#include <pugixml.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <set>
#include <cctype>

namespace kiln {

// Schema for CMake's AutogenInfo.json, consumed by Qt's cmake_automoc_parser.
// HEADERS entries: [path, flags, moc_output_relative, null]
// SOURCES entries: [path, flags, null]
using AutogenHeaderEntry = std::tuple<std::string, std::string, std::string, std::nullptr_t>;
using AutogenSourceEntry = std::tuple<std::string, std::string, std::nullptr_t>;

struct AutogenInfo {
    std::string BUILD_DIR;
    std::string INCLUDE_DIR;
    std::string QT_MOC_EXECUTABLE;
    bool MULTI_CONFIG = false;
    std::vector<AutogenHeaderEntry> HEADERS;
    std::vector<AutogenSourceEntry> SOURCES;
    std::vector<std::string> HEADER_EXTENSIONS = {"h", "hh", "h++", "hm", "hpp", "hxx", "in", "txx"};
};

namespace fs = std::filesystem;

// --- Internal helpers ---

// Compute a short hex checksum of a directory path, used to disambiguate
// moc outputs from headers with the same basename in different directories.
// Uses a simple FNV-1a hash — just needs to be deterministic and unlikely to collide.
static std::string dir_checksum(const std::string& dir) {
    uint64_t hash = 14695981039346656037ULL; // FNV offset basis
    for (char c : dir) {
        hash ^= static_cast<uint64_t>(static_cast<unsigned char>(c));
        hash *= 1099511628211ULL; // FNV prime
    }
    char buf[17];
    snprintf(buf, sizeof(buf), "%012llx", static_cast<unsigned long long>(hash & 0xFFFFFFFFFFFFULL));
    return std::string(buf);
}

// Check if a character is a valid C++ identifier boundary (not alphanumeric or underscore)
static bool is_word_boundary(char c) {
    return !(std::isalnum(static_cast<unsigned char>(c)) || c == '_');
}

// Check if `haystack` contains `needle` as a whole word (not part of a larger identifier)
static bool contains_word(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return false;
    size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        bool left_ok = (pos == 0) || is_word_boundary(haystack[pos - 1]);
        bool right_ok = (pos + needle.size() >= haystack.size()) || is_word_boundary(haystack[pos + needle.size()]);
        if (left_ok && right_ok) return true;
        pos += needle.size();
    }
    return false;
}

// Find the Qt tool executable (moc, uic, or rcc).
// Search order:
//   1. Per-target override property (e.g. AUTOMOC_EXECUTABLE)
//   2. Imported targets (Qt6::moc, Qt5::moc)
//   3. Variables (QT_MOC_EXECUTABLE, Qt6Core_MOC_EXECUTABLE, etc.)
//   4. Error if not found
static std::string find_qt_tool(const Target& target, const Interpreter& interp, const TargetMap& all_targets, GenexEvaluator& evaluator,
                                const std::string& tool_name) // "moc", "uic", or "rcc"
{
    // Map tool name to property/variable names
    std::string upper_tool = tool_name;
    std::transform(upper_tool.begin(), upper_tool.end(), upper_tool.begin(), ::toupper);

    // 1. Per-target override — evaluate genex (e.g. $<TARGET_FILE:Qt6::uic>)
    std::string override_prop = "AUTO" + upper_tool + "_EXECUTABLE";
    std::string override_val = evaluator.evaluate_target_property(target, override_prop);
    if (!override_val.empty()) return override_val;

    // 2. Imported targets: Qt6::<tool>, Qt5::<tool>
    for (const auto& prefix : {"Qt6", "Qt5"}) {
        std::string target_name = std::string(prefix) + "::" + tool_name;
        // Resolve through aliases first
        std::string resolved = interp.resolve_target_alias(target_name);
        auto it = all_targets.find(resolved);
        if (it != all_targets.end() && it->second->is_imported()) {
            std::string loc = it->second->get_imported_location();
            if (!loc.empty()) return loc;
        }
    }

    // 3. Variables
    std::vector<std::string> var_candidates;
    if (tool_name == "moc") {
        var_candidates = {"QT_MOC_EXECUTABLE", "Qt6Core_MOC_EXECUTABLE", "Qt5Core_MOC_EXECUTABLE"};
    } else if (tool_name == "uic") {
        var_candidates = {"QT_UIC_EXECUTABLE", "Qt6Widgets_UIC_EXECUTABLE", "Qt5Widgets_UIC_EXECUTABLE"};
    } else if (tool_name == "rcc") {
        var_candidates = {"QT_RCC_EXECUTABLE", "Qt6Core_RCC_EXECUTABLE", "Qt5Core_RCC_EXECUTABLE"};
    }
    for (const auto& var : var_candidates) {
        std::string val = interp.get_variable(var);
        if (!val.empty()) return val;
    }

    return "";
}

// Scan a file for Qt macros (Q_OBJECT, Q_GADGET, Q_NAMESPACE, etc.)
// Returns: has_macro, list of #include "*.moc" patterns, list of JSON plugin metadata files
struct MocScanResult {
    bool has_macro = false;
    std::vector<std::string> moc_includes; // e.g. "foo.moc", "moc_bar.cpp"
    std::vector<std::string> json_files;   // from Q_PLUGIN_METADATA
};

static MocScanResult scan_file_for_moc(const std::string& path, const std::vector<std::string>& macro_names) {
    MocScanResult result;
    std::ifstream file(path);
    if (!file) { return result; }

    bool in_block_comment = false;
    std::string line;
    while (std::getline(file, line)) {
        // Handle block comments
        if (in_block_comment) {
            auto end = line.find("*/");
            if (end != std::string::npos) {
                in_block_comment = false;
                line = line.substr(end + 2);
            } else {
                continue;
            }
        }

        // Strip block comment starts
        {
            auto start = line.find("/*");
            while (start != std::string::npos) {
                auto end = line.find("*/", start + 2);
                if (end != std::string::npos) {
                    line.erase(start, end - start + 2);
                } else {
                    line.erase(start);
                    in_block_comment = true;
                    break;
                }
                start = line.find("/*");
            }
        }

        // Skip line comments
        {
            auto pos = line.find("//");
            if (pos != std::string::npos) { line = line.substr(0, pos); }
        }

        if (line.empty()) continue;

        // Check for Qt macros
        if (!result.has_macro) {
            for (const auto& macro : macro_names) {
                if (contains_word(line, macro)) {
                    result.has_macro = true;
                    break;
                }
            }
        }

        // Check for #include "*.moc" or #include "moc_*.cpp"
        // Note: C/C++ allows whitespace between # and include (e.g. "# include")
        {
            auto hash_pos = line.find('#');
            size_t pos = std::string::npos;
            if (hash_pos != std::string::npos) {
                auto after_hash = line.find_first_not_of(" \t", hash_pos + 1);
                if (after_hash != std::string::npos && line.compare(after_hash, 7, "include") == 0) { pos = after_hash + 7; }
            }
            if (pos != std::string::npos) {
                // Match both #include "moc_foo.cpp" and #include <moc_foo.cpp>
                std::string inc;
                auto q1 = line.find('"', pos);
                auto a1 = line.find('<', pos);
                if (q1 != std::string::npos) {
                    auto q2 = line.find('"', q1 + 1);
                    if (q2 != std::string::npos) inc = line.substr(q1 + 1, q2 - q1 - 1);
                } else if (a1 != std::string::npos) {
                    auto a2 = line.find('>', a1 + 1);
                    if (a2 != std::string::npos) inc = line.substr(a1 + 1, a2 - a1 - 1);
                }
                if (!inc.empty() && (inc.ends_with(".moc") || inc.starts_with("moc_"))) { result.moc_includes.push_back(inc); }
            }
        }

        // Check for Q_PLUGIN_METADATA(... FILE "foo.json")
        {
            auto pos = line.find("Q_PLUGIN_METADATA");
            if (pos != std::string::npos) {
                auto file_pos = line.find("FILE", pos);
                if (file_pos != std::string::npos) {
                    auto q1 = line.find('"', file_pos + 4);
                    if (q1 != std::string::npos) {
                        auto q2 = line.find('"', q1 + 1);
                        if (q2 != std::string::npos) { result.json_files.push_back(line.substr(q1 + 1, q2 - q1 - 1)); }
                    }
                }
            }
        }
    }

    return result;
}

// Scan a source file for #include "ui_*.h" patterns
struct UicInclude {
    std::string ui_basename;  // "foo" from "ui_foo.h"
    std::string include_path; // "" or "subdir/" if #include "subdir/ui_foo.h"
};

static std::vector<UicInclude> scan_file_for_uic(const std::string& path) {
    std::vector<UicInclude> result;
    std::ifstream file(path);
    if (!file) return result;

    std::string line;
    while (std::getline(file, line)) {
        auto pos = line.find("#include");
        if (pos == std::string::npos) continue;

        // Match both #include "ui_foo.h" and #include <ui_foo.h>
        std::string inc;
        auto after = pos + 8;
        auto q1 = line.find('"', after);
        auto a1 = line.find('<', after);
        if (q1 != std::string::npos) {
            auto q2 = line.find('"', q1 + 1);
            if (q2 != std::string::npos) inc = line.substr(q1 + 1, q2 - q1 - 1);
        } else if (a1 != std::string::npos) {
            auto a2 = line.find('>', a1 + 1);
            if (a2 != std::string::npos) inc = line.substr(a1 + 1, a2 - a1 - 1);
        }
        if (inc.empty()) continue;
        fs::path inc_path(inc);
        std::string filename = inc_path.filename().string();

        if (filename.starts_with("ui_") && (filename.ends_with(".h") || filename.ends_with(".hpp"))) {
            UicInclude ui;
            // Strip "ui_" prefix and extension to get basename
            std::string stem = inc_path.stem().string();
            ui.ui_basename = stem.substr(3); // remove "ui_"
            // Get subdirectory path if present
            if (inc_path.has_parent_path()) {
                ui.include_path = inc_path.parent_path().string();
                if (!ui.include_path.empty() && !ui.include_path.ends_with("/")) { ui.include_path += "/"; }
            }
            result.push_back(std::move(ui));
        }
    }

    return result;
}

// Parse a .qrc file to extract resource file paths
static std::vector<std::string> parse_qrc_resources(const std::string& qrc_path) {
    std::vector<std::string> resources;
    pugi::xml_document doc;
    auto parse_result = doc.load_file(qrc_path.c_str());
    if (!parse_result) return resources;

    fs::path qrc_dir = fs::path(qrc_path).parent_path();

    for (auto file_node : doc.select_nodes("//file")) {
        std::string file_text = file_node.node().text().get();
        if (!file_text.empty()) {
            fs::path resource_path = qrc_dir / file_text;
            resources.push_back(resource_path.lexically_normal().string());
        }
    }

    return resources;
}

// Check if a file exists on disk or is a known custom command output (will be generated).
using CCRuleMap = std::map<std::string, std::shared_ptr<CustomCommandRule>>;
static bool file_exists_or_generated(const std::string& path, const CCRuleMap& cc_rules) {
    return fs::exists(path) || cc_rules.count(path);
}

// Find adjacent headers for a source file
static std::vector<std::string> find_adjacent_headers(const std::string& source_path, const CCRuleMap& cc_rules) {
    std::vector<std::string> headers;
    fs::path src(source_path);
    fs::path dir = src.parent_path();
    std::string stem = src.stem().string();

    // Check for basename variants with common header extensions
    static const std::string extensions[] = {".h", ".hpp", ".hxx", ".hh"};
    static const std::string suffixes[] = {"", "_p"};

    for (const auto& suffix : suffixes) {
        for (const auto& ext : extensions) {
            fs::path candidate = dir / (stem + suffix + ext);
            if (file_exists_or_generated(candidate.lexically_normal().string(), cc_rules)) {
                headers.push_back(candidate.lexically_normal().string());
            }
        }
    }

    return headers;
}

// Build the moc command line
static std::vector<std::string> build_moc_command(const std::string& moc_path, const std::string& input_file,
                                                  const std::string& output_file, const std::vector<std::string>& includes,
                                                  const std::vector<std::string>& definitions,
                                                  const std::vector<std::string>& moc_options) {
    std::vector<std::string> cmd;
    cmd.push_back(moc_path);

    for (const auto& inc : includes) {
        cmd.push_back("-I");
        cmd.push_back(inc);
    }

    for (const auto& def : definitions) {
        if (def.empty()) continue;
        cmd.push_back("-D");
        cmd.push_back(def);
    }

    for (const auto& opt : moc_options) {
        if (opt.empty()) continue;
        cmd.push_back(opt);
    }

    cmd.push_back(input_file);
    cmd.push_back("-o");
    cmd.push_back(output_file);

    return cmd;
}

// --- Main entry point ---

std::expected<void, std::string> generate_autogen_tasks(Target& target, GraphTransaction& txn, Interpreter& interp,
                                                        const TargetMap& all_targets, const std::string& pre_build_task_id,
                                                        const std::vector<std::string>& manual_dep_ids) {
    bool do_moc = !Interpreter::is_falsy(target.get_property("AUTOMOC"));
    bool do_uic = !Interpreter::is_falsy(target.get_property("AUTOUIC"));
    bool do_rcc = !Interpreter::is_falsy(target.get_property("AUTORCC"));

    if (!do_moc && !do_uic && !do_rcc) return {};

    // Set up genex evaluator early — needed for tool discovery and option evaluation
    auto genex_ctx = Target::make_genex_context(&target, interp, all_targets);
    GenexEvaluator evaluator(genex_ctx);

    // Find Qt tools
    std::string moc_path, uic_path, rcc_path;
    if (do_moc) {
        moc_path = find_qt_tool(target, interp, all_targets, evaluator, "moc");
        if (moc_path.empty()) {
            kiln::print_message(std::cerr, "WARNING",
                                "AUTOMOC enabled for target '" + target.get_name()
                                    + "' but moc not found. Did you find_package(Qt6 COMPONENTS Core)?");
            do_moc = false;
        }
    }
    if (do_uic) {
        uic_path = find_qt_tool(target, interp, all_targets, evaluator, "uic");
        if (uic_path.empty()) {
            kiln::print_message(std::cerr, "WARNING",
                                "AUTOUIC enabled for target '" + target.get_name()
                                    + "' but uic not found. Did you find_package(Qt6 COMPONENTS Widgets)?");
            do_uic = false;
        }
    }
    if (do_rcc) {
        rcc_path = find_qt_tool(target, interp, all_targets, evaluator, "rcc");
        if (rcc_path.empty()) {
            kiln::print_message(std::cerr, "WARNING",
                                "AUTORCC enabled for target '" + target.get_name()
                                    + "' but rcc not found. Did you find_package(Qt6 COMPONENTS Core)?");
            do_rcc = false;
        }
    }

    if (!do_moc && !do_uic && !do_rcc) return {};

    // Compute paths
    std::string autogen_dir = (fs::path(target.get_binary_dir()) / (target.get_name() + "_autogen")).string();
    std::string include_dir = (fs::path(autogen_dir) / "include").string();

    // Create directories
    {
        std::error_code ec;
        fs::create_directories(include_dir, ec);
    }

    // Get macro names for moc scanning.
    // CMake default for CMAKE_AUTOMOC_MACRO_NAMES is Q_OBJECT;Q_GADGET;Q_NAMESPACE;Q_NAMESPACE_EXPORT.
    // Per-target AUTOMOC_MACRO_NAMES overrides (Qt appends Q_ENUM_NS;Q_GADGET_EXPORT to it).
    std::vector<std::string> moc_macro_names;
    {
        std::string custom_macros = target.get_property("AUTOMOC_MACRO_NAMES");
        if (custom_macros.empty()) { custom_macros = interp.get_variable("CMAKE_AUTOMOC_MACRO_NAMES"); }
        if (!custom_macros.empty()) {
            for (auto sv : CMakeArrayIterator(custom_macros)) {
                if (!sv.empty()) moc_macro_names.emplace_back(sv);
            }
        }
        if (moc_macro_names.empty()) { moc_macro_names = {"Q_OBJECT", "Q_GADGET", "Q_NAMESPACE", "Q_NAMESPACE_EXPORT"}; }
    }

    // Helper: read a property (with genex evaluation) and split into list
    auto read_option_list = [&](const std::string& prop) -> std::vector<std::string> {
        std::vector<std::string> result;
        std::string val = evaluator.evaluate_target_property(target, prop);
        if (val.empty()) return result;
        for (auto sv : CMakeArrayIterator(val)) { result.emplace_back(sv); }
        return result;
    };

    // Get moc/uic/rcc options
    std::vector<std::string> moc_options = read_option_list("AUTOMOC_MOC_OPTIONS");
    bool moc_output_json = std::find(moc_options.begin(), moc_options.end(), "--output-json") != moc_options.end();
    std::vector<std::string> target_uic_options = read_option_list("AUTOUIC_OPTIONS");
    std::vector<std::string> target_rcc_options = read_option_list("AUTORCC_OPTIONS");

    // AUTOUIC_SEARCH_PATHS
    std::vector<std::string> uic_search_paths = read_option_list("AUTOUIC_SEARCH_PATHS");

    // AUTOGEN_TARGET_DEPENDS: targets/files that all autogen tasks must depend on.
    // Qt uses this to ensure syncqt runs before moc (so framework-style includes resolve).
    std::vector<std::string> autogen_target_deps;
    {
        std::string deps_str = target.get_property_combined("AUTOGEN_TARGET_DEPENDS");
        if (!deps_str.empty()) {
            for (auto sv : CMakeArrayIterator(deps_str)) {
                if (sv.empty()) continue;
                std::string dep(sv);
                // If it's a target name, resolve to its output path (or use name for custom targets)
                auto it = all_targets.find(dep);
                if (it != all_targets.end()) {
                    std::string out = it->second->get_output_path();
                    autogen_target_deps.push_back(out.empty() ? dep : std::move(out));
                } else {
                    // Might be a file path
                    autogen_target_deps.push_back(std::move(dep));
                }
            }
        }
    }

    // Collect target's resolved includes and definitions for moc flags.
    // moc consumes C++ tokens, so evaluate $<COMPILE_LANGUAGE:...> as CXX.
    auto resolved_includes = target.get_resolved_property_for_language("INCLUDE_DIRECTORIES", Language::CXX, interp, all_targets);
    auto resolved_definitions = target.get_resolved_property_for_language("COMPILE_DEFINITIONS", Language::CXX, interp, all_targets);

    // Collect all sources and headers from the target
    auto own_sources = target.get_property_list("SOURCES", TargetPropertyScope::BUILD);

    // Evaluate genex in sources (evaluator created earlier for tool discovery)
    auto eval_result = evaluator.evaluate_property_list(own_sources);
    if (!eval_result) return {}; // silently fail on genex error

    const auto& source_props = interp.get_source_properties();

    // Partition sources into: C++ sources, headers, .qrc files, .ui files
    std::vector<std::string> cpp_sources;
    std::vector<std::string> explicit_headers;
    std::vector<std::string> qrc_files;
    std::set<std::string> all_headers_set; // for dedup

    for (const auto& src : *eval_result) {
        if (src.empty()) continue;

        fs::path src_path(src);
        std::string abs;
        if (src_path.is_absolute()) {
            abs = src_path.lexically_normal().string();
        } else {
            abs = (fs::path(target.get_source_dir()) / src_path).lexically_normal().string();
        }

        // Check skip properties
        auto sp_it = source_props.find(abs);
        auto check_skip = [&](const std::string& prop) -> bool {
            if (sp_it == source_props.end()) return false;
            auto it = sp_it->second.find(prop);
            return it != sp_it->second.end() && !Interpreter::is_falsy(it->second);
        };

        if (check_skip("SKIP_AUTOGEN")) continue;

        std::string ext = src_path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        if (ext == ".qrc") {
            if (!check_skip("SKIP_AUTORCC")) {
                qrc_files.push_back(abs); // existence checked later in AUTORCC section
            }
        } else if (ext == ".h" || ext == ".hpp" || ext == ".hxx" || ext == ".hh") {
            if (!check_skip("SKIP_AUTOMOC") && file_exists_or_generated(abs, interp.get_custom_command_rules())) {
                explicit_headers.push_back(abs);
                all_headers_set.insert(abs);
            }
        } else if (ext == ".cpp" || ext == ".cxx" || ext == ".cc" || ext == ".c") {
            cpp_sources.push_back(abs);
        }
    }

    // Discover adjacent headers for each C++ source
    std::vector<std::string> discovered_headers;
    for (const auto& src : cpp_sources) {
        for (const auto& hdr : find_adjacent_headers(src, interp.get_custom_command_rules())) {
            if (all_headers_set.insert(hdr).second) { discovered_headers.push_back(hdr); }
        }
    }

    // Combine all headers
    std::vector<std::string> all_headers = explicit_headers;
    all_headers.insert(all_headers.end(), discovered_headers.begin(), discovered_headers.end());

    // ========== AUTOMOC ==========
    // Track which moc outputs are explicitly included (via #include "moc_foo.cpp" or "foo.moc")
    // These go to include_dir; non-included ones go into mocs_compilation.cpp
    struct MocEntry {
        std::string input_file;
        std::string output_file;
        std::vector<std::string> extra_inputs; // JSON files
        bool is_source_moc;                    // true = source .moc, false = header moc_*.cpp
        bool is_generated = false;             // input is a custom command output (not yet on disk)
    };
    std::vector<MocEntry> moc_entries;

    // Set of moc outputs that are explicitly included by sources
    std::set<std::string> explicitly_included_mocs;

    if (do_moc) {
        // First pass: scan sources for #include "*.moc" and "moc_*.cpp" patterns
        // This tells us which moc outputs should go to include_dir.
        // We always scan for moc includes (even with SKIP_AUTOMOC) so that
        // explicitly-included moc outputs go to include_dir instead of mocs_compilation.
        std::map<std::string, std::vector<std::string>> source_moc_includes; // source -> moc includes
        for (const auto& src : cpp_sources) {
            auto sp_it = source_props.find(src);
            bool skip_moc = false;
            if (sp_it != source_props.end()) {
                auto it = sp_it->second.find("SKIP_AUTOMOC");
                if (it != sp_it->second.end() && !Interpreter::is_falsy(it->second)) skip_moc = true;
                if (!skip_moc) {
                    it = sp_it->second.find("SKIP_AUTOGEN");
                    if (it != sp_it->second.end() && !Interpreter::is_falsy(it->second)) skip_moc = true;
                }
            }

            auto scan = scan_file_for_moc(src, moc_macro_names);

            // Always record moc includes regardless of SKIP_AUTOMOC
            if (!scan.moc_includes.empty()) {
                source_moc_includes[src] = scan.moc_includes;
                for (const auto& inc : scan.moc_includes) { explicitly_included_mocs.insert(inc); }
            }

            if (skip_moc) continue;

            // Source file with Q_OBJECT: needs "source.moc" include
            if (scan.has_macro) {
                std::string basename = fs::path(src).stem().string();
                std::string moc_output = (fs::path(include_dir) / (basename + ".moc")).string();

                MocEntry entry;
                entry.input_file = src;
                entry.output_file = moc_output;
                entry.is_source_moc = true;
                for (const auto& json : scan.json_files) {
                    fs::path json_path = fs::path(src).parent_path() / json;
                    entry.extra_inputs.push_back(json_path.lexically_normal().string());
                }
                moc_entries.push_back(std::move(entry));
            }
        }

        // Second pass: scan headers for Qt macros
        const auto& cc_rules = interp.get_custom_command_rules();
        for (const auto& hdr : all_headers) {
            bool is_generated = !fs::exists(hdr);
            if (is_generated && !cc_rules.count(hdr)) continue;

            // Check SKIP_AUTOMOC / SKIP_AUTOGEN on the header
            {
                auto sp_it = source_props.find(hdr);
                if (sp_it != source_props.end()) {
                    auto check = [&](const std::string& prop) -> bool {
                        auto it = sp_it->second.find(prop);
                        return it != sp_it->second.end() && !Interpreter::is_falsy(it->second);
                    };
                    if (check("SKIP_AUTOMOC") || check("SKIP_AUTOGEN")) continue;
                }
            }

            // For generated headers that don't exist yet, assume they need MOC
            // (we can't scan them). The MOC task will depend on the generating command.
            MocScanResult scan;
            if (!is_generated) {
                scan = scan_file_for_moc(hdr, moc_macro_names);
                if (!scan.has_macro) continue;
            }

            std::string basename = fs::path(hdr).stem().string();
            std::string moc_filename = "moc_" + basename + ".cpp";

            // Check if this moc output is explicitly included by a source
            bool is_explicitly_included = explicitly_included_mocs.count(moc_filename) > 0;

            std::string moc_output;
            if (is_explicitly_included) {
                // Goes to include_dir so the #include resolves
                moc_output = (fs::path(include_dir) / moc_filename).string();
            } else {
                // Goes to a checksum subdirectory, will be in mocs_compilation.cpp
                std::string checksum = dir_checksum(fs::path(hdr).parent_path().string());
                moc_output = (fs::path(autogen_dir) / checksum / moc_filename).string();
            }

            MocEntry entry;
            entry.input_file = hdr;
            entry.output_file = moc_output;
            entry.is_source_moc = false;
            entry.is_generated = is_generated;
            for (const auto& json : scan.json_files) {
                fs::path json_path = fs::path(hdr).parent_path() / json;
                entry.extra_inputs.push_back(json_path.lexically_normal().string());
            }
            moc_entries.push_back(std::move(entry));
        }

        // Generate moc tasks
        for (const auto& entry : moc_entries) {
            BuildTask task;
            task.id = entry.output_file;
            task.kind = MocTask{entry.input_file};
            task.parent_target = &target;
            task.commands.push_back(
                build_moc_command(moc_path, entry.input_file, entry.output_file, resolved_includes, resolved_definitions, moc_options));
            task.inputs.push_back(entry.input_file);
            for (const auto& json : entry.extra_inputs) { task.inputs.push_back(json); }
            task.outputs.push_back(entry.output_file);
            if (moc_output_json) { task.outputs.push_back(entry.output_file + ".json"); }

            if (!pre_build_task_id.empty()) { task.explicit_deps.push_back(pre_build_task_id); }
            for (const auto& dep : autogen_target_deps) { task.explicit_deps.push_back(dep); }
            for (const auto& dep : manual_dep_ids) { task.explicit_deps.push_back(dep); }
            // Generated headers: MOC must wait for the custom command that creates the input
            if (entry.is_generated) { task.explicit_deps.push_back(entry.input_file); }

            if (auto r = txn.add(std::move(task)); !r) return std::unexpected(r.error());

            // All compile tasks must wait for MOC outputs (same as UIC).
            // On clean builds there are no .d files, so file dep resolution can't
            // discover the #include "moc_*.cpp" / #include "*.moc" dependency.
            target.inject_autogen_dep(entry.output_file);

            // Register as custom command rule so compile tasks wire dependencies
            auto rule = std::make_shared<CustomCommandRule>();
            rule->outputs.push_back(entry.output_file);
            rule->depends.push_back(entry.input_file);
            rule->source_dir = target.get_source_dir();
            rule->binary_dir = target.get_binary_dir();
            interp.get_custom_command_rules()[entry.output_file] = rule;
        }

        // Generate mocs_compilation.cpp
        // Contains #include for all header moc outputs that are NOT explicitly included
        std::set<std::string> moc_output_seen;
        std::vector<std::string> mocs_for_compilation;
        for (const auto& entry : moc_entries) {
            if (!entry.is_source_moc) {
                std::string basename = fs::path(entry.input_file).stem().string();
                std::string moc_filename = "moc_" + basename + ".cpp";
                if (explicitly_included_mocs.count(moc_filename) == 0) {
                    if (moc_output_seen.insert(entry.output_file).second) { mocs_for_compilation.push_back(entry.output_file); }
                }
            }
        }

        if (!mocs_for_compilation.empty()) {
            std::string mocs_comp_path = (fs::path(autogen_dir) / "mocs_compilation.cpp").string();

            // Build content
            std::ostringstream oss;
            oss << "// This file is autogenerated by kiln. Do not edit.\n";
            for (const auto& moc_file : mocs_for_compilation) { oss << "#include \"" << moc_file << "\"\n"; }
            std::string content = oss.str();

            // Only write if changed
            bool needs_write = true;
            if (fs::exists(mocs_comp_path)) {
                std::ifstream existing(mocs_comp_path);
                std::string existing_content((std::istreambuf_iterator<char>(existing)), std::istreambuf_iterator<char>());
                if (existing_content == content) needs_write = false;
            }
            if (needs_write) {
                std::error_code ec;
                fs::create_directories(fs::path(mocs_comp_path).parent_path(), ec);
                std::ofstream out(mocs_comp_path);
                if (out) out << content;
            }

            // Inject mocs_compilation.cpp into target SOURCES
            target.inject_autogen_source(mocs_comp_path);

            // Wire compile task dependency: mocs_compilation.cpp #include's
            // the moc outputs, so its compile task must wait for moc tasks.
            // We use OBJECT_DEPENDS on the source file — generate_object_tasks()
            // adds these as inputs, and file dep resolution wires to moc tasks.
            std::string deps_value;
            for (const auto& moc_file : mocs_for_compilation) {
                if (!deps_value.empty()) deps_value += ';';
                deps_value += moc_file;
            }
            interp.get_source_properties()[mocs_comp_path]["OBJECT_DEPENDS"] = deps_value;
        }

        // Generate AutogenInfo.json for Qt's cmake_automoc_parser.
        // Qt's metatype extraction uses this file to locate moc JSON output.
        {
            std::string autogen_info_dir =
                (fs::path(target.get_binary_dir()) / "CMakeFiles" / (target.get_name() + "_autogen.dir")).string();
            fs::create_directories(autogen_info_dir);
            std::string info_path = (fs::path(autogen_info_dir) / "AutogenInfo.json").string();

            AutogenInfo info;
            info.BUILD_DIR = autogen_dir;
            info.INCLUDE_DIR = include_dir;
            info.QT_MOC_EXECUTABLE = moc_path;

            for (const auto& entry : moc_entries) {
                if (entry.is_source_moc) continue;
                std::string rel = fs::relative(fs::path(entry.output_file), fs::path(autogen_dir)).string();
                info.HEADERS.push_back({entry.input_file, "Mu", rel, nullptr});
            }

            for (const auto& src : cpp_sources) { info.SOURCES.push_back({src, "Mu", nullptr}); }

            std::string content;
            if (auto ec = glz::write<glz::opts{.prettify = true}>(info, content); ec) {
                kiln::print_message(std::cerr, "FATAL_ERROR",
                                    "Failed to serialize AutogenInfo.json for target '" + target.get_name()
                                        + "': " + glz::format_error(ec));
                return {};
            }

            bool needs_write = true;
            if (fs::exists(info_path)) {
                std::ifstream existing(info_path);
                std::string old_content((std::istreambuf_iterator<char>(existing)), std::istreambuf_iterator<char>());
                if (old_content == content) needs_write = false;
            }
            if (needs_write) {
                std::ofstream out(info_path);
                if (out) out << content;
            }

            // Create empty ParseCache.txt — cmake_automoc_parser expects it.
            // kiln handles moc directly, so there's no CMake automoc parse cache.
            std::string parse_cache_path = (fs::path(autogen_info_dir) / "ParseCache.txt").string();
            if (!fs::exists(parse_cache_path)) { std::ofstream pc(parse_cache_path); }
        }
    }

    // ========== AUTOUIC ==========
    if (do_uic) {
        std::set<std::string> generated_uic_outputs;

        // Scan both sources and headers for #include "ui_*.h" patterns
        // (CMake's AUTOUIC scans both; some projects include ui_ headers from .hpp files)
        std::vector<std::string> uic_scan_files;
        uic_scan_files.reserve(cpp_sources.size() + all_headers.size());
        uic_scan_files.insert(uic_scan_files.end(), cpp_sources.begin(), cpp_sources.end());
        uic_scan_files.insert(uic_scan_files.end(), all_headers.begin(), all_headers.end());

        for (const auto& src : uic_scan_files) {
            auto sp_it = source_props.find(src);
            bool skip = false;
            if (sp_it != source_props.end()) {
                auto it = sp_it->second.find("SKIP_AUTOUIC");
                if (it != sp_it->second.end() && !Interpreter::is_falsy(it->second)) skip = true;
                if (!skip) {
                    it = sp_it->second.find("SKIP_AUTOGEN");
                    if (it != sp_it->second.end() && !Interpreter::is_falsy(it->second)) skip = true;
                }
            }
            if (skip) continue;

            auto uic_includes = scan_file_for_uic(src);
            for (const auto& ui_inc : uic_includes) {
                // Find the .ui file
                std::string ui_filename = ui_inc.ui_basename + ".ui";
                std::string ui_file;

                // Search order: source_dir, source_dir/path, search_paths, search_paths/path
                auto try_find = [&](const std::string& base_dir) -> bool {
                    fs::path candidate = fs::path(base_dir) / ui_filename;
                    if (fs::exists(candidate)) {
                        ui_file = candidate.lexically_normal().string();
                        return true;
                    }
                    if (!ui_inc.include_path.empty()) {
                        candidate = fs::path(base_dir) / ui_inc.include_path / ui_filename;
                        if (fs::exists(candidate)) {
                            ui_file = candidate.lexically_normal().string();
                            return true;
                        }
                    }
                    return false;
                };

                // Try source directory of the file that includes it
                if (!try_find(fs::path(src).parent_path().string())) {
                    // Try target source directory
                    if (!try_find(target.get_source_dir())) {
                        // Try AUTOUIC_SEARCH_PATHS
                        for (const auto& search_path : uic_search_paths) {
                            if (try_find(search_path)) break;
                        }
                    }
                }

                if (ui_file.empty()) {
                    kiln::print_message(std::cerr, "WARNING",
                                        "AUTOUIC: Cannot find " + ui_filename + " for target '" + target.get_name() + "' (included from "
                                            + fs::path(src).filename().string() + ")");
                    continue;
                }

                // Generate output header
                std::string output_header = (fs::path(include_dir) / (ui_inc.include_path + "ui_" + ui_inc.ui_basename + ".h")).string();

                // Skip if already generated (same .ui might be included from multiple sources)
                if (!generated_uic_outputs.insert(output_header).second) continue;

                // Get per-source UIC options from the .ui file's properties
                std::vector<std::string> uic_options = target_uic_options;
                auto ui_sp_it = source_props.find(ui_file);
                if (ui_sp_it != source_props.end()) {
                    auto opts_it = ui_sp_it->second.find("AUTOUIC_OPTIONS");
                    if (opts_it != ui_sp_it->second.end()) {
                        uic_options.clear();
                        for (auto sv : CMakeArrayIterator(opts_it->second)) { uic_options.emplace_back(sv); }
                    }
                }

                // Build uic command
                std::vector<std::string> cmd;
                cmd.push_back(uic_path);
                for (const auto& opt : uic_options) { cmd.push_back(opt); }
                cmd.push_back(ui_file);
                cmd.push_back("-o");
                cmd.push_back(output_header);

                BuildTask task;
                task.id = output_header;
                task.kind = UicTask{ui_file};
                task.parent_target = &target;
                task.commands.push_back(std::move(cmd));
                task.inputs.push_back(ui_file);
                task.outputs.push_back(output_header);

                if (!pre_build_task_id.empty()) { task.explicit_deps.push_back(pre_build_task_id); }
                for (const auto& dep : autogen_target_deps) { task.explicit_deps.push_back(dep); }
                for (const auto& dep : manual_dep_ids) { task.explicit_deps.push_back(dep); }

                if (auto r = txn.add(std::move(task)); !r) return std::unexpected(r.error());

                // All compile tasks in this target must wait for UIC outputs,
                // since ui_*.h headers can be included transitively via other headers.
                target.inject_autogen_dep(output_header);

                // Register as custom command rule
                auto rule = std::make_shared<CustomCommandRule>();
                rule->outputs.push_back(output_header);
                rule->depends.push_back(ui_file);
                rule->source_dir = target.get_source_dir();
                rule->binary_dir = target.get_binary_dir();
                interp.get_custom_command_rules()[output_header] = rule;
            }
        }
    }

    // ========== AUTORCC ==========
    if (do_rcc) {
        std::set<std::string> rcc_output_names; // track used output names to disambiguate
        for (const auto& qrc_file : qrc_files) {
            // Skip non-existent .qrc files with a warning
            if (!fs::exists(qrc_file)) {
                kiln::print_message(std::cerr, "WARNING",
                                    "AUTORCC: .qrc file does not exist: " + qrc_file + " (target '" + target.get_name() + "')");
                target.remove_source(qrc_file);
                continue;
            }

            std::string qrc_name = fs::path(qrc_file).stem().string();

            // Disambiguate when multiple .qrc files share the same basename
            std::string output_name = "qrc_" + qrc_name + ".cpp";
            if (!rcc_output_names.insert(output_name).second) {
                // Collision — append a dir checksum to make it unique
                std::string checksum = dir_checksum(fs::path(qrc_file).parent_path().string());
                output_name = "qrc_" + qrc_name + "_" + checksum.substr(0, 8) + ".cpp";
                rcc_output_names.insert(output_name);
            }
            std::string rcc_output = (fs::path(autogen_dir) / output_name).string();

            // Parse .qrc to find resource files (for incremental build tracking)
            auto resource_files = parse_qrc_resources(qrc_file);

            // Check if any resource files are generated by custom commands.
            // If so, add those commands as explicit deps so RCC waits for them.
            std::vector<std::string> generated_resource_deps;
            {
                const auto& rules = interp.get_custom_command_rules();
                for (const auto& res : resource_files) {
                    auto it = rules.find(res);
                    if (it != rules.end()) { generated_resource_deps.push_back(res); }
                }
            }

            // Get per-source RCC options
            std::vector<std::string> rcc_options = target_rcc_options;
            auto sp_it = source_props.find(qrc_file);
            if (sp_it != source_props.end()) {
                auto opts_it = sp_it->second.find("AUTORCC_OPTIONS");
                if (opts_it != sp_it->second.end()) {
                    rcc_options.clear();
                    for (auto sv : CMakeArrayIterator(opts_it->second)) { rcc_options.emplace_back(sv); }
                }
            }

            // Build rcc command
            std::vector<std::string> cmd;
            cmd.push_back(rcc_path);
            for (const auto& opt : rcc_options) { cmd.push_back(opt); }
            cmd.push_back("--name");
            cmd.push_back(qrc_name);
            cmd.push_back(qrc_file);
            cmd.push_back("-o");
            cmd.push_back(rcc_output);

            BuildTask task;
            task.id = rcc_output;
            task.kind = RccTask{qrc_file};
            task.parent_target = &target;
            task.commands.push_back(std::move(cmd));
            task.inputs.push_back(qrc_file);
            for (const auto& res : resource_files) { task.inputs.push_back(res); }
            task.outputs.push_back(rcc_output);

            if (!pre_build_task_id.empty()) { task.explicit_deps.push_back(pre_build_task_id); }
            for (const auto& dep : autogen_target_deps) { task.explicit_deps.push_back(dep); }
            for (const auto& dep : manual_dep_ids) { task.explicit_deps.push_back(dep); }
            for (const auto& dep : generated_resource_deps) { task.explicit_deps.push_back(dep); }

            if (auto r = txn.add(std::move(task)); !r) return std::unexpected(r.error());

            // Register as custom command rule
            auto rule = std::make_shared<CustomCommandRule>();
            rule->outputs.push_back(rcc_output);
            rule->depends.push_back(qrc_file);
            for (const auto& res : resource_files) { rule->depends.push_back(res); }
            rule->source_dir = target.get_source_dir();
            rule->binary_dir = target.get_binary_dir();
            interp.get_custom_command_rules()[rcc_output] = rule;

            // Remove .qrc from SOURCES, inject generated .cpp
            target.remove_source(qrc_file);
            target.inject_autogen_source(rcc_output);
        }
    }

    // Add the autogen include directory to the target's resolved includes
    target.inject_autogen_include(include_dir);
    return {};
}

} // namespace kiln
