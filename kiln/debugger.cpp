#include "debugger.hpp"
#include "interperter.hpp"
#include "parse_number.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <atomic>

namespace kiln {

// Signal flag for Ctrl+C → drop to debugger
static std::atomic<bool> g_sigint_received{false};

static void sigint_handler(int) {
    g_sigint_received.store(true, std::memory_order_relaxed);
}

// Default input function using std::getline (no line editing)
static std::optional<std::string> default_input(const char* prompt, int& key_type) {
    key_type = 0;
    std::cerr << prompt;
    std::string line;
    if (!std::getline(std::cin, line)) {
        key_type = 2; // EOF
        return std::nullopt;
    }
    return line;
}

// Helper to serialize a VariableReference back to ${...} syntax
std::string serialize_variable_reference(const VariableReference& ref) {
    std::string result = "$";
    if (!ref.namespace_prefix.empty()) { result += ref.namespace_prefix; }
    result += "{";
    for (const auto& part : ref.name_parts) {
        if (std::holds_alternative<std::string>(part)) {
            result += std::get<std::string>(part);
        } else {
            result += serialize_variable_reference(std::get<VariableReference>(part));
        }
    }
    result += "}";
    return result;
}

// Reconstruct raw argument text preserving ${VAR} syntax
std::string serialize_argument(const Argument& arg) {
    std::string result;
    if (arg.quoted) result += '"';
    for (const auto& part : arg.parts) {
        if (std::holds_alternative<std::string>(part)) {
            result += std::get<std::string>(part);
        } else {
            result += serialize_variable_reference(std::get<VariableReference>(part));
        }
    }
    if (arg.quoted) result += '"';
    return result;
}

Debugger::Debugger(Interpreter& interp) : interp_(interp), input_fn_(default_input) {
    // Install SIGINT handler so Ctrl+C drops to debugger instead of killing
    struct sigaction sa{};
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, &old_sigint_action_);
}

Debugger::~Debugger() {
    // Restore original SIGINT handler
    sigaction(SIGINT, &old_sigint_action_, nullptr);
}

void Debugger::on_command(const std::string& file, size_t row, size_t col, const std::string& identifier,
                          const std::vector<std::string>& expanded_args, const std::vector<Argument>& raw_args) {
    // Save current command context so on_message/on_variable_access/on_fatal_error
    // can break at the right location (the command that caused the event, not the next one).
    current_file_ = file;
    current_row_ = row;
    current_cmd_ = identifier;
    current_raw_args_ = &raw_args;

    // Always print trace if trace mode is on
    if (g_trace_enabled) { print_trace(file, row, identifier, expanded_args, raw_args); }

    // Check for Ctrl+C → drop to debugger
    if (g_sigint_received.exchange(false, std::memory_order_relaxed)) {
        std::cerr << "\nInterrupted.\n";
        action_ = Action::STEP;
    }

    // Check if we should break into the interactive debugger
    if (g_debug_enabled) {
        bool should_stop = false;

        switch (action_) {
        case Action::STEP:
            should_stop = true;
            break;
        case Action::NEXT:
            should_stop = (call_depth_ <= next_depth_);
            break;
        case Action::CONTINUE:
            should_stop = should_break(file, row, identifier);
            break;
        }

        if (should_stop) { interactive_loop(file, row, identifier, raw_args); }
    }
}

void Debugger::on_message(const std::string& content) {
    if (break_on_message_ && g_debug_enabled && current_raw_args_) {
        if (content.find(*break_on_message_) != std::string::npos) {
            std::cerr << "(kiln) Break on message matching \"" << *break_on_message_ << "\": " << content << "\n";
            interactive_loop(current_file_, current_row_, current_cmd_, *current_raw_args_);
        }
    }
}

void Debugger::on_variable_access(const std::string& var_name, const std::string& access_type, const std::string& value,
                                  const std::string& current_file) {
    if (!g_debug_enabled || !current_raw_args_) return;

    // Check if any breakpoints match this variable
    for (const auto& bp : breakpoints_) {
        if (bp.enabled && bp.type == Breakpoint::Type::VARIABLE && bp.variable == var_name) {
            std::cerr << "(kiln) Variable breakpoint " << bp.id << " hit: " << var_name << " " << access_type;
            if (!value.empty()) { std::cerr << ", value=\"" << value << "\""; }
            std::cerr << " in " << current_file << "\n";
            interactive_loop(current_file_, current_row_, current_cmd_, *current_raw_args_);
            break;
        }
    }
}

