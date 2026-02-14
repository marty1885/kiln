#include "build_system.hpp"
#include "target.hpp"
#include "intercept/external_project_target.hpp"
#include "utils.hpp"
#include "container_utils.hpp"
#include "module_scanner.hpp"
#include "genex_evaluator.hpp"
#include "profiler.hpp"
#include "printing.hpp"
#include "CMakeArray.hpp"
#include "progress_bar.hpp"
#include "interperter.hpp"
#include "toolchain.hpp"
#include "install_executor.hpp"
#include <glaze/core/reflect.hpp>
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <array>
#include <thread>
#include <condition_variable>
#include <functional>
#include <glaze/glaze.hpp>
#include <type_traits>
#include <unordered_map>
#include <cassert>

namespace dmake {

struct CompileCommand {
    std::string directory;
    std::string command;
    std::string file;
    std::string output;
};

// Helper: Compute install task inputs and pre-compute target install paths.
// Returns the output paths of targets being installed (so install task depends on link tasks).
// Also populates ep_target with pre-computed (artifact_path, dest_path) pairs for TARGETS rules.
static std::vector<std::string> compute_install_inputs(
    const std::vector<std::shared_ptr<InstallRule>>& rules,
    Interpreter* ep_interp,
    const std::string& install_prefix,
    ExternalProjectTarget* ep_target)
{
    std::vector<std::string> inputs;
    if (!ep_interp) return inputs;

    for (const auto& rule : rules) {
        if (rule->targets_rule) {
            const auto& tr = *rule->targets_rule;
            // InstallTargetsRule: get output path of each target and compute destination
            for (const auto& target_name : tr.targets) {
                Target* target = ep_interp->find_target(target_name);
                if (!target) continue;

                std::string artifact_path = target->get_output_path();
                if (artifact_path.empty()) continue;

                inputs.push_back(artifact_path);

                // Determine destination based on target type
                const InstallDestination* dest = nullptr;
                if (target->get_type() == TargetType::EXECUTABLE) {
                    dest = &tr.runtime_dest;
                } else if (target->get_type() == TargetType::SHARED_LIBRARY) {
                    dest = &tr.library_dest;
                } else if (target->get_type() == TargetType::STATIC_LIBRARY) {
                    dest = &tr.archive_dest;
                }

                if (dest && !dest->destination.empty() && ep_target) {
                    // Compute full destination path
                    std::filesystem::path dest_path = std::filesystem::path(install_prefix) / dest->destination;
                    dest_path /= std::filesystem::path(artifact_path).filename();

                    PendingTargetInstall install;
                    install.artifact_path = artifact_path;
                    install.dest_path = dest_path.string();
                    ep_target->add_pending_target_install(std::move(install));
                }
            }
        }
        // InstallFilesRule, InstallDirectoryRule: input files are already on disk,
        // they don't depend on build tasks.
        // InstallScriptRule, InstallExportRule: no predictable inputs.
    }
    return inputs;
}

// Helper: Check if a command is a "make install" variant that should be skipped
// when using dmake's install rules instead of invoking make.
// Handles: "make install", "make -j4 install", "$(MAKE) install", etc.
static bool is_make_install_command(const std::vector<std::string>& cmd) {
    if (cmd.empty()) return false;

    // Check if the command is "make" or ends with "/make" or is "$(MAKE)"
    const std::string& exe = cmd[0];
    bool is_make = (exe == "make" || exe == "$(MAKE)" ||
                    exe.ends_with("/make") ||
                    exe.find("make") != std::string::npos);
    if (!is_make) return false;

    // A bare "make" in install context is likely "make install" with default target
    if (cmd.size() == 1) return true;

    // Check if any argument is "install"
    for (size_t i = 1; i < cmd.size(); ++i) {
        if (cmd[i] == "install") return true;
    }

    return false;
}

struct ExecutionState {
    // Configuration (set once, read-only during execution)
    std::string build_dir;
    bool stdout_is_tty;

    // Synchronization (protects all mutable fields below)
    std::mutex mutex;
    std::condition_variable cv;
    std::string fatal_error;

    // Task tracking (protected by mutex)
    std::unordered_set<BuildTask*> completed;
    std::unordered_set<BuildTask*> running;
    std::set<BuildTask*, TaskPtrIdCmp> ready_set;
    std::unordered_map<BuildTask*, std::optional<bool>> dirty_state;

    // Progress (ProgressBar is internally thread-safe)
    ProgressBar progress;

    // Caching (cache is read-only; new_cache/ep_caches protected by mutex)
    std::map<std::string, std::string> cache;
    std::map<std::string, std::string> new_cache;
    std::map<std::string, std::map<std::string, std::string>> ep_caches;

