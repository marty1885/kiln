#pragma once

#include <string>
#include <vector>
#include <map>
#include <set>
#include <unordered_set>
#include <unordered_map>
#include <expected>
#include <optional>
#include <memory>
#include <filesystem>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <variant>
#include "utils.hpp"
#include "language.hpp"

namespace dmake {

template<class... Ts> struct overloaded : Ts... { using Ts::operator()...; };

struct CompileTask        { std::string source_file; std::optional<Language> compile_language; };
struct PCHTask            { std::string source_file; };
struct LinkTask           {};
struct CustomCommandTask  {};
struct CustomTargetTask   {};
struct PreBuildTask       {};
struct PostBuildTask      {};
struct ModuleScannerTask  { std::string source_file; };
struct ModuleCollatorTask {};
struct EPOrchestratorTask { std::string ep_name; };
struct EPSentinelTask     { std::string ep_name; };
struct EPInstallTask      { std::string ep_name; };

using TaskKind = std::variant<
    CompileTask, PCHTask, LinkTask,
    CustomCommandTask, CustomTargetTask, PreBuildTask, PostBuildTask,
    ModuleScannerTask, ModuleCollatorTask,
    EPOrchestratorTask, EPSentinelTask, EPInstallTask
>;

// Forward declarations
class ProgressBar;

// Set by signal handlers to request graceful shutdown.
// The build loop checks this and stops dispatching new tasks,
// then saves the cache so completed work isn't lost.
inline std::atomic<bool> g_interrupted{false};

class Target;
class Interpreter;
class ExternalProjectTarget;
class Toolchain;
struct GenexEvaluationContext;

struct BuildTask {
    std::string id;              // Unique identifier (usually the primary output file)
    TaskKind kind{LinkTask{}};   // Variant-based task type
    std::vector<std::vector<std::string>> commands;
    std::vector<std::string> inputs;
    std::vector<std::string> outputs;
    Target* parent_target = nullptr;
    bool always_run = false;
    std::string working_dir;
    std::string ep_binary_dir;   // EP binary dir for cache routing (empty = use main cache)

    // Dependency edges (resolved pointers, set by graph)
    std::vector<BuildTask*> dependencies;
    std::vector<BuildTask*> dependents;

    // Unresolved dependency IDs (set during task creation, resolved to pointers in finalize)
    std::vector<std::string> explicit_deps;

    // Filled during execution for critical path computation
    double execution_time_s = 0.0;    // wall time for this task
    double critical_path_s = 0.0;     // longest chain ending at this task

    // --- Convenience query methods ---

    bool is_compilation() const {
        return std::holds_alternative<CompileTask>(kind) || std::holds_alternative<PCHTask>(kind);
    }

    bool is_shell_command() const {
        return std::holds_alternative<CustomCommandTask>(kind)
            || std::holds_alternative<CustomTargetTask>(kind)
            || std::holds_alternative<PostBuildTask>(kind);
    }

    bool is_ep_task() const {
        return std::holds_alternative<EPOrchestratorTask>(kind)
            || std::holds_alternative<EPSentinelTask>(kind)
            || std::holds_alternative<EPInstallTask>(kind);
    }

    bool is_marker_task() const {
        if (outputs.empty() && commands.empty()
            && !std::holds_alternative<ModuleCollatorTask>(kind)
            && !std::holds_alternative<EPOrchestratorTask>(kind)
            && !std::holds_alternative<EPSentinelTask>(kind))
            return true;
        return false;
    }

    std::string_view get_source_file() const {
        return std::visit(overloaded{
            [](const CompileTask& t) -> std::string_view { return t.source_file; },
            [](const PCHTask& t) -> std::string_view { return t.source_file; },
            [](const ModuleScannerTask& t) -> std::string_view { return t.source_file; },
            [](const auto&) -> std::string_view { return {}; }
        }, kind);
    }

