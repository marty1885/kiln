#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <mutex>
#include <chrono>
#include <atomic>

namespace kiln {

// Terminal progress bar for build output.
//
// On TTY: draws a transient progress line that updates in-place.
// On non-TTY: no-op (caller handles [N/M] prefix on permanent lines instead).
//
// Thread-safe: all methods can be called from any thread.
class ProgressBar {
public:
    explicit ProgressBar(int total_tasks, bool is_tty);

    // Atomically: erase bar, print permanent line, redraw bar.
    // Single write to stdout prevents visible flicker.
    void print_line(const std::string& line);

    // Erase the progress bar. Caller must flush stdout before writing
    // to a different stream (e.g. stderr).
    void erase();

    // Redraw the progress bar if enough time has passed since last draw.
    void redraw();

    // Track task lifecycle
    void task_started(std::string_view filename);
    void task_finished(std::string_view filename);

    // Increment completed count (includes cached/skipped tasks)
    // Returns the new count (1-based).
    int mark_completed();

    // Bump total when new tasks are injected (e.g. module dependencies)
    void bump_total(int additional);

    // Final cleanup: erase the progress bar permanently
    void finish();

    bool is_tty() const { return is_tty_; }
    int completed() const { return completed_.load(std::memory_order_relaxed); }
    int total() const { return total_.load(std::memory_order_relaxed); }

private:
    void draw_locked();
    std::string build_erase_locked() const;
    std::string build_bar_locked();
    int get_terminal_width() const;
    std::string build_progress_line(int width) const;

    bool is_tty_;
    std::atomic<int> total_;
    std::atomic<int> completed_{0};

    mutable std::mutex mutex_;
    std::vector<std::string> active_tasks_; // filenames currently being built
    int last_rendered_width_ = 0;
    bool bar_visible_ = false;

    std::chrono::steady_clock::time_point start_time_;
    std::chrono::steady_clock::time_point last_draw_time_;
    static constexpr auto initial_delay_ = std::chrono::milliseconds(500);
    static constexpr auto redraw_interval_ = std::chrono::milliseconds(100);
};

} // namespace kiln
