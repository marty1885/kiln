#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <expected>
#include <optional>
#include <memory>

#include "inner/unordered_dense.h"
#include "CMakeArray.hpp"

namespace kiln {

// Transparent hash/equal for heterogeneous lookup with string_view
// Uses wyhash (via ankerl) for better distribution than std::hash
struct TransparentStringHash {
    using is_transparent = void;
    using is_avalanching = void;

    auto operator()(std::string_view sv) const noexcept -> std::uint64_t {
        return inner::ankerl::unordered_dense::detail::wyhash::hash(sv.data(), sv.size());
    }
};

struct TransparentStringEqual {
    using is_transparent = void;

    bool operator()(std::string_view lhs, std::string_view rhs) const noexcept { return lhs == rhs; }
};

/**
 * Shadow Map for efficient variable scoping with O(1) access.
 *
 * Variables are stored with depth-tagged version histories. Each variable
 * maps to a vector of (depth, value) pairs where only the topmost entry
 * is visible. This provides:
 * - O(1) variable access (no parent traversal)
 * - Automatic cleanup on scope exit
 * - Minimal allocations (only when variables are shadowed)
 * - Exception-safe RAII pattern
 */
class ShadowMap {
    struct VariableVersion {
        std::optional<std::string> value; // nullopt = tombstone (unset masking parent)
        int depth;
        // Lazy progressive list-element index. Built on first list random-access
        // op; reset on every mutation (the macros below funnel through reset_list_index()).
        // unique_ptr keeps the per-version overhead at 8 bytes for variables that are
        // never used as lists (the vast majority).
        mutable std::unique_ptr<ProgressiveListIndex> list_index;

        void reset_list_index() { list_index.reset(); }
    };

public:
    class ConstEntry;
    class Entry;

    ShadowMap() = default;

    /**
     * Returns the topmost version's progressive list index, lazily creating
     * it on first call. Returns nullptr if the variable is undefined or a
     * tombstone. Safe to call through a const ShadowMap because `list_index`
     * is a `mutable` member on `VariableVersion`. The index is automatically
     * invalidated by all mutation paths via `reset_list_index()`.
     */
    ProgressiveListIndex* const_list_index_for_read(std::string_view name) const {
        auto it = variables_.find(name);
        if (it == variables_.end() || it->second->empty()) return nullptr;
        auto& top = it->second->back();
        if (!top.value.has_value()) return nullptr;
        if (!top.list_index) { top.list_index = std::make_unique<ProgressiveListIndex>(); }
        return top.list_index.get();
    }

    /**
     * Single-lookup read. Returns nullptr if undefined/tombstone.
     */
    const std::string* try_get(std::string_view name) const {
        auto it = variables_.find(name);
        if (it == variables_.end() || it->second->empty()) return nullptr;
        auto& val = it->second->back().value;
        return val.has_value() ? &*val : nullptr;
    }

    /**
     * Cached read-only handle (8 bytes). Hash lookup at creation;
     * subsequent reads are pointer deref + back(). Sees mutations
     * immediately (points to live vector).
     */
    class ConstEntry {
        friend class ShadowMap;
        friend class Entry;
        const std::vector<VariableVersion>* versions_;
        explicit ConstEntry(const std::vector<VariableVersion>* v) : versions_(v) {}

    public:
        bool is_defined() const { return !versions_->empty() && versions_->back().value.has_value(); }
        const std::string& get() const {
            static const std::string empty;
            if (!is_defined()) return empty;
            return *versions_->back().value;
        }
    };

    /**
     * Returns a cached read-only handle. nullopt if variable has no map entry.
     * No insertion.
     */
    std::optional<ConstEntry> const_entry(std::string_view name) const {
        auto it = variables_.find(name);
        if (it == variables_.end()) return std::nullopt;
        return ConstEntry(it->second.get());
    }