    ExecutionState(std::string build_dir_, bool is_tty, int task_count,
                   std::map<std::string, std::string> loaded_cache)
        : build_dir(std::move(build_dir_)), stdout_is_tty(is_tty)
        , progress(task_count, is_tty)
        , cache(std::move(loaded_cache)), new_cache(cache) {}
};

std::expected<void, std::string> BuildGraph::generate_compile_commands(const std::string& build_dir) {
    std::string current_dir = std::filesystem::current_path().string();

    auto commands = filter_map(tasks_,
        [](const auto& ptr) { return ptr->is_compilation() && !ptr->commands.empty(); },
        [&](const auto& ptr) -> CompileCommand {
            const auto& task = *ptr;
            return {
                .directory = current_dir,
                .command = join_command(task.commands[0]),
                .file = std::string(task.get_source_file()),
                .output = task.outputs.empty() ? "" : task.outputs[0]
            };
        });

    std::string json;
    if (auto ec = glz::write_json(commands, json)) {
        return std::unexpected<std::string>(glz::format_error(ec));
    }

    std::filesystem::path path = std::filesystem::path(build_dir) / "compile_commands.json";
    std::ofstream file(path);
    if (file) {
        file << json;
    }
    else {
        return std::unexpected<std::string>("Failed to create compile_commands.json for writing");
    }
    return {};
}

std::expected<void, std::string> BuildGraph::evaluate_genex(const GenexEvaluationContext& ctx) {
    for (auto& task_ptr : tasks_) {
        auto& task = *task_ptr;
        const auto& id = task.id;

        // Create per-task context with compile_language if set
        GenexEvaluationContext task_ctx = ctx;
        auto task_lang = task.get_compile_language();
        if (task_lang) {
            task_ctx.compile_language = task_lang;
        }
        GenexEvaluator evaluator(task_ctx);

        // Evaluate all command arguments
        std::vector<std::vector<std::string>> evaluated_commands;
        for (const auto& cmd : task.commands) {
            std::vector<std::string> evaluated_cmd;
            for (const auto& arg : cmd) {
                // Fast path: no genex
                if (!GenexParser::contains_genex(arg)) {
                    evaluated_cmd.push_back(arg);
                    continue;
                }
                auto eval = evaluator.evaluate(arg);
                if (!eval) {
                    std::string target_name = task.parent_target ? task.parent_target->get_name() : "<unknown>";
                    return std::unexpected(
                        "Generator expression error in task '" + id + "' (target '" + target_name + "')\n"
                        "  Argument: '" + arg + "'\n"
                        "  Error: " + eval.error());
                }
                if (!eval->empty()) {
                    for (auto sv : CMakeArrayView(*eval)) {
                        evaluated_cmd.emplace_back(sv);
                    }
                }
            }
            if (!evaluated_cmd.empty()) {
                evaluated_commands.push_back(std::move(evaluated_cmd));
            }
        }
        task.commands = std::move(evaluated_commands);

        // Infer dependencies from command arguments that reference target outputs
        for (const auto& cmd : task.commands) {
            for (const auto& arg : cmd) {
                if (arg.empty() || arg[0] != '/') continue;
                auto it = output_to_task_.find(arg);
                if (it != output_to_task_.end() && it->second != task_ptr.get()) {
                    task.inputs.push_back(arg);
                }
            }
        }

        // Evaluate working_dir if it contains genex
        if (!task.working_dir.empty() && GenexParser::contains_genex(task.working_dir)) {
            auto result = evaluator.evaluate(task.working_dir);
            if (!result) {
                return std::unexpected("Generator expression error in working_dir for task '" + id + "': " + result.error());
            }
            task.working_dir = *result;
        }
    }

    return {};
}

void BuildGraph::resolve_inferred_file_deps() {
    // After genex evaluation, tasks may have gained new inputs.
    // Wire file-based dependencies for all tasks.
    std::vector<BuildTask*> all;
    all.reserve(tasks_.size());
    for (auto& t : tasks_) all.push_back(t.get());
    resolve_file_deps(all);
}

void BuildGraph::apply_cmake_compat_deps() {
    // CMake compatibility: ALL custom targets implicitly run before compilation.
    std::vector<BuildTask*> all_custom_tasks;
    std::unordered_set<BuildTask*> excluded_tasks;

    for (const auto& task_ptr : tasks_) {
        if (!task_ptr->parent_target || !task_ptr->always_run) continue;
        auto* custom = dynamic_cast<CustomTarget*>(task_ptr->parent_target);
        if (!custom || !custom->is_build_by_default()) continue;

        all_custom_tasks.push_back(task_ptr.get());

        // BFS to find all transitive dependencies
        std::vector<BuildTask*> bfs_stack = {task_ptr.get()};
        while (!bfs_stack.empty()) {
            auto* cur = bfs_stack.back();
            bfs_stack.pop_back();
            if (!excluded_tasks.insert(cur).second) continue;
            for (auto* dep : cur->dependencies) {
                bfs_stack.push_back(dep);
            }
        }
    }

    if (!all_custom_tasks.empty()) {
        for (auto& task_ptr : tasks_) {
            if (!task_ptr->is_compilation() || excluded_tasks.count(task_ptr.get())) continue;
            for (auto* ct : all_custom_tasks) {
                if (std::find(task_ptr->dependencies.begin(), task_ptr->dependencies.end(), ct) == task_ptr->dependencies.end()) {
                    add_dependency(task_ptr.get(), ct);
                }
            }
        }
    }
}

std::expected<void, std::string> BuildGraph::validate() {
    // Cycle check
    auto cycle_err = check_for_cycles();
    if (cycle_err) return std::unexpected(*cycle_err);

    // Warn about inputs that don't exist and aren't produced by any task
    for (const auto& task_ptr : tasks_) {
        for (const auto& in : task_ptr->inputs) {
            if (!std::filesystem::exists(in) && !output_to_task_.count(in)) {
                dmake::print_message(std::cerr, "WARNING",
                    "Task '" + task_ptr->id + "' references '" + in +
                    "' which doesn't exist and isn't produced by any task "
                    "(CMake/Ninja resolves DEPENDS at generation time)");
            }
        }
    }

    return {};
}

void BuildGraph::add_dependency(BuildTask* from, BuildTask* to) {
    from->dependencies.push_back(to);
    dependents_[to].push_back(from);
}

std::span<BuildTask* const> BuildGraph::get_dependents(BuildTask* task) const {
    auto it = dependents_.find(task);
    if (it != dependents_.end()) {
        return it->second;
    }
    static const std::vector<BuildTask*> empty;
    return empty;
}

std::expected<BuildTask*, std::string> BuildGraph::add_task_internal(std::unique_ptr<BuildTask> task) {
    auto* raw = task.get();
    if (task_by_id_.count(raw->id)) {
        return std::unexpected("Task ID already exists: " + raw->id);
    }
    task_by_id_[raw->id] = raw;
    for (const auto& out : raw->outputs) {
        output_to_task_[out] = raw;
    }
    tasks_.push_back(std::move(task));
    return raw;
}

void BuildGraph::resolve_explicit_deps(std::span<BuildTask*> batch) {
    for (auto* task : batch) {
        for (const auto& dep_id : task->explicit_deps) {
            auto it = task_by_id_.find(dep_id);
            if (it != task_by_id_.end()) {
                add_dependency(task, it->second);
            }
        }
        task->explicit_deps.clear();
        // Deduplicate
        std::sort(task->dependencies.begin(), task->dependencies.end());
        task->dependencies.erase(
            std::unique(task->dependencies.begin(), task->dependencies.end()),
            task->dependencies.end());
    }
}

void BuildGraph::resolve_file_deps(std::span<BuildTask*> batch) {
    for (auto* task : batch) {
        for (const auto& in : task->inputs) {
            auto it = output_to_task_.find(in);
            if (it != output_to_task_.end() && it->second != task) {
                // Avoid duplicate dependency edges
                if (std::find(task->dependencies.begin(), task->dependencies.end(), it->second) == task->dependencies.end()) {
                    add_dependency(task, it->second);
                }
            }
        }
    }
}

void BuildGraph::drain_pending_deps(std::span<BuildTask*> batch) {
    // For each new task in the batch, check if any pending deps match its outputs
    for (auto* task : batch) {
        for (const auto& out : task->outputs) {
            auto range = pending_file_deps_.equal_range(out);
            for (auto it = range.first; it != range.second; ++it) {
                if (it->second != task) {
                    if (std::find(it->second->dependencies.begin(), it->second->dependencies.end(), task) == it->second->dependencies.end()) {
                        add_dependency(it->second, task);
                    }
                }
            }
            pending_file_deps_.erase(out);
        }
    }

    // Register any still-unresolved inputs from batch tasks as pending
    for (auto* task : batch) {
        for (const auto& in : task->inputs) {
            if (!output_to_task_.count(in)) {
                pending_file_deps_.emplace(in, task);
            }
        }
    }
}

// --- GraphTransaction implementation ---

std::expected<BuildTask*, std::string> GraphTransaction::add(BuildTask task) {
    auto ptr = std::make_unique<BuildTask>(std::move(task));
    auto result = graph_.add_task_internal(std::move(ptr));
    if (result) {
        batch_.push_back(*result);
    }
    return result;
}

std::expected<BuildTask*, std::string> GraphTransaction::add_owned(std::unique_ptr<BuildTask> task) {
    auto result = graph_.add_task_internal(std::move(task));
    if (result) {
        batch_.push_back(*result);
    }
    return result;
}

void GraphTransaction::dependency(BuildTask* from, BuildTask* to) {
    graph_.add_dependency(from, to);
}

void GraphTransaction::dependency(BuildTask* from, const std::string& dep_id) {
    // Deferred: add as explicit_dep for resolution at commit time
    from->explicit_deps.push_back(dep_id);
}

bool GraphTransaction::has_task(const std::string& id) const {
    return graph_.has_task(id);
}

BuildTask* GraphTransaction::find_task(const std::string& id) const {
    auto it = graph_.task_by_id_.find(id);
    if (it != graph_.task_by_id_.end()) return it->second;
    return nullptr;
}

std::expected<void, std::string> GraphTransaction::commit() {
    if (committed_) return {};
    committed_ = true;

    if (batch_.empty()) return {};

    // Build reverse edges for pre-existing pointer dependencies
    // (e.g. EP tasks transferred via add_owned that already have internal deps)
    for (auto* task : batch_) {
        for (auto* dep : task->dependencies) {
            graph_.dependents_[dep].push_back(task);
        }
    }

    // Resolve explicit deps → pointer edges (via add_dependency which maintains reverse edges)
    graph_.resolve_explicit_deps(batch_);

    // Wire file-based deps (inputs → producer tasks)
    graph_.resolve_file_deps(batch_);

    // Drain pending: satisfy waiting tasks, register new pending
    graph_.drain_pending_deps(batch_);

    return {};
}

GraphTransaction BuildGraph::begin() {
    return GraphTransaction(*this);
}

LockedGraphTransaction BuildGraph::begin_locked(std::mutex& mtx) {
    return LockedGraphTransaction(*this, mtx);
}

std::optional<std::string> BuildGraph::check_for_cycles() {
    std::unordered_map<BuildTask*, int> color; // 0: White, 1: Gray, 2: Black
    std::vector<BuildTask*> stack;

    std::function<std::optional<std::string>(BuildTask*)> visit = [&](BuildTask* u) -> std::optional<std::string> {
        color[u] = 1; // Gray
        stack.push_back(u);

        for (auto* v : u->dependencies) {
            if (color[v] == 1) { // Cycle!
                std::ostringstream oss;
                oss << "Circular dependency detected: ";
                for (size_t i = 0; i < stack.size(); ++i) {
                    if (stack[i] == v) {
                        for (size_t j = i; j < stack.size(); ++j) oss << stack[j]->id << " -> ";
                        oss << v->id;
                        break;
                    }
                }
                return oss.str();
            }
            if (color[v] == 0) {
                auto res = visit(v);
                if (res) return res;
            }
        }

        color[u] = 2; // Black
        stack.pop_back();
        return std::nullopt;
    };

    for (const auto& task_ptr : tasks_) {
        if (color[task_ptr.get()] == 0) {
            auto res = visit(task_ptr.get());
            if (res) return res;
        }
    }
    return std::nullopt;
}

std::expected<void, std::string> BuildGraph::execute(const std::string& build_dir, int jobs) {
    // Graph setup (dep resolution, cmake compat, reverse edges, validation) is now
    // handled by transaction commit + post-transaction methods before execute() is called.

    // Incremental check
    auto cache = load_cache(build_dir);

    // 2b. Pre-scan: determine which tasks need to execute.
    // Dirtiness is tracked as optional<bool>:
    //   - true: definitely dirty, must execute
    //   - nullopt: maybe dirty (depends on EP sentinel), schedule but re-check at runtime
    //   - not in map: clean, skip
    ProfileScope pre_scan_profile("pre-scan incremental check", "build");
    std::unordered_map<BuildTask*, std::optional<bool>> dirty_state;

    for (const auto& task_ptr : tasks_) {
        auto& task = *task_ptr;
        // Skip marker tasks (but NOT EP orchestrator/sentinel which run in-process)
        if (task.is_marker_task()) continue;

        bool outputs_exist = !task.outputs.empty();
        if (outputs_exist) {
            for (const auto& out : task.outputs) {
                if (!get_file_time_if_exists(out)) { outputs_exist = false; break; }
            }
        }

        if (!outputs_exist || task.always_run) {
            // EP sentinels and orchestrators are "maybe" - we can't know if EP will inject tasks
            // Their dependents should be re-checked at runtime
            dirty_state[task_ptr.get()] = task.is_ep_task() ? std::nullopt : std::optional<bool>(true);
            continue;
        }

        auto sig_res = calculate_signature(task);
        if (!sig_res || !(cache.count(task.id) && cache[task.id] == *sig_res)) {
            dirty_state[task_ptr.get()] = true;
        }
    }

    // Propagate: dirty deps → dirty, maybe deps → maybe (if not already in dirty_state)
    if (!dirty_state.empty()) {
        bool changed;
        do {
            changed = false;
            for (const auto& task_ptr : tasks_) {
                if (dirty_state.count(task_ptr.get())) continue;

                bool dominated_by_dirty = false;
                bool dominated_by_maybe = false;
                for (auto* dep : task_ptr->dependencies) {
                    auto it = dirty_state.find(dep);
                    if (it != dirty_state.end()) {
                        if (it->second == true) {
                            dominated_by_dirty = true;
                            break;  // Dirty dominates, no need to check further
                        } else {
                            dominated_by_maybe = true;
                        }
                    }
                }

                if (dominated_by_dirty) {
                    dirty_state[task_ptr.get()] = true;
                    changed = true;
                } else if (dominated_by_maybe) {
                    dirty_state[task_ptr.get()] = std::nullopt;
                    changed = true;
                }
            }
        } while (changed);
    }

    // Count definitely dirty tasks for progress bar
    int dirty_task_count = 0;
    int maybe_dirty_count = 0;
    for (const auto& [ptr, ds] : dirty_state) {
        if (ds == true) dirty_task_count++;
        else maybe_dirty_count++;
    }
    pre_scan_profile.stop();

    bool stdout_is_tty = isatty(STDOUT_FILENO);

    // 3. Parallel execution with fixed worker threads
    // Bundle all execution-phase state into a single struct.
    ExecutionState state(build_dir, stdout_is_tty,
                         dirty_task_count + maybe_dirty_count, std::move(cache));
    state.dirty_state = std::move(dirty_state);

    // Pre-populate completed with clean tasks (not in dirty_state).
    for (const auto& task_ptr : tasks_) {
        if (state.dirty_state.count(task_ptr.get())) continue;
        bool has_dirty_dep = false;
        for (auto* dep : task_ptr->dependencies) {
            if (state.dirty_state.count(dep)) { has_dirty_dep = true; break; }
        }
        if (!has_dirty_dep) {
            state.completed.insert(task_ptr.get());
        }
    }

    if (jobs <= 0) {
        jobs = std::thread::hardware_concurrency();
        if (jobs <= 0) jobs = 2;
    }

    // Check if all dependencies of a task are complete
    auto is_ready = [&](BuildTask* t) {
        for (auto* dep : t->dependencies) {
            if (!state.completed.count(dep)) return false;
        }
        return true;
    };

    // Initialize ready_set with dirty/maybe tasks whose deps are all complete.
    for (const auto& [ptr, ds] : state.dirty_state) {
        if (is_ready(ptr)) {
            state.ready_set.insert(ptr);
        }
    }

    auto start_time = std::chrono::steady_clock::now();

    std::vector<std::thread> workers;
    workers.reserve(jobs);

    for (int w = 0; w < jobs; w++) {
        workers.emplace_back([this, &state]() {
            bool profiling = g_profiling_enabled.load(std::memory_order_relaxed);

            while (true) {
                BuildTask* current = nullptr;

                // Grab the next ready task from ready_set
                {
                    std::unique_lock<std::mutex> lock(state.mutex);
                    state.cv.wait(lock, [&] {
                        if (!state.fatal_error.empty()) return true;
                        if (state.completed.size() + state.running.size() >= tasks_.size()) return true;
                        if (!state.ready_set.empty()) return true;
                        if (state.running.empty()) return true; // stall: nothing ready and nothing in flight
                        return g_interrupted.load(std::memory_order_relaxed);
                    });

                    if (!state.fatal_error.empty()) {
                        return;
                    }
                    // Check termination: all tasks are either completed or not in dirty_state
                    if (state.ready_set.empty() && state.running.empty()) {
                        // Check if we're truly done
                        bool all_done = true;
                        for (const auto& [ptr, ds] : state.dirty_state) {
                            if (!state.completed.count(ptr)) { all_done = false; break; }
                        }
                        if (all_done) return;
                    }
                    if (g_interrupted.load(std::memory_order_relaxed)) {
                        if (state.fatal_error.empty()) state.fatal_error = "Interrupted";
                        state.cv.notify_all();
                        return;
                    }

                    if (!state.ready_set.empty()) {
                        auto it = state.ready_set.begin();
                        current = *it;
                        state.ready_set.erase(it);
                        state.running.insert(current);
                    } else if (state.running.empty()) {
                        // Stall detection
                        std::ostringstream oss;
                        oss << "Internal error: Build graph stalled. Unresolved dependencies for tasks:";
                        for (const auto& [ptr, ds] : state.dirty_state) {
                            if (state.completed.count(ptr)) continue;
                            oss << "\n  - " << ptr->id << " depends on: ";
                            for (auto* dep : ptr->dependencies) {
                                if (!state.completed.count(dep)) oss << dep->id << " ";
                            }
                        }
                        state.fatal_error = oss.str();
                        state.cv.notify_all();
                        return;
                    } else {
                        // Spurious wakeup or other condition - wait again
                        continue;
                    }
                }

                // Execute the task (outside lock)
                auto& task = *current;
                const auto& id = task.id;
                std::string sig;
                std::string task_error;
                std::string active_display_name;  // tracks name in progress bar's active list
                auto task_start = std::chrono::steady_clock::now();

                do { // do-while(false) for break-on-error
                    // Check for "maybe" dirty tasks: if state is nullopt (not true),
                    // re-check signature at runtime. If clean, skip execution.
                    auto dirty_it = state.dirty_state.find(current);
                    if (dirty_it != state.dirty_state.end() && !dirty_it->second.has_value()) {
                        // "Maybe" dirty - re-check signature now that deps are complete
                        bool outputs_exist = !task.outputs.empty();
                        if (outputs_exist) {
                            for (const auto& out : task.outputs) {
                                if (!get_file_time_if_exists(out)) { outputs_exist = false; break; }
                            }
                        }
                        if (outputs_exist && !task.always_run) {
                            auto sig_res = calculate_signature(task);
                            if (sig_res && state.cache.count(id) && state.cache[id] == *sig_res) {
                                // Actually clean - skip execution, adjust progress total
                                state.progress.bump_total(-1);
                                sig = *sig_res;
                                break;  // Skip to completion handling
                            }
                        }
                    }

                    int64_t profile_start = 0;
                    std::string profile_name;
                    if (profiling) {
                        profile_start = Profiler::instance().now_us();
                        std::string artifact = task.parent_target ? task.parent_target->get_name() : "";
                        profile_name = std::visit(overloaded{
                            [&](const ModuleCollatorTask&) { return "collate " + artifact; },
                            [&](const ModuleScannerTask& t) { return "scan " + std::filesystem::path(t.source_file).filename().string(); },
                            [&](const CompileTask& t) { return "compile " + std::filesystem::path(t.source_file).filename().string(); },
                            [&](const PCHTask& t) { return "compile " + std::filesystem::path(t.source_file).filename().string(); },
                            [&](const LinkTask&) { return "link " + artifact; },
                            [&](const auto&) { return "run " + std::filesystem::path(id).filename().string(); }
                        }, task.kind);
                    }

                    std::string artifact_name = task.parent_target ? task.parent_target->get_name() : "unknown";

                    // Pre-create output directories and working directory
                    {
                        std::error_code ec;
                        if (!task.working_dir.empty()) {
                            std::filesystem::create_directories(task.working_dir, ec);
                        }
                        for (const auto& out : task.outputs) {
                            std::filesystem::create_directories(std::filesystem::path(out).parent_path(), ec);
                            if (ec) { task_error = "Failed to create directory for " + out + ": " + ec.message(); break; }
                        }
                    }
                    if (!task_error.empty()) break;

                    // Helper: print a permanent build status line.
                    // Uses print_line() to atomically erase bar + print + redraw
                    // in a single write, preventing flicker.
                    auto print_status = [&](std::string_view verb, std::string_view target_display) {
                        int done = state.progress.mark_completed();
                        active_display_name = std::string(target_display);
                        state.progress.task_started(active_display_name);

                        auto color = [&](std::string_view code) -> std::string_view {
                            return state.stdout_is_tty ? code : std::string_view{};
                        };

                        std::ostringstream oss;
                        if (state.stdout_is_tty) {
                            oss << color(dmake::colors::BOLD_GREEN) << std::setw(12) << verb
                                << color(dmake::colors::RESET) << " ["
                                << artifact_name << "] " << target_display;
                        } else {
                            int tot = state.progress.total();
                            int width = static_cast<int>(std::to_string(tot).size());
                            oss << "   [" << std::setw(width) << done << "/" << tot << "] "
                                << color(dmake::colors::BOLD_GREEN) << verb
                                << color(dmake::colors::RESET) << " ["
                                << artifact_name << "] " << target_display;
                        }

                        std::lock_guard<std::mutex> lock(output_mutex_);
                        state.progress.print_line(oss.str());
                    };

                    // Dispatch based on task kind
                    std::visit(overloaded{
                        [&](const EPOrchestratorTask& ep) {
                            print_status("Configuring", ep.ep_name);

                            // Run the EP orchestrator outside the lock - it acquires state.mutex when attaching the graph
                            auto ep_result = run_ep_orchestrator(task, state);
                            if (ep_result) {
                                task_error = *ep_result;
                            }
                        },
                        [&](const EPInstallTask& ep) {
                            // EP install task - runs install rules after EP build tasks complete
                            print_status("Installing", ep.ep_name);
                            auto* ep_target = dynamic_cast<ExternalProjectTarget*>(task.parent_target);
                            if (ep_target) {
                                // 1. Install pre-computed target artifacts (TARGETS rules)
                                for (const auto& install : ep_target->get_pending_target_installs()) {
                                    std::filesystem::path dest_dir = std::filesystem::path(install.dest_path).parent_path();
                                    std::filesystem::create_directories(dest_dir);
                                    std::error_code ec;
                                    std::filesystem::copy_file(install.artifact_path, install.dest_path,
                                                              std::filesystem::copy_options::overwrite_existing, ec);
                                    if (ec) {
                                        task_error = "EP " + ep.ep_name + " install failed: cannot copy " +
                                                    install.artifact_path + " to " + install.dest_path + ": " + ec.message();
                                        return;
                                    }
                                    std::cout << "-- Installing: " << install.dest_path << std::endl;
                                }
                                if (!task_error.empty()) return;

                                // 2. Run other install rules (FILES, DIRECTORY, SCRIPT, CODE)
                                // TARGETS rules are skipped since interp is null (we handled them above)
                                const auto& install_rules = ep_target->get_pending_install_rules();
                                if (!install_rules.empty()) {
                                    std::string install_prefix = ep_target->get_ep_install_dir();
                                    std::string config = ep_target->get_pending_install_config();
                                    auto install_result = execute_install_rules(nullptr, install_rules,
                                                                                install_prefix, config);
                                    if (!install_result) {
                                        task_error = "EP " + ep.ep_name + " install failed: " + install_result.error();
                                        return;
                                    }
                                }
                                // Run extra install commands from INSTALL_COMMAND (skip "make install")
                                const auto& install_cmd = ep_target->get_install_command();
                                if (!install_cmd.is_empty && !install_cmd.commands.empty()) {
                                    std::string working_dir = ep_target->get_ep_binary_dir();
                                    for (const auto& cmd : install_cmd.commands) {
                                        // Skip "make install" - we handle that with our install rules
                                        if (is_make_install_command(cmd)) continue;
                                        auto result = dmake::run_command(cmd, working_dir);
                                        if (result.exit_code != 0) {
                                            {
                                                std::lock_guard<std::mutex> lock(output_mutex_);
                                                if (!result.output.empty()) std::cerr << result.output << std::endl;
                                            }
                                            task_error = "EP " + ep.ep_name + " install command failed";
                                            return;
                                        }
                                    }
                                }
                            }
                        },
                        [&](const EPSentinelTask& ep) {
                            // Sentinel task - pure synchronization point, signals EP completion
                            // Install is now handled by a separate install task that sentinel depends on
                            print_status("Ready", ep.ep_name);
                        },
                        [&](const ModuleCollatorTask&) {
                            // C++20 modules: handle collator tasks specially (in-process execution)
                            print_status("Collating", "modules");

                            std::map<std::string, std::string> module_to_task;
                            std::map<std::string, std::vector<std::string>> task_requires;
                            std::vector<ModuleMapEntry> mapper_entries;

                            for (const auto& ddi_path : task.inputs) {
                                auto ddi_result = parse_ddi_file(ddi_path);
                                if (!ddi_result) { task_error = ddi_result.error(); return; }

                                const auto& ddi = *ddi_result;
                                std::string obj_path = get_obj_path(task.parent_target->get_binary_dir(), task.parent_target->get_name(), ddi.source);

                                if (!ddi.provides.empty()) {
                                    module_to_task[ddi.provides] = obj_path;
                                    ModuleMapEntry entry;
                                    entry.module_name = ddi.provides;
                                    entry.bmi_path = get_bmi_path(task.parent_target->get_binary_dir(), ddi.provides);
                                    entry.source_path = ddi.source;
                                    entry.object_task_id = obj_path;
                                    mapper_entries.push_back(entry);
                                }

                                if (!ddi.imports.empty()) {
                                    task_requires[obj_path] = ddi.imports;
                                }
                            }
                            if (!task_error.empty()) return;

                            std::string mapper_content = generate_module_mapper_content(mapper_entries);
                            std::ofstream mapper_file(task.outputs[0]);
                            if (!mapper_file) { task_error = "Failed to write module mapper: " + task.outputs[0]; return; }
                            mapper_file << mapper_content;
                            mapper_file.close();

                            inject_module_dependencies(module_to_task, task_requires);
                        },
                        [&](const ModuleScannerTask& scanner) {
                            std::string scan_display = std::filesystem::path(scanner.source_file).filename().string();
                            print_status("Scanning", scan_display);

                            auto result = run_command(task.commands[0], task.working_dir);
                            ModuleDependencyInfo ddi = parse_module_scan_output(result.output, scanner.source_file);
                            ddi.timestamp = std::filesystem::last_write_time(scanner.source_file);

                            auto write_result = write_ddi_file(task.outputs[0], ddi);
                            if (!write_result) { task_error = write_result.error(); }
                        },
                        [&](const auto&) {
                            // Regular task execution (compile, PCH, link, custom command/target, pre/post-build)
                            std::string verb = "Running";
                            auto src = task.get_source_file();
                            std::string target_display = src.empty() ?
                                std::filesystem::path(id).filename().string() :
                                std::filesystem::path(src).filename().string();

                            if (task.is_compilation()) {
                                 verb = "Compiling";
                            } else if (task.parent_target && id == task.parent_target->get_output_path() && task.parent_target->get_type() != TargetType::CUSTOM) {
                                verb = "  Linking";
                            }

                            if (task.parent_target && task.parent_target->get_type() == TargetType::CUSTOM) {
                                std::string comment = task.parent_target->get_property("COMMENT");
                                if (!comment.empty()) {
                                    target_display = comment;
                                }
                            }

                            print_status(verb, target_display);

                            for (auto cmd : task.commands) {
                                // Strip shell-style quoting from COMMAND args.
                                // CMake expects shell to strip quotes like -flag="value".
                                // Since we use execvp (no shell), we strip them here.
                                if (task.is_shell_command()) {
                                    for (auto& arg : cmd) {
                                        arg = strip_shell_quoting(arg);
                                    }
                                }
                                auto result = dmake::run_command(cmd, task.working_dir);
                                if (result.exit_code != 0) {
                                    {
                                        std::lock_guard<std::mutex> lock(output_mutex_);
                                        state.progress.erase();
                                        std::cout.flush();  // erase wrote to cout; flush before cerr
                                        if (!result.output.empty()) std::cerr << result.output << std::endl;
                                    }
                                    task_error = "Command failed: " + join_command(cmd);
                                    return;
                                } else if (!result.output.empty()) {
                                    std::lock_guard<std::mutex> lock(output_mutex_);
                                    state.progress.print_line(result.output);
                                }
                            }
                        }
                    }, task.kind);
                    if (!task_error.empty()) break;

                    // Emit profiling event with full command details
                    if (profiling) {
                        auto dur = Profiler::instance().now_us() - profile_start;
                        Profiler::Args args;
                        if (!task.commands.empty()) {
                            std::string cmds;
                            for (const auto& cmd : task.commands) {
                                if (!cmds.empty()) cmds += " && ";
                                cmds += join_command(cmd);
                            }
                            args = std::map<std::string, std::string>{{"cmd", std::move(cmds)}};
                        }
                        Profiler::instance().add_complete(profile_name, "build", profile_start, dur, std::move(args));
                    }

                    // Invalidate stat cache for outputs so downstream tasks
                    // see fresh mtimes (e.g. link task sees recompiled .o)
                    {
                        std::lock_guard<std::mutex> lock(state_mutex_);
                        for (const auto& out : task.outputs) {
                            stat_cache_.erase(out);
                        }
                    }

                    // Recalculate signature after compilation (now .d files exist)
                    if (!task.always_run) {
                        auto new_sig_res = calculate_signature(task);
                        if (!new_sig_res) { task_error = new_sig_res.error(); break; }
                        sig = *new_sig_res;
                    }
                } while (false);

                // Remove from active task list if we added it
                if (!active_display_name.empty()) {
                    state.progress.task_finished(active_display_name);
                }

                // Mark task complete (or failed)
                {
                    std::lock_guard<std::mutex> lock(state.mutex);

                    // Record execution time for critical path computation
                    auto task_end = std::chrono::steady_clock::now();
                    task.execution_time_s = std::chrono::duration<double>(task_end - task_start).count();

                    if (!task_error.empty()) {
                        state.fatal_error = task_error;
                    } else {
                        // Route cache entry to appropriate cache file
                        if (!task.ep_binary_dir.empty()) {
                            // EP task: save to EP-specific cache
                            state.ep_caches[task.ep_binary_dir][id] = sig;
                        } else {
                            // Main project task
                            state.new_cache[id] = sig;
                        }
                    }
                    state.completed.insert(current);
                    state.running.erase(current);

                    // Check if any dirty/maybe dependents are now ready
                    for (auto* dep_task : get_dependents(current)) {
                        if (!state.dirty_state.count(dep_task)) continue;  // clean task, skip
                        if (state.completed.count(dep_task)) continue;   // already done
                        if (state.running.count(dep_task)) continue;     // already running
                        if (state.ready_set.count(dep_task)) continue;   // already in ready set

                        // Check if all its dependencies are complete
                        bool ready = true;
                        for (auto* d : dep_task->dependencies) {
                            if (!state.completed.count(d)) { ready = false; break; }
                        }
                        if (ready) state.ready_set.insert(dep_task);
                    }

                    state.cv.notify_all();
                }
            }
        });
    }

    for (auto& w : workers) {
        if (w.joinable()) w.join();
    }

    // Always save cache — even on failure, successful tasks should be cached
    // so we don't redo them on the next build.
    save_cache(state.build_dir, state.new_cache);

    // Save EP-specific caches to their respective build directories
    for (const auto& [ep_dir, ep_cache_entries] : state.ep_caches) {
        // Load existing EP cache and merge with new entries
        auto ep_cache = load_cache(ep_dir);
        for (const auto& [task_id, sig] : ep_cache_entries) {
            ep_cache[task_id] = sig;
        }
        save_cache(ep_dir, ep_cache);
    }

    state.progress.finish();

    if (!state.fatal_error.empty()) {
        return std::unexpected(state.fatal_error);
    }

    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

    // Compute critical path: longest dependency chain by execution time
    double max_critical_path = 0.0;
    if (dirty_task_count > 1) {
        std::unordered_set<BuildTask*> visited;
        std::function<double(BuildTask*)> compute_critical = [&](BuildTask* t) -> double {
            if (t->critical_path_s > 0.0) return t->critical_path_s;  // memoized
            if (!visited.insert(t).second) return 0.0;  // cycle guard

            double dep_max = 0.0;
            for (auto* dep : t->dependencies) {
                dep_max = std::max(dep_max, compute_critical(dep));
            }
            t->critical_path_s = t->execution_time_s + dep_max;
            return t->critical_path_s;
        };

        for (const auto& task_ptr : tasks_) {
            max_critical_path = std::max(max_critical_path, compute_critical(task_ptr.get()));
        }
    }

    {
        std::lock_guard<std::mutex> lock(output_mutex_);
        double wall_s = duration.count() / 1000.0;
        std::cout << dmake::c(std::cout, dmake::colors::BOLD_GREEN) << std::setw(12) << "Finished" << dmake::c(std::cout, dmake::colors::RESET) << " build in "
                << std::fixed << std::setprecision(2) << wall_s << "s";
        if (max_critical_path > 0.0) {
            std::cout << " (critical path: " << std::setprecision(2) << max_critical_path << "s)";
        }
        std::cout << std::endl;
    }

    return {};
}

dmake::CommandResult BuildGraph::run_command(const std::vector<std::string>& command, const std::string& working_dir) {
    return dmake::run_command(command, working_dir);
}

std::vector<std::string> BuildGraph::get_deps_for_output(const std::string& output_path) {
    std::string deps_file = output_path + ".d";

    // Check if .d file exists and get its mtime
    auto d_mtime = get_file_time_if_exists(deps_file);
    if (!d_mtime) return {};

    // Check cache
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = deps_cache_.find(deps_file);
        if (it != deps_cache_.end() && it->second.d_file_mtime == *d_mtime) {
            return it->second.deps;
        }
    }