void Debugger::on_fatal_error(const std::string& message) {
    if (!g_debug_enabled || !current_raw_args_) return;

    std::cerr << "(kiln) FATAL_ERROR: " << message << "\n";
    interactive_loop(current_file_, current_row_, current_cmd_, *current_raw_args_);
}

void Debugger::push_call_depth() {
    ++call_depth_;
}
void Debugger::pop_call_depth() {
    --call_depth_;
}

int Debugger::add_location_breakpoint(const std::string& file, size_t line) {
    Breakpoint bp;
    bp.id = next_breakpoint_id_++;
    bp.type = Breakpoint::Type::LOCATION;
    bp.file = file;
    bp.line = line;
    breakpoints_.push_back(bp);
    return bp.id;
}

int Debugger::add_command_breakpoint(const std::string& command) {
    Breakpoint bp;
    bp.id = next_breakpoint_id_++;
    bp.type = Breakpoint::Type::COMMAND;
    bp.command = kiln::to_lower(command);
    breakpoints_.push_back(bp);
    return bp.id;
}

int Debugger::add_variable_breakpoint(const std::string& variable) {
    Breakpoint bp;
    bp.id = next_breakpoint_id_++;
    bp.type = Breakpoint::Type::VARIABLE;
    bp.variable = variable;
    breakpoints_.push_back(bp);
    return bp.id;
}

void Debugger::delete_breakpoint(int id) {
    breakpoints_.erase(std::remove_if(breakpoints_.begin(), breakpoints_.end(), [id](const Breakpoint& bp) { return bp.id == id; }),
                       breakpoints_.end());
}

void Debugger::print_trace(const std::string& file, size_t row, const std::string& identifier,
                           const std::vector<std::string>& expanded_args, const std::vector<Argument>& raw_args) {
    // Format: file(line): command(args)
    std::cerr << file << "(" << row << "): " << identifier << "(";

    if (g_trace_expand) {
        // --trace-expand: show expanded (evaluated) arguments, semicolon-separated
        for (size_t i = 0; i < expanded_args.size(); ++i) {
            if (i > 0) std::cerr << ";";
            std::cerr << expanded_args[i];
        }
    } else {
        // --trace: show raw arguments preserving ${VAR} references
        for (size_t i = 0; i < raw_args.size(); ++i) {
            if (i > 0) std::cerr << " ";
            std::cerr << serialize_argument(raw_args[i]);
        }
    }

    std::cerr << ")\n";
}

void Debugger::interactive_loop(const std::string& file, size_t row, const std::string& identifier, const std::vector<Argument>& raw_args) {
    // Context (current_file_, current_row_, current_cmd_) is set by on_command()
    // or by on_fatal_error() before entering this loop.
    selected_frame_ = 0;

    // Show current position
    std::cerr << "(kiln) " << file << ":" << row << "  " << identifier << "(";
    for (size_t i = 0; i < raw_args.size(); ++i) {
        if (i > 0) std::cerr << " ";
        std::cerr << serialize_argument(raw_args[i]);
    }
    std::cerr << ")\n";

    while (true) {
        int key_type = 0;
        auto result = input_fn_("(kiln) > ", key_type);
        if (!result) {
            if (key_type == 1) {
                // Ctrl+C in the prompt — clear and re-prompt
                g_sigint_received.store(false, std::memory_order_relaxed);
                continue;
            }
            // EOF (Ctrl+D)
            std::cerr << "Quit.\n";
            std::exit(0);
        }

        std::string input = *result;

        // Trim whitespace
        input = std::string(kiln::strip(input));

        if (input.empty()) continue;

        if (execute_debugger_command(input)) {
            break; // Command wants to resume execution
        }
    }
}

