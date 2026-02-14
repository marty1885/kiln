#include "interperter.hpp"
#include "command_parser.hpp"
#include "target.hpp"
#include "build_system.hpp"
#include "gnu_compiler.hpp"
#include "genex_evaluator.hpp"
#include "profiler.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <array>
#include "regex.hpp"
#include <sys/stat.h>
#include "builtins/registry.hpp"
#include "intercept/external_project.hpp"
#include "intercept/fetch_content.hpp"
#include "condition_evaluator.hpp"
#include "CMakeArray.hpp"

namespace dmake {

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
// Cached compiler detection data. Static so we don't re-detect compilers when
// multiple Interpreters are created (e.g. during tests).
static std::unordered_map<std::string, std::string> backup_vars;

// Variables for each language that enable_compiler_for_language() will apply.
static constexpr std::array c_lang_vars = {
    "CMAKE_C_COMPILER", "CMAKE_C_COMPILER_ID", "CMAKE_C_COMPILER_VERSION",
    "CMAKE_C_IMPLICIT_INCLUDE_DIRECTORIES", "CMAKE_C_IMPLICIT_LINK_DIRECTORIES",
    "CMAKE_C_IMPLICIT_LINK_LIBRARIES", "CMAKE_C_STANDARD_DEFAULT",
    "CMAKE_C90_STANDARD_COMPILE_OPTION", "CMAKE_C99_STANDARD_COMPILE_OPTION",
    "CMAKE_C11_STANDARD_COMPILE_OPTION", "CMAKE_C17_STANDARD_COMPILE_OPTION",
    "CMAKE_C23_STANDARD_COMPILE_OPTION",
};
static constexpr std::array cxx_lang_vars = {
    "CMAKE_CXX_COMPILER", "CMAKE_CXX_COMPILER_ID", "CMAKE_CXX_COMPILER_VERSION",
    "CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES", "CMAKE_CXX_IMPLICIT_LINK_DIRECTORIES",
    "CMAKE_CXX_IMPLICIT_LINK_LIBRARIES", "CMAKE_CXX_STANDARD_DEFAULT",
    "CMAKE_CXX98_STANDARD_COMPILE_OPTION", "CMAKE_CXX11_STANDARD_COMPILE_OPTION",
    "CMAKE_CXX14_STANDARD_COMPILE_OPTION", "CMAKE_CXX17_STANDARD_COMPILE_OPTION",
    "CMAKE_CXX20_STANDARD_COMPILE_OPTION", "CMAKE_CXX23_STANDARD_COMPILE_OPTION",
};
static constexpr std::array asm_lang_vars = {
    "CMAKE_ASM_COMPILER", "CMAKE_ASM_COMPILER_ID", "CMAKE_ASM_COMPILER_VERSION",
    "CMAKE_ASM_FLAGS", "CMAKE_ASM_FLAGS_DEBUG", "CMAKE_ASM_FLAGS_RELEASE",
    "CMAKE_ASM_FLAGS_RELWITHDEBINFO", "CMAKE_ASM_FLAGS_MINSIZEREL",
};

void fake_cmake_compiler_checks_and_init(Interpreter& interp)
{
    auto apply_system_vars = [&] {
        for (const char* name : {"CMAKE_SYSTEM_NAME", "CMAKE_SYSTEM_PROCESSOR",
                                  "CMAKE_SIZEOF_VOID_P", "CMAKE_HOST_SYSTEM_NAME",
                                  "CMAKE_HOST_SYSTEM_PROCESSOR"}) {
            auto it = backup_vars.find(name);
            if (it != backup_vars.end()) {
                interp.set_variable(name, it->second);
            }
        }
        interp.set_variable("CMAKE_CFG_INTDIR", ".");
    };

    if (!backup_vars.empty()) {
        apply_system_vars();
        return;
    }

    // First-run: detect both C and CXX compilers and cache all data
    auto cxx_compiler = std::make_unique<GnuCompiler>("g++", Language::CXX);
    auto c_compiler = std::make_unique<GnuCompiler>("gcc", Language::C);
    auto cxx_info = cxx_compiler->detect_platform();
    auto c_info = c_compiler->detect_platform();

    // Cache CXX data
    backup_vars["CMAKE_CXX_COMPILER"] = "g++";
    backup_vars["CMAKE_CXX_COMPILER_ID"] = cxx_info.compiler_id;
    backup_vars["CMAKE_CXX_COMPILER_VERSION"] = cxx_info.compiler_version;
    backup_vars["CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES"] = CMakeArray(cxx_info.implicit_includes).to_string();
    backup_vars["CMAKE_CXX_IMPLICIT_LINK_DIRECTORIES"] = CMakeArray(cxx_info.implicit_link_dirs).to_string();
    backup_vars["CMAKE_CXX_IMPLICIT_LINK_LIBRARIES"] = CMakeArray(cxx_info.implicit_link_libs).to_string();
    if (cxx_info.default_cxx_standard > 0)
        backup_vars["CMAKE_CXX_STANDARD_DEFAULT"] = std::to_string(cxx_info.default_cxx_standard);
    backup_vars["CMAKE_CXX98_STANDARD_COMPILE_OPTION"] = "-std=c++98";
    backup_vars["CMAKE_CXX11_STANDARD_COMPILE_OPTION"] = "-std=c++11";
    backup_vars["CMAKE_CXX14_STANDARD_COMPILE_OPTION"] = "-std=c++14";
    backup_vars["CMAKE_CXX17_STANDARD_COMPILE_OPTION"] = "-std=c++17";
    backup_vars["CMAKE_CXX20_STANDARD_COMPILE_OPTION"] = "-std=c++20";
    backup_vars["CMAKE_CXX23_STANDARD_COMPILE_OPTION"] = "-std=c++23";

    // Cache C data
    backup_vars["CMAKE_C_COMPILER"] = "gcc";
    backup_vars["CMAKE_C_COMPILER_ID"] = c_info.compiler_id;
    backup_vars["CMAKE_C_COMPILER_VERSION"] = c_info.compiler_version;
    backup_vars["CMAKE_C_IMPLICIT_INCLUDE_DIRECTORIES"] = CMakeArray(c_info.implicit_includes).to_string();
    backup_vars["CMAKE_C_IMPLICIT_LINK_DIRECTORIES"] = CMakeArray(c_info.implicit_link_dirs).to_string();
    backup_vars["CMAKE_C_IMPLICIT_LINK_LIBRARIES"] = CMakeArray(c_info.implicit_link_libs).to_string();
    if (c_info.default_c_standard > 0)
        backup_vars["CMAKE_C_STANDARD_DEFAULT"] = std::to_string(c_info.default_c_standard);
    backup_vars["CMAKE_C90_STANDARD_COMPILE_OPTION"] = "-std=c90";
    backup_vars["CMAKE_C99_STANDARD_COMPILE_OPTION"] = "-std=c99";
    backup_vars["CMAKE_C11_STANDARD_COMPILE_OPTION"] = "-std=c11";
    backup_vars["CMAKE_C17_STANDARD_COMPILE_OPTION"] = "-std=c17";
    backup_vars["CMAKE_C23_STANDARD_COMPILE_OPTION"] = "-std=c23";

    // Cache system-level data (same for both compilers)
    backup_vars["CMAKE_SYSTEM_NAME"] = cxx_info.system_name;
    backup_vars["CMAKE_SYSTEM_PROCESSOR"] = cxx_info.system_processor;
    backup_vars["CMAKE_SIZEOF_VOID_P"] = cxx_info.sizeof_void_p;
    backup_vars["CMAKE_HOST_SYSTEM_NAME"] = cxx_info.system_name;
    backup_vars["CMAKE_HOST_SYSTEM_PROCESSOR"] = cxx_info.system_processor;

    apply_system_vars();
}

} // anonymous namespace