    std::string_view get_ep_name() const {
        return std::visit(overloaded{
            [](const EPOrchestratorTask& t) -> std::string_view { return t.ep_name; },
            [](const EPSentinelTask& t) -> std::string_view { return t.ep_name; },
            [](const EPInstallTask& t) -> std::string_view { return t.ep_name; },
            [](const auto&) -> std::string_view { return {}; }
        }, kind);
    }

    std::optional<Language> get_compile_language() const {
        if (auto* ct = std::get_if<CompileTask>(&kind)) return ct->compile_language;
        return std::nullopt;
    }
};

// Comparator for deterministic ordering of BuildTask pointers (by task ID)
struct TaskPtrIdCmp {
    bool operator()(const BuildTask* a, const BuildTask* b) const {
        return a->id < b->id;
    }
};

class BuildGraph {
public:
    BuildGraph() = default;

    // Move constructor/assignment - needed because mutexes aren't movable
    BuildGraph(BuildGraph&& other) noexcept
        : tasks_(std::move(other.tasks_)),
          task_by_id_(std::move(other.task_by_id_)),
          ep_target_owners_(std::move(other.ep_target_owners_)),
          stat_cache_(std::move(other.stat_cache_)),
          deps_cache_(std::move(other.deps_cache_)),
          compiler_version_cache_(std::move(other.compiler_version_cache_)) {}

    BuildGraph& operator=(BuildGraph&& other) noexcept {
        if (this != &other) {
            tasks_ = std::move(other.tasks_);
            task_by_id_ = std::move(other.task_by_id_);
            ep_target_owners_ = std::move(other.ep_target_owners_);
            stat_cache_ = std::move(other.stat_cache_);
            deps_cache_ = std::move(other.deps_cache_);
            compiler_version_cache_ = std::move(other.compiler_version_cache_);
        }
        return *this;
    }

    // Disable copying
    BuildGraph(const BuildGraph&) = delete;
    BuildGraph& operator=(const BuildGraph&) = delete;

    void add_task(BuildTask task);

    // Checks for cycles and returns an error message if one is found
    std::optional<std::string> check_for_cycles();

    // Executes the graph.
    std::expected<void, std::string> execute(const std::string& build_dir, int jobs = 0);

    std::expected<void, std::string> generate_compile_commands(const std::string& build_dir);

    // Finalize the build graph: evaluate all generator expressions in all tasks.
    // Also resolves explicit_deps (string IDs) to pointer-based dependencies.
    // Call after generate_tasks() and before execute().
    std::expected<void, std::string> finalize(const GenexEvaluationContext& ctx);

    // Extract dirty tasks from the graph (for EP task injection).
    // Returns: pair of (dirty tasks vector, last task ID for sentinel wiring)
    // The last task ID is the "final" task in the chain (e.g., install or link step).
    std::expected<std::pair<std::vector<BuildTask>, std::string>, std::string>
    extract_dirty_tasks(const std::string& build_dir);

    // Helpers for target task generation
    bool has_task(const std::string& id) const { return task_by_id_.count(id); }
    BuildTask& get_task(const std::string& id) { return *task_by_id_.at(id); }

    // Returns dependency IDs that no task produces (for resolving missing targets).
    // Checks explicit_deps (unresolved strings) since this is called before finalize().
    std::vector<std::string> get_missing_dependencies() const {
        std::vector<std::string> missing;
        for (const auto& task_ptr : tasks_) {
            for (const auto& dep : task_ptr->explicit_deps) {
                if (!task_by_id_.count(dep)) {
                    missing.push_back(dep);
                }
            }
        }
        return missing;
    }

    // C++20 modules support: inject dependencies after collator runs
    // Called by collator task to update compile task dependencies based on module imports
    void inject_module_dependencies(
        const std::map<std::string, std::string>& module_to_task,  // Module name -> provider task ID
        const std::map<std::string, std::vector<std::string>>& task_requires  // Task ID -> required modules
    );