bool Debugger::execute_debugger_command(const std::string& input) {
    std::istringstream iss(input);
    std::string cmd;
    iss >> cmd;

    // Check that no unexpected trailing arguments remain
    auto expect_no_more_args = [&](const char* usage) -> bool {
        std::string extra;
        if (iss >> extra) {
            std::cerr << "Unexpected argument: " << extra << "\n";
            std::cerr << "Usage: " << usage << "\n";
            return false;
        }
        return true;
    };

    // Parse an integer argument strictly (rejects "3abc", trailing garbage, etc.)
    auto parse_int = [](const std::string& s) -> std::optional<int> { return parse_number<int>(s); };

    if (cmd == "continue" || cmd == "c") {
        if (!expect_no_more_args("continue")) return false;
        action_ = Action::CONTINUE;
        return true;
    }

    if (cmd == "step" || cmd == "s") {
        if (!expect_no_more_args("step")) return false;
        action_ = Action::STEP;
        return true;
    }

    if (cmd == "next" || cmd == "n") {
        if (!expect_no_more_args("next")) return false;
        action_ = Action::NEXT;
        next_depth_ = call_depth_;
        return true;
    }

    if (cmd == "quit" || cmd == "q") { std::exit(0); }

    if (cmd == "print" || cmd == "p") {
        std::string var_name;
        iss >> var_name;
        if (var_name.empty()) {
            std::cerr << "Usage: print <variable_name>\n";
            return false;
        }
        if (!expect_no_more_args("print <variable_name>")) return false;
        auto val = interp_.get_optional_variable(var_name);
        if (val.has_value()) {
            std::cerr << var_name << " = \"" << *val << "\"\n";
        } else {
            std::cerr << var_name << " is not defined\n";
        }
        return false;
    }

    if (cmd == "backtrace" || cmd == "bt") {
        if (!expect_no_more_args("backtrace")) return false;
        show_backtrace();
        return false;
    }

    if (cmd == "list" || cmd == "l") {
        if (!expect_no_more_args("list")) return false;
        auto [f, r] = selected_file_row();
        show_source_context(f, r);
        return false;
    }

    if (cmd == "frame" || cmd == "f") {
        std::string arg;
        iss >> arg;
        if (arg.empty()) {
            show_selected_frame();
            return false;
        }
        auto n = parse_int(arg);
        if (!n) {
            std::cerr << "Invalid frame number: " << arg << "\n";
            std::cerr << "Usage: frame [N]\n";
            return false;
        }
        if (!expect_no_more_args("frame [N]")) return false;
        if (!select_frame(*n)) { std::cerr << "No frame " << *n << ".\n"; }
        return false;
    }

    if (cmd == "up") {
        int count = 1;
        std::string arg;
        if (iss >> arg) {
            auto n = parse_int(arg);
            if (!n) {
                std::cerr << "Invalid count: " << arg << "\n";
                std::cerr << "Usage: up [N]\n";
                return false;
            }
            count = *n;
        }
        if (!expect_no_more_args("up [N]")) return false;
        auto& stack = interp_.get_trace_stack();
        int max_frame = static_cast<int>(stack.size()) - 1;
        int target = selected_frame_ + count;
        if (target > max_frame) {
            std::cerr << "Already at outermost frame.\n";
        } else {
            select_frame(target);
        }
        return false;
    }

    if (cmd == "down") {
        int count = 1;
        std::string arg;
        if (iss >> arg) {
            auto n = parse_int(arg);
            if (!n) {
                std::cerr << "Invalid count: " << arg << "\n";
                std::cerr << "Usage: down [N]\n";
                return false;
            }
            count = *n;
        }
        if (!expect_no_more_args("down [N]")) return false;
        int target = selected_frame_ - count;
        if (target < 0) {
            std::cerr << "Already at innermost frame.\n";
        } else {
            select_frame(target);
        }
        return false;
    }

    if (cmd == "info") {
        std::string what;
        iss >> what;
        if (what == "variables") {
            if (!expect_no_more_args("info variables")) return false;
            show_variables();
        } else if (what == "breakpoints") {
            if (!expect_no_more_args("info breakpoints")) return false;
            show_breakpoints();
        } else {
            std::cerr << "Usage: info variables | info breakpoints\n";
        }
        return false;
    }

    if (cmd == "break" || cmd == "b") {
        std::string arg;
        iss >> arg;
        if (arg.empty()) {
            std::cerr << "Usage: break <line> | break <file>:<line> | break <command_name>\n";
            return false;
        }
        if (!expect_no_more_args("break <line> | break <file>:<line> | break <command_name>")) return false;

        // Check if it's a plain line number (break on current file)
        bool is_plain_number = std::all_of(arg.begin(), arg.end(), ::isdigit);
        if (is_plain_number) {
            size_t line = parse_number<size_t>(arg).value_or(0);
            warn_if_non_executable(current_file_, line);
            int id = add_location_breakpoint(current_file_, line);
            std::cerr << "Breakpoint " << id << " at " << std::filesystem::path(current_file_).filename().string() << ":" << line << "\n";
            return false;
        }

        // Check if it's file:line format
        auto colon = arg.rfind(':');
        if (colon != std::string::npos && colon > 0) {
            std::string file = arg.substr(0, colon);
            std::string line_str = arg.substr(colon + 1);
            auto line = parse_int(line_str);
            if (line && *line > 0) {
                warn_if_non_executable(file, *line);
                int id = add_location_breakpoint(file, *line);
                std::cerr << "Breakpoint " << id << " at " << file << ":" << *line << "\n";
                return false;
            }
        }

        // Treat as command breakpoint
        int id = add_command_breakpoint(arg);
        std::cerr << "Breakpoint " << id << " on command \"" << arg << "\"\n";
        return false;
    }

    if (cmd == "break-on-message" || cmd == "bm") {
        // Read rest of line as the pattern (may contain spaces)
        std::string pattern;
        std::getline(iss >> std::ws, pattern);
        if (pattern.empty()) {
            if (break_on_message_) {
                std::cerr << "Current break-on-message pattern: \"" << *break_on_message_ << "\"\n";
                std::cerr << "Use 'bm clear' to remove.\n";
            } else {
                std::cerr << "No break-on-message pattern set.\n";
                std::cerr << "Usage: break-on-message <pattern>\n";
            }
            return false;
        }
        if (pattern == "clear" || pattern == "none") {
            break_on_message_.reset();
            std::cerr << "Cleared break-on-message pattern.\n";
        } else {
            break_on_message_ = pattern;
            std::cerr << "Will break on messages matching \"" << pattern << "\"\n";
        }
        return false;
    }

    if (cmd == "watch" || cmd == "w") {
        std::string var_name;
        iss >> var_name;
        if (var_name.empty()) {
            std::cerr << "Usage: watch <variable_name>\n";
            return false;
        }
        if (!expect_no_more_args("watch <variable_name>")) return false;
        int id = add_variable_breakpoint(var_name);
        std::cerr << "Watchpoint " << id << " on variable \"" << var_name << "\"\n";
        return false;
    }

    if (cmd == "delete" || cmd == "d") {
        std::string id_str;
        iss >> id_str;
        if (id_str.empty()) {
            std::cerr << "Usage: delete <breakpoint_id>\n";
            return false;
        }
        if (!expect_no_more_args("delete <breakpoint_id>")) return false;
        auto id = parse_int(id_str);
        if (!id) {
            std::cerr << "Invalid breakpoint id: " << id_str << "\n";
            return false;
        }
        delete_breakpoint(*id);
        std::cerr << "Deleted breakpoint " << *id << "\n";
        return false;
    }

    if (cmd == "help" || cmd == "h") {
        std::cerr << "Commands:\n"
                     "  break <line>         (b) Set breakpoint at line in current file\n"
                     "  break <file>:<line>  (b) Set location breakpoint\n"
                     "  break <command>      (b) Break on command name\n"
                     "  continue             (c) Run until next breakpoint\n"
                     "  step                 (s) Step to next command\n"
                     "  next                 (n) Step over (stay at current call depth)\n"
                     "  print <var>          (p) Print variable value\n"
                     "  backtrace            (bt) Show call stack\n"
                     "  list                 (l) Show source around current line/frame\n"
                     "  frame [N]            (f) Select stack frame N (show current if no arg)\n"
                     "  up [N]                   Move up N stack frames (default: 1)\n"
                     "  down [N]                 Move down N stack frames (default: 1)\n"
                     "  info variables           List all visible variables\n"
                     "  info breakpoints         List all breakpoints\n"
                     "  break-on-message <p> (bm) Break when message matches pattern\n"
                     "  watch <var>          (w) Break when variable changes\n"
                     "  delete <n>           (d) Delete breakpoint by ID\n"
                     "  quit                 (q) Exit kiln\n"
                     "  help                 (h) Show this help\n";
        return false;
    }

    std::cerr << "Unknown command: " << cmd << ". Type 'help' for available commands.\n";
    return false;
}

