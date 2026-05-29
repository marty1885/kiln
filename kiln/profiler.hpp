#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <optional>
#include <mutex>
#include <chrono>
#include <atomic>
#include <cstdint>

namespace kiln {

// Global flag - checked inline at every instrumentation point.
// When false (default), ProfileScope is essentially free (one well-predicted branch).
inline std::atomic<bool> g_profiling_enabled{false};

class Profiler {
public:
    static Profiler& instance();

    // Start recording. Resets the epoch so all timestamps are relative to this call.
    void enable();

    using Args = std::optional<std::map<std::string, std::string>>;

    // Record a complete duration event ("X" phase in Chrome trace format).
    void add_complete(std::string name, std::string cat, int64_t start_us, int64_t duration_us, Args args = std::nullopt);

    // Record a complete duration event with explicit thread ID.
    void add_complete(std::string name, std::string cat, int64_t start_us, int64_t duration_us, int64_t tid, Args args = std::nullopt);

    // Get current timestamp in microseconds since profiler epoch.
    int64_t now_us() const;

    // Get a sequential thread ID for the calling thread.
    // Main thread gets 0, subsequent threads get 1, 2, ...
    static int64_t get_tid();

    // Write the collected trace to a Chrome trace event JSON file.
    void write(const std::string& path);

private:
    Profiler();

    struct Event {
        std::string name;
        std::string cat;
        int64_t ts;
        int64_t dur;
        int64_t tid;
        Args args = std::nullopt;
    };

    std::chrono::steady_clock::time_point epoch_;
    std::vector<Event> events_;
    std::mutex mutex_;
};

// RAII scope helper. Records a complete duration event from construction to
// destruction (or explicit stop() call). When g_profiling_enabled is false,
// construction and destruction each cost one well-predicted branch.
class ProfileScope {
public:
    ProfileScope(std::string_view name, std::string_view cat);
    ~ProfileScope();

    // End the measurement early. The event is recorded immediately.
    // The destructor becomes a no-op after this call.
    void stop();

    ProfileScope(const ProfileScope&) = delete;
    ProfileScope& operator=(const ProfileScope&) = delete;

private:
    std::string name_;
    std::string cat_;
    int64_t start_us_ = 0;
    bool active_ = false;
};

} // namespace kiln
