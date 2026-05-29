#pragma once
#include <charconv>
#include <optional>
#include <string_view>

namespace kiln {

// Thin from_chars wrapper replacing std::sto* for performance.
// No locale, no exceptions, no allocation.
template <typename T> std::optional<T> parse_number(std::string_view s) {
    if (s.empty()) return std::nullopt;
    T value{};
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (ec != std::errc{} || ptr != s.data() + s.size()) return std::nullopt;
    return value;
}

// Partial parse: parses leading digits (like std::stoi), ignoring trailing non-numeric chars.
// Returns 0 on failure (for version component parsing where non-numeric → 0).
template <typename T> T parse_number_partial(std::string_view s, T fallback = T{}) {
    if (s.empty()) return fallback;
    T value{};
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (ec != std::errc{}) return fallback;
    return value;
}

// Double parsing via from_chars (C++17 guarantees from_chars for float/double)
inline std::optional<double> parse_double(std::string_view s) {
    if (s.empty()) return std::nullopt;
    double value{};
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (ec != std::errc{} || ptr != s.data() + s.size()) return std::nullopt;
    return value;
}

} // namespace kiln
