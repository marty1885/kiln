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
#include <mutex>
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

    // GCC-style `-std=c++NN` / `-std=cNN`. ClangCompiler and TccCompiler
    // inherit this — same spelling for all three. MSVC overrides.
    std::string std_compile_option(Language lang, int standard) const override {
        if (lang == Language::CXX) {
            return "-std=c++" + std::to_string(standard);
        }
        if (lang == Language::C) {
            return "-std=c" + std::to_string(standard);
        }
        return {};
    }

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

        if (ctx.color_diagnostics) emit_color_flag(cmd, cosmetic);

        // C++20 modules support. Subclasses override emit_module_compile_flags
        // to swap in their own indirection scheme (clang reads per-task
        // response files instead of GCC's -fmodule-mapper=).
        if (ctx.is_module_source) {
            emit_module_compile_flags(cmd, ctx);
        }

        // gcc-14 doesn't recognize .cppm/.ccm/.cxxm/.ixx/.mpp as C++ source; without
        // -x c++ it silently treats them as linker inputs, "compiles" successfully,
        // and produces no BMI. Force C++ mode for module-interface extensions.
        // Bind the Path to a named local; .extension() returns a string_view
        // into the Path's internal storage, so a temporary here would dangle
        // before the comparisons below run.
        Path src_path(ctx.source);
        std::string_view ext = src_path.extension();
        emit_module_input_kind_flags(cmd, ext, ctx);

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

        emit_dependency_flags(cmd, ctx.output);

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

        if (ctx.color_diagnostics) emit_color_flag(cmd, cosmetic);

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
            if (dir.empty()) return;
            // User-provided rpaths may be relative or use $ORIGIN — keep verbatim
            // and skip the system-dir filter (which only makes sense for
            // absolute paths we derived from -L).
            if (dir[0] != '/') {
                if (!seen.insert(dir).second) return;
                cmd.push_back("-Wl,-rpath," + dir);
                return;
            }
            std::error_code ec;
            auto canon = std::filesystem::weakly_canonical(dir, ec);
            std::string key = ec ? dir : canon.string();
            if (system_dirs.count(key)) return;
            if (!seen.insert(key).second) return;
            cmd.push_back("-Wl,-rpath," + dir);
        };

        if (ctx.build_with_install_rpath) {
            // Bake the install-time RPATH into the build binary directly;
            // skip auto-derived link-dir entries to mirror CMake.
            for (const auto& dir : ctx.install_rpath) add_rpath(dir);
        } else if (!ctx.skip_build_rpath) {
            // Embed each non-system link directory as RUNPATH so that:
            //   1. shared libs we produce can find their NEEDED entries at runtime
            //   2. ld can resolve transitive symbol closure when downstream targets
            //      link against this output (it follows the .so's RUNPATH to find
            //      indirect deps with soname-only NEEDED entries).
            // Mirrors CMake's default CMAKE_INSTALL_RPATH_USE_LINK_PATH=ON for the
            // build tree. System dirs are skipped — embedding them is pointless
            // and clutters RUNPATH.
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
            // User-supplied BUILD_RPATH entries (target property /
            // CMAKE_BUILD_RPATH variable). Added in addition to the auto ones.
            for (const auto& dir : ctx.build_rpath) add_rpath(dir);
        }

        if (ctx.is_shared && !ctx.soname.empty()) {
            cmd.push_back("-Wl,-soname," + ctx.soname);
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
            emit_link_group_open(cmd);
            for (const auto& a : static_libs) cmd.push_back(a);
            for (const auto& so : shared_libs) cmd.push_back(so);
            emit_link_group_close(cmd);
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

    // C++20 modules: scan a TU and emit a P1689r5 dependency-info JSON.
    // GCC writes the JSON straight to ctx.output via -fdeps-file=. The
    // preprocessor pass is necessary (not -fdirectives-only) because GCC
    // needs to see all imports after macro expansion.
    CompilerCommand get_module_scan_command(const ModuleScanContext& ctx) const override {
        std::vector<std::string> cmd;
        std::vector<size_t> cosmetic;
        cmd.push_back(binary_);
        inject_target_flags(cmd);

        // C++20 or later required for modules.
        if (!ctx.standard.empty()) {
            cmd.push_back("-std=c++" + ctx.standard);
        } else {
            cmd.push_back("-std=c++20");
        }

        cmd.push_back("-fmodules-ts");

        // P1689r5 emission. -fdeps-target must equal the final .o path so
        // the collator can join DDI → obj task by primary-output.
        cmd.push_back("-fdeps-format=p1689r5");
        cmd.push_back("-fdeps-file=" + ctx.output);
        cmd.push_back("-fdeps-target=" + ctx.obj_path);

        // GCC requires -M/-MD when emitting deps. -MD writes a make-style
        // header depfile alongside; we use it for header up-to-date checks.
        cmd.push_back("-MD");
        if (!ctx.depfile.empty()) {
            cmd.push_back("-MF");
            cmd.push_back(ctx.depfile);
        }

        // Preprocess-only; discard the preprocessed output, we only want the DDI.
        cmd.push_back("-E");
        cmd.push_back("-o");
        cmd.push_back("/dev/null");

        // Force C++ mode (gcc doesn't recognize .cppm/.ixx/etc.).
        cmd.push_back("-x");
        cmd.push_back("c++");

        if (ctx.color_diagnostics) emit_color_flag(cmd, cosmetic);

        for (const auto& dir : ctx.includes) {
            cmd.push_back("-I" + dir);
        }
        for (const auto& dir : ctx.system_includes) {
            cmd.push_back("-isystem" + dir);
        }

        for (const auto& def : ctx.definitions) {
            std::string clean_def = def;
            if (clean_def.starts_with("-D")) {
                clean_def = clean_def.substr(2);
            }
            if (clean_def.empty()) continue;
            std::string escaped_def;
            escaped_def.reserve(clean_def.size() + 4);
            for (char c : clean_def) {
                if (c == '"') escaped_def += "\\\"";
                else          escaped_def += c;
            }
            cmd.push_back("-D" + escaped_def);
        }

        cmd.push_back(ctx.source);

        return finalize(std::move(cmd), cosmetic);
    }

    // C++20 header-unit compilation. Builds a BMI from a header file in
    // -fmodule-header mode. The BMI lands at the path advertised by the
    // mapper for this header's resolved source path; the .o is suppressed
    // (header units have no compile-side object). is_system_header selects
    // -fmodule-header=system vs =user, mirroring `import <h>;` vs
    // `import "h";` (and the matching -x c++-{system,user}-header input
    // mode so GCC accepts a bare `.h`/`.hpp` as a header unit).
    CompilerCommand get_header_unit_compile_command(const HeaderUnitContext& ctx) const override {
        std::vector<std::string> cmd;
        std::vector<size_t> cosmetic;
        cmd.push_back(binary_);
        inject_target_flags(cmd);

        if (!ctx.standard.empty()) {
            std::string std_prefix = ctx.extensions_enabled ? "gnu++" : "c++";
            cmd.push_back("-std=" + std_prefix + ctx.standard);
        } else {
            cmd.push_back("-std=c++20");
        }

        cmd.push_back("-fmodules-ts");
        cmd.push_back(std::string("-fmodule-header=") + (ctx.is_system_header ? "system" : "user"));
        if (!ctx.module_mapper_file.empty()) {
            cmd.push_back("-fmodule-mapper=" + ctx.module_mapper_file);
        }

        if (ctx.color_diagnostics) emit_color_flag(cmd, cosmetic);

        for (const auto& opt : ctx.options) {
            if (opt.empty()) continue;
            if (opt.starts_with("SHELL:")) {
                std::string rest = opt.substr(6);
                std::stringstream ss(rest);
                std::string arg;
                while (ss >> arg) cmd.push_back(arg);
            } else {
                cmd.push_back(opt);
            }
        }
        for (const auto& def : ctx.definitions) {
            std::string clean_def = def;
            if (clean_def.starts_with("-D")) clean_def = clean_def.substr(2);
            if (clean_def.empty()) continue;
            cmd.push_back("-D" + clean_def);
        }
        for (const auto& dir : ctx.includes) {
            cmd.push_back("-I" + dir);
        }
        for (const auto& dir : ctx.system_includes) {
            cmd.push_back("-isystem" + dir);
        }

        // -c is required even though no .o is produced; the BMI travels via
        // the mapper. -o /dev/null silences the obj write.
        cmd.push_back("-c");
        cmd.push_back("-o");
        cmd.push_back("/dev/null");

        // Tell GCC how to interpret the input. Without -x, a `.h` file would
        // be treated as a C header.
        cmd.push_back("-x");
        cmd.push_back(ctx.is_system_header ? "c++-system-header" : "c++-user-header");
        cmd.push_back(ctx.source);

        for (const auto& arg : cmd) assert_no_genex(arg, "header unit compile command");
        return finalize(std::move(cmd), cosmetic);
    }

    // Platform detection
    // Runs subprocess calls in parallel.
    // Identifies GCC vs Clang vs Intel (classic / LLVM) via predefined macros (-dM -E).
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

        // Compiler ID + version (priority: IntelLLVM > Clang > classic Intel
        // ICC > GNU > …). Classic ICC defines __GNUC__ for compatibility, so
        // __INTEL_COMPILER must be checked before __GNUC__.
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
        } else if (has_macro("__INTEL_COMPILER")) {
            info.compiler_id = "Intel";
            std::string maj = macro("__INTEL_COMPILER");
            std::string pat = macro("__INTEL_COMPILER_UPDATE");
            info.compiler_version = maj + "." + (pat.empty() ? "0" : pat);
        } else if (has_macro("__GNUC__")) {
            info.compiler_id = "GNU";
            std::string maj = macro("__GNUC__");
            std::string min = macro("__GNUC_MINOR__");
            std::string pat = macro("__GNUC_PATCHLEVEL__");
            if (!maj.empty()) {
                info.compiler_version = maj + "." + (min.empty() ? "0" : min)
                                              + "." + (pat.empty() ? "0" : pat);
            }
        } else if (has_macro("__TINYC__")) {
            // TCC defines __TINYC__ but neither __GNUC__ nor __clang__, so
            // this disambiguates cleanly. The macro encodes version as
            // major*10000 + minor*100 + patch (e.g. 927 -> 0.9.27).
            info.compiler_id = "TCC";
            if (auto v_opt = parse_number<long>(macro("__TINYC__")); v_opt) {
                long v = *v_opt;
                long maj = v / 10000;
                long min = (v / 100) % 100;
                long pat = v % 100;
                info.compiler_version = std::to_string(maj) + "."
                                      + std::to_string(min) + "."
                                      + std::to_string(pat);
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
#elif defined(__INTEL_COMPILER)
const char info_compiler[] = "INFO:compiler[Intel]";
#elif defined(__clang__)
const char info_compiler[] = "INFO:compiler[Clang]";
#elif defined(__GNUC__)
const char info_compiler[] = "INFO:compiler[GNU]";
#elif defined(__TINYC__)
const char info_compiler[] = "INFO:compiler[TCC]";
#else
const char info_compiler[] = "INFO:compiler[Unknown]";
#endif

#define INFO_STR(x) INFO_STR_(x)
#define INFO_STR_(x) #x
#if defined(__clang_major__) && defined(__clang_minor__) && defined(__clang_patchlevel__)
const char info_version[] = "INFO:version["
    INFO_STR(__clang_major__) "." INFO_STR(__clang_minor__) "." INFO_STR(__clang_patchlevel__) "]";
#elif defined(__INTEL_COMPILER)
const char info_version[] = "INFO:version[" INFO_STR(__INTEL_COMPILER) "]";
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

    // -fdeps-format=p1689r5 first landed in GCC 14. We confirm two things:
    //   1. The binary identifies as g++ / gcc (not clang masquerading).
    //   2. Major version ≥ 14.
    // Result is cached per-Compiler-instance; this is called from interp time.
    bool supports_p1689() const override {
        std::call_once(p1689_probe_once_, [this] {
            // dumpfullversion handles cases like "14" by printing "14.0.0".
            std::string ver = detail::run_command(binary_ + " -dumpfullversion -dumpversion 2>/dev/null");
            // Strip trailing whitespace/newline.
            while (!ver.empty() && (ver.back() == '\n' || ver.back() == '\r' || ver.back() == ' '))
                ver.pop_back();
            std::string banner = detail::run_command(binary_ + " --version 2>/dev/null");
            bool is_gcc = banner.find("clang") == std::string::npos &&
                          (banner.find("g++") != std::string::npos ||
                           banner.find("gcc") != std::string::npos ||
                           banner.find("GCC") != std::string::npos);
            int major = 0;
            for (char c : ver) {
                if (c == '.') break;
                if (c >= '0' && c <= '9') major = major * 10 + (c - '0');
                else break;
            }
            p1689_supported_ = is_gcc && major >= 14;
            gcc_major_ = is_gcc ? major : 0;
        });
        return p1689_supported_;
    }

    // GCC 15 was the first release to ship libstdc++.modules.json. We require:
    //   1. supports_p1689() (i.e. GCC ≥14) — really we need ≥15.
    //   2. modules.json reachable via `g++ -print-file-name=`.
    bool supports_import_std() const override {
        (void)supports_p1689();  // populates gcc_major_
        std::call_once(import_std_probe_once_, [this] {
            if (gcc_major_ < 15) { import_std_supported_ = false; return; }
            std::string path = libstdcxx_modules_json_path();
            std::error_code ec;
            import_std_supported_ = !path.empty() && std::filesystem::exists(path, ec);
        });
        return import_std_supported_;
    }

    std::string libstdcxx_modules_json_path() const override {
        std::call_once(modules_json_probe_once_, [this] {
            std::string out = detail::run_command(
                binary_ + " -print-file-name=libstdc++.modules.json 2>/dev/null");
            while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
                out.pop_back();
            if (out.empty() || out == "libstdc++.modules.json") {
                modules_json_path_.clear();
                return;
            }
            std::error_code ec;
            auto canon = std::filesystem::weakly_canonical(out, ec);
            modules_json_path_ = ec ? out : canon.string();
        });
        return modules_json_path_;
    }

protected:
    // Emit the module-related flags into a compile command for a TU that
    // imports/exports modules. GCC default: -fmodules-ts plus a single
    // `-fmodule-mapper=<path>` indirection (mapper is filled in late by the
    // collator). ClangCompiler overrides to emit `@<rsp-path>` instead, since
    // clang has no mapper-flag equivalent.
    virtual void emit_module_compile_flags(std::vector<std::string>& cmd,
                                            const CompileContext& ctx) const {
        cmd.push_back("-fmodules-ts");
        if (!ctx.module_mapper_file.empty()) {
            cmd.push_back("-fmodule-mapper=" + ctx.module_mapper_file);
        }
        for (const auto& mf : ctx.module_files) {
            cmd.push_back("-fmodule-file=" + mf);
        }
    }

    // Coerce a module-interface source (.cppm/.ccm/.cxxm/.ixx/.mpp) into
    // module-interface mode. GCC needs `-x c++` because gcc-14 otherwise
    // treats unknown extensions as linker inputs. Clang recognizes .cppm
    // natively and needs `-x c++-module` to mark it as a module interface
    // (plain `-x c++` would silently downgrade it to a regular TU).
    virtual void emit_module_input_kind_flags(std::vector<std::string>& cmd,
                                                std::string_view ext,
                                                const CompileContext& ctx) const {
        (void)ctx;
        if (ext == ".cppm" || ext == ".ccm" || ext == ".cxxm" || ext == ".ixx" || ext == ".mpp") {
            cmd.push_back("-x");
            cmd.push_back("c++");
        }
    }

    // Hooks for kiln-internal flags whose spelling differs across drivers.
    // These are *not* user-facing CMake properties — kiln chooses to add
    // them — so it's the driver's job to pick the spelling that works for
    // its compiler (or capability-flag itself out). User-facing flags
    // (-std=, -fvisibility=, -fPIC, -pie) are emitted unconditionally and
    // it's the user's problem if a given compiler rejects them.

    // Color diagnostics. Cosmetic; index goes into the cosmetic vector so
    // it's elided from the cache signature.
    virtual void emit_color_flag(std::vector<std::string>& cmd,
                                 std::vector<size_t>& cosmetic) const {
        cosmetic.push_back(cmd.size());
        cmd.push_back("-fdiagnostics-color=always");
    }

    // Make-style depfile emission for header up-to-date tracking. GCC
    // emits "-MMD -MF <out>.d"; TCC rejects -MMD (uses -MD) and rejects
    // -MT outright.
    virtual void emit_dependency_flags(std::vector<std::string>& cmd,
                                       const std::string& output) const {
        cmd.push_back("-MMD");
        cmd.push_back("-MF");
        cmd.push_back(output + ".d");
    }

    // Wrap static/shared lib inputs in a linker group so the linker
    // rescans for circular references. GNU ld and lld both accept
    // --start-group/--end-group; TCC's single-pass linker neither
    // accepts nor needs it.
    virtual void emit_link_group_open(std::vector<std::string>& cmd) const {
        cmd.push_back("-Wl,--start-group");
    }
    virtual void emit_link_group_close(std::vector<std::string>& cmd) const {
        cmd.push_back("-Wl,--end-group");
    }

    std::string binary_;
    Language lang_;
    std::string sysroot_;
    std::string compiler_target_;

protected:
    mutable std::once_flag p1689_probe_once_;
    mutable bool p1689_supported_ = false;
    mutable int gcc_major_ = 0;
    mutable std::once_flag import_std_probe_once_;
    mutable bool import_std_supported_ = false;
    mutable std::once_flag modules_json_probe_once_;
    mutable std::string modules_json_path_;
    mutable bool std_uses_libcxx_ = false;
};

// Clang driver. Inherits the gcc-compatible compile/link/archive emitters
// (clang accepts -std=, -fPIC, -fvisibility=, -MMD, -Wl,...) and the platform
// detector. The C++20 modules path differs from GCC in two ways, both
// overridden below:
//   1. P1689r5 dependency scanning is done by an external clang-scan-deps
//      tool, not an in-driver flag (-fdeps-format=).
//   2. Module bindings are consumed via per-importer response files
//      (`@<obj>.modules.rsp`) instead of a single `-fmodule-mapper=` file.
//      The collator writes those rsp files; argv stays static.
class ClangCompiler : public GnuCompiler {
public:
    using GnuCompiler::GnuCompiler;

    bool supports_p1689() const override {
        std::call_once(scan_deps_probe_once_, [this] {
            scan_deps_path_ = locate_clang_scan_deps();
        });
        return !scan_deps_path_.empty();
    }

    bool uses_per_task_module_rsp() const override { return true; }

    // Clang can consume either libc++.modules.json (clang's own libc++,
    // shipped from clang 18+) or libstdc++.modules.json (libstdc++ from
    // GCC 15+, the default stdlib on most Linux distros). We probe libc++
    // first under `-stdlib=libc++`, then fall back to libstdc++ at the
    // driver's default search path.
    bool supports_import_std() const override {
        (void)libstdcxx_modules_json_path();
        return !modules_json_path_.empty();
    }

    std::string libstdcxx_modules_json_path() const override {
        std::call_once(modules_json_probe_once_, [this] {
            auto try_probe = [&](const std::string& stdlib_flag,
                                 const std::string& filename) -> std::string {
                std::string cmd = binary_;
                if (!stdlib_flag.empty()) cmd += " " + stdlib_flag;
                cmd += " -print-file-name=" + filename + " 2>/dev/null";
                std::string out = detail::run_command(cmd);
                while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
                    out.pop_back();
                if (out.empty() || out == filename) return {};
                std::error_code ec;
                if (!std::filesystem::exists(out, ec)) return {};
                auto canon = std::filesystem::weakly_canonical(out, ec);
                return ec ? out : canon.string();
            };
            // libc++ first (only matters if user explicitly opts in via
            // -stdlib=libc++; clang still uses libstdc++ at link time on
            // most Linux unless told otherwise).
            auto p = try_probe("-stdlib=libc++", "libc++.modules.json");
            if (!p.empty()) {
                modules_json_path_ = p;
                std_uses_libcxx_ = true;
                return;
            }
            p = try_probe("", "libstdc++.modules.json");
            if (!p.empty()) {
                modules_json_path_ = p;
                std_uses_libcxx_ = false;
            }
        });
        return modules_json_path_;
    }

    // True when supports_import_std() resolved via libc++ rather than
    // libstdc++ — callers (the std-module compile in target.cpp) need to
    // inject `-stdlib=libc++` so the std.cppm source picks up libc++
    // headers, not libstdc++'s.
    bool import_std_uses_libcxx() const {
        (void)libstdcxx_modules_json_path();
        return std_uses_libcxx_;
    }

    // Wrap a normal clang compile invocation in `clang-scan-deps -format=p1689
    // -o <ddi> -- ...`. The inner command needs `-c`, the source, and enough
    // flags (std, includes, defs, target/sysroot) for clang-scan-deps to
    // resolve the same imports the eventual real compile will see.
    CompilerCommand get_module_scan_command(const ModuleScanContext& ctx) const override {
        std::vector<std::string> cmd;
        std::vector<size_t> cosmetic;

        // Outer: clang-scan-deps invocation. Must be located before this is
        // called (supports_p1689() does the lookup); fall back to PATH name
        // if the cached probe is empty so we get a clean ENOENT later.
        std::call_once(scan_deps_probe_once_, [this] {
            scan_deps_path_ = locate_clang_scan_deps();
        });
        cmd.push_back(scan_deps_path_.empty() ? std::string("clang-scan-deps") : scan_deps_path_);
        cmd.push_back("-format=p1689");
        cmd.push_back("-o");
        cmd.push_back(ctx.output);
        cmd.push_back("--");

        // Inner: a self-contained clang compile command.
        cmd.push_back(binary_);
        inject_target_flags(cmd);

        if (!ctx.standard.empty()) {
            cmd.push_back("-std=c++" + ctx.standard);
        } else {
            cmd.push_back("-std=c++20");
        }

        if (ctx.color_diagnostics) emit_color_flag(cmd, cosmetic);

        // Force C++ mode for module-interface extensions (clang recognizes
        // .cppm but not all variants); harmless for plain .cpp.
        cmd.push_back("-x");
        cmd.push_back("c++");

        for (const auto& dir : ctx.includes) cmd.push_back("-I" + dir);
        for (const auto& dir : ctx.system_includes) cmd.push_back("-isystem" + dir);
        for (const auto& def : ctx.definitions) {
            std::string clean = def;
            if (clean.starts_with("-D")) clean = clean.substr(2);
            if (clean.empty()) continue;
            cmd.push_back("-D" + clean);
        }

        // primary-output gets recorded as ctx.obj_path so the collator can
        // join DDI -> obj task by output path, same as the GCC path.
        cmd.push_back("-c");
        cmd.push_back("-o");
        cmd.push_back(ctx.obj_path);
        cmd.push_back(ctx.source);

        return finalize(std::move(cmd), cosmetic);
    }

protected:
    // Clang has no -fmodule-mapper= equivalent. The collator writes a per-
    // importer response file at `<obj>.modules.rsp` containing
    // `-fmodule-file=name=path` (and `-fmodule-output=` for module-providing
    // TUs); we just point clang at it via `@<path>`. ctx.output is the .o
    // path, which is the stable per-task identifier the collator also uses.
    void emit_module_compile_flags(std::vector<std::string>& cmd,
                                    const CompileContext& ctx) const override {
        if (!ctx.output.empty()) {
            cmd.push_back("@" + ctx.output + ".modules.rsp");
        }
        for (const auto& mf : ctx.module_files) {
            cmd.push_back("-fmodule-file=" + mf);
        }
    }

    // Clang needs `-x c++-module` to treat the input as a module interface;
    // `-x c++` (the GCC override) silently demotes it to a regular TU and the
    // -fmodule-output= flag becomes a no-op. We trigger on either a module-
    // interface extension OR `bmi_output` being set (libstdc++ ships its std
    // module as a plain `bits/std.cc`, which has no special extension).
    void emit_module_input_kind_flags(std::vector<std::string>& cmd,
                                        std::string_view ext,
                                        const CompileContext& ctx) const override {
        const bool is_module_ext = (ext == ".cppm" || ext == ".ccm" ||
                                    ext == ".cxxm" || ext == ".ixx" || ext == ".mpp");
        const bool provides_module = !ctx.bmi_output.empty();
        if (ctx.is_module_source && (is_module_ext || provides_module)) {
            cmd.push_back("-x");
            cmd.push_back("c++-module");
        }
    }

private:
    // Find clang-scan-deps next to the clang driver. Mirrors how clang's own
    // toolchain does it: same directory, same version suffix. e.g.
    //   /usr/bin/clang++-18 -> /usr/bin/clang-scan-deps-18
    //   /opt/llvm/bin/clang++ -> /opt/llvm/bin/clang-scan-deps
    // Falls back to PATH lookup if no sibling is found.
    std::string locate_clang_scan_deps() const {
        if (binary_.empty()) return {};
        std::filesystem::path p(binary_);
        std::string dir = p.parent_path().string();
        std::string name(p.filename());

        // Extract version suffix from the binary basename (e.g. "-18" from
        // "clang++-18"). If absent, suffix is empty.
        std::string suffix;
        auto dash = name.find('-');
        if (dash != std::string::npos) {
            suffix = name.substr(dash);  // includes leading "-"
        }

        auto exists = [](const std::string& path) {
            std::error_code ec;
            return !path.empty() && std::filesystem::exists(path, ec);
        };

        if (!dir.empty()) {
            std::string with_suffix = dir + "/clang-scan-deps" + suffix;
            if (exists(with_suffix)) return with_suffix;
            std::string plain = dir + "/clang-scan-deps";
            if (exists(plain)) return plain;
        }

        // PATH lookup last resort. popen avoids re-implementing PATH parsing.
        std::string out = detail::run_command("command -v clang-scan-deps" + suffix + " 2>/dev/null");
        while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
        if (exists(out)) return out;

        out = detail::run_command("command -v clang-scan-deps 2>/dev/null");
        while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
        if (exists(out)) return out;

        return {};
    }

    mutable std::once_flag scan_deps_probe_once_;
    mutable std::string scan_deps_path_;
};

// TCC (Tiny C Compiler) driver. Inherits gcc-style argv from GnuCompiler;
// overrides only the kiln-internal flags TCC handles differently:
//   - color diagnostics (TCC errors on -fdiagnostics-color=)
//   - depfile syntax (TCC accepts -MD/-MF, not -MMD; rejects -MT)
//   - linker grouping (TCC's single-pass linker has no --start-group)
//   - archive tool (TCC ships its own `tcc -ar` archiver)
// User-facing flags (-std=, -fvisibility=, -fPIC, -pie) pass through
// unchanged: if the user pinned a property TCC rejects, that's a user
// misconfiguration and TCC's own error is more accurate than anything
// kiln could synthesize. Capability flags below opt out of features
// TCC doesn't implement (modules, P1689, import std).
class TccCompiler : public GnuCompiler {
public:
    using GnuCompiler::GnuCompiler;

    bool supports_p1689() const override { return false; }
    bool uses_per_task_module_rsp() const override { return false; }
    bool supports_import_std() const override { return false; }
    std::string libstdcxx_modules_json_path() const override { return {}; }

    std::vector<std::string> get_archive_command(
        const std::string& output, const std::vector<std::string>& objs) const override {
        std::vector<std::string> cmd;
        cmd.push_back(binary_);
        cmd.push_back("-ar");
        cmd.push_back("rcs");
        cmd.push_back(output);
        for (const auto& obj : objs) cmd.push_back(obj);
        return cmd;
    }

protected:
    void emit_color_flag(std::vector<std::string>& cmd,
                         std::vector<size_t>& cosmetic) const override {
        (void)cmd; (void)cosmetic;  // TCC rejects -fdiagnostics-color=*
    }

    void emit_dependency_flags(std::vector<std::string>& cmd,
                               const std::string& output) const override {
        // TCC supports -MD (Make-style depfile) and -MF, but rejects -MMD
        // (a GCC extension) and errors hard on -MT.
        cmd.push_back("-MD");
        cmd.push_back("-MF");
        cmd.push_back(output + ".d");
    }

    void emit_link_group_open(std::vector<std::string>& cmd) const override {
        (void)cmd;  // TCC's single-pass linker doesn't accept --start-group
    }
    void emit_link_group_close(std::vector<std::string>& cmd) const override {
        (void)cmd;
    }
};

// Intel classic ICC / ICPC. Mostly GCC-compatible for flags we emit, but
// rejects `-fdiagnostics-color=*` (warning #10148), unlike GCC/Clang.
class IccCompiler : public GnuCompiler {
public:
    using GnuCompiler::GnuCompiler;

protected:
    void emit_color_flag(std::vector<std::string>& cmd,
                         std::vector<size_t>& cosmetic) const override {
        (void)cmd;
        (void)cosmetic;
    }
};

// True for compiler IDs whose driver honors `--target=<triple>`. The
// Clang family does; GCC, TCC, and (future) MSVC error on it. Used to
// decide whether `CMAKE_<LANG>_COMPILER_TARGET` should be threaded into
// the Compiler constructor or dropped on the floor.
inline bool compiler_honors_target_flag(std::string_view id) {
    return id == "Clang" || id == "AppleClang"
        || id == "IntelLLVM" || id == "ARMClang";
}

// Construct a Compiler driver for a detected compiler id. Dispatch:
//   - Clang/AppleClang/IntelLLVM/ARMClang -> ClangCompiler (modules differ)
//   - TCC                                 -> TccCompiler
//   - Intel / ICC (classic)               -> IccCompiler
//   - GNU/Unknown/everything else         -> GnuCompiler
// Future: MSVC will branch off to its own non-gnu-derived class.
inline std::unique_ptr<Compiler> make_compiler(
    const std::string& compiler_id, std::string binary, Language lang,
    std::string sysroot = {}, std::string compiler_target = {}) {
    if (compiler_id == "Clang" || compiler_id == "AppleClang"
        || compiler_id == "IntelLLVM" || compiler_id == "ARMClang") {
        return std::make_unique<ClangCompiler>(
            std::move(binary), lang, std::move(sysroot), std::move(compiler_target));
    }
    if (compiler_id == "TCC") {
        return std::make_unique<TccCompiler>(
            std::move(binary), lang, std::move(sysroot), std::move(compiler_target));
    }
    if (compiler_id == "Intel" || compiler_id == "ICC") {
        return std::make_unique<IccCompiler>(
            std::move(binary), lang, std::move(sysroot), std::move(compiler_target));
    }
    return std::make_unique<GnuCompiler>(
        std::move(binary), lang, std::move(sysroot), std::move(compiler_target));
}

} // namespace kiln