bool Debugger::should_break(const std::string& file, size_t row, const std::string& identifier) const {
    std::string lower_id = kiln::to_lower(identifier);

    for (const auto& bp : breakpoints_) {
        if (!bp.enabled) continue;

        switch (bp.type) {
        case Breakpoint::Type::LOCATION:
            // Substring match on file, exact match on line (row is 1-based from parser)
            if (row == bp.line && file.find(bp.file) != std::string::npos) { return true; }
            break;
        case Breakpoint::Type::COMMAND:
            if (lower_id == bp.command) { return true; }
            break;
        case Breakpoint::Type::VARIABLE:
            // Variable breakpoints are handled in on_variable_access
            break;
        }
    }
    return false;
}

std::pair<std::string, size_t> Debugger::selected_file_row() const {
    auto& stack = interp_.get_trace_stack();
    int idx = static_cast<int>(stack.size()) - 1 - selected_frame_;
    if (idx < 0 || idx >= static_cast<int>(stack.size())) { return {current_file_, current_row_}; }
    return {stack[idx].file ? *stack[idx].file : std::string(), stack[idx].row};
}

bool Debugger::select_frame(int n) {
    auto& stack = interp_.get_trace_stack();
    if (n < 0 || n >= static_cast<int>(stack.size())) { return false; }
    selected_frame_ = n;
    show_selected_frame();
    return true;
}

