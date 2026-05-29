#include "interperter.hpp"
#include "command_parser.hpp"
#include "utils.hpp"
#include "target.hpp"
#include "build_system.hpp"
#include "gnu_compiler.hpp"
#include "genex_evaluator.hpp"
#include "profiler.hpp"
#include "printing.hpp"
#include "parse_number.hpp"
#include "version.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <array>
#include <charconv>
#ifdef __unix__
#include <sys/utsname.h>
#endif
#include "regex.hpp"
#include "builtins/registry.hpp"
#include "intercept/external_project.hpp"
#include "intercept/fetch_content.hpp"
#include "condition_evaluator.hpp"
#include "CMakeArray.hpp"
#include "path.hpp"
#include <cassert>
#include <future>

namespace kiln {

namespace {

// HACK: This function fakes CMake's compiler detection and platform initialization.
// CMake runs a complex startup sequence (CMakeDetermineSystem, CMakeDetermineCXXCompiler,
// CMakeDetermineCompilerABI, etc.) that sets hundreds of variables. We bypass all that
// and extract the essential information directly from the compiler.
//
// This is NOT proper CMake compatibility - it's a pragmatic shortcut that:
// - Only works with GCC (will break on Clang, MSVC, etc.)
// - Doesn't support cross-compilation properly
// - Doesn't run CMake's try_compile() tests for ABI detection
// - May miss variables that some Find modules expect
//
// TODO: Eventually either:
// 1. Implement proper platform detection (slow but correct)
// 2. Cache the results of CMake's detection (run once, reuse)
// 3. Incrementally add variables as projects need them
// Cached compiler detection data. Process-wide so we don't re-detect compilers
// when multiple Interpreters are created — including parallel EP orchestrators
// each constructing their own child Interpreter from worker threads. The
// once_flag guarantees the initial populate runs exactly once; the mutex
// guards the incremental writes that happen later (e.g. enable_language("ASM")
// caching ASM-compiler vars on first use). All access goes through the
// backup_vars_* helpers below — touching the map directly is a footgun.
static std::unordered_map<std::string, std::string> backup_vars;
static std::mutex backup_vars_mu;
static std::once_flag backup_vars_once;

static bool backup_vars_empty() {
    std::lock_guard lk(backup_vars_mu);
    return backup_vars.empty();
}
// Returns the value for `key`, or "" if absent. "" is also a legitimate stored
// value for some keys (CMAKE_*_FLAGS_RELEASE etc.); callers that need to
// distinguish "absent" from "empty" should use backup_vars_lookup instead.
static std::string backup_vars_get(std::string_view key) {
    std::lock_guard lk(backup_vars_mu);
    auto it = backup_vars.find(std::string(key));
    return it == backup_vars.end() ? std::string{} : it->second;
}
static std::optional<std::string> backup_vars_lookup(std::string_view key) {
    std::lock_guard lk(backup_vars_mu);
    auto it = backup_vars.find(std::string(key));
    if (it == backup_vars.end()) return std::nullopt;
    return it->second;
}
static void backup_vars_set(std::string key, std::string value) {
    std::lock_guard lk(backup_vars_mu);
    backup_vars[std::move(key)] = std::move(value);
}

// Variables for each language that enable_compiler_for_language() will apply.
static constexpr std::array c_lang_vars = {
    "CMAKE_C_COMPILER",
    "CMAKE_C_COMPILER_ID",
    "CMAKE_C_COMPILER_VERSION",
    "CMAKE_C_IMPLICIT_INCLUDE_DIRECTORIES",
    "CMAKE_C_IMPLICIT_LINK_DIRECTORIES",
    "CMAKE_C_IMPLICIT_LINK_LIBRARIES",
    "CMAKE_C_STANDARD_DEFAULT",
    "CMAKE_C90_STANDARD_COMPILE_OPTION",
    "CMAKE_C99_STANDARD_COMPILE_OPTION",
    "CMAKE_C11_STANDARD_COMPILE_OPTION",
    "CMAKE_C17_STANDARD_COMPILE_OPTION",
    "CMAKE_C23_STANDARD_COMPILE_OPTION",
};
static constexpr std::array cxx_lang_vars = {
    "CMAKE_CXX_COMPILER",
    "CMAKE_CXX_COMPILER_ID",
    "CMAKE_CXX_COMPILER_VERSION",
    "CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES",
    "CMAKE_CXX_IMPLICIT_LINK_DIRECTORIES",
    "CMAKE_CXX_IMPLICIT_LINK_LIBRARIES",
    "CMAKE_CXX_STANDARD_DEFAULT",
    "CMAKE_CXX98_STANDARD_COMPILE_OPTION",
    "CMAKE_CXX11_STANDARD_COMPILE_OPTION",
    "CMAKE_CXX14_STANDARD_COMPILE_OPTION",
    "CMAKE_CXX17_STANDARD_COMPILE_OPTION",
    "CMAKE_CXX20_STANDARD_COMPILE_OPTION",
    "CMAKE_CXX23_STANDARD_COMPILE_OPTION",
};
static constexpr std::array asm_lang_vars = {
    "CMAKE_ASM_COMPILER",    "CMAKE_ASM_COMPILER_ID",   "CMAKE_ASM_COMPILER_VERSION",     "CMAKE_ASM_FLAGS",
    "CMAKE_ASM_FLAGS_DEBUG", "CMAKE_ASM_FLAGS_RELEASE", "CMAKE_ASM_FLAGS_RELWITHDEBINFO", "CMAKE_ASM_FLAGS_MINSIZEREL",
};

// Build a cache key for a compiler binary: "<binary>:<realpath>:<mtime>:<sysroot>:<target>"
// Cheap to compute (no subprocesses), changes when binary is updated or swapped via
// symlink, or when sysroot / compiler-target alters the implicit dirs of detection.
std::string make_compiler_cache_key(const std::string& binary, const std::string& sysroot = {}, const std::string& compiler_target = {}) {
    // Resolve to absolute path via which(1)-style lookup
    std::error_code ec;
    auto resolved = std::filesystem::canonical(
        // If binary is just a name (e.g. "g++"), we need the full path.
        // /usr/bin/<binary> is the common case on Linux.
        binary.find('/') != std::string::npos ? binary : "/usr/bin/" + binary, ec);
    if (ec) return binary + ":unresolved:0:" + sysroot + ":" + compiler_target;

    auto mtime = std::filesystem::last_write_time(resolved, ec);
    if (ec) return binary + ":" + resolved.string() + ":0:" + sysroot + ":" + compiler_target;

    auto mtime_val = mtime.time_since_epoch().count();
    return binary + ":" + resolved.string() + ":" + std::to_string(mtime_val) + ":" + sysroot + ":" + compiler_target;
}

// Try to load compiler detection from disk cache. Returns nullopt on miss.
// On hit, validates by running --version and comparing output.
std::optional<PlatformInfo> try_cached_compiler_detection(CacheStore& cache, const std::string& binary, const std::string& cache_key) {
    auto cached = cache.lookup<CacheSubsystem::CompilerDetection>(cache_key);
    if (!cached) return std::nullopt;

    // Validate: run --version and compare (1 subprocess)
    std::string version_output = detail::run_command(binary + " --version 2>&1");
    if (version_output != cached->version_output) {
        return std::nullopt; // Compiler changed (e.g. shim switched target)
    }

    return cached->info;
}

// Detect a (possibly user-supplied) compiler against optional sysroot + target.
// Disk-cached, validates with --version. On miss, runs the full detection.
// This is the on-demand path used when the user specifies a non-default
// CMAKE_<LANG>_COMPILER (toolchain file, -DCMAKE_<LANG>_COMPILER=, etc.).
PlatformInfo detect_compiler_for(CacheStore& cache, const std::string& binary, Language lang, const std::string& sysroot,
                                 const std::string& compiler_target) {
    std::string key = make_compiler_cache_key(binary, sysroot, compiler_target);
    if (auto hit = try_cached_compiler_detection(cache, binary, key)) { return *hit; }
    GnuCompiler probe(binary, lang, sysroot, compiler_target);
    PlatformInfo info = probe.detect_platform();
    CompilerDetectionCacheEntry entry;
    entry.info = info;
    entry.version_output = detail::run_command(binary + " --version 2>&1");
    cache.insert<CacheSubsystem::CompilerDetection>(key, entry);
    return info;
}

// Set CMAKE_<LANG><STD>_STANDARD_COMPILE_OPTION variables from the driver.
// Each compiler family (GCC/Clang/TCC: "-std=cNN"; MSVC: "/std:c++NN")
// owns its spelling — kiln just asks. Empty results are skipped so a
// driver can decline a standard it has no flag for.
void apply_standard_options(Interpreter& interp, const std::string& lang, const Compiler& compiler) {
    auto set_for = [&](const char* var_prefix, Language l, int std_val) {
        std::string flag = compiler.std_compile_option(l, std_val);
        if (flag.empty()) return;
        interp.set_variable(std::string("CMAKE_") + var_prefix + std::to_string(std_val) + "_STANDARD_COMPILE_OPTION", flag);
    };
    if (lang == "C") {
        for (int s : {90, 99, 11, 17, 23}) set_for("C", Language::C, s);
    } else if (lang == "CXX") {
        for (int s : {98, 11, 14, 17, 20, 23}) set_for("CXX", Language::CXX, s);
    }
}

// Populate CMAKE_<LANG>_* variables from a PlatformInfo. Used by the on-demand
// detection path in enable_compiler_for_language.
void populate_lang_vars(Interpreter& interp, const std::string& lang, const std::string& binary, const PlatformInfo& info,
                        const Compiler& compiler) {
    interp.set_variable("CMAKE_" + lang + "_COMPILER", binary);
    interp.set_variable("CMAKE_" + lang + "_COMPILER_ID", info.compiler_id);
    interp.set_variable("CMAKE_" + lang + "_COMPILER_VERSION", info.compiler_version);
    interp.set_variable("CMAKE_" + lang + "_IMPLICIT_INCLUDE_DIRECTORIES", CMakeArray(info.implicit_includes).to_string());
    interp.set_variable("CMAKE_" + lang + "_IMPLICIT_LINK_DIRECTORIES", CMakeArray(info.implicit_link_dirs).to_string());
    interp.set_variable("CMAKE_" + lang + "_IMPLICIT_LINK_LIBRARIES", CMakeArray(info.implicit_link_libs).to_string());
    if (lang == "CXX" && info.default_cxx_standard > 0) {
        interp.set_variable("CMAKE_CXX_STANDARD_DEFAULT", std::to_string(info.default_cxx_standard));
    } else if (lang == "C" && info.default_c_standard > 0) {
        interp.set_variable("CMAKE_C_STANDARD_DEFAULT", std::to_string(info.default_c_standard));
    }
    apply_standard_options(interp, lang, compiler);

    // Cross-compile system info comes from the target compiler's macros.
    if (!info.system_name.empty()) interp.set_variable("CMAKE_SYSTEM_NAME", info.system_name);
    if (!info.system_processor.empty()) interp.set_variable("CMAKE_SYSTEM_PROCESSOR", info.system_processor);
    if (!info.sizeof_void_p.empty()) interp.set_variable("CMAKE_SIZEOF_VOID_P", info.sizeof_void_p);

    // CMAKE_CROSSCOMPILING is TRUE when the target system differs from host.
    // CMake sets this in CMakeDetermineSystem; mirroring it here means
    // try_run, find_package's root_path filtering, and user-side
    // `if(CMAKE_CROSSCOMPILING)` checks all behave as expected.
    const std::string host_name = interp.get_variable("CMAKE_HOST_SYSTEM_NAME");
    const std::string host_proc = interp.get_variable("CMAKE_HOST_SYSTEM_PROCESSOR");
    const std::string sys_name = interp.get_variable("CMAKE_SYSTEM_NAME");
    const std::string sys_proc = interp.get_variable("CMAKE_SYSTEM_PROCESSOR");
    const bool cross = (!sys_name.empty() && !host_name.empty() && sys_name != host_name)
                       || (!sys_proc.empty() && !host_proc.empty() && sys_proc != host_proc);
    interp.set_variable("CMAKE_CROSSCOMPILING", cross ? "TRUE" : "FALSE");
}

void fake_cmake_compiler_checks_and_init(Interpreter& interp, CacheStore& cache) {
    // Populate the process-wide backup_vars exactly once. call_once
    // synchronizes every reader with the populator: if multiple EP
    // orchestrator threads land here concurrently, only one runs the
    // detection block and the rest block until it finishes — no torn
    // reads of an in-flight unordered_map.
    std::call_once(backup_vars_once, [&cache] {
        // Try disk cache first, fall back to full detection
        std::string cxx_cache_key = make_compiler_cache_key("g++");
        std::string c_cache_key = make_compiler_cache_key("gcc");

        auto cxx_cached = try_cached_compiler_detection(cache, "g++", cxx_cache_key);
        auto c_cached = try_cached_compiler_detection(cache, "gcc", c_cache_key);

        PlatformInfo cxx_info, c_info;

        if (cxx_cached && c_cached) {
            // Full cache hit — no additional subprocesses needed
            cxx_info = std::move(*cxx_cached);
            c_info = std::move(*c_cached);
        } else {
            // Cache miss on at least one compiler — detect in parallel
            auto detect_and_cache = [&cache](const std::string& binary, Language lang, const std::string& cache_key,
                                             std::optional<PlatformInfo> cached) -> PlatformInfo {
                if (cached) return std::move(*cached);
                GnuCompiler compiler(binary, lang);
                auto info = compiler.detect_platform();
                CompilerDetectionCacheEntry entry;
                entry.info = info;
                entry.version_output = detail::run_command(binary + " --version 2>&1");
                cache.insert<CacheSubsystem::CompilerDetection>(cache_key, entry);
                return info;
            };

            auto cxx_future = std::async(std::launch::async, detect_and_cache, "g++", Language::CXX, cxx_cache_key, std::move(cxx_cached));
            auto c_future = std::async(std::launch::async, detect_and_cache, "gcc", Language::C, c_cache_key, std::move(c_cached));
            cxx_info = cxx_future.get();
            c_info = c_future.get();
        }

        // Cache CXX data in-process
        backup_vars_set("CMAKE_CXX_COMPILER", "g++");
        backup_vars_set("CMAKE_CXX_COMPILER_ID", cxx_info.compiler_id);
        backup_vars_set("CMAKE_CXX_COMPILER_VERSION", cxx_info.compiler_version);
        backup_vars_set("CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES", CMakeArray(cxx_info.implicit_includes).to_string());
        backup_vars_set("CMAKE_CXX_IMPLICIT_LINK_DIRECTORIES", CMakeArray(cxx_info.implicit_link_dirs).to_string());
        backup_vars_set("CMAKE_CXX_IMPLICIT_LINK_LIBRARIES", CMakeArray(cxx_info.implicit_link_libs).to_string());
        if (cxx_info.default_cxx_standard > 0) backup_vars_set("CMAKE_CXX_STANDARD_DEFAULT", std::to_string(cxx_info.default_cxx_standard));
        // Host-default backup uses gcc-style -std= strings unconditionally:
        // this block fires only when host detection resolved gcc/g++ (the
        // default binary). User-pinned non-GCC compilers go through the
        // on-demand path which queries the driver for std_compile_option().
        backup_vars_set("CMAKE_CXX98_STANDARD_COMPILE_OPTION", "-std=c++98");
        backup_vars_set("CMAKE_CXX11_STANDARD_COMPILE_OPTION", "-std=c++11");
        backup_vars_set("CMAKE_CXX14_STANDARD_COMPILE_OPTION", "-std=c++14");
        backup_vars_set("CMAKE_CXX17_STANDARD_COMPILE_OPTION", "-std=c++17");
        backup_vars_set("CMAKE_CXX20_STANDARD_COMPILE_OPTION", "-std=c++20");
        backup_vars_set("CMAKE_CXX23_STANDARD_COMPILE_OPTION", "-std=c++23");

        // Cache C data in-process
        backup_vars_set("CMAKE_C_COMPILER", "gcc");
        backup_vars_set("CMAKE_C_COMPILER_ID", c_info.compiler_id);
        backup_vars_set("CMAKE_C_COMPILER_VERSION", c_info.compiler_version);
        backup_vars_set("CMAKE_C_IMPLICIT_INCLUDE_DIRECTORIES", CMakeArray(c_info.implicit_includes).to_string());
        backup_vars_set("CMAKE_C_IMPLICIT_LINK_DIRECTORIES", CMakeArray(c_info.implicit_link_dirs).to_string());
        backup_vars_set("CMAKE_C_IMPLICIT_LINK_LIBRARIES", CMakeArray(c_info.implicit_link_libs).to_string());
        if (c_info.default_c_standard > 0) backup_vars_set("CMAKE_C_STANDARD_DEFAULT", std::to_string(c_info.default_c_standard));
        backup_vars_set("CMAKE_C90_STANDARD_COMPILE_OPTION", "-std=c90");
        backup_vars_set("CMAKE_C99_STANDARD_COMPILE_OPTION", "-std=c99");
        backup_vars_set("CMAKE_C11_STANDARD_COMPILE_OPTION", "-std=c11");
        backup_vars_set("CMAKE_C17_STANDARD_COMPILE_OPTION", "-std=c17");
        backup_vars_set("CMAKE_C23_STANDARD_COMPILE_OPTION", "-std=c23");

        // Cache system-level data (same for both compilers)
        backup_vars_set("CMAKE_SYSTEM_NAME", cxx_info.system_name);
        backup_vars_set("CMAKE_SYSTEM_PROCESSOR", cxx_info.system_processor);
        backup_vars_set("CMAKE_SIZEOF_VOID_P", cxx_info.sizeof_void_p);
        backup_vars_set("CMAKE_HOST_SYSTEM_NAME", cxx_info.system_name);
        backup_vars_set("CMAKE_HOST_SYSTEM_PROCESSOR", cxx_info.system_processor);
    });

    // Per-Interpreter: copy system-level vars out of the populated cache.
    for (const char* name :
         {"CMAKE_SYSTEM_NAME", "CMAKE_SYSTEM_PROCESSOR", "CMAKE_SIZEOF_VOID_P", "CMAKE_HOST_SYSTEM_NAME", "CMAKE_HOST_SYSTEM_PROCESSOR"}) {
        if (auto v = backup_vars_lookup(name)) { interp.set_variable(name, *v); }
    }
    interp.set_variable("CMAKE_CFG_INTDIR", ".");
}

} // anonymous namespace

std::string Interpreter::enable_compiler_for_language(const std::string& lang) {
    std::string loaded_var = "CMAKE_" + lang + "_COMPILER_LOADED";
    if (!get_variable(loaded_var).empty()) return {}; // Already loaded

    if (lang == "C" || lang == "CXX") {
        const Language lang_enum = (lang == "C") ? Language::C : Language::CXX;
        const std::string default_binary = (lang == "C") ? "gcc" : "g++";

        // Resolve effective compiler and target options from current scope.
        // User-supplied values (toolchain file, -D, set() in CMakeLists)
        // take precedence over the host-default detection.
        std::string user_binary = get_variable("CMAKE_" + lang + "_COMPILER");
        std::string sysroot = get_variable("CMAKE_SYSROOT");
        std::string compiler_target = get_variable("CMAKE_" + lang + "_COMPILER_TARGET");

        // $CC may embed flags (e.g. "gcc --sysroot=/path"). CMake splits these;
        // kiln must not pass the whole string as the executable path.
        std::string detect_sysroot;
        if (!user_binary.empty()) {
            auto parts = shell_split(user_binary);
            if (parts.size() > 1) {
                user_binary = parts.front();
                set_variable("CMAKE_" + lang + "_COMPILER", user_binary);
                set_cache_variable("CMAKE_" + lang + "_COMPILER", user_binary);
                std::string cc_flags;
                for (size_t i = 1; i < parts.size(); ++i) {
                    const std::string& a = parts[i];
                    if (detect_sysroot.empty() && a.starts_with("--sysroot=")) {
                        detect_sysroot = a.substr(10);
                    } else if (detect_sysroot.empty() && a == "--sysroot" && i + 1 < parts.size()) {
                        detect_sysroot = parts[++i];
                    } else if (compiler_target.empty() && a.starts_with("--target=")) {
                        compiler_target = a.substr(9);
                    } else if (compiler_target.empty() && a == "--target" && i + 1 < parts.size()) {
                        compiler_target = parts[++i];
                    }
                    if (!cc_flags.empty()) cc_flags += ' ';
                    cc_flags += a;
                }
                if (!compiler_target.empty()) {
                    const std::string tvar = "CMAKE_" + lang + "_COMPILER_TARGET";
                    set_variable(tvar, compiler_target);
                    set_cache_variable(tvar, compiler_target);
                }
                const std::string flags_var = "CMAKE_" + lang + "_FLAGS";
                std::string flags = get_variable(flags_var);
                if (!cc_flags.empty()) {
                    if (!flags.empty()) flags += ' ';
                    flags += cc_flags;
                    set_variable(flags_var, flags);
                    set_cache_variable(flags_var, flags);
                }
            }
        }
        const std::string probe_sysroot = !sysroot.empty() ? sysroot : detect_sysroot;

        const std::string effective_binary = user_binary.empty() ? default_binary : user_binary;

        // Fast path: user didn't override anything → reuse host-detection backup.
        const bool host_path = user_binary.empty() && sysroot.empty() && compiler_target.empty() && !backup_vars_empty();

        // Resolve compiler_id either from the host-detection backup or via
        // on-demand detection. We need it early so the right Compiler subclass
        // can be constructed before populate_lang_vars asks the driver for
        // standard-option spellings.
        std::string detected_id;
        std::optional<PlatformInfo> on_demand_info;
        if (host_path) {
            auto apply_backup = [&](auto const& vars) {
                for (const auto& var : vars) {
                    if (auto v = backup_vars_lookup(var); v && !v->empty()) set_variable(var, *v);
                }
            };
            if (lang == "C")
                apply_backup(c_lang_vars);
            else
                apply_backup(cxx_lang_vars);
            detected_id = get_variable("CMAKE_" + lang + "_COMPILER_ID");
        } else {
            // On-demand detection against the user's compiler + sysroot/target.
            // Cache-keyed so repeat invocations are cheap.
            on_demand_info = detect_compiler_for(*cache_store_, effective_binary, lang_enum, probe_sysroot, compiler_target);
            detected_id = on_demand_info->compiler_id;
        }

        // CMAKE_<LANG>_COMPILER_TARGET only makes sense for drivers that
        // honor --target=. (compiler_target is always empty on host_path.)
        const std::string compile_target_effective = compiler_honors_target_flag(detected_id) ? compiler_target : std::string{};
        auto compiler = make_compiler(detected_id, effective_binary, lang_enum, sysroot, compile_target_effective);

        if (on_demand_info) { populate_lang_vars(*this, lang, effective_binary, *on_demand_info, *compiler); }

        const std::string id = get_variable("CMAKE_" + lang + "_COMPILER_ID");
        if (id == "GNU" || id == "Clang") { set_variable("CMAKE_" + lang + "_VERBOSE_FLAG", "-v"); }
        if (id == "GNU") { set_variable(lang == "C" ? "CMAKE_COMPILER_IS_GNUCC" : "CMAKE_COMPILER_IS_GNUCXX", "1"); }

        // Populate the prereq vars that CMake's Compiler/<id>-<lang>.cmake
        // expects to find (CMAKE_<LANG>_STANDARD_COMPUTED_DEFAULT,
        // CMAKE_<LANG>_EXTENSIONS_COMPUTED_DEFAULT). Real CMake derives these
        // by probing the compiler; we approximate from id+version. Required
        // whether or not we end up loading the upstream module — projects
        // can read these directly too.
        if ((id == "GNU" || id == "Clang") && lang == "CXX") {
            std::string ver = get_variable("CMAKE_CXX_COMPILER_VERSION");
            // gcc 11.1+ → C++17, 6.0+ → C++14, else C++98. Clang follows similar
            // thresholds in practice; this default is only used when the project
            // doesn't pin CXX_STANDARD explicitly.
            std::string std_default = "98";
            auto cmp = [&](const char* threshold) {
                int a[3] = {0, 0, 0}, b[3] = {0, 0, 0};
                std::sscanf(ver.c_str(), "%d.%d.%d", &a[0], &a[1], &a[2]);
                std::sscanf(threshold, "%d.%d.%d", &b[0], &b[1], &b[2]);
                for (int i = 0; i < 3; ++i) {
                    if (a[i] != b[i]) return a[i] >= b[i];
                }
                return true;
            };
            if (cmp("11.1"))
                std_default = "17";
            else if (cmp("6.0"))
                std_default = "14";
            set_variable("CMAKE_CXX_STANDARD_COMPUTED_DEFAULT", std_default);
            set_variable("CMAKE_CXX_EXTENSIONS_COMPUTED_DEFAULT", "ON");
        } else if ((id == "GNU" || id == "Clang") && lang == "C") {
            set_variable("CMAKE_C_STANDARD_COMPUTED_DEFAULT", "11");
            set_variable("CMAKE_C_EXTENSIONS_COMPUTED_DEFAULT", "ON");
        }

        // Default path: include CMake's upstream Compiler/<id>-<lang>.cmake so
        // downstream modules (CheckPIESupported, CheckLinkerFlag, …) find every
        // var they expect — PIE, PIC, visibility, language standards, dep
        // formats, module flags. Hardcoding a subset turns into endless
        // whack-a-mole as projects exercise new flags.
        //
        // --fast-setup falls back to a hand-rolled subset that covers the most
        // common needs without reading any external module. Faster, but breaks
        // the moment a project touches something we didn't anticipate.
        bool fast_setup = !get_variable("KILN_FAST_SETUP").empty();
        if (fast_setup) {
            if (id == "GNU" || id == "Clang") {
                const std::string p = "CMAKE_" + lang + "_";
                set_variable(p + "COMPILE_OPTIONS_PIC", "-fPIC");
                set_variable(p + "COMPILE_OPTIONS_PIE", "-fPIE");
                set_variable(p + "LINK_OPTIONS_PIE", "-fPIE;-pie");
                set_variable(p + "LINK_OPTIONS_NO_PIE", "-no-pie");
                set_variable("_CMAKE_" + lang + "_PIE_MAY_BE_SUPPORTED_BY_LINKER", "YES");
                set_variable(p + "COMPILE_OPTIONS_VISIBILITY", "-fvisibility=");
                set_variable(p + "COMPILE_OPTIONS_VISIBILITY_INLINES_HIDDEN", "-fvisibility-inlines-hidden");
            }
        } else if (id == "GNU" || id == "Clang") {
            std::string module = "Compiler/" + id + "-" + lang;
            std::string include_arg = module;
            // include() with a bare module name searches CMAKE_MODULE_PATH and
            // the system Modules dir.
            auto res = execute_command_with_args("include", {include_arg});
            if (!res) {
                // If the module fails to load, surface it but don't abort —
                // user can fall back to --fast-setup. Most projects will
                // never reach this path.
                print_message("WARNING", "enable_language(" + lang + "): failed to load " + module
                                             + ".cmake — "
                                               "consider --fast-setup. Error: "
                                             + res.error().message);
            }
        }
        set_variable("CMAKE_" + lang + "_COMPILER_LOADED", "1");

        compiler->set_version(get_variable("CMAKE_" + lang + "_COMPILER_VERSION"));
        get_toolchain().set_compiler(lang_enum, std::move(compiler));
    } else if (lang == "ASM") {
        // Don't apply the host-detection backup if the user already pinned a
        // compiler (toolchain file, -D, set() before project()). Otherwise
        // the cached host `gcc` would clobber e.g. `riscv64-unknown-elf-gcc`
        // and downstream commands would invoke the host assembler against
        // cross-arch sources.
        std::string user_asm = get_variable("CMAKE_ASM_COMPILER");
        std::string user_sysroot = get_variable("CMAKE_SYSROOT");
        std::string user_asm_target = get_variable("CMAKE_ASM_COMPILER_TARGET");
        const bool asm_host_path = user_asm.empty() && user_sysroot.empty() && user_asm_target.empty();
        auto cached_asm = backup_vars_lookup("CMAKE_ASM_COMPILER");
        if (asm_host_path && cached_asm && !cached_asm->empty()) {
            for (const auto& var : asm_lang_vars) {
                if (auto v = backup_vars_lookup(var); v && !v->empty()) set_variable(var, *v);
            }
        } else if (!user_asm.empty()) {
            // User pinned the ASM compiler; fill in companion vars from the
            // C side if available, then defaults. Don't touch CMAKE_ASM_COMPILER.
            if (get_variable("CMAKE_ASM_COMPILER_ID").empty()) set_variable("CMAKE_ASM_COMPILER_ID", get_variable("CMAKE_C_COMPILER_ID"));
            if (get_variable("CMAKE_ASM_COMPILER_VERSION").empty())
                set_variable("CMAKE_ASM_COMPILER_VERSION", get_variable("CMAKE_C_COMPILER_VERSION"));
            for (const auto* var : asm_lang_vars) {
                std::string_view v(var);
                if (get_variable(var).empty()) {
                    if (v == "CMAKE_ASM_FLAGS_DEBUG")
                        set_variable(var, "-g");
                    else if (v.starts_with("CMAKE_ASM_FLAGS"))
                        set_variable(var, "");
                }
            }
        } else {
            // Derive from C compiler (prefer C, fallback to "cc")
            std::string asm_compiler = get_variable("CMAKE_C_COMPILER");
            std::string asm_id = get_variable("CMAKE_C_COMPILER_ID");
            std::string asm_version = get_variable("CMAKE_C_COMPILER_VERSION");
            if (asm_compiler.empty()) {
                // C not enabled yet, pull from cache
                asm_compiler = backup_vars_get("CMAKE_C_COMPILER");
                asm_id = backup_vars_get("CMAKE_C_COMPILER_ID");
                asm_version = backup_vars_get("CMAKE_C_COMPILER_VERSION");
            }
            if (asm_compiler.empty()) {
                asm_compiler = "cc";
                asm_id = "GNU";
                asm_version = "";
            }
            set_variable("CMAKE_ASM_COMPILER", asm_compiler);
            set_variable("CMAKE_ASM_COMPILER_ID", asm_id);
            set_variable("CMAKE_ASM_COMPILER_VERSION", asm_version);
            set_variable("CMAKE_ASM_FLAGS", "");
            set_variable("CMAKE_ASM_FLAGS_DEBUG", "-g");
            set_variable("CMAKE_ASM_FLAGS_RELEASE", "");
            set_variable("CMAKE_ASM_FLAGS_RELWITHDEBINFO", "");
            set_variable("CMAKE_ASM_FLAGS_MINSIZEREL", "");

            // Cache for next time
            for (const auto& var : asm_lang_vars) { backup_vars_set(var, get_variable(var)); }
        }
        set_variable("CMAKE_ASM_COMPILER_LOADED", "1");
        const std::string asm_id_now = get_variable("CMAKE_ASM_COMPILER_ID");
        const std::string asm_target = compiler_honors_target_flag(asm_id_now) ? get_variable("CMAKE_ASM_COMPILER_TARGET") : std::string{};
        auto compiler =
            make_compiler(asm_id_now, get_variable("CMAKE_ASM_COMPILER"), Language::ASM, get_variable("CMAKE_SYSROOT"), asm_target);
        compiler->set_version(get_variable("CMAKE_ASM_COMPILER_VERSION"));
        get_toolchain().set_compiler(Language::ASM, std::move(compiler));
    } else {
        return "unsupported language: " + lang + " (only C, CXX, and ASM are supported)";
    }

    // Seed CMAKE_<LANG>_FLAGS / per-config flags from *_INIT, mirroring CMake.
    // Toolchain files commonly set CMAKE_<LANG>_FLAGS_INIT (e.g. -mcmodel=medany
    // for RISC-V) and rely on enable_language to promote them. Only apply when
    // the user-visible variable is empty so we don't clobber explicit user flags.
    auto seed_from_init = [&](const std::string& var) {
        if (get_variable(var).empty()) {
            std::string init = get_variable(var + "_INIT");
            if (!init.empty()) set_variable(var, init);
        }
    };
    seed_from_init("CMAKE_" + lang + "_FLAGS");
    for (const auto* cfg : {"DEBUG", "RELEASE", "RELWITHDEBINFO", "MINSIZEREL"}) { seed_from_init("CMAKE_" + lang + "_FLAGS_" + cfg); }
    return {};
}

Interpreter* Interpreter::get_root() {
    return this; // No child interpreters anymore - we ARE the root
}

const Interpreter* Interpreter::get_root() const {
    return this; // No child interpreters anymore - we ARE the root
}

DirectoryContext& Interpreter::get_current_directory_context() {
    if (directory_stack_.empty()) {
        std::cerr << "FATAL: get_current_directory_context() called with empty directory_stack_\n";
        std::abort();
    }
    const std::string& current_dir = directory_stack_.back();
    auto it = directory_contexts_.find(current_dir);
    if (it == directory_contexts_.end()) {
        std::cerr << "FATAL: Directory context not found for: " << current_dir << "\n";
        std::abort();
    }
    return it->second;
}

DirectoryContext* Interpreter::get_directory_context(const std::string& dir) {
    std::string abs_dir = Path::make_absolute_and_normal(get_variable("CMAKE_CURRENT_SOURCE_DIR"), dir);

    auto it = directory_contexts_.find(abs_dir);
    if (it != directory_contexts_.end()) { return &it->second; }
    // CMake also accepts the directory's binary dir as a key for DEFER.
    for (auto& [src, ctx] : directory_contexts_) {
        if (ctx.binary_dir == abs_dir) { return &ctx; }
    }
    return nullptr;
}

void Interpreter::push_directory(const std::string& source_dir, const std::string& binary_dir) {
    std::filesystem::path abs_source = std::filesystem::absolute(source_dir).lexically_normal();
    std::string abs_source_str = abs_source.string();

    // Create new DirectoryContext
    DirectoryContext ctx;
    ctx.source_dir = abs_source_str;
    ctx.binary_dir = binary_dir;

    // Set parent directory for property inheritance
    if (!directory_stack_.empty()) {
        ctx.parent_dir = directory_stack_.back();
        // Inherit accumulated directory properties from parent (CMake semantics)
        auto parent_it = directory_contexts_.find(ctx.parent_dir);
        if (parent_it != directory_contexts_.end()) { ctx.accumulated = parent_it->second.accumulated; }
    }

    // Add to contexts map
    directory_contexts_[abs_source_str] = std::move(ctx);

    // Push onto stack
    directory_stack_.push_back(abs_source_str);
}

void Interpreter::pop_directory() {
    if (directory_stack_.empty()) {
        std::cerr << "FATAL: pop_directory() called with empty directory_stack_\n";
        std::abort();
    }
    // Don't remove the context from the map - it's needed for get_property DIRECTORY /path
    directory_stack_.pop_back();
}

void Interpreter::execute_deferred_calls() {
    auto& ctx = get_root()->get_current_directory_context();
    // Copy the list - deferred calls could schedule more deferred calls
    auto calls = std::move(ctx.deferred_calls);
    ctx.deferred_calls.clear();
    for (const auto& call : calls) {
        if (fatal_error_) break;
        auto res = execute_command_with_args(call.command, call.arguments);
        if (!res) { set_fatal_error(res.error()); }
    }
}

void kiln::Interpreter::process_file_generates(const GenexEvaluationContext& genex_ctx) {
    for (auto& entry : pending_file_generates_) {
        // Set up per-entry genex context with the TARGET if specified
        auto entry_ctx = genex_ctx;
        if (!entry.target_name.empty()) {
            if (auto it = targets_.find(entry.target_name); it != targets_.end()) { entry_ctx.current_target = it->second.get(); }
        }
        GenexEvaluator evaluator(entry_ctx);

        // Evaluate CONDITION — skip if falsy
        if (!entry.condition.empty()) {
            auto cond_result = evaluator.evaluate(entry.condition);
            if (!cond_result || cond_result->empty() || *cond_result == "0") { continue; }
        }

        // Evaluate OUTPUT path
        auto output_result = evaluator.evaluate(entry.output);
        if (!output_result) {
            print_message("WARNING", "file(GENERATE) failed to evaluate OUTPUT '" + entry.output + "': " + output_result.error());
            continue;
        }
        std::filesystem::path out_path = *output_result;
        if (!out_path.is_absolute()) { out_path = std::filesystem::path(entry.binary_dir) / out_path; }

        // Evaluate CONTENT
        auto content_result = evaluator.evaluate(entry.content);
        if (!content_result) {
            set_fatal_error("file(GENERATE) failed to evaluate CONTENT for '" + out_path.string() + "': " + content_result.error());
            return;
        }
        std::string file_content = std::move(*content_result);

        // Apply NEWLINE_STYLE
        if (!entry.newline_style.empty()) {
            std::string nl;
            if (entry.newline_style == "DOS" || entry.newline_style == "WIN32" || entry.newline_style == "CRLF") { nl = "\r\n"; }
            if (!nl.empty()) {
                std::string converted;
                converted.reserve(file_content.size());
                for (size_t j = 0; j < file_content.size(); ++j) {
                    if (file_content[j] == '\r' && j + 1 < file_content.size() && file_content[j + 1] == '\n') {
                        converted += nl;
                        ++j;
                    } else if (file_content[j] == '\n') {
                        converted += nl;
                    } else {
                        converted += file_content[j];
                    }
                }
                file_content = std::move(converted);
            }
        }

        // Create parent directories and write
        if (out_path.has_parent_path()) {
            std::error_code ec;
            std::filesystem::create_directories(out_path.parent_path(), ec);
        }

        // Track this output for implicit dependency injection into custom commands
        auto normalized_out = out_path.lexically_normal().string();
        file_generate_outputs_.insert(normalized_out);

        // Only write if content actually changed (avoid unnecessary mtime updates
        // that would trigger downstream custom command rebuilds)
        bool needs_write = true;
        if (std::filesystem::exists(out_path)) {
            std::ifstream existing(out_path, std::ios::binary);
            if (existing) {
                std::string existing_content((std::istreambuf_iterator<char>(existing)), std::istreambuf_iterator<char>());
                existing.close();
                Hash256 existing_hash = blake2b(existing_content);
                Hash256 new_hash = blake2b(file_content);
                needs_write = (memcmp(existing_hash.bytes, new_hash.bytes, 32) != 0);
            }
        }

        if (needs_write) {
            std::ofstream out(out_path);
            if (!out) {
                print_message("WARNING", "file(GENERATE) could not write to: " + out_path.string());
                continue;
            }
            out << file_content;
        }
    }

    // Inject file(GENERATE) outputs as implicit dependencies of custom commands
    // that reference them in their COMMAND arguments (e.g. -DIN_FILE=/path/to/generated.in).
    // This ensures the custom command's signature includes the generated file's mtime,
    // so it re-runs when the generated content changes.
    if (!file_generate_outputs_.empty()) {
        for (auto& [output_path, rule] : custom_command_rules_) {
            for (const auto& cmd : rule->commands) {
                for (const auto& arg : cmd) {
                    // Extract path from arg: could be bare path or -DVAR=path
                    std::string_view candidate = arg;
                    auto eq = candidate.find('=');
                    if (eq != std::string_view::npos) { candidate = candidate.substr(eq + 1); }
                    auto norm = std::filesystem::path(candidate).lexically_normal().string();
                    if (file_generate_outputs_.count(norm)) { rule->depends.push_back(norm); }
                }
            }
        }
    }

    pending_file_generates_.clear();
}

std::expected<kiln::Interpreter*, kiln::BuildError> kiln::Interpreter::run_build(int jobs,
                                                                                 const std::vector<std::string>& requested_targets) {
    // Sanity check CMAKE_BUILD_TYPE
    std::array<std::string, 4> stanard_build_types_lower = {"debug", "release", "minsize", "relwithdebinfo"};
    auto build_type = get_variable("CMAKE_BUILD_TYPE");
    build_type = to_lower(build_type);
    if (std::find(stanard_build_types_lower.begin(), stanard_build_types_lower.end(), build_type) == stanard_build_types_lower.end()) {
        print_message("WARN", "Build type '" + build_type + "' is not a standard build type. Things MIGHT go wrong.");
    }

    // No longer needed - no child interpreters anymore

    print_message("STATUS", "Generating build graph...");
    auto graph_start = std::chrono::steady_clock::now();

    ProfileScope graph_scope("generate build graph", "graph");
    auto graph_result = generate_build_graph(requested_targets);
    if (!graph_result) { return std::unexpected(graph_result.error()); }
    auto& graph = *graph_result;
    graph_scope.stop();

    // Print deferred target dumps (AT_BUILD) - targets are now resolved
    for (const auto& dump_target_name : get_targets_to_dump_at_build()) {
        if (targets_.count(dump_target_name)) { print_message("", targets_[dump_target_name]->generate_dump_info()); }
    }

    // Apply compat deps and validate
    graph.apply_cmake_compat_deps();
    auto validate_result = graph.validate();
    if (!validate_result) { return std::unexpected(BuildError{current_file_, validate_result.error()}); }

    std::string root_binary_dir = get_variable("CMAKE_BINARY_DIR");

    // 3. Generate compile_commands.json (on by default)
    std::string export_cmds = get_variable("CMAKE_EXPORT_COMPILE_COMMANDS");
    if (!is_falsy(export_cmds)) {
        auto result = graph.generate_compile_commands(root_binary_dir);
        if (!result) { return std::unexpected(BuildError{current_file_, result.error()}); }
    }

    {
        double graph_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - graph_start).count();
        std::ostringstream timing_msg;
        timing_msg << "Generating done (" << std::fixed << std::setprecision(1) << graph_s << "s)";
        print_message("STATUS", timing_msg.str());
    }

