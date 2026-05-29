#pragma once

#include "target.hpp"
#include <expected>
#include <string>

namespace kiln {

class GraphTransaction;
class Interpreter;

// Called from Target::generate_tasks() after resolve() and pre-build task creation,
// but before module scanner and object task generation.
// Scans sources/headers for Qt macros, creates moc/uic/rcc tasks,
// injects generated sources into SOURCES, adds autogen include dir.
std::expected<void, std::string> generate_autogen_tasks(Target& target, GraphTransaction& txn, Interpreter& interp,
                                                        const TargetMap& all_targets, const std::string& pre_build_task_id,
                                                        const std::vector<std::string>& manual_dep_ids);

} // namespace kiln
