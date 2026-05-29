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
#include <array>
#include <sys/types.h>
#if defined(__linux__)
#include <sys/prctl.h>
#elif defined(__FreeBSD__)
#include <sys/procctl.h>
#endif
#include "utils.hpp"
#include "language.hpp"

namespace kiln {

template <class... Ts> struct overloaded : Ts... {
    using Ts::operator()...;
};

struct CompileTask {
    std::string source_file;
    std::optional<Language> compile_language;
};
struct PCHTask {
    std::string source_file;
};
struct LinkTask {};
struct CustomCommandTask {};
struct CustomTargetTask {};
struct PreBuildTask {};
struct PostBuildTask {};
struct ModuleScannerTask {
    std::string source_file;
};
struct ModuleCollatorTask {
    // Compiler used to build header units this target imports. Nullable
    // (e.g. when modules are off, or when the target only consumes named
    // modules and doesn't end up importing any header unit). Owned by the
    // Toolchain, which outlives the build graph.
    const class Compiler* cxx_compiler = nullptr;
    std::string cxx_standard; // numeric form, e.g. "20", "23"
    bool cxx_extensions_enabled = true;
};
struct EPOrchestratorTask {
    std::string ep_name;
};
struct EPSentinelTask {
    std::string ep_name;
};
struct EPInstallTask {
    std::string ep_name;
};
struct MocTask {
    std::string source_file;
};
struct UicTask {
    std::string source_file;
};
struct RccTask {
    std::string source_file;
};

using TaskKind =
    std::variant<CompileTask, PCHTask, LinkTask, CustomCommandTask, CustomTargetTask, PreBuildTask, PostBuildTask, ModuleScannerTask,
                 ModuleCollatorTask, EPOrchestratorTask, EPSentinelTask, EPInstallTask, MocTask, UicTask, RccTask>;

// Forward declarations
class ProgressBar;
class GraphTransaction;
class LockedGraphTransaction;
struct ExecutionState;

// Set by signal handlers to request graceful shutdown.
// The build loop checks this and stops dispatching new tasks,
// then saves the cache so completed work isn't lost.
inline std::atomic<bool> g_interrupted{false};

// Tracked child PIDs so the signal handler can forward signals to them.
// Children are placed in their own process group via setpgid(0, 0) so the
// terminal's Ctrl-C is delivered only to kiln; the handler then explicitly
// kill()s registered children. Slot=0 means free. Bounded array avoids any
// allocation/locking in the async-signal-safe handler.
inline constexpr std::size_t kMaxTrackedChildren = 256;
inline std::array<std::atomic<pid_t>, kMaxTrackedChildren> g_child_pids{};

inline void register_child_pid(pid_t pid) {
    for (auto& slot : g_child_pids) {
        pid_t expected = 0;
        if (slot.compare_exchange_strong(expected, pid, std::memory_order_acq_rel)) return;
    }
}

inline void unregister_child_pid(pid_t pid) {
    for (auto& slot : g_child_pids) {
        pid_t expected = pid;
        if (slot.compare_exchange_strong(expected, 0, std::memory_order_acq_rel)) return;
    }
}

// Ask the kernel to send `sig` to this process when its parent dies. Closes
// the orphan-on-crash window: if kiln segfaults or is SIGKILL'd, the child
// gets killed too instead of being reparented to init. Called from the child
// right after fork. No-op (with no error) on platforms that lack a primitive.
inline void set_parent_death_signal(int sig) {
#if defined(__linux__)
    ::prctl(PR_SET_PDEATHSIG, sig);
#elif defined(__FreeBSD__)
    int s = sig;
    ::procctl(P_PID, 0, PROC_PDEATHSIG_CTL, &s);
#else
    // macOS, OpenBSD, NetBSD: no kernel primitive. Children orphan on parent
    // crash; they typically die soon after via SIGPIPE on the closed stdout
    // pipe. Matches CMake/ninja/make behavior on these platforms.
    (void) sig;
#endif
}

class Target;
class Interpreter;
class ExternalProjectTarget;
class Toolchain;
struct GenexEvaluationContext;

struct BuildTask {
    std::string id;            // Unique identifier (usually the primary output file)
    TaskKind kind{LinkTask{}}; // Variant-based task type
    std::vector<std::vector<std::string>> commands;
    // Parallel to commands, but with cosmetic flags (e.g. -fdiagnostics-color)
    // omitted. Used for cache-signature hashing so toggling presentation
    // flags doesn't bust cache hits. When empty, calculate_signature falls
    // back to commands. When populated, must have the same length as commands.
    std::vector<std::vector<std::string>> signature_commands;
    std::vector<std::string> inputs;
    std::vector<std::string> outputs;
    Target* parent_target = nullptr;
    bool always_run = false;
    std::string working_dir;
    std::string ep_binary_dir; // EP binary dir for cache routing (empty = use main cache)

