#pragma once

#include "cmake-language.hpp"
#include "target.hpp"
#include "build_system.hpp"
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <iostream>
#include <unistd.h>
#include <vector>
#include <memory>
#include <deque>
#include <expected>
#include <optional>
#include <filesystem>
#include "CMakeArray.hpp"
#include "toolchain.hpp"
#include "cache_store.hpp"
#include "shadow_map.hpp"
#include "printing.hpp"
#include "ast_cache.hpp"
#include "debugger.hpp"
#include "policies.hpp"

namespace kiln {

struct InterpreterError {
    std::string file;
    size_t row;
    size_t col;
    size_t offset;
    size_t length;
    std::string message;
    std::vector<CallLocation> backtrace;
    std::optional<std::string> source_content = std::nullopt;
};

struct BuildError {
    std::string file;
    std::string message;
};

struct TestDefinition {
    std::string name;
    std::string command; // Target name or path
    std::vector<std::string> args;
    std::string working_dir;
    std::map<std::string, std::string> properties; // Test properties
};

// Custom command rule for OUTPUT form of add_custom_command
// Maps output files to the commands that generate them
struct CustomCommandRule {
    std::vector<std::string> outputs;               // Files this command generates
    std::vector<std::vector<std::string>> commands; // Commands to run (in order)
    std::vector<std::string> depends;               // Input files/targets
    std::string working_dir;                        // Working directory for commands
    std::string comment;                            // Display comment during build
    std::string source_dir;                         // Source directory where command was defined
    std::string binary_dir;                         // Binary directory where command was defined
};

// Deferred file(GENERATE) — evaluated at graph generation time when genex context is available
struct PendingFileGenerate {
    std::string output;        // Raw OUTPUT path (may contain genex)
    std::string content;       // Content (from CONTENT or read from INPUT)
    std::string condition;     // CONDITION genex (empty = unconditional)
    std::string newline_style; // NEWLINE_STYLE keyword
    std::string binary_dir;    // CMAKE_CURRENT_BINARY_DIR at call time
    std::string target_name;   // TARGET keyword — sets current_target for genex
};

// Install system structures
enum class InstallRuleType { TARGETS, FILES, PROGRAMS, DIRECTORY, SCRIPT, CODE, EXPORT };

struct InstallDestination {
    std::string destination;                 // Relative to CMAKE_INSTALL_PREFIX
    std::vector<std::string> permissions;    // OWNER_READ, OWNER_WRITE, etc.
    std::string component;                   // Component name (empty = "Unspecified")
    std::vector<std::string> configurations; // Debug, Release, etc.
    bool optional = false;                   // Continue if source missing
    bool exclude_from_all = false;           // Skip from default install
};

struct InstallTargetsRule {
    std::vector<std::string> targets;
    InstallDestination archive_dest;        // Static libraries (.a)
    InstallDestination library_dest;        // Shared libraries (.so)
    InstallDestination runtime_dest;        // Executables
    InstallDestination public_header_dest;  // Public headers
    InstallDestination private_header_dest; // Private headers
    InstallDestination bundle_dest;         // macOS app bundles (.app)
};

struct InstallFilesRule {
    std::vector<std::string> files;
    InstallDestination destination;
    std::string rename;       // RENAME <name> (only valid with single file)
    bool is_programs = false; // Different default permissions
};

struct InstallDirectoryRule {
    std::vector<std::string> directories;
    InstallDestination destination;
    std::vector<std::string> file_patterns;    // FILES_MATCHING PATTERN
    std::vector<std::string> exclude_patterns; // PATTERN EXCLUDE
    bool use_source_permissions = false;
};

struct InstallScriptRule {
    std::string script_path; // For SCRIPT mode
    std::string code;        // For CODE mode
    std::string component;   // Component name (empty = "Unspecified")
};

struct InstallExportRule {
    std::string export_name;      // Name of the export set
    std::string file_name;        // Output file name (e.g., MyProjectTargets.cmake)
    std::string namespace_prefix; // Namespace for imported targets
    std::string destination;      // Install destination
    std::string component;        // Component name (empty = "Unspecified")
};

struct InstallRule {
    InstallRuleType type;
    std::string source_dir; // CMAKE_CURRENT_SOURCE_DIR when defined
    std::string binary_dir; // CMAKE_CURRENT_BINARY_DIR when defined