    // Cache miss - parse the .d file
    std::ifstream file(deps_file);
    if (!file) return {};

    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    size_t colon = content.find(':');
    if (colon == std::string::npos) return {};

    std::string deps_part = content.substr(colon + 1);
    std::replace(deps_part.begin(), deps_part.end(), '\\', ' ');
    std::replace(deps_part.begin(), deps_part.end(), '\n', ' ');

    std::vector<std::string> deps;
    std::stringstream ss(deps_part);
    std::string dep;
    while (ss >> dep) {
        deps.push_back(dep);
    }

    // Store in cache
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        deps_cache_[deps_file] = DepsFileCache{*d_mtime, deps};
    }

    return deps;
}

static std::expected<std::vector<std::string>, std::string> get_headers_via_h_flag(const std::vector<std::vector<std::string>>& commands) {
    if (commands.empty()) return std::vector<std::string>{};
    const auto& command = commands[0];

    std::vector<std::string> scan_cmd;
    scan_cmd.push_back("g++");
    scan_cmd.push_back("-H");
    scan_cmd.push_back("-E");

    std::string source;
    for (size_t i = 0; i < command.size(); ++i) {
        const auto& arg = command[i];
        if (arg.starts_with("-I") || arg.starts_with("-D") || arg.starts_with("-std=") ||
            arg.starts_with("-f") || arg == "-include") {
            scan_cmd.push_back(arg);
            if (arg == "-include" && i + 1 < command.size()) {
                scan_cmd.push_back(command[++i]);
            }
        } else if (arg.ends_with(".cpp") || arg.ends_with(".c") || arg.ends_with(".cc")) {
            source = arg;
        }
    }

    if (source.empty()) return std::vector<std::string>{};
    scan_cmd.push_back(source);

    // Redirect stdout to /dev/null for the scan command
    std::string full_cmd = join_command(scan_cmd) + " 2>&1 > /dev/null";

    std::array<char, 256> buffer;
    std::vector<std::string> headers;

    FILE* pipe = popen(full_cmd.c_str(), "r");
    if (!pipe) return std::unexpected("Failed to execute g++ for header scanning");

        std::string full_output;
        while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
            std::string line(buffer.data());
            full_output += line;
            if (line.starts_with(".")) {
                // Lines look like ". /path/to/header.h" or ".. /path/to/header.h"
                size_t first_space = line.find(' ');
                if (first_space != std::string::npos) {
                    std::string header = line.substr(first_space + 1);
                    if (!header.empty() && header.back() == '\n') header.pop_back();
                    headers.push_back(header);
                }
            }
        }

        int status = pclose(pipe);
        if (status != 0) {
            return std::unexpected("Header scanning failed for " + source + " with exit code " + std::to_string(status) + "\nOutput:\n" + full_output);
        }
        return headers;
    }