    // 4. Execute the build graph
    print_message("STATUS", "Starting " + build_type + " build...");
    auto result = graph.execute(root_binary_dir, jobs);

    if (!result) { return std::unexpected(BuildError{current_file_, result.error()}); }

    return this;
}

std::expected<BuildGraph, BuildError> Interpreter::generate_build_graph(const std::vector<std::string>& requested_targets) {
    // Same as generate_dirty_tasks(), but returns the full graph instead of just dirty tasks.
    // Used by EP attachment to atomically add ALL EP tasks (clean tasks skip via normal signature check).

    // Determine which targets to build
    std::set<std::string> targets_to_build;
    std::vector<std::string> initial_targets;

    if (requested_targets.empty()) {
        for (const auto& [name, target] : targets_) {
            auto custom = std::dynamic_pointer_cast<CustomTarget>(target);
            if (custom) {
                if (custom->is_build_by_default()) { initial_targets.push_back(name); }
            } else {
                std::string exclude = target->get_property("EXCLUDE_FROM_ALL");
                if (exclude.empty() || is_falsy(exclude)) { initial_targets.push_back(name); }
            }
        }
    } else {
        for (const auto& t : requested_targets) {
            std::string resolved = resolve_target_alias(t);
            if (!targets_.count(resolved)) { return std::unexpected(BuildError{current_file_, "Unknown target: " + t}); }
            initial_targets.push_back(resolved);
        }
    }

    // Resolve all initial targets
    for (const auto& name : initial_targets) { targets_[name]->resolve(targets_, *this); }

    // Collect targets to build
    std::function<void(const std::string&)> collect = [&](const std::string& name) {
        if (targets_to_build.count(name)) return;
        if (!targets_.count(name)) return;
        targets_to_build.insert(name);
        auto target = targets_[name];

        for (const auto& dep : target->get_resolved_target_deps()) { collect(dep); }

        auto custom = std::dynamic_pointer_cast<CustomTarget>(target);
        if (custom) {
            for (const auto& dep : custom->get_custom_dependencies()) { collect(dep); }
        }

        for (const auto& dep : target->get_manually_added_dependencies()) { collect(dep); }
    };

    for (const auto& name : initial_targets) { collect(name); }

    std::string root_binary_dir = get_variable("CMAKE_BINARY_DIR");
    std::filesystem::create_directories(root_binary_dir);

    BuildGraph graph;
    graph.set_toolchain(&get_toolchain());

    // Build linker flags
    std::vector<std::string> exe_linker_flags;
    std::vector<std::string> shared_linker_flags;

    std::string linker_type = get_variable("CMAKE_LINKER_TYPE");
    if (!linker_type.empty()) {
        if (ci_equals(linker_type, "BFD") || ci_equals(linker_type, "GOLD") || ci_equals(linker_type, "MOLD")
            || ci_equals(linker_type, "LLD")) {
            std::string flag = "-fuse-ld=" + to_lower(linker_type);
            exe_linker_flags.push_back(flag);
            shared_linker_flags.push_back(flag);
        }
    }

    {
        std::istringstream iss(get_variable("CMAKE_EXE_LINKER_FLAGS"));
        std::string flag;
        while (iss >> flag) exe_linker_flags.push_back(flag);
    }
    {
        std::istringstream iss(get_variable("CMAKE_SHARED_LINKER_FLAGS"));
        std::string flag;
        while (iss >> flag) shared_linker_flags.push_back(flag);
    }

    // Process file(GENERATE) entries before task generation so that
    // implicit dependencies are injected into custom_command_rules_ in time.
    {
        auto genex_ctx = GenexEvaluationContext::from_interpreter(*this, targets_);
        process_file_generates(genex_ctx);
        if (fatal_error_) { return std::unexpected(BuildError{fatal_error_->file, fatal_error_->message}); }
    }

    // Generate tasks via transaction(s).
    //
    // We commit between resolver passes because get_missing_dependencies()
    // only sees committed tasks — any deps introduced by tasks added in the
    // current transaction would otherwise be invisible until commit and the
    // build graph would stall on them at execution time.
    {
        // Initial pass: generate tasks for the configured target set.
        auto txn = graph.begin();
        for (const auto& name : targets_to_build) {
            if (auto r =
                    targets_[name]->generate_tasks(txn, get_root()->toolchain_, targets_, *this, exe_linker_flags, shared_linker_flags);
                !r) {
                return std::unexpected(BuildError{current_file_, r.error()});
            }
        }
        auto commit_result = txn.commit();
        if (!commit_result) { return std::unexpected(BuildError{current_file_, commit_result.error()}); }

        std::unordered_map<std::string, std::string> output_to_target;
        for (const auto& [name, target] : targets_) {
            auto path = target->get_output_path();
            if (!path.empty()) output_to_target[path] = name;
            if (auto* ct = dynamic_cast<CustomTarget*>(target.get())) {
                for (const auto& bp : ct->get_byproducts()) { output_to_target[bp] = name; }
            }
        }

        const auto& cc_rules = get_custom_command_rules();
        std::set<std::string> generated_cc_for_resolver;

        bool changed = true;
        while (changed) {
            changed = false;
            auto resolve_txn = graph.begin();
            for (const auto& missing : graph.get_missing_dependencies()) {
                // Check if missing dep is an output file of a known target
                auto it = output_to_target.find(missing);
                if (it != output_to_target.end() && !targets_to_build.count(it->second)) {
                    targets_to_build.insert(it->second);
                    if (auto r = targets_[it->second]->generate_tasks(resolve_txn, get_root()->toolchain_, targets_, *this,
                                                                      exe_linker_flags, shared_linker_flags);
                        !r) {
                        return std::unexpected(BuildError{current_file_, r.error()});
                    }
                    changed = true;
                    continue;
                }
                // Check if missing dep is a target name (e.g. custom targets with no output path)
                auto tgt_it = targets_.find(missing);
                if (tgt_it != targets_.end() && !targets_to_build.count(missing)) {
                    targets_to_build.insert(missing);
                    if (auto r = tgt_it->second->generate_tasks(resolve_txn, get_root()->toolchain_, targets_, *this, exe_linker_flags,
                                                                shared_linker_flags);
                        !r) {
                        return std::unexpected(BuildError{current_file_, r.error()});
                    }
                    changed = true;
                    continue;
                }
                // Check if missing dep is the OUTPUT of a stand-alone add_custom_command
                // not pulled in via any target's source list / DEPENDS.
                auto cc_it = cc_rules.find(missing);
                if (cc_it != cc_rules.end() && !generated_cc_for_resolver.count(missing)) {
                    if (auto r = generate_custom_command_task_for_rule(resolve_txn, *cc_it->second, targets_, cc_rules,
                                                                       generated_cc_for_resolver, get_target_aliases());
                        !r) {
                        return std::unexpected(BuildError{current_file_, r.error()});
                    }
                    changed = true;
                }
            }
            auto rcommit = resolve_txn.commit();
            if (!rcommit) { return std::unexpected(BuildError{current_file_, rcommit.error()}); }
        }
    }

    // Resolve circular deps
    for (auto& [name, target] : targets_) { target->resolve_deferred_circular_deps(targets_); }

    // Wire link dependencies
    for (const auto& name : targets_to_build) {
        auto target = targets_[name];
        if (target->get_type() == TargetType::STATIC_LIBRARY) continue;

        std::string out_path = target->get_output_path();
        std::string task_id = out_path.empty() ? target->get_name() : out_path;

        if (graph.has_task(task_id)) {
            auto& task = graph.get_task(task_id);
            for (const auto& lib : target->get_resolved_property("LINK_LIBRARIES")) {
                if (graph.has_task(lib)) { task.inputs.push_back(lib); }
            }
        }
    }

    // Post-transaction: evaluate genex, resolve inferred file deps
    auto genex_ctx = GenexEvaluationContext::from_interpreter(*this, targets_);

    auto finalize_result = graph.evaluate_genex(genex_ctx);
    if (!finalize_result) { return std::unexpected(BuildError{current_file_, finalize_result.error()}); }
    graph.resolve_inferred_file_deps();

    // Return full graph (not just dirty tasks)
    return std::move(graph);
}

