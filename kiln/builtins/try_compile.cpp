#include "../interperter.hpp"
#include "../command_parser.hpp"
#include "../target.hpp"
#include "../build_system.hpp"
#include "../cache_store.hpp"
#include "../profiler.hpp"
#include "../utils.hpp"
#include "../CMakeArray.hpp"
#include "../compiler.hpp"
#include "../language.hpp"
#include "../genex_evaluator.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <chrono>

namespace kiln {

namespace {

// Helper: Get file modification time as int64_t (nanoseconds since epoch)
std::expected<int64_t, std::string> get_file_mtime(const std::string& path) {
    std::error_code ec;
    auto ftime = std::filesystem::last_write_time(path, ec);
    if (ec) { return std::unexpected("Failed to get mtime for " + path + ": " + ec.message()); }
    return ftime.time_since_epoch().count();
}

// Helper: Compute BLAKE2b hash of string content
std::string blake2b_hash_string(const std::string& content) {
    return blake2b(content).to_string();
}

// Helper: Detect language from file extension
std::string detect_language(const std::string& path) {
    std::filesystem::path p(path);
    std::string ext = p.extension().string();
    if (ext == ".c") return "C";
    if (ext == ".cpp" || ext == ".cxx" || ext == ".cc" || ext == ".C") return "CXX";
    return "CXX"; // Default to C++
}

// Use kiln::shell_split from utils.hpp
using kiln::shell_split;

// Helper: Parse .d file to extract header dependencies
std::expected<std::vector<std::string>, std::string> parse_deps_file(const std::string& deps_file) {
    std::ifstream file(deps_file);
    if (!file) { return std::unexpected("Failed to open .d file: " + deps_file); }

    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    // Parse Make-style dependencies: "target: dep1 dep2 \ \n dep3"
    std::vector<std::string> headers;
    size_t colon_pos = content.find(':');
    if (colon_pos == std::string::npos) {
        return headers; // No dependencies
    }

    std::string deps_part = content.substr(colon_pos + 1);

    // Remove backslashes and newlines
    std::string cleaned;
    for (size_t i = 0; i < deps_part.size(); ++i) {
        if (deps_part[i] == '\\' && i + 1 < deps_part.size() && deps_part[i + 1] == '\n') {
            i++; // Skip backslash-newline
            continue;
        }
        if (deps_part[i] != '\n') {
            cleaned += deps_part[i];
        } else {
            cleaned += ' ';
        }
    }

    // Split by whitespace
    std::istringstream iss(cleaned);
    std::string dep;
    while (iss >> dep) {
        if (!dep.empty()) { headers.push_back(dep); }
    }

    return headers;
}

// Helper: Compute transparent signature (human-readable, not hashed)
// When use_content_hash is false (default), source files are keyed by mtime for speed.
// When true, source files are keyed by content hash — used as a fallback when the
// mtime-based lookup misses because a file was rewritten with identical content
// (e.g. DetermineGflagsNamespace.cmake rewrites the same .cxx with file(WRITE) each run).
std::expected<std::string, std::string>
compute_signature(const std::string& compiler_path, const std::string& compiler_version, const std::string& language,
                  const std::string& standard, const std::vector<std::string>& source_files,
                  const std::map<std::string, std::string>& inline_sources, // name -> content
                  const std::vector<std::string>& compile_defs, const std::vector<std::string>& link_libs,
                  const std::vector<std::string>& link_opts, bool use_content_hash = false,
                  // Toolchain context — must invalidate the cache when changed even if the
                  // compiler binary path stays the same. Default to empty so existing
                  // host-build entries don't churn unnecessarily.
                  const std::string& sysroot = {}, const std::string& compiler_target = {}, const std::string& global_lang_flags = {}) {
    std::ostringstream oss;

    // Compiler info
    oss << "compiler:" << compiler_path << "|";
    oss << "version:" << compiler_version << "|";
    oss << "lang:" << language << "|";
    oss << "std:" << standard << "|";
    if (!sysroot.empty()) oss << "sysroot:" << sysroot << "|";
    if (!compiler_target.empty()) oss << "target:" << compiler_target << "|";
    if (!global_lang_flags.empty()) oss << "lang_flags:" << global_lang_flags << "|";

    // Source files: mtime (fast path) or content hash (fallback for rewritten files)
    for (const auto& src : source_files) {
        if (use_content_hash) {
            std::ifstream ifs(src, std::ios::binary);
            if (!ifs) return std::unexpected("Cannot read source file: " + src);
            std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            oss << "src:" << src << ":hash:" << blake2b(content).to_string() << "|";
        } else {
            auto mtime_result = get_file_mtime(src);
            if (!mtime_result) return std::unexpected(mtime_result.error());
            oss << "src:" << src << ":" << *mtime_result << "|";
        }
    }

    // Inline sources with content hashes
    for (const auto& [name, content] : inline_sources) {
        std::string hash = blake2b_hash_string(content);
        oss << "inline:" << name << ":" << hash << "|";
    }

    // Compile definitions (sorted for consistency)
    std::vector<std::string> sorted_defs = compile_defs;
    std::sort(sorted_defs.begin(), sorted_defs.end());
    for (const auto& def : sorted_defs) { oss << "def:" << def << "|"; }

    // Link libraries (sorted)
    std::vector<std::string> sorted_libs = link_libs;
    std::sort(sorted_libs.begin(), sorted_libs.end());
    for (const auto& lib : sorted_libs) { oss << "lib:" << lib << "|"; }

    // Link options (sorted)
    std::vector<std::string> sorted_opts = link_opts;
    std::sort(sorted_opts.begin(), sorted_opts.end());
    for (const auto& opt : sorted_opts) { oss << "opt:" << opt << "|"; }

    return oss.str();
}

// Helper: Validate cached header deps.
// Check mtime first (cheap). If mtime changed, compare content hash.
// Returns empty string if all headers are unchanged, or a diagnostic reason string.
std::string validate_header_deps(const std::map<std::string, HeaderDep>& header_deps) {
    for (const auto& [header, dep] : header_deps) {
        auto current_mtime = get_file_mtime(header);
        if (!current_mtime) { return "header deleted: " + header; }
        if (*current_mtime != dep.mtime) {
            // Mtime changed — fall back to content hash
            std::ifstream ifs(header, std::ios::binary);
            if (!ifs) return "header unreadable: " + header;
            std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            if (blake2b(content).to_string() != dep.hash) { return "header content changed: " + header; }
            // Content identical despite mtime change — still valid
        }
    }
    return {};
}

// Filter COMPILE_DEFINITIONS in place: items starting with '-' (but not '-D') are compiler flags.
// CMake modules like CheckCCompilerFlag pass -Wall etc. as COMPILE_DEFINITIONS.
void filter_compile_definitions(std::vector<std::string>& compile_definitions, std::vector<std::string>& raw_compile_flags) {
    std::vector<std::string> filtered;
    for (const auto& item : compile_definitions) {
        if (item.empty()) continue;
        if (item.size() >= 2 && item.substr(0, 2) == "-D") {
            filtered.push_back(item.substr(2));
        } else if (item[0] == '-') {
            for (auto& flag : shell_split(item)) raw_compile_flags.push_back(std::move(flag));
        } else {
            filtered.push_back(item);
        }
    }
    compile_definitions = std::move(filtered);
}

// Process CMAKE_FLAGS list (-DCOMPILE_DEFINITIONS:STRING=..., -DLINK_LIBRARIES=..., etc.)
// and route into the appropriate output buckets.
void process_cmake_flags(const std::vector<std::string>& cmake_flags, std::vector<std::string>& compile_definitions,
                         std::vector<std::string>& raw_compile_flags, std::vector<std::string>& link_libraries,
                         std::vector<std::string>& link_options) {
    for (const auto& flag : cmake_flags) {
        if (flag.size() < 2 || flag.substr(0, 2) != "-D") continue;

        std::string def = flag.substr(2);
        size_t eq = def.find('=');
        if (eq == std::string::npos) continue;

        std::string var_name = def.substr(0, eq);
        std::string value = def.substr(eq + 1);

        // Trim leading/trailing whitespace from value
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(value.begin());
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.pop_back();

        // Strip type annotation (e.g., VAR:STRING -> VAR)
        size_t colon = var_name.find(':');
        if (colon != std::string::npos) var_name = var_name.substr(0, colon);

        if (var_name == "COMPILE_DEFINITIONS") {
            // Can be semicolon-separated CMake list or shell-style; handle both.
            for (auto list_item : CMakeArrayIterator(value)) {
                for (const auto& item : shell_split(list_item)) {
                    if (item.empty()) continue;
                    if (item.size() >= 2 && item.substr(0, 2) == "-D") {
                        compile_definitions.emplace_back(item.substr(2));
                    } else if (item[0] == '-') {
                        raw_compile_flags.emplace_back(item);
                    } else {
                        compile_definitions.emplace_back(item);
                    }
                }
            }
        } else if (var_name == "LINK_LIBRARIES") {
            for (auto lib : CMakeArrayIterator(value)) link_libraries.emplace_back(lib);
        } else if (var_name == "LINK_DIRECTORIES") {
            for (auto dir : CMakeArrayIterator(value)) {
                if (!dir.empty()) link_options.push_back(std::string("-L").append(dir));
            }
        } else if (var_name == "LINK_OPTIONS") {
            for (auto opt : CMakeArrayIterator(value)) link_options.emplace_back(opt);
        } else if (var_name == "INCLUDE_DIRECTORIES") {
            for (auto dir : CMakeArrayIterator(value)) {
                if (!dir.empty()) raw_compile_flags.push_back("-I" + std::string(dir));
            }
        }
    }
}

// Pick the first available source name across the various source vectors,
// for language detection / profiling labels.
std::string first_source_name(const std::vector<std::string>& sources, const std::vector<std::string>& source_from_content,
                              const std::vector<std::string>& source_from_var, const std::vector<std::string>& source_from_file) {
    if (!sources.empty()) return sources[0];
    if (!source_from_content.empty()) return source_from_content[0];
    if (!source_from_var.empty()) return source_from_var[0];
    if (!source_from_file.empty()) return source_from_file[0];
    return {};
}

// Auto-generate bindir if empty, using CMAKE_BINARY_DIR / kiln_scratch_area.
// Returns error string on failure.
std::expected<std::string, std::string> resolve_bindir(const std::string& bindir, Interpreter& interp) {
    if (!bindir.empty()) return bindir;
    std::string cmake_binary_dir = interp.get_variable("CMAKE_BINARY_DIR");
    if (cmake_binary_dir.empty()) { return std::unexpected("requires CMAKE_BINARY_DIR to be set"); }
    return (std::filesystem::path(cmake_binary_dir) / "kiln_scratch_area").string();
}

// Aggregated toolchain/standard context derived from interp + user-provided standards.
struct ToolchainContext {
    Language lang;
    std::string language_str;
    const Compiler* compiler = nullptr;
    std::string compiler_path;
    std::string compiler_version;
    std::string standard;
    bool use_extensions = true;
    std::string sysroot;
    std::string target;
    std::string global_lang_flags;
};

std::expected<ToolchainContext, std::string> get_toolchain_context(Interpreter& interp, const std::string& language_str,
                                                                   const std::string& c_standard, const std::string& cxx_standard) {
    ToolchainContext tc;
    tc.language_str = language_str;
    tc.lang = (language_str == "C") ? Language::C : Language::CXX;
    tc.compiler = interp.get_toolchain().get_compiler(tc.lang);
    if (!tc.compiler) { return std::unexpected("No compiler configured for language: " + language_str); }

    bool is_c = (tc.lang == Language::C);
    tc.compiler_version = interp.get_variable(is_c ? "CMAKE_C_COMPILER_VERSION" : "CMAKE_CXX_COMPILER_VERSION");
    tc.compiler_path = interp.get_variable(is_c ? "CMAKE_C_COMPILER" : "CMAKE_CXX_COMPILER");
    tc.sysroot = interp.get_variable("CMAKE_SYSROOT");
    tc.target = interp.get_variable(is_c ? "CMAKE_C_COMPILER_TARGET" : "CMAKE_CXX_COMPILER_TARGET");
    tc.global_lang_flags = interp.get_variable(is_c ? "CMAKE_C_FLAGS" : "CMAKE_CXX_FLAGS");

    const std::string& user_std = is_c ? c_standard : cxx_standard;
    tc.standard = user_std.empty() ? interp.get_variable(is_c ? "CMAKE_C_STANDARD" : "CMAKE_CXX_STANDARD") : user_std;

    std::string ext_value = interp.get_variable(is_c ? "CMAKE_C_EXTENSIONS" : "CMAKE_CXX_EXTENSIONS");
    tc.use_extensions = ext_value.empty() || !Interpreter::is_falsy(ext_value);
    return tc;
}

// Process pair-form SOURCE_FROM_* arguments into a name->content map.
// Sets fatal_error and returns false on failure.
bool process_inline_sources(Interpreter& interp, const std::vector<std::string>& source_from_content,
                            const std::vector<std::string>& source_from_var, const std::vector<std::string>& source_from_file,
                            std::map<std::string, std::string>& out) {
    if (source_from_content.size() % 2 != 0) {
        interp.set_fatal_error("SOURCE_FROM_CONTENT requires pairs of (name, content)");
        return false;
    }
    for (size_t i = 0; i < source_from_content.size(); i += 2) { out[source_from_content[i]] = source_from_content[i + 1]; }

    if (source_from_var.size() % 2 != 0) {
        interp.set_fatal_error("SOURCE_FROM_VAR requires pairs of (name, varname)");
        return false;
    }
    for (size_t i = 0; i < source_from_var.size(); i += 2) { out[source_from_var[i]] = interp.get_variable(source_from_var[i + 1]); }

    if (source_from_file.size() % 2 != 0) {
        interp.set_fatal_error("SOURCE_FROM_FILE requires pairs of (name, filepath)");
        return false;
    }
    for (size_t i = 0; i < source_from_file.size(); i += 2) {
        std::ifstream file(source_from_file[i + 1]);
        if (!file) {
            interp.set_fatal_error("Failed to read file: " + source_from_file[i + 1]);
            return false;
        }
        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        out[source_from_file[i]] = content;
    }
    return true;
}

// Resolved link libraries plus propagated INTERFACE properties from kiln targets.
struct ResolvedLinks {
    std::vector<std::string> resolved_libs;
    std::vector<std::string> propagated_includes;
    std::vector<std::string> propagated_system_includes;
    std::vector<std::string> propagated_definitions;
    std::vector<std::string> propagated_options;
};

ResolvedLinks resolve_link_libraries(Interpreter& interp, const std::vector<std::string>& link_libraries) {
    ResolvedLinks r;
    auto& targets = interp.get_targets();
    for (const auto& lib : link_libraries) {
        if (targets.count(lib)) {
            auto& target = targets[lib];
            target->resolve(targets, interp);

            const auto& iface_includes = target->get_resolved_interface_property("INCLUDE_DIRECTORIES");
            r.propagated_includes.insert(r.propagated_includes.end(), iface_includes.begin(), iface_includes.end());
            const auto& iface_sys = target->get_resolved_interface_property("SYSTEM_INCLUDE_DIRECTORIES");
            r.propagated_system_includes.insert(r.propagated_system_includes.end(), iface_sys.begin(), iface_sys.end());
            const auto& iface_defs = target->get_resolved_interface_property("COMPILE_DEFINITIONS");
            r.propagated_definitions.insert(r.propagated_definitions.end(), iface_defs.begin(), iface_defs.end());
            const auto& iface_opts = target->get_resolved_interface_property("COMPILE_OPTIONS");
            r.propagated_options.insert(r.propagated_options.end(), iface_opts.begin(), iface_opts.end());

            std::string output_path = target->get_output_path();
            if (!output_path.empty()) r.resolved_libs.push_back(output_path);

            const auto& transitive = target->get_resolved_interface_property("LINK_LIBRARIES");
            r.resolved_libs.insert(r.resolved_libs.end(), transitive.begin(), transitive.end());
        } else {
            r.resolved_libs.push_back(lib);
        }
    }
    return r;
}

// Merge propagated properties into compile_definitions / raw_compile_flags,
// evaluating generator expressions on the way (some imported targets carry
// $<COMPILE_LANGUAGE:...> etc. on their INTERFACE properties).
void merge_propagated_into_flags(Interpreter& interp, Language lang, const ResolvedLinks& links,
                                 std::vector<std::string>& compile_definitions, std::vector<std::string>& raw_compile_flags) {
    auto genex_ctx = GenexEvaluationContext::from_interpreter(interp, interp.get_targets());
    genex_ctx.compile_language = lang;
    GenexEvaluator evaluator(genex_ctx);
    auto eval = [&](const std::string& v) -> std::string {
        auto r = evaluator.evaluate(v);
        return r ? *r : v;
    };

    for (const auto& inc : links.propagated_includes) {
        auto v = eval(inc);
        if (!v.empty()) raw_compile_flags.push_back("-I" + v);
    }
    for (const auto& inc : links.propagated_system_includes) {
        auto v = eval(inc);
        if (!v.empty()) raw_compile_flags.push_back("-isystem" + v);
    }
    for (const auto& def : links.propagated_definitions) {
        auto v = eval(def);
        if (!v.empty()) compile_definitions.push_back(v);
    }
    for (const auto& opt : links.propagated_options) {
        auto v = eval(opt);
        if (!v.empty()) raw_compile_flags.push_back(v);
    }
}

} // anonymous namespace

// Shared structure for compilation parameters
struct CompileParams {
    const Compiler* compiler = nullptr; // Compiler from toolchain (handles extensions, etc.)
    Language lang = Language::CXX;      // Language enum
    std::string compiler_path;          // Fallback if no compiler in toolchain
    std::string compiler_version;
    std::string standard;
    bool use_extensions = true; // Whether to use gnu11 vs c11 (default ON like CMake)
    std::vector<std::string> sources;
    std::map<std::string, std::string> inline_sources_map;
    std::vector<std::string> include_dirs; // Include directories
    std::vector<std::string> compile_definitions;
    std::vector<std::string> raw_compile_flags; // Flags passed as-is (e.g., -Werror)
    std::vector<std::string> resolved_link_libs;
    std::vector<std::string> link_options;
    std::filesystem::path temp_dir;
};

// Shared compilation result
struct CompileResult {
    bool success = false;
    std::string output;
    std::map<std::string, HeaderDep> header_deps;
    std::string executable_path; // For try_run
};

// Shared function: compile source files to executable
std::expected<CompileResult, std::string> compile_sources(const CompileParams& params, bool link_executable = false) {
    CompileResult result;

    if (!params.compiler) { return std::unexpected("No compiler available for try_compile"); }

    // Write inline sources to files
    std::vector<std::string> all_sources = params.sources;
    for (const auto& [name, content] : params.inline_sources_map) {
        std::filesystem::path src_path = params.temp_dir / name;
        std::ofstream file(src_path);
        if (!file) { return std::unexpected("Failed to write source file: " + src_path.string()); }
        file << content;
        file.close();
        all_sources.push_back(src_path.string());
    }

    std::string deps_file = (params.temp_dir / "test.d").string();
    std::vector<std::string> obj_files;

    // Compile each source file to object
    for (const auto& src : all_sources) {
        std::filesystem::path src_path(src);
        std::string obj_file = (params.temp_dir / (src_path.stem().string() + ".o")).string();
        obj_files.push_back(obj_file);

        // Build CompileContext for the Compiler
        CompileContext ctx;
        ctx.source = src;
        ctx.output = obj_file;
        ctx.standard = params.standard;
        ctx.extensions_enabled = params.use_extensions;
        ctx.includes = params.include_dirs;
        // Filter empty definitions to avoid bare "-D" flags
        for (const auto& def : params.compile_definitions) {
            auto trimmed = strip(def);
            if (!trimmed.empty()) { ctx.definitions.emplace_back(trimmed); }
        }
        ctx.options = params.raw_compile_flags;

        std::vector<std::string> compile_cmd = params.compiler->get_compile_command(ctx).argv;

        // Execute compile command
        CommandResult compile_result = run_command(compile_cmd, params.temp_dir.string());
        if (compile_result.exit_code != 0) {
            result.success = false;
            result.output = compile_result.output;
            return result;
        }
        result.output += compile_result.output;

        // Parse header dependencies from .d file
        std::string obj_deps = obj_file + ".d";
        if (std::filesystem::exists(obj_deps)) {
            auto headers_result = parse_deps_file(obj_deps);
            if (headers_result) {
                for (const auto& header : *headers_result) {
                    // Skip inline sources (already captured by signature content hash)
                    bool is_inline = false;
                    std::filesystem::path header_path(header);
                    for (const auto& [inline_name, _] : params.inline_sources_map) {
                        if (header_path.filename() == inline_name) {
                            is_inline = true;
                            break;
                        }
                    }
                    if (is_inline) continue;

                    auto mtime = get_file_mtime(header);
                    if (!mtime) continue;

                    std::ifstream ifs(header, std::ios::binary);
                    if (!ifs) continue;
                    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());

                    result.header_deps[header] = HeaderDep{
                        .mtime = *mtime,
                        .hash = blake2b(content).to_string(),
                    };
                }
            }
        }
    }

