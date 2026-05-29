#pragma once

#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace kiln {

/// Transform every element of a range into a new vector.
/// Reserves output size from input size.
template <typename Range, typename Fn> auto map(const Range& range, Fn&& fn) {
    using ResultType = std::decay_t<decltype(fn(*std::begin(range)))>;
    std::vector<ResultType> result;
    result.reserve(std::size(range));
    for (const auto& elem : range) { result.push_back(fn(elem)); }
    return result;
}

/// Keep only elements matching a predicate.
template <typename Range, typename Pred> auto filter(const Range& range, Pred&& pred) {
    using ValueType = std::decay_t<decltype(*std::begin(range))>;
    std::vector<ValueType> result;
    for (const auto& elem : range) {
        if (pred(elem)) { result.push_back(elem); }
    }
    return result;
}

/// Filter then transform in one pass. Only elements where pred returns true
/// are passed to fn and included in the output.
template <typename Range, typename Pred, typename Fn> auto filter_map(const Range& range, Pred&& pred, Fn&& fn) {
    using ResultType = std::decay_t<decltype(fn(*std::begin(range)))>;
    std::vector<ResultType> result;
    for (const auto& elem : range) {
        if (pred(elem)) { result.push_back(fn(elem)); }
    }
    return result;
}

/// Transform each element into a container and flatten the results.
template <typename Range, typename Fn> auto flat_map(const Range& range, Fn&& fn) {
    using InnerRange = std::decay_t<decltype(fn(*std::begin(range)))>;
    using ValueType = std::decay_t<decltype(*std::begin(std::declval<InnerRange>()))>;
    std::vector<ValueType> result;
    for (const auto& elem : range) {
        for (auto&& inner : fn(elem)) { result.push_back(std::move(inner)); }
    }
    return result;
}

/// Remove duplicate elements in-place, preserving first-occurrence order.
template <typename T> void remove_duplicates(std::vector<T>& vec) {
    std::unordered_set<T> seen;
    auto it = std::remove_if(vec.begin(), vec.end(), [&](const T& val) { return !seen.insert(val).second; });
    vec.erase(it, vec.end());
}

/// Join elements of a string range with a separator.
/// Pre-calculates total size for a single allocation.
template <typename Range> std::string join(const Range& range, std::string_view sep) {
    std::string result;
    auto it = std::begin(range);
    auto end = std::end(range);
    if (it == end) return result;

    // Calculate total size
    size_t total = 0;
    size_t count = 0;
    for (auto i = it; i != end; ++i, ++count) { total += std::string_view(*i).size(); }
    total += sep.size() * (count > 0 ? count - 1 : 0);
    result.reserve(total);

    result += std::string_view(*it);
    for (++it; it != end; ++it) {
        result += sep;
        result += std::string_view(*it);
    }
    return result;
}

/// Join with a per-element transform. The transform must return something
/// convertible to std::string_view or std::string.
template <typename Range, typename Fn> std::string join(const Range& range, std::string_view sep, Fn&& fn) {
    std::string result;
    bool first = true;
    for (const auto& elem : range) {
        if (!first) result += sep;
        first = false;
        result += fn(elem);
    }
    return result;
}

} // namespace kiln
