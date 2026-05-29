#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace kiln {
class Interpreter;

// Re-root a list of search paths according to CMAKE_FIND_ROOT_PATH and the
// caller's mode. Modes: "NEVER" (no-op), "ONLY" (only re-rooted), "BOTH"
// (re-rooted then originals). Defined in find_commands.cpp; shared with
// find_package.cpp for cross-build support in package lookup.
std::vector<std::filesystem::path> apply_find_root_path(Interpreter& interp, const std::vector<std::filesystem::path>& search_paths,
                                                        const std::string& mode);

} // namespace kiln
