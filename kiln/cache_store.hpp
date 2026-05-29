#pragma once

#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include <expected>
#include "compiler.hpp"

namespace kiln {

// Subsystem identifiers for cache namespacing
enum class CacheSubsystem {
    TryCompile,
    TryRun,
    FileListing,
    FindResult,
    ExternalCommand,   // Generic external command caching (pkg-config, etc.)
    Glob,              // file(GLOB) / file(GLOB_RECURSE) results
    CompilerDetection, // Cached compiler detection results
};

// Tracked header dependency: mtime for fast validation, hash for content-based fallback
struct HeaderDep {
    int64_t mtime = 0;
    std::string hash; // blake2b of file content
};

// Cache entry for try_compile results
struct TryCompileCacheEntry {
    bool success = false;                         // Compilation succeeded
    std::string output;                           // Compiler stdout/stderr
    std::map<std::string, HeaderDep> header_deps; // Discovered header dependencies
};

// Cache entry for try_run results (compilation + execution)
struct TryRunCacheEntry {
    bool compile_success = false;
    std::string compile_output;
    int exit_code = 0;                            // Run exit code
    std::string run_output;                       // Run stdout/stderr
    std::map<std::string, HeaderDep> header_deps; // Discovered header dependencies
};

// Cache entry for directory listings - used by glob and find commands
struct FileListingCacheEntry {
    int64_t dir_mtime = 0;            // Directory modification time
    std::vector<std::string> files;   // Regular files in directory
    std::vector<std::string> subdirs; // Subdirectories (for recursive glob)
};

// Cache entry for find_xxx results (find_file, find_library, find_path, find_program)
// Tracks all directories searched up to and including the one where we found the result
struct FindResultCacheEntry {
    std::string found_path; // Result path (empty if not found)
    // Directories searched (in order) with their state at search time
    // Key: directory path, Value: mtime (nullopt if didn't exist)
    std::vector<std::pair<std::string, std::optional<int64_t>>> searched_dirs;
};

// Cache entry for external command results
// Tracks output and all filesystem inputs that affect the result
struct ExternalCommandCacheEntry {
    std::string stdout_output; // Captured stdout
    std::string stderr_output; // Captured stderr
    int exit_code = 0;         // Exit code
    // Tracked directories with their mtimes at execution time
    // Can track package config dirs, compiler dirs, or any relevant paths
    std::map<std::string, std::optional<int64_t>> tracked_dir_mtimes;
};

// Cache entry for file(GLOB) results - caches the matched file list
struct GlobCacheEntry {
    std::string result;                        // Semicolon-separated matched files
    std::map<std::string, int64_t> dir_mtimes; // All scanned dirs with mtimes
};

// Cache entry for compiler detection results
struct CompilerDetectionCacheEntry {
    PlatformInfo info;
    std::string version_output; // for cache key validation
};

// Root structure for JSON serialization
struct CacheRoot {
    std::map<std::string, TryCompileCacheEntry> try_compile_cache;
    std::map<std::string, TryRunCacheEntry> try_run_cache;
    std::map<std::string, FileListingCacheEntry> file_listing_cache;
    std::map<std::string, FindResultCacheEntry> find_result_cache;
    std::map<std::string, ExternalCommandCacheEntry> external_command_cache;
    std::map<std::string, GlobCacheEntry> glob_cache;
    std::map<std::string, CompilerDetectionCacheEntry> compiler_detection_cache;
};

// Centralized cache store with subsystem namespacing
class CacheStore {
public:
    explicit CacheStore(const std::filesystem::path& cache_file);
    ~CacheStore() = default;

    // Load cache from disk (call at build start)
    std::expected<void, std::string> load();

    // Save cache to disk (call at build end)
    std::expected<void, std::string> save();

    // Lookup entry by signature
    template <CacheSubsystem S>
    std::optional<typename std::conditional<
        S == CacheSubsystem::TryCompile, TryCompileCacheEntry,
        typename std::conditional<
            S == CacheSubsystem::TryRun, TryRunCacheEntry,
            typename std::conditional<
                S == CacheSubsystem::FileListing, FileListingCacheEntry,
                typename std::conditional<
                    S == CacheSubsystem::FindResult, FindResultCacheEntry,
                    typename std::conditional<S == CacheSubsystem::ExternalCommand, ExternalCommandCacheEntry,
                                              typename std::conditional<S == CacheSubsystem::Glob, GlobCacheEntry,
                                                                        typename std::conditional<S == CacheSubsystem::CompilerDetection,
                                                                                                  CompilerDetectionCacheEntry, void>::
                                                                            type>::type>::type>::type>::type>::type>::type>
    lookup(const std::string& signature);

    // Insert/update entry
    template <CacheSubsystem S>
    void
    insert(const std::string& signature,
           const typename std::conditional<
               S == CacheSubsystem::TryCompile, TryCompileCacheEntry,
               typename std::conditional<
                   S == CacheSubsystem::TryRun, TryRunCacheEntry,
                   typename std::conditional<
                       S == CacheSubsystem::FileListing, FileListingCacheEntry,
                       typename std::conditional<
                           S == CacheSubsystem::FindResult, FindResultCacheEntry,
                           typename std::conditional<
                               S == CacheSubsystem::ExternalCommand, ExternalCommandCacheEntry,
                               typename std::conditional<S == CacheSubsystem::Glob, GlobCacheEntry,
                                                         typename std::conditional<S == CacheSubsystem::CompilerDetection,
                                                                                   CompilerDetectionCacheEntry, void>::type>::type>::type>::
                           type>::type>::type>::type& entry);

