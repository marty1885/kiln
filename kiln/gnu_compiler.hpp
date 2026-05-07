#pragma once
#include "compiler.hpp"
#include "genex_parser.hpp"
#include "language.hpp"
#include "path.hpp"
#include "parse_number.hpp"
#include <sstream>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <future>
#include <map>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include "regex.hpp"

#ifdef __unix__
#include <sys/utsname.h>
#endif

namespace kiln {

namespace detail {

// Execute a command and capture stdout
inline std::string run_command(const std::string& command) {
    std::array<char, 128> buffer;
    std::string result;

    std::unique_ptr<FILE, int(*)(FILE*)> pipe(popen(command.c_str(), "r"), pclose);
    if (!pipe) {
        return "";
    }

    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        result += buffer.data();
    }

    return result;
}

} // namespace detail

class GnuCompiler : public Compiler {
public:
    explicit GnuCompiler(std::string binary, Language lang,
                         std::string sysroot = {}, std::string compiler_target = {})
        : binary_(std::move(binary)), lang_(lang),
          sysroot_(std::move(sysroot)), compiler_target_(std::move(compiler_target)) {}

    const std::string& binary() const override { return binary_; }
    const std::string& sysroot() const override { return sysroot_; }
    const std::string& compiler_target() const override { return compiler_target_; }

    // Inject --sysroot= and --target= flags. Call after pushing the binary,
    // before language-/job-specific flags. Idempotent w.r.t. empty fields.
    // Caller is responsible for only populating compiler_target_ when the
    // compiler honors --target= (Clang-likes); GCC errors on it.
    void inject_target_flags(std::vector<std::string>& cmd) const {
        if (!sysroot_.empty()) {
            cmd.push_back("--sysroot=" + sysroot_);
        }
        if (!compiler_target_.empty()) {
            cmd.push_back("--target=" + compiler_target_);
        }
    }

    // Build a CompilerCommand from an argv and the indices of tokens that
    // were emitted purely for presentation (color, etc). The signature view
    // is the same argv with those indices erased.
    static CompilerCommand finalize(std::vector<std::string> cmd,
                                    const std::vector<size_t>& cosmetic_indices) {
        std::vector<std::string> sig;
        sig.reserve(cmd.size() - cosmetic_indices.size());
        size_t next_skip = 0;
        for (size_t i = 0; i < cmd.size(); ++i) {
            if (next_skip < cosmetic_indices.size() && cosmetic_indices[next_skip] == i) {
                ++next_skip;
                continue;
            }
            sig.push_back(cmd[i]);
        }
        return {std::move(cmd), std::move(sig)};
    }