    /**
     * Cached read-write handle (24 bytes). Hash lookup at creation;
     * subsequent reads/writes are pointer deref + back().
     */
    class Entry {
        friend class ShadowMap;
        ShadowMap& map_;
        std::vector<VariableVersion>* versions_;

        Entry(ShadowMap& m, std::vector<VariableVersion>* v) : map_(m), versions_(v) {}

    public:
        bool is_defined() const { return !versions_->empty() && versions_->back().value.has_value(); }
        const std::string& get() const {
            static const std::string empty;
            if (!is_defined()) return empty;
            return *versions_->back().value;
        }
        void set(const std::string& value) {
            int depth = map_.current_depth_;
            if (!versions_->empty() && versions_->back().depth == depth) {
                // Reuse existing string allocation when possible
                auto& top = versions_->back();
                top.reset_list_index();
                auto& opt = top.value;
                if (opt.has_value())
                    opt->assign(value);
                else
                    opt = value;
                return;
            }
            versions_->push_back({std::optional<std::string>(value), depth});
            if (depth > 0 && depth < static_cast<int>(map_.modified_per_depth_.size())) {
                map_.modified_per_depth_[depth].push_back(versions_);
            }
        }
        void set(std::string&& value) {
            int depth = map_.current_depth_;
            if (!versions_->empty() && versions_->back().depth == depth) {
                auto& top = versions_->back();
                top.reset_list_index();
                auto& opt = top.value;
                if (opt.has_value())
                    *opt = std::move(value);
                else
                    opt = std::move(value);
                return;
            }
            versions_->push_back({std::optional<std::string>(std::move(value)), depth});
            if (depth > 0 && depth < static_cast<int>(map_.modified_per_depth_.size())) {
                map_.modified_per_depth_[depth].push_back(versions_);
            }
        }
        void unset() {
            if (versions_->empty()) return;
            int depth = map_.current_depth_;
            if (versions_->back().depth == depth) {
                versions_->pop_back();
                if (!versions_->empty() && versions_->back().value.has_value()) {
                    versions_->push_back({std::nullopt, depth});
                    if (depth > 0 && depth < static_cast<int>(map_.modified_per_depth_.size())) {
                        map_.modified_per_depth_[depth].push_back(versions_);
                    }
                }
            } else if (versions_->back().depth < depth && versions_->back().value.has_value()) {
                versions_->push_back({std::nullopt, depth});
                if (depth > 0 && depth < static_cast<int>(map_.modified_per_depth_.size())) {
                    map_.modified_per_depth_[depth].push_back(versions_);
                }
            }
        }
        // Returns a pointer to the topmost value if it lives at current_depth and is defined,
        // so callers can mutate it in place (avoiding the read-copy-write round-trip that
        // makes list(APPEND)/string(APPEND) in a loop O(N^2)). Returns nullptr when the
        // value must be snapshot-into-current-scope first (parent depth or tombstone), in
        // which case the caller should fall back to set(get() + ...).
        std::string* mutable_at_current_depth() {
            if (versions_->empty()) return nullptr;
            auto& top = versions_->back();
            if (top.depth != map_.current_depth_) return nullptr;
            if (!top.value.has_value()) return nullptr;
            // Caller is about to mutate the string — any cached list index
            // would be invalidated by reallocation or content change.
            top.reset_list_index();
            return &*top.value;
        }

        // Returns the variable's progressive list-element index, building it
        // lazily on first access. The index lives with the value and is reset
        // by every mutation path, so it is always consistent with the value.
        // Cheaper than CMakeArrayIterator for repeated random access against
        // the same value (matmul-shaped workloads, list(GET) loops).
        ProgressiveListIndex* list_index_for_read() const {
            if (versions_->empty()) return nullptr;
            auto& top = versions_->back();
            if (!top.value.has_value()) return nullptr;
            if (!top.list_index) { top.list_index = std::make_unique<ProgressiveListIndex>(); }
            return top.list_index.get();
        }
        operator ConstEntry() const { return ConstEntry(versions_); }
    };