    // Only one is populated based on type
    std::shared_ptr<InstallTargetsRule> targets_rule;
    std::shared_ptr<InstallFilesRule> files_rule;
    std::shared_ptr<InstallDirectoryRule> directory_rule;
    std::shared_ptr<InstallScriptRule> script_rule;
    std::shared_ptr<InstallExportRule> export_rule;
};

// Entry in an export set (populated by install(TARGETS ... EXPORT))
struct ExportSetEntry {
    std::string target_name;
    std::string source_dir; // For relative path computation
    std::string binary_dir;
    // Destinations from install(TARGETS ... ) so the install-tree export can
    // reproduce IMPORTED_LOCATION at the actual install path. Empty when the
    // user didn't pass a per-type DESTINATION (consumers fall back to
    // CMAKE_INSTALL_{BIN,LIB}DIR via the export generator's defaults).
    std::string archive_dest;
    std::string library_dest;
    std::string runtime_dest;
};

// Forward declaration
class Interpreter;

// Property system
enum class PropertyScope { GLOBAL, DIRECTORY, TARGET, SOURCE, TEST, VARIABLE, CACHED_VARIABLE, INSTALL };

struct PropertyDefinition {
    PropertyScope scope;
    std::string name;
    bool inherited = false;
    std::string brief_docs;
    std::string full_docs;
    std::string initialize_from_variable; // For TARGET properties, optional
};

struct FrameMetadata {
    const std::string* script_dir;                 // Non-owning pointer (outlives the frame)
    const FunctionBlock* function_block = nullptr; // Pointer to FunctionBlock if this is a function frame
};

// Directory-specific state (stored in map at root, keyed by abs source path)
struct DeferredCall {
    std::string id;                     // Unique identifier
    std::string command;                // Command to call
    std::vector<std::string> arguments; // Unevaluated arguments
};

struct DirectoryContext {
    std::string source_dir;
    std::string binary_dir;
    std::string parent_dir; // For property inheritance (empty for root)

    std::map<std::string, std::string> properties;               // DIRECTORY scope properties
    std::map<std::string, std::vector<std::string>> accumulated; // Compile defs, includes, etc.
    std::vector<std::shared_ptr<Target>> owned_targets;          // For finalize_directory_targets()
    std::set<std::string> guarded_files;                         // Directory-level include guards
    std::vector<DeferredCall> deferred_calls;                    // cmake_language(DEFER) calls
    int next_deferred_id = 0;                                    // Auto-increment for ID generation
};

// Lightweight trace stack entry — avoids copying file paths and command names.
// file points into the interpreter's interned_files_ set; command is a view into
// the AST node's identifier (valid while the node's body is executing).
struct TraceEntry {
    const std::string* file; // non-owning pointer into intern pool
    size_t row;
    size_t col;
    size_t offset;
    size_t length;
    std::string_view command; // non-owning view into AST node
};

class Interpreter {
public:
    using BuiltinFunction = std::function<void(Interpreter&, const std::vector<std::string>&)>;
    using TransparentStringSet = std::unordered_set<std::string, TransparentStringHash, TransparentStringEqual>;
    enum class LoopControl { NONE, BREAK, CONTINUE };

    // skip_host_compiler_detection: when true, the constructor will not eagerly
    // detect host gcc/g++. Use when the caller knows a toolchain file or
    // -DCMAKE_<LANG>_COMPILER override is in play and the eager work would be
    // discarded; lazy on-demand detection inside enable_compiler_for_language
    // picks up the slack.
    explicit Interpreter(std::string script_dir, std::ostream* out = &std::cout, std::ostream* err = &std::cerr,
                         std::optional<std::string> build_dir = std::nullopt, bool skip_sys_init = false, bool skip_cache_load = false,
                         bool skip_host_compiler_detection = false);