std::optional<std::filesystem::file_time_type> BuildGraph::get_file_time_if_exists(const std::string& path) {
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = stat_cache_.find(path);
        if (it != stat_cache_.end()) return it->second;
    }

    std::optional<std::filesystem::file_time_type> result;
    try {
        result = std::filesystem::last_write_time(path);
    } catch (...) {
        // File doesn't exist or not accessible
        result = std::nullopt;
    }

    std::lock_guard<std::mutex> lock(state_mutex_);
    stat_cache_[path] = result;
    return result;
}

std::expected<std::string, std::string> BuildGraph::calculate_signature(const BuildTask& task) {
    std::ostringstream oss;
    oss << "cmds:";
    for (const auto& cmd : task.commands) oss << join_command(cmd) << ";";
    oss << "|";

    auto version_res = get_compiler_version();
    if (!version_res) return std::unexpected(version_res.error());
    oss << "compiler:" << *version_res << "|";

    oss << "dmake:" << get_dmake_version() << "|";

    // 1. Primary inputs (combined exists+stat)
    for (const auto& in : task.inputs) {
        if (auto mtime = get_file_time_if_exists(in)) {
            oss << in << ":" << mtime->time_since_epoch().count() << "|";
        }
    }

    // 2. Header dependencies from .d files (cached parsing + combined exists+stat)
    bool found_deps = false;
    for (const auto& out : task.outputs) {
        auto deps = get_deps_for_output(out);
        if (!deps.empty()) {
            for (const auto& dep : deps) {
                if (auto mtime = get_file_time_if_exists(dep)) {
                    oss << "dep:" << dep << ":" << mtime->time_since_epoch().count() << "|";
                }
            }
            found_deps = true;
        }
    }

    // 3. If no .d file exists but it's a compile task, use g++ -H (slow but accurate path)
    // Skip for ASM tasks - g++ -H doesn't work reliably with raw .s files
    if (!found_deps && task.is_compilation() && task.get_compile_language() != std::optional{Language::ASM}) {
        auto headers_res = get_headers_via_h_flag(task.commands);
        if (!headers_res) return std::unexpected(headers_res.error());
        for (const auto& header : *headers_res) {
            if (auto mtime = get_file_time_if_exists(header)) {
                oss << "ext_dep:" << header << ":" << mtime->time_since_epoch().count() << "|";
            }
        }
    }

    return oss.str();
}