Interpreter::Interpreter(std::string script_dir, std::ostream* out, std::ostream* err, std::optional<std::string> build_dir,
                         bool skip_sys_init, bool skip_cache_load, bool skip_host_compiler_detection)
    : out_(out), err_(err) {

    std::filesystem::path abs_script_dir =
        script_dir.empty() ? std::filesystem::current_path() : std::filesystem::absolute(script_dir).lexically_normal();
    frame_stack_.push_back({intern_file(abs_script_dir.string()), nullptr});

    std::filesystem::path abs_binary_dir;
    if (build_dir.has_value()) {
        build_dir_ = *build_dir;
        abs_binary_dir = std::filesystem::absolute(build_dir_).lexically_normal();
    } else {
        // Script mode (-P): CMake sets CMAKE_CURRENT_BINARY_DIR to cwd
        abs_binary_dir = std::filesystem::current_path();
    }

    // Initialize variables via ShadowMap (depth starts at 0)
    variables_.set("CMAKE_SIZEOF_VOID_P", std::to_string(sizeof(void*)));
    variables_.set("CMAKE_CURRENT_SOURCE_DIR", abs_script_dir.string());
    variables_.set("CMAKE_CURRENT_LIST_DIR", abs_script_dir.string());
    variables_.set("CMAKE_CURRENT_LIST_FILE", (abs_script_dir / "CMakeLists.txt").string()); // Default assumption
    // For the top-level file CMake sets CMAKE_PARENT_LIST_FILE to the same
    // path as CMAKE_CURRENT_LIST_FILE; include() swaps both on entry/exit.
    variables_.set("CMAKE_PARENT_LIST_FILE", (abs_script_dir / "CMakeLists.txt").string());

    variables_.set("KILN_VERSION", std::string(kiln::version()));

    variables_.set("CMAKE_SOURCE_DIR", variables_.get("CMAKE_CURRENT_SOURCE_DIR"));
    variables_.set("CMAKE_BINARY_DIR", abs_binary_dir.string());
    variables_.set("CMAKE_CURRENT_BINARY_DIR", abs_binary_dir.string());
    // Deprecated alias of CMAKE_SOURCE_DIR. Some legacy modules still read it.
    variables_.set("CMAKE_HOME_DIRECTORY", variables_.get("CMAKE_CURRENT_SOURCE_DIR"));
    variables_.set("CMAKE_EXPORT_COMPILE_COMMANDS", "ON");

    // In config mode, create the binary directory at configure time (like CMake does)
    if (build_dir.has_value()) {
        std::error_code ec;
        std::filesystem::create_directories(abs_binary_dir, ec);
    }

    // Set default install prefix if not already set
    if (get_variable("CMAKE_INSTALL_PREFIX").empty()) { variables_.set("CMAKE_INSTALL_PREFIX", "/usr/local"); }

    if (build_dir.has_value() && abs_binary_dir == abs_script_dir) {
        set_fatal_error("Build directory cannot be the same as the source directory: " + abs_script_dir.string());
    }

    // Track the system CMake version so cmake_minimum_required() and
    // cmake_minimum_required(VERSION ...) checks in third-party Modules pass.
    // System modules ship with version gates targeting whatever CMake the
    // user has installed; claiming a stale version trips those checks.
    variables_.set("CMAKE_VERSION", "4.3.2");
    variables_.set("CMAKE_MAJOR_VERSION", "4");
    variables_.set("CMAKE_MINOR_VERSION", "3");
    variables_.set("CMAKE_PATCH_VERSION", "2");

    variables_.set("CMAKE_FILES_DIRECTORY", "/CMakeFiles");

    // Platform flags
#ifdef __unix__
    variables_.set("UNIX", "1");
    variables_.set("CMAKE_HOST_UNIX", "1");
#endif
#ifdef __APPLE__
    variables_.set("APPLE", "1");
    variables_.set("CMAKE_HOST_APPLE", "1");
#endif
#ifdef _WIN32
    variables_.set("WIN32", "1");
    variables_.set("CMAKE_HOST_WIN32", "1");
#endif
#ifdef __linux__
    variables_.set("LINUX", "1");
    variables_.set("CMAKE_HOST_LINUX", "1");
#endif
#ifdef __FreeBSD__
    variables_.set("BSD", "FreeBSD");
#endif

    // find_library and several Find modules consult these to enumerate which
    // filename patterns count as a library on the host. Set sane Unix defaults
    // here so projects that never explicitly configure them still work.
    // The leading empty entry in PREFIXES lets find_library match libraries
    // without a "lib" prefix (e.g. some vendor SDKs).
#ifdef _WIN32
    variables_.set("CMAKE_FIND_LIBRARY_PREFIXES", ";lib");
    variables_.set("CMAKE_FIND_LIBRARY_SUFFIXES", ".lib;.dll.a;.a");
#elif defined(__APPLE__)
    variables_.set("CMAKE_FIND_LIBRARY_PREFIXES", "lib;");
    variables_.set("CMAKE_FIND_LIBRARY_SUFFIXES", ".tbd;.dylib;.so;.a");
#else
    variables_.set("CMAKE_FIND_LIBRARY_PREFIXES", "lib;");
    variables_.set("CMAKE_FIND_LIBRARY_SUFFIXES", ".so;.a");
#endif

    variables_.set("CMAKE_INSTALL_DEFAULT_COMPONENT_NAME", "Unspecified");
    variables_.set("CMAKE_C_OUTPUT_EXTENSION", ".o");
    variables_.set("CMAKE_CXX_OUTPUT_EXTENSION", ".o");
    variables_.set("CMAKE_ASM_OUTPUT_EXTENSION", ".o");

    // Byte order detection
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    variables_.set("CMAKE_C_BYTE_ORDER", "LITTLE_ENDIAN");
    variables_.set("CMAKE_CXX_BYTE_ORDER", "LITTLE_ENDIAN");
#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    variables_.set("CMAKE_C_BYTE_ORDER", "BIG_ENDIAN");
    variables_.set("CMAKE_CXX_BYTE_ORDER", "BIG_ENDIAN");
#endif

    variables_.set("CMAKE_EXECUTABLE_SUFFIX", "");
    variables_.set("CMAKE_SHARED_LIBRARY_PREFIX", "lib");
    variables_.set("CMAKE_SHARED_LIBRARY_SUFFIX", ".so");
    variables_.set("CMAKE_SHARED_MODULE_PREFIX", "");
    variables_.set("CMAKE_SHARED_MODULE_SUFFIX", ".so");
    variables_.set("CMAKE_STATIC_LIBRARY_PREFIX", "lib");
    variables_.set("CMAKE_STATIC_LIBRARY_SUFFIX", ".a");
    variables_.set("CMAKE_SHARED_LIBRARY_C_FLAGS", "-fPIC");
    variables_.set("CMAKE_SHARED_LIBRARY_CXX_FLAGS", "-fPIC");
#ifdef __linux__
    variables_.set("CMAKE_DL_LIBS", "dl");
#endif

    variables_.set("CMAKE_COMMAND", get_executable_path());
    variables_.set("CMAKE_GENERATOR", "kiln");
    variables_.set("CMAKE_AUTOMOC_MACRO_NAMES", "Q_OBJECT;Q_GADGET;Q_NAMESPACE;Q_NAMESPACE_EXPORT");
    variables_.set("CMAKE_MAKE_PROGRAM", get_executable_path());
    {
        auto& extra = cmake_extra_modules_root();
        variables_.set("CMAKE_ROOT", extra.empty() ? "/usr/share/cmake" : extra);
    }

    // Initialize cache store early so compiler detection can use it
    std::filesystem::path cache_path = std::filesystem::path(abs_binary_dir) / ".kiln_subsystem_cache.json";
    cache_store_ = std::make_unique<CacheStore>(cache_path);
    if (!skip_cache_load) {
        ProfileScope cache_profile("loading persistent cache", "init");
        auto cache_load_res = cache_store_->load(); // Graceful - starts with empty cache if file doesn't exist
        (void) cache_load_res;
    }

    // Initialize toolchain with compiler detection
    // See fake_cmake_compiler_checks_and_init() for what this does and its limitations
    // NOTE: This function directly modifies variables via set_variable(), which now uses ShadowMap
    //
    // skip_host_compiler_detection short-circuits when the caller knows a
    // toolchain file or -DCMAKE_<LANG>_COMPILER override will redirect us;
    // the host detection would just be wasted subprocess work. Lazy on-demand
    // detection inside enable_compiler_for_language picks up the slack and
    // also seeds CMAKE_HOST_SYSTEM_NAME/_PROCESSOR via uname() below.
    if (!skip_sys_init && !skip_host_compiler_detection) {
        ProfileScope compiler_profile("compiler detection", "init");
        fake_cmake_compiler_checks_and_init(*this, *cache_store_);
    } else if (!skip_sys_init) {
        // Still seed the cheap host-info vars (no subprocesses) so that
        // CMakeLists / toolchain files can read CMAKE_HOST_SYSTEM_NAME etc.
        // before any enable_language runs.
#ifdef __unix__
        struct utsname uname_info;
        if (uname(&uname_info) == 0) {
            set_variable("CMAKE_HOST_SYSTEM_NAME", uname_info.sysname);
            set_variable("CMAKE_HOST_SYSTEM_PROCESSOR", uname_info.machine);
        }
#endif
    }

    // Host system version + composite name strings. uname is cheap; the
    // compiler-detection backup populates NAME/PROCESSOR but never .release,
    // so populate the *_VERSION and *_SYSTEM vars unconditionally here.
#ifdef __unix__
    if (!skip_sys_init) {
        struct utsname uname_info;
        if (uname(&uname_info) == 0) {
            set_variable("CMAKE_HOST_SYSTEM_VERSION", uname_info.release);
            std::string host_system = std::string(uname_info.sysname) + "-" + uname_info.release;
            set_variable("CMAKE_HOST_SYSTEM", host_system);
            // CMAKE_SYSTEM_VERSION / CMAKE_SYSTEM mirror host until a toolchain
            // file overrides them (cross-compile). Match CMake: the bare
            // CMAKE_SYSTEM is "<name>-<version>".
            if (get_variable("CMAKE_SYSTEM_VERSION").empty()) set_variable("CMAKE_SYSTEM_VERSION", uname_info.release);
            if (get_variable("CMAKE_SYSTEM").empty()) set_variable("CMAKE_SYSTEM", host_system);
        }
    }
#endif

    // Debian/Ubuntu multiarch tuple (e.g. x86_64-linux-gnu). find_library and
    // pkg-config search lib/${CMAKE_LIBRARY_ARCHITECTURE}; without it system
    // libs on multiarch distros are missed. gcc/clang both implement this;
    // empty output (Arch/Fedora/non-multiarch) leaves the var unset.
    if (!skip_sys_init) {
        std::string arch = detail::run_command("gcc -print-multiarch 2>/dev/null");
        while (!arch.empty() && (arch.back() == '\n' || arch.back() == '\r' || arch.back() == ' ')) arch.pop_back();
        if (!arch.empty()) { set_variable("CMAKE_LIBRARY_ARCHITECTURE", arch); }
    }

    // Initialize built-in global properties
    // ENABLED_LANGUAGES starts empty; project()/enable_language() populates it
    global_properties_["ENABLED_LANGUAGES"] = "";
    global_properties_["GENERATOR_IS_MULTI_CONFIG"] = "FALSE";
    global_properties_["TARGET_SUPPORTS_SHARED_LIBS"] = "TRUE";
    global_properties_["CMAKE_ROLE"] = "PROJECT";
    global_properties_["FIND_LIBRARY_USE_LIB64_PATHS"] = "TRUE";
    global_properties_["FIND_LIBRARY_USE_LIB32_PATHS"] = "FALSE";

    // Some CMake defaults
    variables_.set("CMAKE_INSTALL_BINDIR", "bin");
    variables_.set("CMAKE_INSTALL_LIBDIR", "lib");
    variables_.set("CMAKE_INSTALL_INCLUDEDIR", "include");

    // Build CMAKE_SYSTEM_PREFIX_PATH (mirrors CMake's UnixPaths.cmake)
    // Standard system prefixes first
    std::string system_prefix_path = "/usr/local;/usr;/";

    // Append install prefix and staging prefix (unless CMAKE_FIND_NO_INSTALL_PREFIX)
    std::string install_prefix = get_variable("CMAKE_INSTALL_PREFIX");
    std::string staging_prefix = get_variable("CMAKE_STAGING_PREFIX");
    if (get_variable("CMAKE_FIND_NO_INSTALL_PREFIX") != "1" && get_variable("CMAKE_FIND_NO_INSTALL_PREFIX") != "ON"
        && get_variable("CMAKE_FIND_NO_INSTALL_PREFIX") != "TRUE") {
        if (!install_prefix.empty()) { system_prefix_path += ";" + install_prefix; }
        if (!staging_prefix.empty()) { system_prefix_path += ";" + staging_prefix; }
    }
    variables_.set("CMAKE_SYSTEM_PREFIX_PATH", system_prefix_path);

    // Record install prefix snapshot (_cmake_record_install_prefix equivalent)
    // This snapshots which occurrence of the install/staging prefix in
    // CMAKE_SYSTEM_PREFIX_PATH was added because it IS the install prefix,
    // so find_package() can later remove/add it precisely.
    {
        variables_.set("_CMAKE_SYSTEM_PREFIX_PATH_INSTALL_PREFIX_VALUE", install_prefix);
        variables_.set("_CMAKE_SYSTEM_PREFIX_PATH_STAGING_PREFIX_VALUE", staging_prefix);

        int icount = 0, scount = 0;
        CMakeArrayIterator path_view(system_prefix_path);
        for (auto entry : path_view) {
            if (!install_prefix.empty() && entry == install_prefix) ++icount;
            if (!staging_prefix.empty() && entry == staging_prefix) ++scount;
        }
        variables_.set("_CMAKE_SYSTEM_PREFIX_PATH_INSTALL_PREFIX_COUNT", std::to_string(icount));
        variables_.set("_CMAKE_SYSTEM_PREFIX_PATH_STAGING_PREFIX_COUNT", std::to_string(scount));
    }

    // Append non-standard but common prefixes (after recording, matching CMake's order)
    system_prefix_path += ";/usr/X11R6;/usr/pkg;/opt";
    variables_.set("CMAKE_SYSTEM_PREFIX_PATH", system_prefix_path);

    // CMAKE_SYSTEM_LIBRARY_PATH and CMAKE_SYSTEM_INCLUDE_PATH
    variables_.set("CMAKE_SYSTEM_INCLUDE_PATH", "/usr/include/X11");
    variables_.set("CMAKE_SYSTEM_LIBRARY_PATH", "/usr/lib/X11");
    variables_.set("CMAKE_PLATFORM_IMPLICIT_LINK_DIRECTORIES", "/lib;/lib32;/lib64;/usr/lib;/usr/lib32;/usr/lib64");

    // Initialize root directory context
    push_directory(abs_script_dir.string(), abs_binary_dir.string());

    // Register non-internal builtins
    register_message_builtins(*this);
    register_variable_builtins(*this);
    register_list_builtins(*this);
    register_target_builtins(*this);
    register_project_builtins(*this);
    register_file_builtins(*this);
    register_find_commands_builtins(*this);
    register_math_builtins(*this);
    register_string_builtins(*this);
    register_process_builtins(*this);
    register_property_builtins(*this);
    register_try_compile_builtins(*this);
    register_path_builtins(*this);
    register_install_builtins(*this);
    register_source_properties_builtins(*this);
    register_system_info_builtins(*this);

    // CMake compatibility: _cmake_record_install_prefix() re-snapshots
    // the install/staging prefix position in CMAKE_SYSTEM_PREFIX_PATH.
    // Called by platform modules after modifying CMAKE_SYSTEM_PREFIX_PATH.
    add_builtin("_cmake_record_install_prefix", [](Interpreter& interp, const std::vector<std::string>&) {
        std::string install_prefix = interp.get_variable("CMAKE_INSTALL_PREFIX");
        std::string staging_prefix = interp.get_variable("CMAKE_STAGING_PREFIX");
        std::string sys_prefix_path = interp.get_variable("CMAKE_SYSTEM_PREFIX_PATH");

        interp.set_variable("_CMAKE_SYSTEM_PREFIX_PATH_INSTALL_PREFIX_VALUE", install_prefix);
        interp.set_variable("_CMAKE_SYSTEM_PREFIX_PATH_STAGING_PREFIX_VALUE", staging_prefix);

        int icount = 0, scount = 0;
        CMakeArrayIterator path_view(sys_prefix_path);
        for (auto entry : path_view) {
            if (!install_prefix.empty() && entry == install_prefix) ++icount;
            if (!staging_prefix.empty() && entry == staging_prefix) ++scount;
        }
        interp.set_variable("_CMAKE_SYSTEM_PREFIX_PATH_INSTALL_PREFIX_COUNT", std::to_string(icount));
        interp.set_variable("_CMAKE_SYSTEM_PREFIX_PATH_STAGING_PREFIX_COUNT", std::to_string(scount));
    });

    add_builtin("enable_testing", [](Interpreter& interp, const std::vector<std::string>& args) {
        if (!args.empty()) { interp.print_message("WARN", "enable_testing() expects no arguments"); }
        interp.set_variable("BUILD_TESTING", "ON");
        interp.mark_project_enabled_testing();
    });

    add_builtin("add_test", [](Interpreter& interp, const std::vector<std::string>& args) {
        if (args.empty()) {
            interp.set_fatal_error("add_test requires arguments");
            return;
        }

        std::string name;
        std::vector<std::string> raw_cmd;
        std::string working_dir;

        // Detect legacy form: add_test(testname command [arg...])
        // Legacy form is used when the first argument is NOT the "NAME" keyword
        if (args[0] != "NAME") {
            if (args.size() < 2) {
                interp.set_fatal_error("add_test requires at least a test name and command");
                return;
            }
            name = args[0];
            raw_cmd.assign(args.begin() + 1, args.end());
        } else {
            // NAME form: add_test(NAME <name> COMMAND <cmd> [args...]
            //             [CONFIGURATIONS <config>...]
            //             [WORKING_DIRECTORY <dir>]
            //             [COMMAND_EXPAND_LISTS])
            CommandParser parser("add_test");
            std::vector<std::string> configurations;
            bool command_expand_lists = false;

            parser.value("NAME", name);
            parser.list("COMMAND", raw_cmd);
            parser.value("WORKING_DIRECTORY", working_dir);
            parser.list("CONFIGURATIONS", configurations);
            parser.flag("COMMAND_EXPAND_LISTS", command_expand_lists);

            auto parse_res = parser.parse(args);
            if (!parse_res) {
                interp.set_fatal_error(parse_res.error());
                return;
            }
            for (const auto& w : *parse_res) interp.print_message("WARNING", w);

            if (name.empty()) {
                interp.set_fatal_error("add_test NAME requires a non-empty test name");
                return;
            }

            if (raw_cmd.empty()) {
                interp.set_fatal_error("add_test requires COMMAND");
                return;
            }

            // Filter by CONFIGURATIONS if specified
            if (!configurations.empty()) {
                auto build_type = interp.get_variable("CMAKE_BUILD_TYPE");
                bool config_match = false;
                for (const auto& config : configurations) {
                    // Case-insensitive comparison (CMake behavior)
                    std::string config_lower = to_lower(config);
                    std::string build_type_lower = to_lower(build_type);
                    if (config_lower == build_type_lower) {
                        config_match = true;
                        break;
                    }
                }
                if (!config_match) {
                    return; // Skip test — not for this configuration
                }
            }
        }

        TestDefinition test;
        test.name = name;
        test.command = raw_cmd[0];
        for (size_t i = 1; i < raw_cmd.size(); ++i) { test.args.push_back(raw_cmd[i]); }
        test.working_dir = working_dir.empty() ? interp.get_variable("CMAKE_CURRENT_BINARY_DIR") : working_dir;

        interp.get_tests().push_back(std::move(test));
    });

    add_builtin("set_tests_properties", [](Interpreter& interp, const std::vector<std::string>& args) {
        // Known test properties (stored silently; unrecognized ones still stored but warn)
        static const std::unordered_set<std::string> KNOWN_PROPERTIES = {
            "TIMEOUT",
            "SKIP_RETURN_CODE",
            "DEPENDS",
            "WORKING_DIRECTORY",
            "SKIP_REGULAR_EXPRESSION",
            "WILL_FAIL",
            "PASS_REGULAR_EXPRESSION",
            "FAIL_REGULAR_EXPRESSION",
            "ENVIRONMENT",
            "ENVIRONMENT_MODIFICATION",
            "LABELS",
            "FIXTURES_SETUP",
            "FIXTURES_CLEANUP",
            "FIXTURES_REQUIRED",
            "DISABLED",
            "COST",
            "PROCESSORS",
            "RESOURCE_LOCK",
            "RUN_SERIAL",
            "MEASUREMENT",
            "ATTACHED_FILES",
            "ATTACHED_FILES_ON_FAIL",
            "REQUIRED_FILES",
            // Metadata-only properties (no runtime behavior needed)
            "DEF_SOURCE_LINE",
            "_BACKTRACE_TRIPLES",
        };

        // Parse: set_tests_properties(test1 [test2...] PROPERTIES prop1 val1 [prop2 val2...])
        // CMake quietly accepts an empty test-name list (e.g. when the
        // caller's ${TEST_NAME} expands to nothing) and treats the whole
        // call as a no-op. Match that — projects in the wild rely on it.
        auto props_it = std::find(args.begin(), args.end(), "PROPERTIES");
        if (props_it == args.end()) {
            interp.set_fatal_error("set_tests_properties requires PROPERTIES keyword");
            return;
        }

        // Extract test names (everything before PROPERTIES)
        std::vector<std::string> test_names(args.begin(), props_it);
        if (test_names.empty()) {
            return; // No-op, matching CMake.
        }

        // Extract properties (everything after PROPERTIES)
        std::vector<std::string> prop_args(props_it + 1, args.end());
        if (prop_args.size() % 2 != 0) {
            interp.set_fatal_error("set_tests_properties PROPERTIES must have pairs of property names and values");
            return;
        }

        // Parse properties into map
        std::map<std::string, std::string> properties;
        for (size_t i = 0; i < prop_args.size(); i += 2) {
            std::string prop_name = prop_args[i];
            std::string prop_value = prop_args[i + 1];

            // Warn if property is completely unrecognized (but still set it)
            if (KNOWN_PROPERTIES.find(prop_name) == KNOWN_PROPERTIES.end()) {
                interp.print_message("WARN", "Unknown test property '" + prop_name + "'");
            }

            properties[prop_name] = prop_value;
        }

        // Apply properties to all named tests
        auto& tests = interp.get_tests();
        for (const auto& test_name : test_names) {
            bool found = false;
            for (auto& test : tests) {
                if (test.name == test_name) {
                    // Merge properties (allowing multiple set_tests_properties calls)
                    for (const auto& [key, value] : properties) { test.properties[key] = value; }
                    found = true;
                    break;
                }
            }

            if (!found) { interp.print_message("WARN", "Test '" + test_name + "' not found, cannot set properties"); }
        }
    });

    register_find_package_builtins(*this);

    add_builtin("include_guard", [](Interpreter& interp, const std::vector<std::string>& args) {
        std::string scope = "DIRECTORY";
        if (!args.empty()) scope = args[0];

        std::string current_file = interp.get_current_file();
        if (current_file.empty()) return;

        if (scope == "GLOBAL") {
            // If already guarded, trigger early return (skip rest of file)
            if (interp.global_guarded_files_.contains(current_file)) {
                interp.request_return();
                return;
            }
            interp.global_guarded_files_.insert(current_file);
        } else {
            if (interp.get_current_directory_context().guarded_files.contains(current_file)) {
                interp.request_return();
                return;
            }
            interp.get_current_directory_context().guarded_files.insert(current_file);
        }
    });

    // Internal builtins (interact with interpreter state/stack)

    add_builtin("add_subdirectory", [](Interpreter& interp, const std::vector<std::string>& args) {
        if (args.empty()) return;
        std::string subdir = args[0];
        std::string current_source_dir = interp.get_variable("CMAKE_CURRENT_SOURCE_DIR");
        std::string current_binary_dir = interp.get_variable("CMAKE_CURRENT_BINARY_DIR");
        std::string source_path_str = Path::join(current_source_dir, subdir);
        std::string abs_source_str = std::filesystem::absolute(std::filesystem::path(source_path_str)).string();
        abs_source_str = Path(abs_source_str).lexically_normal().str();
        std::string cmake_file_str = Path::join(abs_source_str, "CMakeLists.txt");

        // Compute binary directory for the subdirectory
        // Check for explicit binary_dir argument (args[1]), skipping EXCLUDE_FROM_ALL
        std::string binary_path_str;
        bool has_explicit_binary_dir = false;
        for (size_t i = 1; i < args.size(); ++i) {
            if (args[i] != "EXCLUDE_FROM_ALL") {
                binary_path_str = Path::join(current_binary_dir, args[i]);
                has_explicit_binary_dir = true;
                break;
            }
        }
        if (!has_explicit_binary_dir) {
            if (Path(subdir).is_absolute()) {
                // For absolute source paths without explicit binary dir,
                // use the last component as subdirectory name under current binary dir
                binary_path_str = Path::join(current_binary_dir, Path(subdir).filename());
            } else {
                binary_path_str = Path::join(current_binary_dir, subdir);
            }
        }

        // Create binary directory (CMake does this implicitly)
        std::error_code ec;
        std::filesystem::create_directories(binary_path_str, ec);

        if (!interp.cached_file_exists(cmake_file_str)) {
            interp.set_fatal_error("CMakeLists.txt not found in " + abs_source_str);
            return;
        }

        std::ifstream file(cmake_file_str);
        if (!file) {
            interp.set_fatal_error("Could not read " + cmake_file_str);
            return;
        }
        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

        // Save current file for restoration
        std::string saved_file = interp.get_current_file();

        // Push variable scope and directory context
        interp.get_variables().push_scope();
        interp.push_directory(abs_source_str, binary_path_str);

        // Set CMAKE_CURRENT_* variables for the subdirectory
        interp.set_variable("CMAKE_CURRENT_SOURCE_DIR", abs_source_str);
        interp.set_variable("CMAKE_CURRENT_BINARY_DIR", binary_path_str);
        interp.set_variable("CMAKE_CURRENT_LIST_FILE", cmake_file_str);
        interp.set_variable("CMAKE_CURRENT_LIST_DIR", abs_source_str);
        interp.set_current_file(cmake_file_str);

        // Parse and interpret the subdirectory CMakeLists.txt
        std::string subdir_filename = std::string(Path(cmake_file_str).filename());

        ProfileScope parse_profile("parse " + subdir + "/" + subdir_filename, "parse");
        Parser parser(content, cmake_file_str);
        auto ast = parser.parse();
        parse_profile.stop();

        if (interp.get_debugger()) interp.get_debugger()->push_call_depth();

        // Auto-scope policies around subdirectories (CMake behavior)
        interp.push_policies();

        {
            // return() in subdirectory shouldn't propagate to parent
            ReturnGuard rg(interp);

            if (ast) {
                ProfileScope interpret_profile("interpret " + subdir + "/" + subdir_filename, "interpret");
                auto res = interp.interpret(ast.value());
                interpret_profile.stop();
                if (!res) {
                    interp.set_fatal_error(res.error());
                } else {
                    interp.execute_deferred_calls();
                    interp.finalize_directory_targets(); // Apply retroactive properties
                }
            } else {
                interp.set_fatal_error(InterpreterError{
                    cmake_file_str, ast.error().row, ast.error().col, ast.error().offset, ast.error().length, ast.error().reason, {}});
            }
        }

        interp.pop_policies();

        if (interp.get_debugger()) interp.get_debugger()->pop_call_depth();

        // Pop directory context and variable scope
        interp.pop_directory();
        interp.get_variables().pop_scope();

        // Restore current file
        interp.set_current_file(saved_file);
    });

    // Deprecated command - just calls add_subdirectory for each directory
    add_builtin("subdirs", [](Interpreter& interp, const std::vector<std::string>& args) {
        CommandParser parser("subdirs");
        std::vector<std::string> dirs;
        std::vector<std::string> exclude_dirs;
        bool preorder = false;

        parser.positionals(dirs, "directories");
        parser.list("EXCLUDE_FROM_ALL", exclude_dirs);
        parser.flag("PREORDER", preorder);

        PARSE_OR_RETURN(parser, interp, args);

        // Process normal directories
        for (const auto& dir : dirs) {
            auto res = interp.execute_command_with_args("add_subdirectory", {dir});
            if (!res) return;
        }

        // Process EXCLUDE_FROM_ALL directories
        for (const auto& dir : exclude_dirs) {
            auto res = interp.execute_command_with_args("add_subdirectory", {dir, dir, "EXCLUDE_FROM_ALL"});
            if (!res) return;
        }
    });

    add_builtin("include", [](Interpreter& interp, const std::vector<std::string>& args) {
        if (args.empty()) {
            interp.set_fatal_error("include() requires an argument");
            return;
        }
        bool optional = false;
        for (size_t i = 1; i < args.size(); ++i)
            if (args[i] == "OPTIONAL") optional = true;

        auto res = interp.include_file(args[0], optional);
        if (!res) interp.set_fatal_error(res.error());
    });

    add_builtin("break", [](Interpreter& interp, const std::vector<std::string>&) {
        if (interp.get_loop_depth() == 0) {
            interp.set_fatal_error("break() can only be called inside a loop");
            return;
        }
        interp.set_loop_control(Interpreter::LoopControl::BREAK);
    });

    add_builtin("continue", [](Interpreter& interp, const std::vector<std::string>&) {
        if (interp.get_loop_depth() == 0) {
            interp.set_fatal_error("continue() can only be called inside a loop");
            return;
        }
        interp.set_loop_control(Interpreter::LoopControl::CONTINUE);
    });

    add_builtin("return", [](Interpreter& interp, const std::vector<std::string>& args) {
        // CMake 3.25+ supports return(PROPAGATE var1 var2 ...)
        // which propagates variables to the parent scope before returning
        if (!args.empty() && args[0] == "PROPAGATE") {
            for (size_t i = 1; i < args.size(); ++i) {
                const std::string& var_name = args[i];
                auto value = interp.get_optional_variable(var_name);
                auto result = value.has_value() ? interp.get_variables().set_parent_scope(var_name, *value)
                                                : interp.get_variables().unset_parent_scope(var_name);
                if (!result) {
                    interp.set_fatal_error("return(PROPAGATE " + var_name + "): " + result.error());
                    return;
                }
            }
        }
        interp.request_return();
    });

    add_builtin("cmake_language", [](Interpreter& interp, const std::vector<std::string>& args) {
        if (args.empty()) {
            interp.set_fatal_error("cmake_language requires arguments");
            return;
        }

        if (args[0] == "EVAL") {
            if (args.size() < 3 || args[1] != "CODE") {
                interp.set_fatal_error("cmake_language(EVAL) requires CODE keyword and code arguments");
                return;
            }
            std::string code;
            for (size_t i = 2; i < args.size(); ++i) {
                if (i > 2) code += ' ';
                code += args[i];
            }

            Parser p(code);
            auto ast = p.parse();
            if (!ast) {
                std::vector<CallLocation> backtrace;
                Interpreter* root = interp.get_root();
                if (!root->trace_stack_.empty()) {
                    // For parse errors during EVAL, the current command (cmake_language)
                    // should be part of the backtrace. Convert TraceEntry → CallLocation.
                    backtrace.reserve(root->trace_stack_.size());
                    for (const auto& te : root->trace_stack_) {
                        backtrace.push_back(
                            {te.file ? *te.file : std::string(), te.row, te.col, te.offset, te.length, std::string(te.command)});
                    }
                }
                InterpreterError err{"<EVAL>",           ast.error().row,
                                     ast.error().col,    ast.error().offset,
                                     ast.error().length, "cmake_language(EVAL) parse error: " + ast.error().reason,
                                     backtrace,          code};
                interp.set_fatal_error(err);
                return;
            }

            std::string old_file = interp.get_current_file();
            interp.set_current_file("<EVAL>");
            auto res = interp.interpret(ast.value());
            interp.set_current_file(old_file);

            if (!res) {
                InterpreterError err = res.error();
                if (err.file == "<EVAL>" && !err.source_content) { err.source_content = code; }
                interp.set_fatal_error(err);
            }

        } else if (args[0] == "CALL") {
            if (args.size() < 2) {
                interp.set_fatal_error("cmake_language(CALL) requires a command name");
                return;
            }
            std::string cmd = args[1];
            std::vector<std::string> cmd_args;
            if (args.size() > 2) { cmd_args.assign(args.begin() + 2, args.end()); }

            auto res = interp.execute_command_with_args(cmd, cmd_args);
            if (!res) interp.set_fatal_error(res.error());
        } else if (args[0] == "DEFER") {
            // cmake_language(DEFER [DIRECTORY <dir>] [ID <id>] [ID_VAR <var>] CALL <cmd> [<args>...])
            // cmake_language(DEFER [DIRECTORY <dir>] CANCEL_CALL <id>...)
            // cmake_language(DEFER [DIRECTORY <dir>] GET_CALL_IDS <var>)
            // cmake_language(DEFER [DIRECTORY <dir>] GET_CALL <id> <var>)
            if (args.size() < 2) {
                interp.set_fatal_error("cmake_language(DEFER) requires additional arguments");
                return;
            }

            size_t i = 1;
            DirectoryContext* target_ctx = nullptr;
            std::string user_id;
            std::string id_var;

            // Parse optional DIRECTORY, ID, ID_VAR before the subcommand
            while (i < args.size()) {
                if (args[i] == "DIRECTORY") {
                    if (i + 1 >= args.size()) {
                        interp.set_fatal_error("cmake_language(DEFER DIRECTORY) missing directory argument");
                        return;
                    }
                    auto* ctx = interp.get_root()->get_directory_context(args[i + 1]);
                    if (!ctx) {
                        interp.set_fatal_error("cmake_language(DEFER): directory not known: " + args[i + 1]);
                        return;
                    }
                    target_ctx = ctx;
                    i += 2;
                } else if (args[i] == "ID") {
                    if (i + 1 >= args.size()) {
                        interp.set_fatal_error("cmake_language(DEFER ID) missing id argument");
                        return;
                    }
                    user_id = args[i + 1];
                    i += 2;
                } else if (args[i] == "ID_VAR") {
                    if (i + 1 >= args.size()) {
                        interp.set_fatal_error("cmake_language(DEFER ID_VAR) missing variable argument");
                        return;
                    }
                    id_var = args[i + 1];
                    i += 2;
                } else {
                    break; // Must be a subcommand
                }
            }

            if (!target_ctx) { target_ctx = &interp.get_root()->get_current_directory_context(); }

            if (i >= args.size()) {
                interp.set_fatal_error("cmake_language(DEFER) missing subcommand (CALL, CANCEL_CALL, GET_CALL_IDS, GET_CALL)");
                return;
            }

            const std::string& subcmd = args[i];
            if (subcmd == "CALL") {
                if (i + 1 >= args.size()) {
                    interp.set_fatal_error("cmake_language(DEFER CALL) requires a command name");
                    return;
                }
                std::string call_id;
                if (!user_id.empty()) {
                    call_id = user_id;
                } else {
                    call_id = "_" + std::to_string(target_ctx->next_deferred_id++);
                }
                if (!id_var.empty()) { interp.set_variable(id_var, call_id); }
                DeferredCall dc;
                dc.id = call_id;
                dc.command = args[i + 1];
                dc.arguments.assign(args.begin() + i + 2, args.end());
                target_ctx->deferred_calls.push_back(std::move(dc));
            } else if (subcmd == "CANCEL_CALL") {
                std::set<std::string> ids_to_cancel(args.begin() + i + 1, args.end());
                auto& calls = target_ctx->deferred_calls;
                std::erase_if(calls, [&](const DeferredCall& dc) { return ids_to_cancel.count(dc.id) > 0; });
            } else if (subcmd == "GET_CALL_IDS") {
                if (i + 1 >= args.size()) {
                    interp.set_fatal_error("cmake_language(DEFER GET_CALL_IDS) requires a variable name");
                    return;
                }
                std::string result;
                for (size_t j = 0; j < target_ctx->deferred_calls.size(); ++j) {
                    if (j > 0) result += ';';
                    result += target_ctx->deferred_calls[j].id;
                }
                interp.set_variable(args[i + 1], result);
            } else if (subcmd == "GET_CALL") {
                if (i + 2 >= args.size()) {
                    interp.set_fatal_error("cmake_language(DEFER GET_CALL) requires an id and a variable name");
                    return;
                }
                const std::string& search_id = args[i + 1];
                const std::string& var_name = args[i + 2];
                std::string result;
                for (const auto& dc : target_ctx->deferred_calls) {
                    if (dc.id == search_id) {
                        result = dc.command;
                        for (const auto& a : dc.arguments) {
                            result += ';';
                            result += a;
                        }
                        break;
                    }
                }
                interp.set_variable(var_name, result);
            } else {
                interp.set_fatal_error("cmake_language(DEFER): unknown subcommand: " + subcmd);
            }
        } else if (args[0] == "GET_MESSAGE_LOG_LEVEL") {
            if (args.size() < 2) {
                interp.set_fatal_error("cmake_language(GET_MESSAGE_LOG_LEVEL) requires a variable name");
                return;
            }
            std::string level = interp.get_variable("CMAKE_MESSAGE_LOG_LEVEL");
            interp.set_variable(args[1], level.empty() ? "STATUS" : level);
        } else {
            interp.set_fatal_error("Unknown cmake_language mode: " + args[0]);
        }
    });
}

