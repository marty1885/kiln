#include "utils.hpp"
#include "build_system.hpp"
#include "container_utils.hpp"
#include "inner/blake2b.h"
#include "inner/sha256.h"
#include "inner/md5.h"
#include "inner/sha1.h"
#ifdef __linux__
#include <linux/limits.h>
#include <sys/utsname.h>
#elif defined __FreeBSD__
#include <sys/sysctl.h>
#endif
#include <type_traits>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <signal.h>
#include <cstring>
#include <chrono>
#include <algorithm>
#include <charconv>
#include <filesystem>

kiln::Hash256 kiln::blake2b(const void* data, size_t len, const void* key, size_t keylen) {
    Hash256 hash;
    static_assert(sizeof(hash) == 32, "Hash256 must be 32 bytes");
    static_assert(std::is_standard_layout<Hash256>::value, "Hash256 must be a standard layout type");
    kiln_blake2b(&hash, sizeof(hash), data, len, key, keylen);
    return hash;
}

kiln::Hash256 kiln::sha256(const void* data, size_t len) {
    Hash256 hash;
    SHA256_CTX ctx;
    kiln_sha256_init(&ctx);
    kiln_sha256_update(&ctx, (const uint8_t*) data, len);
    kiln_sha256_final(&ctx, (uint8_t*) &hash);
    return hash;
}

kiln::Hash160 kiln::sha1(const void* data, size_t len) {
    Hash160 hash;
    SHA1_CTX ctx;
    kiln_sha1_init(&ctx);
    kiln_sha1_update(&ctx, (const unsigned char*) data, len);
    kiln_sha1_final((unsigned char*) &hash, &ctx);
    return hash;
}

kiln::Hash128 kiln::md5(const void* data, size_t len) {
    Hash128 hash;
    MD5_CTX ctx;
    kiln_md5_init(&ctx);
    kiln_md5_update(&ctx, (const uint8_t*) data, len);
    kiln_md5_final(&ctx, (uint8_t*) &hash);
    return hash;
}

std::string kiln::Hash256::to_string() const {
    std::string result;
    result.reserve(2 * sizeof(bytes));
    for (unsigned char byte : bytes) {
        result += "0123456789abcdef"[byte >> 4];
        result += "0123456789abcdef"[byte & 0xf];
    }
    return result;
}

std::string kiln::Hash160::to_string() const {
    std::string result;
    result.reserve(2 * sizeof(bytes));
    for (unsigned char byte : bytes) {
        result += "0123456789abcdef"[byte >> 4];
        result += "0123456789abcdef"[byte & 0xf];
    }
    return result;
}

std::string kiln::Hash128::to_string() const {
    std::string result;
    result.reserve(2 * sizeof(bytes));
    for (unsigned char byte : bytes) {
        result += "0123456789abcdef"[byte >> 4];
        result += "0123456789abcdef"[byte & 0xf];
    }
    return result;
}

kiln::CommandResult kiln::run_command(const std::string& command, const std::string& working_dir) {
    // Avoid popen/sh whenever possible. shell_split handles quoting/escapes
    // exactly like the shell would for the simple word-splitting case; the
    // vector overload then handles redirects (<, >, >>, 2>) natively via
    // fork+dup2 and only falls back to spawning a shell for genuine shell
    // pipelines/logic (|, &&, ||, 2>&1). This is critical: the string
    // overload was previously the hottest user of process-global chdir,
    // which races between concurrent build workers.
    auto argv = shell_split(command);
    if (argv.empty()) { return {-1, "Empty command"}; }
    return run_command(argv, working_dir);
}