std::map<std::string, std::string> BuildGraph::load_cache(const std::string& build_dir) {
    std::map<std::string, std::string> cache;

    // Validate build_dir hasn't moved
    std::error_code ec;
    auto canonical_build_dir = std::filesystem::canonical(build_dir, ec);
    if (ec) return cache;

    std::ifstream meta_file(std::filesystem::path(build_dir) / ".dmake_build_path");
    if (meta_file) {
        std::string cached_path;
        std::getline(meta_file, cached_path);
        if (cached_path != canonical_build_dir.string()) {
            // Build dir moved - invalidate cache
            return cache;
        }
    }

    std::ifstream file(std::filesystem::path(build_dir) / ".dmake_cache");
    if (!file) return cache;

    std::string line;
    while (std::getline(file, line)) {
        size_t sep = line.find('=');
        if (sep != std::string::npos) {
            cache[line.substr(0, sep)] = line.substr(sep + 1);
        }
    }
    return cache;
}

std::expected<void, std::string> BuildGraph::save_cache(const std::string& build_dir, const std::map<std::string, std::string>& cache) {
    std::error_code ec;
    std::filesystem::create_directories(build_dir, ec);
    if (ec) {
        return std::unexpected("Failed to create build directory: " + build_dir + " (" + ec.message() + ")");
    }

    // Write build_dir validation file
    auto canonical_build_dir = std::filesystem::canonical(build_dir, ec);
    if (ec) {
        return std::unexpected("Failed to canonicalize build directory: " + build_dir);
    }
    std::ofstream meta_file(std::filesystem::path(build_dir) / ".dmake_build_path");
    if (meta_file) {
        meta_file << canonical_build_dir.string();
    }

    std::ofstream file(std::filesystem::path(build_dir) / ".dmake_cache");
    if (!file) {
        return std::unexpected("Failed to open cache file for writing: " + (std::filesystem::path(build_dir) / ".dmake_cache").string());
    }

    for (const auto& [id, sig] : cache) {
        file << id << "=" << sig << "\n";
    }

    if (!file) {
        return std::unexpected("Failed to write to cache file");
    }

    return {};
}