    std::expected<void, InterpreterError> interpret(const std::vector<AstNode>& ast);
    std::expected<Interpreter*, BuildError> run_build(int jobs = 0, const std::vector<std::string>& targets = {});

    // For ExternalProject: generate full build graph for atomic EP attachment.
    // Returns the complete BuildGraph (not just dirty tasks).
    std::expected<BuildGraph, BuildError> generate_build_graph(const std::vector<std::string>& targets = {});

    void add_builtin(const std::string& name, BuiltinFunction func);
    std::string evaluate_argument(const Argument& arg);
    std::vector<std::string> expand_arguments(const std::vector<Argument>& args);
    void expand_arguments_into(const std::vector<Argument>& args, std::vector<std::string>& result);

    void set_current_file(const std::string& file) {
        current_file_ = file;
        current_file_interned_ = intern_file(file);
    }
    std::string get_current_file() const { return current_file_; }

    // Force color output even when writing to non-TTY (for child interpreters)
    void set_force_colors(bool force) { force_colors_ = force; }
    bool get_force_colors() const { return force_colors_; }

    // Public API for builtins and internal use
    void set_fatal_error(const std::string& message);
    void set_fatal_error(const InterpreterError& error);

    std::string get_variable(std::string_view var_name) const;
    std::optional<std::string> get_optional_variable(std::string_view var_name) const;
    // Returns a non-owning view of the variable's stored value.
    // Valid until the variable is next mutated or scope changes.
    std::optional<std::string_view> get_variable_view(std::string_view var_name) const;
    void set_variable(const std::string& var_name, const std::string& value);
    std::expected<void, std::string> set_variable_parent_scope(const std::string& var_name, const std::string& value);
    std::expected<void, std::string> unset_variable_parent_scope(const std::string& var_name);
    bool unset_variable(const std::string& var_name);
    bool is_variable_set(std::string_view var_name) const;
    static bool is_falsy(std::string_view val);
    static bool is_truthy(const std::string& val);
    void set_cache_variable(const std::string& var_name, const std::string& value);

    void print_message(const std::string& mode, const std::string& message, bool is_error = false);
    void print_warning_with_context(const std::string& message);

    // Non-owning view of the top-level source being interpreted. Used to render
    // code context in diagnostics when no on-disk file backs the script (e.g.
    // tests). The view must outlive any diagnostic that may consult it.
    void set_source_view(std::string_view source) { source_view_ = source; }
    std::ostream* error_stream() const { return err_; }

    // CHECK_* message support
    void check_start(const std::string& message);
    void check_pass(const std::string& result_message);
    void check_fail(const std::string& result_message);

    // SEND_ERROR support - accumulates errors
    void accumulate_error(const std::string& error);
    bool has_accumulated_errors() const { return has_send_errors_; }

    std::expected<void, InterpreterError> include_file(const std::string& file_path, bool optional = false);

    // File existence check (caching handled internally)
    bool cached_file_exists(std::string_view full_path);
    bool cached_file_exists(std::string_view dir, const std::string& filename);

    // Directory check with caching (uses dir_scan_cache_ from directory listings)
    bool cached_is_directory(std::string_view path);

    // Canonical path with directory-level caching (avoids redundant readlink syscalls)
    std::string cached_weakly_canonical(std::string_view p);

    // Get directory listing (caching handled internally)
    // Returns nullptr if directory doesn't exist or can't be read
    const TransparentStringSet* get_directory_listing(std::string_view dir);

    // Get subdirectory names within a directory (populates cache if needed)
    // Returns nullptr if directory doesn't exist or can't be read
    const TransparentStringSet* get_directory_subdirs(std::string_view dir);

    // Policy system
    PolicyState get_policy(CMakePolicy p) const { return policies_.get(p); }
    void set_policy(CMakePolicy p, PolicyState s) { policies_.set(p, s); }
    void set_policies_for_version(std::string_view version) { policies_.set_defaults_for_version(version); }
    void push_policies() { policies_.push(); }
    void pop_policies() { policies_.pop(); }

