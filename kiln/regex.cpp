#define PCRE2_CODE_UNIT_WIDTH 8
#include "regex.hpp"
#include <pcre2.h>

namespace kiln {

namespace {
struct CodeDeleter {
    void operator()(pcre2_code* p) const noexcept {
        if (p) pcre2_code_free(p);
    }
};
struct MatchDataDeleter {
    void operator()(pcre2_match_data* p) const noexcept {
        if (p) pcre2_match_data_free(p);
    }
};
} // namespace

struct Regex::Impl {
    std::unique_ptr<pcre2_code, CodeDeleter> code;
    std::unique_ptr<pcre2_match_data, MatchDataDeleter> match_data;
};

// Normalize CMake regex patterns for PCRE2 compatibility.
//
// CMake regex spec: \<char> matches the literal character <char>.
// Special regex chars: ^ $ . \ [ ] * + ? | ( )
// Escaping non-special chars is allowed: \a matches a, \h matches h, etc.
//
// PCRE2 gives special meaning to escapes like \t (tab), \n (newline), \d (digit),
// \s (whitespace), \h (horizontal space), and errors on unknown escapes like \o.
//
// This function converts CMake regex to PCRE2-compatible form:
// - \<metachar> stays as \<metachar> (escaping special chars)
// - \<other> becomes just <other> (CMake treats as literal)
static std::string normalize_cmake_regex(std::string_view pattern, std::string* warning_out = nullptr) {
    // CMake regex metacharacters: ^ $ . \ [ ] * + ? | ( )
    auto is_cmake_metachar = [](char c) -> bool {
        switch (c) {
        case '^':
        case '$':
        case '.':
        case '\\':
        case '[':
        case ']':
        case '*':
        case '+':
        case '?':
        case '|':
        case '(':
        case ')':
            return true;
        default:
            return false;
        }
    };

    std::string result;
    result.reserve(pattern.size());
    std::string normalized_escapes; // Track which escapes were normalized
    bool in_bracket = false;        // Inside a character class [...]
    bool bracket_first = false;     // Next char is first in class (where ] is literal)

    for (size_t i = 0; i < pattern.size(); ++i) {
        if (in_bracket) {
            // CMake regex uses POSIX ERE semantics inside character classes:
            // - Backslash has NO special meaning, it's a literal backslash
            // - ] closes the class (unless it's the first char after [ or [^)
            // PCRE2 treats \ as an escape inside brackets, so we must double it.
            if (pattern[i] == ']' && !bracket_first) {
                in_bracket = false;
                result += ']';
            } else if (pattern[i] == '\\') {
                result += "\\\\"; // Literal backslash for PCRE2
                bracket_first = false;
            } else {
                if (bracket_first && pattern[i] != '^') { bracket_first = false; }
                result += pattern[i];
            }
        } else if (pattern[i] == '[') {
            in_bracket = true;
            bracket_first = true;
            result += '[';
        } else if (pattern[i] == '\\' && i + 1 < pattern.size()) {
            char next = pattern[i + 1];
            if (is_cmake_metachar(next)) {
                // Escaping a metacharacter - keep the escape for PCRE2
                result += '\\';
                result += next;
            } else {
                // CMake: \<non-meta> matches the literal char
                // Just output the character without backslash
                result += next;
                // Record the first few unique normalized escapes for warning
                if (normalized_escapes.size() < 30) {
                    std::string esc = "\\";
                    esc += next;
                    if (normalized_escapes.find(esc) == std::string::npos) {
                        if (!normalized_escapes.empty()) normalized_escapes += " ";
                        normalized_escapes += esc;
                    }
                }
            }
            ++i;
        } else {
            result += pattern[i];
        }
    }

    if (warning_out && !normalized_escapes.empty()) {
        *warning_out =
            "Regex contains non-special escape sequences (" + normalized_escapes + ") - these match literal characters in CMake regex";
    }
    return result;
}

static std::expected<std::unique_ptr<Regex::Impl>, std::string> compile_raw(std::string_view pattern) {
    int errcode;
    PCRE2_SIZE erroffset;

    std::unique_ptr<pcre2_code, CodeDeleter> code(pcre2_compile(reinterpret_cast<PCRE2_SPTR>(pattern.data()), pattern.size(),
                                                                PCRE2_UTF | PCRE2_NO_UTF_CHECK | PCRE2_DOTALL, &errcode, &erroffset,
                                                                nullptr));

    if (!code) {
        PCRE2_UCHAR buf[256];
        pcre2_get_error_message(errcode, buf, sizeof(buf));
        return std::unexpected(std::string(reinterpret_cast<const char*>(buf)));
    }

    // JIT compile for speed (best-effort, fallback to interpreter on failure)
    pcre2_jit_compile(code.get(), PCRE2_JIT_COMPLETE);

    auto impl = std::make_unique<Regex::Impl>();
    impl->match_data.reset(pcre2_match_data_create_from_pattern(code.get(), nullptr));
    impl->code = std::move(code);

    return impl;
}

std::expected<Regex, std::string> Regex::compile(std::string_view pattern) {
    auto result = compile_raw(pattern);
    if (!result) return std::unexpected(result.error());
    return Regex(std::move(*result));
}

std::expected<Regex, std::string> Regex::compile_match(std::string_view pattern) {
    std::string anchored = "^(?:";
    anchored.append(pattern);
    anchored += ")$";
    auto result = compile_raw(anchored);
    if (!result) return std::unexpected(result.error());
    return Regex(std::move(*result));
}

std::expected<Regex, std::string> Regex::from_cmake_regex(std::string_view pattern, std::string* warning) {
    std::string normalized = normalize_cmake_regex(pattern, warning);
    auto result = compile_raw(normalized);
    if (!result) return std::unexpected(result.error());
    return Regex(std::move(*result));
}

std::expected<Regex, std::string> Regex::from_cmake_regex_match(std::string_view pattern, std::string* warning) {
    std::string normalized = normalize_cmake_regex(pattern, warning);
    std::string anchored = "^(?:";
    anchored.append(normalized);
    anchored += ")$";
    auto result = compile_raw(anchored);
    if (!result) return std::unexpected(result.error());
    return Regex(std::move(*result));
}

Regex::Regex(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Regex::~Regex() = default;
Regex::Regex(Regex&&) noexcept = default;
Regex& Regex::operator=(Regex&&) noexcept = default;

static bool do_match(const Regex::Impl* impl, std::string_view input, std::vector<std::string>* captures) {
    int rc = pcre2_match(impl->code.get(), reinterpret_cast<PCRE2_SPTR>(input.data()), input.size(), 0, 0, impl->match_data.get(), nullptr);

    if (rc < 0) return false;

    if (captures) {
        captures->clear();
        PCRE2_SIZE* ovector = pcre2_get_ovector_pointer(impl->match_data.get());
        uint32_t count = pcre2_get_ovector_count(impl->match_data.get());

        for (uint32_t i = 0; i < count; ++i) {
            if (ovector[2 * i] == PCRE2_UNSET) {
                captures->emplace_back();
            } else {
                captures->emplace_back(input.data() + ovector[2 * i], ovector[2 * i + 1] - ovector[2 * i]);
            }
        }
    }

    return true;
}

bool Regex::search(std::string_view input, std::vector<std::string>& captures) const {
    return do_match(impl_.get(), input, &captures);
}

bool Regex::search(std::string_view input) const {
    return do_match(impl_.get(), input, nullptr);
}

bool Regex::match(std::string_view input, std::vector<std::string>& captures) const {
    return do_match(impl_.get(), input, &captures);
}

bool Regex::match(std::string_view input) const {
    return do_match(impl_.get(), input, nullptr);
}

std::vector<std::vector<std::string>> Regex::match_all(std::string_view input) const {
    std::vector<std::vector<std::string>> results;
    PCRE2_SIZE offset = 0;

    while (offset <= input.size()) {
        int rc = pcre2_match(impl_->code.get(), reinterpret_cast<PCRE2_SPTR>(input.data()), input.size(), offset, 0,
                             impl_->match_data.get(), nullptr);

        if (rc < 0) break;

        PCRE2_SIZE* ovector = pcre2_get_ovector_pointer(impl_->match_data.get());
        uint32_t count = pcre2_get_ovector_count(impl_->match_data.get());

        std::vector<std::string> groups;
        for (uint32_t i = 0; i < count; ++i) {
            if (ovector[2 * i] == PCRE2_UNSET) {
                groups.emplace_back();
            } else {
                groups.emplace_back(input.data() + ovector[2 * i], ovector[2 * i + 1] - ovector[2 * i]);
            }
        }
        results.push_back(std::move(groups));

        // Advance past the match; handle zero-length matches
        PCRE2_SIZE match_end = ovector[1];
        if (match_end == offset) {
            offset++;
        } else {
            offset = match_end;
        }
    }

    return results;
}

std::string Regex::replace_all(std::string_view input, std::string_view replacement) const {
    // Convert CMake-style \1 \2 to PCRE2 $1 $2 syntax
    std::string pcre2_repl;
    pcre2_repl.reserve(replacement.size());

    for (size_t i = 0; i < replacement.size(); ++i) {
        if (replacement[i] == '\\' && i + 1 < replacement.size()) {
            char next = replacement[i + 1];
            if (next >= '0' && next <= '9') {
                pcre2_repl += '$';
                pcre2_repl += next;
                ++i;
                continue;
            }
            if (next == '\\') {
                pcre2_repl += '\\';
                ++i;
                continue;
            }
            // Other escape — pass through
            pcre2_repl += replacement[i];
        } else if (replacement[i] == '$') {
            // Literal $ needs escaping as $$
            pcre2_repl += "$$";
        } else {
            pcre2_repl += replacement[i];
        }
    }

    // First call to determine output length
    PCRE2_SIZE out_len = 0;
    int rc = pcre2_substitute(impl_->code.get(), reinterpret_cast<PCRE2_SPTR>(input.data()), input.size(), 0,
                              PCRE2_SUBSTITUTE_GLOBAL | PCRE2_SUBSTITUTE_OVERFLOW_LENGTH, impl_->match_data.get(), nullptr,
                              reinterpret_cast<PCRE2_SPTR>(pcre2_repl.data()), pcre2_repl.size(), nullptr, &out_len);

    if (rc == PCRE2_ERROR_NOMATCH) { return std::string(input); }

    if (rc != PCRE2_ERROR_NOMEMORY && rc < 0) { return std::string(input); }

    // Allocate and perform the substitution
    PCRE2_SIZE result_len = out_len + 1; // pcre2 needs space for NUL
    std::string result(result_len, '\0');

    rc = pcre2_substitute(impl_->code.get(), reinterpret_cast<PCRE2_SPTR>(input.data()), input.size(), 0, PCRE2_SUBSTITUTE_GLOBAL,
                          impl_->match_data.get(), nullptr, reinterpret_cast<PCRE2_SPTR>(pcre2_repl.data()), pcre2_repl.size(),
                          reinterpret_cast<PCRE2_UCHAR*>(result.data()), &result_len);

    if (rc < 0) { return std::string(input); }

    result.resize(result_len);
    return result;
}

} // namespace kiln