std::expected<std::string, std::string> BuildGraph::get_compiler_version() {
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (compiler_version_cache_) return *compiler_version_cache_;
    }

    std::array<char, 128> buffer;
    std::string result;
    FILE* pipe = popen("g++ --version 2>/dev/null | head -n 1", "r");
    if (!pipe) {
        return std::unexpected("Failed to execute g++ to get version");
    }

    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        result += buffer.data();
    }

    int status = pclose(pipe);
    if (status != 0) {
        return std::unexpected("g++ --version failed with exit code " + std::to_string(status));
    }

    if (result.empty()) {
        return std::unexpected("g++ --version produced no output");
    }

    if (result.back() == '\n') result.pop_back();

    std::lock_guard<std::mutex> lock(state_mutex_);
    compiler_version_cache_ = result;

    return *compiler_version_cache_;
}

void BuildGraph::inject_module_dependencies(
    const std::map<std::string, std::string>& module_to_task,
    const std::map<std::string, std::vector<std::string>>& task_requires) {

    auto txn = begin_locked(graph_mutation_mutex_);

    for (auto& task_ptr : tasks_) {
        // Find module requirements for this task
        auto req_it = task_requires.find(task_ptr->id);
        if (req_it == task_requires.end()) continue;

        const auto& required_modules = req_it->second;

        for (const auto& required_module : required_modules) {
            // Skip standard library modules (not built locally)
            if (required_module == "std" || required_module.starts_with("std.")) {
                continue;
            }

            auto provider_it = module_to_task.find(required_module);
            if (provider_it == module_to_task.end()) {
                continue;
            }

            auto task_it = task_by_id_.find(provider_it->second);
            if (task_it == task_by_id_.end()) continue;

            auto* provider = task_it->second;

            // Add dependency: this task depends on the provider task
            txn.dependency(task_ptr.get(), provider);
        }
    }
    // txn commits + releases lock on destruction
}