    int get_loop_depth() const { return loop_depth_; }
    void set_loop_control(LoopControl control) { loop_control_ = control; }
    void clear_loop_control() { loop_control_ = LoopControl::NONE; }

    // Access to targets (for testing and build system)
    TargetMap& get_targets() { return get_root()->targets_; }
    std::unordered_map<std::string, std::string>& get_target_aliases() { return get_root()->target_aliases_; }
    const std::unordered_map<std::string, std::string>& get_target_aliases() const { return get_root()->target_aliases_; }

    // Resolve alias to real target name (returns input if not an alias)
    std::string resolve_target_alias(const std::string& name) const {
        auto& aliases = get_root()->target_aliases_;
        auto it = aliases.find(name);
        return (it != aliases.end()) ? it->second : name;
    }

    // Look up a target by name, resolving aliases. Returns nullptr if not found.
    // Use this for user-provided target names. For internal/already-resolved names, use get_targets() directly.
    //
    // Imported targets are directory-scoped (visible only in their creating dir
    // and subdirs) unless declared GLOBAL. Non-imported targets are always
    // global. We filter out imported non-global targets that aren't visible
    // from the current source dir so multi-find_package() across sibling
    // subdirs reproduces CMake's per-scope semantics.
    Target* find_target(const std::string& name) const {
        auto& targets = get_root()->targets_;
        auto it = targets.find(resolve_target_alias(name));
        if (it == targets.end()) return nullptr;
        Target* t = it->second.get();
        if (t->is_imported() && !t->is_imported_global()) {
            const auto& creator = t->get_source_dir();
            // CMAKE_CURRENT_SOURCE_DIR mutates as we walk into subdirs;
            // visibility is "creator dir is a prefix of current dir".
            std::string cur = const_cast<Interpreter*>(this)->get_variable("CMAKE_CURRENT_SOURCE_DIR");
            if (!creator.empty() && !cur.empty()) {
                if (cur != creator
                    && !(cur.size() > creator.size() && cur.compare(0, creator.size(), creator) == 0 && cur[creator.size()] == '/')) {
                    return nullptr;
                }
            }
        }
        return t;
    }

    // Check if a name is an alias
    bool is_target_alias(const std::string& name) const {
        return get_root()->target_aliases_.find(name) != get_root()->target_aliases_.end();
    }

    Toolchain& get_toolchain() { return get_root()->toolchain_; }

    // Toolchain file (CMAKE_TOOLCHAIN_FILE) is loaded exactly once at the
    // start of the first project()/enable_language. These accessors guard
    // against re-entry and let project() know whether to load.
    bool toolchain_file_loaded() const { return get_root()->toolchain_file_loaded_; }
    void mark_toolchain_file_loaded() { get_root()->toolchain_file_loaded_ = true; }

    // Whether project() has been called at least once. Used to give a clear
    // diagnostic when add_executable()/add_library() runs without a project()
    // call instead of failing later with "no compiler available".
    bool project_called() const { return get_root()->project_called_; }
    void mark_project_called() { get_root()->project_called_ = true; }
    bool no_project_warned() const { return get_root()->no_project_warned_; }
    void mark_no_project_warned() { get_root()->no_project_warned_ = true; }

    // True iff the current scope sits in the top-level source directory.
    // Drives PROJECT_IS_TOP_LEVEL and similar gates that should fire only
    // when the current CMakeLists is not being consumed via add_subdirectory
    // or FetchContent.
    bool in_top_source_dir() { return get_variable("CMAKE_CURRENT_SOURCE_DIR") == get_variable("CMAKE_SOURCE_DIR"); }
    CacheStore& get_cache_store() { return *get_root()->cache_store_; }

    // Enable a compiler for the given language (C, CXX, ASM).
    // Pulls cached detection data, sets language-specific variables, and creates
    // the toolchain compiler. Returns empty string on success, error on failure.
    std::string enable_compiler_for_language(const std::string& lang);