    CompilerCommand get_compile_command(const CompileContext& ctx) const override {
        std::vector<std::string> cmd;
        std::vector<size_t> cosmetic;
        cmd.push_back(binary_);
        inject_target_flags(cmd);

        if (!ctx.standard.empty() && lang_ != Language::ASM) {
            std::string std_prefix = (lang_ == Language::C ? "c" : "c++");
            // Use "gnu" prefix if extensions are enabled (gnu11 vs c11)
            if (ctx.extensions_enabled) {
                std_prefix = "gnu" + std_prefix.substr(1);  // "c" -> "gnu", "c++" -> "gnu++"
            }
            cmd.push_back("-std=" + std_prefix + ctx.standard);
        }

        if (ctx.color_diagnostics) {
            cosmetic.push_back(cmd.size());
            cmd.push_back("-fdiagnostics-color=always");
        }

        // C++20 modules support
        if (ctx.is_module_source) {
            cmd.push_back("-fmodules-ts");

            // Module mapper file for resolving import declarations
            if (!ctx.module_mapper_file.empty()) {
                cmd.push_back("-fmodule-mapper=" + ctx.module_mapper_file);
            }

            // Explicit module file mappings
            for (const auto& mf : ctx.module_files) {
                cmd.push_back("-fmodule-file=" + mf);
            }
        }

        // gcc-14 doesn't recognize .cppm/.ccm/.cxxm/.ixx/.mpp as C++ source; without
        // -x c++ it silently treats them as linker inputs, "compiles" successfully,
        // and produces no BMI. Force C++ mode for module-interface extensions.
        std::string_view ext = Path(ctx.source).extension();
        if (ext == ".cppm" || ext == ".ccm" || ext == ".cxxm" || ext == ".ixx" || ext == ".mpp") {
            cmd.push_back("-x");
            cmd.push_back("c++");
        }

        for (const auto& opt : ctx.options) {
            if (opt.empty()) continue;
            // Handle CMake's SHELL: prefix - strips prefix and passes arguments as-is
            if (opt.starts_with("SHELL:")) {
                // Split the rest by whitespace and add as separate arguments
                std::string rest = opt.substr(6);  // Skip "SHELL:"
                std::stringstream ss(rest);
                std::string arg;
                while (ss >> arg) {
                    cmd.push_back(arg);
                }
            } else {
                cmd.push_back(opt);
            }
        }
        for (const auto& def : ctx.definitions) {
            // Strip -D prefix if present (some CMakeLists.txt files include it)
            std::string clean_def = def;
            if (clean_def.starts_with("-D")) {
                clean_def = clean_def.substr(2);
            }
            if (clean_def.empty()) continue;
            // No shell escaping needed: commands are executed via execvp,
            // so each argv element is passed directly to the compiler.
            cmd.push_back("-D" + clean_def);
        }

        cmd.push_back("-MMD");
        cmd.push_back("-MF");
        cmd.push_back(ctx.output + ".d");

        if (ctx.is_shared) cmd.push_back("-fPIC");
        else if (ctx.is_pie) cmd.push_back("-fPIE");
        if (!ctx.visibility_preset.empty()) cmd.push_back("-fvisibility=" + ctx.visibility_preset);
        if (ctx.visibility_inlines_hidden) cmd.push_back("-fvisibility-inlines-hidden");

        cmd.push_back("-c");
        cmd.push_back("-o");
        cmd.push_back(ctx.output);

        for (const auto& dir : ctx.includes) {
            cmd.push_back("-I" + dir);
        }
        for (const auto& dir : ctx.system_includes) {
            cmd.push_back("-isystem" + dir);
        }

        if (!ctx.pch_include.empty()) {
            cmd.push_back("-Winvalid-pch");
            // Split pch_include if it contains multiple arguments (e.g., "-include wrapper.hpp")
            std::stringstream ss(ctx.pch_include);
            std::string arg;
            while (ss >> arg) cmd.push_back(arg);
        }

        cmd.push_back(ctx.source);

        for (const auto& arg : cmd) assert_no_genex(arg, "compile command");
        return finalize(std::move(cmd), cosmetic);
    }