void Debugger::show_selected_frame() {
    auto& stack = interp_.get_trace_stack();
    int idx = static_cast<int>(stack.size()) - 1 - selected_frame_;
    auto [file, row] = selected_file_row();
    std::cerr << "#" << selected_frame_ << "  " << file << ":" << row << "  " << stack[idx].command << "\n";
    show_source_context(file, row);
}

void Debugger::warn_if_non_executable(const std::string& file, size_t line) {
    std::ifstream in(file);
    if (!in) return;

    std::string content;
    size_t cur = 1;
    while (std::getline(in, content)) {
        if (cur == line) {
            // Trim leading whitespace
            auto trimmed = kiln::lstrip(content);
            if (trimmed.empty()) {
                std::cerr << "Warning: line " << line << " is empty (breakpoint will never hit)\n";
            } else if (trimmed[0] == '#') {
                std::cerr << "Warning: line " << line << " is a comment (breakpoint will never hit)\n";
            }
            return;
        }
        ++cur;
    }
    std::cerr << "Warning: line " << line << " is past end of file\n";
}

void Debugger::show_source_context(const std::string& file, size_t row) {
    std::ifstream in(file);
    if (!in) {
        std::cerr << "Cannot read: " << file << "\n";
        return;
    }

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) { lines.push_back(line); }

    // Show +-5 lines around current position (row is 1-based from parser)
    int center = static_cast<int>(row) - 1; // Convert to 0-based index
    int start = std::max(0, center - 5);
    int end_idx = std::min(static_cast<int>(lines.size()), center + 6);

    for (int i = start; i < end_idx; ++i) {
        char marker = (i == center) ? '>' : ' ';
        std::cerr << marker << " " << std::setw(5) << (i + 1) << "  " << lines[i] << "\n";
    }
}

void Debugger::show_backtrace() {
    auto& stack = interp_.get_trace_stack();

    for (int i = static_cast<int>(stack.size()) - 1; i >= 0; --i) {
        int frame_num = static_cast<int>(stack.size()) - 1 - i;
        const auto& frame = stack[i];
        char marker = (frame_num == selected_frame_) ? '>' : ' ';
        std::cerr << marker << " #" << frame_num << "  " << (frame.file ? *frame.file : std::string()) << ":" << frame.row << "  "
                  << frame.command << "\n";
    }
}

void Debugger::show_variables() {
    auto all = interp_.get_variables().snapshot();

    if (all.empty()) {
        std::cerr << "No variables defined.\n";
        return;
    }

    // Sort by name for consistent output
    std::vector<std::pair<std::string, std::string>> sorted(all.begin(), all.end());
    std::sort(sorted.begin(), sorted.end());

    for (const auto& [name, value] : sorted) { std::cerr << name << " = \"" << value << "\"\n"; }
}

void Debugger::show_breakpoints() {
    if (breakpoints_.empty()) {
        std::cerr << "No breakpoints.\n";
        return;
    }

    for (const auto& bp : breakpoints_) {
        std::cerr << bp.id << ": ";
        switch (bp.type) {
        case Breakpoint::Type::LOCATION:
            std::cerr << "break at " << bp.file << ":" << bp.line;
            break;
        case Breakpoint::Type::COMMAND:
            std::cerr << "break on command \"" << bp.command << "\"";
            break;
        case Breakpoint::Type::VARIABLE:
            std::cerr << "watch on variable \"" << bp.variable << "\"";
            break;
        }
        if (!bp.enabled) std::cerr << " [disabled]";
        std::cerr << "\n";
    }
}

// --- DebugController ---

DebugController::DebugController(const DebugOptions& opts) : opts_(opts) {
    // Set global flags once at construction
    if (opts_.trace || opts_.trace_expand) {
        g_trace_enabled = true;
        g_trace_expand = opts_.trace_expand;
    }
    if (opts_.debugger || !opts_.break_on_message.empty()) { g_debug_enabled = true; }
}

void DebugController::attach(Interpreter& interp) {
    if (!g_trace_enabled && !g_debug_enabled) return;

    auto dbg = std::make_unique<Debugger>(interp);
    if (opts_.debugger) dbg->set_step_mode();
    if (!opts_.break_on_message.empty()) { dbg->set_break_on_message(opts_.break_on_message); }
    if (input_fn_) { dbg->set_input_function(input_fn_); }
    interp.set_debugger(std::move(dbg));
}

} // namespace kiln