    /**
     * Returns a cached read-write handle. Creates map entry if needed.
     */
    Entry entry(const std::string& name) {
        auto [it, inserted] = variables_.try_emplace(name);
        if (inserted) { it->second = std::make_unique<std::vector<VariableVersion>>(); }
        return Entry(*this, it->second.get());
    }

    /**
     * Get the current value of a variable.
     * Returns empty string if variable is not defined.
     * O(1) complexity.
     */
    const std::string& get(std::string_view name) const {
        static const std::string empty;
        auto it = variables_.find(name);
        if (it == variables_.end() || it->second->empty()) { return empty; }
        // Check for tombstone (nullopt = unset)
        const auto& val = it->second->back().value;
        if (!val.has_value()) { return empty; }
        return *val;
    }

    /**
     * Set a variable at the current depth.
     * If the variable already exists at this depth, modifies it.
     * Otherwise, pushes a new version.
     * Uses transparent find() for existing variables to avoid non-transparent hash path.
     * O(1) amortized complexity.
     */
    void set(const std::string& name, const std::string& value) {
        // Fast path: transparent find for existing variables
        auto it = variables_.find(std::string_view(name));
        if (it != variables_.end()) {
            auto* versions = it->second.get();
            if (!versions->empty() && versions->back().depth == current_depth_) {
                versions->back().reset_list_index();
                versions->back().value = value;
                return;
            }
            versions->push_back({std::optional<std::string>(value), current_depth_});
            if (current_depth_ > 0 && current_depth_ < static_cast<int>(modified_per_depth_.size())) {
                modified_per_depth_[current_depth_].push_back(versions);
            }
            return;
        }
        // Slow path: new variable — must allocate key + version vector
        auto [new_it, _] = variables_.try_emplace(name, std::make_unique<std::vector<VariableVersion>>());
        auto* versions = new_it->second.get();
        versions->push_back({std::optional<std::string>(value), current_depth_});
        if (current_depth_ > 0 && current_depth_ < static_cast<int>(modified_per_depth_.size())) {
            modified_per_depth_[current_depth_].push_back(versions);
        }
    }

    /**
     * Set a variable at the parent scope (depth - 1).
     * Used for CMake's PARENT_SCOPE modifier.
     *
     * CMake semantics: PARENT_SCOPE modifies the parent scope but does NOT
     * affect the current scope's view of the variable. To achieve this, if
     * the variable is visible from the parent but has no local entry, we
     * first "snapshot" the current value into a local entry before modifying
     * the parent.
     *
     * Returns: expected<bool, string>
     *   - true: replaced existing variable
     *   - false: created new variable
     *   - error string: operation failed (no parent scope)
     * O(n) where n is the number of versions (typically small).
     */
    std::expected<bool, std::string> set_parent_scope(const std::string& name, const std::string& value) {
        if (current_depth_ == 0) { return std::unexpected("PARENT_SCOPE requires a parent scope (must be called from a function)"); }

        int target_depth = current_depth_ - 1;
        auto [it, inserted] = variables_.try_emplace(name);
        if (inserted) { it->second = std::make_unique<std::vector<VariableVersion>>(); }
        auto* versions = it->second.get();

        // CMake semantics: PARENT_SCOPE should NOT affect current scope's view.
        // If the variable is visible from a parent depth but has no local entry,
        // we need to "snapshot" the current visible value into a local entry first.
        bool has_local_entry = !versions->empty() && versions->back().depth == current_depth_;
        if (!has_local_entry && !versions->empty() && versions->back().value.has_value()) {
            // Variable is visible from parent - snapshot current value to preserve local view
            std::string current_value = *versions->back().value;
            versions->push_back({std::optional<std::string>(current_value), current_depth_});
            if (current_depth_ < static_cast<int>(modified_per_depth_.size())) { modified_per_depth_[current_depth_].push_back(versions); }
        }

        // Search for existing entry at parent depth and modify
        for (auto& ver : *versions) {
            if (ver.depth == target_depth) {
                ver.reset_list_index();
                ver.value = value;
                return true; // Replaced existing
            }
        }

        // No existing entry at parent depth - this is a NEW variable being created
        // CMake semantics: a variable created via PARENT_SCOPE is NOT visible in
        // the current scope until we exit.

        // Only insert tombstone if there's no local entry already
        // (If there's a local entry, the current scope's view is already established)
        if (!has_local_entry) {
            // Insert tombstone at current depth to hide the new parent variable
            versions->push_back({std::nullopt, current_depth_});
            if (current_depth_ < static_cast<int>(modified_per_depth_.size())) { modified_per_depth_[current_depth_].push_back(versions); }
        }

        // Insert the actual value at parent depth
        // Find the insertion point to maintain depth ordering
        auto insert_pos = versions->begin();
        while (insert_pos != versions->end() && insert_pos->depth < target_depth) { ++insert_pos; }

        versions->insert(insert_pos, {std::optional<std::string>(value), target_depth});

        // Track modification at parent depth
        if (target_depth < static_cast<int>(modified_per_depth_.size())) { modified_per_depth_[target_depth].push_back(versions); }

        return false; // Created new
    }