    CompilerCommand get_link_command(const LinkContext& ctx) const override {
        std::vector<std::string> cmd;
        std::vector<size_t> cosmetic;
        cmd.push_back(binary_);
        inject_target_flags(cmd);

        if (!ctx.standard.empty()) {
            std::string std_prefix = (lang_ == Language::C ? "c" : "c++");
            // Use "gnu" prefix if extensions are enabled (gnu11 vs c11)
            if (ctx.extensions_enabled) {
                std_prefix = "gnu" + std_prefix.substr(1);  // "c" -> "gnu", "c++" -> "gnu++"
            }
            cmd.push_back("-std=" + std_prefix + ctx.standard);
        }

        if (ctx.color_diagnostics) {
            cosmetic.push_back(cmd.size());
            cmd.push_back("-fdiagnostics-color=always");
        }

        if (ctx.is_pie && !ctx.is_shared) {
            // PIE executables need -pie at link time so the loader maps the
            // image at a random base address and references to symbols in
            // shared libs go through the GOT/PLT instead of becoming
            // non-preemptible (which would error under -Wl,-z,defs).
            cmd.push_back("-pie");
        }

        // Expand a single linker-flag entry (handles SHELL:/LINKER: prefixes).
        // Returns true if the entry was a recognized prefix and was expanded.
        auto expand_linker_prefix = [&](const std::string& flag) -> bool {
            if (flag.starts_with("SHELL:")) {
                std::string rest = flag.substr(6);
                std::stringstream ss(rest);
                std::string arg;
                while (ss >> arg) cmd.push_back(arg);
                return true;
            }
            if (flag.starts_with("LINKER:")) {
                // CMake LINKER: prefix - split on commas, each piece gets -Wl,
                // e.g. LINKER:--version-script,/path/to/map -> -Wl,--version-script -Wl,/path/to/map
                // e.g. LINKER:-Bsymbolic-functions -> -Wl,-Bsymbolic-functions
                std::string_view sv(flag);
                sv.remove_prefix(7);
                size_t pos = 0;
                while (pos < sv.size()) {
                    auto comma = sv.find(',', pos);
                    if (comma == std::string_view::npos) comma = sv.size();
                    cmd.push_back("-Wl," + std::string(sv.substr(pos, comma - pos)));
                    pos = comma + 1;
                }
                return true;
            }
            return false;
        };

        for (const auto& flag : ctx.linker_flags) {
            if (!expand_linker_prefix(flag)) cmd.push_back(flag);
        }

        cmd.push_back("-Wl,-rpath,'$ORIGIN'");

        if (!ctx.skip_build_rpath)
        // Embed each non-system link directory as RUNPATH so that:
        //   1. shared libs we produce can find their NEEDED entries at runtime
        //   2. ld can resolve transitive symbol closure when downstream targets
        //      link against this output (it follows the .so's RUNPATH to find
        //      indirect deps with soname-only NEEDED entries).
        // Mirrors CMake's default CMAKE_INSTALL_RPATH_USE_LINK_PATH=ON for the
        // build tree. System dirs are skipped — embedding them is pointless
        // and clutters RUNPATH.
        {
            // Canonicalize implicit link dirs once so prefix-stripped/symlinked
            // forms (e.g. /usr/lib vs /usr/lib/../lib) compare equal.
            std::unordered_set<std::string> system_dirs;
            for (const auto& d : ctx.implicit_link_dirs) {
                std::error_code ec;
                auto canon = std::filesystem::weakly_canonical(d, ec);
                system_dirs.insert(ec ? d : canon.string());
            }
            std::unordered_set<std::string> seen;
            auto add_rpath = [&](std::string dir) {
                if (dir.empty() || dir[0] != '/') return;
                std::error_code ec;
                auto canon = std::filesystem::weakly_canonical(dir, ec);
                std::string key = ec ? dir : canon.string();
                if (system_dirs.count(key)) return;
                if (!seen.insert(key).second) return;
                cmd.push_back("-Wl,-rpath," + dir);
            };
            // Explicit -L dirs.
            for (const auto& dir : ctx.lib_dirs) add_rpath(dir);
            // Absolute-path shared libraries: their directory must be on
            // RUNPATH so we (and downstream linkers) can find their indirect
            // NEEDED entries.
            for (const auto& obj : ctx.objects) {
                if (obj.empty() || obj[0] != '/') continue;
                if (obj.find(".so") == std::string::npos) continue;
                auto slash = obj.find_last_of('/');
                if (slash == std::string::npos) continue;
                add_rpath(obj.substr(0, slash));
            }
        }

        if (ctx.is_shared) cmd.push_back("-shared");
        cmd.push_back("-o");
        cmd.push_back(ctx.output);

        // Partition ctx.objects into: .o files, static libs (.a), shared libs (.so).
        // GNU ld processes left-to-right: static libs only pull in objects for
        // currently unresolved symbols, so shared libs that satisfy static lib
        // references must come AFTER them.
        std::vector<std::string> obj_files, static_libs, shared_libs;
        for (const auto& obj : ctx.objects) {
            if (obj.ends_with(".a"))
                static_libs.push_back(obj);
            else if (obj.find(".so") != std::string::npos)
                shared_libs.push_back(obj);
            else
                obj_files.push_back(obj);
        }

        for (const auto& o : obj_files) cmd.push_back(o);
        // Wrap all libraries in --start-group/--end-group so the linker
        // rescans and resolves circular dependencies between archives
        // and shared libs that reference each other's symbols.
        if (!static_libs.empty() || !shared_libs.empty()) {
            cmd.push_back("-Wl,--start-group");
            for (const auto& a : static_libs) cmd.push_back(a);
            for (const auto& so : shared_libs) cmd.push_back(so);
            cmd.push_back("-Wl,--end-group");
        }

        for (const auto& dir : ctx.lib_dirs) cmd.push_back("-L" + dir);
        for (const auto& lib : ctx.libs) {
            // CMake permits LINKER:/SHELL: entries inside target_link_libraries()
            // — used to wrap a group of libs with linker state changes
            // (e.g. LINKER:--push-state,--no-as-needed lib LINKER:--pop-state).
            // Order matters, so expand inline at the position the entry appeared.
            if (expand_linker_prefix(lib)) continue;
            // Handle different library formats:
            // - Flags like "-pthread" -> pass through as-is
            // - Already prefixed "-l..." -> pass through as-is
            // - Paths containing "/" -> pass through as-is
            // - Plain library names -> add "-l" prefix
            if (lib.starts_with("-") || lib.find('/') != std::string::npos) {
                cmd.push_back(lib);
            } else {
                cmd.push_back("-l" + lib);
            }
        }

        for (const auto& arg : cmd) assert_no_genex(arg, "link command");
        return finalize(std::move(cmd), cosmetic);
    }