std::string Interpreter::enable_compiler_for_language(const std::string& lang) {
    std::string loaded_var = "CMAKE_" + lang + "_COMPILER_LOADED";
    if (!get_variable(loaded_var).empty()) return {}; // Already loaded

    if (lang == "C") {
        for (const auto& var : c_lang_vars) {
            auto it = backup_vars.find(var);
            if (it != backup_vars.end() && !it->second.empty())
                set_variable(var, it->second);
        }
        if (backup_vars["CMAKE_C_COMPILER_ID"] == "GNU")
            set_variable("CMAKE_COMPILER_IS_GNUCC", "1");
        set_variable("CMAKE_C_COMPILER_LOADED", "1");
        auto compiler = std::make_unique<GnuCompiler>(get_variable("CMAKE_C_COMPILER"), Language::C);
        get_toolchain().set_compiler(Language::C, std::move(compiler));
    } else if (lang == "CXX") {
        for (const auto& var : cxx_lang_vars) {
            auto it = backup_vars.find(var);
            if (it != backup_vars.end() && !it->second.empty())
                set_variable(var, it->second);
        }
        if (backup_vars["CMAKE_CXX_COMPILER_ID"] == "GNU")
            set_variable("CMAKE_COMPILER_IS_GNUCXX", "1");
        set_variable("CMAKE_CXX_COMPILER_LOADED", "1");
        auto compiler = std::make_unique<GnuCompiler>(get_variable("CMAKE_CXX_COMPILER"), Language::CXX);
        get_toolchain().set_compiler(Language::CXX, std::move(compiler));
    } else if (lang == "ASM") {
        // Check cache first
        auto cached = backup_vars.find("CMAKE_ASM_COMPILER");
        if (cached != backup_vars.end() && !cached->second.empty()) {
            for (const auto& var : asm_lang_vars) {
                auto it = backup_vars.find(var);
                if (it != backup_vars.end() && !it->second.empty())
                    set_variable(var, it->second);
            }
        } else {
            // Derive from C compiler (prefer C, fallback to "cc")
            std::string asm_compiler = get_variable("CMAKE_C_COMPILER");
            std::string asm_id = get_variable("CMAKE_C_COMPILER_ID");
            std::string asm_version = get_variable("CMAKE_C_COMPILER_VERSION");
            if (asm_compiler.empty()) {
                // C not enabled yet, pull from cache
                asm_compiler = backup_vars["CMAKE_C_COMPILER"];
                asm_id = backup_vars["CMAKE_C_COMPILER_ID"];
                asm_version = backup_vars["CMAKE_C_COMPILER_VERSION"];
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
            for (const auto& var : asm_lang_vars) {
                backup_vars[var] = get_variable(var);
            }
        }
        set_variable("CMAKE_ASM_COMPILER_LOADED", "1");
        auto compiler = std::make_unique<GnuCompiler>(get_variable("CMAKE_ASM_COMPILER"), Language::ASM);
        get_toolchain().set_compiler(Language::ASM, std::move(compiler));
    } else {
        return "unsupported language: " + lang + " (only C, CXX, and ASM are supported)";
    }
    return {};
}

Interpreter* Interpreter::get_root() {
    return this;  // No child interpreters anymore - we ARE the root
}

const Interpreter* Interpreter::get_root() const {
    return this;  // No child interpreters anymore - we ARE the root
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
    std::filesystem::path abs_dir;
    if (std::filesystem::path(dir).is_absolute()) {
        abs_dir = std::filesystem::path(dir).lexically_normal();
    } else {
        abs_dir = (std::filesystem::path(get_variable("CMAKE_CURRENT_SOURCE_DIR")) / dir).lexically_normal();
    }

    auto it = directory_contexts_.find(abs_dir.string());
    if (it != directory_contexts_.end()) {
        return &it->second;
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
        if (parent_it != directory_contexts_.end()) {
            ctx.accumulated = parent_it->second.accumulated;
        }
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
        if (!res) {
            set_fatal_error(res.error());
        }
    }
}


std::expected<dmake::Interpreter*, dmake::BuildError> dmake::Interpreter::run_build(int jobs, const std::vector<std::string>& requested_targets) {
    // Sanity check CMAKE_BUILD_TYPE
    std::array<std::string, 4> stanard_build_types_lower = {"debug", "release", "minsize", "relwithdebinfo"};
    auto build_type = get_variable("CMAKE_BUILD_TYPE");
    build_type = to_lower(build_type);
    if (std::find(stanard_build_types_lower.begin(), stanard_build_types_lower.end(), build_type) == stanard_build_types_lower.end()) {
        print_message("WARN", "Build type '" + build_type + "' is not a standard build type. Things MIGHT go wrong.");
    }

    // No longer needed - no child interpreters anymore

    // Determine which targets to build
    std::set<std::string> targets_to_build;

    // Phase A: Determine initial targets (before resolution)
    std::vector<std::string> initial_targets;
    if (requested_targets.empty()) {
        for (const auto& [name, target] : targets_) {
            auto custom = std::dynamic_pointer_cast<CustomTarget>(target);
            if (custom) {
                // Custom targets only build by default if they have ALL flag
                if (custom->is_build_by_default()) {
                    initial_targets.push_back(name);
                }
            } else {
                // Executables and libraries are "ALL" by default unless EXCLUDE_FROM_ALL is set
                std::string exclude = target->get_property("EXCLUDE_FROM_ALL");
                if (exclude.empty() || is_falsy(exclude)) {
                    initial_targets.push_back(name);
                }
            }
        }
    } else {
        for (const auto& t : requested_targets) {
            // Resolve aliases to real target names
            std::string resolved = resolve_target_alias(t);
            if (!targets_.count(resolved)) {
                return std::unexpected(BuildError{current_file_, "Unknown target: " + t});
            }
            initial_targets.push_back(resolved);
        }
    }

    // Phase B: Resolve all initial targets (recursive — resolves entire dep graph
    // via resolve()'s transitive dependency walking)
    {
        ProfileScope scope("resolve dependencies", "graph");
        for (const auto& name : initial_targets) {
            targets_[name]->resolve(targets_, *this);
        }
    }

    // Phase C: Collect targets to build using resolved dependency data.
    // resolve() already decoded genex + aliases, so get_resolved_target_deps()
    // returns canonical target names — no raw property walking needed.
    {
        ProfileScope scope("collect targets", "graph");
        std::function<void(const std::string&)> collect = [&](const std::string& name) {
            if (targets_to_build.count(name)) return;
            if (!targets_.count(name)) return;
            targets_to_build.insert(name);
            auto target = targets_[name];

            // Use resolved dependency data — correct by construction
            for (const auto& dep : target->get_resolved_target_deps()) {
                collect(dep);
            }

            // Custom target deps (already plain names, not genex)
            auto custom = std::dynamic_pointer_cast<CustomTarget>(target);
            if (custom) {
                for (const auto& dep : custom->get_custom_dependencies()) {
                    collect(dep);
                }
            }

            // Manually added dependencies (from add_dependencies command)
            for (const auto& dep : target->get_manually_added_dependencies()) {
                collect(dep);
            }
        };

        for (const auto& name : initial_targets) {
            collect(name);
        }
    }

    std::string root_binary_dir = get_variable("CMAKE_BINARY_DIR");
    std::filesystem::create_directories(root_binary_dir);

    print_message("STATUS", "Generating build graph...");
    BuildGraph graph;

    // 1. (Redundant property propagation removed - handled by Target::resolve)

    // 2. Generate tasks for selected targets
    // Build linker flags from CMAKE variables
    std::vector<std::string> exe_linker_flags;
    std::vector<std::string> shared_linker_flags;

    // Handle CMAKE_LINKER_TYPE (convert to -fuse-ld=<type>)
    std::string linker_type = get_variable("CMAKE_LINKER_TYPE");
    if (!linker_type.empty()) {
        std::string linker_type_upper = to_upper(linker_type);

        if (linker_type_upper == "BFD" || linker_type_upper == "GOLD" ||
            linker_type_upper == "MOLD" || linker_type_upper == "LLD") {
            std::string linker_type_lower = to_lower(linker_type);
            std::string flag = "-fuse-ld=" + linker_type_lower;
            exe_linker_flags.push_back(flag);
            shared_linker_flags.push_back(flag);
        } else {
            return std::unexpected(BuildError{current_file_, "Invalid CMAKE_LINKER_TYPE: " + linker_type + ". Must be one of: BFD, GOLD, MOLD, LLD"});
        }
    }

    // Handle CMAKE_EXE_LINKER_FLAGS (space-separated, not semicolon-separated)
    {
        std::istringstream iss(get_variable("CMAKE_EXE_LINKER_FLAGS"));
        std::string flag;
        while (iss >> flag) {
            exe_linker_flags.push_back(flag);
        }
    }

    // Handle CMAKE_SHARED_LINKER_FLAGS (space-separated, not semicolon-separated)
    {
        std::istringstream iss(get_variable("CMAKE_SHARED_LINKER_FLAGS"));
        std::string flag;
        while (iss >> flag) {
            shared_linker_flags.push_back(flag);
        }
    }

    {
        ProfileScope scope("generate tasks", "graph");
        auto txn = graph.begin();
        for (const auto& name : targets_to_build) {
            targets_[name]->generate_tasks(txn, get_root()->toolchain_, targets_, *this, exe_linker_flags, shared_linker_flags);
        }

        // Resolve missing dependencies: tasks may reference targets (e.g.
        // custom commands invoking llvm-min-tblgen) that weren't in the
        // initial targets_to_build set. Find them and generate their tasks.
        std::unordered_map<std::string, std::string> output_to_target;
        for (const auto& [name, target] : targets_) {
            auto path = target->get_output_path();
            if (!path.empty()) output_to_target[path] = name;
        }

        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto& missing : graph.get_missing_dependencies()) {
                auto it = output_to_target.find(missing);
                if (it != output_to_target.end() && !targets_to_build.count(it->second)) {
                    targets_to_build.insert(it->second);
                    targets_[it->second]->generate_tasks(txn, get_root()->toolchain_, targets_, *this, exe_linker_flags, shared_linker_flags);
                    changed = true;
                }
            }
        }
        auto commit_result = txn.commit();
        if (!commit_result) {
            return std::unexpected(BuildError{current_file_, commit_result.error()});
        }
    }

    // Second pass: resolve circular dependency properties now that all targets are resolved
    for (auto& [name, target] : targets_) {
        target->resolve_deferred_circular_deps(targets_);
    }

    // Print deferred target dumps (AT_BUILD) - targets are now resolved
    for (const auto& dump_target_name : get_targets_to_dump_at_build()) {
        if (targets_.count(dump_target_name)) {
            print_message("", targets_[dump_target_name]->generate_dump_info());
        }
    }

    // Link dependency resolution (adding inputs to link tasks)
    // Static libraries are just .o archives — they don't link against other libs,
    // so adding link-library inputs would create spurious (and circular) dependencies.
    // Uses resolved LINK_LIBRARIES (output paths + system libs) — no raw property walking.
    {
        ProfileScope scope("wire link deps", "graph");
        for (const auto& name : targets_to_build) {
            auto target = targets_[name];
            if (target->get_type() == TargetType::STATIC_LIBRARY)
                continue;

            std::string out_path = target->get_output_path();

            // For custom targets, out_path might be empty or same as name
            std::string task_id = out_path.empty() ? target->get_name() : out_path;

            if (graph.has_task(task_id)) {
                auto& task = graph.get_task(task_id);

                // Resolved LINK_LIBRARIES already contains the flattened link line
                // (output paths for target deps, raw names for system libs).
                // Just check which ones are build graph tasks to wire dependencies.
                for (const auto& lib : target->get_resolved_property("LINK_LIBRARIES")) {
                    if (graph.has_task(lib)) {
                        task.inputs.push_back(lib);
                    }
                }
            }
        }
    }

    // 2b. Post-transaction: evaluate genex, resolve inferred file deps, apply compat deps, validate
    {
        ProfileScope scope("finalize graph", "graph");
        GenexEvaluationContext genex_ctx;
        genex_ctx.build_type = get_variable("CMAKE_BUILD_TYPE");
        genex_ctx.system_name = get_variable("CMAKE_SYSTEM_NAME");
        genex_ctx.cxx_compiler_id = get_variable("CMAKE_CXX_COMPILER_ID");
        genex_ctx.c_compiler_id = get_variable("CMAKE_C_COMPILER_ID");
        genex_ctx.cxx_compiler_version = get_variable("CMAKE_CXX_COMPILER_VERSION");
        genex_ctx.c_compiler_version = get_variable("CMAKE_C_COMPILER_VERSION");
        genex_ctx.all_targets = &targets_;
        genex_ctx.target_aliases = &target_aliases_;
        genex_ctx.install_prefix = get_variable("CMAKE_INSTALL_PREFIX");
        genex_ctx.phase = GenexEvaluationContext::Phase::BUILD;

        auto genex_result = graph.evaluate_genex(genex_ctx);
        if (!genex_result) {
            return std::unexpected(BuildError{current_file_, genex_result.error()});
        }
        graph.resolve_inferred_file_deps();
        graph.apply_cmake_compat_deps();
        auto validate_result = graph.validate();
        if (!validate_result) {
            return std::unexpected(BuildError{current_file_, validate_result.error()});
        }
    }

    // 3. Generate compile_commands.json (on by default)
    std::string export_cmds = get_variable("CMAKE_EXPORT_COMPILE_COMMANDS");
    if (!is_falsy(export_cmds)) {
        auto result = graph.generate_compile_commands(root_binary_dir);
        if (!result) {
            return std::unexpected(BuildError{current_file_, result.error()});
        }
    }

    // 4. Execute the build graph
    print_message("STATUS", "Starting " + build_type + " build...");
    auto result = graph.execute(root_binary_dir, jobs);


    if (!result) {
        return std::unexpected(BuildError{current_file_, result.error()});
    }

    print_message("STATUS", "Build finished.");
    return this;
}

