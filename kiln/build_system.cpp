#include "build_system.hpp"
#include "version.hpp"
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
#include "compiler.hpp"
#include "install_executor.hpp"
#include "path.hpp"
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

namespace kiln {

struct CompileCommand {
    std::string directory;
    std::string command;
    std::string file;
    std::string output;
};

// Helper: Compute install task inputs and pre-compute target install paths.
// Returns the output paths of targets being installed (so install task depends on link tasks).
// Also populates ep_target with pre-computed (artifact_path, dest_path) pairs for TARGETS rules.
static std::vector<std::string> compute_install_inputs(const std::vector<std::shared_ptr<InstallRule>>& rules, Interpreter* ep_interp,
                                                       const std::string& install_prefix, ExternalProjectTarget* ep_target) {
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
                    std::string dest_str = Path::join(Path::join(install_prefix, dest->destination), Path(artifact_path).filename());

                    PendingTargetInstall install;
                    install.artifact_path = artifact_path;
                    install.dest_path = dest_str;
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
// when using kiln's install rules instead of invoking make.
// Handles: "make install", "make -j4 install", "$(MAKE) install", etc.
static bool is_make_install_command(const std::vector<std::string>& cmd) {
    if (cmd.empty()) return false;

    // Check if the command is "make" or ends with "/make" or is "$(MAKE)"
    const std::string& exe = cmd[0];
    bool is_make = (exe == "make" || exe == "$(MAKE)" || exe.ends_with("/make") || exe.find("make") != std::string::npos);
    if (!is_make) return false;

    // A bare "make" in install context is likely "make install" with default target
    if (cmd.size() == 1) return true;

    // Check if any argument is "install"
    for (size_t i = 1; i < cmd.size(); ++i) {
        if (cmd[i] == "install") return true;
    }

    return false;
}

// Helper: Check if a command is a "cmake --install" invocation that should be
// skipped when kiln handles install rules itself. Handles wrappers like
// "cmake -E env ... cmake --install . --config Release".
// Kiln does not generate cmake_install.cmake, so running cmake --install
// against an EP build dir would fail.
static bool is_cmake_install_command(const std::vector<std::string>& cmd) {
    for (const auto& tok : cmd) {
        if (tok == "--install") return true;
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

    // If non-null, command stdout (lines that would otherwise be printed via
    // progress.print_line) is appended here instead. Used by try_compile to
    // route the sub-build's command output back into OUTPUT_VARIABLE.
    std::string* output_capture = nullptr;

    ExecutionState(std::string build_dir_, bool is_tty, int task_count, std::map<std::string, std::string> loaded_cache)
        : build_dir(std::move(build_dir_)), stdout_is_tty(is_tty), progress(task_count, is_tty), cache(std::move(loaded_cache)),
          new_cache(cache) {}
};

// Promote a task into the ready set if it's a tracked dirty task that isn't
// already running/queued/completed and all its dependencies have completed.
// Caller must hold state.mutex.
static bool try_promote_to_ready(BuildTask* t, ExecutionState& state) {
    if (!state.dirty_state.count(t)) return false;
    if (state.completed.count(t)) return false;
    if (state.running.count(t)) return false;
    if (state.ready_set.count(t)) return false;
    for (auto* dep : t->dependencies) {
        if (!state.completed.count(dep)) return false;
    }
    state.ready_set.insert(t);
    return true;
}

// Short tag for a BuildTask::kind variant — used in stall diagnostics.
static const char* task_kind_name(const BuildTask& t) {
    return std::visit(overloaded{
                          [](const CompileTask&) { return "compile"; },
                          [](const PCHTask&) { return "pch"; },
                          [](const LinkTask&) { return "link"; },
                          [](const CustomCommandTask&) { return "custom_command"; },
                          [](const CustomTargetTask&) { return "custom_target"; },
                          [](const PreBuildTask&) { return "pre_build"; },
                          [](const PostBuildTask&) { return "post_build"; },
                          [](const ModuleScannerTask&) { return "module_scan"; },
                          [](const ModuleCollatorTask&) { return "module_collate"; },
                          [](const EPOrchestratorTask&) { return "ep_orchestrate"; },
                          [](const EPSentinelTask&) { return "ep_sentinel"; },
                          [](const EPInstallTask&) { return "ep_install"; },
                          [](const MocTask&) { return "moc"; },
                          [](const UicTask&) { return "uic"; },
                          [](const RccTask&) { return "rcc"; },
                      },
                      t.kind);
}

// Which scheduler bucket a task is currently in. "limbo" means the task
// belongs to the graph but isn't tracked anywhere — that's almost always a
// missed try_promote_to_ready or a forgotten state.completed insertion, and
// is the shape of bug this diagnostic exists to surface.
// Caller must hold state.mutex.
static const char* task_bucket(BuildTask* t, const ExecutionState& state) {
    if (state.completed.count(t)) return "completed";
    if (state.running.count(t)) return "running";
    if (state.ready_set.count(t)) return "ready";
    auto it = state.dirty_state.find(t);
    if (it == state.dirty_state.end()) return "limbo";
    if (!it->second.has_value()) return "maybe-dirty";
    return *it->second ? "dirty" : "clean";
}

// Build a diagnostic for a stalled build graph: every uncompleted task that
// the scheduler is supposed to have touched, with its kind, bucket, and the
// uncompleted deps it's waiting on. Walks the full task list (not just
// dirty_state) so a "limbo" task shows up as its own entry, not merely as a
// dangling name on someone else's "waiting on" line.
// Caller must hold state.mutex.
static std::string format_stall_diagnostic(const std::vector<std::unique_ptr<BuildTask>>& tasks, const ExecutionState& state) {
    // A task is worth dumping if it's in dirty_state, or if some uncompleted
    // dirty task lists it as a dep. Anything else is a clean task the
    // scheduler correctly skipped, and dumping those would bury the signal.
    std::unordered_set<BuildTask*> referenced_as_dep;
    for (const auto& [other, _] : state.dirty_state) {
        if (state.completed.count(other)) continue;
        for (auto* d : other->dependencies) referenced_as_dep.insert(d);
    }

    std::ostringstream oss;
    oss << "Build graph stalled. Uncompleted tasks (bucket, kind):";
    for (const auto& task_ptr : tasks) {
        BuildTask* t = task_ptr.get();
        if (state.completed.count(t)) continue;
        const bool tracked = state.dirty_state.count(t) || referenced_as_dep.count(t);
        if (!tracked) continue;

        oss << "\n  - [" << task_bucket(t, state) << "] (" << task_kind_name(*t) << ") " << t->id;

        bool any_unmet = false;
        for (auto* dep : t->dependencies) {
            if (state.completed.count(dep)) continue;
            if (!any_unmet) {
                oss << "\n      waiting on:";
                any_unmet = true;
            }
            oss << "\n        [" << task_bucket(dep, state) << "] (" << task_kind_name(*dep) << ") " << dep->id;
        }
    }
    return oss.str();
}

std::expected<void, std::string> BuildGraph::generate_compile_commands(const std::string& build_dir) {
    std::string current_dir = std::filesystem::current_path().string();

    auto commands = filter_map(
        tasks_, [](const auto& ptr) { return ptr->is_compilation() && !ptr->commands.empty(); },
        [&](const auto& ptr) -> CompileCommand {
            const auto& task = *ptr;
            return {.directory = current_dir,
                    .command = join_command(task.commands[0]),
                    .file = std::string(task.get_source_file()),
                    .output = task.outputs.empty() ? "" : task.outputs[0]};
        });

    std::string json;
    if (auto ec = glz::write_json(commands, json)) { return std::unexpected<std::string>(glz::format_error(ec)); }

    std::string path = Path::join(build_dir, "compile_commands.json");
    std::ofstream file(path);
    if (file) {
        file << json;
    } else {
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
        if (task_lang) { task_ctx.compile_language = task_lang; }
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
                    return std::unexpected("Generator expression error in task '" + id + "' (target '" + target_name
                                           + "')\n"
                                             "  Argument: '"
                                           + arg
                                           + "'\n"
                                             "  Error: "
                                           + eval.error());
                }
                if (!eval->empty()) {
                    for (auto sv : CMakeArrayIterator(*eval)) { evaluated_cmd.emplace_back(sv); }
                }
            }
            if (!evaluated_cmd.empty()) { evaluated_commands.push_back(std::move(evaluated_cmd)); }
        }
        task.commands = std::move(evaluated_commands);

        // Hard gate: no unevaluated genex may survive into shell commands
        for (const auto& cmd : task.commands) {
            for (const auto& arg : cmd) { assert_no_genex(arg, "task command for " + id); }
        }

        // Infer dependencies from command arguments that reference target outputs.
        // Skip module scanners: their argv legitimately contains the obj path
        // (as P1689's primary-output identifier when wrapped in clang-scan-deps,
        // or as -fdeps-target on GCC) but the scanner does not actually depend
        // on the compile -- it produces the DDI the compile *consumes*. Treating
        // the obj-path argv mention as an input creates a scanner->compile edge,
        // which combined with collator->scanner gives a 3-cycle.
        const bool is_scanner = std::holds_alternative<ModuleScannerTask>(task.kind);
        if (!is_scanner) {
            for (const auto& cmd : task.commands) {
                for (const auto& arg : cmd) {
                    if (arg.empty() || arg[0] != '/') continue;
                    auto it = output_to_task_.find(arg);
                    if (it != output_to_task_.end() && it->second != task_ptr.get()) { task.inputs.push_back(arg); }
                }
            }
        }

        // Evaluate inputs (dependency paths) that contain genex
        {
            std::vector<std::string> new_inputs;
            for (auto& input : task.inputs) {
                if (!GenexParser::contains_genex(input)) {
                    new_inputs.push_back(std::move(input));
                    continue;
                }
                auto eval = evaluator.evaluate(input);
                if (!eval || eval->empty()) {
                    continue; // Drop unevaluable dependency
                }
                // Result may be a semicolon-separated CMake list — split into individual inputs
                for (auto sv : CMakeArrayIterator(*eval)) {
                    if (sv.empty()) continue;
                    std::string item(sv);
                    // Check if the evaluated result is a target name → resolve to output or explicit dep
                    if (ctx.all_targets) {
                        auto it = ctx.all_targets->find(item);
                        if (it == ctx.all_targets->end() && ctx.target_aliases) {
                            auto alias_it = ctx.target_aliases->find(item);
                            if (alias_it != ctx.target_aliases->end()) { it = ctx.all_targets->find(alias_it->second); }
                        }
                        if (it != ctx.all_targets->end()) {
                            std::string out = it->second->get_output_path();
                            if (!out.empty()) {
                                new_inputs.push_back(std::move(out));
                            } else {
                                // Custom target with no output — add as explicit dep for task-level dependency
                                task.explicit_deps.push_back(item);
                            }
                            continue;
                        }
                    }
                    new_inputs.push_back(std::move(item));
                }
            }
            task.inputs = std::move(new_inputs);
        }

        // Evaluate explicit_deps that contain genex (they reference output paths)
        for (auto& dep : task.explicit_deps) {
            if (!GenexParser::contains_genex(dep)) continue;
            auto eval = evaluator.evaluate(dep);
            if (eval && !eval->empty()) { dep = *eval; }
        }

        // Hard gate: no unevaluated genex in inputs
        for (const auto& input : task.inputs) { assert_no_genex(input, "task input for " + id); }

        // Evaluate working_dir if it contains genex
        if (!task.working_dir.empty() && GenexParser::contains_genex(task.working_dir)) {
            auto result = evaluator.evaluate(task.working_dir);
            if (!result) { return std::unexpected("Generator expression error in working_dir for task '" + id + "': " + result.error()); }
            task.working_dir = *result;
        }
        if (!task.working_dir.empty()) { assert_no_genex(task.working_dir, "working_dir for " + id); }

        // Evaluate outputs that contain genex and update output_to_task_ index
        for (auto& output : task.outputs) {
            if (!GenexParser::contains_genex(output)) continue;
            // Remove old key from index
            output_to_task_.erase(output);
            auto eval = evaluator.evaluate(output);
            if (eval && !eval->empty()) { output = *eval; }
            // Re-register with evaluated path
            output_to_task_[output] = task_ptr.get();
        }

        // Also evaluate the task ID if it contains genex (custom command task IDs are output paths)
        if (GenexParser::contains_genex(task.id)) {
            task_by_id_.erase(id);
            auto eval = evaluator.evaluate(task.id);
            if (eval && !eval->empty()) { task.id = *eval; }
            task_by_id_[task.id] = task_ptr.get();
        }
    }