    // Directory mtime caching for find_xxx performance
    // Returns mtime of directory (nullopt if doesn't exist)
    // Uses session cache for paths outside source/binary dirs
    std::optional<int64_t> get_dir_mtime_cached(const std::string& path);

    // Check if path is under our source or binary directory (skip mtime caching for these)
    bool is_project_path(const std::string& path) const;

    // Apply accumulated directory properties to all owned targets (retroactive application)
    void finalize_directory_targets();

    std::vector<TestDefinition>& get_tests() { return get_root()->tests_; }
    bool is_testing_enabled() const { return !is_falsy(get_variable("BUILD_TESTING")); }

    // Whether the project's CMakeLists called enable_testing() / include(CTest).
    // Distinct from is_testing_enabled(), which also flips when the kiln CLI is
    // invoked in test mode. CMP0037's `test`-target carve-out keys off this flag,
    // not BUILD_TESTING, because the policy is about project intent.
    bool project_enabled_testing() const { return get_root()->project_enabled_testing_; }
    void mark_project_enabled_testing() { get_root()->project_enabled_testing_ = true; }

    // Deferred file(GENERATE) entries
    std::vector<PendingFileGenerate>& get_pending_file_generates() { return get_root()->pending_file_generates_; }

    // Deferred target dump (for kiln_dump_target_info AT_BUILD)
    void add_target_to_dump_at_build(const std::string& name) { get_root()->targets_to_dump_at_build_.insert(name); }
    const std::set<std::string>& get_targets_to_dump_at_build() const { return get_root()->targets_to_dump_at_build_; }

    // Custom command rules (OUTPUT form of add_custom_command)
    std::map<std::string, std::shared_ptr<CustomCommandRule>>& get_custom_command_rules() { return get_root()->custom_command_rules_; }
    const std::map<std::string, std::shared_ptr<CustomCommandRule>>& get_custom_command_rules() const {
        return get_root()->custom_command_rules_;
    }

    // Install rules
    std::vector<std::shared_ptr<InstallRule>>& get_install_rules() { return get_root()->install_rules_; }
    const std::vector<std::shared_ptr<InstallRule>>& get_install_rules() const { return get_root()->install_rules_; }

    // Export sets (populated by install(TARGETS ... EXPORT))
    void add_to_export_set(const std::string& export_name, const std::string& target, const std::string& src_dir,
                           const std::string& bin_dir, const std::string& archive_dest = {}, const std::string& library_dest = {},
                           const std::string& runtime_dest = {}) {
        get_root()->export_sets_[export_name].push_back({target, src_dir, bin_dir, archive_dest, library_dest, runtime_dest});
    }
    const std::map<std::string, std::vector<ExportSetEntry>>& get_export_sets() const { return get_root()->export_sets_; }

    // Property system accessors
    std::map<PropertyScope, std::map<std::string, PropertyDefinition>>& get_property_definitions() {
        return get_root()->property_definitions_;
    }
    std::map<std::string, std::string>& get_global_properties() { return get_root()->global_properties_; }
    std::map<std::string, std::map<std::string, std::string>>& get_source_properties() { return get_root()->source_properties_; }
    const std::map<std::string, std::map<std::string, std::string>>& get_source_properties() const {
        return get_root()->source_properties_;
    }
    auto& get_cache_variables() { return get_root()->cache_variables_; }

    // Variable map accessor for builtins (needed for PARENT_SCOPE)
    ShadowMap& get_variables() { return variables_; }
    const ShadowMap& get_variables() const { return variables_; }

    // Directory context accessors (scope-based approach)
    DirectoryContext& get_current_directory_context();
    DirectoryContext* get_directory_context(const std::string& dir);
    const std::map<std::string, DirectoryContext>& get_all_directory_contexts() const { return get_root()->directory_contexts_; }
    void push_directory(const std::string& source_dir, const std::string& binary_dir);
    void pop_directory();
    void execute_deferred_calls();

    // For property.cpp compatibility - returns current directory's properties
    std::map<std::string, std::string>& get_directory_properties() { return get_current_directory_context().properties; }