    // Parse linker side-effect outputs out of an already-built link argv.
    // Recognized flags:
    //   -Map=PATH                 (BFD/gold/lld: link map)
    //   -Wl,-Map=PATH | -Wl,-Map,PATH
    //   -Xlinker -Map=PATH | -Xlinker -Map -Xlinker PATH
    //   -Map PATH                 (bare, when invoking ld directly)
    //   -Wl,--out-implib=PATH | --out-implib=PATH | --out-implib PATH
    //                             (MinGW import library for shared DLLs)
    std::vector<std::string> get_link_side_effect_outputs(
        const std::vector<std::string>& argv) const override {
        std::vector<std::string> outputs;

        auto strip_quotes = [](std::string s) {
            if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') && s.front() == s.back())
                s = s.substr(1, s.size() - 2);
            return s;
        };
        auto match_value_flag = [&](std::string_view tok, std::string_view name) -> std::optional<std::string> {
            // name like "-Map" or "--out-implib": match name= or returns nullopt.
            if (tok.size() > name.size() && tok.starts_with(name) && tok[name.size()] == '=')
                return std::string(tok.substr(name.size() + 1));
            return std::nullopt;
        };

        for (size_t i = 0; i < argv.size(); ++i) {
            std::string_view a(argv[i]);

            // -Wl,foo,bar — split on commas and recurse logically.
            if (a.starts_with("-Wl,")) {
                std::string_view rest = a.substr(4);
                std::vector<std::string_view> parts;
                while (!rest.empty()) {
                    auto comma = rest.find(',');
                    parts.push_back(rest.substr(0, comma));
                    if (comma == std::string_view::npos) break;
                    rest = rest.substr(comma + 1);
                }
                for (size_t j = 0; j < parts.size(); ++j) {
                    auto p = parts[j];
                    if (auto v = match_value_flag(p, "-Map")) { outputs.push_back(strip_quotes(*v)); continue; }
                    if (auto v = match_value_flag(p, "--out-implib")) { outputs.push_back(strip_quotes(*v)); continue; }
                    if (p == "-Map" && j + 1 < parts.size()) { outputs.push_back(strip_quotes(std::string(parts[++j]))); continue; }
                    if (p == "--out-implib" && j + 1 < parts.size()) { outputs.push_back(strip_quotes(std::string(parts[++j]))); continue; }
                }
                continue;
            }

            if (auto v = match_value_flag(a, "-Map")) { outputs.push_back(strip_quotes(*v)); continue; }
            if (auto v = match_value_flag(a, "--out-implib")) { outputs.push_back(strip_quotes(*v)); continue; }

            if (a == "-Xlinker" && i + 1 < argv.size()) {
                std::string_view nxt(argv[i + 1]);
                if (auto v = match_value_flag(nxt, "-Map")) { outputs.push_back(strip_quotes(*v)); ++i; continue; }
                if (auto v = match_value_flag(nxt, "--out-implib")) { outputs.push_back(strip_quotes(*v)); ++i; continue; }
                if ((nxt == "-Map" || nxt == "--out-implib") && i + 3 < argv.size() && argv[i + 2] == "-Xlinker") {
                    outputs.push_back(strip_quotes(argv[i + 3]));
                    i += 3;
                    continue;
                }
            }

            if ((a == "-Map" || a == "--out-implib") && i + 1 < argv.size()) {
                outputs.push_back(strip_quotes(argv[i + 1]));
                ++i;
                continue;
            }
        }
        return outputs;
    }

    std::vector<std::string> get_archive_command(const std::string& output, const std::vector<std::string>& objs) const override {
        std::vector<std::string> cmd;
        cmd.push_back("ar");
        cmd.push_back("rcs");
        cmd.push_back(output);
        for (const auto& obj : objs) cmd.push_back(obj);
        return cmd;
    }

    // C++20 modules: generate scan command for extracting module dependencies
    // Uses preprocessor-only mode to quickly extract import/export declarations
    CompilerCommand get_module_scan_command(const ModuleScanContext& ctx) const override {
        std::vector<std::string> cmd;
        std::vector<size_t> cosmetic;
        cmd.push_back(binary_);
        inject_target_flags(cmd);

        // Set C++ standard (must be C++20 or later for modules)
        if (!ctx.standard.empty()) {
            cmd.push_back("-std=c++" + ctx.standard);
        } else {
            cmd.push_back("-std=c++20");  // Default to C++20 for modules
        }

        // Enable modules TS
        cmd.push_back("-fmodules-ts");

        // Preprocessor-only mode with directives
        cmd.push_back("-E");
        cmd.push_back("-fdirectives-only");

        // Force C++ mode for module interface files
        cmd.push_back("-x");
        cmd.push_back("c++");

        if (ctx.color_diagnostics) {
            cosmetic.push_back(cmd.size());
            cmd.push_back("-fdiagnostics-color=always");
        }

        // Include directories
        for (const auto& dir : ctx.includes) {
            cmd.push_back("-I" + dir);
        }
        for (const auto& dir : ctx.system_includes) {
            cmd.push_back("-isystem" + dir);
        }

        // Definitions
        for (const auto& def : ctx.definitions) {
            // Strip -D prefix if present (some CMakeLists.txt files include it)
            std::string clean_def = def;
            if (clean_def.starts_with("-D")) {
                clean_def = clean_def.substr(2);
            }
            if (clean_def.empty()) continue;
            // Escape quotes in definition values so they reach the preprocessor
            std::string escaped_def;
            escaped_def.reserve(clean_def.size() + 4);
            for (char c : clean_def) {
                if (c == '"') {
                    escaped_def += "\\\"";
                } else {
                    escaped_def += c;
                }
            }
            cmd.push_back("-D" + escaped_def);
        }

        // Source file
        cmd.push_back(ctx.source);

        return finalize(std::move(cmd), cosmetic);
    }