std::expected<BuildGraph, BuildError>
Interpreter::generate_build_graph(const std::vector<std::string>& requested_targets) {
    // Same as generate_dirty_tasks(), but returns the full graph instead of just dirty tasks.
    // Used by EP attachment to atomically add ALL EP tasks (clean tasks skip via normal signature check).

    // Determine which targets to build
    std::set<std::string> targets_to_build;
    std::vector<std::string> initial_targets;

    if (requested_targets.empty()) {
        for (const auto& [name, target] : targets_) {
            auto custom = std::dynamic_pointer_cast<CustomTarget>(target);
            if (custom) {
                if (custom->is_build_by_default()) {
                    initial_targets.push_back(name);
                }
            } else {
                std::string exclude = target->get_property("EXCLUDE_FROM_ALL");
                if (exclude.empty() || is_falsy(exclude)) {
                    initial_targets.push_back(name);
                }
            }
        }
    } else {
        for (const auto& t : requested_targets) {
            std::string resolved = resolve_target_alias(t);
            if (!targets_.count(resolved)) {
                return std::unexpected(BuildError{current_file_, "Unknown target: " + t});
            }
            initial_targets.push_back(resolved);
        }
    }

    // Resolve all initial targets
    for (const auto& name : initial_targets) {
        targets_[name]->resolve(targets_, *this);
    }

    // Collect targets to build
    std::function<void(const std::string&)> collect = [&](const std::string& name) {
        if (targets_to_build.count(name)) return;
        if (!targets_.count(name)) return;
        targets_to_build.insert(name);
        auto target = targets_[name];

        for (const auto& dep : target->get_resolved_target_deps()) {
            collect(dep);
        }

        auto custom = std::dynamic_pointer_cast<CustomTarget>(target);
        if (custom) {
            for (const auto& dep : custom->get_custom_dependencies()) {
                collect(dep);
            }
        }

        for (const auto& dep : target->get_manually_added_dependencies()) {
            collect(dep);
        }
    };

    for (const auto& name : initial_targets) {
        collect(name);
    }

    std::string root_binary_dir = get_variable("CMAKE_BINARY_DIR");
    std::filesystem::create_directories(root_binary_dir);

    BuildGraph graph;

    // Build linker flags
    std::vector<std::string> exe_linker_flags;
    std::vector<std::string> shared_linker_flags;

    std::string linker_type = get_variable("CMAKE_LINKER_TYPE");
    if (!linker_type.empty()) {
        std::string linker_type_upper = to_upper(linker_type);
        if (linker_type_upper == "BFD" || linker_type_upper == "GOLD" ||
            linker_type_upper == "MOLD" || linker_type_upper == "LLD") {
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

    // Generate tasks via transaction
    {
        auto txn = graph.begin();
        for (const auto& name : targets_to_build) {
            targets_[name]->generate_tasks(txn, get_root()->toolchain_, targets_, *this, exe_linker_flags, shared_linker_flags);
        }

        // Resolve missing dependencies
        std::unordered_map<std::string, std::string> output_to_target;
        for (const auto& [name, target] : targets_) {
            auto path = target->get_output_path();
            if (!path.empty()) output_to_target[path] = name;
        }

        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto& missing : graph.get_missing_dependencies()) {
                auto it = output_to_target.find(missing);
                if (it != output_to_target.end() && !targets_to_build.count(it->second)) {
                    targets_to_build.insert(it->second);
                    targets_[it->second]->generate_tasks(txn, get_root()->toolchain_, targets_, *this, exe_linker_flags, shared_linker_flags);
                    changed = true;
                }
            }
        }
        auto commit_result = txn.commit();
        if (!commit_result) {
            return std::unexpected(BuildError{current_file_, commit_result.error()});
        }
    }

    // Resolve circular deps
    for (auto& [name, target] : targets_) {
        target->resolve_deferred_circular_deps(targets_);
    }

    // Wire link dependencies
    for (const auto& name : targets_to_build) {
        auto target = targets_[name];
        if (target->get_type() == TargetType::STATIC_LIBRARY)
            continue;

        std::string out_path = target->get_output_path();
        std::string task_id = out_path.empty() ? target->get_name() : out_path;

        if (graph.has_task(task_id)) {
            auto& task = graph.get_task(task_id);
            for (const auto& lib : target->get_resolved_property("LINK_LIBRARIES")) {
                if (graph.has_task(lib)) {
                    task.inputs.push_back(lib);
                }
            }
        }
    }

    // Post-transaction: evaluate genex, resolve inferred file deps
    GenexEvaluationContext genex_ctx;
    genex_ctx.build_type = get_variable("CMAKE_BUILD_TYPE");
    genex_ctx.system_name = get_variable("CMAKE_SYSTEM_NAME");
    genex_ctx.cxx_compiler_id = get_variable("CMAKE_CXX_COMPILER_ID");
    genex_ctx.c_compiler_id = get_variable("CMAKE_C_COMPILER_ID");
    genex_ctx.cxx_compiler_version = get_variable("CMAKE_CXX_COMPILER_VERSION");
    genex_ctx.c_compiler_version = get_variable("CMAKE_C_COMPILER_VERSION");
    genex_ctx.all_targets = &targets_;
    genex_ctx.target_aliases = &target_aliases_;
    genex_ctx.install_prefix = get_variable("CMAKE_INSTALL_PREFIX");
    genex_ctx.phase = GenexEvaluationContext::Phase::BUILD;

    auto finalize_result = graph.evaluate_genex(genex_ctx);
    if (!finalize_result) {
        return std::unexpected(BuildError{current_file_, finalize_result.error()});
    }
    graph.resolve_inferred_file_deps();

    // Return full graph (not just dirty tasks)
    return std::move(graph);
}