std::expected<void, InterpreterError> Interpreter::interpret(const std::vector<AstNode>& ast) {
    for (const auto& node : ast) {
        if (g_interrupted.load(std::memory_order_relaxed)) {
            return std::unexpected(InterpreterError{"", 0, 0, 0, 0, "Interrupted", {}, std::nullopt});
        }
        auto res = std::visit(
            [this](const auto& n) -> std::expected<void, InterpreterError> {
                using T = std::decay_t<decltype(n)>;
                if constexpr (std::is_same_v<T, CommandInvocation>)
                    return execute_command(n);
                else if constexpr (std::is_same_v<T, IfBlock>)
                    return execute_if_block(n);
                else if constexpr (std::is_same_v<T, FunctionBlock>)
                    return execute_function_block(n);
                else if constexpr (std::is_same_v<T, MacroBlock>)
                    return execute_macro_block(n);
                else if constexpr (std::is_same_v<T, ForeachBlock>)
                    return execute_foreach_block(n);
                else if constexpr (std::is_same_v<T, WhileBlock>)
                    return execute_while_block(n);
                else if constexpr (std::is_same_v<T, BlockBlock>)
                    return execute_block_block(n);
            },
            node);

        if (!res) return res;
        if (loop_control_ != LoopControl::NONE) return {};
        if (return_requested_) return {}; // Early return from script/function
    }
    return {};
}