    // Platform detection
    // Runs subprocess calls in parallel.
    // Identifies GCC vs Clang vs IntelLLVM via predefined macros (-dM -E).
    // When sysroot_/compiler_target_ are set, all probes inherit them so
    // implicit include/link dirs reflect the target environment, not the host.
    PlatformInfo detect_platform() const override {
        PlatformInfo info;
        info.sizeof_void_p = std::to_string(sizeof(void*));

        const std::string flags = build_target_flag_string();
        const std::string lang_flag = (lang_ == Language::C || lang_ == Language::ASM) ? "c" : "c++";

        auto version_future = std::async(std::launch::async, [&] {
            return detail::run_command(binary_ + " --version 2>&1");
        });
        auto verbose_future = std::async(std::launch::async, [&] {
            return detail::run_command("echo | " + binary_ + flags + " -E -v -x " + lang_flag + " - 2>&1");
        });
        auto search_dirs_future = std::async(std::launch::async, [&] {
            return detail::run_command(binary_ + flags + " -print-search-dirs 2>&1");
        });
        auto macros_future = std::async(std::launch::async, [&] {
            return detail::run_command(binary_ + flags + " -dM -E -x " + lang_flag + " /dev/null 2>/dev/null");
        });

        // Probe-TU compile + binary scan, mirroring CMake's CompilerId flow.
        // Runs in parallel; consumed only as a fallback when the macro probe
        // can't identify the compiler (or as a sanity check otherwise). The
        // probe.c TU below resolves to a tiny rodata string of the form
        // "INFO:compiler[X]" / "INFO:version[Y]" / "INFO:arch[Z]" via
        // preprocessor macros, which we then strings-grep out of the produced
        // object. Robust against compilers that don't accept `-dM -E`.
        auto probe_tu_future = std::async(std::launch::async, [&]() -> std::map<std::string, std::string> {
            return run_compiler_id_probe(binary_, flags, lang_flag);
        });

        // Parse predefined macros — single source of truth for compiler ID,
        // version, default standard, and (in cross builds) system info.
        std::unordered_map<std::string, std::string> macros;
        {
            std::istringstream iss(macros_future.get());
            std::string line;
            while (std::getline(iss, line)) {
                // "#define NAME VALUE" — split on the second space
                if (line.rfind("#define ", 0) != 0) continue;
                auto sp = line.find(' ', 8);
                if (sp == std::string::npos) {
                    macros.emplace(line.substr(8), std::string{});
                } else {
                    macros.emplace(line.substr(8, sp - 8), line.substr(sp + 1));
                }
            }
        }
        auto macro = [&](const std::string& name) -> std::string {
            auto it = macros.find(name);
            return it == macros.end() ? std::string{} : it->second;
        };
        auto has_macro = [&](const std::string& name) {
            return macros.find(name) != macros.end();
        };

        // Compiler ID + version (priority: IntelLLVM > Clang > GNU > Unknown)
        if (has_macro("__INTEL_LLVM_COMPILER")) {
            info.compiler_id = "IntelLLVM";
            info.compiler_version = macro("__INTEL_LLVM_COMPILER");
        } else if (has_macro("__clang__")) {
            info.compiler_id = "Clang";
            std::string maj = macro("__clang_major__");
            std::string min = macro("__clang_minor__");
            std::string pat = macro("__clang_patchlevel__");
            if (!maj.empty()) {
                info.compiler_version = maj + "." + (min.empty() ? "0" : min)
                                              + "." + (pat.empty() ? "0" : pat);
            }
        } else if (has_macro("__GNUC__")) {
            info.compiler_id = "GNU";
            std::string maj = macro("__GNUC__");
            std::string min = macro("__GNUC_MINOR__");
            std::string pat = macro("__GNUC_PATCHLEVEL__");
            if (!maj.empty()) {
                info.compiler_version = maj + "." + (min.empty() ? "0" : min)
                                              + "." + (pat.empty() ? "0" : pat);
            }
        } else {
            info.compiler_id = "Unknown";
        }

        // Probe-TU result: authoritative for compilers where -dM -E doesn't
        // give us a clean answer (compiler_id == Unknown). For known
        // compilers we use it as a sanity check — disagreement here usually
        // means a wrapper that lies on -dM -E but actually invokes a
        // different compiler underneath.
        const auto probe = probe_tu_future.get();
        auto get_probe = [&](const std::string& key) -> std::string {
            auto it = probe.find(key);
            return it == probe.end() ? std::string{} : it->second;
        };
        const std::string probe_compiler = get_probe("compiler");
        const std::string probe_version  = get_probe("version");
        if (info.compiler_id == "Unknown" && !probe_compiler.empty()) {
            info.compiler_id = probe_compiler;
            if (!probe_version.empty() && probe_version != "unknown") {
                info.compiler_version = probe_version;
            }
        }
        if (info.compiler_version.empty() && !probe_version.empty() && probe_version != "unknown") {
            info.compiler_version = probe_version;
        }

        // Fallback for compiler_version: parse --version banner if macro path failed
        if (info.compiler_version.empty()) {
            std::string version_output = version_future.get();
            static auto version_re = Regex::compile(R"((\d+\.\d+\.\d+))").value();
            std::vector<std::string> captures;
            if (version_re.search(version_output, captures)) {
                info.compiler_version = captures[1];
            }
        } else {
            (void)version_future.get();  // drain
        }

        // Default standard — parse __cplusplus / __STDC_VERSION__
        {
            std::string val = (lang_ == Language::CXX) ? macro("__cplusplus") : macro("__STDC_VERSION__");
            if (!val.empty() && val.back() == 'L') val.pop_back();
            if (auto v_opt = parse_number<long>(val); v_opt) {
                long v = *v_opt;
                int std_val = 0;
                if (lang_ == Language::CXX) {
                    if (v >= 202602L)      std_val = 26;
                    else if (v >= 202302L) std_val = 23;
                    else if (v >= 202002L) std_val = 20;
                    else if (v >= 201703L) std_val = 17;
                    else if (v >= 201402L) std_val = 14;
                    else if (v >= 201103L) std_val = 11;
                    else                   std_val = 98;
                    info.default_cxx_standard = std_val;
                } else {
                    if (v >= 202311L)      std_val = 23;
                    else if (v >= 201710L) std_val = 17;
                    else if (v >= 201112L) std_val = 11;
                    else if (v >= 199901L) std_val = 99;
                    else                   std_val = 90;
                    info.default_c_standard = std_val;
                }
            }
        }

        // System info: derive from target macros when cross-compiling
        // (sysroot or --target is in play); otherwise fall back to host uname.
        const bool cross = !sysroot_.empty() || !compiler_target_.empty();
        if (cross) {
            // OS
            if (has_macro("__linux__"))      info.system_name = "Linux";
            else if (has_macro("__APPLE__")) info.system_name = "Darwin";
            else if (has_macro("_WIN32"))    info.system_name = "Windows";
            else if (has_macro("__FreeBSD__")) info.system_name = "FreeBSD";
            else                             info.system_name = "Generic";

            // Processor
            if (has_macro("__riscv")) {
                std::string xlen = macro("__riscv_xlen");
                info.system_processor = (xlen == "64") ? "riscv64" : "riscv32";
            } else if (has_macro("__x86_64__"))  info.system_processor = "x86_64";
            else if (has_macro("__i386__"))      info.system_processor = "i386";
            else if (has_macro("__aarch64__"))   info.system_processor = "aarch64";
            else if (has_macro("__arm__"))       info.system_processor = "arm";
            else if (has_macro("__powerpc64__")) info.system_processor = "ppc64";
            else if (has_macro("__powerpc__"))   info.system_processor = "ppc";
            else                                 info.system_processor = "Unknown";

            // sizeof_void_p from __SIZEOF_POINTER__ if present (more correct than host's sizeof)
            std::string sp = macro("__SIZEOF_POINTER__");
            if (!sp.empty()) info.sizeof_void_p = sp;

            // Probe-TU fallbacks: if -dM -E didn't give us arch / ptrsize,
            // pick them up from the compiled probe object (still authoritative
            // for the *target* even if the macro probe choked).
            if (info.system_processor == "Unknown") {
                std::string a = get_probe("arch");
                if (!a.empty() && a != "unknown") info.system_processor = a;
            }
            if (info.sizeof_void_p.empty() || info.sizeof_void_p == "0") {
                std::string ps = get_probe("ptrsize");
                if (!ps.empty() && ps != "unknown") info.sizeof_void_p = ps;
            }
        } else {
#ifdef __unix__
            struct utsname uname_info;
            if (uname(&uname_info) == 0) {
                info.system_name = uname_info.sysname;
                info.system_processor = uname_info.machine;
            }
#endif
        }

        // Implicit link libraries. For GCC libstdc++ is the canonical set.
        // Clang-libc++ users pass -stdlib=libc++ via flags so we still emit
        // these; bare-metal targets typically override via toolchain file.
        if (info.compiler_id == "Clang" && cross) {
            // Bare/embedded cross usually doesn't link these implicitly;
            // leave empty and let the toolchain file / target deps decide.
        } else {
            info.implicit_link_libs = {"stdc++", "m", "gcc_s", "c"};
        }

        // Parse implicit includes from -E -v output
        std::string verbose_output = verbose_future.get();
        bool in_include_section = false;
        std::istringstream iss(verbose_output);
        std::string line;
        while (std::getline(iss, line)) {
            if (line.find("#include <...> search starts here:") != std::string::npos) {
                in_include_section = true;
                continue;
            }
            if (line.find("End of search list.") != std::string::npos) {
                in_include_section = false;
                continue;
            }
            if (in_include_section && !line.empty() && line[0] == ' ') {
                std::string dir = line.substr(1);
                auto paren = dir.find(" (");
                if (paren != std::string::npos) {
                    dir = dir.substr(0, paren);
                }
                if (!dir.empty()) {
                    info.implicit_includes.push_back(dir);
                }
            }
        }

        // Parse implicit link dirs from -print-search-dirs
        std::string search_dirs = search_dirs_future.get();
        std::istringstream search_iss(search_dirs);
        while (std::getline(search_iss, line)) {
            if (line.rfind("libraries: =", 0) == 0) {
                std::string paths = line.substr(12);
                std::istringstream path_stream(paths);
                std::string path;
                while (std::getline(path_stream, path, ':')) {
                    if (!path.empty()) {
                        info.implicit_link_dirs.push_back(path);
                    }
                }
                break;
            }
        }

        return info;
    }

private:
    // Run CMake-style CompilerId probe: write a tiny TU whose rodata strings
    // encode the compiler ID, version, architecture, and pointer size via
    // preprocessor macros, compile it with the binary under test, then scan
    // the resulting object for the embedded "INFO:" markers. Returns a map
    // of marker -> value (e.g. {"compiler", "GNU"}, {"version", "13.2.0"}).
    // Empty map on any failure — caller must treat that as "no signal".
    static std::map<std::string, std::string> run_compiler_id_probe(
            const std::string& binary, const std::string& flags, const std::string& lang_flag) {
        std::map<std::string, std::string> out;

        // Probe TU. The macro mash assembles version components into a
        // string literal so we don't have to encode digits with DEC()/UNIT()
        // tricks — every compiler we care about defines __VERSION__ as a
        // string, and __GNUC__/__clang_major__/__INTEL_LLVM_COMPILER are
        // available as integers we can dispatch on textually. A canonical
        // version comes from the major.minor.patch macros where present.
        static constexpr const char* probe_tu = R"PROBE(
#if defined(__INTEL_LLVM_COMPILER)
const char info_compiler[] = "INFO:compiler[IntelLLVM]";
#elif defined(__clang__)
const char info_compiler[] = "INFO:compiler[Clang]";
#elif defined(__GNUC__)
const char info_compiler[] = "INFO:compiler[GNU]";
#else
const char info_compiler[] = "INFO:compiler[Unknown]";
#endif

#define INFO_STR(x) INFO_STR_(x)
#define INFO_STR_(x) #x
#if defined(__clang_major__) && defined(__clang_minor__) && defined(__clang_patchlevel__)
const char info_version[] = "INFO:version["
    INFO_STR(__clang_major__) "." INFO_STR(__clang_minor__) "." INFO_STR(__clang_patchlevel__) "]";
#elif defined(__GNUC__) && defined(__GNUC_MINOR__) && defined(__GNUC_PATCHLEVEL__)
const char info_version[] = "INFO:version["
    INFO_STR(__GNUC__) "." INFO_STR(__GNUC_MINOR__) "." INFO_STR(__GNUC_PATCHLEVEL__) "]";
#elif defined(__INTEL_LLVM_COMPILER)
const char info_version[] = "INFO:version[" INFO_STR(__INTEL_LLVM_COMPILER) "]";
#else
const char info_version[] = "INFO:version[unknown]";
#endif

#if defined(__riscv)
#  if __riscv_xlen == 64
const char info_arch[] = "INFO:arch[riscv64]";
#  else
const char info_arch[] = "INFO:arch[riscv32]";
#  endif
#elif defined(__x86_64__) || defined(_M_X64)
const char info_arch[] = "INFO:arch[x86_64]";
#elif defined(__i386__) || defined(_M_IX86)
const char info_arch[] = "INFO:arch[i386]";
#elif defined(__aarch64__) || defined(_M_ARM64)
const char info_arch[] = "INFO:arch[aarch64]";
#elif defined(__arm__) || defined(_M_ARM)
const char info_arch[] = "INFO:arch[arm]";
#elif defined(__powerpc64__)
const char info_arch[] = "INFO:arch[ppc64]";
#elif defined(__powerpc__)
const char info_arch[] = "INFO:arch[ppc]";
#else
const char info_arch[] = "INFO:arch[unknown]";
#endif

#if defined(__SIZEOF_POINTER__)
const char info_ptrsize[] = "INFO:ptrsize[" INFO_STR(__SIZEOF_POINTER__) "]";
#else
const char info_ptrsize[] = "INFO:ptrsize[unknown]";
#endif

int kiln_compiler_id_probe_anchor = 0;
)PROBE";