Interpreter::Interpreter(std::string script_dir, std::ostream* out, std::ostream* err, std::optional<std::string> build_dir)
    : out_(out), err_(err) {

    std::filesystem::path abs_script_dir = script_dir.empty() ?
        std::filesystem::current_path() :
        std::filesystem::absolute(script_dir).lexically_normal();
    frame_stack_.push_front({abs_script_dir.string(), nullptr});

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

    variables_.set("DMAKE_VERSION", "0.1.0");

    variables_.set("CMAKE_SOURCE_DIR", variables_.get("CMAKE_CURRENT_SOURCE_DIR"));
    variables_.set("CMAKE_BINARY_DIR", abs_binary_dir.string());
    variables_.set("CMAKE_CURRENT_BINARY_DIR", abs_binary_dir.string());
    variables_.set("CMAKE_EXPORT_COMPILE_COMMANDS", "ON");

    // In config mode, create the binary directory at configure time (like CMake does)
    if (build_dir.has_value()) {
        std::error_code ec;
        std::filesystem::create_directories(abs_binary_dir, ec);
    }

    // Set default install prefix if not already set
    if (get_variable("CMAKE_INSTALL_PREFIX").empty()) {
        variables_.set("CMAKE_INSTALL_PREFIX", "/usr/local");
    }

    if (build_dir.has_value() && abs_binary_dir == abs_script_dir) {
        set_fatal_error("Build directory cannot be the same as the source directory: " + abs_script_dir.string());
    }

    variables_.set("CMAKE_VERSION", "3.22.0");
    variables_.set("CMAKE_MAJOR_VERSION", "3");
    variables_.set("CMAKE_MINOR_VERSION", "22");
    variables_.set("CMAKE_PATCH_VERSION", "0");

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
#endif

    variables_.set("CMAKE_EXECUTABLE_SUFFIX", "");
    variables_.set("CMAKE_SHARED_LIBRARY_PREFIX", "lib");
    variables_.set("CMAKE_SHARED_LIBRARY_SUFFIX", ".so");
    variables_.set("CMAKE_STATIC_LIBRARY_PREFIX", "lib");
    variables_.set("CMAKE_STATIC_LIBRARY_SUFFIX", ".a");
    variables_.set("CMAKE_SHARED_LIBRARY_C_FLAGS", "-fPIC");
    variables_.set("CMAKE_SHARED_LIBRARY_CXX_FLAGS", "-fPIC");
#ifdef __linux__
    variables_.set("CMAKE_DL_LIBS", "dl");
#endif

    variables_.set("CMAKE_COMMAND", get_executable_path());
    variables_.set("CMAKE_GENERATOR", "dmake");
    variables_.set("CMAKE_MAKE_PROGRAM", get_executable_path());
    variables_.set("CMAKE_ROOT", "/usr/share/cmake");

    // Initialize toolchain with compiler detection
    // See fake_cmake_compiler_checks_and_init() for what this does and its limitations
    // NOTE: This function directly modifies variables via set_variable(), which now uses ShadowMap
    fake_cmake_compiler_checks_and_init(*this);

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
    if (get_variable("CMAKE_FIND_NO_INSTALL_PREFIX") != "1"
        && get_variable("CMAKE_FIND_NO_INSTALL_PREFIX") != "ON"
        && get_variable("CMAKE_FIND_NO_INSTALL_PREFIX") != "TRUE") {
        if (!install_prefix.empty()) {
            system_prefix_path += ";" + install_prefix;
        }
        if (!staging_prefix.empty()) {
            system_prefix_path += ";" + staging_prefix;
        }
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
        CMakeArrayView path_view(system_prefix_path);
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

        // Initialize cache store
        std::filesystem::path cache_path = std::filesystem::path(abs_binary_dir) / ".dmake_subsystem_cache.json";
        cache_store_ = std::make_unique<CacheStore>(cache_path);
        auto res = cache_store_->load();  // Graceful - starts with empty cache if file doesn't exist
        (void)res;

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
            CMakeArrayView path_view(sys_prefix_path);
            for (auto entry : path_view) {
                if (!install_prefix.empty() && entry == install_prefix) ++icount;
                if (!staging_prefix.empty() && entry == staging_prefix) ++scount;
            }
            interp.set_variable("_CMAKE_SYSTEM_PREFIX_PATH_INSTALL_PREFIX_COUNT", std::to_string(icount));
            interp.set_variable("_CMAKE_SYSTEM_PREFIX_PATH_STAGING_PREFIX_COUNT", std::to_string(scount));
        });

        add_builtin("enable_testing", [](Interpreter& interp, const std::vector<std::string>& args) {
            if (!args.empty()) {
                interp.print_message("WARN", "enable_testing() expects no arguments");
            }
            interp.enable_testing_globally();
        });

        add_builtin("add_test", [](Interpreter& interp, const std::vector<std::string>& args) {
            if (!interp.is_testing_enabled()) {
                return;
            }

            if (args.empty()) {
                interp.set_fatal_error("add_test requires arguments");
                return;
            }

            // Warn if BUILD_TESTING is OFF but add_test is being called
            auto build_testing = interp.get_variable("BUILD_TESTING");
            if (Interpreter::is_falsy(build_testing)) {
                interp.print_message("WARNING", "add_test() called but BUILD_TESTING is OFF. Tests may not have been built. Use -DBUILD_TESTING=ON");
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
            for (size_t i = 1; i < raw_cmd.size(); ++i) {
                test.args.push_back(raw_cmd[i]);
            }
            test.working_dir = working_dir.empty() ? interp.get_variable("CMAKE_CURRENT_BINARY_DIR") : working_dir;

            interp.get_tests().push_back(std::move(test));
        });

        add_builtin("set_tests_properties", [](Interpreter& interp, const std::vector<std::string>& args) {
            if (!interp.is_testing_enabled()) {
                return;
            }

            // Supported test properties (explicitly tracked)
            static const std::unordered_set<std::string> SUPPORTED_PROPERTIES = {
                "TIMEOUT",
                "SKIP_RETURN_CODE",
                // Future properties can be added here:
                // "WILL_FAIL", "PASS_REGULAR_EXPRESSION", "FAIL_REGULAR_EXPRESSION",
                // "WORKING_DIRECTORY", "ENVIRONMENT", "LABELS", "DEPENDS", etc.
            };

            // Parse: set_tests_properties(test1 [test2...] PROPERTIES prop1 val1 [prop2 val2...])
            if (args.size() < 3) {
                interp.set_fatal_error("set_tests_properties requires at least one test name and PROPERTIES keyword");
                return;
            }

            // Find PROPERTIES keyword
            auto props_it = std::find(args.begin(), args.end(), "PROPERTIES");
            if (props_it == args.end()) {
                interp.set_fatal_error("set_tests_properties requires PROPERTIES keyword");
                return;
            }

            // Extract test names (everything before PROPERTIES)
            std::vector<std::string> test_names(args.begin(), props_it);
            if (test_names.empty()) {
                interp.set_fatal_error("set_tests_properties requires at least one test name");
                return;
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

                // Warn if property is not supported (but still set it)
                if (SUPPORTED_PROPERTIES.find(prop_name) == SUPPORTED_PROPERTIES.end()) {
                    interp.print_message("WARN", "Test property '" + prop_name + "' is not yet supported by dmake");
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
                        for (const auto& [key, value] : properties) {
                            test.properties[key] = value;
                        }
                        found = true;
                        break;
                    }
                }

                if (!found) {
                    interp.print_message("WARN", "Test '" + test_name + "' not found, cannot set properties");
                }
            }
        });

        register_find_package_builtins(*this);

        add_builtin("include_guard", [](Interpreter& interp, const std::vector<std::string>& args) {
            std::string scope = "DIRECTORY";
            if (!args.empty()) scope = args[0];

            std::string current_file = interp.get_current_file();
            if (current_file.empty()) return;

            if (scope == "GLOBAL") {
                interp.global_guarded_files_.insert(current_file);
            } else {
                interp.get_current_directory_context().guarded_files.insert(current_file);
            }
        });

        // Internal builtins (interact with interpreter state/stack)

        add_builtin("add_subdirectory", [](Interpreter& interp, const std::vector<std::string>& args) {
            if (args.empty()) return;
            std::string subdir = args[0];
            std::string current_source_dir = interp.get_variable("CMAKE_CURRENT_SOURCE_DIR");
            std::string current_binary_dir = interp.get_variable("CMAKE_CURRENT_BINARY_DIR");
            std::filesystem::path source_path = std::filesystem::path(current_source_dir) / subdir;
            std::filesystem::path abs_source_path = std::filesystem::absolute(source_path).lexically_normal();
            std::filesystem::path cmake_file = abs_source_path / "CMakeLists.txt";

            // Compute binary directory for the subdirectory
            // Check for explicit binary_dir argument (args[1]), skipping EXCLUDE_FROM_ALL
            std::filesystem::path binary_path;
            bool has_explicit_binary_dir = false;
            for (size_t i = 1; i < args.size(); ++i) {
                if (args[i] != "EXCLUDE_FROM_ALL") {
                    binary_path = std::filesystem::path(current_binary_dir) / args[i];
                    has_explicit_binary_dir = true;
                    break;
                }
            }
            if (!has_explicit_binary_dir) {
                if (std::filesystem::path(subdir).is_absolute()) {
                    // For absolute source paths without explicit binary dir,
                    // use the last component as subdirectory name under current binary dir
                    binary_path = std::filesystem::path(current_binary_dir) / std::filesystem::path(subdir).filename();
                } else {
                    binary_path = std::filesystem::path(current_binary_dir) / subdir;
                }
            }

            // Create binary directory (CMake does this implicitly)
            std::error_code ec;
            std::filesystem::create_directories(binary_path, ec);

            if (!std::filesystem::exists(cmake_file)) {
                interp.set_fatal_error("CMakeLists.txt not found in " + abs_source_path.string());
                return;
            }

            std::ifstream file(cmake_file);
            if(!file) { interp.set_fatal_error("Could not read " + cmake_file.string()); return; }
            std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

            // Save current file for restoration
            std::string saved_file = interp.get_current_file();

            // Push variable scope and directory context
            interp.get_variables().push_scope();
            interp.push_directory(abs_source_path.string(), binary_path.string());

            // Set CMAKE_CURRENT_* variables for the subdirectory
            interp.set_variable("CMAKE_CURRENT_SOURCE_DIR", abs_source_path.string());
            interp.set_variable("CMAKE_CURRENT_BINARY_DIR", binary_path.string());
            interp.set_variable("CMAKE_CURRENT_LIST_FILE", cmake_file.string());
            interp.set_variable("CMAKE_CURRENT_LIST_DIR", abs_source_path.string());
            interp.set_current_file(cmake_file.string());

            // Parse and interpret the subdirectory CMakeLists.txt
            std::string subdir_filename = cmake_file.filename().string();

            ProfileScope parse_profile("parse " + subdir + "/" + subdir_filename, "parse");
            Parser parser(content, cmake_file.string());
            auto ast = parser.parse();
            parse_profile.stop();

            if (interp.get_debugger()) interp.get_debugger()->push_call_depth();

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
                        interp.finalize_directory_targets();  // Apply retroactive properties
                    }
                } else {
                    interp.set_fatal_error(InterpreterError{cmake_file.string(), ast.error().row, ast.error().col, ast.error().offset, ast.error().length, ast.error().reason, {}});
                }
            }

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
            if (args.empty()) { interp.set_fatal_error("include() requires an argument"); return; }
            bool optional = false;
            for (size_t i = 1; i < args.size(); ++i) if (args[i] == "OPTIONAL") optional = true;

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
                    if (value.has_value()) {
                        interp.get_variables().set_parent_scope(var_name, *value);
                    } else {
                        // Variable not defined - unset in parent scope
                        interp.get_variables().unset_parent_scope(var_name);
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
                     code += args[i];
                 }

                 Parser p(code);
                 auto ast = p.parse();
                 if (!ast) {
                     std::vector<CallLocation> backtrace;
                     Interpreter* root = interp.get_root();
                     if (!root->trace_stack_.empty()) {
                         // For parse errors during EVAL, the current command (cmake_language)
                         // should be part of the backtrace.
                         backtrace = root->trace_stack_;
                     }
                     InterpreterError err{"<EVAL>", ast.error().row, ast.error().col, ast.error().offset, ast.error().length, "cmake_language(EVAL) parse error: " + ast.error().reason, backtrace, code};
                     interp.set_fatal_error(err);
                     return;
                 }

                 std::string old_file = interp.get_current_file();
                 interp.set_current_file("<EVAL>");
                 auto res = interp.interpret(ast.value());
                 interp.set_current_file(old_file);

                 if (!res) {
                     InterpreterError err = res.error();
                     if (err.file == "<EVAL>" && !err.source_content) {
                         err.source_content = code;
                     }
                     interp.set_fatal_error(err);
                 }

            } else if (args[0] == "CALL") {
                 if (args.size() < 2) {
                     interp.set_fatal_error("cmake_language(CALL) requires a command name");
                     return;
                 }
                 std::string cmd = args[1];
                 std::vector<std::string> cmd_args;
                 if (args.size() > 2) {
                     cmd_args.assign(args.begin() + 2, args.end());
                 }

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
                         break;  // Must be a subcommand
                     }
                 }

                 if (!target_ctx) {
                     target_ctx = &interp.get_root()->get_current_directory_context();
                 }

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
                     if (!id_var.empty()) {
                         interp.set_variable(id_var, call_id);
                     }
                     DeferredCall dc;
                     dc.id = call_id;
                     dc.command = args[i + 1];
                     dc.arguments.assign(args.begin() + i + 2, args.end());
                     target_ctx->deferred_calls.push_back(std::move(dc));
                 } else if (subcmd == "CANCEL_CALL") {
                     std::set<std::string> ids_to_cancel(args.begin() + i + 1, args.end());
                     auto& calls = target_ctx->deferred_calls;
                     std::erase_if(calls, [&](const DeferredCall& dc) {
                         return ids_to_cancel.count(dc.id) > 0;
                     });
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
            } else {
                interp.set_fatal_error("Unknown cmake_language mode: " + args[0]);
            }
        });
}