    // Dependency edges (resolved pointers, set by graph)
    std::vector<BuildTask*> dependencies;

    // Unresolved dependency IDs (set during task creation, resolved to pointers in finalize)
    std::vector<std::string> explicit_deps;

    // Filled during execution for critical path computation
    double execution_time_s = 0.0; // wall time for this task
    double critical_path_s = 0.0;  // longest chain ending at this task

    // --- Convenience query methods ---

    bool is_compilation() const { return std::holds_alternative<CompileTask>(kind) || std::holds_alternative<PCHTask>(kind); }

    bool is_shell_command() const {
        return std::holds_alternative<CustomCommandTask>(kind) || std::holds_alternative<CustomTargetTask>(kind)
               || std::holds_alternative<PostBuildTask>(kind) || std::holds_alternative<MocTask>(kind)
               || std::holds_alternative<UicTask>(kind) || std::holds_alternative<RccTask>(kind);
    }

    bool is_ep_task() const {
        return std::holds_alternative<EPOrchestratorTask>(kind) || std::holds_alternative<EPSentinelTask>(kind)
               || std::holds_alternative<EPInstallTask>(kind);
    }

    bool is_marker_task() const {
        if (outputs.empty() && commands.empty() && !std::holds_alternative<ModuleCollatorTask>(kind)
            && !std::holds_alternative<EPOrchestratorTask>(kind) && !std::holds_alternative<EPSentinelTask>(kind))
            return true;
        return false;
    }

    std::string_view get_source_file() const {
        return std::visit(overloaded{[](const CompileTask& t) -> std::string_view { return t.source_file; },
                                     [](const PCHTask& t) -> std::string_view { return t.source_file; },
                                     [](const ModuleScannerTask& t) -> std::string_view { return t.source_file; },
                                     [](const MocTask& t) -> std::string_view { return t.source_file; },
                                     [](const UicTask& t) -> std::string_view { return t.source_file; },
                                     [](const RccTask& t) -> std::string_view { return t.source_file; },
                                     [](const auto&) -> std::string_view { return {}; }},
                          kind);
    }

    std::string_view get_ep_name() const {
        return std::visit(overloaded{[](const EPOrchestratorTask& t) -> std::string_view { return t.ep_name; },
                                     [](const EPSentinelTask& t) -> std::string_view { return t.ep_name; },
                                     [](const EPInstallTask& t) -> std::string_view { return t.ep_name; },
                                     [](const auto&) -> std::string_view { return {}; }},
                          kind);
    }

    std::optional<Language> get_compile_language() const {
        if (auto* ct = std::get_if<CompileTask>(&kind)) return ct->compile_language;
        return std::nullopt;
    }
};

// Comparator for deterministic ordering of BuildTask pointers (by task ID)
struct TaskPtrIdCmp {
    bool operator()(const BuildTask* a, const BuildTask* b) const { return a->id < b->id; }
};

class BuildGraph {
public:
    BuildGraph() = default;

