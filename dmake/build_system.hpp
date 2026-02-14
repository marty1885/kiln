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
#include <span>
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
class GraphTransaction;
class LockedGraphTransaction;
struct ExecutionState;

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
          output_to_task_(std::move(other.output_to_task_)),
          pending_file_deps_(std::move(other.pending_file_deps_)),
          dependents_(std::move(other.dependents_)),
          ep_target_owners_(std::move(other.ep_target_owners_)),
          stat_cache_(std::move(other.stat_cache_)),
          deps_cache_(std::move(other.deps_cache_)),
          compiler_version_cache_(std::move(other.compiler_version_cache_)) {}

    BuildGraph& operator=(BuildGraph&& other) noexcept {
        if (this != &other) {
            tasks_ = std::move(other.tasks_);
            task_by_id_ = std::move(other.task_by_id_);
            output_to_task_ = std::move(other.output_to_task_);
            pending_file_deps_ = std::move(other.pending_file_deps_);
            dependents_ = std::move(other.dependents_);
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

    // Checks for cycles and returns an error message if one is found
    std::optional<std::string> check_for_cycles();

    // Executes the graph.
    std::expected<void, std::string> execute(const std::string& build_dir, int jobs = 0);

    std::expected<void, std::string> generate_compile_commands(const std::string& build_dir);

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
        BuildTask& task, ExecutionState& state);

    // Atomically attach entire EP graph to main graph.
    // Unlike inject_tasks(), this attaches ALL tasks (not just dirty ones).
    // Clean tasks are added to completed immediately; dirty tasks execute normally.
    // Returns dirty_count for progress reporting.
    // IMPORTANT: Caller must NOT hold loop_mutex; this function acquires it.
    std::expected<int, std::string>
    attach_ep_graph(
        BuildGraph&& ep_graph, const std::string& ep_binary_dir, ExecutionState& state);

    // Access to tasks (needed by EP orchestrator for isolated interpreter)
    const std::vector<std::unique_ptr<BuildTask>>& get_tasks() const { return tasks_; }
    size_t task_count() const { return tasks_.size(); }

    // Read reverse edges (used by execute worker loop)
    std::span<BuildTask* const> get_dependents(BuildTask* task) const;

    // Transaction factory methods
    GraphTransaction begin();
    LockedGraphTransaction begin_locked(std::mutex& mtx);

    // --- Post-transaction graph-wide steps ---

    // Evaluate genex in all task commands/working_dirs. Adds inferred inputs.
    std::expected<void, std::string> evaluate_genex(const GenexEvaluationContext& ctx);

    // Resolve file deps created by genex-inferred inputs
    void resolve_inferred_file_deps();

    // CMake compat: wire ALL custom targets before compilation tasks
    void apply_cmake_compat_deps();

    // Validate graph: cycle check + warn about missing inputs
    std::expected<void, std::string> validate();

private:
    friend class GraphTransaction;
    friend class LockedGraphTransaction;

    std::vector<std::unique_ptr<BuildTask>> tasks_;
    std::unordered_map<std::string, BuildTask*> task_by_id_;

    // Persistent indexes (always up-to-date)
    std::unordered_map<std::string, BuildTask*> output_to_task_;
    std::unordered_multimap<std::string, BuildTask*> pending_file_deps_;

    // Reverse edges — graph-private, never on the task itself
    std::unordered_map<BuildTask*, std::vector<BuildTask*>> dependents_;

    mutable std::mutex output_mutex_;
    mutable std::mutex state_mutex_;
    mutable std::mutex graph_mutation_mutex_;  // For thread-safe module dependency injection

    // Single point of edge creation — always bidirectional
    void add_dependency(BuildTask* from, BuildTask* to);

    // Indexed task insertion — updates task_by_id_ + output_to_task_
    std::expected<BuildTask*, std::string> add_task_internal(std::unique_ptr<BuildTask> task);

    // Batch resolution steps (operate ONLY on the given batch)
    void resolve_explicit_deps(std::span<BuildTask*> batch);
    void resolve_file_deps(std::span<BuildTask*> batch);
    void drain_pending_deps(std::span<BuildTask*> batch);

    // Resolve explicit_deps (string IDs) to pointer-based dependencies.


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

class GraphTransaction {
public:
    explicit GraphTransaction(BuildGraph& g) : graph_(g) {}
    ~GraphTransaction() noexcept(false) {
        if (!committed_) {
            auto result = commit();
            if (!result) {
                throw std::runtime_error("GraphTransaction commit failed in destructor: " + result.error());
            }
        }
    }

    // Non-copyable, non-movable
    GraphTransaction(const GraphTransaction&) = delete;
    GraphTransaction& operator=(const GraphTransaction&) = delete;

    // Add a new task (constructed externally, moved in)
    std::expected<BuildTask*, std::string> add(BuildTask task);

    // Add a pre-built unique_ptr (for EP graph transfer — preserves internal pointer edges)
    std::expected<BuildTask*, std::string> add_owned(std::unique_ptr<BuildTask> task);

    // Wire dependency (pointer — immediate, bidirectional)
    void dependency(BuildTask* from, BuildTask* to);

    // Wire dependency (string ID — deferred to commit)
    void dependency(BuildTask* from, const std::string& dep_id);

    // Query: does this task exist in graph OR in this batch?
    bool has_task(const std::string& id) const;
    BuildTask* find_task(const std::string& id) const;

    // Commit: resolve all deferred deps, wire file deps, drain pending
    std::expected<void, std::string> commit();

    // Access batch (e.g. for post-commit dirty computation)
    std::span<BuildTask* const> batch() const { return batch_; }

protected:
    BuildGraph& graph_;
    std::vector<BuildTask*> batch_;
    bool committed_ = false;
};

class LockedGraphTransaction : public GraphTransaction {
public:
    LockedGraphTransaction(BuildGraph& g, std::mutex& mtx)
        : GraphTransaction(g), lock_(mtx) {}

    // Lock held for lifetime. Destructor: commit (via base), then release lock.

private:
    std::unique_lock<std::mutex> lock_;
};

} // namespace dmake