std::expected<void, InterpreterError> Interpreter::include_file(const std::string& file_path, bool optional) {
    Path path(file_path);

    // CMake's include() resolves a bare module name (e.g. "ExternalProject")
    // to one of the *.cmake files under CMAKE_MODULE_PATH or the system
    // module dir. Across that resolution the same module surfaces as the
    // bare name, "<dir>/Foo.cmake", or "<dir>/Foo" (no extension). This
    // helper hides the three forms behind a single "is this module Foo?"
    // check so the bespoke matching for each module doesn't drift.
    auto is_module = [&file_path](std::string_view name) {
        if (file_path == name) return true;
        std::string suffix_cmake = "/" + std::string(name) + ".cmake";
        std::string suffix_bare = "/" + std::string(name);
        if (file_path.ends_with(std::string(name) + ".cmake")) return true;
        if (file_path.ends_with(suffix_bare)) return true;
        return false;
    };

    if (is_module("CPack")) {
        print_message("WARNING", "kiln does not support cpack (yet). Ignoring..");
        return {};
    }

    if (is_module("ExternalProject")) {
        register_external_project_builtins(*this);
        return {};
    }

    if (is_module("FetchContent")) {
        register_fetch_content_builtins(*this);
        return {};
    }

    // Helper to check if a path exists, trying with and without .cmake extension
    auto try_path = [this](std::string_view p) -> std::optional<std::string> {
        if (cached_file_exists(p)) {
            if (!cached_is_directory(p)) return std::string(p);
        }
        if (!Path(p).has_extension()) {
            std::string with_ext = Path(p).replace_extension(".cmake").str();
            if (cached_file_exists(with_ext)) {
                if (!cached_is_directory(with_ext)) return with_ext;
            }
        }
        return std::nullopt;
    };

    auto find_in_dir = [&try_path](std::string_view dir, const std::string& fp) -> std::optional<std::string> {
        return try_path(Path::join(dir, fp));
    };

    std::optional<std::string> found_path;

    if (path.is_absolute()) {
        found_path = try_path(file_path);
    } else {
        // Try relative to CMAKE_CURRENT_SOURCE_DIR first
        found_path = try_path(Path::join(get_variable("CMAKE_CURRENT_SOURCE_DIR"), file_path));

        // If not found, search CMAKE_MODULE_PATH
        if (!found_path) {
            std::string module_path = get_variable("CMAKE_MODULE_PATH");
            for (auto dir : CMakeArrayIterator(module_path)) {
                if (!dir.empty()) {
                    found_path = find_in_dir(dir, file_path);
                    if (found_path) break;
                }
            }
        }

        // If not found, search system paths
        if (!found_path) {
            std::vector<std::string> sys_module_dirs = {
                "/usr/share/cmake/Modules",
                "/usr/local/share/cmake/Modules",
                "/usr/lib/cmake/Modules",
            };
            if (auto& extra = cmake_extra_modules_root(); !extra.empty()) sys_module_dirs.push_back(extra + "/Modules");
            if (auto& triplet = gnu_arch_triplet(); !triplet.empty()) {
                sys_module_dirs.push_back("/usr/lib/" + triplet + "/cmake/Modules");
            }
            for (const auto& dir : sys_module_dirs) {
                found_path = find_in_dir(dir, file_path);
                if (found_path) break;
            }
        }
    }

    if (!found_path) {
        if (optional) return {};
        return std::unexpected(
            InterpreterError{current_file_, current_cmd_row_, current_cmd_col_, 0, 0, "include() could not find: " + file_path, {}});
    }

    const std::string& resolved_path = *found_path;

    // Check if it's a directory (reject directories, but accept symlinks to files)
    if (cached_is_directory(resolved_path)) {
        if (optional) return {};
        return std::unexpected(InterpreterError{
            current_file_, current_cmd_row_, current_cmd_col_, 0, 0, "include() path is a directory: " + resolved_path, {}});
    }

    std::string abs_path = std::filesystem::absolute(resolved_path).string();

    // Note: include guards are NOT checked here. CMake executes code before
    // include_guard() on every inclusion; include_guard() itself triggers an
    // early return. We must enter the file so pre-guard code runs.

    // Check AST cache for frequently-included system modules
    const std::vector<AstNode>* cached_ast = ast_cache_.get(abs_path);
    const std::vector<AstNode>* ast_to_use = nullptr;
    std::expected<std::vector<AstNode>, ParseError> parsed_ast;

    if (cached_ast) {
        ast_to_use = cached_ast;
    } else {
        std::ifstream file(abs_path);
        if (!file) { return std::unexpected(InterpreterError{abs_path, 0, 0, 0, 0, "include() could not read: " + abs_path, {}}); }

        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

        ProfileScope parse_profile("parse " + abs_path, "parse");
        Parser parser(content, abs_path);
        parsed_ast = parser.parse();
        parse_profile.stop();

        if (!parsed_ast) {
            return std::unexpected(InterpreterError{abs_path,
                                                    parsed_ast.error().row,
                                                    parsed_ast.error().col,
                                                    parsed_ast.error().offset,
                                                    parsed_ast.error().length,
                                                    parsed_ast.error().reason,
                                                    {}});
        }

        ast_to_use = &parsed_ast.value();

        // Cache the AST if this is a frequently-included system module
        if (AstCache::is_cacheable(abs_path)) {
            ast_cache_.put(abs_path, parsed_ast.value());
            // Point to the cached copy so parsed_ast can be moved
            ast_to_use = ast_cache_.get(abs_path);
        }
    }

    std::string old_file = get_current_file();
    std::string abs_dir = std::string(Path(abs_path).parent_path());

    std::string old_list_file = get_variable("CMAKE_CURRENT_LIST_FILE");
    std::string old_list_dir = get_variable("CMAKE_CURRENT_LIST_DIR");
    std::string old_parent_list_file = get_variable("CMAKE_PARENT_LIST_FILE");

    set_variable("CMAKE_CURRENT_LIST_FILE", abs_path);
    set_variable("CMAKE_CURRENT_LIST_DIR", abs_dir);
    // While the included file runs, its parent is the file that included it.
    set_variable("CMAKE_PARENT_LIST_FILE", old_list_file);
    set_current_file(abs_path);

    if (debugger_) debugger_->push_call_depth();

    // Auto-scope policies around included files (CMake behavior)
    push_policies();

    std::expected<void, InterpreterError> res;
    {
        // return() in included file shouldn't propagate to caller
        ReturnGuard rg(*this);

        ProfileScope interpret_profile("interpret " + abs_path, "interpret");
        res = interpret(*ast_to_use);
        interpret_profile.stop();
    }

    pop_policies();

    if (debugger_) debugger_->pop_call_depth();

    set_variable("CMAKE_CURRENT_LIST_FILE", old_list_file);
    set_variable("CMAKE_CURRENT_LIST_DIR", old_list_dir);
    set_variable("CMAKE_PARENT_LIST_FILE", old_parent_list_file);
    set_current_file(old_file);

    return res;
}

const Interpreter::DirectoryCacheEntry* Interpreter::get_directory_cache_entry(std::string_view dir) {
    Interpreter* root = get_root();

    // Fast path: if dir is already absolute and normalized, look up directly
    // using string_view — zero allocations on cache hit.
    // This covers the common case of paths from GLOB_RECURSE and other FS operations.
    if (!dir.empty() && dir[0] == '/') {
        auto it = root->dir_scan_cache_.find(dir);
        if (it != root->dir_scan_cache_.end()) { return &it->second; }
    }

    // Slow path: normalize to absolute path
    std::string dir_key = Path(Path::absolute(dir)).lexically_normal().str();
    if (dir_key.empty()) return nullptr;

    // Session cache lookup with normalized key
    auto it = root->dir_scan_cache_.find(dir_key);
    if (it != root->dir_scan_cache_.end()) { return &it->second; }

    // Cache miss — single stat via last_write_time (replaces exists + is_directory + last_write_time)
    std::error_code ec2;
    auto current_mtime = std::filesystem::last_write_time(dir_key, ec2);
    if (ec2) return nullptr; // doesn't exist, not accessible, or not a directory

    // Clock skew detection
    auto now = std::filesystem::file_time_type::clock::now();
    if (current_mtime > now) {
        static std::unordered_set<std::string> warned_dirs;
        if (!warned_dirs.contains(dir_key)) {
            *err_ << colors::YELLOW << "Warning: Directory has modification time in the future: " << dir_key
                  << ". Possible clock skew detected." << colors::RESET << std::endl;
            warned_dirs.insert(dir_key);
        }
    }

    // Convert file_time_type to epoch seconds for persistent cache comparison
    auto sys_tp = std::chrono::file_clock::to_sys(current_mtime);
    int64_t mtime_sec = std::chrono::duration_cast<std::chrono::seconds>(sys_tp.time_since_epoch()).count();

    // Check persistent cache
    if (root->cache_store_) {
        auto persistent = root->cache_store_->lookup<CacheSubsystem::FileListing>(dir_key);
        if (persistent && persistent->dir_mtime == mtime_sec) {
            // Populate session cache from persistent cache
            DirectoryCacheEntry cache_entry;
            cache_entry.mtime = current_mtime;
            for (const auto& f : persistent->files) { cache_entry.entries.insert(f); }
            for (const auto& d : persistent->subdirs) {
                cache_entry.entries.insert(d);
                cache_entry.subdirs.insert(d);
            }
            auto [ins_it, _] = root->dir_scan_cache_.emplace(dir_key, std::move(cache_entry));
            return &ins_it->second;
        }
    }

    // Cache miss - scan directory
    TransparentStringSet entries;
    TransparentStringSet subdirs;
    try {
        std::error_code dir_ec;
        for (const auto& entry : std::filesystem::directory_iterator(dir_key, dir_ec)) {
            if (dir_ec) {
                // Error during iteration - don't cache
                return nullptr;
            }
            auto name = entry.path().filename().string();
            entries.insert(name);
            std::error_code ec2;
            if (entry.is_directory(ec2) && !ec2) { subdirs.insert(name); }
        }
    } catch (...) {
        // Permission denied or other errors - don't cache
        return nullptr;
    }

    // Populate persistent cache
    if (root->cache_store_) {
        FileListingCacheEntry persistent_entry;
        persistent_entry.dir_mtime = mtime_sec;
        for (const auto& e : entries) {
            if (subdirs.contains(e)) {
                persistent_entry.subdirs.push_back(e);
            } else {
                persistent_entry.files.push_back(e);
            }
        }
        root->cache_store_->insert<CacheSubsystem::FileListing>(dir_key, persistent_entry);
    }

    // Store in session cache
    DirectoryCacheEntry cache_entry{current_mtime, std::move(entries), std::move(subdirs)};
    auto [inserted_it, _] = root->dir_scan_cache_.emplace(dir_key, std::move(cache_entry));
    return &inserted_it->second;
}

const Interpreter::TransparentStringSet* Interpreter::get_directory_listing(std::string_view dir) {
    auto* entry = get_directory_cache_entry(dir);
    return entry ? &entry->entries : nullptr;
}

const Interpreter::TransparentStringSet* Interpreter::get_directory_subdirs(std::string_view dir) {
    auto* entry = get_directory_cache_entry(dir);
    return entry ? &entry->subdirs : nullptr;
}

