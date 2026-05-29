#pragma once

#include "parse_number.hpp"
#include <cctype>
#include <cstddef>
#include <unistd.h>
#include <string>
#include <string_view>
#include <vector>

namespace kiln {
struct Hash256 {
    unsigned char bytes[32];

    std::string to_string() const;
};

struct Hash160 {
    unsigned char bytes[20];

    std::string to_string() const;
};

struct Hash128 {
    unsigned char bytes[16];

    std::string to_string() const;
};

/**
 * @brief Compute the BLAKE2b hash of the given data
 * @note When in doubt, use SHA3 or BLAKE2b. Both are safe and SHA3 is faster if
 * you are using OpenSSL and it has SHA3 in hardware mode. Otherwise BLAKE2b is
 * faster in software.
 */
Hash256 blake2b(const void* data, size_t len, const void* key, size_t keylen);
inline Hash256 blake2b(std::string_view str, std::string_view key = "") {
    return blake2b(str.data(), str.size(), key.data(), key.size());
}

/**
 * @brief Compute the SHA256 hash of the given data
 */
Hash256 sha256(const void* data, size_t len);
inline Hash256 sha256(std::string_view str) {
    return sha256(str.data(), str.size());
}

/**
 * @brief Compute the SHA1 hash of the given data
 */
Hash160 sha1(const void* data, size_t len);
inline Hash160 sha1(std::string_view str) {
    return sha1(str.data(), str.size());
}

Hash128 md5(const void* data, size_t len);
inline Hash128 md5(std::string_view str) {
    return md5(str.data(), str.size());
}

struct CommandResult {
    int exit_code;
    std::string output;
};

/**
 * @brief Escapes a string for use as a shell argument.
 */
std::string escape_shell_arg(const std::string& arg);

/**
 * @brief Joins a command vector into a single string, escaping each argument.
 */
std::string join_command(const std::vector<std::string>& args);

/**
 * @brief Joins a command vector with spaces, no escaping.
 * Used for custom commands where args contain user shell syntax.
 */
std::string join_command_raw(const std::vector<std::string>& args);

/**
 * @brief Strip shell-level quoting from a string.
 * CMake preserves quotes in COMMAND args; since we use execvp we must strip them.
 */
std::string strip_shell_quoting(const std::string& arg);

/**
 * @brief Execute a shell command and capture its output.
 * @param command The command to execute.
 * @param working_dir The directory to run the command in (empty for current dir).
 * @return CommandResult containing exit code and combined stdout/stderr.
 */
CommandResult run_command(const std::string& command, const std::string& working_dir = "");

/**
 * @brief Execute a command (vector of args) and capture its output.
 */
CommandResult run_command(const std::vector<std::string>& command, const std::string& working_dir = "");

/**
 * @brief Get the absolute path to the current executable.
 */
std::string get_executable_path();

struct ProcessOptions {
    std::string working_dir;
    std::string input_file;
    std::string output_file;
    std::string error_file;
    bool output_quiet = false;
    bool error_quiet = false;
    double timeout = 0.0; // Seconds, 0 means no timeout
    // If not empty, stdout/stderr will be captured here
    std::string* output_variable = nullptr;
    std::string* error_variable = nullptr;
};

struct PipelineResult {
    std::vector<int> exit_codes;
    std::string captured_stdout;
    std::string captured_stderr;
    std::string setup_error;
};

/**
 * @brief Execute a pipeline of commands.
 */
PipelineResult execute_pipeline(const std::vector<std::vector<std::string>>& commands, const ProcessOptions& options = {});

/**
 * @brief Case-insensitive equality comparison (no allocation).
 */
inline bool ci_equals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        auto upper = [](unsigned char c) -> unsigned char { return (c >= 'a' && c <= 'z') ? static_cast<unsigned char>(c - 32) : c; };
        if (upper(a[i]) != upper(b[i])) return false;
    }
    return true;
}

/**
 * @brief Convert string to uppercase using current locale.
 */
std::string to_upper(std::string_view str);

/**
 * @brief Convert string to lowercase using current locale.
 */
std::string to_lower(std::string_view str);

/**
 * @brief Remove leading and trailing whitespace.
 * @return View into input string (valid while input is alive)
 */
std::string_view strip(std::string_view str);

/**
 * @brief Remove leading whitespace.
 */
std::string_view lstrip(std::string_view str);

/**
 * @brief Remove trailing whitespace.
 */
std::string_view rstrip(std::string_view str);

/**
 * @brief Replace all occurrences of a substring with another.
 * @param str Input string (will be copied)
 * @param from Substring to find
 * @param to Replacement substring
 * @return New string with all replacements made
 */
std::string replace_all(std::string str, std::string_view from, std::string_view to);

/**
 * @brief Split a string into tokens with shell-like quote handling.
 * Handles both single and double quotes as grouping delimiters (stripped from output).
 */
std::vector<std::string> shell_split(std::string_view input);

/**
 * @brief Get the GNU architecture triplet for the current host (e.g. "x86_64-linux-gnu", "aarch64-linux-gnu").
 * Returns empty string on non-Linux or unrecognized architectures.
 * Result is cached after the first call.
 */
const std::string& gnu_arch_triplet();

/**
 * @brief If /usr/share/cmake/Modules exists (Arch layout), returns empty string.
 * Otherwise lists /usr/share/, filters cmake-X.Y entries, picks the highest version
 * that has a Modules/ subdirectory, and returns that path (e.g. "/usr/share/cmake-3.28").
 * Result is cached after the first call.
 */
const std::string& cmake_extra_modules_root();

/**
 * @brief Compare CMake version strings component-wise.
 * @return -1 if a < b, 0 if a == b, 1 if a > b
 * Split by '.', each component parsed as integer (non-numeric suffix stripped),
 * missing components treated as 0.
 */
inline int compare_versions(std::string_view a, std::string_view b) {
    // Parse version component as unsigned leading digits only.
    // CMake treats non-digit leading chars (including '-') as 0.
    auto parse_component = [](std::string_view s) -> unsigned {
        unsigned val = 0;
        for (char c : s) {
            if (c < '0' || c > '9') break;
            val = val * 10 + static_cast<unsigned>(c - '0');
        }
        return val;
    };

    auto parse_parts = [&](std::string_view v) {
        std::vector<unsigned> parts;
        while (!v.empty()) {
            auto dot = v.find('.');
            parts.push_back(parse_component(v.substr(0, dot)));
            v = (dot == std::string_view::npos) ? std::string_view{} : v.substr(dot + 1);
        }
        return parts;
    };

    auto pa = parse_parts(a);
    auto pb = parse_parts(b);
    size_t len = std::max(pa.size(), pb.size());
    pa.resize(len, 0u);
    pb.resize(len, 0u);

    for (size_t i = 0; i < len; ++i) {
        if (pa[i] < pb[i]) return -1;
        if (pa[i] > pb[i]) return 1;
    }
    return 0;
}

} // namespace kiln
