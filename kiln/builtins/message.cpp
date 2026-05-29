#include "registry.hpp"
#include "../interperter.hpp"
#include "../command_parser.hpp"
#include "../utils.hpp"
#include "../genex_evaluator.hpp"
#include "../genex_parser.hpp"
#include <sstream>
#include <algorithm>
#include <stack>
#include <iostream>

namespace kiln {

namespace {

// Message log levels (higher number = more verbose)
enum class LogLevel {
    ERROR = 0,   // FATAL_ERROR, SEND_ERROR
    WARNING = 1, // WARNING, AUTHOR_WARNING
    NOTICE = 2,  // NOTICE, (none)
    STATUS = 3,  // STATUS
    VERBOSE = 4, // VERBOSE
    DEBUG = 5,   // DEBUG
    TRACE = 6    // TRACE
};

LogLevel parse_log_level(const std::string& level_str) {
    if (ci_equals(level_str, "ERROR")) return LogLevel::ERROR;
    if (ci_equals(level_str, "WARNING")) return LogLevel::WARNING;
    if (ci_equals(level_str, "NOTICE")) return LogLevel::NOTICE;
    if (ci_equals(level_str, "STATUS")) return LogLevel::STATUS;
    if (ci_equals(level_str, "VERBOSE")) return LogLevel::VERBOSE;
    if (ci_equals(level_str, "DEBUG")) return LogLevel::DEBUG;
    if (ci_equals(level_str, "TRACE")) return LogLevel::TRACE;
    return LogLevel::STATUS; // Default
}

LogLevel get_message_log_level(Interpreter& interp) {
    std::string level = interp.get_variable("CMAKE_MESSAGE_LOG_LEVEL");
    if (level.empty()) {
        return LogLevel::STATUS; // Default level
    }
    return parse_log_level(level);
}

bool should_show_message(LogLevel msg_level, LogLevel threshold) {
    return static_cast<int>(msg_level) <= static_cast<int>(threshold);
}

} // anonymous namespace

void register_message_builtins(Interpreter& interp) {
    interp.add_builtin("message", [](Interpreter& interp, const std::vector<std::string>& args) {
        if (args.empty()) {
            interp.set_fatal_error("message() requires at least one argument");
            return;
        }

        const auto& first_arg = args[0];

        std::string mode;
        LogLevel log_level = LogLevel::NOTICE;
        bool mode_found = true;
        bool is_check_command = false;

        // Parse mode
        if (ci_equals(first_arg, "STATUS")) {
            mode = "STATUS";
            log_level = LogLevel::STATUS;
        } else if (ci_equals(first_arg, "VERBOSE")) {
            mode = "VERBOSE";
            log_level = LogLevel::VERBOSE;
        } else if (ci_equals(first_arg, "DEBUG")) {
            mode = "DEBUG";
            log_level = LogLevel::DEBUG;
        } else if (ci_equals(first_arg, "TRACE")) {
            mode = "TRACE";
            log_level = LogLevel::TRACE;
        } else if (ci_equals(first_arg, "WARNING")) {
            mode = "WARNING";
            log_level = LogLevel::WARNING;
        } else if (ci_equals(first_arg, "AUTHOR_WARNING")) {
            mode = "AUTHOR_WARNING";
            log_level = LogLevel::WARNING;
        } else if (ci_equals(first_arg, "DEPRECATION")) {
            // Check CMAKE_ERROR_DEPRECATED and CMAKE_WARN_DEPRECATED
            std::string error_deprecated = interp.get_variable("CMAKE_ERROR_DEPRECATED");
            std::string warn_deprecated = interp.get_variable("CMAKE_WARN_DEPRECATED");

            if (error_deprecated == "TRUE" || error_deprecated == "ON" || error_deprecated == "1") {
                mode = "DEPRECATION_ERROR";
                log_level = LogLevel::ERROR;
            } else if (warn_deprecated == "TRUE" || warn_deprecated == "ON" || warn_deprecated == "1") {
                mode = "DEPRECATION";
                log_level = LogLevel::WARNING;
            } else {
                // Silent - don't print anything
                return;
            }
        } else if (ci_equals(first_arg, "NOTICE")) {
            mode = "NOTICE";
            log_level = LogLevel::NOTICE;
        } else if (ci_equals(first_arg, "SEND_ERROR")) {
            mode = "SEND_ERROR";
            log_level = LogLevel::ERROR;
        } else if (ci_equals(first_arg, "FATAL_ERROR")) {
            mode = "FATAL_ERROR";
            log_level = LogLevel::ERROR;
        } else if (ci_equals(first_arg, "CHECK_START")) {
            mode = "CHECK_START";
            log_level = LogLevel::STATUS;
            is_check_command = true;
        } else if (ci_equals(first_arg, "CHECK_PASS")) {
            mode = "CHECK_PASS";
            log_level = LogLevel::STATUS;
            is_check_command = true;
        } else if (ci_equals(first_arg, "CHECK_FAIL")) {
            mode = "CHECK_FAIL";
            log_level = LogLevel::STATUS;
            is_check_command = true;
        } else if (ci_equals(first_arg, "CONFIGURE_LOG")) {
            // CONFIGURE_LOG - for now, just ignore (CMake 3.26+ feature for detailed logging)
            // This would write to a configure log file
            return;
        } else {
            mode_found = false;
            mode = "NOTICE";
            log_level = LogLevel::NOTICE;
        }

        // Extract message content
        std::vector<std::string> message_args;
        if (mode_found) {
            message_args.assign(args.begin() + 1, args.end());
        } else {
            message_args = args;
        }

        std::ostringstream oss;
        for (size_t i = 0; i < message_args.size(); i++) { oss << message_args[i]; }
        std::string content = oss.str();

        // Handle FATAL_ERROR specially
        if (mode == "FATAL_ERROR") {
            interp.set_fatal_error(content);
            return;
        }

        // Handle SEND_ERROR - accumulate error but continue
        if (mode == "SEND_ERROR") {
            interp.print_message("SEND_ERROR", content, true);
            interp.accumulate_error(content); // Mark that generation should be skipped
            return;
        }

        // Check log level filtering (except for errors and warnings)
        LogLevel threshold = get_message_log_level(interp);
        if (!should_show_message(log_level, threshold)) {
            return; // Filtered out
        }

        // Handle CHECK commands
        if (is_check_command) {
            if (mode == "CHECK_START") {
                interp.check_start(content);
            } else if (mode == "CHECK_PASS") {
                interp.check_pass(content);
            } else if (mode == "CHECK_FAIL") {
                interp.check_fail(content);
            }
            return;
        }

        // Normal message output
        // In CMake, NOTICE (and no-mode) goes to stderr, same as WARNING etc.
        // Only STATUS/VERBOSE/DEBUG/TRACE go to stdout.
        bool is_error =
            (mode == "NOTICE" || mode == "WARNING" || mode == "AUTHOR_WARNING" || mode == "DEPRECATION" || mode == "DEPRECATION_ERROR");

        interp.print_message(mode, content, is_error);
    });

    // Debug builtin to evaluate genex expressions
    // Usage: kiln_eval_genex(<expression> [LANGUAGE <lang>] [DEFERRED])
    interp.add_builtin("kiln_eval_genex", [](Interpreter& interp, const std::vector<std::string>& args) {
        if (args.empty()) {
            interp.set_fatal_error("kiln_eval_genex() requires at least one argument");
            return;
        }

        std::string expr = args[0];
        std::optional<Language> compile_lang;
        bool allow_deferred = false;

        // Parse options
        for (size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "LANGUAGE" && i + 1 < args.size()) {
                std::string lang = args[++i];
                if (lang == "C")
                    compile_lang = Language::C;
                else if (lang == "CXX")
                    compile_lang = Language::CXX;
                else if (lang == "CUDA")
                    compile_lang = Language::CUDA;
            } else if (args[i] == "DEFERRED") {
                allow_deferred = true;
            }
        }

        // Set up context
        auto ctx = GenexEvaluationContext::from_interpreter(interp, interp.get_targets());
        ctx.allow_deferred_compile_language = allow_deferred;
        if (compile_lang) { ctx.compile_language = *compile_lang; }

        GenexEvaluator evaluator(ctx);

        auto result = evaluator.evaluate(expr);
        if (result) {
            std::cerr << "[kiln_eval_genex] Result: '" << *result << "'\n";
        } else {
            std::cerr << "[kiln_eval_genex] Error: " << result.error() << "\n";
        }
    });
}

} // namespace kiln