bool Interpreter::cached_file_exists(std::string_view full_path) {
    if (full_path.empty()) return false;

    // Split path into directory and filename using string ops
    Path p(full_path);
    auto parent = p.parent_path();
    auto filename = p.filename();

    if (filename.empty()) {
        // Path is a root or has trailing slash — check if directory itself exists
        // by trying to get its listing (single stat on cache miss, zero on hit)
        return get_directory_listing(full_path) != nullptr;
    }

    // Try to get cached listing of parent directory
    // This will populate the cache on-demand if not already cached
    auto* entries = get_directory_listing(parent);
    if (!entries) {
        // Cache population failed (permissions, doesn't exist, etc.)
        // Fall back to direct filesystem check
        std::error_code ec;
        return std::filesystem::exists(std::string(full_path), ec);
    }

    return entries->contains(filename);
}

bool Interpreter::cached_file_exists(std::string_view dir, const std::string& filename) {
    auto* entries = get_directory_listing(dir);
    return entries && entries->contains(filename);
}

bool Interpreter::cached_is_directory(std::string_view path) {
    Path p(path);
    auto parent = p.parent_path();
    auto filename = p.filename();

    if (!filename.empty()) {
        // get_directory_listing handles session cache → persistent cache → directory scan
        auto* subdirs = get_directory_subdirs(parent);
        if (subdirs) { return subdirs->contains(filename); }
    }

    // Parent doesn't exist or can't be read — fall back to direct stat
    std::error_code ec;
    return std::filesystem::is_directory(std::string(path), ec);
}

void Interpreter::set_fatal_error(const std::string& message) {
    // If debugger is attached, drop into it before dying
    if (debugger_) { debugger_->on_fatal_error(message); }

    Interpreter* root = get_root();
    std::vector<CallLocation> backtrace;
    if (!root->trace_stack_.empty()) {
        // The last element is the current command, we want everything BEFORE it as backtrace
        // Convert TraceEntry (non-owning) → CallLocation (owning) for the error
        for (size_t i = 0; i < root->trace_stack_.size() - 1; ++i) {
            const auto& te = root->trace_stack_[i];
            backtrace.push_back({te.file ? *te.file : std::string(), te.row, te.col, te.offset, te.length, std::string(te.command)});
        }

        const auto& current = root->trace_stack_.back();
        set_fatal_error(InterpreterError{current.file ? *current.file : std::string(), current.row, current.col, current.offset,
                                         current.length, message, backtrace});
    } else {
        set_fatal_error(InterpreterError{current_file_, current_cmd_row_, current_cmd_col_, 0, 0, message, {}});
    }
}

void Interpreter::set_fatal_error(const InterpreterError& error) {
    Interpreter* root = get_root();
    if (!root->fatal_error_) {
        root->fatal_error_ = error;
    } else {
        // Enrich existing error if possible
        if (error.source_content && !root->fatal_error_->source_content && error.file == root->fatal_error_->file) {
            root->fatal_error_->source_content = error.source_content;
        }
    }
}

std::optional<InterpreterError> Interpreter::get_fatal_error() const {
    return const_cast<Interpreter*>(this)->get_root()->fatal_error_;
}

void Interpreter::clear_fatal_error() {
    get_root()->fatal_error_ = std::nullopt;
}

void Interpreter::add_builtin(const std::string& name, BuiltinFunction func) {
    get_root()->builtins_[name] = func;
}

void Interpreter::expand_arguments_into(const std::vector<Argument>& args, std::vector<std::string>& result) {
    for (const auto& arg : args) {
        // Fast path: single literal part, unquoted — skip evaluate_argument entirely
        if (arg.parts.size() == 1 && !arg.quoted && std::holds_alternative<std::string>(arg.parts[0])) {
            const auto& s = std::get<std::string>(arg.parts[0]);
            if (s.empty()) continue;
            if (s.find(';') == std::string::npos) {
                result.push_back(s);
            } else {
                for (auto item : CMakeArrayIterator(s)) {
                    if (item.find('\\') != std::string_view::npos) {
                        result.push_back(unescape_list_element(item));
                    } else {
                        result.emplace_back(item);
                    }
                }
            }
            continue;
        }

        // Fast path: single ${VAR} reference, unquoted — iterate variable storage directly (zero copy)
        if (arg.parts.size() == 1 && !arg.quoted && std::holds_alternative<VariableReference>(arg.parts[0])) {
            const auto& ref = std::get<VariableReference>(arg.parts[0]);
            if (ref.namespace_prefix.empty() && ref.name_parts.size() == 1 && std::holds_alternative<std::string>(ref.name_parts[0])) {
                auto view = get_variable_view(std::get<std::string>(ref.name_parts[0]));
                if (view && !view->empty()) {
                    if (view->find(';') == std::string_view::npos) {
                        result.emplace_back(*view);
                    } else {
                        for (auto item : CMakeArrayIterator(*view)) {
                            if (!item.empty()) {
                                if (item.find('\\') != std::string_view::npos) {
                                    result.push_back(unescape_list_element(item));
                                } else {
                                    result.emplace_back(item);
                                }
                            }
                        }
                    }
                }
                continue;
            }
        }

        std::string val = evaluate_argument(arg);
        if (arg.quoted) {
            result.push_back(std::move(val));
        } else {
            if (val.empty()) continue;

            // Check if this argument contains a variable reference
            // CMake filters empty elements only when expanding variable references
            bool has_var_ref = false;
            for (const auto& part : arg.parts) {
                if (std::holds_alternative<VariableReference>(part)) {
                    has_var_ref = true;
                    break;
                }
            }

            // Split by semicolon for unquoted arguments (list expansion)
            for (auto item : CMakeArrayIterator(val)) {
                // CMake removes empty elements when expanding variable references
                if (has_var_ref && item.empty()) { continue; }
                if (item.find('\\') != std::string_view::npos) {
                    result.push_back(unescape_list_element(item));
                } else {
                    result.emplace_back(item);
                }
            }
        }
    }
}

std::vector<std::string> Interpreter::expand_arguments(const std::vector<Argument>& args) {
    std::vector<std::string> result;
    expand_arguments_into(args, result);
    return result;
}

std::expected<void, InterpreterError> Interpreter::execute_command(const CommandInvocation& cmd) {
    current_cmd_row_ = cmd.row;
    current_cmd_col_ = cmd.col;

    // Push to backtrace stack
    Interpreter* root = get_root();
    root->trace_stack_.push_back({current_file_interned_, cmd.row, cmd.col, cmd.offset, cmd.length, cmd.identifier});

    if (root->trace_stack_.size() > max_trace_depth_) {
        pop_trace_stack();
        return std::unexpected(InterpreterError{current_file_,
                                                cmd.row,
                                                cmd.col,
                                                cmd.offset,
                                                cmd.length,
                                                "call stack depth limit exceeded (" + std::to_string(max_trace_depth_) + ")",
                                                {}});
    }

    // Fast paths: shapes recognized at parse time. Each skips arg-vector
    // construction, builtin dispatch, and the per-builtin parser. Valid only
    // when no debugger trace and no user override of the command is active
    // (matches the dispatch order in execute_command_with_args).
    if (!debugger_) {
        if (cmd.pre_parsed_math) {
            if (root->user_functions_.find(std::string_view("math")) == root->user_functions_.end()
                && root->user_macros_.find(std::string_view("math")) == root->user_macros_.end()) {
                if (try_execute_pre_parsed_math(*this, *cmd.pre_parsed_math, cmd.arguments)) {
                    pop_trace_stack();
                    return {};
                }
            }
        } else if (cmd.pre_parsed_substring) {
            if (root->user_functions_.find(std::string_view("string")) == root->user_functions_.end()
                && root->user_macros_.find(std::string_view("string")) == root->user_macros_.end()) {
                if (try_execute_pre_parsed_substring(*this, *cmd.pre_parsed_substring)) {
                    pop_trace_stack();
                    return {};
                }
            }
        }
    }

    // Expand arguments once (reuse buffer to avoid per-call allocation)
    expanded_args_buf_.clear();
    expand_arguments_into(cmd.arguments, expanded_args_buf_);

    // Debugger/trace hook - must happen after argument expansion
    if (debugger_) { debugger_->on_command(current_file_, cmd.row, cmd.col, cmd.identifier, expanded_args_buf_, cmd.arguments); }

    auto res = execute_command_with_args(cmd.identifier, expanded_args_buf_);

    if (!res) {
        if (auto err = get_fatal_error()) {
            // The error is already set, just clean up stack and return it
            pop_trace_stack();
            return std::unexpected(*err);
        }
    }

    pop_trace_stack();
    return res;
}

std::expected<void, InterpreterError> Interpreter::execute_command_with_args(const std::string& identifier,
                                                                             const std::vector<std::string>& args) {
    Interpreter* root = get_root();

    // Fast path: check if already lowercase (common case) to avoid allocation
    bool is_lower = true;
    for (char c : identifier) {
        if (c >= 'A' && c <= 'Z') {
            is_lower = false;
            break;
        }
    }
    const std::string& lower_identifier = is_lower ? identifier : (lower_buf_ = to_lower(identifier), lower_buf_);

    // CMake dispatch order: user functions/macros override builtins.
    // When a user defines `macro(find_package ...)`, calls to `find_package` go
    // to the macro; the original builtin is reachable as `_find_package`.
    // vcpkg.cmake relies on this to inject its toolchain logic.
    auto fit = root->user_functions_.find(lower_identifier);
    if (fit != root->user_functions_.end()) { return invoke_user_function(*fit->second, args); }
    auto mit = root->user_macros_.find(lower_identifier);
    if (mit != root->user_macros_.end()) { return invoke_user_macro(*mit->second, args); }

    auto bit = root->builtins_.find(lower_identifier);
    if (bit != root->builtins_.end()) {
        bit->second(*this, args);
        if (auto err = get_fatal_error()) { return std::unexpected(*err); }
        return {};
    }

    // Underscore-prefixed name reaches past a user override to the original builtin.
    // CMake actually maintains a per-name override stack; a single-level fallback
    // covers the common case (one wrapper around a builtin) which is all vcpkg uses.
    if (!lower_identifier.empty() && lower_identifier.front() == '_') {
        auto bit_under = root->builtins_.find(lower_identifier.substr(1));
        if (bit_under != root->builtins_.end()) {
            bit_under->second(*this, args);
            if (auto err = get_fatal_error()) { return std::unexpected(*err); }
            return {};
        }
    }

    set_fatal_error("Unknown command: " + identifier);
    return std::unexpected(*get_fatal_error());
}

std::expected<void, InterpreterError> Interpreter::execute_if_block(const IfBlock& if_block) {
    Interpreter* root = get_root();
    root->trace_stack_.push_back({current_file_interned_, if_block.row, if_block.col, if_block.offset, if_block.length, "if"});

    auto cond_result =
        evaluate_condition(if_block.condition, if_block.pre_parsed, if_block.row, if_block.col, if_block.offset, if_block.length);
    if (!cond_result) {
        set_fatal_error(cond_result.error());
        pop_trace_stack();
        return std::unexpected(cond_result.error());
    }

    if (cond_result.value()) {
        auto res = interpret(if_block.then_branch);
        if (!res) set_fatal_error(res.error());
        pop_trace_stack();
        return res;
    }

    for (const auto& elseif : if_block.elseif_branches) {
        auto elseif_cond = evaluate_condition(elseif.condition, elseif.pre_parsed, elseif.row, elseif.col, elseif.offset, elseif.length);
        if (!elseif_cond) {
            set_fatal_error(elseif_cond.error());
            pop_trace_stack();
            return std::unexpected(elseif_cond.error());
        }
        if (elseif_cond.value()) {
            auto res = interpret(elseif.body);
            if (!res) set_fatal_error(res.error());
            pop_trace_stack();
            return res;
        }
    }

    auto res = interpret(if_block.else_branch);
    if (!res) set_fatal_error(res.error());
    pop_trace_stack();
    return res;
}

std::expected<void, InterpreterError> Interpreter::execute_function_block(const FunctionBlock& block) {
    // Resolve dynamic name (e.g., function("${VAR}" ...)) at registration time.
    std::string resolved_name = block.name;
    if (resolved_name.empty()) { resolved_name = evaluate_argument(block.name_argument); }
    std::string lower_name = to_lower(resolved_name);
    // Store at root - CMake functions/macros are globally visible
    Interpreter* root = get_root();

    // Single lookup: try to insert nullptr, get iterator to existing or new entry
    auto [it, inserted] = root->user_functions_.try_emplace(lower_name, nullptr);
    if (!inserted && it->second) {
        // Replacing existing function - check if it's currently executing
        const FunctionBlock* old_ptr = it->second.get();
        size_t shallowest_index = 0;
        bool found = false;
        for (size_t i = 0; i < root->frame_stack_.size(); ++i) {
            if (root->frame_stack_[i].function_block == old_ptr) {
                shallowest_index = i;
                found = true;
                break; // first hit = outermost with push_back vector
            }
        }
        if (found) {
            // Defer deletion until we've popped past this frame
            root->deferred_function_deletions_.emplace_back(shallowest_index, std::move(it->second));
        }
    }

    it->second = std::make_unique<FunctionBlock>(block);
    if (it->second->name.empty()) it->second->name = resolved_name;
    root->user_macros_.erase(lower_name);
    return {};
}

std::expected<void, InterpreterError> Interpreter::execute_macro_block(const MacroBlock& block) {
    // Resolve dynamic name (e.g., macro("${VAR}" ...)) at registration time.
    std::string resolved_name = block.name;
    if (resolved_name.empty()) { resolved_name = evaluate_argument(block.name_argument); }
    std::string lower_name = to_lower(resolved_name);
    // Store at root - CMake functions/macros are globally visible
    Interpreter* root = get_root();

    // Single lookup: try to insert nullptr, get iterator to existing or new entry
    auto [it, inserted] = root->user_macros_.try_emplace(lower_name, nullptr);
    if (!inserted && it->second) {
        // Replacing existing macro - check if it's currently executing
        auto depth_it = root->macro_execution_depth_.find(lower_name);
        if (depth_it != root->macro_execution_depth_.end() && depth_it->second > 0) {
            // Macro is executing, defer deletion
            root->deferred_macro_deletions_.push_back({lower_name, static_cast<size_t>(depth_it->second), std::move(it->second)});
        }
    }

    it->second = std::make_unique<MacroBlock>(block);
    if (it->second->name.empty()) it->second->name = resolved_name;
    root->user_functions_.erase(lower_name);
    return {};
}

