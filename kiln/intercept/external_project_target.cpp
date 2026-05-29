#include "external_project_target.hpp"
#include "../build_system.hpp"
#include "../interperter.hpp"
#include <filesystem>

namespace kiln {

ExternalProjectTarget::~ExternalProjectTarget() = default;

bool ExternalProjectTarget::is_cmake_based() const {
    // cmake-based if:
    // 1. No custom CONFIGURE_COMMAND (or empty)
    // 2. CMakeLists.txt exists in effective source dir
    if (!configure_command_.commands.empty() && !configure_command_.is_empty) {
        return false; // Has custom configure command
    }

    std::string cmake_file = get_effective_source_dir() + "/CMakeLists.txt";
    return std::filesystem::exists(cmake_file);
}

std::vector<std::pair<std::string, std::string>> ExternalProjectTarget::get_token_replacements() const {
    return {
        {"<SOURCE_DIR>", ep_source_dir_}, {"<SOURCE_SUBDIR>", get_effective_source_dir()},
        {"<BINARY_DIR>", ep_binary_dir_}, {"<INSTALL_DIR>", ep_install_dir_},
        {"<TMP_DIR>", ep_tmp_dir_},
    };
}

std::expected<void, std::string> ExternalProjectTarget::generate_tasks(GraphTransaction& txn, const Toolchain&,
                                                                       const TargetMap& all_targets, const Interpreter& interp,
                                                                       const std::vector<std::string>&, const std::vector<std::string>&) {
    // ExternalProjectTarget generates TWO tasks:
    //
    // 1. Orchestrator task (name_:orchestrate)
    //    - Runs at build time when all DEPENDS are satisfied
    //    - For cmake-based EPs: spawns isolated interpreter, extracts dirty tasks, injects them
    //    - For custom EPs: runs CONFIGURE_COMMAND/BUILD_COMMAND/INSTALL_COMMAND
    //    - Uses EPOrchestratorTask kind so execute() handles it specially
    //
    // 2. Sentinel task (name_)
    //    - Initially depends only on orchestrator
    //    - Orchestrator will add more dependencies at runtime (the injected tasks)
    //    - Parent targets depend on this (via add_dependencies(X, ep_name))
    //    - Ensures parent targets wait for ALL EP work to complete

    std::string orchestrator_id = name_ + ":orchestrate";
    std::string sentinel_id = name_;

    // --- Orchestrator task ---
    BuildTask orchestrator;
    orchestrator.id = orchestrator_id;
    orchestrator.kind = EPOrchestratorTask{name_};
    orchestrator.parent_target = this;
    orchestrator.always_run = true; // Must check if EP needs rebuilding
    orchestrator.working_dir = ep_binary_dir_;

    // Handle DEPENDS from add_custom_target/ExternalProject_Add
    // These are dependencies on other targets (often other EPs)
    for (const auto& dep_name : get_custom_dependencies()) {
        // Skip self-dependencies
        if (dep_name == name_) continue;

        auto dep_it = all_targets.find(dep_name);
        if (dep_it != all_targets.end()) {
            // Target dependency - depend on the target's output or its task ID
            std::string dep_out = dep_it->second->get_output_path();
            if (!dep_out.empty()) {
                orchestrator.explicit_deps.push_back(dep_out);
            } else {
                // Custom/EP targets may not have output paths - use target name as task ID
                orchestrator.explicit_deps.push_back(dep_name);
            }
        }
        // File dependencies are not relevant for orchestrator (it doesn't use files directly)
    }

    if (auto r = txn.add(std::move(orchestrator)); !r) return std::unexpected(r.error());

    // --- Sentinel task ---
    BuildTask sentinel;
    sentinel.id = sentinel_id;
    sentinel.kind = EPSentinelTask{name_};
    sentinel.parent_target = this;
    sentinel.always_run = true; // Sentinel must run every build to check if EP is dirty
    // No commands - sentinel is just a synchronization point

    // Sentinel depends on orchestrator
    sentinel.explicit_deps.push_back(orchestrator_id);

    if (auto r = txn.add(std::move(sentinel)); !r) return std::unexpected(r.error());
    return {};
}

} // namespace kiln
