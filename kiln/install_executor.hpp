#pragma once
#include "install_plan.hpp"
#include "printing.hpp"
#include <expected>
#include <memory>
#include <string>
#include <vector>

namespace kiln {

class Interpreter;
struct InstallRule;

// Resolve InstallRules from the interpreter into a self-contained InstallPlan.
// Genex evaluation, target lookups, export-file rendering all happen here so the
// install phase can execute the plan without re-entering the interpreter.
//
// Called at the end of a successful build; the resulting plan is serialized to
// <build_dir>/install_plan.json and consumed by `kiln install`.
std::expected<InstallPlan, std::string> build_install_plan(Interpreter* interp, const std::vector<std::shared_ptr<InstallRule>>& rules,
                                                           const std::string& default_prefix, const std::string& current_config,
                                                           const OutputCtx& out = stdout_output_ctx());

// Execute a resolved plan. No interpreter is needed; writes go only to
// install_prefix (joined with DESTDIR by the caller if applicable).
std::expected<void, std::string> execute_install_plan(const InstallPlan& plan, const std::string& install_prefix,
                                                      const std::string& current_config, const std::string& component_filter = "",
                                                      const OutputCtx& out = stdout_output_ctx());

} // namespace kiln