std::expected<int, std::string>
BuildGraph::attach_ep_graph(
    BuildGraph&& ep_graph, const std::string& ep_binary_dir, ExecutionState& state) {

    // Lock state.mutex for thread safety - same lock used by execute() worker threads
    std::lock_guard<std::mutex> lock(state.mutex);

    // 1. Load EP's cache for dirty computation
    auto ep_cache = load_cache(ep_binary_dir);

    // 2. Pre-compute dirty state per task (before transferring ownership)
    struct TaskDirtyInfo {
        BuildTask* raw;
        bool is_marker;
        bool is_dirty;
    };
    std::vector<TaskDirtyInfo> dirty_info;
    dirty_info.reserve(ep_graph.tasks_.size());

    for (auto& task_uptr : ep_graph.tasks_) {
        auto& task = *task_uptr;
        bool is_marker = task.is_marker_task();
        bool is_dirty = false;

        if (!is_marker) {
            bool outputs_exist = true;
            for (const auto& out : task.outputs) {
                if (!std::filesystem::exists(out)) {
                    outputs_exist = false;
                    break;
                }
            }

            if (!outputs_exist || task.always_run) {
                is_dirty = true;
            } else {
                auto sig_res = calculate_signature(task);
                if (!sig_res || !(ep_cache.count(task.id) && ep_cache[task.id] == *sig_res)) {
                    is_dirty = true;
                }
            }
        }

        task.ep_binary_dir = ep_binary_dir;
        dirty_info.push_back({task_uptr.get(), is_marker, is_dirty});
    }

    // 3. Transfer task ownership via transaction
    //    add_owned() preserves pointer stability - all internal BuildTask*
    //    edges within the EP graph remain valid.
    //    commit() rebuilds reverse edges, resolves file deps, drains pending.
    {
        auto txn = begin();
        for (auto& task_uptr : ep_graph.tasks_) {
            auto result = txn.add_owned(std::move(task_uptr));
            if (!result) return std::unexpected(result.error());
        }
        auto cr = txn.commit();
        if (!cr) return std::unexpected(cr.error());
    }
    ep_graph.tasks_.clear();
    ep_graph.task_by_id_.clear();

    // 4. Apply dirty state from pre-computed info
    int dirty_count = 0;
    for (const auto& info : dirty_info) {
        if (!info.is_marker) {
            if (info.is_dirty) {
                state.dirty_state[info.raw] = true;
                dirty_count++;
            } else {
                state.completed.insert(info.raw);
            }
        }
    }

    // 5. Check if any existing tasks gained new unsatisfied deps from cross-graph resolution
    //    (drain_pending_deps may have wired main→EP deps via pending_file_deps_)
    for (auto& task_ptr : tasks_) {
        if (!state.ready_set.count(task_ptr.get())) continue;
        for (auto* dep : task_ptr->dependencies) {
            if (!state.completed.count(dep)) {
                state.ready_set.erase(task_ptr.get());
                break;
            }
        }
    }

    // 6. Propagate dirtiness through dependencies
    if (dirty_count > 0) {
        bool changed;
        do {
            changed = false;
            for (auto& task_ptr : tasks_) {
                if (state.dirty_state.count(task_ptr.get())) continue;
                for (auto* dep : task_ptr->dependencies) {
                    auto dit = state.dirty_state.find(dep);
                    if (dit != state.dirty_state.end() && dit->second == true) {
                        state.dirty_state[task_ptr.get()] = true;
                        dirty_count++;
                        changed = true;
                        break;
                    }
                }
            }
        } while (changed);
    }

    // 7. Update progress and add ready dirty tasks to ready_set
    state.progress.bump_total(dirty_count);

    for (const auto& [ptr, ds] : state.dirty_state) {
        if (!ds.has_value() || !*ds) continue;  // Only check definitely-dirty
        if (state.completed.count(ptr)) continue;
        if (state.ready_set.count(ptr)) continue;

        bool all_deps_done = true;
        for (auto* dep : ptr->dependencies) {
            if (!state.completed.count(dep)) {
                all_deps_done = false;
                break;
            }
        }
        if (all_deps_done) {
            state.ready_set.insert(ptr);
        }
    }

    // Notify waiting threads that new tasks are available
    state.cv.notify_all();

    return dirty_count;
}