        // Stage TU and output in a per-binary temp dir under /tmp so concurrent
        // probes for different compilers don't collide.
        std::error_code ec;
        std::string fingerprint = binary + flags;
        // Hash to a short stable tag without pulling in blake2b here.
        std::size_t h = std::hash<std::string>{}(fingerprint);
        auto tmp = std::filesystem::temp_directory_path(ec) / ("kiln-cid-" + std::to_string(h));
        if (ec) return out;
        std::filesystem::create_directories(tmp, ec);
        if (ec) return out;

        // Pick a source extension that matches the language so the compiler
        // doesn't need an explicit -x.
        const std::string src_ext = (lang_flag == "c++") ? ".cpp" : ".c";
        auto src_path = tmp / ("probe" + src_ext);
        auto obj_path = tmp / "probe.o";
        {
            std::ofstream ofs(src_path, std::ios::binary);
            if (!ofs) return out;
            ofs << probe_tu;
        }

        // Compile (-c). Suppress warnings, no optimizations, no preprocessor
        // checks beyond compiling. Stay quiet on stdout/stderr.
        std::string cmd = binary + flags + " -c -w -O0 -o '"
                        + obj_path.string() + "' '" + src_path.string() + "' >/dev/null 2>&1";
        int rc = std::system(cmd.c_str());
        if (rc != 0) {
            std::filesystem::remove_all(tmp, ec);
            return out;
        }