std::string kiln::escape_shell_arg(const std::string& arg) {
    if (arg.empty()) return "''";

    bool needed = false;
    for (char c : arg) {
        if (std::isspace(static_cast<unsigned char>(c)) || c == '\'' || c == '"' || c == '\\' || c == '`' || c == '$' || c == '&'
            || c == '|' || c == ';' || c == '<' || c == '>' || c == '(' || c == ')' || c == '{' || c == '}' || c == '[' || c == ']'
            || c == '*' || c == '?' || c == '~' || c == '#' || c == '!' || c == '^') {
            needed = true;
            break;
        }
    }

    if (!needed) return arg;

    std::string result = "'";
    for (char c : arg) {
        if (c == '\'') {
            result += "'\\''";
        } else {
            result += c;
        }
    }
    result += "'";
    return result;
}

// Check if arg starts with a shell redirection prefix.
// Returns the length of the prefix (0 if none).
// Handles: >>, 2>>, 2>, 1>, >, <
static size_t shell_redirect_prefix_len(const std::string& arg) {
    // Order matters: check longer prefixes first
    if (arg.starts_with("2>>")) return 3;
    if (arg.starts_with("1>>")) return 3;
    if (arg.starts_with(">>")) return 2;
    if (arg.starts_with("2>")) return 2;
    if (arg.starts_with("1>")) return 2;
    if (arg.starts_with(">")) return 1;
    if (arg.starts_with("<")) return 1;
    return 0;
}

// Find an embedded redirect operator within an argument (not at position 0).
// Returns {position, operator_length} or {npos, 0} if none found.
// Handles cases like "file.sql>" or "file.sql>output" where the redirect
// is glued to the preceding text (common in CMake COMMAND arguments).
static std::pair<size_t, size_t> find_embedded_redirect(const std::string& arg) {
    for (size_t i = 1; i < arg.size(); ++i) {
        if (arg[i] == '>') {
            if (i + 1 < arg.size() && arg[i + 1] == '>') {
                return {i, 2}; // >>
            }
            return {i, 1}; // >
        }
        if (arg[i] == '<') {
            return {i, 1}; // <
        }
    }
    return {std::string::npos, 0};
}

static bool is_shell_operator(const std::string& arg) {
    return arg == "|" || arg == "&&" || arg == "||" || arg == "2>&1" || arg == "(" || arg == ")";
}

std::string kiln::join_command(const std::vector<std::string>& args) {
    std::string result;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) result += " ";
        if (is_shell_operator(args[i])) {
            result += args[i];
        } else if (size_t pfx = shell_redirect_prefix_len(args[i])) {
            // Split redirection operator from path: ">file" → > + escaped(file)
            result += args[i].substr(0, pfx);
            std::string path = args[i].substr(pfx);
            if (!path.empty()) { result += escape_shell_arg(path); }
        } else if (auto [pos, len] = find_embedded_redirect(args[i]); pos != std::string::npos) {
            // Split embedded redirect: "file.sql>" → escaped(file.sql) + " >" + escaped(path)
            result += escape_shell_arg(args[i].substr(0, pos));
            result += " ";
            result += args[i].substr(pos, len);
            std::string path = args[i].substr(pos + len);
            if (!path.empty()) { result += escape_shell_arg(path); }
        } else {
            result += escape_shell_arg(args[i]);
        }
    }
    return result;
}

std::string kiln::join_command_raw(const std::vector<std::string>& args) {
    return join(args, " ");
}

