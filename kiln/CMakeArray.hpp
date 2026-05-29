#pragma once

#include <vector>
#include <string>
#include <string_view>
#include <cstring>
#include <stdexcept>

namespace kiln {

class Interpreter;

// Owning CMake semicolon-separated list with mutation support
class CMakeArray {
public:
    CMakeArray() = default;
    explicit CMakeArray(const std::string& semicolon_separated);
    explicit CMakeArray(const std::vector<std::string>& items);
    CMakeArray(std::initializer_list<std::string> items);

    std::string to_string() const;
    std::vector<std::string> to_vector() const;

    size_t size() const { return items_.size(); }
    bool empty() const { return items_.empty(); }
    const std::string& operator[](size_t idx) const { return items_[idx]; }
    const std::string& at(size_t idx) const { return items_.at(idx); }

    auto begin() const { return items_.begin(); }
    auto end() const { return items_.end(); }
    auto begin() { return items_.begin(); }
    auto end() { return items_.end(); }

    void append(const std::string& item);
    void append(const CMakeArray& other);
    void push_back(const std::string& item) { append(item); }
    void erase(size_t idx) { items_.erase(items_.begin() + idx); }
    void erase_range(size_t idx, size_t count) { items_.erase(items_.begin() + idx, items_.begin() + idx + count); }
    void insert(size_t idx, const std::vector<std::string>& items);

    void reverse();
    void sort(bool natural = false, bool descending = false);
    void remove_duplicates();
    CMakeArray sublist(size_t begin_idx, size_t length) const;
    bool contains(const std::string& item) const;

    // Count list elements without allocating (for LENGTH)
    static size_t count_elements(std::string_view str);

private:
    std::vector<std::string> items_;
    static std::vector<std::string> split_by_semicolon(const std::string& str);
};

// Non-owning read-only view over a semicolon-separated string.
// Avoids heap-allocating N strings when you only need to iterate.
class CMakeArrayView {
public:
    class iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = std::string_view;
        using difference_type = std::ptrdiff_t;
        using pointer = const std::string_view*;
        using reference = std::string_view;

        iterator() = default;
        iterator(const CMakeArrayView* parent, size_t idx) : parent_(parent), idx_(idx) {}

        std::string_view operator*() const { return parent_->element_at(idx_); }
        iterator& operator++() {
            ++idx_;
            return *this;
        }
        iterator operator++(int) {
            auto tmp = *this;
            ++idx_;
            return tmp;
        }
        bool operator==(const iterator& other) const { return idx_ == other.idx_; }
        bool operator!=(const iterator& other) const { return idx_ != other.idx_; }

    private:
        const CMakeArrayView* parent_ = nullptr;
        size_t idx_ = 0;
    };

    CMakeArrayView() = default;
    explicit CMakeArrayView(std::string_view semicolon_separated);

    size_t size() const;
    bool empty() const { return size() == 0; }
    std::string_view operator[](size_t idx) const { return element_at(idx); }
    std::string_view at(size_t idx) const;
    bool contains(std::string_view item) const;
    std::string to_string() const { return std::string(source_); }

    iterator begin() const { return iterator(this, 0); }
    iterator end() const { return iterator(this, size()); }

private:
    std::string_view source_;
    std::vector<size_t> separators_; // positions of ';' outside genex

    std::string_view element_at(size_t i) const;
};

// Zero-allocation forward range for iterating semicolon-separated lists.
// Unlike CMakeArrayView, does no pre-scanning or heap allocation.
// Use this when you only need sequential iteration (95% of cases).
class CMakeArrayIterator {
public:
    struct sentinel {};

    class iterator {
    public:
        using value_type = std::string_view;
        using difference_type = std::ptrdiff_t;

        explicit iterator(std::string_view source) : source_(source) {
            if (source_.empty()) {
                done_ = true;
            } else {
                find_end();
            }
        }

        std::string_view operator*() const { return source_.substr(pos_, end_ - pos_); }

        iterator& operator++() {
            if (end_ >= source_.size()) {
                done_ = true;
            } else {
                pos_ = end_ + 1;
                find_end();
            }
            return *this;
        }

        friend bool operator==(const iterator& it, sentinel) { return it.done_; }
        friend bool operator!=(const iterator& it, sentinel) { return !it.done_; }

    private:
        void find_end() {
            // Use memchr to find ';' — SIMD-optimized in glibc, scans 16-32
            // bytes/cycle vs 1 byte/iteration. Escaped semicolons (\;) are
            // extremely rare, so the backtrack check almost never triggers.
            const char* data = source_.data();
            size_t len = source_.size();
            size_t cur = pos_;
            while (cur < len) {
                const void* found = std::memchr(data + cur, ';', len - cur);
                if (!found) {
                    end_ = len;
                    return;
                }
                size_t semi = static_cast<const char*>(found) - data;
                if (semi > pos_ && data[semi - 1] == '\\') {
                    // Escaped semicolon — skip past it and keep scanning
                    cur = semi + 1;
                    continue;
                }
                end_ = semi;
                return;
            }
            end_ = len;
        }

