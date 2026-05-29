#pragma once
#include "language.hpp"
#include <string>
#include <vector>

namespace kiln {

// A command emitted by a Compiler driver, in two views:
//   - argv: the actual argv to spawn (includes cosmetic flags like
//     -fdiagnostics-color=always).
//   - signature_argv: the same command with cosmetic-only flags omitted, safe
//     to feed into the build cache signature so toggling color/presentation
//     doesn't bust cache hits. The driver itself decides which flags it
//     emitted are cosmetic — we never parse argv strings to find out.
//
// User-supplied options (CompileContext::options et al) pass through
// unchanged in both views. Anything smuggled into a user CFLAGS string is
// the user's problem, same as in CMake/ninja.
struct CompilerCommand {
    std::vector<std::string> argv;
    std::vector<std::string> signature_argv;
};

struct CompileContext {
    std::string source;
    std::string output;
    std::vector<std::string> includes;
    std::vector<std::string> system_includes; // Directories to include with -isystem (no warnings)
    std::vector<std::string> definitions;
    std::vector<std::string> options;
    std::string standard;
    bool extensions_enabled = true; // GNU extensions (gnu11 vs c11)
    bool is_shared = false;
    bool is_pie = false;                    // -fPIE for executables with POSITION_INDEPENDENT_CODE
    std::string visibility_preset;          // e.g. "hidden", "default", "protected", "internal"
    bool visibility_inlines_hidden = false; // -fvisibility-inlines-hidden (GCC/Clang)
    std::string pch_include;
    bool color_diagnostics = false;

    // C++20 modules support
    bool is_module_source = false;         // True if source uses modules
    std::string module_mapper_file;        // Path to module mapper file (for -fmodule-mapper=)
    std::string bmi_output;                // Path for BMI output (if this provides a module)
    std::vector<std::string> module_files; // Explicit -fmodule-file= arguments
};

// C++20 header-unit compilation context. Inputs to the compile that turns a
// header file into a precompiled BMI (Binary Module Interface) consumable by
// `import <header>;` / `import "header";`. The driver emits
// -fmodule-header=<system|user> based on is_system_header. The BMI is written
// to the path advertised by the mapper file (kiln pre-stages an entry there).
struct HeaderUnitContext {
    std::string source;             // Absolute path to the header
    std::string bmi_output;         // Where the BMI should land
    std::string module_mapper_file; // Mapper used to advertise the BMI path
    bool is_system_header = false;  // include-angle (true) vs include-quote
    std::vector<std::string> includes;
    std::vector<std::string> system_includes;
    std::vector<std::string> definitions;
    std::vector<std::string> options;
    std::string standard;
    bool extensions_enabled = true;
    bool color_diagnostics = false;
};

struct ModuleScanContext {
    std::string source;
    std::string output;   // DDI (P1689 JSON) output file path
    std::string obj_path; // Final .o path; emitted as `primary-output` in P1689
    std::string depfile;  // .d depfile for header dependencies (-MF)
    std::vector<std::string> includes;
    std::vector<std::string> system_includes; // Directories to include with -isystem (no warnings)
    std::vector<std::string> definitions;
    std::string standard;
    bool extensions_enabled = true; // GNU extensions (gnu11 vs c11)
    bool color_diagnostics = false;
};

struct LinkContext {
    std::string output;
    std::vector<std::string> objects;
    std::vector<std::string> lib_dirs;
    std::vector<std::string> libs;
    std::vector<std::string> linker_flags;
    // Compiler/system implicit link search dirs (CMAKE_<LANG>_IMPLICIT_LINK_DIRECTORIES).
    // Used to skip system paths when emitting RUNPATH so we don't pollute it.
    std::vector<std::string> implicit_link_dirs;
    // CMAKE_SKIP_BUILD_RPATH / SKIP_BUILD_RPATH target property.
    // When true, do not embed any link-path-derived RUNPATH entries.
    bool skip_build_rpath = false;
    // BUILD_RPATH (target property / CMAKE_BUILD_RPATH variable). Extra
    // RUNPATH entries to embed in the build-tree binary, in addition to the
    // automatically derived ones (unless skip_build_rpath / build_with_install_rpath).
    std::vector<std::string> build_rpath;
    // INSTALL_RPATH (target property / CMAKE_INSTALL_RPATH variable). Used at
    // build time only when build_with_install_rpath is set; otherwise reserved
    // for the install step (kiln does not currently rewrite at install).
    std::vector<std::string> install_rpath;
    // BUILD_WITH_INSTALL_RPATH: when true, embed install_rpath in the build
    // binary and skip the automatically derived link-dir RUNPATH entries.
    bool build_with_install_rpath = false;
    // SOVERSION-derived shared object name. Emitted as -Wl,-soname,<soname>.
    // Empty means no explicit soname (linker uses output filename).
    // NOTE: kiln does not currently rename the on-disk file or generate
    // libfoo.so → libfoo.so.N symlinks; only DT_SONAME is set.
    std::string soname;
    bool is_shared = false;
    bool is_pie = false; // -pie for executables with POSITION_INDEPENDENT_CODE
    std::string standard;
    bool extensions_enabled = true; // GNU extensions (gnu11 vs c11)
    bool color_diagnostics = false;
};

// Platform information detected from the compiler and system
struct PlatformInfo {
    std::string compiler_id;                     // "GNU", "Clang", etc.
    std::string compiler_version;                // "11.3.0"
    std::string system_name;                     // "Linux", "Darwin", "Windows"
    std::string system_processor;                // "x86_64", "aarch64", etc.
    std::string sizeof_void_p;                   // "8" or "4"
    std::vector<std::string> implicit_includes;  // Implicit include directories
    std::vector<std::string> implicit_link_dirs; // Implicit link directories
    std::vector<std::string> implicit_link_libs; // Implicit link libraries
    int default_cxx_standard = 0;                // Compiler's default C++ standard (e.g. 17)
    int default_c_standard = 0;                  // Compiler's default C standard (e.g. 17)
};

class Compiler {
public:
    virtual ~Compiler() = default;