    return {};
}

void BuildGraph::resolve_inferred_file_deps() {
    // After genex evaluation, tasks may have gained new inputs and explicit_deps.
    // Resolve both explicit deps (from genex-evaluated target names) and file deps.
    std::vector<BuildTask*> all;
    all.reserve(tasks_.size());
    for (auto& t : tasks_) all.push_back(t.get());
    resolve_explicit_deps(all);
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
            for (auto* dep : cur->dependencies) { bfs_stack.push_back(dep); }
        }
    }

    if (!all_custom_tasks.empty()) {
        for (auto& task_ptr : tasks_) {
            if (!task_ptr->is_compilation() || excluded_tasks.count(task_ptr.get())) continue;
            std::unordered_set<BuildTask*> dep_set(task_ptr->dependencies.begin(), task_ptr->dependencies.end());
            for (auto* ct : all_custom_tasks) {
                if (dep_set.insert(ct).second) { add_dependency(task_ptr.get(), ct); }
            }
        }
    }
}

std::expected<void, std::string> BuildGraph::validate() {
    // Cycle check
    auto cycle_err = check_for_cycles();
    if (cycle_err) return std::unexpected(*cycle_err);

    // Hard gate: nothing in the graph may carry unevaluated generator expressions
    // by the time we hand it to the executor. evaluate_genex() asserts inline as it
    // walks tasks, but tasks injected after that point (e.g. EP graphs attached via
    // attach_ep_graph, or builtins that synthesize tasks late) bypass that pass.
    // This scan catches them before any shell command runs.
    auto check_genex = [](std::string_view value, std::string_view ctx) -> std::optional<std::string> {
        if (value.find("$<") != std::string_view::npos) {
            return std::string("Unevaluated generator expression in ") + std::string(ctx) + ": '" + std::string(value) + "'";
        }
        return std::nullopt;
    };
    for (const auto& task_ptr : tasks_) {
        const auto& id = task_ptr->id;
        if (auto e = check_genex(id, "task id")) return std::unexpected(*e);
        if (auto e = check_genex(task_ptr->working_dir, "working_dir for " + id)) return std::unexpected(*e);
        for (const auto& cmd : task_ptr->commands)
            for (const auto& arg : cmd)
                if (auto e = check_genex(arg, "command for " + id)) return std::unexpected(*e);
        for (const auto& in : task_ptr->inputs)
            if (auto e = check_genex(in, "input for " + id)) return std::unexpected(*e);
        for (const auto& out : task_ptr->outputs)
            if (auto e = check_genex(out, "output for " + id)) return std::unexpected(*e);
        for (const auto& dep : task_ptr->explicit_deps)
            if (auto e = check_genex(dep, "explicit_dep for " + id)) return std::unexpected(*e);
    }

    // Check for missing inputs.
    // Compile tasks with missing source files are errors (the source won't appear).
    // Other missing inputs are warnings (resource files, etc.).
    std::map<std::string, std::vector<std::string>> missing_sources_by_target; // target name -> source files

    for (const auto& task_ptr : tasks_) {
        for (const auto& in : task_ptr->inputs) {
            if (output_to_task_.count(in) || std::filesystem::exists(in)) continue;

            if (task_ptr->is_compilation() && task_ptr->parent_target) {
                // Source file for a compile task — this is a hard error
                missing_sources_by_target[task_ptr->parent_target->get_name()].push_back(in);
            } else {
                kiln::print_message(std::cerr, "WARNING",
                                    "Task '" + task_ptr->id + "' references '" + in
                                        + "' which doesn't exist and isn't produced by any task");
            }
        }
    }

    if (!missing_sources_by_target.empty()) {
        std::string err;
        for (const auto& [target_name, sources] : missing_sources_by_target) {
            err += "Target '" + target_name + "' has source files that do not exist:\n";
            for (const auto& src : sources) { err += "    " + src + "\n"; }
        }
        err += "Remove them from the target's source list, or ensure they are generated.";
        return std::unexpected(err);
    }

    return {};
}

void BuildGraph::add_dependency(BuildTask* from, BuildTask* to) {
    from->dependencies.push_back(to);
    dependents_[to].push_back(from);
}

std::span<BuildTask* const> BuildGraph::get_dependents(BuildTask* task) const {
    auto it = dependents_.find(task);
    if (it != dependents_.end()) { return it->second; }
    static const std::vector<BuildTask*> empty;
    return empty;
}

std::expected<BuildTask*, std::string> BuildGraph::add_task_internal(std::unique_ptr<BuildTask> task) {
    auto* raw = task.get();
    if (task_by_id_.count(raw->id)) { return std::unexpected("Task ID already exists: " + raw->id); }
    task_by_id_[raw->id] = raw;
    for (const auto& out : raw->outputs) { output_to_task_[out] = raw; }
    tasks_.push_back(std::move(task));
    return raw;
}

void BuildGraph::resolve_explicit_deps(std::span<BuildTask*> batch) {
    for (auto* task : batch) {
        std::vector<std::string> unresolved;
        for (const auto& dep_id : task->explicit_deps) {
            auto it = task_by_id_.find(dep_id);
            if (it != task_by_id_.end()) {
                add_dependency(task, it->second);
            } else {
                unresolved.push_back(dep_id);
            }
        }
        task->explicit_deps = std::move(unresolved);
        // Deduplicate
        std::sort(task->dependencies.begin(), task->dependencies.end());
        task->dependencies.erase(std::unique(task->dependencies.begin(), task->dependencies.end()), task->dependencies.end());
    }
}

void BuildGraph::resolve_file_deps(std::span<BuildTask*> batch) {
    for (auto* task : batch) {
        std::unordered_set<BuildTask*> dep_set(task->dependencies.begin(), task->dependencies.end());
        for (const auto& in : task->inputs) {
            auto it = output_to_task_.find(in);
            if (it != output_to_task_.end() && it->second != task) {
                if (dep_set.insert(it->second).second) { add_dependency(task, it->second); }
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
                    auto& deps = it->second->dependencies;
                    // Linear search is acceptable here — pending deps are rare
                    if (std::find(deps.begin(), deps.end(), task) == deps.end()) { add_dependency(it->second, task); }
                }
            }
            pending_file_deps_.erase(out);
        }
    }

    // Register any still-unresolved inputs from batch tasks as pending
    for (auto* task : batch) {
        for (const auto& in : task->inputs) {
            if (!output_to_task_.count(in)) { pending_file_deps_.emplace(in, task); }
        }
    }
}

// --- GraphTransaction implementation ---

std::expected<BuildTask*, std::string> GraphTransaction::add(BuildTask task) {
    auto ptr = std::make_unique<BuildTask>(std::move(task));
    auto result = graph_.add_task_internal(std::move(ptr));
    if (result) { batch_.push_back(*result); }
    return result;
}