        std::string_view source_;
        size_t pos_ = 0;
        size_t end_ = 0;
        bool done_ = false;
    };

    explicit CMakeArrayIterator(std::string_view source) : source_(source) {}

    iterator begin() const { return iterator(source_); }
    sentinel end() const { return {}; }

private:
    std::string_view source_;
};

// Zero-allocation contains check using forward iteration only.
inline bool cmake_list_contains(std::string_view list, std::string_view item) {
    for (auto it = CMakeArrayIterator::iterator(list); it != CMakeArrayIterator::sentinel{}; ++it) {
        if (*it == item) return true;
    }
    return false;
}

// Progressive (lazy, monotone-forward) index over a semicolon-separated list.
// Owns no source — call sites pass the source string_view, which must point to
// stable storage and the same buffer/content between calls until the index is
// reset. The index records each element's start offset as it scans forward;
// repeated random-access on the same value is amortized O(N+queries) instead
// of O(queries * avg_idx).
//
// Lifetime contract: the owner (e.g. VariableVersion) MUST reset the index
// whenever the underlying string is mutated (in-place append, assign, etc.).
struct ProgressiveListIndex {
    // offsets_[i] = byte offset of element i's first char in source.
    // Always starts with 0 once any scan happens.
    std::vector<uint32_t> offsets;
    // Bytes of source consumed so far. Equal to source.size() once exhausted.
    uint32_t scan_pos = 0;
    bool exhausted = false;

    // Advance the scan until offsets.size() > target_idx, or the source is
    // exhausted. Honors '\;' escapes the same way CMakeArrayIterator does.
    void scan_to(std::string_view source, size_t target_idx) {
        if (exhausted) return;
        if (source.empty()) {
            // CMake: empty string => 0 elements (matches CMakeArray::count_elements).
            exhausted = true;
            return;
        }
        if (offsets.empty()) { offsets.push_back(0); }
        const char* data = source.data();
        size_t len = source.size();
        while (offsets.size() <= target_idx + 1 && scan_pos < len) {
            const void* found = std::memchr(data + scan_pos, ';', len - scan_pos);
            if (!found) {
                scan_pos = static_cast<uint32_t>(len);
                exhausted = true;
                return;
            }
            size_t semi = static_cast<const char*>(found) - data;
            // Skip escaped semicolons (\;): keep scanning, don't record a boundary.
            if (semi > offsets.back() && data[semi - 1] == '\\') {
                scan_pos = static_cast<uint32_t>(semi + 1);
                continue;
            }
            offsets.push_back(static_cast<uint32_t>(semi + 1));
            scan_pos = static_cast<uint32_t>(semi + 1);
        }
        if (scan_pos >= len) exhausted = true;
    }

    // Returns the element at idx, or empty view if out of bounds. Caller is
    // responsible for scan_to() before this call.
    std::string_view element(std::string_view source, size_t idx) const {
        if (idx >= offsets.size()) return {};
        uint32_t start = offsets[idx];
        uint32_t end = (idx + 1 < offsets.size()) ? offsets[idx + 1] - 1 : static_cast<uint32_t>(source.size());
        return source.substr(start, end - start);
    }

    // Total element count. Requires a full scan; safe to call but may walk
    // the rest of the string.
    size_t total(std::string_view source) {
        scan_to(source, static_cast<size_t>(-2));
        // offsets contains one entry per element, plus possibly one trailing if
        // the source ends with a ';' (empty trailing element). Actual count is:
        // - if source is empty: still 1 element (the empty string)
        // - else: offsets.size(), unless source ended with ';' in which case +0
        //   (we already record the position after the trailing ';' as a starts).
        // Simpler: rely on count_elements semantics — but to stay consistent
        // with how the scan records starts, count == offsets.size().
        return offsets.size();
    }

    void reset() {
        offsets.clear();
        scan_pos = 0;
        exhausted = false;
    }
};

// Unescape \; to ; in a list element extracted via CMakeArrayIterator/View.
// Only call when sv contains '\\' (caller should check to avoid allocation).
inline std::string unescape_list_element(std::string_view sv) {
    std::string result;
    result.reserve(sv.size());
    for (size_t i = 0; i < sv.size(); ++i) {
        if (sv[i] == '\\' && i + 1 < sv.size() && sv[i + 1] == ';') {
            result += ';';
            ++i;
        } else {
            result += sv[i];
        }
    }
    return result;
}

} // namespace kiln
