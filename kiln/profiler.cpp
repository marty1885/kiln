#include "profiler.hpp"
#include <glaze/glaze.hpp>
#include <fstream>
#include <filesystem>

namespace kiln {

// Chrome trace event JSON structures (Glaze-serializable, not exposed in header)
struct TraceEventJson {
    std::string name;
    std::string cat;
    std::string ph;
    int64_t ts;
    int64_t dur;
    int64_t pid;
    int64_t tid;
    std::optional<std::map<std::string, std::string>> args;
};

struct TraceFileJson {
    std::vector<TraceEventJson> traceEvents;
};

Profiler::Profiler() : epoch_(std::chrono::steady_clock::now()) {}

Profiler& Profiler::instance() {
    static Profiler inst;
    return inst;
}

void Profiler::enable() {
    epoch_ = std::chrono::steady_clock::now();
    g_profiling_enabled.store(true, std::memory_order_release);
}

int64_t Profiler::now_us() const {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(now - epoch_).count();
}

int64_t Profiler::get_tid() {
    static std::atomic<int64_t> counter{0};
    thread_local int64_t id = counter.fetch_add(1, std::memory_order_relaxed);
    return id;
}

void Profiler::add_complete(std::string name, std::string cat, int64_t start_us, int64_t duration_us, Profiler::Args args) {
    add_complete(std::move(name), std::move(cat), start_us, duration_us, get_tid(), std::move(args));
}

void Profiler::add_complete(std::string name, std::string cat, int64_t start_us, int64_t duration_us, int64_t tid, Profiler::Args args) {
    std::lock_guard<std::mutex> lock(mutex_);
    events_.push_back({std::move(name), std::move(cat), start_us, duration_us, tid, std::move(args)});
}

void Profiler::write(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);

    TraceFileJson trace;
    trace.traceEvents.reserve(events_.size());

    for (const auto& e : events_) {
        trace.traceEvents.push_back(
            {.name = e.name, .cat = e.cat, .ph = "X", .ts = e.ts, .dur = e.dur, .pid = 1, .tid = e.tid, .args = e.args});
    }

    std::string json;
    auto ec = glz::write_json(trace, json);
    if (ec) return;

    std::error_code fs_ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), fs_ec);

    std::ofstream file(path);
    if (file) { file << json; }
}

// ProfileScope implementation

ProfileScope::ProfileScope(std::string_view name, std::string_view cat) : active_(g_profiling_enabled.load(std::memory_order_acquire)) {
    if (active_) {
        name_ = name;
        cat_ = cat;
        start_us_ = Profiler::instance().now_us();
    }
}

ProfileScope::~ProfileScope() {
    stop();
}

void ProfileScope::stop() {
    if (!active_) return;
    active_ = false;
    auto dur = Profiler::instance().now_us() - start_us_;
    Profiler::instance().add_complete(std::move(name_), std::move(cat_), start_us_, dur);
}

} // namespace kiln