    // Install properties: installed_path -> property_name -> value
    std::map<std::string, std::map<std::string, std::string>>& get_install_properties() { return get_root()->install_properties_; }

    // Friend registration functions
    friend void register_message_builtins(Interpreter& interp);
    friend void register_variable_builtins(Interpreter& interp);
    friend void register_list_builtins(Interpreter& interp);
    friend void register_target_builtins(Interpreter& interp);
    friend void register_project_builtins(Interpreter& interp);
    friend void register_file_builtins(Interpreter& interp);
    friend void register_find_commands_builtins(Interpreter& interp);
    friend void register_process_builtins(Interpreter& interp);
    friend void register_math_builtins(Interpreter& interp);
    friend void register_string_builtins(Interpreter& interp);
    friend void register_property_builtins(Interpreter& interp);
    friend void register_try_compile_builtins(Interpreter& interp);
    friend void register_path_builtins(Interpreter& interp);
    friend void register_install_builtins(Interpreter& interp);
    friend void register_source_properties_builtins(Interpreter& interp);
    friend void register_external_project_builtins(Interpreter& interp);
    friend void register_fetch_content_builtins(Interpreter& interp);

    CMakeArray from_arguments(const std::vector<std::string>& args);

    void request_return() { return_requested_ = true; }
    bool is_return_requested() const { return return_requested_; }
    void clear_return_request() { return_requested_ = false; }

    // Debug/validation helpers
    void check_invariants() const;
    void pop_trace_stack();

    // Call a user-defined function by name (returns false if function doesn't exist or execution failed)
    bool call_user_function(const std::string& name, const std::vector<std::string>& args);

    // Check if a user-defined function exists (case-insensitive)
    bool has_user_function(const std::string& name) const;

    // Variable watch support (for variable_watch() CMake command)
    struct VariableWatch {
        std::string variable;
        std::optional<std::string> callback_function;
    };
    void add_variable_watch(const std::string& var_name, std::optional<std::string> callback = std::nullopt);
    void fire_variable_watch(const std::string& name, const std::string& access_type, const std::string& value);
    // Cheap check for hot paths (e.g. tight list/string APPEND loops) so they can
    // skip building the value-for-watch when nobody's watching.
    bool has_variable_watches() const { return !variable_watches_.empty(); }

    // Access fatal error state (used by condition evaluator and debugger)
    std::optional<InterpreterError> get_fatal_error() const;

    Interpreter* get_root();
    const Interpreter* get_root() const;

    // Trace stack accessor (for debugger backtrace and source listing)
    const std::vector<TraceEntry>& get_trace_stack() const { return get_root()->trace_stack_; }

    // Debugger accessor (may be null if debug/trace not enabled)
    Debugger* get_debugger() { return debugger_.get(); }
    void set_debugger(std::unique_ptr<Debugger> dbg) { debugger_ = std::move(dbg); }

private:
    std::expected<void, InterpreterError> execute_command(const CommandInvocation& cmd);
    std::expected<void, InterpreterError> execute_command_with_args(const std::string& identifier, const std::vector<std::string>& args);
    void process_file_generates(const GenexEvaluationContext& genex_ctx);
    std::expected<void, InterpreterError> execute_if_block(const IfBlock& if_block);
    std::expected<void, InterpreterError> execute_function_block(const FunctionBlock& function_block);
    std::expected<void, InterpreterError> execute_macro_block(const MacroBlock& macro_block);
    std::expected<void, InterpreterError> execute_foreach_block(const ForeachBlock& foreach_block);
    std::expected<void, InterpreterError> execute_while_block(const WhileBlock& while_block);
    std::expected<void, InterpreterError> execute_block_block(const BlockBlock& block_block);
    std::expected<void, InterpreterError> invoke_user_function(const FunctionBlock& func, const std::vector<std::string>& args);
    std::expected<void, InterpreterError> invoke_user_macro(const MacroBlock& macro, const std::vector<std::string>& args);
    std::expected<bool, InterpreterError> evaluate_condition(const std::vector<Argument>& condition, size_t row, size_t col, size_t offset,
                                                             size_t length);
    std::expected<bool, InterpreterError> evaluate_condition(const std::vector<Argument>& condition, const PreParsedCondition& pp,
                                                             size_t row, size_t col, size_t offset, size_t length);
    std::string evaluate_variable_reference(const VariableReference& ref);