std::expected<BuildTask*, std::string> GraphTransaction::add_owned(std::unique_ptr<BuildTask> task) {
    auto result = graph_.add_task_internal(std::move(task));
    if (result) { batch_.push_back(*result); }
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
        for (auto* dep : task->dependencies) { graph_.dependents_[dep].push_back(task); }
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

bool BuildGraph::all_outputs_exist(const BuildTask& task) {
    if (task.outputs.empty()) return false;
    for (const auto& out : task.outputs) {
        if (!get_file_time_if_exists(out)) return false;
    }
    return true;
}

std::optional<std::string> BuildGraph::clean_signature(const BuildTask& task, const std::map<std::string, std::string>& cache) {
    if (task.always_run) return std::nullopt;
    if (!all_outputs_exist(task)) return std::nullopt;
    auto sig_res = calculate_signature(task);
    if (!sig_res) return std::nullopt;
    auto it = cache.find(task.id);
    if (it == cache.end() || it->second != *sig_res) return std::nullopt;
    return *sig_res;
}

void BuildGraph::propagate_dirty_bfs(std::unordered_map<BuildTask*, std::optional<bool>>& dirty_state) {
    if (dirty_state.empty()) return;
    std::vector<BuildTask*> worklist;
    worklist.reserve(dirty_state.size());
    for (auto& [ptr, _] : dirty_state) worklist.push_back(ptr);

    for (size_t i = 0; i < worklist.size(); ++i) {
        auto* t = worklist[i];
        // always_run tasks (add_custom_target ALL, pre/post-build) execute
        // every build, but matching CMake/Ninja their edges to dependents are
        // order-only: dependents must wait, not auto-rebuild. Propagate as
        // maybe-dirty so each dependent gets a runtime sig recheck — a stable
        // byproduct (e.g. copy_if_different version.cpp) won't cascade.
        bool definite = (dirty_state[t] == true) && !t->always_run;
        for (auto* dependent : get_dependents(t)) {
            auto [it, inserted] = dirty_state.try_emplace(dependent, definite ? std::optional<bool>(true) : std::nullopt);
            if (inserted) {
                worklist.push_back(dependent);
            } else if (definite && !it->second.has_value()) {
                // Upgrade existing maybe → definite. No re-enqueue needed:
                // the entry was already walked (or will be) under its prior
                // state, and definite-ness doesn't add new dependents.
                it->second = true;
            }
        }
    }
}

void BuildGraph::report_command_failure(ExecutionState& state, std::string_view captured_output) {
    std::lock_guard<std::mutex> lock(output_mutex_);
    state.progress.erase();
    std::cout.flush(); // erase wrote to cout; flush before cerr
    if (!captured_output.empty())
        std::cerr << "[t=" << std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count() << "] "
                  << captured_output << std::endl;
}

std::expected<void, std::string> BuildGraph::execute(const std::string& build_dir, int jobs, std::string* output_capture) {
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

        // POST_BUILD tasks have no outputs but should only run when their
        // dependency (the link task) actually rebuilds. Skip direct marking —
        // they'll become dirty via propagation from the link task if needed.
        if (std::holds_alternative<PostBuildTask>(task.kind) && !all_outputs_exist(task)) { continue; }

        if (clean_signature(task, cache)) continue;

        // EP sentinels and orchestrators are "maybe" — we can't know if EP
        // will inject tasks. Their dependents are re-checked at runtime.
        dirty_state[task_ptr.get()] = task.is_ep_task() ? std::nullopt : std::optional<bool>(true);
    }

    propagate_dirty_bfs(dirty_state);

    // Count definitely dirty tasks for progress bar
    int dirty_task_count = 0;
    int maybe_dirty_count = 0;
    for (const auto& [ptr, ds] : dirty_state) {
        if (ds == true)
            dirty_task_count++;
        else
            maybe_dirty_count++;
    }
    pre_scan_profile.stop();

    bool stdout_is_tty = isatty(STDOUT_FILENO);

    // 3. Parallel execution with fixed worker threads
    // Bundle all execution-phase state into a single struct.
    ExecutionState state(build_dir, stdout_is_tty, dirty_task_count + maybe_dirty_count, std::move(cache));
    state.dirty_state = std::move(dirty_state);
    state.output_capture = output_capture;

    // Pre-populate completed with clean tasks (not in dirty_state).
    //
    // Marker tasks (no outputs, no commands — pure synchronization points)
    // have nothing to execute, so they unconditionally count as completed.
    // Without this, a marker whose deps include a dirty task gets caught
    // by the has_dirty_dep guard below and is left in limbo: not in
    // dirty_state, not in completed, never executed — and any dependent
    // waits for it forever, stalling the graph.
    for (const auto& task_ptr : tasks_) {
        if (state.dirty_state.count(task_ptr.get())) continue;
        if (task_ptr->is_marker_task()) {
            state.completed.insert(task_ptr.get());
            continue;
        }
        bool has_dirty_dep = false;
        for (auto* dep : task_ptr->dependencies) {
            if (state.dirty_state.count(dep)) {
                has_dirty_dep = true;
                break;
            }
        }
        if (!has_dirty_dep) { state.completed.insert(task_ptr.get()); }
    }

    if (jobs <= 0) {
        jobs = std::thread::hardware_concurrency();
        if (jobs <= 0) jobs = 2;
    }

    // Initialize ready_set with dirty/maybe tasks whose deps are all complete.
    for (const auto& [ptr, _] : state.dirty_state) { try_promote_to_ready(ptr, state); }

    auto start_time = std::chrono::steady_clock::now();

    std::vector<std::thread> workers;
    workers.reserve(jobs);

    for (int w = 0; w < jobs; w++) {
        workers.emplace_back([this, &state]() {
            bool profiling = g_profiling_enabled.load(std::memory_order_relaxed);

            while (true) {
                BuildTask* current = nullptr;
                bool current_is_maybe_dirty = false;

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

                    if (!state.fatal_error.empty()) { return; }
                    // Check termination: all tasks are either completed or not in dirty_state
                    if (state.ready_set.empty() && state.running.empty()) {
                        // Check if we're truly done
                        bool all_done = true;
                        for (const auto& [ptr, ds] : state.dirty_state) {
                            if (!state.completed.count(ptr)) {
                                all_done = false;
                                break;
                            }
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
                        // dirty_state is shared and mutated under state.mutex
                        // (run_ep_orchestrator injects install tasks while
                        // workers schedule). Read it here, while we still
                        // hold the lock, so the post-unlock work below never
                        // touches the map directly.
                        auto it2 = state.dirty_state.find(current);
                        current_is_maybe_dirty = it2 != state.dirty_state.end() && !it2->second.has_value();
                    } else if (state.running.empty()) {
                        // Stall: nothing ready, nothing running, but
                        // uncompleted tasks remain. Hand the diagnostic off
                        // to format_stall_diagnostic so the message tells the
                        // reader which bucket each stuck task is in (limbo
                        // entries are usually the proximate bug).
                        state.fatal_error = format_stall_diagnostic(tasks_, state);
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
                std::string active_display_name; // tracks name in progress bar's active list
                auto task_start = std::chrono::steady_clock::now();

                do { // do-while(false) for break-on-error
                    // "Maybe" dirty (EP sentinel/orchestrator dependents):
                    // re-check signature now that deps are complete; if clean,
                    // skip execution.
                    if (current_is_maybe_dirty) {
                        if (auto clean_sig = clean_signature(task, state.cache)) {
                            state.progress.bump_total(-1);
                            sig = std::move(*clean_sig);
                            break; // Skip to completion handling
                        }
                    }

                    int64_t profile_start = 0;
                    std::string profile_name;
                    if (profiling) {
                        profile_start = Profiler::instance().now_us();
                        std::string artifact = task.parent_target ? task.parent_target->get_name() : "";
                        profile_name = std::visit(
                            overloaded{[&](const ModuleCollatorTask&) { return "collate " + artifact; },
                                       [&](const ModuleScannerTask& t) { return "scan " + std::string(Path(t.source_file).filename()); },
                                       [&](const CompileTask& t) { return "compile " + std::string(Path(t.source_file).filename()); },
                                       [&](const PCHTask& t) { return "compile " + std::string(Path(t.source_file).filename()); },
                                       [&](const LinkTask&) { return "link " + artifact; },
                                       [&](const MocTask& t) { return "moc " + std::string(Path(t.source_file).filename()); },
                                       [&](const UicTask& t) { return "uic " + std::string(Path(t.source_file).filename()); },
                                       [&](const RccTask& t) { return "rcc " + std::string(Path(t.source_file).filename()); },
                                       [&](const auto&) { return "run " + std::string(Path(id).filename()); }},
                            task.kind);
                    }

                    std::string artifact_name = task.parent_target ? task.parent_target->get_name() : "unknown";

                    // Pre-create output directories and working directory
                    {
                        std::error_code ec;
                        if (!task.working_dir.empty()) { std::filesystem::create_directories(task.working_dir, ec); }
                        for (const auto& out : task.outputs) {
                            std::filesystem::create_directories(std::string(Path(out).parent_path()), ec);
                            if (ec) {
                                task_error = "Failed to create directory for " + out + ": " + ec.message();
                                break;
                            }
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
                        // Dim the [artifact] bracket on TTY so the filename becomes the per-line headline
                        // and the target context recedes to secondary status.
                        std::string detail;
                        detail.append(color(kiln::colors::DIM));
                        detail.append("[").append(artifact_name).append("]");
                        detail.append(color(kiln::colors::RESET));
                        detail.append(" ").append(target_display);
                        if (state.stdout_is_tty) {
                            oss << kiln::format_action(verb, detail, true);
                        } else {
                            int tot = state.progress.total();
                            int width = static_cast<int>(std::to_string(tot).size());
                            oss << "   [" << std::setw(width) << done << "/" << tot << "] " << color(kiln::colors::BOLD_GREEN) << verb
                                << color(kiln::colors::RESET) << ' ' << detail;
                        }

                        std::lock_guard<std::mutex> lock(output_mutex_);
                        if (state.output_capture) {
                            state.output_capture->append(oss.str());
                            state.output_capture->push_back('\n');
                        } else {
                            state.progress.print_line(oss.str());
                        }
                    };

                    // Dispatch based on task kind
                    std::visit(
                        overloaded{[&](const EPOrchestratorTask& ep) {
                                       print_status("Configuring", ep.ep_name);

                                       // Run the EP orchestrator outside the lock - it acquires state.mutex when attaching the graph
                                       auto ep_result = run_ep_orchestrator(task, state);
                                       if (ep_result) { task_error = *ep_result; }
                                   },
                                   [&](const EPInstallTask& ep) {
                                       // EP install task - runs install rules after EP build tasks complete
                                       print_status("Installing", ep.ep_name);
                                       // Bar-aware sink: every install line goes through the bar's
                                       // atomic erase+print+redraw so it can't tear against concurrent
                                       // build output.
                                       kiln::OutputCtx ep_out{[this, &state](std::string_view line) {
                                                                  std::lock_guard<std::mutex> lock(output_mutex_);
                                                                  state.progress.print_line(std::string(line));
                                                              },
                                                              state.stdout_is_tty};
                                       auto* ep_target = dynamic_cast<ExternalProjectTarget*>(task.parent_target);
                                       if (ep_target) {
                                           // 1. Install pre-computed target artifacts (TARGETS rules)
                                           for (const auto& install : ep_target->get_pending_target_installs()) {
                                               std::string dest_dir(Path(install.dest_path).parent_path());
                                               std::filesystem::create_directories(dest_dir);
                                               std::error_code ec;
                                               std::filesystem::copy_file(install.artifact_path, install.dest_path,
                                                                          std::filesystem::copy_options::overwrite_existing, ec);
                                               if (ec) {
                                                   task_error = "EP " + ep.ep_name + " install failed: cannot copy " + install.artifact_path
                                                                + " to " + install.dest_path + ": " + ec.message();
                                                   return;
                                               }
                                               kiln::print_action(ep_out, "Installing", install.dest_path);
                                           }
                                           if (!task_error.empty()) return;

                                           // 2. Run other install rules (FILES, DIRECTORY, SCRIPT, CODE)
                                           // TARGETS rules are skipped since interp is null (we handled them above)
                                           const auto& install_rules = ep_target->get_pending_install_rules();
                                           if (!install_rules.empty()) {
                                               // Mirror compute_install_inputs path: prefer the EP's
                                               // CMAKE_INSTALL_PREFIX (set via CMAKE_ARGS) over the
                                               // ExternalProject INSTALL_DIR default.
                                               std::string install_prefix;
                                               if (auto* ep_interp_for_prefix = ep_target->get_ep_interpreter())
                                                   install_prefix = ep_interp_for_prefix->get_variable("CMAKE_INSTALL_PREFIX");
                                               if (install_prefix.empty()) install_prefix = ep_target->get_ep_install_dir();
                                               std::string config = ep_target->get_pending_install_config();
                                               auto plan_or = build_install_plan(ep_target->get_ep_interpreter(), install_rules,
                                                                                 install_prefix, config, ep_out);
                                               if (!plan_or) {
                                                   task_error = "EP " + ep.ep_name + " install plan failed: " + plan_or.error();
                                                   return;
                                               }
                                               auto install_result = execute_install_plan(*plan_or, install_prefix, config,
                                                                                          /*component_filter=*/"", ep_out);
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
                                                   // Skip "make install" / "cmake --install" - we handle install rules ourselves
                                                   if (is_make_install_command(cmd) || is_cmake_install_command(cmd)) continue;
                                                   auto result = kiln::run_command(cmd, working_dir);
                                                   if (result.exit_code != 0) {
                                                       report_command_failure(state, result.output);
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
                                   [&](const ModuleCollatorTask& collator) {
                                       // C++20 modules: handle collator tasks specially (in-process execution).
                                       //
                                       // Inputs to this task are a mix of:
                                       //   - per-source DDI files (P1689 JSON, ".ddi" suffix) from this
                                       //     target's own scanners, and
                                       //   - foreign module-export manifests (".module-exports.json")
                                       //     from transitive PUBLIC/INTERFACE link deps that provide
                                       //     modules. The graph guarantees those manifests are written
                                       //     before this task runs.
                                       print_status("Collating", "modules");

                                       std::map<std::string, std::string> module_to_task;
                                       std::map<std::string, std::vector<std::string>> task_requires;
                                       std::vector<ModuleMapEntry> mapper_entries;
                                       // Track who first claimed each logical name, so a conflict
                                       // diagnostic can name both source paths.
                                       std::map<std::string, std::string> module_to_source;
                                       // Local provides (for the manifest we'll write).
                                       ModuleManifest own_manifest;

                                       // Header-unit imports surfaced by the scanner. Keyed by
                                       // resolved header path so the same header imported with
                                       // both `<h>` and `"h"` collapses to a single BMI.
                                       using HeaderUnitNeeded = BuildGraph::HeaderUnitInfo;
                                       std::map<std::string, HeaderUnitNeeded> header_units;
                                       // Per-importer-task list of header-unit source paths that
                                       // task imports. Used to wire compile-task → header-unit
                                       // dependencies once header-unit tasks exist.
                                       std::map<std::string, std::vector<std::string>> task_header_units;

                                       auto record_provider = [&](const std::string& logical_name, const std::string& obj_path,
                                                                  const std::string& bmi_path, const std::string& source_path) -> bool {
                                           auto existing = module_to_task.find(logical_name);
                                           if (existing != module_to_task.end()) {
                                               task_error = "Module '" + logical_name + "' is provided by two sources: '"
                                                            + module_to_source[logical_name] + "' and '" + source_path + "'";
                                               return false;
                                           }
                                           module_to_task[logical_name] = obj_path;
                                           module_to_source[logical_name] = source_path;
                                           ModuleMapEntry entry;
                                           entry.module_name = logical_name;
                                           entry.bmi_path = bmi_path;
                                           entry.source_path = source_path;
                                           entry.object_task_id = obj_path;
                                           mapper_entries.push_back(std::move(entry));
                                           return true;
                                       };

                                       // Every local module-bearing TU's obj path (= rule.primary_output).
                                       // Used below to emit per-task clang response files: every TU that
                                       // imports/exports a module needs an rsp file at <obj>.modules.rsp,
                                       // even one that doesn't import anything (still needs -fmodule-output=).
                                       std::vector<std::string> local_module_objs;
                                       for (const auto& input_path : task.inputs) {
                                           if (input_path.size() >= 20
                                               && input_path.compare(input_path.size() - 20, 20, ".module-exports.json") == 0) {
                                               // Foreign manifest: producer collator wrote this.
                                               auto man = read_module_manifest(input_path);
                                               if (!man) {
                                                   task_error = man.error();
                                                   return;
                                               }
                                               for (const auto& e : man->entries) {
                                                   // Defense in depth — producer should have already filtered.
                                                   if (e.visibility != "PUBLIC" && e.visibility != "INTERFACE") continue;
                                                   if (!record_provider(e.logical_name, e.primary_output, e.bmi_path, e.source_path))
                                                       return;
                                               }
                                               continue;
                                           }

                                           // Local DDI (P1689 JSON).
                                           auto p1689 = parse_p1689_file(input_path);
                                           if (!p1689) {
                                               task_error = p1689.error();
                                               return;
                                           }
                                           if (p1689->rules.empty()) {
                                               task_error = "P1689 file has no rules: " + input_path;
                                               return;
                                           }
                                           if (p1689->rules.size() > 1) {
                                               task_error = "P1689 file has more than one rule: " + input_path;
                                               return;
                                           }
                                           const auto& rule = p1689->rules[0];

                                           // primary-output is what we passed via -fdeps-target;
                                           // it equals get_obj_path(...) at scanner-task generation
                                           // time, so it's the obj task id directly.
                                           const std::string& obj_path = rule.primary_output;
                                           local_module_objs.push_back(obj_path);

                                           if (rule.provides.size() > 1) {
                                               task_error = "P1689 rule provides more than one module: " + input_path;
                                               return;
                                           }
                                           if (!rule.provides.empty()) {
                                               const auto& prov = rule.provides[0];
                                               std::string bmi = get_bmi_path(task.parent_target->get_binary_dir(), prov.logical_name);
                                               // GCC's P1689 doesn't populate provides[].source-path,
                                               // so recover it from the upstream scanner task that
                                               // produced this DDI.
                                               std::string src = prov.source_path.value_or("");
                                               if (src.empty()) {
                                                   auto scanner_it = task_by_id_.find(input_path);
                                                   if (scanner_it != task_by_id_.end()) {
                                                       if (auto* st = std::get_if<ModuleScannerTask>(&scanner_it->second->kind)) {
                                                           src = st->source_file;
                                                       }
                                                   }
                                               }
                                               if (!record_provider(prov.logical_name, obj_path, bmi, src)) return;

                                               // This module is exported iff its source lives in a
                                               // PUBLIC or INTERFACE cxx_modules file set. Otherwise
                                               // it stays internal to this target.
                                               auto vis = task.parent_target->file_set_visibility_for_source(src);
                                               if (vis && (*vis == PropertyVisibility::PUBLIC || *vis == PropertyVisibility::INTERFACE)) {
                                                   ModuleManifestEntry me;
                                                   me.logical_name = prov.logical_name;
                                                   me.bmi_path = bmi;
                                                   me.primary_output = obj_path;
                                                   me.source_path = src;
                                                   me.visibility = (*vis == PropertyVisibility::PUBLIC ? "PUBLIC" : "INTERFACE");
                                                   own_manifest.entries.push_back(std::move(me));
                                               }
                                           }

                                           std::vector<std::string> requires_names;
                                           for (const auto& req : rule.requires_) {
                                               if (req.lookup_method == "by-name") {
                                                   requires_names.push_back(req.logical_name);
                                                   continue;
                                               }
                                               // Header unit. The scanner gives us the resolved
                                               // header path in source_path; that's the key both
                                               // GCC and we use to identify the header unit.
                                               if (req.lookup_method != "include-angle" && req.lookup_method != "include-quote") {
                                                   task_error = "Unsupported lookup-method '" + req.lookup_method + "' for import of '"
                                                                + req.logical_name + "' in " + input_path;
                                                   return;
                                               }
                                               if (!req.source_path.has_value() || req.source_path->empty()) {
                                                   task_error = "Header-unit import '" + req.logical_name
                                                                + "' has no resolved source-path in " + input_path
                                                                + " — scanner failed to locate the header on the include path";
                                                   return;
                                               }
                                               HeaderUnitNeeded hu;
                                               hu.source_path = *req.source_path;
                                               hu.is_system = (req.lookup_method == "include-angle");
                                               // First sighting wins; later sightings just confirm.
                                               auto it = header_units.find(hu.source_path);
                                               if (it == header_units.end()) {
                                                   header_units.emplace(hu.source_path, hu);
                                               } else if (it->second.is_system != hu.is_system) {
                                                   // Mixed angle/quote include of the same resolved
                                                   // path — pick angle ("system") since it's the more
                                                   // permissive include search.
                                                   it->second.is_system = it->second.is_system || hu.is_system;
                                               }
                                               task_header_units[obj_path].push_back(hu.source_path);
                                           }
                                           if (!requires_names.empty()) { task_requires[obj_path] = std::move(requires_names); }
                                       }
                                       if (!task_error.empty()) return;

                                       // Validate every required module has a provider.
                                       // The most common failure mode is `import std;` on
                                       // a GCC<15 toolchain (no libstdc++.modules.json) —
                                       // emit a targeted diagnostic for that case.
                                       for (const auto& [importer_obj, reqs] : task_requires) {
                                           for (const auto& mod : reqs) {
                                               if (module_to_task.count(mod)) continue;
                                               if (mod == "std" || mod == "std.compat") {
                                                   task_error = "Module '" + mod
                                                                + "' is not available: "
                                                                  "kiln couldn't locate libstdc++.modules.json. "
                                                                  "`import std;` requires GCC >= 15 with libstdc++ "
                                                                  "module support installed. (importer: "
                                                                + importer_obj + ")";
                                               } else {
                                                   task_error = "Module '" + mod
                                                                + "' has no provider in this build. "
                                                                  "Did you forget a target_link_libraries() to the providing target? "
                                                                  "(importer: "
                                                                + importer_obj + ")";
                                               }
                                               return;
                                           }
                                       }

                                       // Header-unit imports: each unique header gets a single
                                       // BMI under bmis/header_units/. We add a mapper entry
                                       // (key = resolved header path, value = BMI path) so
                                       // GCC finds the BMI when an importer compiles, and
                                       // remember the path so the next pass can spawn a
                                       // header-unit compile task and wire the dep edge.
                                       std::map<std::string, std::string> header_unit_to_bmi;
                                       if (!header_units.empty()) {
                                           if (!task.parent_target) {
                                               task_error = "Header-unit imports require a parent target on the collator task";
                                               return;
                                           }
                                           if (!collator.cxx_compiler) {
                                               task_error = "Header-unit imports need a C++ compiler, but none was resolved for target '"
                                                            + task.parent_target->get_name() + "'";
                                               return;
                                           }
                                           const std::string& bin_dir = task.parent_target->get_binary_dir();
                                           for (auto& [src, hu] : header_units) {
                                               std::string bmi = get_header_unit_bmi_path(bin_dir, src);
                                               header_unit_to_bmi[src] = bmi;
                                               ModuleMapEntry entry;
                                               entry.module_name = src; // header path is the key
                                               entry.bmi_path = bmi;
                                               entry.source_path = src;
                                               entry.object_task_id = bmi; // header-unit task id = BMI path
                                               entry.is_header_unit = true;
                                               mapper_entries.push_back(std::move(entry));
                                           }
                                       }

                                       // GCC writes BMIs straight to the mapper-advertised path
                                       // without creating intermediate dirs — so bmis/ must exist
                                       // before any compile reads/writes the mapper.
                                       for (const auto& entry : mapper_entries) {
                                           std::error_code dir_ec;
                                           std::filesystem::create_directories(std::string(Path(entry.bmi_path).parent_path()), dir_ec);
                                       }

                                       std::string mapper_content = generate_module_mapper_content(mapper_entries);
                                       // task.outputs[0] is the mapper, [1] is the manifest (set in
                                       // generate_module_collator_task). Write both.
                                       std::ofstream mapper_file(task.outputs[0]);
                                       if (!mapper_file) {
                                           task_error = "Failed to write module mapper: " + task.outputs[0];
                                           return;
                                       }
                                       mapper_file << mapper_content;
                                       mapper_file.close();

                                       if (task.outputs.size() >= 2) {
                                           auto wm = write_module_manifest(task.outputs[1], own_manifest);
                                           if (!wm) {
                                               task_error = wm.error();
                                               return;
                                           }
                                       }

                                       // Clang-style consumption: per-importer response files.
                                       // Clang has no -fmodule-mapper= equivalent, so we materialize
                                       // the same name -> BMI bindings into per-task rsp files at
                                       // <obj>.modules.rsp. ClangCompiler::emit_module_compile_flags
                                       // bakes `@<that-path>` into argv at scheduling time; the file
                                       // here is the late-bound contents.
                                       if (collator.cxx_compiler && collator.cxx_compiler->uses_per_task_module_rsp()) {
                                           // Build logical_name -> bmi_path lookup once.
                                           std::map<std::string, std::string> name_to_bmi;
                                           for (const auto& e : mapper_entries) {
                                               if (!e.is_header_unit) name_to_bmi[e.module_name] = e.bmi_path;
                                           }
                                           // Reverse module_to_task to find: obj_path -> logical_name
                                           // (so a producer task can emit -fmodule-output=<bmi>).
                                           std::map<std::string, std::string> obj_to_provided;
                                           for (const auto& [name, obj] : module_to_task) { obj_to_provided[obj] = name; }
                                           for (const auto& obj : local_module_objs) {
                                               std::string rsp_path = obj + ".modules.rsp";
                                               std::error_code dir_ec;
                                               std::filesystem::create_directories(std::string(Path(rsp_path).parent_path()), dir_ec);
                                               std::ofstream rsp(rsp_path);
                                               if (!rsp) {
                                                   task_error = "Failed to write modules rsp: " + rsp_path;
                                                   return;
                                               }
                                               auto req_it = task_requires.find(obj);
                                               if (req_it != task_requires.end()) {
                                                   for (const auto& name : req_it->second) {
                                                       auto bit = name_to_bmi.find(name);
                                                       if (bit == name_to_bmi.end()) continue;
                                                       rsp << "-fmodule-file=" << name << "=" << bit->second << "\n";
                                                   }
                                               }
                                               auto prov_it = obj_to_provided.find(obj);
                                               if (prov_it != obj_to_provided.end()) {
                                                   auto bit = name_to_bmi.find(prov_it->second);
                                                   if (bit != name_to_bmi.end()) { rsp << "-fmodule-output=" << bit->second << "\n"; }
                                               }
                                           }
                                       }

                                       inject_module_dependencies(module_to_task, task_requires);

                                       // Spawn header-unit compile tasks and wire compile-task
                                       // → header-unit-task edges. The mapper file already has
                                       // each header unit's BMI path advertised, so GCC will
                                       // place the BMI exactly where downstream importers expect.
                                       if (!header_units.empty()) {
                                           inject_header_unit_tasks(*task.parent_target, collator.cxx_compiler, collator.cxx_standard,
                                                                    collator.cxx_extensions_enabled,
                                                                    task.outputs[0], // mapper path
                                                                    header_units, header_unit_to_bmi, task_header_units, task_error);
                                           if (!task_error.empty()) return;
                                       }
                                   },
                                   [&](const ModuleScannerTask& scanner) {
                                       std::string scan_display(Path(scanner.source_file).filename());
                                       print_status("Scanning", scan_display);

                                       // GCC writes the P1689 JSON directly to ctx.output via
                                       // -fdeps-file=. We only need to surface compiler errors;
                                       // the DDI is parsed later by the collator.
                                       auto result = run_command(task.commands[0], task.working_dir);
                                       if (result.exit_code != 0) {
                                           report_command_failure(state, result.output);
                                           task_error = "Module scan failed for " + scanner.source_file;
                                           return;
                                       }
                                       if (!std::filesystem::exists(task.outputs[0])) {
                                           task_error = "Module scan produced no DDI: " + task.outputs[0];
                                       }
                                   },
                                   [&](const auto&) {
                                       // Regular task execution (compile, PCH, link, custom command/target, pre/post-build)
                                       std::string verb = "Running";
                                       auto src = task.get_source_file();
                                       std::string target_display(src.empty() ? Path(id).filename() : Path(src).filename());

                                       if (task.is_compilation()) {
                                           verb = "Compiling";
                                       } else if (std::holds_alternative<MocTask>(task.kind)) {
                                           verb = "  Moc'ing";
                                       } else if (std::holds_alternative<UicTask>(task.kind)) {
                                           verb = "  Uic'ing";
                                       } else if (std::holds_alternative<RccTask>(task.kind)) {
                                           verb = "  Rcc'ing";
                                       } else if (task.parent_target && id == task.parent_target->get_output_path()
                                                  && task.parent_target->get_type() != TargetType::CUSTOM) {
                                           verb = "  Linking";
                                       }

                                       if (task.parent_target && task.parent_target->get_type() == TargetType::CUSTOM) {
                                           std::string comment = task.parent_target->get_property("COMMENT");
                                           if (!comment.empty()) { target_display = comment; }
                                       }

                                       print_status(verb, target_display);

                                       for (auto cmd : task.commands) {
                                           // Strip shell-style quoting from COMMAND args.
                                           // CMake expects shell to strip quotes like -flag="value".
                                           // Since we use execvp (no shell), we strip them here.
                                           if (task.is_shell_command()) {
                                               for (auto& arg : cmd) { arg = strip_shell_quoting(arg); }
                                           }
                                           auto result = kiln::run_command(cmd, task.working_dir);
                                           if (result.exit_code != 0) {
                                               report_command_failure(state, result.output);
                                               task_error = "Command failed: " + join_command(cmd);
                                               return;
                                           } else if (!result.output.empty()) {
                                               std::lock_guard<std::mutex> lock(output_mutex_);
                                               if (state.output_capture) {
                                                   state.output_capture->append(result.output);
                                                   if (state.output_capture->empty() || state.output_capture->back() != '\n') {
                                                       state.output_capture->push_back('\n');
                                                   }
                                               } else {
                                                   state.progress.print_line(result.output);
                                               }
                                           }
                                       }

                                       // For custom_command tasks: warn if declared OUTPUTs
                                       // were not actually produced. CMake's Make/Ninja
                                       // generators do NOT error out here — they happily
                                       // accept rules whose touch lands somewhere other
                                       // than the declared OUTPUT (e.g. a relative OUTPUT
                                       // combined with a WORKING_DIRECTORY pointing at a
                                       // different binary subdir). A warning still flags
                                       // commands that exit 0 but write nothing, which
                                       // would otherwise cause confusing downstream
                                       // "missing header" errors.
                                       if (std::holds_alternative<CustomCommandTask>(task.kind)) {
                                           auto color = [&](std::string_view code) -> std::string_view {
                                               return state.stdout_is_tty ? code : std::string_view{};
                                           };
                                           for (const auto& out : task.outputs) {
                                               std::error_code ec;
                                               if (std::filesystem::exists(out, ec)) continue;

                                               std::filesystem::path expected(out);
                                               std::string detail;
                                               detail += std::string(color(kiln::colors::BOLD_YELLOW))
                                                         + "warning:" + std::string(color(kiln::colors::RESET));
                                               detail += " custom_command rule for '" + expected.filename().string()
                                                         + "' exited 0 but did not create its declared OUTPUT\n";
                                               detail += "         declared OUTPUT: " + out + "\n";
                                               detail += "         working dir:     " + task.working_dir + "\n";
                                               detail += std::string(color(kiln::colors::DIM));
                                               detail += "         downstream rules depending on this OUTPUT may fail, and this\n";
                                               detail += "         rule will re-run on every build since its OUTPUT never appears.\n";
                                               detail += std::string(color(kiln::colors::RESET));
                                               detail += "         commands run:\n";
                                               for (const auto& cmd : task.commands) {
                                                   detail += "           $ " + join_command(cmd) + "\n";
                                               }
                                               std::lock_guard<std::mutex> lock(output_mutex_);
                                               state.progress.print_line(detail);
                                           }
                                       }
                                   }},
                        task.kind);
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
                        for (const auto& out : task.outputs) { stat_cache_.erase(out); }
                    }

                    // Recalculate signature after compilation (now .d files exist)
                    if (!task.always_run) {
                        auto new_sig_res = calculate_signature(task);
                        if (!new_sig_res) {
                            task_error = new_sig_res.error();
                            break;
                        }
                        sig = *new_sig_res;
                    }
                } while (false);

                // Remove from active task list if we added it
                if (!active_display_name.empty()) { state.progress.task_finished(active_display_name); }

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
                    for (auto* dep_task : get_dependents(current)) { try_promote_to_ready(dep_task, state); }

                    state.cv.notify_all();
                }
            }
        });
    }

    for (auto& w : workers) {
        if (w.joinable()) w.join();
    }

    // Always save cache — even on failure, successful tasks should be cached
    // so we don't redo them on the next build. Cache-save errors are logged
    // but never override an in-flight build error.
    if (auto r = save_cache(state.build_dir, state.new_cache); !r) {
        kiln::print_message(std::cerr, "WARNING", "Failed to save build cache (" + state.build_dir + "): " + r.error());
    }

    // Save EP-specific caches to their respective build directories
    for (const auto& [ep_dir, ep_cache_entries] : state.ep_caches) {
        // Load existing EP cache and merge with new entries
        auto ep_cache = load_cache(ep_dir);
        for (const auto& [task_id, sig] : ep_cache_entries) { ep_cache[task_id] = sig; }
        if (auto r = save_cache(ep_dir, ep_cache); !r) {
            kiln::print_message(std::cerr, "WARNING", "Failed to save EP cache (" + ep_dir + "): " + r.error());
        }
    }

    state.progress.finish();

    if (!state.fatal_error.empty()) { return std::unexpected(state.fatal_error); }

    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

    // Compute critical path: longest dependency chain by execution time
    double max_critical_path = 0.0;
    if (dirty_task_count > 1) {
        std::unordered_set<BuildTask*> visited;
        std::function<double(BuildTask*)> compute_critical = [&](BuildTask* t) -> double {
            if (t->critical_path_s > 0.0) return t->critical_path_s; // memoized
            if (!visited.insert(t).second) return 0.0;               // cycle guard

            double dep_max = 0.0;
            for (auto* dep : t->dependencies) { dep_max = std::max(dep_max, compute_critical(dep)); }
            t->critical_path_s = t->execution_time_s + dep_max;
            return t->critical_path_s;
        };

        for (const auto& task_ptr : tasks_) { max_critical_path = std::max(max_critical_path, compute_critical(task_ptr.get())); }
    }

    {
        std::lock_guard<std::mutex> lock(output_mutex_);
        double wall_s = duration.count() / 1000.0;
        std::cout << kiln::c(std::cout, kiln::colors::BOLD_GREEN) << std::setw(12) << "Finished" << kiln::c(std::cout, kiln::colors::RESET)
                  << " build in " << std::fixed << std::setprecision(2) << wall_s << "s";
        if (max_critical_path > 0.0) { std::cout << " (critical path: " << std::setprecision(2) << max_critical_path << "s)"; }
        std::cout << std::endl;
    }

    return {};
}

kiln::CommandResult BuildGraph::run_command(const std::vector<std::string>& command, const std::string& working_dir) {
    return kiln::run_command(command, working_dir);
}

std::string BuildGraph::get_kiln_version() {
    return std::string(kiln::version_full());
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
        if (it != deps_cache_.end() && it->second.d_file_mtime == *d_mtime) { return it->second.deps; }
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
    while (ss >> dep) { deps.push_back(dep); }

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
    // Use the actual compiler binary from the task (argv[0]) so the scan runs
    // through the same driver as the real compile — clang++ accepts -H -E
    // identically to g++. Hardcoding "g++" here would invoke the wrong driver
    // (or fail entirely) on systems without GCC installed.
    if (command.empty() || command[0].empty()) return std::vector<std::string>{};
    scan_cmd.push_back(command[0]);
    scan_cmd.push_back("-H");
    scan_cmd.push_back("-E");

    std::string source;
    for (size_t i = 1; i < command.size(); ++i) {
        const auto& arg = command[i];
        if (arg.starts_with("-I") || arg.starts_with("-D") || arg.starts_with("-std=") || arg.starts_with("-f") || arg == "-include") {
            scan_cmd.push_back(arg);
            if (arg == "-include" && i + 1 < command.size()) { scan_cmd.push_back(command[++i]); }
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
    if (!pipe) return std::unexpected("Failed to execute " + scan_cmd[0] + " for header scanning");

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
        return std::unexpected("Header scanning failed for " + source + " with exit code " + std::to_string(status) + "\nOutput:\n"
                               + full_output);
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
    std::error_code ec;
    auto mtime = std::filesystem::last_write_time(path, ec);
    if (!ec) result = mtime;

    std::lock_guard<std::mutex> lock(state_mutex_);
    stat_cache_[path] = result;
    return result;
}

std::expected<std::string, std::string> BuildGraph::calculate_signature(const BuildTask& task) {
    std::ostringstream oss;
    oss << "cmds:";
    // Prefer signature_commands when populated — cosmetic flags (e.g.
    // -fdiagnostics-color=always) are stripped there so toggling color
    // doesn't invalidate object cache. Fall back to the real argv when a
    // task didn't go through a Compiler driver (custom commands etc).
    const auto& sig_cmds = (!task.signature_commands.empty() && task.signature_commands.size() == task.commands.size())
                               ? task.signature_commands
                               : task.commands;
    for (const auto& cmd : sig_cmds) oss << join_command(cmd) << ";";
    oss << "|";

    // Mix in the compiler version for any binary that's a registered toolchain
    // compiler. The binary path is already in the command string (so swap-by-
    // path already invalidates); this catches the system-update case where the
    // path is stable but the underlying compiler changed (e.g. gcc 13.2 →
    // 13.3 under /usr/bin/g++).
    //
    // The version is detected once at startup by enable_language /
    // detect_platform() and stored on the Compiler instance. We just look it
    // up here — no re-probing at signature time. Critically, this means we
    // never invoke arbitrary first-token binaries from custom_command argv,
    // which would be unsafe: side-effectful project scripts that use `$1` to
    // pick an output filename will happily create `--version.<ext>` in CWD if
    // we hand them `--version`.
    if (toolchain_) {
        std::set<std::string> unique_binaries;
        for (const auto& cmd : task.commands) {
            if (!cmd.empty()) unique_binaries.insert(cmd.front());
        }
        for (const auto& binary : unique_binaries) {
            std::string ver = toolchain_->version_for(binary);
            if (!ver.empty()) { oss << "tool:" << binary << ":" << ver << "|"; }
        }
    }

    oss << "kiln:" << get_kiln_version() << "|";

    // 1. Primary inputs (combined exists+stat)
    for (const auto& in : task.inputs) {
        if (auto mtime = get_file_time_if_exists(in)) { oss << in << ":" << mtime->time_since_epoch().count() << "|"; }
    }

    // 2. Header dependencies from .d files (cached parsing + combined exists+stat).
    // Filter out the task's own outputs: GCC's depfile for module units emits
    // multi-target rules (e.g. `std.o std.gcm: ...`) plus order-only rules
    // (`std.gcm:| std.o`) that the simple `find(':')` parser conflates into
    // listing the .o as a dep of itself. Re-stating an output as a dep makes
    // every signature mtime-mismatch, so the task perpetually rebuilds.
    std::set<std::string> own_outputs(task.outputs.begin(), task.outputs.end());
    bool found_deps = false;
    for (const auto& out : task.outputs) {
        auto deps = get_deps_for_output(out);
        if (!deps.empty()) {
            for (const auto& dep : deps) {
                if (own_outputs.count(dep)) continue;
                if (auto mtime = get_file_time_if_exists(dep)) { oss << "dep:" << dep << ":" << mtime->time_since_epoch().count() << "|"; }
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

    std::ifstream meta_file(Path::join(build_dir, ".kiln_build_path"));
    if (meta_file) {
        std::string cached_path;
        std::getline(meta_file, cached_path);
        if (cached_path != canonical_build_dir.string()) {
            // Build dir moved - invalidate cache
            return cache;
        }
    }

    std::ifstream file(Path::join(build_dir, ".kiln_cache"));
    if (!file) return cache;

    std::string line;
    while (std::getline(file, line)) {
        size_t sep = line.find('=');
        if (sep != std::string::npos) { cache[line.substr(0, sep)] = line.substr(sep + 1); }
    }
    return cache;
}

std::expected<void, std::string> BuildGraph::save_cache(const std::string& build_dir, const std::map<std::string, std::string>& cache) {
    std::error_code ec;
    std::filesystem::create_directories(build_dir, ec);
    if (ec) { return std::unexpected("Failed to create build directory: " + build_dir + " (" + ec.message() + ")"); }

    // Write build_dir validation file
    auto canonical_build_dir = std::filesystem::canonical(build_dir, ec);
    if (ec) { return std::unexpected("Failed to canonicalize build directory: " + build_dir); }
    std::ofstream meta_file(Path::join(build_dir, ".kiln_build_path"));
    if (meta_file) { meta_file << canonical_build_dir.string(); }

    std::string cache_path = Path::join(build_dir, ".kiln_cache");
    std::ofstream file(cache_path);
    if (!file) { return std::unexpected("Failed to open cache file for writing: " + cache_path); }

    for (const auto& [id, sig] : cache) { file << id << "=" << sig << "\n"; }

    if (!file) { return std::unexpected("Failed to write to cache file"); }

    return {};
}

void BuildGraph::inject_header_unit_tasks(Target& parent_target, const Compiler* cxx_compiler, const std::string& cxx_standard,
                                          bool cxx_extensions_enabled, const std::string& mapper_path,
                                          const std::map<std::string, HeaderUnitInfo>& header_units,
                                          const std::map<std::string, std::string>& header_unit_to_bmi,
                                          const std::map<std::string, std::vector<std::string>>& task_header_units,
                                          std::string& task_error) {

    if (!cxx_compiler) return;

    auto txn = begin_locked(graph_mutation_mutex_);

    // Snapshot resolved CXX-relevant properties from the target. These are the
    // include / definition / option flags an importer of one of these header
    // units would compile with — using them here keeps the header unit's BMI
    // ABI-compatible with the importing TU's view of macros and headers.
    const auto& include_dirs = parent_target.get_resolved_property("INCLUDE_DIRECTORIES");
    const auto& sys_include_dirs = parent_target.get_resolved_property("SYSTEM_INCLUDE_DIRECTORIES");
    const auto& definitions = parent_target.get_resolved_property("COMPILE_DEFINITIONS");

    // Map header source path → header-unit task pointer, so we can wire dep
    // edges in the second pass without a re-find through task_by_id_.
    std::map<std::string, BuildTask*> hu_task_by_src;

    for (const auto& [src_path, info] : header_units) {
        auto bmi_it = header_unit_to_bmi.find(src_path);
        if (bmi_it == header_unit_to_bmi.end()) continue;
        const std::string& bmi_path = bmi_it->second;

        // Idempotent: a previous collator (cross-target shared header unit)
        // may have already inserted a task with this id — reuse it rather
        // than fail.
        if (auto* existing = txn.find_task(bmi_path)) {
            hu_task_by_src[src_path] = existing;
            continue;
        }

        HeaderUnitContext hctx;
        hctx.source = src_path;
        hctx.bmi_output = bmi_path;
        hctx.module_mapper_file = mapper_path;
        hctx.is_system_header = info.is_system;
        hctx.standard = cxx_standard;
        hctx.extensions_enabled = cxx_extensions_enabled;
        hctx.includes.assign(include_dirs.begin(), include_dirs.end());
        hctx.system_includes.assign(sys_include_dirs.begin(), sys_include_dirs.end());
        hctx.definitions.assign(definitions.begin(), definitions.end());
        hctx.color_diagnostics = isatty(STDOUT_FILENO);

        auto cmd = cxx_compiler->get_header_unit_compile_command(hctx);
        if (cmd.argv.empty()) {
            task_error = "Compiler does not support header-unit compilation; cannot import header '" + src_path + "'";
            return;
        }

        BuildTask hu_task;
        hu_task.id = bmi_path;
        hu_task.kind = CompileTask{src_path, Language::CXX};
        hu_task.parent_target = &parent_target;
        hu_task.commands.push_back(std::move(cmd.argv));
        hu_task.signature_commands.push_back(std::move(cmd.signature_argv));
        hu_task.inputs.push_back(src_path);
        hu_task.inputs.push_back(mapper_path);
        hu_task.outputs.push_back(bmi_path);

        auto added = txn.add(std::move(hu_task));
        if (!added) {
            task_error = "Failed to add header-unit task: " + added.error();
            return;
        }
        hu_task_by_src[src_path] = *added;
    }

    // Wire compile-task → header-unit-task edges. The importing TU's compile
    // reads the BMI off the mapper-advertised path, so it must wait for the
    // header unit's compile to land that BMI.
    for (const auto& [importer_id, hu_srcs] : task_header_units) {
        auto importer_it = task_by_id_.find(importer_id);
        if (importer_it == task_by_id_.end()) continue;
        BuildTask* importer = importer_it->second;
        for (const auto& src : hu_srcs) {
            auto hu_it = hu_task_by_src.find(src);
            if (hu_it == hu_task_by_src.end()) continue;
            txn.dependency(importer, hu_it->second);
        }
    }
}

void BuildGraph::inject_module_dependencies(const std::map<std::string, std::string>& module_to_task,
                                            const std::map<std::string, std::vector<std::string>>& task_requires) {

    auto txn = begin_locked(graph_mutation_mutex_);

    for (auto& task_ptr : tasks_) {
        // Find module requirements for this task
        auto req_it = task_requires.find(task_ptr->id);
        if (req_it == task_requires.end()) continue;

        const auto& required_modules = req_it->second;

        for (const auto& required_module : required_modules) {
            auto provider_it = module_to_task.find(required_module);
            if (provider_it == module_to_task.end()) { continue; }

            auto task_it = task_by_id_.find(provider_it->second);
            if (task_it == task_by_id_.end()) continue;

            auto* provider = task_it->second;

            // Add dependency: this task depends on the provider task
            txn.dependency(task_ptr.get(), provider);
        }
    }
    // txn commits + releases lock on destruction
}

std::expected<int, std::string> BuildGraph::attach_ep_graph(BuildGraph&& ep_graph, const std::string& ep_binary_dir,
                                                            ExecutionState& state) {

    // Lock state.mutex for thread safety - same lock used by execute() worker threads
    std::lock_guard<std::mutex> lock(state.mutex);

    // 1. Load EP's cache for dirty computation
    auto ep_cache = load_cache(ep_binary_dir);

    // 2. Pre-compute dirty state per task (before transferring ownership).
    //    Mirrors the main pre-scan: empty-outputs tasks (with commands) are
    //    treated as dirty, and the stat cache is reused via clean_signature().
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
        bool is_dirty = !is_marker && !clean_signature(task, ep_cache).has_value();

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

    // 4. Apply dirty state from pre-computed info.
    //    Marker tasks (no outputs, no commands — e.g. a commandless custom_target
    //    used purely to group DEPENDS) have nothing to execute, but they still
    //    serve to order their dependents AFTER their dependencies. Route them
    //    through dirty_state so the scheduler waits for their deps before
    //    declaring them complete. The executor's catch-all branch handles a
    //    commandless task as a no-op (the commands loop runs zero times).
    //    Markers without dependencies are completed immediately to avoid
    //    enqueuing trivial no-ops.
    int dirty_count = 0;
    for (const auto& info : dirty_info) {
        if (info.is_marker) {
            if (info.raw->dependencies.empty()) {
                state.completed.insert(info.raw);
            } else {
                state.dirty_state[info.raw] = true;
                dirty_count++;
            }
        } else if (info.is_dirty) {
            state.dirty_state[info.raw] = true;
            dirty_count++;
        } else {
            state.completed.insert(info.raw);
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

    // 6. Propagate dirtiness through reverse edges. Walks the entire dirty_state
    //    so newly-attached EP-dirty tasks can also upgrade existing main-graph
    //    "maybe" entries (whose orchestrator is now known to have produced real
    //    work) to definite. The size delta is the count of newly-dirty tasks.
    size_t before = state.dirty_state.size();
    propagate_dirty_bfs(state.dirty_state);
    dirty_count += static_cast<int>(state.dirty_state.size() - before);

    // 7. Update progress and add ready dirty tasks to ready_set
    state.progress.bump_total(dirty_count);

    for (const auto& [ptr, _] : state.dirty_state) { try_promote_to_ready(ptr, state); }

    // Notify waiting threads that new tasks are available
    state.cv.notify_all();

    return dirty_count;
}

std::optional<std::string> BuildGraph::run_ep_orchestrator(BuildTask& task, ExecutionState& state) {

    // Get the ExternalProjectTarget from the task
    auto* ep_target = dynamic_cast<ExternalProjectTarget*>(task.parent_target);
    if (!ep_target) { return "EP orchestrator task has no ExternalProjectTarget"; }

    std::string ep_name = ep_target->get_name();
    std::string sentinel_id = ep_name; // Sentinel task ID is the EP name

    // Helper to print output lines with EP name prefix (for child interpreter output)
    auto print_prefixed_output = [&](const std::string& output) {
        if (output.empty()) return;
        std::string prefix = std::string(c(state.stdout_is_tty, colors::DIM)) + std::string(c(state.stdout_is_tty, colors::WHITE)) + "["
                             + ep_name + "] " + std::string(c(state.stdout_is_tty, colors::RESET));
        std::istringstream iss(output);
        std::string line;
        std::lock_guard<std::mutex> lock(output_mutex_);
        while (std::getline(iss, line)) { state.progress.print_line(prefix + line); }
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
        // Tracks whether the install-injection block already moved ep_interp
        // into ep_target. The fallback hand-off at the end of this branch
        // only runs if it didn't.
        bool ep_interpreter_handed_off = false;

        // Force colors in child interpreter if parent stdout is a TTY
        if (state.stdout_is_tty) { ep_interp->set_force_colors(true); }

        // Apply CMAKE_ARGS and CMAKE_CACHE_ARGS.
        // Hard gate: values forwarded into the child interpreter's variables must
        // have had their genex resolved by external_project.cpp at intercept time.
        // Defense in depth: catches future regressions where a value bypasses that
        // evaluation and gets expanded as a literal in install paths or commands.
        std::vector<std::string> initial_cache_scripts;
        auto apply_arg = [&](const std::string& arg, const char* origin) {
            if (arg.starts_with("-D")) {
                std::string def = arg.substr(2);
                size_t eq = def.find('=');
                if (eq != std::string::npos) {
                    std::string var = def.substr(0, eq);
                    std::string val = def.substr(eq + 1);
                    // Strip type annotation
                    size_t colon = var.find(':');
                    if (colon != std::string::npos) var = var.substr(0, colon);
                    assert_no_genex(val, "EP " + ep_name + " " + origin + " " + var);
                    ep_interp->set_variable(var, val);
                }
            } else if (arg.starts_with("-C")) {
                // -C<path> / -C <path>: initial-cache script to pre-populate
                // the child cache before interpreting CMakeLists.txt.
                std::string path = arg.substr(2);
                if (!path.empty()) initial_cache_scripts.push_back(std::move(path));
            }
        };
        for (const auto& arg : ep_target->get_cmake_args()) apply_arg(arg, "CMAKE_ARGS");
        for (const auto& arg : ep_target->get_cmake_cache_args()) apply_arg(arg, "CMAKE_CACHE_ARGS");

        // Set CMAKE_INSTALL_PREFIX if not already set
        if (ep_interp->get_variable("CMAKE_INSTALL_PREFIX").empty()) { ep_interp->set_variable("CMAKE_INSTALL_PREFIX", install_dir); }

        // Pre-load any initial-cache scripts from -C<file>. CMake interprets
        // these before CMakeLists.txt; projects use this to seed cache vars
        // (e.g. CMAKE_MODULE_PATH) for sub-builds.
        for (const auto& script_path : initial_cache_scripts) {
            if (!std::filesystem::exists(script_path)) { return "EP " + ep_name + ": initial-cache script not found: " + script_path; }
            std::ifstream cfs(script_path);
            std::string cscript((std::istreambuf_iterator<char>(cfs)), std::istreambuf_iterator<char>());
            Parser cparser(cscript, script_path);
            auto cast_ = cparser.parse();
            if (!cast_) { return "EP " + ep_name + ": parse error in -C " + script_path + ": " + cast_.error().reason; }
            ep_interp->set_current_file(script_path);
            auto cres = ep_interp->interpret(*cast_);
            if (!cres) {
                std::string output = ep_output.str();
                return "EP " + ep_name + ": -C " + script_path + ": " + cres.error().message
                       + (output.empty() ? "" : "\nOutput:\n" + output);
            }
        }

        // Read and parse CMakeLists.txt
        std::string cmake_file = source_dir + "/CMakeLists.txt";
        if (!std::filesystem::exists(cmake_file)) { return "EP " + ep_name + ": CMakeLists.txt not found at " + cmake_file; }

        std::ifstream file(cmake_file);
        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

        Parser parser(content, cmake_file);
        auto ast = parser.parse();
        if (!ast) { return "EP " + ep_name + ": Parse error in " + cmake_file + ": " + ast.error().reason; }

        ep_interp->set_current_file(cmake_file);

        // Interpret
        auto interp_result = ep_interp->interpret(*ast);
        if (!interp_result) {
            std::string output = ep_output.str();
            // Render the full diagnostic (file, line, caret, call stack) so
            // command-syntax errors and friends carry the same context as
            // top-level errors. Plain string concatenation lost the backtrace.
            std::ostringstream diag;
            const auto& err = interp_result.error();
            kiln::print_diagnostic(diag, kiln::DiagnosticSeverity::Error, err.message, err.file, err.row, err.col, err.offset, err.length,
                                   err.backtrace, err.source_content);
            return "EP " + ep_name + ": Interpretation error:\n" + diag.str() + (output.empty() ? "" : "\nOutput:\n" + output);
        }

        // Apply accumulated directory properties (include_directories, add_definitions, etc.)
        // to all targets, matching what add_subdirectory/FetchContent do.
        ep_interp->finalize_directory_targets();

        // Print any buffered interpretation output (with EP name prefix)
        print_prefixed_output(ep_output.str());
        ep_output.str(""); // Clear for next phase

        // Generate full build graph (not just dirty tasks)
        // attach_ep_graph will handle dirty detection for each task
        auto graph_result = ep_interp->generate_build_graph({});
        if (!graph_result) {
            std::string output = ep_output.str();
            return "EP " + ep_name + ": Task generation error: " + graph_result.error().message
                   + (output.empty() ? "" : "\nOutput:\n" + output);
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
        if (!attach_result) { return "EP " + ep_name + ": " + attach_result.error(); }

        int dirty_count = *attach_result;

        // For cmake-based EPs, we interpret CMakeLists.txt so we use install rules,
        // not any custom INSTALL_COMMAND (which would require cmake-generated Makefile).
        const auto& install_rules = ep_interp->get_install_rules();
        // Mirror what `cmake --install` would do: honor CMAKE_INSTALL_PREFIX when the
        // EP's CMakeLists set it (typically via CMAKE_ARGS=-DCMAKE_INSTALL_PREFIX=...).
        // Fall back to ExternalProject's INSTALL_DIR when not set.
        std::string install_prefix = ep_interp->get_variable("CMAKE_INSTALL_PREFIX");
        if (install_prefix.empty()) install_prefix = ep_target->get_ep_install_dir();
        std::string config = ep_interp->get_variable("CMAKE_BUILD_TYPE");

        // Helper to run extra install commands (skip "make install" which we replaced with install rules)
        auto run_extra_install_cmds = [&]() -> std::optional<std::string> {
            const auto& install_cmd = ep_target->get_install_command();
            if (install_cmd.is_empty || install_cmd.commands.empty()) return std::nullopt;
            std::string working_dir = ep_target->get_ep_binary_dir();
            for (const auto& cmd : install_cmd.commands) {
                // Skip "make install" / "cmake --install" - we handle install rules ourselves
                if (is_make_install_command(cmd) || is_cmake_install_command(cmd)) continue;
                auto result = kiln::run_command(cmd, working_dir);
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
                auto plan_or = build_install_plan(ep_interp.get(), install_rules, install_prefix, config);
                if (!plan_or) { return "EP " + ep_name + " install plan failed: " + plan_or.error(); }
                auto install_result = execute_install_plan(*plan_or, install_prefix, config);
                if (!install_result) { return "EP " + ep_name + " install failed: " + install_result.error(); }
            }
            // Run extra install commands (e.g., cp for internal headers)
            if (auto err = run_extra_install_cmds()) return *err;
            print_prefixed_output("EP is up-to-date");
        } else {
            // EP has build tasks - create install task with proper dependencies
            if (!install_rules.empty()) {
                ep_target->set_pending_install_rules(install_rules, config);

                // Compute install task inputs and pre-compute target destinations
                // (MUST be done BEFORE storing interpreter on ep_target)
                auto install_inputs = compute_install_inputs(install_rules, ep_interp.get(), install_prefix, ep_target);

                // Create install task
                std::string install_id = ep_name + ":install";
                BuildTask install_task;
                install_task.id = install_id;
                install_task.kind = EPInstallTask{ep_name};
                install_task.parent_target = ep_target;
                install_task.ep_binary_dir = binary_dir;
                install_task.inputs = std::move(install_inputs);

                // Inject install task and wire sentinel to depend on it.
                //
                // set_ep_interpreter MUST happen under the same lock that
                // promotes install_raw to ready_set: the install task body
                // reads ep_target->get_ep_interpreter() (build_system.cpp
                // EPInstallTask handler) for CMAKE_INSTALL_PREFIX. If the
                // store is sequenced after the promotion, a worker can pick
                // up install_raw and dereference an empty unique_ptr (TSan:
                // external_project_target.hpp:119 read vs. set_ep_interpreter
                // write). Passing the unique_ptr through the same mutex
                // acquire gives proper happens-before to the reader.
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

                    // Hand the EP interpreter to the install task (and any
                    // deferred install(EXPORT) work) BEFORE making install_raw
                    // schedulable.
                    ep_target->set_ep_interpreter(std::move(ep_interp));
                    ep_interpreter_handed_off = true;

                    // Promote install_raw if its deps are already satisfied —
                    // otherwise it sits in dirty_state forever (no edge fires
                    // a re-check, because nothing it depends on is going to
                    // transition to completed after this point). The
                    // scheduler only promotes via dependency-completion
                    // edges or via this explicit nudge.
                    try_promote_to_ready(install_raw, state);
                    state.cv.notify_all();
                }
            }
            // Extra install commands will be run by install task (it has access to ep_target)
        }

        // Keep EP interpreter alive — injected tasks hold raw parent_target
        // pointers into its target map. If we didn't hand it off above (e.g.
        // no install task was injected), do it here.
        if (!ep_interpreter_handed_off) { ep_target->set_ep_interpreter(std::move(ep_interp)); }

    } else {
        // === CUSTOM COMMANDS EP ===
        // Run CONFIGURE_COMMAND, BUILD_COMMAND, INSTALL_COMMAND sequentially

        std::string working_dir = ep_target->get_ep_binary_dir();
        std::filesystem::create_directories(working_dir);

        auto run_step = [&](const EPStepCommand& step, const char* step_name) -> std::optional<std::string> {
            if (step.is_empty || step.commands.empty()) return std::nullopt;
            for (const auto& cmd : step.commands) {
                auto result = kiln::run_command(cmd, working_dir);
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

    return std::nullopt; // Success
}

} // namespace kiln