    // Move constructor/assignment - needed because mutexes aren't movable
    BuildGraph(BuildGraph&& other) noexcept
        : tasks_(std::move(other.tasks_)), task_by_id_(std::move(other.task_by_id_)), output_to_task_(std::move(other.output_to_task_)),
          pending_file_deps_(std::move(other.pending_file_deps_)), dependents_(std::move(other.dependents_)),
          ep_target_owners_(std::move(other.ep_target_owners_)), stat_cache_(std::move(other.stat_cache_)),
          deps_cache_(std::move(other.deps_cache_)), toolchain_(other.toolchain_) {}

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
            toolchain_ = other.toolchain_;
        }
        return *this;
    }

    // Disable copying
    BuildGraph(const BuildGraph&) = delete;
    BuildGraph& operator=(const BuildGraph&) = delete;

    // Checks for cycles and returns an error message if one is found
    std::optional<std::string> check_for_cycles();

    // Executes the graph.
    // If output_capture is non-null, command stdout/stderr that would normally
    // be printed to the terminal is appended there instead (used by
    // try_compile to populate OUTPUT_VARIABLE).
    std::expected<void, std::string> execute(const std::string& build_dir, int jobs = 0, std::string* output_capture = nullptr);

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
                if (!task_by_id_.count(dep)) { missing.push_back(dep); }
            }
            // Also report file inputs not yet produced by any task
            for (const auto& in : task_ptr->inputs) {
                if (!output_to_task_.count(in)) { missing.push_back(in); }
            }
        }
        return missing;
    }

    // Header-unit info shared between the collator and the injector.
    struct HeaderUnitInfo {
        std::string source_path;
        bool is_system = false;
    };

    // C++20 modules support: inject dependencies after collator runs
    // Called by collator task to update compile task dependencies based on module imports
    void inject_module_dependencies(const std::map<std::string, std::string>& module_to_task,            // Module name -> provider task ID
                                    const std::map<std::string, std::vector<std::string>>& task_requires // Task ID -> required modules
    );

    // Header-unit support: spawn header-unit compile tasks and wire
    // compile-task → header-unit-task edges. Called by the collator after
    // the mapper has been written, so GCC can read it on the spawned
    // compiles. header_units is keyed by the resolved header path; the
    // is_system flag selects -fmodule-header=system vs =user.
    void inject_header_unit_tasks(Target& parent_target, const class Compiler* cxx_compiler, const std::string& cxx_standard,
                                  bool cxx_extensions_enabled, const std::string& mapper_path,
                                  const std::map<std::string, HeaderUnitInfo>& header_units,
                                  const std::map<std::string, std::string>& header_unit_to_bmi,
                                  const std::map<std::string, std::vector<std::string>>& task_header_units, std::string& task_error);

    // ExternalProject support: run EP orchestrator task in-process
    // Called by execute() when an EP orchestrator task becomes ready.
    // Returns error message on failure, nullopt on success.
    std::optional<std::string> run_ep_orchestrator(BuildTask& task, ExecutionState& state);

    // Atomically attach entire EP graph to main graph.
    // Unlike inject_tasks(), this attaches ALL tasks (not just dirty ones).
    // Clean tasks are added to completed immediately; dirty tasks execute normally.
    // Returns dirty_count for progress reporting.
    // IMPORTANT: Caller must NOT hold loop_mutex; this function acquires it.
    std::expected<int, std::string> attach_ep_graph(BuildGraph&& ep_graph, const std::string& ep_binary_dir, ExecutionState& state);

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
    mutable std::mutex graph_mutation_mutex_; // For thread-safe module dependency injection

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

    // True iff task has at least one output and all outputs exist on disk.
    // Empty outputs → false (caller decides if that means "dirty" or "skip").
    bool all_outputs_exist(const BuildTask& task);

    // If the task is clean (outputs exist, not always_run, sig matches cache),
    // returns the matching signature; else nullopt. Used by both the pre-scan
    // (skip clean tasks) and the worker's "maybe dirty" runtime re-check.
    std::optional<std::string> clean_signature(const BuildTask& task, const std::map<std::string, std::string>& cache);

    // Tri-state BFS over reverse edges. Seeds from every entry already in
    // dirty_state. Definitely-dirty propagates as definite; maybe-dirty
    // propagates as maybe; an existing maybe entry reached by a definite
    // parent is upgraded to definite.
    void propagate_dirty_bfs(std::unordered_map<BuildTask*, std::optional<bool>>& dirty_state);

    // Atomically erase the progress bar and dump captured output to stderr.
    // Caller must NOT hold output_mutex_.
    void report_command_failure(ExecutionState& state, std::string_view captured_output);

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

    // Toolchain pointer for compiler-version lookup at signature time.
    // Set by Interpreter::generate_build_graph after the graph is built.
    // Versions are detected once at startup (enable_language ->
    // detect_platform) and stored on the Compiler instance; we just look
    // them up here, never re-probing arbitrary binaries.
    const Toolchain* toolchain_ = nullptr;

public:
    void set_toolchain(const Toolchain* t) { toolchain_ = t; }

private:
    std::string get_kiln_version();

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
            if (!result) { throw std::runtime_error("GraphTransaction commit failed in destructor: " + result.error()); }
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
    LockedGraphTransaction(BuildGraph& g, std::mutex& mtx) : GraphTransaction(g), lock_(mtx) {}

    // Lock held for lifetime. Destructor: commit (via base), then release lock.

private:
    std::unique_lock<std::mutex> lock_;
};

} // namespace kiln