std::expected<void, InterpreterError> Interpreter::execute_foreach_block(const ForeachBlock& block) {
    Interpreter* root = get_root();
    root->trace_stack_.push_back({current_file_interned_, block.row, block.col, block.offset, block.length, "foreach"});

    // Handle ZIP_LISTS mode separately due to different variable handling
    if (std::holds_alternative<ForeachZipLists>(block.params)) {
        const auto& zip = std::get<ForeachZipLists>(block.params);

        // Save previous values of all loop variables (for nested loops)
        std::vector<std::pair<std::string, bool>> saved_vars;
        std::vector<std::string> saved_values;

        // For single loop var: save var_0, var_1, ... based on number of lists
        // For multiple loop vars: save each variable directly
        std::vector<std::string> var_names_to_save;
        if (zip.loop_vars.size() == 1) {
            // Single variable mode: create var_0, var_1, etc.
            for (size_t i = 0; i < zip.lists.size(); ++i) { var_names_to_save.push_back(zip.loop_vars[0] + "_" + std::to_string(i)); }
        } else {
            // Multiple variables mode: use variables directly
            var_names_to_save = zip.loop_vars;
        }

        for (const auto& var_name : var_names_to_save) {
            bool was_set = is_variable_set(var_name);
            std::string old_value = was_set ? get_variable(var_name) : "";
            saved_vars.push_back({var_name, was_set});
            saved_values.push_back(old_value);
        }

        // Evaluate all lists and find max length
        std::vector<CMakeArray> evaluated_lists;
        size_t max_length = 0;
        for (const auto& list_arg : zip.lists) {
            std::string list_name = evaluate_argument(list_arg);
            CMakeArray list(get_variable(list_name));
            max_length = std::max(max_length, list.size());
            evaluated_lists.push_back(std::move(list));
        }

        loop_depth_++;

        // Fast path: use Entry handles to avoid per-iteration hash lookups
        if (variable_watches_.empty()) {
            // Pre-create Entry handles for all loop variables
            std::vector<ShadowMap::Entry> loop_entries;
            loop_entries.reserve(var_names_to_save.size());
            for (const auto& var_name : var_names_to_save) { loop_entries.push_back(variables_.entry(var_name)); }

            for (size_t i = 0; i < max_length; ++i) {
                for (size_t list_idx = 0; list_idx < evaluated_lists.size(); ++list_idx) {
                    const auto& list = evaluated_lists[list_idx];
                    std::string value = (i < list.size()) ? list[i] : "";

                    size_t entry_idx;
                    if (zip.loop_vars.size() == 1) {
                        entry_idx = list_idx;
                    } else {
                        if (list_idx < zip.loop_vars.size()) {
                            entry_idx = list_idx;
                        } else {
                            continue;
                        }
                    }
                    if (entry_idx < loop_entries.size()) { loop_entries[entry_idx].set(value); }
                }

                auto res = interpret(block.body);
                if (!res) {
                    set_fatal_error(res.error());
                    if (loop_depth_ <= 0) {
                        std::cerr << "FATAL: loop_depth_ is " << loop_depth_ << " when handling foreach body error\n";
                        std::abort();
                    }
                    loop_depth_--;
                    for (size_t j = 0; j < loop_entries.size(); ++j) {
                        if (saved_vars[j].second)
                            loop_entries[j].set(saved_values[j]);
                        else
                            loop_entries[j].set("");
                    }
                    pop_trace_stack();
                    return res;
                }

                if (loop_control_ == LoopControl::BREAK) {
                    clear_loop_control();
                    break;
                }
                if (loop_control_ == LoopControl::CONTINUE) clear_loop_control();
                if (return_requested_) break;
            }

            for (size_t j = 0; j < loop_entries.size(); ++j) {
                if (saved_vars[j].second)
                    loop_entries[j].set(saved_values[j]);
                else
                    loop_entries[j].set("");
            }
        } else {
            // Slow path: use set_variable() which fires watches
            for (size_t i = 0; i < max_length; ++i) {
                for (size_t list_idx = 0; list_idx < evaluated_lists.size(); ++list_idx) {
                    const auto& list = evaluated_lists[list_idx];
                    std::string value = (i < list.size()) ? list[i] : "";

                    std::string var_name;
                    if (zip.loop_vars.size() == 1) {
                        var_name = zip.loop_vars[0] + "_" + std::to_string(list_idx);
                    } else {
                        if (list_idx < zip.loop_vars.size()) {
                            var_name = zip.loop_vars[list_idx];
                        } else {
                            continue;
                        }
                    }
                    set_variable(var_name, value);
                }

                auto res = interpret(block.body);
                if (!res) {
                    set_fatal_error(res.error());
                    if (loop_depth_ <= 0) {
                        std::cerr << "FATAL: loop_depth_ is " << loop_depth_ << " when handling foreach body error\n";
                        std::abort();
                    }
                    loop_depth_--;
                    for (size_t j = 0; j < saved_vars.size(); ++j) {
                        if (saved_vars[j].second) {
                            set_variable(saved_vars[j].first, saved_values[j]);
                        } else {
                            set_variable(saved_vars[j].first, "");
                        }
                    }
                    pop_trace_stack();
                    return res;
                }

                if (loop_control_ == LoopControl::BREAK) {
                    clear_loop_control();
                    break;
                }
                if (loop_control_ == LoopControl::CONTINUE) clear_loop_control();
                if (return_requested_) break;
            }

            for (size_t j = 0; j < saved_vars.size(); ++j) {
                if (saved_vars[j].second) {
                    set_variable(saved_vars[j].first, saved_values[j]);
                } else {
                    set_variable(saved_vars[j].first, "");
                }
            }
        }

        if (loop_depth_ <= 0) {
            std::cerr << "FATAL: loop_depth_ is " << loop_depth_ << " when trying to decrement in foreach\n";
            std::abort();
        }
        loop_depth_--;
        pop_trace_stack();
        return {};
    }

    // Original logic for non-ZIP_LISTS modes
    // Evaluate the loop variable name (may contain variable references like _${PREFIX}_VAR)
    std::string loop_var_name = evaluate_argument(block.loop_var);

    // Save the loop variable's previous value (for nested loops)
    bool loop_var_was_set = is_variable_set(loop_var_name);
    std::string loop_var_old_value;
    if (loop_var_was_set) { loop_var_old_value = get_variable(loop_var_name); }

    loop_depth_++;

    // Validate RANGE step=0 before any early-out
    if (std::holds_alternative<ForeachRange>(block.params)) {
        const auto& r = std::get<ForeachRange>(block.params);
        if (r.step) {
            long step = parse_number<long>(evaluate_argument(*r.step)).value_or(0);
            if (step == 0) {
                if (loop_depth_ <= 0) {
                    std::cerr << "FATAL: loop_depth_ is " << loop_depth_ << " when handling step=0 error\n";
                    std::abort();
                }
                loop_depth_--;
                set_fatal_error("Step cannot be zero");
                auto err = get_fatal_error();
                clear_fatal_error();
                pop_trace_stack();
                return std::unexpected(*err);
            }
        }
    }

    // Empty body: skip building items and iteration entirely.
    // The loop var restore below handles the post-loop state.
    if (block.body.empty()) {
        // CMake sets loop var to empty string after loop if it wasn't defined before
        if (!loop_var_was_set) set_variable(loop_var_name, "");
        if (loop_depth_ <= 0) {
            std::cerr << "FATAL: loop_depth_ is " << loop_depth_ << " when trying to decrement in foreach\n";
            std::abort();
        }
        loop_depth_--;
        pop_trace_stack();
        return {};
    }

    // RANGE fast path: skip building items_raw and re-parsing it. Iterate the
    // integer counter directly and write each value into the loop variable via
    // to_chars + an Entry handle (one hash lookup amortized over the loop).
    // Skipped when variable watches are active (they need the slow set path).
    if (std::holds_alternative<ForeachRange>(block.params) && variable_watches_.empty()) {
        const auto& r = std::get<ForeachRange>(block.params);
        long start = r.start ? parse_number<long>(evaluate_argument(*r.start)).value_or(0) : 0;
        long stop = parse_number<long>(evaluate_argument(r.stop)).value_or(0);
        long step;
        if (r.step) {
            step = parse_number<long>(evaluate_argument(*r.step)).value_or(0);
        } else {
            step = (start <= stop) ? 1 : -1;
        }
        auto loop_entry = variables_.entry(loop_var_name);
        char buf[24];
        for (long i = start; (step > 0) ? (i <= stop) : (i >= stop); i += step) {
            auto rc = std::to_chars(buf, buf + sizeof(buf), i);
            loop_entry.set(std::string(buf, rc.ptr - buf));
            auto res = interpret(block.body);
            if (!res) {
                set_fatal_error(res.error());
                if (loop_depth_ <= 0) {
                    std::cerr << "FATAL: loop_depth_ is " << loop_depth_ << " when handling foreach body error\n";
                    std::abort();
                }
                loop_depth_--;
                if (loop_var_was_set)
                    loop_entry.set(loop_var_old_value);
                else
                    loop_entry.set("");
                pop_trace_stack();
                return res;
            }
            if (loop_control_ == LoopControl::BREAK) {
                clear_loop_control();
                break;
            }
            if (loop_control_ == LoopControl::CONTINUE) clear_loop_control();
            if (return_requested_) break;
        }
        if (loop_var_was_set)
            loop_entry.set(loop_var_old_value);
        else
            loop_entry.set("");

        if (loop_depth_ <= 0) {
            std::cerr << "FATAL: loop_depth_ is " << loop_depth_ << " when trying to decrement in foreach\n";
            std::abort();
        }
        loop_depth_--;
        pop_trace_stack();
        return {};
    }

    // Build items as a single semicolon-separated string, then iterate with
    // CMakeArrayIterator — avoids O(N) vector<string> + CMakeArray allocations.
    std::string items_raw;
    bool filter_empty = false;

    if (std::holds_alternative<ForeachSimple>(block.params)) {
        const auto& simple = std::get<ForeachSimple>(block.params);
        filter_empty = true;
        if (simple.items.size() == 1) {
            items_raw = evaluate_argument(simple.items[0]);
        } else {
            for (const auto& arg : simple.items) {
                auto val = evaluate_argument(arg);
                if (!val.empty()) {
                    if (!items_raw.empty()) items_raw += ';';
                    items_raw += val;
                }
            }
        }
    } else if (std::holds_alternative<ForeachRange>(block.params)) {
        const auto& r = std::get<ForeachRange>(block.params);
        long start = r.start ? parse_number<long>(evaluate_argument(*r.start)).value_or(0) : 0;
        long stop = parse_number<long>(evaluate_argument(r.stop)).value_or(0);
        long step;
        if (r.step) {
            step = parse_number<long>(evaluate_argument(*r.step)).value_or(0);
        } else {
            step = (start <= stop) ? 1 : -1;
        }
        for (long i = start; (step > 0) ? (i <= stop) : (i >= stop); i += step) {
            if (!items_raw.empty()) items_raw += ';';
            items_raw += std::to_string(i);
        }
    } else if (std::holds_alternative<ForeachIn>(block.params)) {
        const auto& in = std::get<ForeachIn>(block.params);
        for (const auto& l : in.lists) {
            auto val = get_variable(evaluate_argument(l));
            if (!val.empty()) {
                if (!items_raw.empty()) items_raw += ';';
                items_raw += val;
            }
        }
        for (const auto& arg : in.items) {
            auto val = evaluate_argument(arg);
            if (!val.empty()) {
                if (!items_raw.empty()) items_raw += ';';
                items_raw += val;
            }
        }
    }

    // Fast path: use Entry to avoid per-iteration hash lookups (watches almost always empty)
    if (variable_watches_.empty()) {
        auto loop_entry = variables_.entry(loop_var_name);
        for (auto item : CMakeArrayIterator(items_raw)) {
            if (filter_empty && item.empty()) continue;
            if (item.find('\\') != std::string_view::npos) {
                loop_entry.set(unescape_list_element(item));
            } else {
                loop_entry.set(std::string(item));
            }
            auto res = interpret(block.body);
            if (!res) {
                set_fatal_error(res.error());
                if (loop_depth_ <= 0) {
                    std::cerr << "FATAL: loop_depth_ is " << loop_depth_ << " when handling foreach body error\n";
                    std::abort();
                }
                loop_depth_--;
                if (loop_var_was_set)
                    loop_entry.set(loop_var_old_value);
                else
                    loop_entry.set("");
                pop_trace_stack();
                return res;
            }
            if (loop_control_ == LoopControl::BREAK) {
                clear_loop_control();
                break;
            }
            if (loop_control_ == LoopControl::CONTINUE) clear_loop_control();
            if (return_requested_) break;
        }
        if (loop_var_was_set)
            loop_entry.set(loop_var_old_value);
        else
            loop_entry.set("");
    } else {
        // Slow path: use set_variable() which fires watches
        for (auto item : CMakeArrayIterator(items_raw)) {
            if (filter_empty && item.empty()) continue;
            if (item.find('\\') != std::string_view::npos) {
                set_variable(loop_var_name, unescape_list_element(item));
            } else {
                set_variable(loop_var_name, std::string(item));
            }
            auto res = interpret(block.body);
            if (!res) {
                set_fatal_error(res.error());
                if (loop_depth_ <= 0) {
                    std::cerr << "FATAL: loop_depth_ is " << loop_depth_ << " when handling foreach body error\n";
                    std::abort();
                }
                loop_depth_--;
                if (loop_var_was_set) {
                    set_variable(loop_var_name, loop_var_old_value);
                } else {
                    set_variable(loop_var_name, "");
                }
                pop_trace_stack();
                return res;
            }
            if (loop_control_ == LoopControl::BREAK) {
                clear_loop_control();
                break;
            }
            if (loop_control_ == LoopControl::CONTINUE) clear_loop_control();
            if (return_requested_) break;
        }
        if (loop_var_was_set) {
            set_variable(loop_var_name, loop_var_old_value);
        } else {
            set_variable(loop_var_name, "");
        }
    }

    // Sanity check: loop depth should never go negative
    if (loop_depth_ <= 0) {
        std::cerr << "FATAL: loop_depth_ is " << loop_depth_ << " when trying to decrement in foreach\n";
        std::abort();
    }
    loop_depth_--;
    pop_trace_stack();
    return {};
}

std::expected<void, InterpreterError> Interpreter::execute_while_block(const WhileBlock& block) {
    Interpreter* root = get_root();
    root->trace_stack_.push_back({current_file_interned_, block.row, block.col, block.offset, block.length, "while"});

    loop_depth_++;

    // Evaluate condition and loop
    while (true) {
        auto cond_result = evaluate_condition(block.condition, block.pre_parsed, block.row, block.col, block.offset, block.length);
        if (!cond_result) {
            loop_depth_--;
            pop_trace_stack();
            return std::unexpected(cond_result.error());
        }

        // Break if condition is false
        if (!cond_result.value()) { break; }

        // Execute body
        auto res = interpret(block.body);
        if (!res) {
            loop_depth_--;
            pop_trace_stack();
            return res;
        }

        if (loop_control_ == LoopControl::BREAK) {
            clear_loop_control();
            break;
        }
        if (loop_control_ == LoopControl::CONTINUE) clear_loop_control();
        if (return_requested_) break; // return() exits the loop and propagates to caller
    }

    loop_depth_--;
    pop_trace_stack();
    return {};
}

std::expected<void, InterpreterError> Interpreter::execute_block_block(const BlockBlock& block) {
    Interpreter* root = get_root();
    root->trace_stack_.push_back({current_file_interned_, block.row, block.col, block.offset, block.length, "block"});

    // Create a new variable scope if SCOPE_FOR VARIABLES is set
    if (block.scope_for_variables) {
        // Push a new variable scope
        variables_.push_scope();

        // Execute the body
        auto res = interpret(block.body);

        // Propagate variables back to parent scope before popping
        if (!block.propagate_vars.empty()) {
            // Save propagated variable values before popping
            std::unordered_map<std::string, std::string> propagated_values;
            for (const auto& var_name : block.propagate_vars) {
                if (auto* val = variables_.try_get(var_name)) { propagated_values[var_name] = *val; }
            }

            // Pop the block scope
            variables_.pop_scope();

            // Set propagated variables in parent scope
            for (const auto& [var_name, value] : propagated_values) { variables_.set(var_name, value); }
        } else {
            // Pop the block scope without propagation
            variables_.pop_scope();
        }

        if (!res) {
            pop_trace_stack();
            return res;
        }
    } else {
        // No variable scope, just execute the body
        auto res = interpret(block.body);
        if (!res) {
            pop_trace_stack();
            return res;
        }
    }

    pop_trace_stack();
    return {};
}

std::expected<void, InterpreterError> Interpreter::invoke_user_function(const FunctionBlock& func, const std::vector<std::string>& args) {
    // Push metadata frame (for script_dir and function_block tracking)
    frame_stack_.push_back({&func.definition_dir, &func});

    // Push variable scope
    variables_.push_scope();

    // Set function parameters — build ARGV/ARGN strings directly from args
    variables_.set("ARGC", std::to_string(args.size()));

    // ARGV: join all args with semicolons (pre-reserve to avoid reallocations)
    {
        size_t total = args.empty() ? 0 : args.size() - 1; // semicolons
        for (const auto& a : args) total += a.size();
        std::string argv_str;
        argv_str.reserve(total);
        for (size_t i = 0; i < args.size(); ++i) {
            if (i > 0) argv_str += ';';
            argv_str += args[i];
        }
        variables_.set("ARGV", std::move(argv_str));
    }

    // ARGN: join args past the declared parameters
    {
        size_t extra = args.size() > func.parameters.size() ? args.size() - func.parameters.size() : 0;
        size_t total = extra == 0 ? 0 : extra - 1;
        for (size_t i = func.parameters.size(); i < args.size(); ++i) total += args[i].size();
        std::string argn_str;
        argn_str.reserve(total);
        for (size_t i = func.parameters.size(); i < args.size(); ++i) {
            if (i > func.parameters.size()) argn_str += ';';
            argn_str += args[i];
        }
        variables_.set("ARGN", std::move(argn_str));
    }

    // ARGVn and named parameters — use pre-computed keys for common cases
    static const std::string argv_keys[] = {
        "ARGV0",  "ARGV1",  "ARGV2",  "ARGV3",  "ARGV4",  "ARGV5",  "ARGV6",  "ARGV7",  "ARGV8",  "ARGV9",  "ARGV10",
        "ARGV11", "ARGV12", "ARGV13", "ARGV14", "ARGV15", "ARGV16", "ARGV17", "ARGV18", "ARGV19", "ARGV20", "ARGV21",
        "ARGV22", "ARGV23", "ARGV24", "ARGV25", "ARGV26", "ARGV27", "ARGV28", "ARGV29", "ARGV30", "ARGV31",
    };
    static constexpr size_t num_precomputed = sizeof(argv_keys) / sizeof(argv_keys[0]);
    for (size_t i = 0; i < args.size(); ++i) {
        if (i < num_precomputed) {
            variables_.set(argv_keys[i], args[i]);
        } else {
            variables_.set("ARGV" + std::to_string(i), args[i]);
        }
    }
    for (size_t i = 0; i < func.parameters.size() && i < args.size(); ++i) { variables_.set(func.parameters[i], args[i]); }

    int saved_depth = loop_depth_;
    LoopControl saved_control = loop_control_;
    std::string saved_file = current_file_;
    const std::string* saved_file_interned = current_file_interned_;
    // Save and clear macro substitutions - functions create a new scope so outer
    // macro parameters should not bleed into the function's variable lookups.
    // Skip save/restore when empty (common case — no macros in scope).
    bool had_macro_subs = !macro_substitutions_.empty();
    std::map<std::string, std::string> saved_macro_substitutions;
    if (had_macro_subs) {
        saved_macro_substitutions = std::move(macro_substitutions_);
        macro_substitutions_.clear();
    }

    loop_depth_ = 0;
    loop_control_ = LoopControl::NONE;
    current_file_ = func.definition_file;
    current_file_interned_ = intern_file(current_file_);

    if (debugger_) debugger_->push_call_depth();

    std::expected<void, InterpreterError> res;
    {
        // return() in function shouldn't propagate to caller
        ReturnGuard rg(*this);
        res = interpret(func.body);
    }

    if (debugger_) debugger_->pop_call_depth();

    // Pop variable scope (automatic cleanup of all function-local variables)
    variables_.pop_scope();

    // Pop metadata frame
    frame_stack_.pop_back();

    // Clean up any deferred function deletions we've passed
    Interpreter* root = get_root();
    if (!root->deferred_function_deletions_.empty()) {
        std::erase_if(root->deferred_function_deletions_, [&](const auto& entry) { return root->frame_stack_.size() <= entry.first; });
    }

    if (!res) set_fatal_error(res.error());

    loop_depth_ = saved_depth;
    loop_control_ = saved_control;
    current_file_ = saved_file;
    current_file_interned_ = saved_file_interned;
    if (had_macro_subs) { macro_substitutions_ = std::move(saved_macro_substitutions); }
    return res;
}

bool Interpreter::call_user_function(const std::string& name, const std::vector<std::string>& args) {
    // Normalize to lowercase for lookup
    std::string lower_name = name;
    std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), [](unsigned char c) { return std::tolower(c); });

    // Look up function in root interpreter
    auto* root = get_root();
    auto it = root->user_functions_.find(lower_name);
    if (it == root->user_functions_.end()) {
        // Function doesn't exist - return false
        return false;
    }

    // Call the function
    auto result = invoke_user_function(*it->second, args);
    return result.has_value();
}

std::expected<void, InterpreterError> Interpreter::invoke_user_macro(const MacroBlock& macro, const std::vector<std::string>& args) {
    // Save only the entries we're about to overwrite (not the entire map)
    struct SavedEntry {
        std::string key;
        std::optional<std::string> old_value;
    };
    std::vector<SavedEntry> saved;
    saved.reserve(args.size() + macro.parameters.size() + 3);

    auto save_and_set = [&](const std::string& key, std::string value) {
        auto it = macro_substitutions_.find(key);
        if (it != macro_substitutions_.end())
            saved.push_back({key, std::move(it->second)});
        else
            saved.push_back({key, std::nullopt});
        macro_substitutions_[key] = std::move(value);
    };

    auto save_and_set_ref = [&](const std::string& key, const std::string& value) {
        auto it = macro_substitutions_.find(key);
        if (it != macro_substitutions_.end())
            saved.push_back({key, std::move(it->second)});
        else
            saved.push_back({key, std::nullopt});
        macro_substitutions_[key] = value;
    };

    // Build ARGC/ARGV/ARGN directly from args
    save_and_set("ARGC", std::to_string(args.size()));

    std::string argv_str;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) argv_str += ';';
        argv_str += args[i];
    }
    save_and_set("ARGV", std::move(argv_str));

    std::string argn_str;
    for (size_t i = macro.parameters.size(); i < args.size(); ++i) {
        if (i > macro.parameters.size()) argn_str += ';';
        argn_str += args[i];
    }
    save_and_set("ARGN", std::move(argn_str));

    for (size_t i = 0; i < args.size(); ++i) { save_and_set("ARGV" + std::to_string(i), args[i]); }
    for (size_t i = 0; i < macro.parameters.size() && i < args.size(); ++i) { save_and_set_ref(macro.parameters[i], args[i]); }

    std::string saved_file = current_file_;
    const std::string* saved_file_interned = current_file_interned_;
    current_file_ = macro.definition_file;
    current_file_interned_ = intern_file(current_file_);

    // Track macro execution depth for deferred deletion
    Interpreter* root = get_root();
    std::string lower_name = to_lower(macro.name);
    ++root->macro_execution_depth_[lower_name];

    if (debugger_) debugger_->push_call_depth();
    auto res = interpret(macro.body);
    if (debugger_) debugger_->pop_call_depth();

    // Decrement and cleanup deferred deletions
    int& depth = root->macro_execution_depth_[lower_name];
    --depth;
    if (!root->deferred_macro_deletions_.empty()) {
        std::erase_if(root->deferred_macro_deletions_, [&lower_name, depth](const auto& entry) {
            return entry.name == lower_name && entry.depth > static_cast<size_t>(depth);
        });
    }
    if (depth == 0) { root->macro_execution_depth_.erase(lower_name); }

    current_file_ = saved_file;
    current_file_interned_ = saved_file_interned;
    if (!res) set_fatal_error(res.error());

    // Restore only the entries we modified
    for (auto& [key, old_val] : saved) {
        if (old_val)
            macro_substitutions_[key] = std::move(*old_val);
        else
            macro_substitutions_.erase(key);
    }

    return res;
}

// Allowlist-based truthy check matching CMake's cmIsOn().
// Only 1/Y/ON/YES/TRUE (case-insensitive) are truthy.
// This is NOT the inverse of is_falsy() — values like "/usr/local/bin"
// are neither is_truthy() nor is_falsy().
// Case-insensitive equality without allocation (ASCII only — CMake booleans are always ASCII)
static bool ci_equal(const char* a, const char* b, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        auto upper = [](char c) -> char { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c; };
        if (upper(a[i]) != upper(b[i])) return false;
    }
    return true;
}

bool Interpreter::is_truthy(const std::string& val) {
    switch (val.size()) {
    case 1:
        return val[0] == '1' || val[0] == 'Y' || val[0] == 'y';
    case 2:
        return ci_equal(val.data(), "ON", 2);
    case 3:
        return ci_equal(val.data(), "YES", 3);
    case 4:
        return ci_equal(val.data(), "TRUE", 4);
    default:
        return false;
    }
}

