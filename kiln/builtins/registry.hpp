#pragma once

#include <vector>

namespace kiln {

class Interpreter;
struct PreParsedMath;
struct PreParsedSubstring;
struct Argument;

// Fast-path executors for invocations whose shape was recognized at parse
// time. Each returns true iff it produced a result and set the destination
// variable; false means a runtime check (e.g., non-numeric variable value,
// or out-of-range index) failed and the caller should fall back to the
// regular dispatch path.
bool try_execute_pre_parsed_math(Interpreter& interp, const PreParsedMath& pp, const std::vector<Argument>& args);
bool try_execute_pre_parsed_substring(Interpreter& interp, const PreParsedSubstring& pp);

void register_message_builtins(Interpreter& interp);
void register_variable_builtins(Interpreter& interp);
void register_list_builtins(Interpreter& interp);
void register_target_builtins(Interpreter& interp);
void register_project_builtins(Interpreter& interp);
void register_file_builtins(Interpreter& interp);
void register_find_package_builtins(Interpreter& interp);
void register_find_commands_builtins(Interpreter& interp);
void register_math_builtins(Interpreter& interp);
void register_string_builtins(Interpreter& interp);
void register_process_builtins(Interpreter& interp);
void register_property_builtins(Interpreter& interp);
void register_path_builtins(Interpreter& interp);
void register_install_builtins(Interpreter& interp);
void register_source_properties_builtins(Interpreter& interp);
void register_system_info_builtins(Interpreter& interp);

} // namespace kiln