    // Identity: the binary path / sysroot / compiler-target this Compiler
    // instance was configured with. Used by Toolchain to dedupe registry
    // entries — two captures with the same identity tuple resolve to the
    // same Compiler*. Default-empty for compilers that don't carry these
    // (only the GNU/Clang family does today).
    virtual const std::string& binary() const {
        static const std::string e;
        return e;
    }
    virtual const std::string& sysroot() const {
        static const std::string e;
        return e;
    }
    virtual const std::string& compiler_target() const {
        static const std::string e;
        return e;
    }

    // Detected version string (e.g. "13.2.0"). Populated once by the caller
    // after detect_platform() runs and the compiler is registered with the
    // toolchain — empty until then. Used as part of the per-task signature
    // hash so a system update that swaps the underlying compiler under a
    // stable PATH entry invalidates cached objects.
    const std::string& version() const { return version_; }
    void set_version(std::string v) { version_ = std::move(v); }

    virtual CompilerCommand get_compile_command(const CompileContext& ctx) const = 0;
    virtual CompilerCommand get_link_command(const LinkContext& ctx) const = 0;
    virtual std::vector<std::string> get_archive_command(const std::string& output, const std::vector<std::string>& objs) const = 0;

    // Files the linker writes as a side effect of producing the primary
    // output, parsed out of the final argv. CMake doesn't track these
    // explicitly, but custom_target DEPENDS frequently reference them
    // (e.g. -Map= map files, --out-implib= MinGW import libraries) and
    // kiln's stricter dependency graph stalls if no producer is registered.
    //
    // Returned paths are as they appear in argv (unnormalized). The caller
    // resolves them against the link task's working directory. Drivers that
    // emit no side-effect outputs (or aren't implemented yet, e.g. MSVC
    // /MAP, /IMPLIB, /PDB) return an empty vector.
    virtual std::vector<std::string> get_link_side_effect_outputs(const std::vector<std::string>& argv) const {
        (void) argv;
        return {};
    }

    // C++20 modules support
    virtual CompilerCommand get_module_scan_command(const ModuleScanContext& ctx) const {
        (void) ctx;
        return {}; // Default: no module support
    }

    // C++20 header-unit compilation. Compile a header file into a BMI usable
    // by `import <header>;` / `import "header";`. Default: not supported.
    virtual CompilerCommand get_header_unit_compile_command(const HeaderUnitContext& ctx) const {
        (void) ctx;
        return {};
    }

    // True if this compiler can emit a P1689r5 dependency-info JSON
    // (-fdeps-format=p1689r5 on GCC ≥14, /scanDependencies on MSVC, etc.).
    // kiln rejects module sources at interpretation time when this is false.
    virtual bool supports_p1689() const { return false; }

    // Module bindings can't be known until after the collator runs (logical
    // name -> BMI path is resolved cross-target). GCC takes this via a single
    // `-fmodule-mapper=<file>` indirection on argv; the file's contents are
    // late-bound. Clang has no mapper; instead each compile gets a per-task
    // `@<file>` response file containing `-fmodule-file=name=path` lines
    // (and `-fmodule-output=` if the TU provides a module).
    //
    // When this returns true, the collator writes a per-importer rsp file at
    // `<obj>.modules.rsp` and the compiler's get_compile_command emits
    // `@<obj>.modules.rsp` instead of `-fmodule-mapper=`.
    virtual bool uses_per_task_module_rsp() const { return false; }

    // True if this compiler ships a libstdc++.modules.json (or equivalent)
    // describing the std module's source unit, AND is recent enough to
    // compile it. GCC ≥15 with libstdc++.modules.json present.
    virtual bool supports_import_std() const { return false; }

    // Absolute path to the toolchain's libstdc++.modules.json, or empty if
    // the compiler doesn't ship one or doesn't expose it.
    virtual std::string libstdcxx_modules_json_path() const { return {}; }

    // Platform detection - detects compiler info and system platform
    virtual PlatformInfo detect_platform() const {
        return {}; // Default: empty platform info
    }

    // Spelling for `-std=cNN`/`-std=c++NN` style flags. Used to populate
    // `CMAKE_<LANG><STD>_STANDARD_COMPILE_OPTION` so CMake scripts that
    // read those variables (or the upstream Compiler/<id>-<lang>.cmake
    // modules) see driver-correct strings. GCC/Clang/TCC return
    // "-std=c++17" etc.; MSVC will return "/std:c++17". Returns empty
    // string for standards the driver has no spelling for.
    virtual std::string std_compile_option(Language lang, int standard) const {
        (void) lang;
        (void) standard;
        return {};
    }

private:
    std::string version_;
};

} // namespace kiln
