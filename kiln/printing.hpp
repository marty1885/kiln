#pragma once

#include "cmake-language.hpp"
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <ostream>
#include <utility>
#include <functional>

namespace kiln {

// ANSI escape codes for colors
namespace colors {
inline constexpr std::string_view RESET = "\033[0m";
inline constexpr std::string_view RED = "\033[31m";
inline constexpr std::string_view BRIGHT_RED = "\033[91m";
inline constexpr std::string_view BOLD_RED = "\033[1;31m";
inline constexpr std::string_view YELLOW = "\033[33m";
inline constexpr std::string_view BOLD_YELLOW = "\033[1;33m";
inline constexpr std::string_view GREEN = "\033[32m";
inline constexpr std::string_view CYAN = "\033[36m";
inline constexpr std::string_view BOLD_CYAN = "\033[1;36m";
inline constexpr std::string_view WHITE = "\033[37m";
inline constexpr std::string_view DIM = "\033[2m";
inline constexpr std::string_view DIM_CYAN = "\033[2;36m";
inline constexpr std::string_view MAGENTA = "\033[35m";
inline constexpr std::string_view BOLD_BLUE = "\033[1;34m";
inline constexpr std::string_view BOLD_GREEN = "\033[1;32m";
inline constexpr std::string_view BOLD_MAGENTA = "\033[1;35m";
} // namespace colors

// Check if an output stream is a terminal (cached, one isatty() call per fd)
bool is_color_enabled(std::ostream& os);

// Return color code if stream is a tty, empty string_view otherwise
inline std::string_view c(std::ostream& os, std::string_view code) {
    return is_color_enabled(os) ? code : std::string_view{};
}

// Overload for forced color output (e.g., child interpreters writing to buffers)
inline std::string_view c(bool force_color, std::string_view code) {
    return force_color ? code : std::string_view{};
}

// Print a CMake-style message (e.g. [STATUS], [WARNING], etc.)
// force_color: if true, output colors even when os is not a TTY
void print_message(std::ostream& os, std::string_view mode, std::string_view msg, std::string_view indent = "", bool force_color = false);

// Right-alignment width for build-action verbs ("Compiling", "Installing", ...).
inline constexpr int action_verb_width = 12;

// Format a build-action line: right-aligned bold-green verb followed by detail.
// Example: "  Installing /usr/local/lib/libfoo.so" (no trailing newline).
std::string format_action(std::string_view verb, std::string_view detail, bool use_color);

// Format a [MODE] message line (no trailing newline). Matches print_message's layout.
std::string format_message(std::string_view mode, std::string_view msg, std::string_view indent, bool use_color);

// Write a build-action line followed by '\n'. Coloring follows is_color_enabled(os).
void print_action(std::ostream& os, std::string_view verb, std::string_view detail);

// Sink for one already-formatted line (no trailing newline). Implementations
// either write directly to a stream, or coordinate with a progress bar so
// concurrent lines don't tear against the bar redraw.
using LineSink = std::function<void(std::string_view)>;

// Build a sink that writes line + '\n' to the given stream.
LineSink ostream_sink(std::ostream& os);

// Output context for long-running commands (install, etc.) whose output may
// race with a progress bar. `sink` receives one formatted line at a time;
// `use_color` controls whether ANSI escapes are emitted in those lines.
struct OutputCtx {
    LineSink sink;
    bool use_color;
};

// Default OutputCtx: writes to std::cout, color follows is_color_enabled(std::cout).
OutputCtx stdout_output_ctx();

// LineSink-based variants used by code that may share output with a progress bar.
void print_action(const OutputCtx& out, std::string_view verb, std::string_view detail);
void print_message(const OutputCtx& out, std::string_view mode, std::string_view msg, std::string_view indent = "");

// Expand tabs to spaces (4-column tab stops) and return mapping from source column to visual column
std::pair<std::string, std::vector<size_t>> expand_tabs(std::string_view line, size_t tab_width = 4);

enum class DiagnosticSeverity { Error, Warning, Note };

// Print a Rust-style diagnostic with source context
void print_diagnostic(std::ostream& os, DiagnosticSeverity severity, const std::string& message, const std::string& file_path, size_t row,
                      size_t col, size_t offset, size_t length, const std::vector<CallLocation>& backtrace = {},
                      const std::optional<std::string>& source_content = std::nullopt, const std::string& note = "");

} // namespace kiln