    // Link if requested
    if (link_executable) {
        result.executable_path = (params.temp_dir / "test").string();

        LinkContext lctx;
        lctx.output = result.executable_path;
        lctx.objects = obj_files;
        lctx.libs = params.resolved_link_libs;
        // raw_compile_flags must also be passed to the linker — flags like -m32
        // affect both compilation and linking (CMake passes CMAKE_REQUIRED_FLAGS
        // to CMAKE_<LANG>_FLAGS which applies to both stages).
        lctx.linker_flags = params.link_options;
        lctx.linker_flags.insert(lctx.linker_flags.end(), params.raw_compile_flags.begin(), params.raw_compile_flags.end());
        lctx.standard = params.standard;
        lctx.extensions_enabled = params.use_extensions;

        std::vector<std::string> link_cmd = params.compiler->get_link_command(lctx).argv;

        CommandResult link_result = run_command(link_cmd, params.temp_dir.string());
        if (link_result.exit_code != 0) {
            result.success = false;
            result.output += "\n" + link_result.output;
            return result;
        }
        result.output += link_result.output;
    }

    result.success = true;
    return result;
}

void register_try_compile_builtins(Interpreter& interp) {
    interp.add_builtin("try_compile", [](Interpreter& interp, const std::vector<std::string>& args) {
        CommandParser parser("try_compile");

        std::string result_var;
        std::string bindir;
        std::string srcdir_or_srcfile; // Could be source file (old syntax) or source directory (project mode)
        std::string project_name;      // For project mode
        std::string target_name;       // For project mode (optional)
        std::vector<std::string> sources;
        std::vector<std::string> source_from_content; // Pairs: name, content, name, content...
        std::vector<std::string> source_from_var;     // Pairs: name, varname, name, varname...
        std::vector<std::string> source_from_file;    // Pairs: name, filepath, name, filepath...
        std::vector<std::string> compile_definitions;
        std::vector<std::string> raw_compile_flags; // Compiler flags passed as-is (from CMAKE_FLAGS)
        std::vector<std::string> link_libraries;
        std::vector<std::string> link_options;
        std::string cxx_standard;
        std::string c_standard;
        std::string cxx_standard_required;
        std::string c_standard_required;
        std::string output_variable;
        std::string copy_file;
        std::string copy_file_error;
        std::string log_description; // Ignored but must be parsed to avoid polluting other keywords
        bool no_cache = false;
        bool no_log = false;
        std::vector<std::string> cmake_flags;

        parser.positional(result_var, "result variable");
        parser.positional(bindir, "binary directory", false);           // Optional
        parser.positional(srcdir_or_srcfile, "source dir/file", false); // Optional
        parser.positional(project_name, "project name", false);         // Optional (project mode)
        parser.positional(target_name, "target name", false);           // Optional (project mode)
        parser.list("SOURCES", sources);
        parser.list("SOURCE_FROM_CONTENT", source_from_content);
        parser.list("SOURCE_FROM_VAR", source_from_var);
        parser.list("SOURCE_FROM_FILE", source_from_file);
        parser.list("COMPILE_DEFINITIONS", compile_definitions);
        parser.list("LINK_LIBRARIES", link_libraries);
        parser.list("LINK_OPTIONS", link_options);
        parser.list("CMAKE_FLAGS", cmake_flags);
        parser.value("CXX_STANDARD", cxx_standard);
        parser.value("C_STANDARD", c_standard);
        parser.value("CXX_STANDARD_REQUIRED", cxx_standard_required);
        parser.value("C_STANDARD_REQUIRED", c_standard_required);
        parser.value("OUTPUT_VARIABLE", output_variable);
        parser.value("COPY_FILE", copy_file);
        parser.value("COPY_FILE_ERROR", copy_file_error);
        parser.value("LOG_DESCRIPTION", log_description);
        parser.flag("NO_CACHE", no_cache);
        parser.flag("NO_LOG", no_log);

        PARSE_OR_RETURN(parser, interp, args);

        filter_compile_definitions(compile_definitions, raw_compile_flags);

        // Detect project mode vs source mode
        bool project_mode = false;
        if (!srcdir_or_srcfile.empty() && !project_name.empty()) {
            // Project mode: try_compile(result bindir srcdir projectName [targetName])
            std::filesystem::path srcdir_path(srcdir_or_srcfile);
            if (std::filesystem::is_directory(srcdir_path)) { project_mode = true; }
        }

        if (project_mode) {
            // Project mode: spawn an isolated interpreter to build the project
            std::filesystem::path srcdir(srcdir_or_srcfile);
            std::filesystem::path build_dir(bindir);

            // Create build directory
            std::error_code ec;
            std::filesystem::create_directories(build_dir, ec);
            if (ec) {
                interp.set_fatal_error("Failed to create build directory: " + ec.message());
                return;
            }

            // Find CMakeLists.txt in source directory
            std::string cmake_file = (srcdir / "CMakeLists.txt").string();
            if (!std::filesystem::exists(cmake_file)) {
                interp.set_variable(result_var, "FALSE");
                interp.set_cache_variable(result_var, "FALSE");
                if (!output_variable.empty()) { interp.set_variable(output_variable, "CMakeLists.txt not found in " + srcdir.string()); }
                return;
            }

            // Create isolated child interpreter
            std::stringstream child_output;
            Interpreter child_interp(srcdir.string(), &child_output, &child_output, build_dir.string());

            // Apply CMAKE_FLAGS as variables (same pattern as ExternalProject)
            for (const auto& flag : cmake_flags) {
                if (flag.size() >= 2 && flag.substr(0, 2) == "-D") {
                    std::string def = flag.substr(2);
                    size_t eq = def.find('=');
                    if (eq != std::string::npos) {
                        std::string var = def.substr(0, eq);
                        std::string val = def.substr(eq + 1);
                        // Strip type annotation (e.g., VAR:STRING -> VAR)
                        size_t colon = var.find(':');
                        if (colon != std::string::npos) var = var.substr(0, colon);
                        child_interp.set_variable(var, val);
                    }
                }
            }

            // Suppress status messages in child interpreter
            child_interp.set_variable("CMAKE_MESSAGE_LOG_LEVEL", "WARNING");

            // Parse CMakeLists.txt
            std::ifstream file(cmake_file);
            std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            Parser parser(content, cmake_file);
            auto ast = parser.parse();
            if (!ast) {
                interp.set_variable(result_var, "FALSE");
                interp.set_cache_variable(result_var, "FALSE");
                if (!output_variable.empty()) { interp.set_variable(output_variable, "Parse error: " + ast.error().reason); }
                return;
            }

            child_interp.set_current_file(cmake_file);

            // Interpret
            auto interp_result = child_interp.interpret(*ast);
            std::string full_output = child_output.str();
            if (!interp_result) {
                interp.set_variable(result_var, "FALSE");
                interp.set_cache_variable(result_var, "FALSE");
                if (!output_variable.empty()) { interp.set_variable(output_variable, full_output + "\n" + interp_result.error().message); }
                return;
            }

            // Finalize targets (apply directory properties)
            child_interp.execute_deferred_calls();
            child_interp.finalize_directory_targets();

            // Generate build graph and execute
            std::vector<std::string> build_targets;
            if (!target_name.empty()) { build_targets.push_back(target_name); }

            auto graph_result = child_interp.generate_build_graph(build_targets);
            if (!graph_result) {
                full_output += child_output.str();
                interp.set_variable(result_var, "FALSE");
                interp.set_cache_variable(result_var, "FALSE");
                if (!output_variable.empty()) { interp.set_variable(output_variable, full_output + "\n" + graph_result.error().message); }
                return;
            }

            auto& graph = *graph_result;
            graph.apply_cmake_compat_deps();

            std::string captured_build_output;
            auto exec_result = graph.execute(build_dir.string(), 1, &captured_build_output);
            full_output += child_output.str();
            full_output += captured_build_output;

            bool success = exec_result.has_value();
            interp.set_variable(result_var, success ? "TRUE" : "FALSE");
            interp.set_cache_variable(result_var, success ? "TRUE" : "FALSE");
            if (!output_variable.empty()) {
                if (!success) full_output += "\n" + exec_result.error();
                interp.set_variable(output_variable, full_output);
            }
            return;
        }

        // Source mode: handle old-style syntax try_compile(result bindir srcfile)
        if (!srcdir_or_srcfile.empty()) {
            if (bindir.empty()) {
                interp.set_fatal_error("try_compile with source file requires binary directory");
                return;
            }
            sources.push_back(srcdir_or_srcfile);
        }

        // Auto-generate bindir if not specified (CMake 3.25+ behavior).
        if (auto br = resolve_bindir(bindir, interp); br)
            bindir = *br;
        else {
            interp.set_fatal_error("try_compile " + br.error());
            return;
        }

        process_cmake_flags(cmake_flags, compile_definitions, raw_compile_flags, link_libraries, link_options);

        // Validate: need at least one source
        if (sources.empty() && source_from_content.empty() && source_from_var.empty() && source_from_file.empty()) {
            interp.set_fatal_error("try_compile requires at least one source");
            return;
        }

        // Profiling setup
        int64_t profile_start = 0;
        bool profiling = g_profiling_enabled.load(std::memory_order_relaxed);
        std::string profile_src;
        if (profiling) {
            profile_start = Profiler::instance().now_us();
            std::string first = first_source_name(sources, source_from_content, source_from_var, source_from_file);
            profile_src = sources.empty() ? first : std::filesystem::path(first).filename().string();
        }

        // Get cache store
        CacheStore& cache = interp.get_cache_store();

        // Detect language from first source
        std::string first_src = first_source_name(sources, source_from_content, source_from_var, source_from_file);
        std::string language_str = first_src.empty() ? "CXX" : detect_language(first_src);

        auto tc_result = get_toolchain_context(interp, language_str, c_standard, cxx_standard);
        if (!tc_result) {
            interp.set_fatal_error(tc_result.error());
            return;
        }
        auto& tc = *tc_result;

        // Process inline sources
        std::map<std::string, std::string> inline_sources_map;
        if (!process_inline_sources(interp, source_from_content, source_from_var, source_from_file, inline_sources_map)) return;

        // Resolve LINK_LIBRARIES (convert target names to paths + propagate properties)
        ResolvedLinks links = resolve_link_libraries(interp, link_libraries);

        // Merge propagated INTERFACE properties (with genex evaluation) into compile flags.
        // Imported targets like BlocksRuntime::BlocksRuntime carry options like
        // "$<$<COMPILE_LANGUAGE:C,CXX>:-fblocks>" that must be resolved before the test compile.
        merge_propagated_into_flags(interp, tc.lang, links, compile_definitions, raw_compile_flags);
        for (const auto& flag : shell_split(tc.global_lang_flags)) {
            if (!flag.empty()) raw_compile_flags.push_back(flag);
        }

        // Compute initial signature (without header deps)
        auto sig_result = compute_signature(tc.compiler_path, tc.compiler_version, tc.language_str, tc.standard, sources,
                                            inline_sources_map, compile_definitions, links.resolved_libs, link_options,
                                            /*use_content_hash=*/false, tc.sysroot, tc.target, tc.global_lang_flags);
        if (!sig_result) {
            interp.set_fatal_error("Failed to compute signature: " + sig_result.error());
            return;
        }
        std::string base_signature = *sig_result;

        // Check cache: first by mtime-based signature (fast), then by content-hash (handles rewritten files)
        // Track diagnostic info for profiling
        std::string cache_status; // "cached", "miss", "invalidated"
        std::string cache_reason; // details when invalidated or missed

        auto try_cache_hit = [&](const std::string& sig, const char* sig_type) -> bool {
            auto cached = cache.lookup<CacheSubsystem::TryCompile>(sig);
            if (!cached) {
                cache_status = "miss";
                cache_reason = std::string("no cache entry (") + sig_type + ")";
                return false;
            }
            auto reason = validate_header_deps(cached->header_deps);
            if (!reason.empty()) {
                cache_status = "invalidated";
                cache_reason = reason;
                return false;
            }
            cache_status = "cached";
            if (profiling) {
                auto dur = Profiler::instance().now_us() - profile_start;
                Profiler::Args pargs = std::map<std::string, std::string>{
                    {"status", "cached"},
                    {"sig_type", sig_type},
                    {"source", profile_src},
                };
                Profiler::instance().add_complete("try_compile " + profile_src + " (cached)", "configure", profile_start, dur,
                                                  std::move(pargs));
            }
            interp.set_variable(result_var, cached->success ? "TRUE" : "FALSE");
            interp.set_cache_variable(result_var, cached->success ? "TRUE" : "FALSE");
            if (!output_variable.empty()) { interp.set_variable(output_variable, cached->output); }
            return true;
        };

        if (try_cache_hit(base_signature, "mtime")) return;

        // Mtime miss — try content-hash signature (source rewritten with same content?)
        if (!sources.empty()) {
            auto hash_sig = compute_signature(tc.compiler_path, tc.compiler_version, tc.language_str, tc.standard, sources,
                                              inline_sources_map, compile_definitions, links.resolved_libs, link_options,
                                              /*use_content_hash=*/true, tc.sysroot, tc.target, tc.global_lang_flags);
            if (hash_sig && try_cache_hit(*hash_sig, "content-hash")) return;
        }

        // Cache miss - need to compile
        // Create temporary directory
        std::filesystem::path temp_dir =
            std::filesystem::path(bindir) / ".kiln_try_compile" / std::to_string(std::hash<std::string>{}(base_signature));
        std::error_code ec;
        std::filesystem::create_directories(temp_dir, ec);
        if (ec) {
            interp.set_fatal_error("Failed to create temp directory: " + ec.message());
            return;
        }

        // Check CMAKE_TRY_COMPILE_TARGET_TYPE
        std::string target_type = interp.get_variable("CMAKE_TRY_COMPILE_TARGET_TYPE");
        bool build_static_lib = (target_type == "STATIC_LIBRARY");

        // Prepare compilation parameters
        CompileParams params;
        params.compiler = tc.compiler;
        params.lang = tc.lang;
        params.compiler_path = tc.compiler_path;
        params.compiler_version = tc.compiler_version;
        params.standard = tc.standard;
        params.use_extensions = tc.use_extensions;
        params.sources = sources;
        params.inline_sources_map = inline_sources_map;
        params.include_dirs = links.propagated_includes;
        params.compile_definitions = compile_definitions;
        params.raw_compile_flags = raw_compile_flags;
        params.resolved_link_libs = links.resolved_libs;
        params.link_options = link_options;
        params.temp_dir = temp_dir;

        // Compile
        // Default to linking to executable unless building static library
        // This is necessary for check_function_exists which needs to link against system libraries
        bool link_to_executable = !build_static_lib;

        auto compile_result = compile_sources(params, link_to_executable);
        if (!compile_result) {
            interp.set_fatal_error("Compilation failed: " + compile_result.error());
            return;
        }

        bool compile_success = compile_result->success;
        std::string output = compile_result->output;
        auto header_deps = compile_result->header_deps;
        std::string artifact_path;

        // For static library, create the .a file
        if (build_static_lib && compile_success) {
            // compile_sources with link_to_executable=false creates .o files
            // We need to find the object file(s) and create a static library
            // Object files are named after source file stems (e.g., src.c -> src.o)
            std::vector<std::string> obj_files;
            for (const auto& src : sources) {
                std::filesystem::path src_path(src);
                obj_files.push_back((temp_dir / (src_path.stem().string() + ".o")).string());
            }
            for (const auto& [name, _] : inline_sources_map) {
                std::filesystem::path src_path(name);
                obj_files.push_back((temp_dir / (src_path.stem().string() + ".o")).string());
            }

            std::string lib_file = (temp_dir / "libtest.a").string();
            std::vector<std::string> ar_cmd = tc.compiler->get_archive_command(lib_file, obj_files);
            CommandResult ar_result = run_command(ar_cmd, temp_dir.string());

            if (ar_result.exit_code != 0) {
                compile_success = false;
                output += "\nArchive creation failed:\n" + ar_result.output;
            } else {
                artifact_path = lib_file;
            }
        } else if (compile_success) {
            // Executable was built
            artifact_path = compile_result->executable_path;
        }

        // Store in cache under both mtime and content-hash signatures so that
        // future lookups hit regardless of whether the source file was rewritten
        TryCompileCacheEntry entry;
        entry.success = compile_success;
        entry.output = output;
        entry.header_deps = header_deps;
        cache.insert<CacheSubsystem::TryCompile>(base_signature, entry);

        if (!sources.empty()) {
            auto hash_sig = compute_signature(tc.compiler_path, tc.compiler_version, tc.language_str, tc.standard, sources,
                                              inline_sources_map, compile_definitions, links.resolved_libs, link_options,
                                              /*use_content_hash=*/true, tc.sysroot, tc.target, tc.global_lang_flags);
            if (hash_sig && *hash_sig != base_signature) { cache.insert<CacheSubsystem::TryCompile>(*hash_sig, entry); }
        }

        // Handle COPY_FILE if specified
        if (!copy_file.empty() && compile_success && !artifact_path.empty()) {
            std::error_code copy_ec;

            // Create parent directories for copy destination
            std::filesystem::path copy_dest(copy_file);
            if (copy_dest.has_parent_path()) {
                std::filesystem::create_directories(copy_dest.parent_path(), copy_ec);
                if (copy_ec && !copy_file_error.empty()) {
                    interp.set_variable(copy_file_error, "Failed to create directory: " + copy_ec.message());
                    copy_ec.clear();
                }
            }

            // Copy the file
            if (!copy_ec) {
                std::filesystem::copy_file(artifact_path, copy_file, std::filesystem::copy_options::overwrite_existing, copy_ec);
                if (copy_ec && !copy_file_error.empty()) {
                    interp.set_variable(copy_file_error, "Failed to copy file: " + copy_ec.message());
                }
            }
        }

        // Set result variables (both local and cache, matching CMake behavior)
        interp.set_variable(result_var, compile_success ? "TRUE" : "FALSE");
        interp.set_cache_variable(result_var, compile_success ? "TRUE" : "FALSE");
        if (!output_variable.empty()) { interp.set_variable(output_variable, output); }

        if (profiling) {
            auto dur = Profiler::instance().now_us() - profile_start;
            Profiler::Args pargs = std::map<std::string, std::string>{
                {"status", cache_status},      {"reason", cache_reason},
                {"source", profile_src},       {"result", compile_success ? "TRUE" : "FALSE"},
                {"signature", base_signature},
            };
            Profiler::instance().add_complete("try_compile " + profile_src, "configure", profile_start, dur, std::move(pargs));
        }

        // Clean up temp directory on success (keep on failure for debugging)
        if (compile_success) { std::filesystem::remove_all(temp_dir, ec); }
    });

    interp.add_builtin("try_run", [](Interpreter& interp, const std::vector<std::string>& args) {
        CommandParser parser("try_run");

        std::string run_result_var;
        std::string compile_result_var;
        std::string bindir;
        std::string old_style_srcfile;
        std::vector<std::string> sources;
        std::vector<std::string> source_from_content;
        std::vector<std::string> source_from_var;
        std::vector<std::string> source_from_file;
        std::vector<std::string> compile_definitions;
        std::vector<std::string> raw_compile_flags;
        std::vector<std::string> link_libraries;
        std::vector<std::string> link_options;
        std::string cxx_standard;
        std::string c_standard;
        std::string cxx_standard_required;
        std::string c_standard_required;
        std::string compile_output_variable;
        std::string run_output_variable;
        std::string run_output_stdout_variable;
        std::string run_output_stderr_variable;
        std::string working_directory;
        std::vector<std::string> run_args;
        std::vector<std::string> cmake_flags;

        parser.positional(run_result_var, "run result variable");
        parser.positional(compile_result_var, "compile result variable");
        parser.positional(bindir, "binary directory", false);
        parser.positional(old_style_srcfile, "source file (old syntax)", false);
        parser.list("SOURCES", sources);
        parser.list("SOURCE_FROM_CONTENT", source_from_content);
        parser.list("SOURCE_FROM_VAR", source_from_var);
        parser.list("SOURCE_FROM_FILE", source_from_file);
        parser.list("COMPILE_DEFINITIONS", compile_definitions);
        parser.list("LINK_LIBRARIES", link_libraries);
        parser.list("LINK_OPTIONS", link_options);
        parser.list("CMAKE_FLAGS", cmake_flags);
        parser.list("ARGS", run_args);
        parser.value("CXX_STANDARD", cxx_standard);
        parser.value("C_STANDARD", c_standard);
        parser.value("CXX_STANDARD_REQUIRED", cxx_standard_required);
        parser.value("C_STANDARD_REQUIRED", c_standard_required);
        parser.value("COMPILE_OUTPUT_VARIABLE", compile_output_variable);
        parser.value("RUN_OUTPUT_VARIABLE", run_output_variable);
        parser.value("RUN_OUTPUT_STDOUT_VARIABLE", run_output_stdout_variable);
        parser.value("RUN_OUTPUT_STDERR_VARIABLE", run_output_stderr_variable);
        parser.value("WORKING_DIRECTORY", working_directory);

        PARSE_OR_RETURN(parser, interp, args);

        filter_compile_definitions(compile_definitions, raw_compile_flags);

        // Handle old-style syntax
        if (!old_style_srcfile.empty()) {
            if (bindir.empty()) {
                interp.set_fatal_error("try_run with source file requires binary directory");
                return;
            }
            sources.push_back(old_style_srcfile);
        }

        // Profiling
        std::string profile_src;
        if (g_profiling_enabled.load(std::memory_order_relaxed)) {
            std::string first = first_source_name(sources, source_from_content, source_from_var, source_from_file);
            profile_src = sources.empty() ? first : std::filesystem::path(first).filename().string();
        }
        ProfileScope try_run_profile("try_run " + profile_src, "configure");

        if (auto br = resolve_bindir(bindir, interp); br)
            bindir = *br;
        else {
            interp.set_fatal_error("try_run " + br.error());
            return;
        }

        process_cmake_flags(cmake_flags, compile_definitions, raw_compile_flags, link_libraries, link_options);

        // Validate: need at least one source
        if (sources.empty() && source_from_content.empty() && source_from_var.empty() && source_from_file.empty()) {
            interp.set_fatal_error("try_run requires at least one source");
            return;
        }

        // Detect language from first source
        std::string first_src = first_source_name(sources, source_from_content, source_from_var, source_from_file);
        std::string language_str = first_src.empty() ? "CXX" : detect_language(first_src);

        auto tc_result = get_toolchain_context(interp, language_str, c_standard, cxx_standard);
        if (!tc_result) {
            interp.set_fatal_error(tc_result.error());
            return;
        }
        auto& tc = *tc_result;

        // Process inline sources
        std::map<std::string, std::string> inline_sources_map;
        if (!process_inline_sources(interp, source_from_content, source_from_var, source_from_file, inline_sources_map)) return;

        // Resolve LINK_LIBRARIES (convert target names to paths + propagate properties)
        ResolvedLinks links = resolve_link_libraries(interp, link_libraries);

        // Merge propagated INTERFACE properties (with genex evaluation) into compile flags.
        merge_propagated_into_flags(interp, tc.lang, links, compile_definitions, raw_compile_flags);
        for (const auto& flag : shell_split(tc.global_lang_flags)) {
            if (!flag.empty()) raw_compile_flags.push_back(flag);
        }

        // Compute signature for caching (compilation inputs + run args)
        auto sig_result = compute_signature(tc.compiler_path, tc.compiler_version, tc.language_str, tc.standard, sources,
                                            inline_sources_map, compile_definitions, links.resolved_libs, link_options,
                                            /*use_content_hash=*/false, tc.sysroot, tc.target, tc.global_lang_flags);
        if (!sig_result) {
            interp.set_fatal_error("Failed to compute signature: " + sig_result.error());
            return;
        }
        // Extend with run-specific inputs
        std::ostringstream run_sig;
        run_sig << *sig_result;
        for (const auto& arg : run_args) { run_sig << "arg:" << arg << "|"; }
        if (!working_directory.empty()) { run_sig << "workdir:" << working_directory << "|"; }
        for (const auto& flag : raw_compile_flags) { run_sig << "cflag:" << flag << "|"; }
        std::string base_signature = run_sig.str();

        // Check cache
        CacheStore& cache = interp.get_cache_store();

        // Helper to check cache and return early on hit
        auto try_cache_hit = [&](const std::string& sig) -> bool {
            auto cached = cache.lookup<CacheSubsystem::TryRun>(sig);
            if (!cached) return false;
            auto reason = validate_header_deps(cached->header_deps);
            if (!reason.empty()) return false;
            // Cache hit
            interp.set_variable(compile_result_var, cached->compile_success ? "TRUE" : "FALSE");
            interp.set_cache_variable(compile_result_var, cached->compile_success ? "TRUE" : "FALSE");
            if (!compile_output_variable.empty()) { interp.set_variable(compile_output_variable, cached->compile_output); }
            if (!cached->compile_success) {
                interp.set_variable(run_result_var, "FAILED_TO_RUN");
            } else {
                interp.set_variable(run_result_var, std::to_string(cached->exit_code));
                if (!run_output_variable.empty()) { interp.set_variable(run_output_variable, cached->run_output); }
                if (!run_output_stdout_variable.empty()) { interp.set_variable(run_output_stdout_variable, cached->run_output); }
                if (!run_output_stderr_variable.empty()) { interp.set_variable(run_output_stderr_variable, cached->run_output); }
            }
            return true;
        };

        if (try_cache_hit(base_signature)) return;

        // Mtime miss — try content-hash signature (source rewritten with same content?)
        // Compute the content-hash extended signature
        std::string hash_extended_signature;
        if (!sources.empty()) {
            auto hash_sig = compute_signature(tc.compiler_path, tc.compiler_version, tc.language_str, tc.standard, sources,
                                              inline_sources_map, compile_definitions, links.resolved_libs, link_options,
                                              /*use_content_hash=*/true, tc.sysroot, tc.target, tc.global_lang_flags);
            if (hash_sig) {
                std::ostringstream hash_run_sig;
                hash_run_sig << *hash_sig;
                for (const auto& arg : run_args) { hash_run_sig << "arg:" << arg << "|"; }
                if (!working_directory.empty()) { hash_run_sig << "workdir:" << working_directory << "|"; }
                for (const auto& flag : raw_compile_flags) { hash_run_sig << "cflag:" << flag << "|"; }
                hash_extended_signature = hash_run_sig.str();
                if (try_cache_hit(hash_extended_signature)) return;
            }
        }

        // Check for cross-compilation
        bool is_cross_compiling =
            (interp.get_variable("CMAKE_CROSSCOMPILING") == "TRUE" || interp.get_variable("CMAKE_CROSSCOMPILING") == "ON"
             || interp.get_variable("CMAKE_CROSSCOMPILING") == "1");

        // Create temporary directory
        std::filesystem::path temp_dir =
            std::filesystem::path(bindir) / ".kiln_try_run" / std::to_string(std::hash<std::string>{}(base_signature));
        std::error_code ec;
        std::filesystem::create_directories(temp_dir, ec);
        if (ec) {
            interp.set_fatal_error("Failed to create temp directory: " + ec.message());
            return;
        }

        // Prepare compilation parameters
        CompileParams params;
        params.compiler = tc.compiler;
        params.lang = tc.lang;
        params.compiler_path = tc.compiler_path;
        params.compiler_version = tc.compiler_version;
        params.standard = tc.standard;
        params.use_extensions = tc.use_extensions;
        params.sources = sources;
        params.inline_sources_map = inline_sources_map;
        params.include_dirs = links.propagated_includes;
        params.compile_definitions = compile_definitions;
        params.raw_compile_flags = raw_compile_flags;
        params.resolved_link_libs = links.resolved_libs;
        params.link_options = link_options;
        params.temp_dir = temp_dir;

        // Compile to executable
        auto compile_result = compile_sources(params, true);
        if (!compile_result) {
            interp.set_fatal_error("Compilation failed: " + compile_result.error());
            return;
        }

        bool compile_success = compile_result->success;
        std::string compile_output = compile_result->output;

        // Set compile result (both local and cache, matching CMake behavior)
        interp.set_variable(compile_result_var, compile_success ? "TRUE" : "FALSE");
        interp.set_cache_variable(compile_result_var, compile_success ? "TRUE" : "FALSE");
        if (!compile_output_variable.empty()) { interp.set_variable(compile_output_variable, compile_output); }

        // If compilation failed, cache the failure and return
        if (!compile_success) {
            interp.set_variable(run_result_var, "FAILED_TO_RUN");
            TryRunCacheEntry entry;
            entry.compile_success = false;
            entry.compile_output = compile_output;
            entry.header_deps = compile_result->header_deps;
            cache.insert<CacheSubsystem::TryRun>(base_signature, entry);
            // Also insert under content-hash signature for rewritten source files
            if (!hash_extended_signature.empty() && hash_extended_signature != base_signature) {
                cache.insert<CacheSubsystem::TryRun>(hash_extended_signature, entry);
            }
            return;
        }

        // Handle cross-compilation case
        if (is_cross_compiling) {
            std::string emulator = interp.get_variable("CMAKE_CROSSCOMPILING_EMULATOR");
            if (emulator.empty()) {
                // Can't run - require manual cache variables
                // Check if cache variables exist
                std::string cached_exit_code = interp.get_variable(run_result_var);
                std::string cached_output = interp.get_variable(run_result_var + "__TRYRUN_OUTPUT");

                if (cached_exit_code.empty()) {
                    interp.set_fatal_error("Cross-compiling without emulator - please set " + run_result_var
                                           + " cache variable to expected exit code");
                    return;
                }

                // Use cached values
                if (!run_output_variable.empty() && !cached_output.empty()) { interp.set_variable(run_output_variable, cached_output); }
                // Exit code already set via cache variable
                return;
            }
            // If emulator is set, we'll use it below
        }

        // Run the executable
        std::string exe_path = compile_result->executable_path;
        std::string run_dir = working_directory.empty() ? temp_dir.string() : working_directory;

        // Build run command
        std::vector<std::string> run_cmd;

        // Add emulator if cross-compiling
        if (is_cross_compiling) {
            std::string emulator = interp.get_variable("CMAKE_CROSSCOMPILING_EMULATOR");
            if (!emulator.empty()) {
                for (auto part : CMakeArrayIterator(emulator)) { run_cmd.emplace_back(part); }
            }
        }

        run_cmd.push_back(exe_path);
        for (const auto& arg : run_args) { run_cmd.push_back(arg); }

        CommandResult run_result = run_command(run_cmd, run_dir);

        // Set run result (exit code)
        interp.set_variable(run_result_var, std::to_string(run_result.exit_code));

        // Set output variables
        if (!run_output_variable.empty()) { interp.set_variable(run_output_variable, run_result.output); }
        if (!run_output_stdout_variable.empty()) { interp.set_variable(run_output_stdout_variable, run_result.output); }
        if (!run_output_stderr_variable.empty()) {
            // Combined output goes to both for now
            interp.set_variable(run_output_stderr_variable, run_result.output);
        }

        // Cache the result under both mtime and content-hash signatures
        TryRunCacheEntry entry;
        entry.compile_success = true;
        entry.compile_output = compile_output;
        entry.exit_code = run_result.exit_code;
        entry.run_output = run_result.output;
        entry.header_deps = compile_result->header_deps;
        cache.insert<CacheSubsystem::TryRun>(base_signature, entry);
        if (!hash_extended_signature.empty() && hash_extended_signature != base_signature) {
            cache.insert<CacheSubsystem::TryRun>(hash_extended_signature, entry);
        }

        // Clean up temp directory on success
        if (run_result.exit_code == 0) { std::filesystem::remove_all(temp_dir, ec); }
    });
}

} // namespace kiln