    // ExternalProject support: run EP orchestrator task in-process
    // Called by execute() when an EP orchestrator task becomes ready.
    // Returns error message on failure, nullopt on success.
    std::optional<std::string> run_ep_orchestrator(
        BuildTask& task,
        const std::string& build_dir,
        std::unordered_set<BuildTask*>& completed,
        std::unordered_map<BuildTask*, std::optional<bool>>& dirty_state,
        std::set<BuildTask*, TaskPtrIdCmp>& ready_set,
        ProgressBar& progress,
        std::map<std::string, std::string>& new_cache,
        bool stdout_is_tty,
        std::mutex& loop_mutex,
        std::condition_variable& cv);

    // Inject tasks into the live build graph during execution.
    // DEPRECATED: Use attach_ep_graph() instead for proper incremental builds.
    void inject_tasks(
        std::vector<BuildTask> new_tasks,
        const std::string& sentinel_id,
        const std::string& last_task_id,
        const std::string& ep_binary_dir,
        std::unordered_set<BuildTask*>& completed,
        std::unordered_map<BuildTask*, std::optional<bool>>& dirty_state,
        std::set<BuildTask*, TaskPtrIdCmp>& ready_set,
        ProgressBar& progress);

    // Atomically attach entire EP graph to main graph.
    // Unlike inject_tasks(), this attaches ALL tasks (not just dirty ones).
    // Clean tasks are added to completed immediately; dirty tasks execute normally.
    // Returns dirty_count for progress reporting.
    // IMPORTANT: Caller must NOT hold loop_mutex; this function acquires it.
    std::expected<int, std::string>
    attach_ep_graph(
        BuildGraph&& ep_graph,
        const std::string& ep_binary_dir,
        std::unordered_set<BuildTask*>& completed,
        std::unordered_map<BuildTask*, std::optional<bool>>& dirty_state,
        std::set<BuildTask*, TaskPtrIdCmp>& ready_set,
        ProgressBar& progress,
        std::mutex& loop_mutex,
        std::condition_variable& cv);

    // Access to tasks (needed by EP orchestrator for isolated interpreter)
    const std::vector<std::unique_ptr<BuildTask>>& get_tasks() const { return tasks_; }
    size_t task_count() const { return tasks_.size(); }

private:
    std::vector<std::unique_ptr<BuildTask>> tasks_;
    std::unordered_map<std::string, BuildTask*> task_by_id_;
    mutable std::mutex output_mutex_;
    mutable std::mutex state_mutex_;
    mutable std::mutex graph_mutation_mutex_;  // For thread-safe module dependency injection

    // Resolve explicit_deps (string IDs) to pointer-based dependencies.
    // Called by finalize() after all tasks are created and genex evaluated.
    void resolve_explicit_deps();

    // Keeps EP child interpreter targets alive while injected tasks hold raw parent_target pointers
    std::vector<std::shared_ptr<Target>> ep_target_owners_;

    // Incremental build logic
    std::expected<std::string, std::string> calculate_signature(const BuildTask& task);
    std::map<std::string, std::string> load_cache(const std::string& build_dir);
    std::expected<void, std::string> save_cache(const std::string& build_dir, const std::map<std::string, std::string>& cache);

    // Returns file mtime, or nullopt if file doesn't exist (single syscall)
    std::optional<std::filesystem::file_time_type> get_file_time_if_exists(const std::string& path);
    std::map<std::string, std::optional<std::filesystem::file_time_type>> stat_cache_;

    // Cache for parsed .d files: avoids re-reading/parsing on every build
    // Memory: ~8KB per source file for LLVM-scale projects (~80MB total)
    struct DepsFileCache {
        std::filesystem::file_time_type d_file_mtime;
        std::vector<std::string> deps;
    };
    std::map<std::string, DepsFileCache> deps_cache_;

    std::expected<std::string, std::string> get_compiler_version();
    std::optional<std::string> compiler_version_cache_;
    std::string get_dmake_version() { return "0.1.0-alpha (task-refactor)"; }

    // Parsers for .d files (header dependencies) - uses deps_cache_
    std::vector<std::string> get_deps_for_output(const std::string& output_path);

    // Subprocess execution with output capture
    CommandResult run_command(const std::vector<std::string>& command, const std::string& working_dir = "");
};

} // namespace dmake