std::expected<void, InterpreterError> Interpreter::interpret(const std::vector<AstNode>& ast) {
    for (const auto& node : ast) {
        std::expected<void, InterpreterError> res;
        if (std::holds_alternative<CommandInvocation>(node)) res = execute_command(std::get<CommandInvocation>(node));
        else if (std::holds_alternative<IfBlock>(node)) res = execute_if_block(std::get<IfBlock>(node));
        else if (std::holds_alternative<FunctionBlock>(node)) res = execute_function_block(std::get<FunctionBlock>(node));
        else if (std::holds_alternative<MacroBlock>(node)) res = execute_macro_block(std::get<MacroBlock>(node));
        else if (std::holds_alternative<ForeachBlock>(node)) res = execute_foreach_block(std::get<ForeachBlock>(node));
        else if (std::holds_alternative<WhileBlock>(node)) res = execute_while_block(std::get<WhileBlock>(node));
        else if (std::holds_alternative<BlockBlock>(node)) res = execute_block_block(std::get<BlockBlock>(node));

        if (!res) return res;
        if (loop_control_ != LoopControl::NONE) return {};
        if (return_requested_) return {};  // Early return from script/function
    }
    return {};
}

std::expected<void, InterpreterError> Interpreter::include_file(const std::string& file_path, bool optional) {
    std::filesystem::path path = std::filesystem::path(file_path);

    if(file_path.ends_with("CPack") || file_path.ends_with("CPack.cmake")) {
        print_message("WARNING", "dmake does not support cpack (yet). Ignoring..");
        return {};
    }

    if (file_path == "ExternalProject" || file_path.ends_with("ExternalProject.cmake") || file_path.ends_with("/ExternalProject")) {
        register_external_project_builtins(*this);
        return {};
    }

    if (file_path == "FetchContent" || file_path.ends_with("FetchContent.cmake") || file_path.ends_with("/FetchContent")) {
        register_fetch_content_builtins(*this);
        return {};
    }



    // Helper to check if a path exists, trying with and without .cmake extension
    auto try_path = [this](const std::filesystem::path& p) -> std::optional<std::filesystem::path> {
        if (cached_file_exists(p)) {
            // Verify it's not a directory
            std::error_code ec;
            if (!std::filesystem::is_directory(p, ec)) return p;
        }
        if (!p.has_extension()) {
            std::filesystem::path with_ext = p;
            with_ext.replace_extension(".cmake");
            if (cached_file_exists(with_ext)) {
                std::error_code ec;
                if (!std::filesystem::is_directory(with_ext, ec)) return with_ext;
            }
        }
        return std::nullopt;
    };

    auto find_in_dir = [&try_path](const std::filesystem::path& dir, const std::string& file_path) -> std::optional<std::filesystem::path> {
        auto candidate = dir / file_path;
        return try_path(candidate);
    };

    std::optional<std::filesystem::path> found_path;

    if (path.is_absolute()) {
        found_path = try_path(path);
    } else {
        // Try relative to CMAKE_CURRENT_SOURCE_DIR first
        std::filesystem::path candidate = std::filesystem::path(get_variable("CMAKE_CURRENT_SOURCE_DIR")) / file_path;
        found_path = try_path(candidate);

        // If not found, search CMAKE_MODULE_PATH
        if (!found_path) {
            auto module_path_str = get_variable("CMAKE_MODULE_PATH");
            for (auto dir : CMakeArrayView(module_path_str)) {
                if (!dir.empty()) {
                    found_path = find_in_dir(dir, file_path);
                    if (found_path) break;
                }
            }
        }

        // If not found, search system paths
        if (!found_path) {
            std::vector<std::string> system_modules = {
                "/usr/share/cmake/Modules",
                "/usr/local/share/cmake/Modules",
                "/usr/lib/cmake/Modules",
                "/usr/lib/x86_64-linux-gnu/cmake/Modules"
            };
            for (const auto& dir : system_modules) {
                found_path = find_in_dir(dir, file_path);
                if (found_path) break;
            }
        }
    }

    if (!found_path) {
        if (optional) return {};
        return std::unexpected(InterpreterError{current_file_, current_cmd_row_, current_cmd_col_, 0, 0, "include() could not find: " + file_path, {}});
    }

    path = *found_path;

    // Check if it's a directory (reject directories, but accept symlinks to files)
    if (std::filesystem::is_directory(path)) {
        if (optional) return {};
        return std::unexpected(InterpreterError{current_file_, current_cmd_row_, current_cmd_col_, 0, 0, "include() path is a directory: " + path.string(), {}});
    }

    std::string abs_path = std::filesystem::absolute(path).string();

    // Check include guards
    if (global_guarded_files_.contains(abs_path)) return {};
    if (get_current_directory_context().guarded_files.contains(abs_path)) return {};

    // Check AST cache for frequently-included system modules
    const std::vector<AstNode>* cached_ast = ast_cache_.get(abs_path);
    const std::vector<AstNode>* ast_to_use = nullptr;
    std::expected<std::vector<AstNode>, ParseError> parsed_ast;

    if (cached_ast) {
        ast_to_use = cached_ast;
    } else {
        std::ifstream file(path);
        if (!file) {
            return std::unexpected(InterpreterError{path.string(), 0, 0, 0, 0, "include() could not read: " + path.string(), {}});
        }

        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

        ProfileScope parse_profile("parse " + abs_path, "parse");
        Parser parser(content, path.string());
        parsed_ast = parser.parse();
        parse_profile.stop();

        if (!parsed_ast) {
            return std::unexpected(InterpreterError{path.string(), parsed_ast.error().row, parsed_ast.error().col, parsed_ast.error().offset, parsed_ast.error().length, parsed_ast.error().reason, {}});
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
    std::string abs_dir = std::filesystem::path(abs_path).parent_path().string();

    std::string old_list_file = get_variable("CMAKE_CURRENT_LIST_FILE");
    std::string old_list_dir = get_variable("CMAKE_CURRENT_LIST_DIR");

    set_variable("CMAKE_CURRENT_LIST_FILE", abs_path);
    set_variable("CMAKE_CURRENT_LIST_DIR", abs_dir);
    set_current_file(abs_path);

    if (debugger_) debugger_->push_call_depth();

    std::expected<void, InterpreterError> res;
    {
        // return() in included file shouldn't propagate to caller
        ReturnGuard rg(*this);

        ProfileScope interpret_profile("interpret " + abs_path, "interpret");
        res = interpret(*ast_to_use);
        interpret_profile.stop();
    }

    if (debugger_) debugger_->pop_call_depth();

    set_variable("CMAKE_CURRENT_LIST_FILE", old_list_file);
    set_variable("CMAKE_CURRENT_LIST_DIR", old_list_dir);
    set_current_file(old_file);

    return res;
}

const std::unordered_set<std::string>* Interpreter::get_directory_listing(const std::filesystem::path& dir) {
    Interpreter* root = get_root();

    // Normalize to absolute path
    std::error_code ec;
    std::filesystem::path abs_dir = std::filesystem::absolute(dir, ec);
    if (ec) return nullptr;

    std::string dir_key = abs_dir.string();

    // Check if directory exists
    if (!std::filesystem::exists(abs_dir, ec) || ec) return nullptr;
    if (!std::filesystem::is_directory(abs_dir, ec) || ec) return nullptr;

    // Get current directory mtime
    std::filesystem::file_time_type current_mtime;
    try {
        current_mtime = std::filesystem::last_write_time(abs_dir, ec);
        if (ec) return nullptr;
    } catch (...) {
        return nullptr;
    }

    // Clock skew detection
    auto now = std::filesystem::file_time_type::clock::now();
    if (current_mtime > now) {
        // Print warning once per directory
        static std::unordered_set<std::string> warned_dirs;
        if (!warned_dirs.contains(dir_key)) {
            *err_ << colors::YELLOW << "Warning: Directory has modification time in the future: "
                  << dir_key << ". Possible clock skew detected." << colors::RESET << std::endl;
            warned_dirs.insert(dir_key);
        }
        // Invalidate cache entry
        root->dir_scan_cache_.erase(dir_key);
        // Continue with fresh scan
    }

    // Look up in session cache
    auto it = root->dir_scan_cache_.find(dir_key);
    if (it != root->dir_scan_cache_.end()) {
        // Cache hit - validate mtime
        if (it->second.mtime == current_mtime) {
            return &it->second.entries;
        }
        // Mtime changed - invalidate
        root->dir_scan_cache_.erase(it);
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
            for (const auto& f : persistent->files) {
                cache_entry.entries.insert(f);
            }
            for (const auto& d : persistent->subdirs) {
                cache_entry.entries.insert(d);
                cache_entry.subdirs.insert(d);
            }
            auto [ins_it, _] = root->dir_scan_cache_.emplace(dir_key, std::move(cache_entry));
            return &ins_it->second.entries;
        }
    }

    // Cache miss - scan directory
    std::unordered_set<std::string> entries;
    std::unordered_set<std::string> subdirs;
    try {
        for (const auto& entry : std::filesystem::directory_iterator(abs_dir, ec)) {
            if (ec) {
                // Error during iteration - don't cache
                return nullptr;
            }
            auto name = entry.path().filename().string();
            entries.insert(name);
            std::error_code ec2;
            if (entry.is_directory(ec2) && !ec2) {
                subdirs.insert(name);
            }
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
    return &inserted_it->second.entries;
}

const std::unordered_set<std::string>* Interpreter::get_directory_subdirs(const std::filesystem::path& dir) {
    // Ensure the directory is cached (populates entries + subdirs)
    if (!get_directory_listing(dir)) return nullptr;

    std::error_code ec;
    std::string dir_key = std::filesystem::absolute(dir, ec).string();
    if (ec) return nullptr;

    auto it = get_root()->dir_scan_cache_.find(dir_key);
    if (it == get_root()->dir_scan_cache_.end()) return nullptr;
    return &it->second.subdirs;
}

bool Interpreter::cached_file_exists(const std::filesystem::path& full_path) {
    // Split path into directory and filename
    auto parent = full_path.parent_path();
    auto filename = full_path.filename().string();

    if (filename.empty()) return false;

    // Try to get cached listing of parent directory
    // This will populate the cache on-demand if not already cached
    auto* entries = get_directory_listing(parent);
    if (!entries) {
        // Cache population failed (permissions, doesn't exist, etc.)
        // Fall back to direct filesystem check
        std::error_code ec;
        return std::filesystem::exists(full_path, ec);
    }

    return entries->contains(filename);
}

bool Interpreter::cached_file_exists(const std::filesystem::path& dir, const std::string& filename) {
    auto* entries = get_directory_listing(dir);
    return entries && entries->contains(filename);
}

void Interpreter::set_fatal_error(const std::string& message) {
    // If debugger is attached, drop into it before dying
    if (debugger_) {
        debugger_->on_fatal_error(message);
    }

    Interpreter* root = get_root();
    std::vector<CallLocation> backtrace;
    if (!root->trace_stack_.empty()) {
        // The last element is the current command, we want everything BEFORE it as backtrace
        for (size_t i = 0; i < root->trace_stack_.size() - 1; ++i) {
            backtrace.push_back(root->trace_stack_[i]);
        }

        const auto& current = root->trace_stack_.back();
        set_fatal_error(InterpreterError{current.file, current.row, current.col, current.offset, current.length, message, backtrace});
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

std::vector<std::string> Interpreter::expand_arguments(const std::vector<Argument>& args) {
    std::vector<std::string> result;
    for (const auto& arg : args) {
        std::string val = evaluate_argument(arg);
        if (arg.quoted) {
            result.push_back(val);
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
            for (auto item : CMakeArrayView(val)) {
                // CMake removes empty elements when expanding variable references
                if (has_var_ref && item.empty()) {
                    continue;
                }
                result.emplace_back(item);
            }
        }
    }
    return result;
}

std::expected<void, InterpreterError> Interpreter::execute_command(const CommandInvocation& cmd) {
    current_cmd_row_ = cmd.row;
    current_cmd_col_ = cmd.col;

    // Push to backtrace stack
    Interpreter* root = get_root();
    root->trace_stack_.push_back({current_file_, cmd.row, cmd.col, cmd.offset, cmd.length, cmd.identifier});

    // Expand arguments once
    std::vector<std::string> expanded_args = expand_arguments(cmd.arguments);

    // Debugger/trace hook - must happen after argument expansion
    if (debugger_) {
        debugger_->on_command(current_file_, cmd.row, cmd.col,
                              cmd.identifier, expanded_args, cmd.arguments);
    }

    auto res = execute_command_with_args(cmd.identifier, expanded_args);

    if (!res) {
        if (auto err = get_fatal_error()) {
             // The error is already set, just clean up stack and return it
             safe_pop_trace_stack("error in: " + cmd.identifier);
             return std::unexpected(*err);
        }
    }

    safe_pop_trace_stack("command: " + cmd.identifier);
    return res;
}

std::expected<void, InterpreterError> Interpreter::execute_command_with_args(const std::string& identifier, const std::vector<std::string>& args) {
    Interpreter* root = get_root();
    std::string lower_identifier = to_lower(identifier);

    auto bit = root->builtins_.find(lower_identifier);
    if (bit != root->builtins_.end()) {
        bit->second(*this, args);
        if (auto err = get_fatal_error()) {
            return std::unexpected(*err);
        }
        return {};
    }

    // Look up user functions/macros at root - CMake functions are globally visible
    auto fit = root->user_functions_.find(lower_identifier);
    if (fit != root->user_functions_.end()) {
        return invoke_user_function(*fit->second, args);
    }
    auto mit = root->user_macros_.find(lower_identifier);
    if (mit != root->user_macros_.end()) {
        return invoke_user_macro(*mit->second, args);
    }

    set_fatal_error("Unknown command: " + identifier);
    return std::unexpected(*get_fatal_error());
}

std::expected<void, InterpreterError> Interpreter::execute_if_block(const IfBlock& if_block) {
    Interpreter* root = get_root();
    root->trace_stack_.push_back({current_file_, if_block.row, if_block.col, if_block.offset, if_block.length, "if"});

    // CMake argument elision and list expansion: Remove arguments that evaluate
    // to empty strings, and split semicolon-separated lists into separate arguments
    // when they come from variable references (unquoted arguments with ${...})
    auto filter_empty_args = [&](const std::vector<Argument>& args) -> std::vector<Argument> {
        std::vector<Argument> filtered;
        for (const auto& arg : args) {
            // Check if argument contains variable references
            bool has_var_ref = false;
            for (const auto& part : arg.parts) {
                if (std::holds_alternative<VariableReference>(part)) {
                    has_var_ref = true;
                    break;
                }
            }

            // If it has variable references and is unquoted, expand and split lists
            if (has_var_ref && !arg.quoted) {
                std::string val = evaluate_argument(arg);
                if (val.empty()) {
                    continue;  // Skip this argument (elision)
                }
                // Split semicolon-separated lists into separate arguments
                for (auto item : CMakeArrayView(val)) {
                    if (item.empty()) continue;
                    Argument new_arg;
                    new_arg.quoted = false;
                    new_arg.parts.push_back(std::string(item));
                    filtered.push_back(std::move(new_arg));
                }
            } else {
                filtered.push_back(arg);
            }
        }
        return filtered;
    };

    auto filtered_condition = filter_empty_args(if_block.condition);
    auto cond_result = evaluate_condition(filtered_condition, if_block.row, if_block.col, if_block.offset, if_block.length);
    if (!cond_result) {
        set_fatal_error(cond_result.error());
        safe_pop_trace_stack("if block condition error");
        return std::unexpected(cond_result.error());
    }

    if (cond_result.value()) {
        auto res = interpret(if_block.then_branch);
        if (!res) set_fatal_error(res.error());
        safe_pop_trace_stack("if block");
        return res;
    }

    for (const auto& elseif : if_block.elseif_branches) {
        auto filtered_elseif_cond = filter_empty_args(elseif.condition);
        auto elseif_cond = evaluate_condition(filtered_elseif_cond, elseif.row, elseif.col, elseif.offset, elseif.length);
        if (!elseif_cond) {
            set_fatal_error(elseif_cond.error());
            safe_pop_trace_stack("elseif block condition error");
            return std::unexpected(elseif_cond.error());
        }
        if (elseif_cond.value()) {
            auto res = interpret(elseif.body);
            if (!res) set_fatal_error(res.error());
            safe_pop_trace_stack("elseif block");
            return res;
        }
    }

    auto res = interpret(if_block.else_branch);
    if (!res) set_fatal_error(res.error());
    safe_pop_trace_stack("if block");
    return res;
}

std::expected<void, InterpreterError> Interpreter::execute_function_block(const FunctionBlock& block) {
    std::string lower_name = to_lower(block.name);
    // Store at root - CMake functions/macros are globally visible
    Interpreter* root = get_root();

    // Single lookup: try to insert nullptr, get iterator to existing or new entry
    auto [it, inserted] = root->user_functions_.try_emplace(lower_name, nullptr);
    if (!inserted && it->second) {
        // Replacing existing function - check if it's currently executing
        const FunctionBlock* old_ptr = it->second.get();
        size_t deepest_index = 0;
        bool found = false;
        for (size_t i = 0; i < root->frame_stack_.size(); ++i) {
            if (root->frame_stack_[i].function_block == old_ptr) {
                deepest_index = i;
                found = true;
            }
        }
        if (found) {
            // Defer deletion until we've popped past this frame
            size_t delete_when = root->frame_stack_.size() - (deepest_index + 1);
            root->deferred_function_deletions_.emplace_back(delete_when, std::move(it->second));
        }
    }

    it->second = std::make_unique<FunctionBlock>(block);
    root->user_macros_.erase(lower_name);
    return {};
}

std::expected<void, InterpreterError> Interpreter::execute_macro_block(const MacroBlock& block) {
    std::string lower_name = to_lower(block.name);
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
    root->user_functions_.erase(lower_name);
    return {};
}

std::expected<void, InterpreterError> Interpreter::execute_foreach_block(const ForeachBlock& block) {
    Interpreter* root = get_root();
    root->trace_stack_.push_back({current_file_, block.row, block.col, block.offset, block.length, "foreach"});

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
            for (size_t i = 0; i < zip.lists.size(); ++i) {
                var_names_to_save.push_back(zip.loop_vars[0] + "_" + std::to_string(i));
            }
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

        // Iterate over all lists in parallel
        for (size_t i = 0; i < max_length; ++i) {
            // Set loop variables for this iteration
            for (size_t list_idx = 0; list_idx < evaluated_lists.size(); ++list_idx) {
                const auto& list = evaluated_lists[list_idx];
                std::string value = (i < list.size()) ? list[i] : "";  // Pad with empty string

                // Determine variable name
                std::string var_name;
                if (zip.loop_vars.size() == 1) {
                    // Single loop var: set var_0, var_1, etc.
                    var_name = zip.loop_vars[0] + "_" + std::to_string(list_idx);
                } else {
                    // Multiple loop vars: set directly (if we have enough variables)
                    if (list_idx < zip.loop_vars.size()) {
                        var_name = zip.loop_vars[list_idx];
                    } else {
                        // More lists than variables - skip extra lists
                        continue;
                    }
                }

                set_variable(var_name, value);
            }

            // Execute loop body
            auto res = interpret(block.body);
            if (!res) {
                set_fatal_error(res.error());
                if (loop_depth_ <= 0) {
                    std::cerr << "FATAL: loop_depth_ is " << loop_depth_ << " when handling foreach body error\n";
                    std::abort();
                }
                loop_depth_--;
                // Restore all loop variables
                for (size_t j = 0; j < saved_vars.size(); ++j) {
                    if (saved_vars[j].second) {
                        set_variable(saved_vars[j].first, saved_values[j]);
                    } else {
                        unset_variable(saved_vars[j].first);
                    }
                }
                safe_pop_trace_stack("foreach zip_lists body error");
                return res;
            }

            if (loop_control_ == LoopControl::BREAK) { clear_loop_control(); break; }
            if (loop_control_ == LoopControl::CONTINUE) clear_loop_control();
            if (return_requested_) break;
        }

        // Restore all loop variables
        for (size_t j = 0; j < saved_vars.size(); ++j) {
            if (saved_vars[j].second) {
                set_variable(saved_vars[j].first, saved_values[j]);
            } else {
                unset_variable(saved_vars[j].first);
            }
        }

        if (loop_depth_ <= 0) {
            std::cerr << "FATAL: loop_depth_ is " << loop_depth_ << " when trying to decrement in foreach\n";
            std::abort();
        }
        loop_depth_--;
        safe_pop_trace_stack("foreach zip_lists");
        return {};
    }

    // Original logic for non-ZIP_LISTS modes
    // Evaluate the loop variable name (may contain variable references like _${PREFIX}_VAR)
    std::string loop_var_name = evaluate_argument(block.loop_var);

    // Save the loop variable's previous value (for nested loops)
    bool loop_var_was_set = is_variable_set(loop_var_name);
    std::string loop_var_old_value;
    if (loop_var_was_set) {
        loop_var_old_value = get_variable(loop_var_name);
    }

    loop_depth_++;
    CMakeArray items;
    if (std::holds_alternative<ForeachSimple>(block.params)) {
        // CMake filters out empty elements during unquoted expansion in simple foreach
        auto expanded = expand_arguments(std::get<ForeachSimple>(block.params).items);
        std::erase_if(expanded, [](const std::string& s) { return s.empty(); });
        items = from_arguments(expanded);
    } else if (std::holds_alternative<ForeachRange>(block.params)) {
        const auto& r = std::get<ForeachRange>(block.params);
        long start = r.start ? std::stol(evaluate_argument(*r.start)) : 0;
        long stop = std::stol(evaluate_argument(r.stop));
        // If step not provided, infer direction from start/stop
        long step;
        if (r.step) {
            step = std::stol(evaluate_argument(*r.step));
        } else {
            // Default step: 1 if ascending, -1 if descending
            step = (start <= stop) ? 1 : -1;
        }
        if (step == 0) {
            if (loop_depth_ <= 0) {
                std::cerr << "FATAL: loop_depth_ is " << loop_depth_ << " when handling step=0 error\n";
                std::abort();
            }
            loop_depth_--;
            set_fatal_error("Step cannot be zero");
            auto err = get_fatal_error();
            clear_fatal_error();
            safe_pop_trace_stack("foreach step=0 error");
            return std::unexpected(*err);
        }
        for (long i = start; (step > 0) ? (i <= stop) : (i >= stop); i += step) items.append(std::to_string(i));
    } else if (std::holds_alternative<ForeachIn>(block.params)) {
        const auto& in = std::get<ForeachIn>(block.params);
        for (const auto& l : in.lists) items.append(CMakeArray(get_variable(evaluate_argument(l))));
        items.append(from_arguments(expand_arguments(in.items)));
    }

    for (const auto& item : items) {
        set_variable(loop_var_name, item);
        auto res = interpret(block.body);
        if (!res) {
            set_fatal_error(res.error());
            if (loop_depth_ <= 0) {
                std::cerr << "FATAL: loop_depth_ is " << loop_depth_ << " when handling foreach body error\n";
                std::abort();
            }
            loop_depth_--;
            // Restore loop variable before returning
            if (loop_var_was_set) {
                set_variable(loop_var_name, loop_var_old_value);
            } else {
                unset_variable(loop_var_name);
            }
            safe_pop_trace_stack("foreach body error");
            return res;
        }
        if (loop_control_ == LoopControl::BREAK) { clear_loop_control(); break; }
        if (loop_control_ == LoopControl::CONTINUE) clear_loop_control();
        if (return_requested_) break;  // return() exits the loop and propagates to caller
    }

    // Restore loop variable after loop completes
    if (loop_var_was_set) {
        set_variable(loop_var_name, loop_var_old_value);
    } else {
        unset_variable(loop_var_name);
    }

    // Sanity check: loop depth should never go negative
    if (loop_depth_ <= 0) {
        std::cerr << "FATAL: loop_depth_ is " << loop_depth_ << " when trying to decrement in foreach\n";
        std::abort();
    }
    loop_depth_--;
    safe_pop_trace_stack("foreach");
    return {};
}

std::expected<void, InterpreterError> Interpreter::execute_while_block(const WhileBlock& block) {
    Interpreter* root = get_root();
    root->trace_stack_.push_back({current_file_, block.row, block.col, block.offset, block.length, "while"});

    loop_depth_++;

    // Evaluate condition and loop
    while (true) {
        auto cond_result = evaluate_condition(block.condition, block.row, block.col, block.offset, block.length);
        if (!cond_result) {
            loop_depth_--;
            safe_pop_trace_stack("while condition error");
            return std::unexpected(cond_result.error());
        }

        // Break if condition is false
        if (!cond_result.value()) {
            break;
        }

        // Execute body
        auto res = interpret(block.body);
        if (!res) {
            loop_depth_--;
            safe_pop_trace_stack("while body error");
            return res;
        }

        if (loop_control_ == LoopControl::BREAK) { clear_loop_control(); break; }
        if (loop_control_ == LoopControl::CONTINUE) clear_loop_control();
        if (return_requested_) break;  // return() exits the loop and propagates to caller
    }

    loop_depth_--;
    safe_pop_trace_stack("while");
    return {};
}

std::expected<void, InterpreterError> Interpreter::execute_block_block(const BlockBlock& block) {
    Interpreter* root = get_root();
    root->trace_stack_.push_back({current_file_, block.row, block.col, block.offset, block.length, "block"});

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
                if (variables_.is_defined(var_name)) {
                    propagated_values[var_name] = variables_.get(var_name);
                }
            }

            // Pop the block scope
            variables_.pop_scope();

            // Set propagated variables in parent scope
            for (const auto& [var_name, value] : propagated_values) {
                variables_.set(var_name, value);
            }
        } else {
            // Pop the block scope without propagation
            variables_.pop_scope();
        }

        if (!res) {
            safe_pop_trace_stack("block error");
            return res;
        }
    } else {
        // No variable scope, just execute the body
        auto res = interpret(block.body);
        if (!res) {
            safe_pop_trace_stack("block error");
            return res;
        }
    }

    safe_pop_trace_stack("block");
    return {};
}

std::expected<void, InterpreterError> Interpreter::invoke_user_function(const FunctionBlock& func, const std::vector<std::string>& args) {
    // Push metadata frame (for script_dir and function_block tracking)
    frame_stack_.push_front({func.definition_dir, &func});

    // Push variable scope
    variables_.push_scope();

    // Set function parameters
    CMakeArray all(args);
    variables_.set("ARGC", std::to_string(all.size()));
    variables_.set("ARGV", all.to_string());
    variables_.set("ARGN", all.sublist(func.parameters.size(), all.size()).to_string());
    for (size_t i = 0; i < all.size(); ++i) {
        variables_.set("ARGV" + std::to_string(i), all[i]);
    }
    for (size_t i = 0; i < func.parameters.size() && i < all.size(); ++i) {
        variables_.set(func.parameters[i], all[i]);
    }


    int saved_depth = loop_depth_;
    LoopControl saved_control = loop_control_;
    std::string saved_file = current_file_;
    // Save and clear macro substitutions - functions create a new scope so outer
    // macro parameters should not bleed into the function's variable lookups
    std::map<std::string, std::string> saved_macro_substitutions = macro_substitutions_;
    macro_substitutions_.clear();

    loop_depth_ = 0;
    loop_control_ = LoopControl::NONE;
    current_file_ = func.definition_file;

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
    frame_stack_.pop_front();

    // Clean up any deferred function deletions we've passed
    Interpreter* root = get_root();
    std::erase_if(root->deferred_function_deletions_, [&](const auto& entry) {
        return root->frame_stack_.size() <= entry.first;
    });

    if (!res) set_fatal_error(res.error());

    loop_depth_ = saved_depth;
    loop_control_ = saved_control;
    current_file_ = saved_file;
    macro_substitutions_ = saved_macro_substitutions;
    return res;
}

bool Interpreter::call_user_function(const std::string& name, const std::vector<std::string>& args) {
    // Normalize to lowercase for lookup
    std::string lower_name = name;
    std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(),
                  [](unsigned char c){ return std::tolower(c); });

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
    // Save and set up macro parameter substitutions (text-replacement, not variables)
    std::map<std::string, std::string> saved_substitutions = macro_substitutions_;

    CMakeArray all(args);
    macro_substitutions_["ARGC"] = std::to_string(all.size());
    macro_substitutions_["ARGV"] = all.to_string();
    macro_substitutions_["ARGN"] = all.sublist(macro.parameters.size(), all.size()).to_string();
    for (size_t i = 0; i < all.size(); ++i) {
        macro_substitutions_["ARGV" + std::to_string(i)] = all[i];
    }
    for (size_t i = 0; i < macro.parameters.size() && i < all.size(); ++i) {
        macro_substitutions_[macro.parameters[i]] = all[i];
    }

    std::string saved_file = current_file_;
    current_file_ = macro.definition_file;

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
    std::erase_if(root->deferred_macro_deletions_, [&lower_name, depth](const auto& entry) {
        return entry.name == lower_name && entry.depth > static_cast<size_t>(depth);
    });
    if (depth == 0) {
        root->macro_execution_depth_.erase(lower_name);
    }

    current_file_ = saved_file;
    if (!res) set_fatal_error(res.error());

    // Restore macro substitutions
    macro_substitutions_ = saved_substitutions;

    return res;
}