    const std::string* intern_file(const std::string& path) { return &*get_root()->interned_files_.insert(path).first; }

    void clear_fatal_error();

    std::string build_dir_;
    std::ostream* out_;
    std::ostream* err_;
    bool force_colors_ = false; // Force color output even when not writing to TTY

    // Global state (managed by root)
    inner::ankerl::unordered_dense::map<std::string, BuiltinFunction, TransparentStringHash, TransparentStringEqual> builtins_;
    TargetMap targets_;
    std::unordered_map<std::string, std::string> target_aliases_; // alias_name -> real_target_name
    std::vector<TestDefinition> tests_;
    std::set<std::string> targets_to_dump_at_build_; // For kiln_dump_target_info AT_BUILD
    std::vector<PendingFileGenerate> pending_file_generates_;
    std::set<std::string> file_generate_outputs_; // Resolved paths from file(GENERATE)

    // Custom command rules (OUTPUT form of add_custom_command)
    // Maps output file path -> rule that generates it
    std::map<std::string, std::shared_ptr<CustomCommandRule>> custom_command_rules_;

    // Install rules
    std::vector<std::shared_ptr<InstallRule>> install_rules_;

    // Export sets: export_name -> list of target entries
    std::map<std::string, std::vector<ExportSetEntry>> export_sets_;

    Toolchain toolchain_;
    bool toolchain_file_loaded_ = false;
    bool project_called_ = false;
    bool no_project_warned_ = false;
    bool project_enabled_testing_ = false;
    std::unique_ptr<CacheStore> cache_store_;
    AstCache ast_cache_;
    std::unique_ptr<Debugger> debugger_;
    std::set<std::string> global_guarded_files_;
    std::unordered_map<std::string, std::string, TransparentStringHash, TransparentStringEqual> cache_variables_;

    // Session-wide directory mtime cache (for find_xxx performance)
    // Key: absolute path, Value: mtime (or nullopt if doesn't exist)
    std::map<std::string, std::optional<int64_t>> dir_mtime_cache_;

    // Cached absolute source/binary dir strings for is_project_path() (lazily populated)
    mutable std::string cached_abs_source_dir_;
    mutable std::string cached_abs_binary_dir_;

    // Session-wide canonical directory cache (avoids redundant readlink syscalls)
    // Key: parent directory path, Value: resolved canonical path
    std::unordered_map<std::string, std::string> canonical_dir_cache_;

    // Property system (managed by root)
    // Property definitions: scope -> property_name -> definition
    std::map<PropertyScope, std::map<std::string, PropertyDefinition>> property_definitions_;
    // Global property values: property_name -> value
    std::map<std::string, std::string> global_properties_;
    // Source property values: absolute_source_path -> property_name -> value
    std::map<std::string, std::map<std::string, std::string>> source_properties_;
    // Install property values: normalized_install_path -> property_name -> value
    std::map<std::string, std::map<std::string, std::string>> install_properties_;

    // Directory scan cache for optimizing file lookups and glob
    // Uses transparent hashing so lookups can use string_view without allocating
    struct DirectoryCacheEntry {
        std::filesystem::file_time_type mtime; // Directory modification time
        TransparentStringSet entries;          // All entries (filenames only) - O(1) lookup
        TransparentStringSet subdirs;          // Subdirectory names (for recursive glob)
    };
    const DirectoryCacheEntry* get_directory_cache_entry(std::string_view dir);
    std::unordered_map<std::string, DirectoryCacheEntry, TransparentStringHash, TransparentStringEqual> dir_scan_cache_;