std::optional<std::string> BuildGraph::run_ep_orchestrator(
    BuildTask& task, ExecutionState& state) {

    // Get the ExternalProjectTarget from the task
    auto* ep_target = dynamic_cast<ExternalProjectTarget*>(task.parent_target);
    if (!ep_target) {
        return "EP orchestrator task has no ExternalProjectTarget";
    }

    std::string ep_name = ep_target->get_name();
    std::string sentinel_id = ep_name;  // Sentinel task ID is the EP name

    // Helper to print output lines with EP name prefix (for child interpreter output)
    auto print_prefixed_output = [&](const std::string& output) {
        if (output.empty()) return;
        std::string prefix = std::string(c(state.stdout_is_tty, colors::DIM)) + std::string(c(state.stdout_is_tty, colors::WHITE)) + "[" + ep_name + "] " + std::string(c(state.stdout_is_tty, colors::RESET));
        std::istringstream iss(output);
        std::string line;
        std::lock_guard<std::mutex> lock(output_mutex_);
        while (std::getline(iss, line)) {
            state.progress.print_line(prefix + line);
        }
    };

    // Check if this is a cmake-based EP or custom commands EP
    if (ep_target->is_cmake_based()) {
        // === CMAKE-BASED EP ===
        // Spawn an isolated interpreter to build the EP

        std::string source_dir = ep_target->get_effective_source_dir();
        std::string binary_dir = ep_target->get_ep_binary_dir();
        std::string install_dir = ep_target->get_ep_install_dir();

        // Create isolated interpreter
        std::stringstream ep_output;
        auto ep_interp = std::make_unique<Interpreter>(source_dir, &ep_output, &ep_output, binary_dir);

        // Force colors in child interpreter if parent stdout is a TTY
        if (state.stdout_is_tty) {
            ep_interp->set_force_colors(true);
        }

        // Apply CMAKE_ARGS and CMAKE_CACHE_ARGS
        for (const auto& arg : ep_target->get_cmake_args()) {
            // Parse -DVAR=value or -DVAR:TYPE=value
            if (arg.starts_with("-D")) {
                std::string def = arg.substr(2);
                size_t eq = def.find('=');
                if (eq != std::string::npos) {
                    std::string var = def.substr(0, eq);
                    std::string val = def.substr(eq + 1);
                    // Strip type annotation
                    size_t colon = var.find(':');
                    if (colon != std::string::npos) var = var.substr(0, colon);
                    ep_interp->set_variable(var, val);
                }
            }
        }
        for (const auto& arg : ep_target->get_cmake_cache_args()) {
            if (arg.starts_with("-D")) {
                std::string def = arg.substr(2);
                size_t eq = def.find('=');
                if (eq != std::string::npos) {
                    std::string var = def.substr(0, eq);
                    std::string val = def.substr(eq + 1);
                    size_t colon = var.find(':');
                    if (colon != std::string::npos) var = var.substr(0, colon);
                    ep_interp->set_variable(var, val);
                }
            }
        }

        // Set CMAKE_INSTALL_PREFIX if not already set
        if (ep_interp->get_variable("CMAKE_INSTALL_PREFIX").empty()) {
            ep_interp->set_variable("CMAKE_INSTALL_PREFIX", install_dir);
        }

        // Suppress STATUS messages in child interpreter
        ep_interp->set_variable("CMAKE_MESSAGE_LOG_LEVEL", "WARNING");

        // Read and parse CMakeLists.txt
        std::string cmake_file = source_dir + "/CMakeLists.txt";
        if (!std::filesystem::exists(cmake_file)) {
            return "EP " + ep_name + ": CMakeLists.txt not found at " + cmake_file;
        }

        std::ifstream file(cmake_file);
        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

        Parser parser(content, cmake_file);
        auto ast = parser.parse();
        if (!ast) {
            return "EP " + ep_name + ": Parse error in " + cmake_file + ": " + ast.error().reason;
        }

        ep_interp->set_current_file(cmake_file);

        // Interpret
        auto interp_result = ep_interp->interpret(*ast);
        if (!interp_result) {
            std::string output = ep_output.str();
            return "EP " + ep_name + ": Interpretation error: " + interp_result.error().message +
                   (output.empty() ? "" : "\nOutput:\n" + output);
        }

        // Apply accumulated directory properties (include_directories, add_definitions, etc.)
        // to all targets, matching what add_subdirectory/FetchContent do.
        ep_interp->finalize_directory_targets();

        // Print any buffered interpretation output (with EP name prefix)
        print_prefixed_output(ep_output.str());
        ep_output.str("");  // Clear for next phase

        // Generate full build graph (not just dirty tasks)
        // attach_ep_graph will handle dirty detection for each task
        auto graph_result = ep_interp->generate_build_graph({});
        if (!graph_result) {
            std::string output = ep_output.str();
            return "EP " + ep_name + ": Task generation error: " + graph_result.error().message +
                   (output.empty() ? "" : "\nOutput:\n" + output);
        }

        // Print any buffered task generation output
        print_prefixed_output(ep_output.str());

        // Save EP's subsystem cache (try_compile, find_*, glob results)
        auto cache_save_result = ep_interp->get_cache_store().save();
        if (!cache_save_result) {
            // Non-fatal - just warn
            print_prefixed_output("warning: Failed to save subsystem cache: " + cache_save_result.error());
        }

        // Attach full EP graph to main graph
        // This handles dirty detection and proper dependency wiring
        auto attach_result = attach_ep_graph(std::move(*graph_result), binary_dir, state);
        if (!attach_result) {
            return "EP " + ep_name + ": " + attach_result.error();
        }

        int dirty_count = *attach_result;

        // For cmake-based EPs, we interpret CMakeLists.txt so we use install rules,
        // not any custom INSTALL_COMMAND (which would require cmake-generated Makefile).
        const auto& install_rules = ep_interp->get_install_rules();
        std::string install_prefix = ep_target->get_ep_install_dir();
        std::string config = ep_interp->get_variable("CMAKE_BUILD_TYPE");

        // Helper to run extra install commands (skip "make install" which we replaced with install rules)
        auto run_extra_install_cmds = [&]() -> std::optional<std::string> {
            const auto& install_cmd = ep_target->get_install_command();
            if (install_cmd.is_empty || install_cmd.commands.empty()) return std::nullopt;
            std::string working_dir = ep_target->get_ep_binary_dir();
            for (const auto& cmd : install_cmd.commands) {
                // Skip "make install" - we handle that with our install rules
                if (is_make_install_command(cmd)) continue;
                auto result = dmake::run_command(cmd, working_dir);
                if (result.exit_code != 0) {
                    print_prefixed_output(result.output);
                    return "EP " + ep_name + " install command failed";
                }
                print_prefixed_output(result.output);
            }
            return std::nullopt;
        };

        if (dirty_count == 0) {
            // Header-only EP or all tasks up-to-date - run install immediately
            if (!install_rules.empty()) {
                print_prefixed_output("Installing...");
                auto install_result = execute_install_rules(ep_interp.get(), install_rules,
                                                            install_prefix, config);
                if (!install_result) {
                    return "EP " + ep_name + " install failed: " + install_result.error();
                }
            }
            // Run extra install commands (e.g., cp for internal headers)
            if (auto err = run_extra_install_cmds()) return *err;
            print_prefixed_output("EP is up-to-date");
        } else {
            // EP has build tasks - create install task with proper dependencies
            if (!install_rules.empty()) {
                ep_target->set_pending_install_rules(install_rules, config);

                // Compute install task inputs and pre-compute target destinations
                // (MUST be done BEFORE moving targets to ep_target_owners_)
                auto install_inputs = compute_install_inputs(install_rules, ep_interp.get(),
                                                             install_prefix, ep_target);

                // Create install task
                std::string install_id = ep_name + ":install";
                BuildTask install_task;
                install_task.id = install_id;
                install_task.kind = EPInstallTask{ep_name};
                install_task.parent_target = ep_target;
                install_task.ep_binary_dir = binary_dir;
                install_task.inputs = std::move(install_inputs);

                // Inject install task and wire sentinel to depend on it
                {
                    auto txn = begin_locked(state.mutex);
                    auto install_result = txn.add(std::move(install_task));
                    if (!install_result) return install_result.error();
                    auto* install_raw = *install_result;

                    auto* sentinel = task_by_id_[sentinel_id];
                    txn.dependency(sentinel, install_raw);
                    auto cr = txn.commit();
                    if (!cr) return cr.error();

                    // Mark install task as dirty
                    state.dirty_state[install_raw] = true;
                    state.progress.bump_total(1);

                    // Remove sentinel from ready set (has unsatisfied dependency now)
                    state.ready_set.erase(sentinel);
                }
            }
            // Extra install commands will be run by install task (it has access to ep_target)
        }

        // Keep child interpreter targets alive — injected tasks hold raw parent_target pointers
        for (auto& [_, target] : ep_interp->get_targets()) {
            ep_target_owners_.push_back(std::move(target));
        }

    } else {
        // === CUSTOM COMMANDS EP ===
        // Run CONFIGURE_COMMAND, BUILD_COMMAND, INSTALL_COMMAND sequentially

        std::string working_dir = ep_target->get_ep_binary_dir();
        std::filesystem::create_directories(working_dir);

        auto run_step = [&](const EPStepCommand& step, const char* step_name)
            -> std::optional<std::string> {
            if (step.is_empty || step.commands.empty()) return std::nullopt;
            for (const auto& cmd : step.commands) {
                auto result = dmake::run_command(cmd, working_dir);
                if (result.exit_code != 0) {
                    print_prefixed_output(result.output);
                    return "EP " + ep_name + " " + step_name + " failed";
                }
                print_prefixed_output(result.output);
            }
            return std::nullopt;
        };

        if (auto err = run_step(ep_target->get_configure_command(), "configure")) return *err;
        if (auto err = run_step(ep_target->get_build_command(), "build")) return *err;
        if (auto err = run_step(ep_target->get_install_command(), "install")) return *err;
    }

    return std::nullopt;  // Success
}

} // namespace dmake
