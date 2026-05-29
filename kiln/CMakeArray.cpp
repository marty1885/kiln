#include "CMakeArray.hpp"
#include "parse_number.hpp"
#include <sstream>
#include <algorithm>
#include <set>

namespace kiln {

std::vector<std::string> CMakeArray::split_by_semicolon(const std::string& str) {
    if (str.empty()) { return {}; }

    // CMake's list() does NOT track genex nesting -- semicolons inside $<...>
    // are still separators. Only \; is treated as an escaped (non-separator) semicolon.

    // Fast path: no backslashes means no escaped semicolons
    if (str.find('\\') == std::string::npos) {
        std::vector<std::string> result;
        size_t start = 0;
        for (size_t i = 0; i < str.size(); ++i) {
            if (str[i] == ';') {
                result.emplace_back(str, start, i - start);
                start = i + 1;
            }
        }
        result.emplace_back(str, start, str.size() - start);
        return result;
    }

    // Slow path: handle \; escaping
    std::vector<std::string> result;
    std::string current;
    for (size_t i = 0; i < str.size(); ++i) {
        char c = str[i];
        if (c == '\\' && i + 1 < str.size() && str[i + 1] == ';') {
            // Escaped semicolon - not a separator, unescape it
            current += ';';
            i++; // skip the semicolon
        } else if (c == ';') {
            result.push_back(std::move(current));
            current.clear();
        } else {
            current += c;
        }
    }
    result.push_back(std::move(current));
    return result;
}

size_t CMakeArray::count_elements(std::string_view str) {
    if (str.empty()) return 0;
    size_t count = 1;
    for (size_t i = 0; i < str.size(); ++i) {
        if (str[i] == '\\' && i + 1 < str.size() && str[i + 1] == ';') {
            i++; // skip escaped semicolon
        } else if (str[i] == ';') {
            count++;
        }
    }
    return count;
}

CMakeArray::CMakeArray(const std::string& semicolon_separated) : items_(split_by_semicolon(semicolon_separated)) {}

CMakeArray::CMakeArray(const std::vector<std::string>& items) : items_(items) {}

CMakeArray::CMakeArray(std::initializer_list<std::string> items) : items_(items) {}

std::string CMakeArray::to_string() const {
    std::string res;
    for (size_t i = 0; i < items_.size(); ++i) {
        if (i > 0) res += ";";
        res += items_[i];
    }
    return res;
}

std::vector<std::string> CMakeArray::to_vector() const {
    return items_;
}

void CMakeArray::append(const std::string& item) {
    if (item.empty()) return;
    if (item.find(';') == std::string::npos) {
        items_.push_back(item);
        return;
    }
    auto parts = split_by_semicolon(item);
    items_.insert(items_.end(), parts.begin(), parts.end());
}

void CMakeArray::append(const CMakeArray& other) {
    items_.insert(items_.end(), other.items_.begin(), other.items_.end());
}

void CMakeArray::insert(size_t idx, const std::vector<std::string>& items) {
    items_.insert(items_.begin() + idx, items.begin(), items.end());
}

void CMakeArray::reverse() {
    std::reverse(items_.begin(), items_.end());
}

// Natural sort comparison function
static bool natural_compare(const std::string& a, const std::string& b) {
    auto is_digit = [](char c) { return c >= '0' && c <= '9'; };

    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        // If both are digits, compare numerically
        if (is_digit(a[i]) && is_digit(b[j])) {
            // Extract the numeric parts
            size_t num_start_a = i, num_start_b = j;
            while (i < a.size() && is_digit(a[i])) i++;
            while (j < b.size() && is_digit(b[j])) j++;

            std::string num_a = a.substr(num_start_a, i - num_start_a);
            std::string num_b = b.substr(num_start_b, j - num_start_b);

            // Compare numerically
            auto opt_a = parse_number<unsigned long long>(num_a);
            auto opt_b = parse_number<unsigned long long>(num_b);
            if (opt_a && opt_b) {
                if (*opt_a != *opt_b) { return *opt_a < *opt_b; }
                // If numerically equal, longer strings (more leading zeros) come first
                // This handles "00" vs "0" - "00" (length 2) comes before "0" (length 1)
                if (num_a.size() != num_b.size()) {
                    return num_a.size() > num_b.size(); // Longer first!
                }
            } else {
                // If numeric conversion fails, fall back to string comparison
                if (num_a != num_b) { return num_a < num_b; }
            }
        } else {
            // Lexicographic comparison
            if (a[i] != b[j]) { return a[i] < b[j]; }
            i++;
            j++;
        }
    }

    // If we've exhausted one string, the shorter one comes first
    return a.size() < b.size();
}

void CMakeArray::sort(bool natural, bool descending) {
    if (natural) {
        if (descending) {
            std::sort(items_.begin(), items_.end(), [](const std::string& a, const std::string& b) {
                return natural_compare(b, a); // Reverse comparison for descending
            });
        } else {
            std::sort(items_.begin(), items_.end(), natural_compare);
        }
    } else {
        if (descending) {
            std::sort(items_.begin(), items_.end(), std::greater<std::string>());
        } else {
            std::sort(items_.begin(), items_.end());
        }
    }
}

void CMakeArray::remove_duplicates() {
    std::set<std::string> seen;
    std::vector<std::string> unique_items;
    for (const auto& item : items_) {
        if (seen.find(item) == seen.end()) {
            unique_items.push_back(item);
            seen.insert(item);
        }
    }
    items_ = std::move(unique_items);
}

CMakeArray CMakeArray::sublist(size_t begin_idx, size_t length) const {
    if (begin_idx >= items_.size()) return CMakeArray();
    size_t count = std::min(length, items_.size() - begin_idx);
    std::vector<std::string> sub(items_.begin() + begin_idx, items_.begin() + begin_idx + count);
    return CMakeArray(sub);
}

bool CMakeArray::contains(const std::string& item) const {
    return std::find(items_.begin(), items_.end(), item) != items_.end();
}

// --- CMakeArrayView ---

CMakeArrayView::CMakeArrayView(std::string_view semicolon_separated) : source_(semicolon_separated) {
    if (source_.empty()) return;

    // CMake does NOT track genex nesting for list splitting.
    // Only \; is treated as an escaped (non-separator) semicolon.
    for (size_t i = 0; i < source_.size(); ++i) {
        char c = source_[i];
        if (c == '\\' && i + 1 < source_.size() && source_[i + 1] == ';') {
            i++; // skip escaped semicolon
        } else if (c == ';') {
            separators_.push_back(i);
        }
    }
}

size_t CMakeArrayView::size() const {
    if (source_.empty()) return 0;
    return separators_.size() + 1;
}

std::string_view CMakeArrayView::at(size_t idx) const {
    if (idx >= size()) { throw std::out_of_range("CMakeArrayView::at: index out of range"); }
    return element_at(idx);
}

bool CMakeArrayView::contains(std::string_view item) const {
    for (size_t i = 0; i < size(); ++i) {
        if (element_at(i) == item) return true;
    }
    return false;
}

std::string_view CMakeArrayView::element_at(size_t i) const {
    size_t start = (i == 0) ? 0 : separators_[i - 1] + 1;
    size_t end = (i < separators_.size()) ? separators_[i] : source_.size();
    return source_.substr(start, end - start);
}

} // namespace kiln