// Allowlist-based truthy check matching CMake's cmIsOn().
// Only 1/Y/ON/YES/TRUE (case-insensitive) are truthy.
// This is NOT the inverse of is_falsy() — values like "/usr/local/bin"
// are neither is_truthy() nor is_falsy().
bool Interpreter::is_truthy(const std::string& val) {
    auto upper = to_upper(val);
    return upper == "1" || upper == "Y" || upper == "ON" ||
           upper == "YES" || upper == "TRUE";
}

bool Interpreter::is_falsy(const std::string& val) {
    if (val.empty()) return true;

    // Exact match for "0" only (not "00", "0.0", "-0", etc.)
    if (val == "0") return true;

    std::string upper_val = to_upper(val);

    // False constants (case-insensitive, exact match)
    if (upper_val == "OFF" || upper_val == "NO" ||
        upper_val == "FALSE" || upper_val == "N" ||
        upper_val == "IGNORE" || upper_val == "NOTFOUND") {
        return true;
    }

    // Ends with -NOTFOUND
    if (upper_val.ends_with("-NOTFOUND")) {
        return true;
    }

    // Everything else is truthy (including "2x", "0.0", "00", "-0", etc.)
    return false;
}

std::expected<bool, InterpreterError> Interpreter::evaluate_condition(const std::vector<Argument>& condition, size_t row, size_t col, size_t offset, size_t length) {
    return dmake::evaluate_condition(*this, condition, row, col, offset, length);
}