bool Interpreter::is_falsy(std::string_view val) {
    if (val.empty()) return true;
    switch (val.size()) {
    case 1:
        return val[0] == '0' || val[0] == 'N' || val[0] == 'n';
    case 2:
        return ci_equal(val.data(), "NO", 2);
    case 3:
        return ci_equal(val.data(), "OFF", 3);
    case 5:
        return ci_equal(val.data(), "FALSE", 5);
    case 6:
        return ci_equal(val.data(), "IGNORE", 6);
    case 8:
        return ci_equal(val.data(), "NOTFOUND", 8);
    }
    // *-NOTFOUND (case-insensitive suffix)
    if (val.size() > 9 && ci_equal(val.data() + val.size() - 9, "-NOTFOUND", 9)) { return true; }
    return false;
}

std::expected<bool, InterpreterError> Interpreter::evaluate_condition(const std::vector<Argument>& condition, size_t row, size_t col,
                                                                      size_t offset, size_t length) {
    return kiln::evaluate_condition(*this, condition, row, col, offset, length);
}

std::expected<bool, InterpreterError> Interpreter::evaluate_condition(const std::vector<Argument>& condition, const PreParsedCondition& pp,
                                                                      size_t row, size_t col, size_t offset, size_t length) {
    return kiln::evaluate_condition(*this, condition, pp, row, col, offset, length);
}

bool Interpreter::has_user_function(const std::string& name) const {
    std::string lower_name = to_lower(name);
    return get_root()->user_functions_.contains(lower_name) || get_root()->user_macros_.contains(lower_name)
           || builtins_.contains(lower_name);
}

std::string Interpreter::evaluate_variable_reference(const VariableReference& ref) {
    // Fast path: single string name_part with no namespace (most common: ${SIMPLE_VAR})
    // Avoids copying the variable name - uses string_view directly from AST
    if (ref.name_parts.size() == 1 && std::holds_alternative<std::string>(ref.name_parts[0]) && ref.namespace_prefix.empty()) {
        // Use string_view to avoid copying the variable name for lookup
        std::string_view name = std::get<std::string>(ref.name_parts[0]);
        return get_variable(name);
    }

    // General path: build variable name from parts (handles nested refs like ${${PREFIX}_VAR})
    std::string name;
    for (const auto& part : ref.name_parts) {
        if (std::holds_alternative<std::string>(part)) {
            name += std::get<std::string>(part);
        } else {
            name += evaluate_variable_reference(std::get<VariableReference>(part));
        }
    }

    // Lookup based on namespace
    if (ref.namespace_prefix.empty()) {
        // Dynamically computed name: skip macro substitutions.
        // CMake macros do textual substitution on the body before evaluation,
        // so a nested form like ${${VAR}} that resolves the inner ${VAR} to the
        // string "ARGC" hits the *variable* ARGC, not the macro's ARGC param.
        // The simple-path branch above still consults macro substitutions for
        // direct references like ${ARGC}.
        if (auto* val = variables_.try_get(name)) return *val;
        const auto* root = const_cast<Interpreter*>(this)->get_root();
        if (auto cache_it = root->cache_variables_.find(name); cache_it != root->cache_variables_.end()) return cache_it->second;
        return "";
    } else if (ref.namespace_prefix == "ENV") {
        const char* env_var = getenv(name.c_str());
        return env_var ? env_var : "";
    } else if (ref.namespace_prefix == "CACHE") {
        auto* root = get_root();
        auto it = root->cache_variables_.find(name);
        return (it != root->cache_variables_.end()) ? it->second : "";
    }
    return "";
}

std::string Interpreter::evaluate_argument(const Argument& arg) {
    // Fast path: single-part arguments (covers bare keywords like "EXPR"
    // and simple variable refs like ${var} — the vast majority of arguments)
    if (arg.parts.size() == 1) {
        auto& p = arg.parts[0];
        if (std::holds_alternative<std::string>(p)) return std::get<std::string>(p);
        return evaluate_variable_reference(std::get<VariableReference>(p));
    }

    // General path: multi-part arguments (e.g. "${prefix}_suffix")
    std::string res;
    for (const auto& p : arg.parts) {
        if (std::holds_alternative<std::string>(p)) {
            res += std::get<std::string>(p);
        } else {
            res += evaluate_variable_reference(std::get<VariableReference>(p));
        }
    }
    return res;
}

std::string Interpreter::get_variable(std::string_view name) const {
    auto view = get_variable_view(name);
    return view ? std::string(*view) : std::string();
}

void Interpreter::add_variable_watch(const std::string& var_name, std::optional<std::string> callback) {
    VariableWatch watch;
    watch.variable = var_name;
    watch.callback_function = std::move(callback);
    variable_watches_[var_name] = std::move(watch);
}

void Interpreter::fire_variable_watch(const std::string& name, const std::string& access_type, const std::string& value) {
    auto it = variable_watches_.find(name);
    if (it == variable_watches_.end()) return;

    // Print to stderr
    *err_ << "-- variable_watch: " << name << " was " << access_type << ", value=\"" << value << "\", in file " << current_file_ << "\n";

    // If callback function specified, call it
    if (it->second.callback_function) {
        call_user_function(*it->second.callback_function, {name, access_type, value, current_file_, "UNKNOWN"});
    }

    // Notify debugger if present
    if (debugger_) { debugger_->on_variable_access(name, access_type, value, current_file_); }
}

void Interpreter::set_variable(const std::string& name, const std::string& val) {
    variables_.set(name, val);
    if (!variable_watches_.empty()) { fire_variable_watch(name, "MODIFIED_ACCESS", val); }
}

std::expected<void, std::string> Interpreter::set_variable_parent_scope(const std::string& name, const std::string& val) {
    // Use ShadowMap's parent scope for both function and subdirectory contexts
    // add_subdirectory now uses push_scope/pop_scope, so ShadowMap handles all scoping
    auto result = variables_.set_parent_scope(name, val);
    if (result) { return {}; }
    return std::unexpected(result.error());
}

std::expected<void, std::string> Interpreter::unset_variable_parent_scope(const std::string& name) {
    // Use ShadowMap's parent scope for both function and subdirectory contexts
    // add_subdirectory now uses push_scope/pop_scope, so ShadowMap handles all scoping
    auto result = variables_.unset_parent_scope(name);
    if (result) { return {}; }
    return std::unexpected(result.error());
}

void Interpreter::set_cache_variable(const std::string& var_name, const std::string& value) {
    get_root()->cache_variables_[var_name] = value;
}

bool Interpreter::unset_variable(const std::string& name) {
    if (!variable_watches_.empty()) { fire_variable_watch(name, "REMOVED_ACCESS", ""); }
    variables_.unset(name);
    return true; // ShadowMap::unset is always safe (no-op if variable doesn't exist)
}

bool Interpreter::is_variable_set(std::string_view name) const {
    // Check macro substitutions first (skip when empty — common case for non-macro code)
    if (!macro_substitutions_.empty()) {
        if (auto it = macro_substitutions_.find(std::string(name)); it != macro_substitutions_.end()) { return true; }
    }

    // Check local variables (single-lookup via ShadowMap with transparent lookup)
    if (variables_.try_get(name)) { return true; }

    // Check cache variables (always on root — cache is global)
    const auto* root = const_cast<Interpreter*>(this)->get_root();
    return root->cache_variables_.find(name) != root->cache_variables_.end();
}

std::optional<std::string> Interpreter::get_optional_variable(std::string_view name) const {
    if (frame_stack_.empty()) {
        std::cerr << "FATAL: get_optional_variable('" << name << "') called with empty frame_stack_\n";
        std::abort();
    }

    // Check macro substitutions first (macro parameters take precedence)
    // Skip when empty — common case for non-macro code paths
    if (!macro_substitutions_.empty()) {
        if (auto macro_it = macro_substitutions_.find(std::string(name)); macro_it != macro_substitutions_.end()) {
            return macro_it->second;
        }
    }

    // Function-scoped special variables (lazy evaluation - check current frame only)
    // Fast reject: CMAKE_CURRENT_FUNCTION* vs other CMAKE_CURRENT_* variables.
    // The common lookups (SOURCE_DIR, BINARY_DIR, LIST_*) have 'S', 'B', or 'L' at position 14.
    // CMAKE_CURRENT_FUNCTION* has 'F' at position 14. Minimum length is 22 ("CMAKE_CURRENT_FUNCTION").
    // Position 14: CMAKE_CURRENT_FUNCTION
    //              01234567890123456789...
    //                            ^-- position 14 is 'F'
    if (name.size() >= 22 && name[14] == 'F'
        && (name == "CMAKE_CURRENT_FUNCTION" || name == "CMAKE_CURRENT_FUNCTION_LIST_FILE" || name == "CMAKE_CURRENT_FUNCTION_LIST_DIR")) {

        const auto* fb = frame_stack_.back().function_block;
        if (fb != nullptr) {
            if (name == "CMAKE_CURRENT_FUNCTION") return fb->name;
            if (name == "CMAKE_CURRENT_FUNCTION_LIST_FILE") return fb->definition_file;
            if (name == "CMAKE_CURRENT_FUNCTION_LIST_DIR") return fb->definition_dir;
        }
        return std::nullopt; // Not in a function
    }

    // Check local variables (single-lookup via ShadowMap with transparent lookup)
    if (auto* val = variables_.try_get(name)) { return std::string(*val); }

    // Check cache variables (always on root — cache is global)
    const auto* root = const_cast<Interpreter*>(this)->get_root();
    if (auto cache_it = root->cache_variables_.find(name); cache_it != root->cache_variables_.end()) { return cache_it->second; }

    return std::nullopt;
}

std::optional<std::string_view> Interpreter::get_variable_view(std::string_view name) const {
    // Check macro substitutions first (macro parameters take precedence)
    if (!macro_substitutions_.empty()) {
        if (auto it = macro_substitutions_.find(std::string(name)); it != macro_substitutions_.end()) return std::string_view(it->second);
    }

    // Function-scoped special variables
    if (name.size() >= 22 && name[14] == 'F'
        && (name == "CMAKE_CURRENT_FUNCTION" || name == "CMAKE_CURRENT_FUNCTION_LIST_FILE" || name == "CMAKE_CURRENT_FUNCTION_LIST_DIR")) {
        const auto* fb = frame_stack_.back().function_block;
        if (fb != nullptr) {
            if (name == "CMAKE_CURRENT_FUNCTION") return std::string_view(fb->name);
            if (name == "CMAKE_CURRENT_FUNCTION_LIST_FILE") return std::string_view(fb->definition_file);
            if (name == "CMAKE_CURRENT_FUNCTION_LIST_DIR") return std::string_view(fb->definition_dir);
        }
        return std::nullopt;
    }

    // Check local variables (ShadowMap stores std::string, pointer is stable)
    if (auto* val = variables_.try_get(name)) return std::string_view(*val);

    // Check cache variables (always on root — cache is global)
    {
        const auto* root = const_cast<Interpreter*>(this)->get_root();
        if (auto it = root->cache_variables_.find(name); it != root->cache_variables_.end()) return std::string_view(it->second);
    }

    return std::nullopt;
}

void Interpreter::print_message(const std::string& mode, const std::string& msg, bool is_err) {
    std::ostream& os = is_err ? *err_ : *out_;
    std::string indent = get_variable("CMAKE_MESSAGE_INDENT");
    kiln::print_message(os, mode, msg, indent, force_colors_);

    // Debugger hook for --break-on-message
    if (debugger_) { debugger_->on_message(msg); }
}

void Interpreter::print_warning_with_context(const std::string& msg) {
    Interpreter* root = get_root();
    if (!root->trace_stack_.empty()) {
        std::vector<CallLocation> backtrace;
        for (size_t i = 0; i < root->trace_stack_.size() - 1; ++i) {
            const auto& te = root->trace_stack_[i];
            backtrace.push_back({te.file ? *te.file : std::string(), te.row, te.col, te.offset, te.length, std::string(te.command)});
        }
        const auto& current = root->trace_stack_.back();
        std::optional<std::string> source_content;
        if (!root->source_view_.empty()) { source_content = std::string(root->source_view_); }
        print_diagnostic(*err_, DiagnosticSeverity::Warning, msg, current.file ? *current.file : std::string(), current.row, current.col,
                         current.offset, current.length, backtrace, source_content);
    } else {
        print_message("WARNING", msg, true);
    }
}

CMakeArray Interpreter::from_arguments(const std::vector<std::string>& args) {
    return CMakeArray(args);
}

void Interpreter::check_start(const std::string& message) {
    // Add indentation for nested checks
    std::string indent(check_stack_.size() * 2, ' ');
    print_message("CHECK_START", indent + "Checking " + message, false);
    check_stack_.push_back(message);
}

void Interpreter::check_pass(const std::string& result_message) {
    if (check_stack_.empty()) {
        print_message("WARNING", "CHECK_PASS without matching CHECK_START", true);
        return;
    }

    std::string original = check_stack_.back();
    check_stack_.pop_back();

    // Add indentation for nested checks
    std::string indent(check_stack_.size() * 2, ' ');

    // Print result with original message
    print_message("CHECK_PASS", indent + "Checking " + original + " - " + result_message, false);
}

void Interpreter::check_fail(const std::string& result_message) {
    if (check_stack_.empty()) {
        print_message("WARNING", "CHECK_FAIL without matching CHECK_START", true);
        return;
    }

    std::string original = check_stack_.back();
    check_stack_.pop_back();

    // Add indentation for nested checks
    std::string indent(check_stack_.size() * 2, ' ');

    // Print result with original message
    print_message("CHECK_FAIL", indent + "Checking " + original + " - " + result_message, false);
}

void Interpreter::accumulate_error(const std::string& error) {
    has_send_errors_ = true;
    // Errors are already printed by message(), just track that we had errors
}

void Interpreter::check_invariants() const {
    // These are internal invariant checks - if any fail, it's a bug in kiln itself.
    // Please report at: https://github.com/anthropics/kiln/issues

    if (frame_stack_.empty()) {
        std::cerr << "INTERNAL ERROR (kiln bug): frame_stack_ is empty\n";
        std::abort();
    }

    if (loop_control_ != LoopControl::NONE && loop_depth_ <= 0) {
        std::cerr << "INTERNAL ERROR (kiln bug): loop_control_ is set (" << static_cast<int>(loop_control_) << ") but loop_depth_ is "
                  << loop_depth_ << "\n";
        std::abort();
    }

    if (loop_depth_ < 0) {
        std::cerr << "INTERNAL ERROR (kiln bug): loop_depth_ is negative (" << loop_depth_ << ")\n";
        std::abort();
    }

    if (builtins_.empty()) {
        std::cerr << "INTERNAL ERROR (kiln bug): interpreter has no builtins\n";
        std::abort();
    }

    if (directory_stack_.empty()) {
        std::cerr << "INTERNAL ERROR (kiln bug): directory_stack_ is empty\n";
        std::abort();
    }
}

void Interpreter::pop_trace_stack() {
    auto* root = get_root();
    assert(!root->trace_stack_.empty() && "push/pop imbalance in trace_stack_");
    root->trace_stack_.pop_back();
}

void Interpreter::finalize_directory_targets() {
    // Apply accumulated directory properties to all owned targets in current directory
    auto& ctx = get_current_directory_context();

    // CMAKE_INCLUDE_CURRENT_DIR: when ON, prepend source and binary dirs to include path
    bool include_current_dir = false;
    std::string icd_val = get_variable("CMAKE_INCLUDE_CURRENT_DIR");
    if (!icd_val.empty() && !is_falsy(icd_val)) { include_current_dir = true; }

    for (const auto& target : ctx.owned_targets) {
        // Imported targets are not affected by directory-level commands like
        // link_libraries(), include_directories(), add_definitions(), etc.
        // CMake only applies these to targets created with add_executable/add_library
        // (non-IMPORTED) in the current directory scope.
        if (target->is_imported()) continue;

        // Apply CMAKE_INCLUDE_CURRENT_DIR (binary dir first, then source dir — CMake order)
        if (include_current_dir && target->get_type() != TargetType::INTERFACE_LIBRARY) {
            std::string src_dir = get_variable("CMAKE_CURRENT_SOURCE_DIR");
            std::string bin_dir = get_variable("CMAKE_CURRENT_BINARY_DIR");
            target->prepend_property("INCLUDE_DIRECTORIES", {src_dir}, PropertyVisibility::PRIVATE);
            target->prepend_property("INCLUDE_DIRECTORIES", {bin_dir}, PropertyVisibility::PRIVATE);
        }

        for (const auto& [prop_name, values] : ctx.accumulated) {
            if (!values.empty()) {
                // append_property handles duplicates gracefully (just appends again)
                target->append_property(prop_name, values, PropertyVisibility::PRIVATE);
            }
        }
    }
}

std::optional<int64_t> Interpreter::get_dir_mtime_cached(const std::string& path) {
    auto root = get_root();

    // Check session cache (project paths cached too — dirs don't change during a single run)
    auto it = root->dir_mtime_cache_.find(path);
    if (it != root->dir_mtime_cache_.end()) { return it->second; }

    // Not cached - stat and cache it
    std::error_code ec;
    std::optional<int64_t> mtime;
    auto ft = std::filesystem::last_write_time(path, ec);
    if (!ec) {
        auto sys_tp = std::chrono::file_clock::to_sys(ft);
        mtime = std::chrono::duration_cast<std::chrono::seconds>(sys_tp.time_since_epoch()).count();
    }

    root->dir_mtime_cache_[path] = mtime;
    return mtime;
}

std::string Interpreter::cached_weakly_canonical(std::string_view p) {
    auto parent = std::string(Path(p).parent_path());
    auto fname = std::string(Path(p).filename());

    // Look up resolved parent directory in cache
    std::string resolved_parent;
    auto it = canonical_dir_cache_.find(parent);
    if (it != canonical_dir_cache_.end()) {
        resolved_parent = it->second;
    } else {
        try {
            resolved_parent = std::filesystem::weakly_canonical(parent).string();
        } catch (...) { resolved_parent = Path(parent).lexically_normal().str(); }
        canonical_dir_cache_[parent] = resolved_parent;
    }

    // For the leaf: check if it's a symlink (single lstat syscall)
    std::string full_str = Path::join(resolved_parent, fname);
    std::error_code ec;
    if (std::filesystem::is_symlink(full_str, ec)) {
        try {
            return std::filesystem::weakly_canonical(full_str).string();
        } catch (...) { return Path(full_str).lexically_normal().str(); }
    }
    return Path(full_str).lexically_normal().str();
}

bool Interpreter::is_project_path(const std::string& path) const {
    const auto* root = get_root();

    // Lazily populate cached absolute dir strings
    if (root->cached_abs_source_dir_.empty()) {
        root->cached_abs_source_dir_ = std::filesystem::absolute(get_variable("CMAKE_SOURCE_DIR")).string();
        // Ensure trailing slash for prefix matching
        if (!root->cached_abs_source_dir_.empty() && root->cached_abs_source_dir_.back() != '/') root->cached_abs_source_dir_ += '/';
    }
    if (root->cached_abs_binary_dir_.empty()) {
        root->cached_abs_binary_dir_ = std::filesystem::absolute(get_variable("CMAKE_BINARY_DIR")).string();
        if (!root->cached_abs_binary_dir_.empty() && root->cached_abs_binary_dir_.back() != '/') root->cached_abs_binary_dir_ += '/';
    }

    // Only compute absolute() for the input path
    std::string abs_path = std::filesystem::absolute(path).string();

    // Use string prefix matching (cheaper than path iterator mismatch)
    if (abs_path.starts_with(root->cached_abs_source_dir_)) return true;
    if (abs_path.starts_with(root->cached_abs_binary_dir_)) return true;

    // Also check exact match (path IS the source/binary dir itself)
    // Compare without trailing slash
    std::string_view src_view(root->cached_abs_source_dir_);
    src_view.remove_suffix(1); // remove trailing '/'
    if (abs_path == src_view) return true;

    std::string_view bin_view(root->cached_abs_binary_dir_);
    bin_view.remove_suffix(1);
    if (abs_path == bin_view) return true;

    return false;
}

} // namespace kiln