    // Clear all entries for a subsystem
    template <CacheSubsystem S> void clear_subsystem();

private:
    std::filesystem::path cache_file_;
    CacheRoot cache_data_;
    std::mutex mutex_; // Thread-safe access
};

// Template specializations for lookup
template <> inline std::optional<TryCompileCacheEntry> CacheStore::lookup<CacheSubsystem::TryCompile>(const std::string& signature) {
    std::lock_guard lock(mutex_);
    auto it = cache_data_.try_compile_cache.find(signature);
    if (it != cache_data_.try_compile_cache.end()) { return it->second; }
    return std::nullopt;
}

template <> inline std::optional<TryRunCacheEntry> CacheStore::lookup<CacheSubsystem::TryRun>(const std::string& signature) {
    std::lock_guard lock(mutex_);
    auto it = cache_data_.try_run_cache.find(signature);
    if (it != cache_data_.try_run_cache.end()) { return it->second; }
    return std::nullopt;
}

template <> inline std::optional<FileListingCacheEntry> CacheStore::lookup<CacheSubsystem::FileListing>(const std::string& signature) {
    std::lock_guard lock(mutex_);
    auto it = cache_data_.file_listing_cache.find(signature);
    if (it != cache_data_.file_listing_cache.end()) { return it->second; }
    return std::nullopt;
}

template <> inline std::optional<FindResultCacheEntry> CacheStore::lookup<CacheSubsystem::FindResult>(const std::string& signature) {
    std::lock_guard lock(mutex_);
    auto it = cache_data_.find_result_cache.find(signature);
    if (it != cache_data_.find_result_cache.end()) { return it->second; }
    return std::nullopt;
}

template <>
inline std::optional<ExternalCommandCacheEntry> CacheStore::lookup<CacheSubsystem::ExternalCommand>(const std::string& signature) {
    std::lock_guard lock(mutex_);
    auto it = cache_data_.external_command_cache.find(signature);
    if (it != cache_data_.external_command_cache.end()) { return it->second; }
    return std::nullopt;
}

// Template specializations for insert
template <> inline void CacheStore::insert<CacheSubsystem::TryCompile>(const std::string& signature, const TryCompileCacheEntry& entry) {
    std::lock_guard lock(mutex_);
    cache_data_.try_compile_cache[signature] = entry;
}

template <> inline void CacheStore::insert<CacheSubsystem::TryRun>(const std::string& signature, const TryRunCacheEntry& entry) {
    std::lock_guard lock(mutex_);
    cache_data_.try_run_cache[signature] = entry;
}

template <> inline void CacheStore::insert<CacheSubsystem::FileListing>(const std::string& signature, const FileListingCacheEntry& entry) {
    std::lock_guard lock(mutex_);
    cache_data_.file_listing_cache[signature] = entry;
}

template <> inline void CacheStore::insert<CacheSubsystem::FindResult>(const std::string& signature, const FindResultCacheEntry& entry) {
    std::lock_guard lock(mutex_);
    cache_data_.find_result_cache[signature] = entry;
}

template <>
inline void CacheStore::insert<CacheSubsystem::ExternalCommand>(const std::string& signature, const ExternalCommandCacheEntry& entry) {
    std::lock_guard lock(mutex_);
    cache_data_.external_command_cache[signature] = entry;
}

// Template specializations for clear_subsystem
template <> inline void CacheStore::clear_subsystem<CacheSubsystem::TryCompile>() {
    std::lock_guard lock(mutex_);
    cache_data_.try_compile_cache.clear();
}

template <> inline void CacheStore::clear_subsystem<CacheSubsystem::TryRun>() {
    std::lock_guard lock(mutex_);
    cache_data_.try_run_cache.clear();
}

template <> inline void CacheStore::clear_subsystem<CacheSubsystem::FileListing>() {
    std::lock_guard lock(mutex_);
    cache_data_.file_listing_cache.clear();
}

template <> inline void CacheStore::clear_subsystem<CacheSubsystem::FindResult>() {
    std::lock_guard lock(mutex_);
    cache_data_.find_result_cache.clear();
}

template <> inline void CacheStore::clear_subsystem<CacheSubsystem::ExternalCommand>() {
    std::lock_guard lock(mutex_);
    cache_data_.external_command_cache.clear();
}

template <> inline std::optional<GlobCacheEntry> CacheStore::lookup<CacheSubsystem::Glob>(const std::string& signature) {
    std::lock_guard lock(mutex_);
    auto it = cache_data_.glob_cache.find(signature);
    if (it != cache_data_.glob_cache.end()) { return it->second; }
    return std::nullopt;
}

template <> inline void CacheStore::insert<CacheSubsystem::Glob>(const std::string& signature, const GlobCacheEntry& entry) {
    std::lock_guard lock(mutex_);
    cache_data_.glob_cache[signature] = entry;
}

template <> inline void CacheStore::clear_subsystem<CacheSubsystem::Glob>() {
    std::lock_guard lock(mutex_);
    cache_data_.glob_cache.clear();
}

template <>
inline std::optional<CompilerDetectionCacheEntry> CacheStore::lookup<CacheSubsystem::CompilerDetection>(const std::string& signature) {
    std::lock_guard lock(mutex_);
    auto it = cache_data_.compiler_detection_cache.find(signature);
    if (it != cache_data_.compiler_detection_cache.end()) { return it->second; }
    return std::nullopt;
}

template <>
inline void CacheStore::insert<CacheSubsystem::CompilerDetection>(const std::string& signature, const CompilerDetectionCacheEntry& entry) {
    std::lock_guard lock(mutex_);
    cache_data_.compiler_detection_cache[signature] = entry;
}

template <> inline void CacheStore::clear_subsystem<CacheSubsystem::CompilerDetection>() {
    std::lock_guard lock(mutex_);
    cache_data_.compiler_detection_cache.clear();
}

} // namespace kiln