    /**
     * Unset a variable at the current depth.
     * If the variable exists at this depth, removes that version.
     * If the variable exists at a parent depth, inserts a tombstone to mask it.
     * O(1) complexity.
     */
    void unset(const std::string& name) {
        auto it = variables_.find(name);
        if (it == variables_.end() || it->second->empty()) { return; }

        auto* versions = it->second.get();
        if (versions->back().depth == current_depth_) {
            // Variable is at current depth - remove it
            versions->pop_back();
            // If a parent version exists, insert a tombstone to mask it
            if (!versions->empty() && versions->back().value.has_value()) {
                versions->push_back({std::nullopt, current_depth_});
                if (current_depth_ > 0 && current_depth_ < static_cast<int>(modified_per_depth_.size())) {
                    modified_per_depth_[current_depth_].push_back(versions);
                }
            }
        } else if (versions->back().depth < current_depth_ && versions->back().value.has_value()) {
            // Variable exists at parent depth and is visible - insert tombstone to mask it
            versions->push_back({std::nullopt, current_depth_});
            if (current_depth_ > 0 && current_depth_ < static_cast<int>(modified_per_depth_.size())) {
                modified_per_depth_[current_depth_].push_back(versions);
            }
        }
        // If variable is already a tombstone at parent depth, do nothing
    }

    /**
     * Unset a variable at the parent scope (depth - 1).
     * Used for CMake's unset(VAR PARENT_SCOPE).
     *
     * CMake semantics: unset(VAR PARENT_SCOPE) removes the variable from the
     * parent scope but does NOT affect the current scope's view. To achieve
     * this, if the variable is visible from the parent but has no local entry,
     * we first "snapshot" the current value into a local entry before unsetting
     * the parent.
     *
     * Returns: expected<bool, string>
     *   - true: variable was found and removed
     *   - false: variable was not found at parent scope
     *   - error string: operation failed (no parent scope)
     * O(n) where n is the number of versions (typically small).
     */
    std::expected<bool, std::string> unset_parent_scope(const std::string& name) {
        if (current_depth_ == 0) { return std::unexpected("PARENT_SCOPE requires a parent scope (must be called from a function)"); }

        int target_depth = current_depth_ - 1;
        auto it = variables_.find(name);
        if (it == variables_.end() || it->second->empty()) {
            return false; // Variable not found
        }

        auto* versions = it->second.get();

        // CMake semantics: unset PARENT_SCOPE should NOT affect current scope's view.
        // If the variable is visible from a parent depth but has no local entry,
        // we need to "snapshot" the current visible value into a local entry first.
        bool has_local_entry = !versions->empty() && versions->back().depth == current_depth_;
        if (!has_local_entry && !versions->empty() && versions->back().value.has_value()) {
            // Variable is visible from parent - snapshot current value to preserve local view
            std::string current_value = *versions->back().value;
            versions->push_back({std::optional<std::string>(current_value), current_depth_});
            if (current_depth_ < static_cast<int>(modified_per_depth_.size())) { modified_per_depth_[current_depth_].push_back(versions); }
        }

        // Find and remove the entry at parent depth
        for (auto ver_it = versions->begin(); ver_it != versions->end(); ++ver_it) {
            if (ver_it->depth == target_depth) {
                versions->erase(ver_it);
                return true; // Found and removed
            }
        }
        return false; // Not found at parent depth
    }

