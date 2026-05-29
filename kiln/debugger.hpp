#pragma once

#include "cmake-language.hpp"
#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <utility>
#include <csignal>

namespace kiln {

class Interpreter;

// Global flags for trace/debug modes. The interpreter is single-threaded,
// so plain bools are fine (no atomics needed).
inline bool g_debug_enabled = false;
inline bool g_trace_enabled = false;
inline bool g_trace_expand = false;

// Reconstruct the raw text of an Argument (with ${VAR} syntax preserved)
std::string serialize_argument(const Argument& arg);
std::string serialize_variable_reference(const VariableReference& ref);

struct Breakpoint {
    int id;
    enum class Type { LOCATION, COMMAND, VARIABLE };
    Type type;
    std::string file;     // For LOCATION: filename (substring match)
    size_t line = 0;      // For LOCATION: line number (1-based)
    std::string command;  // For COMMAND: command name
    std::string variable; // For VARIABLE: variable name
    bool enabled = true;
};

// Function type for reading a line from the user.
// Returns the input string, or std::nullopt on EOF/error.
// key_type is set to 1 for Ctrl+C, 2 for Ctrl+D, 0 otherwise.
using InputFunction = std::function<std::optional<std::string>(const char* prompt, int& key_type)>;

class Debugger {
public:
    explicit Debugger(Interpreter& interp);
    ~Debugger();

    // Called from execute_command() on every command
    void on_command(const std::string& file, size_t row, size_t col, const std::string& identifier,
                    const std::vector<std::string>& expanded_args, const std::vector<Argument>& raw_args);

    // Called from print_message() to support --break-on-message
    void on_message(const std::string& content);

    // Called from set_variable/get_optional_variable/unset_variable for watches
    void on_variable_access(const std::string& var_name, const std::string& access_type, const std::string& value,
                            const std::string& current_file);

    // Called from message(FATAL_ERROR) to break before dying
    void on_fatal_error(const std::string& message);

    // Call depth tracking for "next" stepping
    void push_call_depth();
    void pop_call_depth();

    // Set the message pattern for --break-on-message
    void set_break_on_message(const std::string& pattern) { break_on_message_ = pattern; }

    // Set step mode (for --debug, breaks on first command)
    void set_step_mode() { action_ = Action::STEP; }

    // Override the input function (default uses std::getline)
    void set_input_function(InputFunction fn) { input_fn_ = std::move(fn); }

    // Breakpoint management
    int add_location_breakpoint(const std::string& file, size_t line);
    int add_command_breakpoint(const std::string& command);
    int add_variable_breakpoint(const std::string& variable);
    void delete_breakpoint(int id);
    const std::vector<Breakpoint>& get_breakpoints() const { return breakpoints_; }

private:
    enum class Action { STEP, NEXT, CONTINUE };

    void print_trace(const std::string& file, size_t row, const std::string& identifier, const std::vector<std::string>& expanded_args,
                     const std::vector<Argument>& raw_args);

    void interactive_loop(const std::string& file, size_t row, const std::string& identifier, const std::vector<Argument>& raw_args);

    bool should_break(const std::string& file, size_t row, const std::string& identifier) const;

    // Parse a debugger command and execute it
    bool execute_debugger_command(const std::string& input);

    void show_source_context(const std::string& file, size_t row);
    void show_backtrace();
    void show_variables();
    void show_breakpoints();
    void warn_if_non_executable(const std::string& file, size_t line);

    // Frame navigation
    std::pair<std::string, size_t> selected_file_row() const;
    bool select_frame(int n);
    void show_selected_frame();

    Interpreter& interp_;
    Action action_ = Action::CONTINUE; // Caller sets STEP via set_step_mode() for --debug
    int call_depth_ = 0;
    int next_depth_ = 0; // Depth recorded when "next" was issued
    std::vector<Breakpoint> breakpoints_;
    int next_breakpoint_id_ = 1;
    std::optional<std::string> break_on_message_;
    std::string current_file_;                                // Set on entering on_command / interactive_loop
    size_t current_row_ = 0;                                  // Set on entering on_command / interactive_loop
    std::string current_cmd_;                                 // Set on entering on_command / interactive_loop
    const std::vector<Argument>* current_raw_args_ = nullptr; // Valid during on_command scope
    int selected_frame_ = 0;                                  // 0 = current command, 1..N = callers
    struct sigaction old_sigint_action_{};                    // Saved for restoration in destructor
    InputFunction input_fn_;                                  // Pluggable input (default: std::getline)
};

// Owns debug/trace configuration and attaches a Debugger to any Interpreter.
// Centralizes all debug setup so callers just call attach().
struct DebugOptions {
    bool trace = false;
    bool trace_expand = false;
    bool debugger = false;
    std::string break_on_message;

    bool any_enabled() const { return trace || trace_expand || debugger || !break_on_message.empty(); }
};

class DebugController {
public:
    explicit DebugController(const DebugOptions& opts);

    // Set a custom input function for the debugger prompt (e.g. linenoise).
    // Must be called before attach().
    void set_input_function(InputFunction fn) { input_fn_ = std::move(fn); }

    // Set global flags and attach a configured Debugger to the interpreter.
    // Safe to call multiple times for different interpreters.
    void attach(Interpreter& interp);

private:
    DebugOptions opts_;
    InputFunction input_fn_;
};

} // namespace kiln
