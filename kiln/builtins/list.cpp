#include "registry.hpp"
#include "../interperter.hpp"
#include "../command_parser.hpp"
#include "../utils.hpp"
#include "../container_utils.hpp"
#include "../parse_number.hpp"
#include <algorithm>
#include <cstring>
#include <optional>
#include <set>
#include "../regex.hpp"
#include "../clock_cache.hpp"

namespace kiln {

namespace {
// Helper function to strip whitespace
std::string strip(const std::string& str) {
    return std::string(kiln::strip(str));
}

// Helper function to extract basename from file path
std::string get_basename(const std::string& path) {
    size_t last_slash = path.find_last_of("/\\");
    if (last_slash == std::string::npos) { return path; }
    return path.substr(last_slash + 1);
}

// Locate the [start, sep_or_end) byte range of element `target` in a
// semicolon-separated list. `sep_or_end` is the position of the ';' that
// terminates the element, or src.size() if it is the last element.
// Returns nullopt if `target` is out of range. Respects \;-escaping.
std::optional<std::pair<size_t, size_t>> element_extent(std::string_view src, size_t target) {
    if (src.empty()) return std::nullopt;
    size_t pos = 0, idx = 0, cur = 0;
    while (cur <= src.size()) {
        const void* f = (cur < src.size()) ? std::memchr(src.data() + cur, ';', src.size() - cur) : nullptr;
        if (!f) {
            if (idx == target) return std::make_pair(pos, src.size());
            return std::nullopt;
        }
        size_t s = static_cast<const char*>(f) - src.data();
        if (s > 0 && src[s - 1] == '\\') {
            cur = s + 1;
            continue;
        }
        if (idx == target) return std::make_pair(pos, s);
        ++idx;
        pos = s + 1;
        cur = pos;
    }
    return std::nullopt;
}

// Locate the byte offset where element `target` begins. For
// target == element_count this returns src.size() (the append position).
// Returns nullopt if `target` is past one-after-last.
std::optional<size_t> element_insert_pos(std::string_view src, size_t target) {
    if (target == 0) return 0;
    if (src.empty()) return std::nullopt;
    size_t idx = 0, cur = 0;
    while (cur < src.size()) {
        const void* f = std::memchr(src.data() + cur, ';', src.size() - cur);
        if (!f) break;
        size_t s = static_cast<const char*>(f) - src.data();
        if (s > 0 && src[s - 1] == '\\') {
            cur = s + 1;
            continue;
        }
        ++idx;
        cur = s + 1;
        if (idx == target) return cur;
    }
    // Hit end of string. Element count is idx + 1; valid append index is that.
    if (target == idx + 1) return src.size();
    return std::nullopt;
}
} // namespace

void register_list_builtins(Interpreter& interp) {
    interp.add_builtin("list", [](Interpreter& interp, const std::vector<std::string>& args) {
        if (args.empty()) {
            interp.set_fatal_error("list() requires at least one argument");
            return;
        }

        std::string operation = args[0];
        std::transform(operation.begin(), operation.end(), operation.begin(),
                       [](unsigned char c) { return (c >= 'a' && c <= 'z') ? static_cast<unsigned char>(c - 32) : c; });

        std::vector<std::string> sub_args(args.begin() + 1, args.end());

        if (operation == "LENGTH") {
            // list(LENGTH <list> <output variable>)
            if (sub_args.size() != 2) {
                interp.set_fatal_error("list(LENGTH) requires exactly 2 arguments: list(LENGTH <list> <output variable>)");
                return;
            }
            auto len_view = interp.get_variable_view(sub_args[0]);
            if (!len_view) {
                interp.set_variable(sub_args[1], "0");
                return;
            }
            // Use the progressive index: first call scans (same cost as
            // count_elements), repeated LENGTH on the same value is O(1).
            auto* lidx = interp.get_variables().const_list_index_for_read(sub_args[0]);
            size_t count = lidx ? lidx->total(*len_view) : CMakeArray::count_elements(*len_view);
            interp.set_variable(sub_args[1], std::to_string(count));
        } else if (operation == "GET") {
            // list(GET <list> <index> [<index> ...] <output variable>)
            if (sub_args.size() < 3) {
                interp.set_fatal_error("list(GET) requires at least 3 arguments: list(GET <list> <index> <output variable>)");
                return;
            }

            const std::string& list_var = sub_args[0];
            const std::string& out_var = sub_args.back();

            // Fast path: single non-negative index. Use the variable's progressive
            // list-element index — amortizes element-boundary scans across calls
            // on the same value (random-access loops become O(N + queries)).
            if (sub_args.size() == 3) {
                auto entry_opt = interp.get_variables().const_entry(list_var);
                if (entry_opt && entry_opt->is_defined()) {
                    auto idx_opt = parse_number<long>(sub_args[1]);
                    if (idx_opt && *idx_opt >= 0) {
                        const std::string& src = entry_opt->get();
                        if (src.empty()) {
                            interp.set_variable(out_var, "NOTFOUND");
                            return;
                        }
                        auto* idx = interp.get_variables().const_list_index_for_read(list_var);
                        if (idx) {
                            std::string_view sv(src);
                            idx->scan_to(sv, static_cast<size_t>(*idx_opt));
                            if (static_cast<size_t>(*idx_opt) >= idx->offsets.size()) {
                                interp.set_variable(out_var, "NOTFOUND");
                                return;
                            }
                            std::string_view elem = idx->element(sv, static_cast<size_t>(*idx_opt));
                            if (elem.find('\\') != std::string_view::npos) {
                                interp.set_variable(out_var, unescape_list_element(elem));
                            } else {
                                interp.set_variable(out_var, std::string(elem));
                            }
                            return;
                        }
                    }
                }
            }

            auto list_view = interp.get_variable_view(list_var);
            std::string_view list_str = list_view.value_or("");

            if (list_str.empty()) {
                interp.set_variable(out_var, "NOTFOUND");
                return;
            }

            // Parse and resolve indices, keeping original request order.
            // `total` is only needed to normalise negative indices, so it is
            // computed lazily — for the common all-non-negative case we skip
            // the full string scan that count_elements does.
            size_t num_indices = sub_args.size() - 2; // exclude list_var and out_var
            struct IndexRequest {
                size_t orig_pos;
                long resolved;
            };
            IndexRequest buf[8];
            std::vector<IndexRequest> heap_buf;
            IndexRequest* requests = buf;
            if (num_indices > 8) {
                heap_buf.resize(num_indices);
                requests = heap_buf.data();
            }

            long total = -1; // lazily filled when the first negative index appears
            for (size_t i = 0; i < num_indices; ++i) {
                auto idx_opt = parse_number<long>(sub_args[i + 1]);
                if (!idx_opt) {
                    interp.set_fatal_error("list(GET) invalid index: " + sub_args[i + 1]);
                    return;
                }
                long idx = *idx_opt;
                if (idx < 0) {
                    if (total < 0) total = static_cast<long>(CMakeArray::count_elements(list_str));
                    idx = total + idx;
                }
                requests[i] = {i, idx};
            }

            // Sort by resolved index for single-pass iteration
            std::sort(requests, requests + num_indices,
                      [](const IndexRequest& a, const IndexRequest& b) { return a.resolved < b.resolved; });

            // Single-pass collection
            std::string results[8];
            std::vector<std::string> heap_results;
            std::string* result_arr = results;
            if (num_indices > 8) {
                heap_results.resize(num_indices);
                result_arr = heap_results.data();
            }

            size_t req_i = 0;
            // Skip requests with out-of-bounds negative indices
            while (req_i < num_indices && requests[req_i].resolved < 0) {
                result_arr[requests[req_i].orig_pos] = "NOTFOUND";
                ++req_i;
            }

            size_t elem_idx = 0;
            auto it = CMakeArrayIterator::iterator(list_str);
            auto end = CMakeArrayIterator::sentinel{};
            while (req_i < num_indices && it != end) {
                long target_idx = requests[req_i].resolved;
                if (static_cast<long>(elem_idx) == target_idx) {
                    auto val = *it;
                    // Handle all requests for this same index
                    // Unescape \; to ; in extracted elements (CMake behavior)
                    auto unescaped = (val.find('\\') != std::string_view::npos) ? unescape_list_element(val) : std::string(val);
                    while (req_i < num_indices && requests[req_i].resolved == target_idx) {
                        result_arr[requests[req_i].orig_pos] = unescaped;
                        ++req_i;
                    }
                }
                if (req_i < num_indices && static_cast<long>(elem_idx) < requests[req_i].resolved) {
                    ++it;
                    ++elem_idx;
                }
            }
            // Any remaining requests are out of bounds
            while (req_i < num_indices) {
                result_arr[requests[req_i].orig_pos] = "NOTFOUND";
                ++req_i;
            }

            // Build result in original order
            std::string result_str;
            for (size_t i = 0; i < num_indices; ++i) {
                if (!result_str.empty()) result_str += ';';
                result_str += result_arr[i];
            }
            interp.set_variable(out_var, result_str);
        } else if (operation == "JOIN") {
            CommandParser parser("list", "JOIN");
            std::string list_var, glue, out_var;
            parser.positional(list_var, "list variable");
            parser.positional(glue, "glue string");
            parser.positional(out_var, "output variable");
            PARSE_OR_RETURN(parser, interp, sub_args);

            auto list_view = interp.get_variable_view(list_var);
            if (!list_view || list_view->empty()) {
                interp.set_variable(out_var, "");
            } else {
                interp.set_variable(out_var, join(CMakeArrayIterator(*list_view), glue));
            }
        } else if (operation == "APPEND") {
            // list(APPEND <list> [<element> ...])
            if (sub_args.empty()) {
                interp.set_fatal_error("list(APPEND) requires at least the list variable name");
                return;
            }
            // Fast path: when the variable already lives at the current scope depth,
            // mutate the stored string directly. This turns a tight 100k-iter append
            // loop from O(N^2) (copy-out + copy-back per call) into amortized O(1).
            auto entry = interp.get_variables().entry(sub_args[0]);
            if (std::string* val = entry.mutable_at_current_depth()) {
                for (size_t i = 1; i < sub_args.size(); ++i) {
                    if (sub_args[i].empty() && val->empty()) continue;
                    if (!val->empty()) val->push_back(';');
                    val->append(sub_args[i]);
                }
                if (interp.has_variable_watches()) { interp.fire_variable_watch(sub_args[0], "MODIFIED_ACCESS", *val); }
            } else {
                // Slow path: must read with cache fallback (when the local variable
                // is unset, CMake exposes the cache value via `${VAR}` so list(APPEND)
                // sees it — reading the local entry directly would start from empty
                // and silently drop cache-only values, e.g. CMAKE_MODULE_PATH seeded
                // by a -C initial cache) and snapshot into the current scope.
                std::string new_val = interp.get_variable(sub_args[0]);
                for (size_t i = 1; i < sub_args.size(); ++i) {
                    if (sub_args[i].empty() && new_val.empty()) continue;
                    if (!new_val.empty()) new_val += ';';
                    new_val += sub_args[i];
                }
                interp.set_variable(sub_args[0], new_val);
            }
        } else if (operation == "PREPEND") {
            CommandParser parser("list", "PREPEND");
            std::string list_var;
            std::vector<std::string> items;
            parser.positional(list_var, "list variable");
            parser.positionals(items, "items");
            PARSE_OR_RETURN(parser, interp, sub_args);

            // Cache fallback: same reasoning as APPEND — a cache-only list
            // would otherwise look empty.
            CMakeArray list(interp.get_variable(list_var));
            list.insert(0, items);
            interp.set_variable(list_var, list.to_string());
        } else if (operation == "POP_BACK") {
            CommandParser parser("list", "POP_BACK");
            std::string list_var;
            std::vector<std::string> out_vars;
            parser.positional(list_var, "list variable");
            parser.positionals(out_vars, "output variables");
            PARSE_OR_RETURN(parser, interp, sub_args);

            auto entry = interp.get_variables().entry(list_var);
            CMakeArray list(entry.get());

            // CMake silently does nothing when popping from empty/undefined lists,
            // setting output variables to empty.
            if (list.empty()) {
                for (const auto& out_var : out_vars) { interp.set_variable(out_var, ""); }
                return;
            }

            // Pop one element for each output variable (default 1)
            size_t num_to_pop = std::min(out_vars.empty() ? 1 : out_vars.size(), list.size());

            // Store popped values in output variables (in reverse order from back)
            for (size_t i = 0; i < std::min(out_vars.size(), list.size()); ++i) {
                size_t idx = list.size() - 1 - i;
                interp.set_variable(out_vars[i], list[idx]);
            }
            // Any remaining output vars get empty
            for (size_t i = list.size(); i < out_vars.size(); ++i) { interp.set_variable(out_vars[i], ""); }

            // Remove elements from the list
            for (size_t i = 0; i < num_to_pop; ++i) { list.erase(list.size() - 1); }

            entry.set(list.to_string());
        } else if (operation == "POP_FRONT") {
            CommandParser parser("list", "POP_FRONT");
            std::string list_var;
            std::vector<std::string> out_vars;
            parser.positional(list_var, "list variable");
            parser.positionals(out_vars, "output variables");
            PARSE_OR_RETURN(parser, interp, sub_args);

            auto entry = interp.get_variables().entry(list_var);
            CMakeArray list(entry.get());

            // CMake silently does nothing when popping from empty/undefined lists
            if (list.empty()) {
                for (const auto& out_var : out_vars) { interp.set_variable(out_var, ""); }
                return;
            }

            size_t num_to_pop = std::min(out_vars.empty() ? 1 : out_vars.size(), list.size());

            // Store popped values in output variables
            for (size_t i = 0; i < std::min(out_vars.size(), list.size()); ++i) { interp.set_variable(out_vars[i], list[i]); }
            for (size_t i = list.size(); i < out_vars.size(); ++i) { interp.set_variable(out_vars[i], ""); }

            list.erase_range(0, num_to_pop);

            entry.set(list.to_string());
        } else if (operation == "INSERT") {
            CommandParser parser("list", "INSERT");
            std::string list_var, index_str;
            std::vector<std::string> items;
            parser.positional(list_var, "list variable");
            parser.positional(index_str, "index");
            parser.positionals(items, "items");
            PARSE_OR_RETURN(parser, interp, sub_args);

            auto idx_opt = parse_number<long>(index_str);
            if (!idx_opt) {
                interp.set_fatal_error("list(INSERT) invalid index: " + index_str);
                return;
            }
            auto entry = interp.get_variables().entry(list_var);

            // Fast path for non-negative indices: splice the source string
            // directly instead of splitting it into a vector<string> and
            // rejoining. Avoids O(N) string allocations per call when the
            // list is large (e.g. DP-table style code).
            long idx = *idx_opt;
            if (idx >= 0) {
                std::string_view src = entry.get();
                auto start = element_insert_pos(src, static_cast<size_t>(idx));
                if (!start) {
                    interp.set_fatal_error("list(INSERT) index out of range: " + index_str);
                    return;
                }
                size_t joined_size = items.empty() ? 0 : (items.size() - 1);
                for (const auto& it : items) joined_size += it.size();

                std::string result;
                if (src.empty()) {
                    result.reserve(joined_size);
                    for (size_t i = 0; i < items.size(); ++i) {
                        if (i > 0) result += ';';
                        result += items[i];
                    }
                } else if (*start == src.size()) {
                    result.reserve(src.size() + 1 + joined_size);
                    result.append(src.data(), src.size());
                    for (const auto& it : items) {
                        result += ';';
                        result += it;
                    }
                } else {
                    result.reserve(src.size() + joined_size + 1);
                    result.append(src.data(), *start);
                    for (size_t i = 0; i < items.size(); ++i) {
                        if (i > 0) result += ';';
                        result += items[i];
                    }
                    result += ';';
                    result.append(src.data() + *start, src.size() - *start);
                }
                entry.set(std::move(result));
            } else {
                // Negative index: rare path, full materialize is fine.
                CMakeArray list(entry.get());
                long resolved = static_cast<long>(list.size()) + idx;
                if (resolved < 0 || static_cast<size_t>(resolved) > list.size()) {
                    interp.set_fatal_error("list(INSERT) index out of range: " + index_str);
                    return;
                }
                list.insert(static_cast<size_t>(resolved), items);
                entry.set(list.to_string());
            }
        } else if (operation == "REVERSE") {
            CommandParser parser("list", "REVERSE");
            std::string list_var;
            parser.positional(list_var, "list variable");
            PARSE_OR_RETURN(parser, interp, sub_args);

            auto entry = interp.get_variables().entry(list_var);
            CMakeArray list(entry.get());
            list.reverse();
            entry.set(list.to_string());
        } else if (operation == "SORT") {
            CommandParser parser("list", "SORT");
            std::string list_var;
            std::string compare_mode, case_mode, order_mode;
            parser.positional(list_var, "list variable");
            parser.value("COMPARE", compare_mode);
            parser.value("CASE", case_mode);
            parser.value("ORDER", order_mode);
            PARSE_OR_RETURN(parser, interp, sub_args);

            bool natural = false;
            bool descending = false;
            bool file_basename = false;

            if (!compare_mode.empty()) {
                std::transform(compare_mode.begin(), compare_mode.end(), compare_mode.begin(),
                               [](unsigned char c) { return std::toupper(c); });
                if (compare_mode == "NATURAL") {
                    natural = true;
                } else if (compare_mode == "STRING") {
                    natural = false;
                } else if (compare_mode == "FILE_BASENAME") {
                    file_basename = true;
                } else {
                    interp.set_fatal_error("list(SORT) unknown COMPARE mode: " + compare_mode);
                    return;
                }
            }

            if (!order_mode.empty()) {
                std::transform(order_mode.begin(), order_mode.end(), order_mode.begin(), [](unsigned char c) { return std::toupper(c); });
                if (order_mode == "DESCENDING") {
                    descending = true;
                } else if (order_mode != "ASCENDING") {
                    interp.set_fatal_error("list(SORT) unknown ORDER mode: " + order_mode);
                    return;
                }
            }

            auto entry = interp.get_variables().entry(list_var);
            CMakeArray list(entry.get());

            // Handle FILE_BASENAME comparison specially
            if (file_basename) {
                auto vec = list.to_vector();
                std::sort(vec.begin(), vec.end(), [descending](const std::string& a, const std::string& b) {
                    std::string basename_a = get_basename(a);
                    std::string basename_b = get_basename(b);
                    if (descending) { return basename_a > basename_b; }
                    return basename_a < basename_b;
                });
                list = CMakeArray(vec);
            } else {
                // Note: CASE is handled inside CMakeArray::sort if needed
                list.sort(natural, descending);
            }
            entry.set(list.to_string());
        } else if (operation == "REMOVE_DUPLICATES") {
            CommandParser parser("list", "REMOVE_DUPLICATES");
            std::string list_var;
            parser.positional(list_var, "list variable");
            PARSE_OR_RETURN(parser, interp, sub_args);

            auto entry = interp.get_variables().entry(list_var);
            CMakeArray list(entry.get());
            list.remove_duplicates();
            entry.set(list.to_string());
        } else if (operation == "SUBLIST") {
            CommandParser parser("list", "SUBLIST");
            std::string list_var, out_var, start_str, length_str;
            parser.positional(list_var, "list variable");
            parser.positional(start_str, "start index");
            parser.positional(length_str, "length");
            parser.positional(out_var, "output variable");
            PARSE_OR_RETURN(parser, interp, sub_args);

            auto list_sv = interp.get_variable_view(list_var);
            std::string_view list_str = list_sv.value_or("");
            {
                auto start_opt = parse_number<long>(start_str);
                auto length_opt = parse_number<long>(length_str);
                if (!start_opt || !length_opt) {
                    interp.set_fatal_error("list(SUBLIST) invalid index");
                    return;
                }
                long start = *start_opt;
                long length = *length_opt;

                auto* lidx = list_sv ? interp.get_variables().const_list_index_for_read(list_var) : nullptr;

                // Negative start or length<0 requires the total; both progressive
                // index and count_elements pay the same scan, so let the
                // progressive path do it (and memoize for future calls).
                size_t total;
                bool need_total = (start < 0) || (length < 0);
                if (need_total) {
                    total = lidx ? lidx->total(list_str) : CMakeArray::count_elements(list_str);
                    if (start < 0) start = static_cast<long>(total) + start;
                } else {
                    total = 0; // computed on demand below if needed for the range check
                }

                if (start < 0) {
                    interp.set_fatal_error("list(SUBLIST) start index out of range");
                    return;
                }

                size_t s = static_cast<size_t>(start);
                // Resolve `count` and validate `s` against the actual size.
                size_t count;
                if (need_total) {
                    if (s > total) {
                        interp.set_fatal_error("list(SUBLIST) start index out of range");
                        return;
                    }
                    count = (length < 0) ? total - s : std::min(static_cast<size_t>(length), total - s);
                } else {
                    count = static_cast<size_t>(length);
                }

                if (count == 0) {
                    interp.set_variable(out_var, "");
                    return;
                }

                std::string result_str;
                if (lidx) {
                    // Scan only as far as we actually need.
                    lidx->scan_to(list_str, s + count - 1);
                    // Validate the upper bound now that we may have learned the size.
                    if (s >= lidx->offsets.size()) {
                        if (!need_total) {
                            interp.set_fatal_error("list(SUBLIST) start index out of range");
                            return;
                        }
                        interp.set_variable(out_var, "");
                        return;
                    }
                    size_t end_idx = std::min(s + count, lidx->offsets.size());
                    for (size_t i = s; i < end_idx; ++i) {
                        if (i != s) result_str += ';';
                        result_str += lidx->element(list_str, i);
                    }
                } else {
                    size_t idx = 0;
                    for (auto it = CMakeArrayIterator::iterator(list_str); it != CMakeArrayIterator::sentinel{}; ++it, ++idx) {
                        if (idx < s) continue;
                        if (idx >= s + count) break;
                        if (idx != s) result_str += ';';
                        result_str += *it;
                    }
                }
                interp.set_variable(out_var, result_str);
            }
        } else if (operation == "FIND") {
            CommandParser parser("list", "FIND");
            std::string list_var, value, out_var;
            parser.positional(list_var, "list variable");
            parser.positional(value, "value to find");
            parser.positional(out_var, "output variable");
            PARSE_OR_RETURN(parser, interp, sub_args);

            auto list_view = interp.get_variable_view(list_var);
            long found_index = -1;
            if (list_view && !list_view->empty()) {
                size_t idx = 0;
                for (auto item : CMakeArrayIterator(*list_view)) {
                    // Unescape \; before comparing (CMake compares unescaped values)
                    bool match = (item.find('\\') != std::string_view::npos) ? unescape_list_element(item) == value : item == value;
                    if (match) {
                        found_index = static_cast<long>(idx);
                        break;
                    }
                    ++idx;
                }
            }
            interp.set_variable(out_var, std::to_string(found_index));
        } else if (operation == "REMOVE_ITEM") {
            CommandParser parser("list", "REMOVE_ITEM");
            std::string list_var;
            std::vector<std::string> items;
            parser.positional(list_var, "list variable");
            parser.positionals(items, "items");
            PARSE_OR_RETURN(parser, interp, sub_args);

            auto entry = interp.get_variables().entry(list_var);
            const std::string& list_str = entry.get();
            std::set<std::string, std::less<>> items_set(items.begin(), items.end());
            std::string result_str;
            for (auto item : CMakeArrayIterator(list_str)) {
                if (!items_set.contains(item)) {
                    if (!result_str.empty()) result_str += ';';
                    result_str += item;
                }
            }
            entry.set(std::move(result_str));
        } else if (operation == "REMOVE_AT") {
            CommandParser parser("list", "REMOVE_AT");
            std::string list_var;
            std::vector<std::string> indices;
            parser.positional(list_var, "list variable");
            parser.positionals(indices, "indices");
            PARSE_OR_RETURN(parser, interp, sub_args);

            auto entry = interp.get_variables().entry(list_var);

            // Fast path: single non-negative index, no list materialization.
            // Hot for DP-table style code that does REMOVE_AT + INSERT to
            // overwrite a slot — avoids splitting the list into a vector and
            // rejoining it.
            if (indices.size() == 1) {
                auto idx_opt = parse_number<long>(indices[0]);
                if (!idx_opt) {
                    interp.set_fatal_error("list(REMOVE_AT) invalid index: " + indices[0]);
                    return;
                }
                if (*idx_opt >= 0) {
                    std::string_view src = entry.get();
                    auto extent = element_extent(src, static_cast<size_t>(*idx_opt));
                    if (!extent) {
                        interp.set_fatal_error("list(REMOVE_AT) index out of range: " + indices[0]);
                        return;
                    }
                    auto [start, sep_or_end] = *extent;
                    std::string result;
                    if (start == 0 && sep_or_end == src.size()) {
                        // Only element — list becomes empty.
                    } else if (sep_or_end == src.size()) {
                        // Last element: drop the preceding ';' and the element.
                        result.assign(src.data(), start - 1);
                    } else {
                        // Drop the element and its trailing ';'.
                        result.reserve(src.size() - (sep_or_end + 1 - start));
                        result.append(src.data(), start);
                        result.append(src.data() + sep_or_end + 1, src.size() - sep_or_end - 1);
                    }
                    entry.set(std::move(result));
                    return;
                }
            }

            // General path: multiple indices or negative indices.
            CMakeArray list(entry.get());
            std::vector<size_t> positive_indices;
            for (const auto& idx_str : indices) {
                auto idx_opt = parse_number<long>(idx_str);
                if (!idx_opt) {
                    interp.set_fatal_error("list(REMOVE_AT) invalid index: " + idx_str);
                    return;
                }
                long idx = *idx_opt;
                if (idx < 0) { idx = static_cast<long>(list.size()) + idx; }
                if (idx < 0 || static_cast<size_t>(idx) >= list.size()) {
                    interp.set_fatal_error("list(REMOVE_AT) index out of range: " + idx_str);
                    return;
                }
                positive_indices.push_back(static_cast<size_t>(idx));
            }

            std::sort(positive_indices.begin(), positive_indices.end(), std::greater<size_t>());
            for (size_t idx : positive_indices) { list.erase(idx); }
            entry.set(list.to_string());
        } else if (operation == "FILTER") {
            CommandParser parser("list", "FILTER");
            std::string list_var, mode, regex_mode, pattern;
            parser.positional(list_var, "list variable");
            parser.positional(mode, "mode (INCLUDE/EXCLUDE)");
            parser.positional(regex_mode, "REGEX keyword");
            parser.positional(pattern, "regex pattern");
            PARSE_OR_RETURN(parser, interp, sub_args);

            std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char c) { return std::toupper(c); });
            std::transform(regex_mode.begin(), regex_mode.end(), regex_mode.begin(), [](unsigned char c) { return std::toupper(c); });

            if (mode != "INCLUDE" && mode != "EXCLUDE") {
                interp.set_fatal_error("list(FILTER) mode must be INCLUDE or EXCLUDE");
                return;
            }

            if (regex_mode != "REGEX") {
                interp.set_fatal_error("list(FILTER) requires REGEX keyword");
                return;
            }

            thread_local ClockCache<std::string, Regex> cache(8, [](const std::string& p) { return Regex::from_cmake_regex(p); });
            auto rx = cache.get(pattern);
            if (!rx) {
                interp.set_fatal_error("list(FILTER) invalid regex: " + rx.error());
                return;
            }

            auto entry = interp.get_variables().entry(list_var);
            const std::string& list_str = entry.get();
            bool include = (mode == "INCLUDE");
            std::string result_str;
            for (auto item : CMakeArrayIterator(list_str)) {
                bool matches = (*rx)->search(item);
                if ((include && matches) || (!include && !matches)) {
                    if (!result_str.empty()) result_str += ';';
                    result_str += item;
                }
            }
            entry.set(std::move(result_str));
        } else if (operation == "TRANSFORM") {
            // list(TRANSFORM <list> <ACTION> [<SELECTOR>] [OUTPUT_VARIABLE <output variable>])
            if (sub_args.size() < 2) {
                interp.set_fatal_error("list(TRANSFORM) requires at least list variable and action");
                return;
            }

            std::string list_var = sub_args[0];
            std::string action = sub_args[1];
            std::transform(action.begin(), action.end(), action.begin(), [](unsigned char c) { return std::toupper(c); });

            // Parse remaining arguments
            std::string output_var;
            std::string selector_mode;
            std::vector<std::string> selector_params;
            std::vector<std::string> action_args;

            size_t i = 2;
            while (i < sub_args.size()) {
                std::string arg = sub_args[i];
                std::string arg_upper = arg;
                std::transform(arg_upper.begin(), arg_upper.end(), arg_upper.begin(), [](unsigned char c) { return std::toupper(c); });

                if (arg_upper == "OUTPUT_VARIABLE") {
                    if (i + 1 >= sub_args.size()) {
                        interp.set_fatal_error("list(TRANSFORM) OUTPUT_VARIABLE requires a variable name");
                        return;
                    }
                    output_var = sub_args[++i];
                } else if (arg_upper == "AT") {
                    selector_mode = arg_upper;
                    // Collect all indices until we hit another keyword or end
                    ++i;
                    while (i < sub_args.size()) {
                        std::string next = sub_args[i];
                        std::string next_upper = next;
                        std::transform(next_upper.begin(), next_upper.end(), next_upper.begin(),
                                       [](unsigned char c) { return std::toupper(c); });
                        if (next_upper == "OUTPUT_VARIABLE" || next_upper == "FOR" || next_upper == "REGEX") {
                            --i; // Back up so the outer loop processes this keyword
                            break;
                        }
                        selector_params.push_back(next);
                        ++i;
                    }
                    if (selector_params.empty()) {
                        interp.set_fatal_error("list(TRANSFORM) AT requires at least one index");
                        return;
                    }
                } else if (arg_upper == "FOR") {
                    selector_mode = arg_upper;
                    // CMake FOR selector takes 2 or 3 separate arguments: start stop [step]
                    ++i;
                    while (i < sub_args.size() && selector_params.size() < 3) {
                        std::string next = sub_args[i];
                        std::string next_upper = next;
                        std::transform(next_upper.begin(), next_upper.end(), next_upper.begin(),
                                       [](unsigned char c) { return std::toupper(c); });
                        if (next_upper == "OUTPUT_VARIABLE" || next_upper == "AT" || next_upper == "REGEX") {
                            --i; // Back up so the outer loop processes this keyword
                            break;
                        }
                        selector_params.push_back(next);
                        ++i;
                    }
                    if (selector_params.size() < 2) {
                        interp.set_fatal_error("list(TRANSFORM) FOR requires at least start and stop");
                        return;
                    }
                    --i; // Compensate for the outer ++i
                } else if (arg_upper == "REGEX") {
                    selector_mode = arg_upper;
                    if (i + 1 >= sub_args.size()) {
                        interp.set_fatal_error("list(TRANSFORM) REGEX requires a pattern");
                        return;
                    }
                    selector_params.push_back(sub_args[++i]);
                } else {
                    // Action-specific arguments
                    action_args.push_back(arg);
                }
                ++i;
            }

            auto entry = interp.get_variables().entry(list_var);
            CMakeArray list(entry.get());
            CMakeArray result = list;

            // Lambda to apply transformation to an element
            auto transform_element = [&](std::string& element) {
                if (action == "APPEND") {
                    if (action_args.empty()) {
                        interp.set_fatal_error("list(TRANSFORM APPEND) requires a string argument");
                        return false;
                    }
                    element += action_args[0];
                } else if (action == "PREPEND") {
                    if (action_args.empty()) {
                        interp.set_fatal_error("list(TRANSFORM PREPEND) requires a string argument");
                        return false;
                    }
                    element = action_args[0] + element;
                } else if (action == "TOLOWER") {
                    std::transform(element.begin(), element.end(), element.begin(), [](unsigned char c) { return std::tolower(c); });
                } else if (action == "TOUPPER") {
                    std::transform(element.begin(), element.end(), element.begin(), [](unsigned char c) { return std::toupper(c); });
                } else if (action == "STRIP") {
                    element = strip(element);
                } else if (action == "REPLACE") {
                    if (action_args.size() < 2) {
                        interp.set_fatal_error("list(TRANSFORM REPLACE) requires regex and replacement string");
                        return false;
                    }
                    thread_local ClockCache<std::string, Regex> cache(8, [](const std::string& p) { return Regex::from_cmake_regex(p); });
                    auto rx = cache.get(action_args[0]);
                    if (!rx) {
                        interp.set_fatal_error("list(TRANSFORM REPLACE) invalid regex: " + rx.error());
                        return false;
                    }
                    element = (*rx)->replace_all(element, action_args[1]);
                } else {
                    interp.set_fatal_error("list(TRANSFORM) unknown action: " + action);
                    return false;
                }
                return true;
            };

            // Apply transformation based on selector
            if (selector_mode.empty()) {
                // Transform all elements
                for (size_t idx = 0; idx < result.size(); ++idx) {
                    std::string elem = result[idx];
                    if (!transform_element(elem)) return;
                    result = CMakeArray(result.to_vector());
                    // Need to rebuild list after modification
                    auto vec = result.to_vector();
                    vec[idx] = elem;
                    result = CMakeArray(vec);
                }
            } else if (selector_mode == "AT") {
                // Transform at specific indices
                auto vec = result.to_vector();
                for (const auto& idx_str : selector_params) {
                    auto idx_opt = parse_number<long>(idx_str);
                    if (!idx_opt) {
                        interp.set_fatal_error("list(TRANSFORM) invalid index in AT selector");
                        return;
                    }
                    long idx = *idx_opt;
                    if (idx < 0) { idx = static_cast<long>(result.size()) + idx; }
                    if (idx >= 0 && static_cast<size_t>(idx) < vec.size()) {
                        if (!transform_element(vec[idx])) return;
                    }
                }
                result = CMakeArray(vec);
            } else if (selector_mode == "REGEX") {
                // Transform elements matching regex
                if (selector_params.empty()) {
                    interp.set_fatal_error("list(TRANSFORM) REGEX requires a pattern");
                    return;
                }
                thread_local ClockCache<std::string, Regex> cache(8, [](const std::string& p) { return Regex::from_cmake_regex_match(p); });
                auto rx = cache.get(selector_params[0]);
                if (!rx) {
                    interp.set_fatal_error("list(TRANSFORM) invalid REGEX selector: " + rx.error());
                    return;
                }
                auto vec = result.to_vector();
                for (size_t idx = 0; idx < vec.size(); ++idx) {
                    if ((*rx)->match(vec[idx])) {
                        if (!transform_element(vec[idx])) return;
                    }
                }
                result = CMakeArray(vec);
            } else if (selector_mode == "FOR") {
                // Transform elements in range (start, stop[, step])
                // CMake syntax: FOR <start> <stop> [<step>] (separate arguments)
                if (selector_params.size() < 2) {
                    interp.set_fatal_error("list(TRANSFORM) FOR requires at least start and stop");
                    return;
                }

                {
                    auto start_opt = parse_number<long>(selector_params[0]);
                    auto stop_opt = parse_number<long>(selector_params[1]);
                    if (!start_opt || !stop_opt) {
                        interp.set_fatal_error("list(TRANSFORM) FOR invalid range parameters");
                        return;
                    }
                    long start = *start_opt;
                    long stop = *stop_opt;
                    long step = 1;
                    if (selector_params.size() >= 3) {
                        auto step_opt = parse_number<long>(selector_params[2]);
                        if (!step_opt) {
                            interp.set_fatal_error("list(TRANSFORM) FOR invalid step");
                            return;
                        }
                        step = *step_opt;
                        if (step == 0) {
                            interp.set_fatal_error("list(TRANSFORM) FOR step cannot be zero");
                            return;
                        }
                    }

                    // Handle negative indices
                    if (start < 0) { start = static_cast<long>(result.size()) + start; }
                    if (stop < 0) { stop = static_cast<long>(result.size()) + stop; }

                    auto vec = result.to_vector();

                    // Apply transformation to range
                    if (step > 0) {
                        for (long idx = start; idx <= stop && idx < static_cast<long>(vec.size()); idx += step) {
                            if (idx >= 0) {
                                if (!transform_element(vec[idx])) return;
                            }
                        }
                    } else {
                        for (long idx = start; idx >= stop && idx >= 0; idx += step) {
                            if (idx < static_cast<long>(vec.size())) {
                                if (!transform_element(vec[idx])) return;
                            }
                        }
                    }

                    result = CMakeArray(vec);
                }
            }

            // Store result
            if (output_var.empty()) {
                entry.set(result.to_string());
            } else {
                interp.set_variable(output_var, result.to_string());
            }
        } else {
            interp.set_fatal_error("Unknown list operation: " + operation);
        }
    });
}

} // namespace kiln