        // Read the produced object and scan for INFO: markers.
        std::ifstream ifs(obj_path, std::ios::binary);
        if (!ifs) {
            std::filesystem::remove_all(tmp, ec);
            return out;
        }
        std::string buf((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        std::filesystem::remove_all(tmp, ec);

        // Find every "INFO:<key>[<value>]" run. The TU defines markers for
        // compiler, version, arch, ptrsize.
        std::string_view sv(buf);
        size_t pos = 0;
        while ((pos = sv.find("INFO:", pos)) != std::string::npos) {
            size_t key_start = pos + 5;
            size_t bracket = sv.find('[', key_start);
            if (bracket == std::string::npos) break;
            size_t end = sv.find(']', bracket);
            if (end == std::string::npos) break;
            std::string key(sv.substr(key_start, bracket - key_start));
            std::string val(sv.substr(bracket + 1, end - bracket - 1));
            // First occurrence wins (the strings are emitted once each but
            // some object formats duplicate rodata in debug sections).
            out.try_emplace(std::move(key), std::move(val));
            pos = end + 1;
        }
        return out;
    }

    // Build a leading flag string (with leading space) suitable for splicing
    // into popen-style command lines. Empty string when neither sysroot nor
    // target is configured.
    std::string build_target_flag_string() const {
        std::string s;
        if (!sysroot_.empty()) {
            s += " --sysroot=";
            s += sysroot_;
        }
        if (!compiler_target_.empty()) {
            s += " --target=";
            s += compiler_target_;
        }
        return s;
    }

private:
    std::string binary_;
    Language lang_;
    std::string sysroot_;
    std::string compiler_target_;
};

} // namespace kiln