// Strip shell-style embedded quoting from a COMMAND argument.
//
// CMake allows embedded quoted segments in unquoted arguments like:
//   -flag="${VAR}"  →  should become -flag=value (quotes are shell grouping)
//
// But we must NOT strip quotes that are content, like:
//   print("hello")  →  should stay as print("hello") (quotes are Python syntax)
//
// The heuristic: only strip quotes that form a "value segment" pattern:
//   - Quotes immediately after = (like -DFOO="bar")
//   - Quotes that wrap the entire argument (like "value")
//   - Single quotes following shell conventions
//
// We do NOT strip quotes that appear mid-content (like function calls).
std::string kiln::strip_shell_quoting(const std::string& arg) {
    if (arg.empty()) return arg;

    // Case 1: Entire argument is quoted (rare, but handle it)
    if ((arg.front() == '"' && arg.back() == '"' && arg.size() >= 2) || (arg.front() == '\'' && arg.back() == '\'' && arg.size() >= 2)) {
        return arg.substr(1, arg.size() - 2);
    }

    // Case 2: Pattern like -flag="value" or VAR="value"
    // Look for =" or =' and strip the quotes around the value part
    std::string result;
    result.reserve(arg.size());

    for (size_t i = 0; i < arg.size(); ++i) {
        // Check for =" or =' pattern
        if (arg[i] == '=' && i + 1 < arg.size() && (arg[i + 1] == '"' || arg[i + 1] == '\'')) {
            char quote = arg[i + 1];
            size_t end = arg.find(quote, i + 2);
            if (end != std::string::npos && (end == arg.size() - 1 || arg[end + 1] == '=' || arg[end + 1] == ' ')) {
                // Found a complete quoted value segment after =
                result += '=';
                result.append(arg, i + 2, end - i - 2);
                i = end;
                continue;
            }
        }
        result += arg[i];
    }

    return result;
}

// Resolved command: argv is what we'll exec; the *_file fields are paths
// to redirect the child's std fds to (empty = inherit/pipe).
struct ResolvedCommand {
    std::vector<std::string> argv;
    std::string stdin_file;
    std::string stdout_file;
    std::string stderr_file;
    bool stdout_append = false;
};

