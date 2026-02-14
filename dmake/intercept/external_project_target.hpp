#pragma once

#include "../target.hpp"
#include <string>
#include <vector>
#include <map>
#include <memory>

namespace dmake {

class Interpreter;
struct InstallRule;

// Pre-computed target install info (computed while interpreter is alive,
// used by install task after interpreter is destroyed)
struct PendingTargetInstall {
    std::string artifact_path;  // Built artifact (e.g., /build/libmylib.a)
    std::string dest_path;      // Full destination path (e.g., /install/lib/libmylib.a)
};

// Metadata for an ExternalProject step command (may contain multiple commands)
struct EPStepCommand {
    std::vector<std::vector<std::string>> commands;  // One or more commands
    bool is_empty = false;                           // Empty string means skip this step
};

// ExternalProjectTarget: Represents an ExternalProject_Add target.
// Extends CustomTarget to participate in the build graph.
// Unlike regular custom targets, EP targets create TWO tasks:
//   1. Orchestrator task (name:orchestrate) - runs at build time to spawn
//      an isolated interpreter (cmake-based) or execute shell commands
//   2. Sentinel task (name) - depends on orchestrator and all injected tasks;
//      parent targets depend on this sentinel (via add_dependencies)
//
// The orchestrator + sentinel pattern ensures that:
// - EP work happens at build time, not configure time
// - Parent targets that depend on the EP (via add_dependencies) properly
//   wait for ALL EP work (configure, build, install) to complete
// - Dynamic task injection can extend the sentinel's dependencies
class ExternalProjectTarget : public CustomTarget {
public:
    ExternalProjectTarget(std::string name, std::string source_dir, std::string binary_dir)
        : CustomTarget(std::move(name), std::move(source_dir), std::move(binary_dir)) {}

    // Generate tasks creates TWO tasks: orchestrator and sentinel
    void generate_tasks(GraphTransaction& txn, const Toolchain& toolchain,
                        const std::map<std::string, std::shared_ptr<Target>>& all_targets,
                        const Interpreter& interp,
                        const std::vector<std::string>& exe_linker_flags = {},
                        const std::vector<std::string>& shared_linker_flags = {}) override;

    // EP configuration (set by ExternalProject_Add at configure time)
    void set_ep_source_dir(std::string dir) { ep_source_dir_ = std::move(dir); }
    void set_ep_binary_dir(std::string dir) { ep_binary_dir_ = std::move(dir); }
    void set_ep_install_dir(std::string dir) { ep_install_dir_ = std::move(dir); }
    void set_ep_prefix(std::string dir) { ep_prefix_ = std::move(dir); }
    void set_ep_tmp_dir(std::string dir) { ep_tmp_dir_ = std::move(dir); }
    void set_ep_stamp_dir(std::string dir) { ep_stamp_dir_ = std::move(dir); }
    void set_ep_download_dir(std::string dir) { ep_download_dir_ = std::move(dir); }
    void set_ep_source_subdir(std::string dir) { ep_source_subdir_ = std::move(dir); }

    const std::string& get_ep_source_dir() const { return ep_source_dir_; }
    const std::string& get_ep_binary_dir() const { return ep_binary_dir_; }
    const std::string& get_ep_install_dir() const { return ep_install_dir_; }
    const std::string& get_ep_prefix() const { return ep_prefix_; }
    const std::string& get_ep_tmp_dir() const { return ep_tmp_dir_; }
    const std::string& get_ep_stamp_dir() const { return ep_stamp_dir_; }
    const std::string& get_ep_download_dir() const { return ep_download_dir_; }
    const std::string& get_ep_source_subdir() const { return ep_source_subdir_; }

    // Returns effective source dir (with SOURCE_SUBDIR applied)
    std::string get_effective_source_dir() const {
        if (ep_source_subdir_.empty()) return ep_source_dir_;
        return ep_source_dir_ + "/" + ep_source_subdir_;
    }

    // CMAKE_ARGS and CMAKE_CACHE_ARGS (for cmake-based EPs)
    void add_cmake_arg(std::string arg) { cmake_args_.push_back(std::move(arg)); }
    void add_cmake_cache_arg(std::string arg) { cmake_cache_args_.push_back(std::move(arg)); }
    const std::vector<std::string>& get_cmake_args() const { return cmake_args_; }
    const std::vector<std::string>& get_cmake_cache_args() const { return cmake_cache_args_; }

    // LIST_SEPARATOR for cmake args
    void set_list_separator(std::string sep) { list_separator_ = std::move(sep); }
    const std::string& get_list_separator() const { return list_separator_; }

    // Step commands (for non-cmake EPs or custom steps)
    void set_configure_command(EPStepCommand cmd) { configure_command_ = std::move(cmd); }
    void set_build_command(EPStepCommand cmd) { build_command_ = std::move(cmd); }
    void set_install_command(EPStepCommand cmd) { install_command_ = std::move(cmd); }
    const EPStepCommand& get_configure_command() const { return configure_command_; }
    const EPStepCommand& get_build_command() const { return build_command_; }
    const EPStepCommand& get_install_command() const { return install_command_; }

    // Build options
    void set_build_in_source(bool v) { build_in_source_ = v; }
    void set_build_always(bool v) { build_always_ = v; }
    bool is_build_in_source() const { return build_in_source_; }
    bool is_build_always() const { return build_always_; }

    // Check if this is a cmake-based EP (no custom CONFIGURE_COMMAND and CMakeLists.txt exists)
    bool is_cmake_based() const;

    // Token replacements for step commands
    std::vector<std::pair<std::string, std::string>> get_token_replacements() const;

    // Pending install rules (for cmake-based EPs, stored during orchestrator, run during sentinel)
    void set_pending_install_rules(std::vector<std::shared_ptr<InstallRule>> rules, std::string config) {
        pending_install_rules_ = std::move(rules);
        pending_install_config_ = std::move(config);
    }
    const std::vector<std::shared_ptr<InstallRule>>& get_pending_install_rules() const { return pending_install_rules_; }
    const std::string& get_pending_install_config() const { return pending_install_config_; }

    // Pre-computed target installs (computed while interpreter is alive)
    void add_pending_target_install(PendingTargetInstall install) {
        pending_target_installs_.push_back(std::move(install));
    }
    const std::vector<PendingTargetInstall>& get_pending_target_installs() const { return pending_target_installs_; }

private:
    // EP directories
    std::string ep_source_dir_;
    std::string ep_binary_dir_;
    std::string ep_install_dir_;
    std::string ep_prefix_;
    std::string ep_tmp_dir_;
    std::string ep_stamp_dir_;
    std::string ep_download_dir_;
    std::string ep_source_subdir_;

    // CMAKE_ARGS and CMAKE_CACHE_ARGS
    std::vector<std::string> cmake_args_;
    std::vector<std::string> cmake_cache_args_;
    std::string list_separator_;

    // Step commands
    EPStepCommand configure_command_;
    EPStepCommand build_command_;
    EPStepCommand install_command_;

    // Build options
    bool build_in_source_ = false;
    bool build_always_ = false;

    // Pending install rules (for cmake-based EPs with build tasks)
    std::vector<std::shared_ptr<InstallRule>> pending_install_rules_;
    std::string pending_install_config_;

    // Pre-computed target installs (artifact_path -> dest_path)
    std::vector<PendingTargetInstall> pending_target_installs_;
};

} // namespace dmake