    // Directory contexts: keyed by absolute source path (managed at root)
    // Each directory has its own context with properties, accumulated values, and owned targets
    std::map<std::string, DirectoryContext> directory_contexts_;

    // Directory stack: tracks the current directory (back = current)
    // Used for push_directory/pop_directory during add_subdirectory
    std::vector<std::string> directory_stack_;

    // Global functions and macros (stored at root, accessible everywhere)
    // CMake semantics: functions/macros are globally visible once defined
    inner::ankerl::unordered_dense::map<std::string, std::unique_ptr<FunctionBlock>, TransparentStringHash, TransparentStringEqual>
        user_functions_;
    inner::ankerl::unordered_dense::map<std::string, std::unique_ptr<MacroBlock>, TransparentStringHash, TransparentStringEqual>
        user_macros_;

    // Deferred deletion for functions/macros replaced during their own execution.
    // Each entry records {delete_when_size_at_or_below, function_ptr}.
    // When frame_stack_.size() drops to or below the threshold, the function is deleted.
    std::vector<std::pair<size_t, std::unique_ptr<FunctionBlock>>> deferred_function_deletions_;

    struct DeferredMacroDeletion {
        std::string name; // Which macro was replaced
        size_t depth;     // Execution depth at time of replacement
        std::unique_ptr<MacroBlock> block;
    };
    std::vector<DeferredMacroDeletion> deferred_macro_deletions_;

    // Shadow Map-based variable scoping (O(1) access, automatic cleanup)
    ShadowMap variables_; // Regular variables with scope tracking

    std::vector<FrameMetadata> frame_stack_; // Metadata only (no variables)
    std::vector<TraceEntry> trace_stack_;    // For backtraces (lightweight, non-owning)
    static constexpr size_t max_trace_depth_ = 2000;
    std::string current_file_;
    const std::string* current_file_interned_ = nullptr; // Points into interned_files_
    std::string_view source_view_;                       // Non-owning; set by set_source_view() when source is in-memory
    std::unordered_set<std::string> interned_files_;     // Owns file path strings
    std::optional<InterpreterError> fatal_error_;
    size_t current_cmd_row_ = 0;
    size_t current_cmd_col_ = 0;

    // Policy stack (per-interpreter, scoped by include/add_subdirectory)
    PolicyStack policies_ = PolicyStack::make_defaults();

    // Loop control state (local to current script/function scope)
    int loop_depth_ = 0;
    LoopControl loop_control_ = LoopControl::NONE;

    // Return control state (for return() command)
    bool return_requested_ = false;

    // Macro parameter substitution (for text-replacement in macros)
    // Checked before variable lookup to implement CMake macro semantics
    std::map<std::string, std::string> macro_substitutions_;

    // Tracks active macro invocations by name (lowercase) for deferred deletion.
    // Incremented on entry, decremented on exit. If > 0 when replacing, defer deletion.
    std::unordered_map<std::string, int> macro_execution_depth_;

    // CHECK_* message state (stack for nested checks)
    std::vector<std::string> check_stack_;

    // SEND_ERROR accumulation
    bool has_send_errors_ = false;

    // Variable watches (for variable_watch() CMake command)
    std::map<std::string, VariableWatch> variable_watches_;

    // Reusable buffers (avoid per-call allocation — one per Interpreter instance)
    std::vector<std::string> expanded_args_buf_;
    std::string lower_buf_;
};

// RAII guard that saves/clears return_requested_ on construction and restores it
// on destruction. Use at scope boundaries (add_subdirectory, include, function)
// so that return() in a child scope never leaks into the parent.
struct ReturnGuard {
    Interpreter& interp_;
    bool saved_;
    explicit ReturnGuard(Interpreter& interp) : interp_(interp), saved_(interp.is_return_requested()) { interp_.clear_return_request(); }
    ~ReturnGuard() {
        interp_.clear_return_request();
        if (saved_) interp_.request_return();
    }
    ReturnGuard(const ReturnGuard&) = delete;
    ReturnGuard& operator=(const ReturnGuard&) = delete;
};

} // namespace kiln