    /**
     * Check if a variable is defined at any visible depth.
     * O(1) complexity.
     */
    bool is_defined(std::string_view name) const {
        auto it = variables_.find(name);
        if (it == variables_.end() || it->second->empty()) { return false; }
        // Check for tombstone
        return it->second->back().value.has_value();
    }

    /**
     * Enter a new scope (increment depth).
     * O(1) complexity.
     */
    void push_scope() {
        ++current_depth_;

        // Ensure tracking vector is large enough
        if (current_depth_ >= static_cast<int>(modified_per_depth_.size())) { modified_per_depth_.resize(current_depth_ + 1); }
    }

    /**
     * Exit current scope (decrement depth).
     * Automatically removes all variables modified at this depth.
     * O(modified_vars) complexity.
     */
    void pop_scope() {
        if (current_depth_ == 0) {
            return; // Already at root
        }

        // Remove all modifications at current depth — direct pointer access, no hashing
        if (current_depth_ < static_cast<int>(modified_per_depth_.size())) {
            for (auto* versions : modified_per_depth_[current_depth_]) {
                // Remove all versions at current depth (should be at most one,
                // but duplicates in the tracking vector are handled gracefully)
                while (!versions->empty() && versions->back().depth == current_depth_) { versions->pop_back(); }
            }
            modified_per_depth_[current_depth_].clear();
        }

        --current_depth_;
    }

    /**
     * Get current scope depth.
     * Used for debugging and validation.
     */
    int depth() const { return current_depth_; }

    /**
     * Return a snapshot of all currently visible variables.
     * Useful for caching/restoring state.
     */
    std::unordered_map<std::string, std::string> snapshot() const {
        std::unordered_map<std::string, std::string> result;
        for (const auto& [name, versions] : variables_) {
            if (!versions->empty() && versions->back().value.has_value()) { result[name] = *versions->back().value; }
        }
        return result;
    }

    /**
     * Merge all variables from a map into the current scope.
     */
    void merge(const std::unordered_map<std::string, std::string>& vars) {
        for (const auto& [name, value] : vars) { set(name, value); }
    }

    /**
     * Get all currently visible variable names.
     * O(n) where n is total number of variables.
     */
    std::vector<std::string> get_all_names() const {
        std::vector<std::string> result;
        for (const auto& [name, versions] : variables_) {
            if (!versions->empty() && versions->back().value.has_value()) { result.push_back(name); }
        }
        return result;
    }

private:
    // Variable name -> [version history sorted by depth]
    // Uses open-addressing hash map (ankerl::unordered_dense) for cache-friendly probing.
    // Values are heap-allocated via unique_ptr for stable pointers across rehash.
    // Transparent hash/equal enables string_view lookup without allocation.
    inner::ankerl::unordered_dense::map<std::string, std::unique_ptr<std::vector<VariableVersion>>, TransparentStringHash,
                                        TransparentStringEqual>
        variables_;

    // Current scope depth (0 = root)
    int current_depth_ = 0;

    // Track which variables were modified at each depth (for cleanup)
    // Uses pointers to version vectors in variables_ (stable due to unique_ptr heap allocation).
    // Duplicates are tolerated — pop_scope handles them gracefully.
    // Storing version vector pointers directly eliminates hash lookups in pop_scope.
    std::vector<std::vector<std::vector<VariableVersion>*>> modified_per_depth_;
};

} // namespace kiln