// Fork + execvp the resolved command. No parsing — argv and redirects are
// applied verbatim. Caller is responsible for ensuring argv is non-empty.
static kiln::CommandResult exec_resolved(const ResolvedCommand& rc, const std::string& working_dir) {
    int pipe_fd[2];
    if (pipe(pipe_fd) != 0) return {-1, "Failed to create pipe"};

    pid_t pid = fork();
    if (pid < 0) {
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        return {-1, "Failed to fork"};
    }

    if (pid == 0) {
        // Put child in its own process group so the terminal's Ctrl-C goes
        // only to kiln; kiln then forwards SIGINT explicitly. Reset signal
        // dispositions so the forwarded signal actually terminates the child.
        setpgid(0, 0);
        // Ensure the child dies if kiln crashes (segfault, SIGKILL, etc.).
        // Linux/FreeBSD only — see set_parent_death_signal.
        kiln::set_parent_death_signal(SIGTERM);
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGHUP, SIG_DFL);

        close(pipe_fd[0]);

        if (!working_dir.empty()) {
            if (chdir(working_dir.c_str()) != 0) _exit(127);
        }

        if (!rc.stdin_file.empty()) {
            int fd = open(rc.stdin_file.c_str(), O_RDONLY);
            if (fd < 0) _exit(127);
            dup2(fd, STDIN_FILENO);
            close(fd);
        }
        if (!rc.stdout_file.empty()) {
            int flags = O_WRONLY | O_CREAT | (rc.stdout_append ? O_APPEND : O_TRUNC);
            int fd = open(rc.stdout_file.c_str(), flags, 0644);
            if (fd < 0) _exit(127);
            dup2(fd, STDOUT_FILENO);
            close(fd);
        } else {
            dup2(pipe_fd[1], STDOUT_FILENO);
        }
        if (!rc.stderr_file.empty()) {
            int fd = open(rc.stderr_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) _exit(127);
            dup2(fd, STDERR_FILENO);
            close(fd);
        } else {
            dup2(pipe_fd[1], STDERR_FILENO);
        }
        close(pipe_fd[1]);

        std::vector<const char*> argv;
        argv.reserve(rc.argv.size() + 1);
        for (const auto& a : rc.argv) argv.push_back(a.c_str());
        argv.push_back(nullptr);

        execvp(argv[0], const_cast<char* const*>(argv.data()));
        fprintf(stderr, "exec: %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }

    // Parent side of setpgid: avoid a race with the child's execvp by setting
    // its pgrp here too. setpgid is idempotent and one of the two calls will
    // win; if the child already exec'd, EACCES is ignored.
    setpgid(pid, pid);
    kiln::register_child_pid(pid);

    close(pipe_fd[1]);

    std::string output;
    char buffer[4096];
    ssize_t n;
    while ((n = read(pipe_fd[0], buffer, sizeof(buffer))) > 0) { output.append(buffer, n); }
    close(pipe_fd[0]);

    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    kiln::unregister_child_pid(pid);
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    return {exit_code, output};
}

kiln::CommandResult kiln::run_command(const std::vector<std::string>& command, const std::string& working_dir) {
    if (command.empty()) return {-1, "Empty command"};

    // Parse command vector: separate program+args from redirections.
    // This lets us handle redirects via dup2() without needing a shell.
    // Redirect operators can appear as standalone args (">"), prefixes (">file"),
    // or embedded within args ("file>out", "file.sql>"). The embedded case
    // arises when CMake source glues a redirect to the preceding token
    // (e.g. `mariadb_sys_schema.sql>` on one line, `output.sql` on the next).
    //
    // If we encounter true shell logic (|, ||, &&, 2>&1), we hand the whole
    // command off to /bin/sh -c verbatim and skip our own redirect handling
    // entirely — the shell will parse it. We must NOT recurse through this
    // function with `{"/bin/sh","-c",joined}` because the joined command
    // contains literal `>` / `<` that find_embedded_redirect would split on.
    ResolvedCommand rc;

    for (size_t i = 0; i < command.size(); ++i) {
        const auto& arg = command[i];

        if (arg == "|" || arg == "&&" || arg == "||" || arg == "2>&1") {
            ResolvedCommand shell_rc;
            shell_rc.argv = {"/bin/sh", "-c", join(command, " ")};
            return exec_resolved(shell_rc, working_dir);
        }

        size_t pfx = shell_redirect_prefix_len(arg);
        if (pfx > 0) {
            std::string op = arg.substr(0, pfx);
            std::string path = arg.substr(pfx);
            if (path.empty() && i + 1 < command.size()) { path = command[++i]; }
            path = strip_shell_quoting(path);

            if (op == "<")
                rc.stdin_file = path;
            else if (op == ">>" || op == "1>>") {
                rc.stdout_file = path;
                rc.stdout_append = true;
            } else if (op == ">" || op == "1>")
                rc.stdout_file = path;
            else if (op == "2>" || op == "2>>")
                rc.stderr_file = path;
        } else if (auto [pos, len] = find_embedded_redirect(arg); pos != std::string::npos) {
            std::string pre = arg.substr(0, pos);
            std::string op = arg.substr(pos, len);
            std::string path = arg.substr(pos + len);
            if (path.empty() && i + 1 < command.size()) { path = command[++i]; }
            path = strip_shell_quoting(path);

            if (!pre.empty()) rc.argv.push_back(pre);

            if (op == "<")
                rc.stdin_file = path;
            else if (op == ">>") {
                rc.stdout_file = path;
                rc.stdout_append = true;
            } else if (op == ">")
                rc.stdout_file = path;
        } else {
            rc.argv.push_back(arg);
        }
    }

    if (rc.argv.empty()) return {-1, "Empty command after parsing"};
    return exec_resolved(rc, working_dir);
}

std::string kiln::get_executable_path() {
    char result[PATH_MAX];
#ifdef __linux__
    ssize_t count = readlink("/proc/self/exe", result, PATH_MAX);
    if (count != -1) { return std::string(result, count); }
#elif defined __FreeBSD__
    size_t len = sizeof(result);
    int mib[] = {CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, getpid()};
    if (sysctl(mib, 4, result, &len, NULL, 0) == 0) return result;
#endif
    // Fallback or other platforms
    return "kiln";
}

kiln::PipelineResult kiln::execute_pipeline(const std::vector<std::vector<std::string>>& commands, const ProcessOptions& options) {
    if (commands.empty()) return {};

    size_t num_commands = commands.size();
    std::vector<int> pids(num_commands);
    std::vector<std::vector<int>> pipes(num_commands - 1, std::vector<int>(2));

    for (size_t i = 0; i < num_commands - 1; ++i) {
        if (pipe(pipes[i].data()) == -1) { return {{1}, "", "", "Failed to create pipe"}; }
    }

    int stdout_pipe[2];
    int stderr_pipe[2];
    if (options.output_variable) {
        if (pipe(stdout_pipe) == -1) { return {{1}, "", "", "Failed to create stdout pipe"}; }
    }
    if (options.error_variable) {
        if (pipe(stderr_pipe) == -1) { return {{1}, "", "", "Failed to create stderr pipe"}; }
    }

    int setup_pipe[2];
    if (pipe(setup_pipe) == -1) { return {{1}, "", "", "Failed to create setup pipe"}; }

    for (size_t i = 0; i < num_commands; ++i) {
        pids[i] = fork();
        if (pids[i] == 0) { // Child
            // Own process group so terminal Ctrl-C doesn't reach the child;
            // kiln forwards signals explicitly via the tracked-pid registry.
            setpgid(0, 0);
            signal(SIGINT, SIG_DFL);
            signal(SIGTERM, SIG_DFL);
            signal(SIGHUP, SIG_DFL);

            close(setup_pipe[0]);
            auto report_setup_error = [&](const std::string& message) {
                std::string msg = message + ": " + std::strerror(errno);
                ssize_t ignored = write(setup_pipe[1], msg.data(), msg.size());
                (void) ignored;
                _exit(127);
            };

            if (!options.working_dir.empty()) {
                if (chdir(options.working_dir.c_str()) != 0) { report_setup_error("Failed to change directory to " + options.working_dir); }
            }

            // Stdin
            if (i == 0) {
                if (!options.input_file.empty()) {
                    int fd = open(options.input_file.c_str(), O_RDONLY);
                    if (fd == -1) { report_setup_error("Failed to open INPUT_FILE " + options.input_file); }
                    if (dup2(fd, STDIN_FILENO) == -1) { report_setup_error("Failed to redirect INPUT_FILE " + options.input_file); }
                    close(fd);
                }
            } else {
                dup2(pipes[i - 1][0], STDIN_FILENO);
            }

            // Stdout
            if (i == num_commands - 1) {
                if (options.output_variable) {
                    dup2(stdout_pipe[1], STDOUT_FILENO);
                } else if (!options.output_file.empty()) {
                    int fd = open(options.output_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
                    if (fd == -1) { report_setup_error("Failed to open OUTPUT_FILE " + options.output_file); }
                    if (dup2(fd, STDOUT_FILENO) == -1) { report_setup_error("Failed to redirect OUTPUT_FILE " + options.output_file); }
                    close(fd);
                } else if (options.output_quiet) {
                    int fd = open("/dev/null", O_WRONLY);
                    if (fd == -1 || dup2(fd, STDOUT_FILENO) == -1) { report_setup_error("Failed to redirect stdout to /dev/null"); }
                    close(fd);
                }
            } else {
                dup2(pipes[i][1], STDOUT_FILENO);
            }

            // Stderr
            if (options.error_variable) {
                dup2(stderr_pipe[1], STDERR_FILENO);
            } else if (!options.error_file.empty()) {
                int fd = open(options.error_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fd == -1) { report_setup_error("Failed to open ERROR_FILE " + options.error_file); }
                if (dup2(fd, STDERR_FILENO) == -1) { report_setup_error("Failed to redirect ERROR_FILE " + options.error_file); }
                close(fd);
            } else if (options.error_quiet) {
                int fd = open("/dev/null", O_WRONLY);
                if (fd == -1 || dup2(fd, STDERR_FILENO) == -1) { report_setup_error("Failed to redirect stderr to /dev/null"); }
                close(fd);
            }

            // Close all pipes in child
            for (auto& p : pipes) {
                close(p[0]);
                close(p[1]);
            }
            if (options.output_variable) {
                close(stdout_pipe[0]);
                close(stdout_pipe[1]);
            }
            if (options.error_variable) {
                close(stderr_pipe[0]);
                close(stderr_pipe[1]);
            }
            close(setup_pipe[1]);

            // Exec
            std::vector<char*> argv;
            for (const auto& arg : commands[i]) argv.push_back(const_cast<char*>(arg.c_str()));
            argv.push_back(nullptr);

            execvp(argv[0], argv.data());
            // exec failed — surface via setup_pipe so the parent raises a
            // fatal error with a stack trace, instead of letting a stray
            // "command not found" silently flow through RESULT_VARIABLE.
            report_setup_error(std::string("exec ") + argv[0]);
        }
        // Parent-side setpgid mirrors the child's call to close the race window
        // before exec. Then register so the signal handler can forward signals.
        setpgid(pids[i], pids[i]);
        kiln::register_child_pid(pids[i]);
    }

    // Parent
    for (auto& p : pipes) {
        close(p[0]);
        close(p[1]);
    }
    if (options.output_variable) close(stdout_pipe[1]);
    if (options.error_variable) close(stderr_pipe[1]);
    close(setup_pipe[1]);

    PipelineResult result;

    // Read stdout/stderr if needed
    auto read_all = [](int fd) {
        std::string out;
        char buffer[4096];
        ssize_t bytes;
        while ((bytes = read(fd, buffer, sizeof(buffer))) > 0) { out.append(buffer, bytes); }
        return out;
    };

    if (options.output_variable) {
        result.captured_stdout = read_all(stdout_pipe[0]);
        close(stdout_pipe[0]);
    }
    if (options.error_variable) {
        result.captured_stderr = read_all(stderr_pipe[0]);
        close(stderr_pipe[0]);
    }

    result.setup_error = read_all(setup_pipe[0]);
    close(setup_pipe[0]);

    // Wait for all processes with optional timeout
    auto start_time = std::chrono::steady_clock::now();
    std::vector<bool> finished(num_commands, false);
    size_t finished_count = 0;
    result.exit_codes.resize(num_commands, -1);

    while (finished_count < num_commands) {
        bool any_progress = false;
        for (size_t i = 0; i < num_commands; ++i) {
            if (!finished[i]) {
                int status;
                pid_t res = waitpid(pids[i], &status, WNOHANG);
                if (res > 0) {
                    if (WIFEXITED(status))
                        result.exit_codes[i] = WEXITSTATUS(status);
                    else
                        result.exit_codes[i] = -1;
                    finished[i] = true;
                    finished_count++;
                    any_progress = true;
                    kiln::unregister_child_pid(pids[i]);
                } else if (res == -1 && errno != EINTR) {
                    finished[i] = true;
                    finished_count++;
                    kiln::unregister_child_pid(pids[i]);
                }
            }
        }

        if (finished_count == num_commands) break;

        if (options.timeout > 0.0) {
            auto now = std::chrono::steady_clock::now();
            std::chrono::duration<double> elapsed = now - start_time;
            if (elapsed.count() >= options.timeout) {
                // Timeout! Kill remaining
                for (size_t i = 0; i < num_commands; ++i) {
                    if (!finished[i]) {
                        kill(pids[i], SIGKILL);
                        int status;
                        waitpid(pids[i], &status, 0);
                        kiln::unregister_child_pid(pids[i]);
                        result.exit_codes[i] = -1; // Or some other indicator
                    }
                }
                break;
            }
        }

        if (!any_progress) {
            usleep(10000); // 10ms sleep to avoid busy wait
        }
    }

    return result;
}

std::string kiln::to_upper(std::string_view str) {
    std::string result;
    result.reserve(str.size());
    for (char c : str) { result += (c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c; }
    return result;
}

std::string kiln::to_lower(std::string_view str) {
    std::string result;
    result.reserve(str.size());
    for (char c : str) { result += (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }
    return result;
}

std::string_view kiln::strip(std::string_view str) {
    constexpr const char* whitespace = " \t\n\r\f\v";
    size_t start = str.find_first_not_of(whitespace);
    if (start == std::string_view::npos) return {};
    size_t end = str.find_last_not_of(whitespace);
    return str.substr(start, end - start + 1);
}

std::string_view kiln::lstrip(std::string_view str) {
    constexpr const char* whitespace = " \t\n\r\f\v";
    size_t start = str.find_first_not_of(whitespace);
    if (start == std::string_view::npos) return {};
    return str.substr(start);
}

std::string_view kiln::rstrip(std::string_view str) {
    constexpr const char* whitespace = " \t\n\r\f\v";
    size_t end = str.find_last_not_of(whitespace);
    if (end == std::string_view::npos) return {};
    return str.substr(0, end + 1);
}

std::string kiln::replace_all(std::string str, std::string_view from, std::string_view to) {
    if (from.empty()) return str;
    size_t pos = 0;
    while ((pos = str.find(from, pos)) != std::string::npos) {
        str.replace(pos, from.size(), to);
        pos += to.size();
    }
    return str;
}

std::vector<std::string> kiln::shell_split(std::string_view input) {
    std::vector<std::string> result;
    std::string current;
    char quote_char = 0;

    for (size_t i = 0; i < input.size(); ++i) {
        char c = input[i];

        if (quote_char) {
            if (c == quote_char) {
                quote_char = 0;
            } else {
                current += c;
            }
        } else {
            if (c == '"' || c == '\'') {
                quote_char = c;
            } else if (c == ' ' || c == '\t') {
                if (!current.empty()) {
                    result.push_back(std::move(current));
                    current.clear();
                }
            } else {
                current += c;
            }
        }
    }

    if (!current.empty()) { result.push_back(std::move(current)); }

    return result;
}

const std::string& kiln::cmake_extra_modules_root() {
    static const std::string root = [] -> std::string {
        if (std::filesystem::is_directory("/usr/share/cmake/Modules")) return {};

        std::vector<std::pair<std::pair<int, int>, std::string>> candidates;

        std::error_code ec;
        for (auto& entry : std::filesystem::directory_iterator("/usr/share", ec)) {
            auto name = entry.path().filename().string();
            if (!name.starts_with("cmake-")) continue;

            std::string_view ver(name.c_str() + 6, name.size() - 6);
            int major = 0, minor = 0;
            auto [p1, e1] = std::from_chars(ver.data(), ver.data() + ver.size(), major);
            if (e1 != std::errc{} || p1 >= ver.data() + ver.size() || *p1 != '.') continue;
            auto [p2, e2] = std::from_chars(p1 + 1, ver.data() + ver.size(), minor);
            if (e2 != std::errc{}) continue;

            candidates.emplace_back(std::pair{major, minor}, entry.path().string());
        }

        std::ranges::sort(candidates, std::greater{}, [](auto& c) { return c.first; });

        for (auto& [ver, path] : candidates) {
            if (std::filesystem::is_directory(path + "/Modules")) return path;
        }

        return {};
    }();
    return root;
}

const std::string& kiln::gnu_arch_triplet() {
    static const std::string triplet = [] {
#ifdef __linux__
        struct utsname buf;
        if (uname(&buf) != 0) {
            fprintf(stderr, "fatal: uname() failed\n");
            abort();
        }
        std::string machine = buf.machine;
        if (machine == "x86_64") return std::string("x86_64-linux-gnu");
        if (machine == "aarch64") return std::string("aarch64-linux-gnu");
        if (machine == "armv7l") return std::string("arm-linux-gnueabihf");
        if (machine == "i686" || machine == "i386") return std::string("i386-linux-gnu");
        if (machine == "riscv64") return std::string("riscv64-linux-gnu");
        if (machine == "s390x") return std::string("s390x-linux-gnu");
        if (machine == "ppc64le") return std::string("powerpc64le-linux-gnu");
        fprintf(stderr, "fatal: unrecognized architecture '%s' - please add support for it\n", machine.c_str());
        abort();
#else
        return std::string{};
#endif
    }();
    return triplet;
}