bool Interpreter::has_user_function(const std::string& name) const {
    std::string lower_name = to_lower(name);
    return get_root()->user_functions_.contains(lower_name);
}

std::string Interpreter::evaluate_variable_reference(const VariableReference& ref) {
    // Fast path: single string name_part with no namespace (most common: ${SIMPLE_VAR})
    // Avoids copying the variable name - uses string_view directly from AST
    if (ref.name_parts.size() == 1 &&
        std::holds_alternative<std::string>(ref.name_parts[0]) &&
        ref.namespace_prefix.empty()) {
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
        // Regular variable lookup
        return get_variable(name);
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
    return get_optional_variable(name).value_or("");
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
    *err_ << "-- variable_watch: " << name << " was " << access_type
           << ", value=\"" << value << "\", in file " << current_file_ << "\n";

    // If callback function specified, call it
    if (it->second.callback_function) {
        call_user_function(*it->second.callback_function,
                           {name, access_type, value, current_file_, "UNKNOWN"});
    }

    // Notify debugger if present
    if (debugger_) {
        debugger_->on_variable_access(name, access_type, value, current_file_);
    }
}

void Interpreter::set_variable(const std::string& name, const std::string& val) {
    variables_.set(name, val);
    if (!variable_watches_.empty()) {
        fire_variable_watch(name, "MODIFIED_ACCESS", val);
    }
}

std::expected<void, std::string> Interpreter::set_variable_parent_scope(const std::string& name, const std::string& val) {
    // Use ShadowMap's parent scope for both function and subdirectory contexts
    // add_subdirectory now uses push_scope/pop_scope, so ShadowMap handles all scoping
    auto result = variables_.set_parent_scope(name, val);
    if (result) {
        return {};
    }
    return std::unexpected(result.error());
}

std::expected<void, std::string> Interpreter::unset_variable_parent_scope(const std::string& name) {
    // Use ShadowMap's parent scope for both function and subdirectory contexts
    // add_subdirectory now uses push_scope/pop_scope, so ShadowMap handles all scoping
    auto result = variables_.unset_parent_scope(name);
    if (result) {
        return {};
    }
    return std::unexpected(result.error());
}

void Interpreter::set_cache_variable(const std::string& var_name, const std::string& value) {
    get_root()->cache_variables_[var_name] = value;
}

bool Interpreter::unset_variable(const std::string& name) {
    if (!variable_watches_.empty()) {
        fire_variable_watch(name, "REMOVED_ACCESS", "");
    }
    variables_.unset(name);
    return true;  // ShadowMap::unset is always safe (no-op if variable doesn't exist)
}

bool Interpreter::is_variable_set(std::string_view name) const {
    // Check macro substitutions first
    // Note: macro_substitutions_ is std::map<string,string>, need string for lookup
    if (auto it = macro_substitutions_.find(std::string(name)); it != macro_substitutions_.end()) {
        return true;
    }

    // Check local variables (O(1) via ShadowMap with transparent lookup)
    // ShadowMap now handles all scoping including subdirectory scope
    if (variables_.is_defined(name)) {
        return true;
    }

    // Check cache variables
    // Note: cache_variables_ is std::map<string,string>, need string for lookup
    return cache_variables_.contains(std::string(name));
}

std::optional<std::string> Interpreter::get_optional_variable(std::string_view name) const {
    if (frame_stack_.empty()) {
        std::cerr << "FATAL: get_optional_variable('" << name << "') called with empty frame_stack_\n";
        std::abort();
    }

    // Check macro substitutions first (macro parameters take precedence)
    // Note: macro_substitutions_ is std::map<string,string>, need string for lookup
    if (auto macro_it = macro_substitutions_.find(std::string(name)); macro_it != macro_substitutions_.end()) {
        return macro_it->second;
    }

    // Function-scoped special variables (lazy evaluation - check current frame only)
    // Fast reject: CMAKE_CURRENT_FUNCTION* vs other CMAKE_CURRENT_* variables.
    // The common lookups (SOURCE_DIR, BINARY_DIR, LIST_*) have 'S', 'B', or 'L' at position 14.
    // CMAKE_CURRENT_FUNCTION* has 'F' at position 14. Minimum length is 22 ("CMAKE_CURRENT_FUNCTION").
    // Position 14: CMAKE_CURRENT_FUNCTION
    //              01234567890123456789...
    //                            ^-- position 14 is 'F'
    if (name.size() >= 22 && name[14] == 'F' &&
        (name == "CMAKE_CURRENT_FUNCTION" ||
         name == "CMAKE_CURRENT_FUNCTION_LIST_FILE" ||
         name == "CMAKE_CURRENT_FUNCTION_LIST_DIR")) {

        const auto* fb = frame_stack_.front().function_block;
        if (fb != nullptr) {
            if (name == "CMAKE_CURRENT_FUNCTION") return fb->name;
            if (name == "CMAKE_CURRENT_FUNCTION_LIST_FILE") return fb->definition_file;
            if (name == "CMAKE_CURRENT_FUNCTION_LIST_DIR") return fb->definition_dir;
        }
        return std::nullopt;  // Not in a function
    }

    // Check local variables (use is_defined to distinguish "not set" from "set to empty")
    // ShadowMap now handles all scoping including subdirectory scope (O(1) with transparent lookup)
    if (variables_.is_defined(name)) {
        return std::string(variables_.get(name));
    }

    // Check cache variables (CACHE variables are globally accessible)
    // Note: cache_variables_ is std::map<string,string>, need string for lookup
    if (auto cache_it = cache_variables_.find(std::string(name)); cache_it != cache_variables_.end()) {
        return cache_it->second;
    }

    return std::nullopt;
}

void Interpreter::print_message(const std::string& mode, const std::string& msg, bool is_err) {
    std::ostream& os = is_err ? *err_ : *out_;
    std::string indent = get_variable("CMAKE_MESSAGE_INDENT");
    dmake::print_message(os, mode, msg, indent, force_colors_);

    // Debugger hook for --break-on-message
    if (debugger_) {
        debugger_->on_message(msg);
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
    // These are internal invariant checks - if any fail, it's a bug in dmake itself.
    // Please report at: https://github.com/anthropics/dmake/issues

    if (frame_stack_.empty()) {
        std::cerr << "INTERNAL ERROR (dmake bug): frame_stack_ is empty\n";
        std::abort();
    }

    if (loop_control_ != LoopControl::NONE && loop_depth_ <= 0) {
        std::cerr << "INTERNAL ERROR (dmake bug): loop_control_ is set (" << static_cast<int>(loop_control_)
                  << ") but loop_depth_ is " << loop_depth_ << "\n";
        std::abort();
    }

    if (loop_depth_ < 0) {
        std::cerr << "INTERNAL ERROR (dmake bug): loop_depth_ is negative (" << loop_depth_ << ")\n";
        std::abort();
    }

    if (builtins_.empty()) {
        std::cerr << "INTERNAL ERROR (dmake bug): interpreter has no builtins\n";
        std::abort();
    }

    if (directory_stack_.empty()) {
        std::cerr << "INTERNAL ERROR (dmake bug): directory_stack_ is empty\n";
        std::abort();
    }
}

void Interpreter::safe_pop_trace_stack(const std::string& context) {
    Interpreter* root = get_root();
    if (root->trace_stack_.empty()) {
        std::cerr << "FATAL: Attempting to pop empty trace_stack_ in context: " << context << "\n";
        std::cerr << "       This indicates a push/pop imbalance in the interpreter\n";
        std::abort();
    }
    root->trace_stack_.pop_back();
}

void Interpreter::finalize_directory_targets() {
    // Apply accumulated directory properties to all owned targets in current directory
    auto& ctx = get_current_directory_context();

    // CMAKE_INCLUDE_CURRENT_DIR: when ON, prepend source and binary dirs to include path
    bool include_current_dir = false;
    std::string icd_val = get_variable("CMAKE_INCLUDE_CURRENT_DIR");
    if (!icd_val.empty() && !is_falsy(icd_val)) {
        include_current_dir = true;
    }

    for (const auto& target : ctx.owned_targets) {
        // Apply CMAKE_INCLUDE_CURRENT_DIR (binary dir first, then source dir — CMake order)
        if (include_current_dir &&
            !target->is_imported() &&
            target->get_type() != TargetType::INTERFACE_LIBRARY) {
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

    // Skip caching for project paths (source/binary dirs) - they can change during build
    if (is_project_path(path)) {
        struct stat st;
        if (stat(path.c_str(), &st) == 0) {
            return st.st_mtime;
        }
        return std::nullopt;
    }

    // Check session cache
    auto it = root->dir_mtime_cache_.find(path);
    if (it != root->dir_mtime_cache_.end()) {
        return it->second;
    }

    // Not cached - stat and cache it
    struct stat st;
    std::optional<int64_t> mtime;
    if (stat(path.c_str(), &st) == 0) {
        mtime = st.st_mtime;
    }

    root->dir_mtime_cache_[path] = mtime;
    return mtime;
}

bool Interpreter::is_project_path(const std::string& path) const {
    std::filesystem::path abs_path = std::filesystem::absolute(path);
    std::filesystem::path source_dir = std::filesystem::absolute(get_variable("CMAKE_SOURCE_DIR"));
    std::filesystem::path binary_dir = std::filesystem::absolute(get_variable("CMAKE_BINARY_DIR"));

    // Check if path starts with source_dir or binary_dir
    auto mismatch_src = std::mismatch(source_dir.begin(), source_dir.end(), abs_path.begin(), abs_path.end());
    if (mismatch_src.first == source_dir.end()) {
        return true;  // Under source dir
    }

    auto mismatch_bin = std::mismatch(binary_dir.begin(), binary_dir.end(), abs_path.begin(), abs_path.end());
    if (mismatch_bin.first == binary_dir.end()) {
        return true;  // Under binary dir
    }

    return false;
}

}
