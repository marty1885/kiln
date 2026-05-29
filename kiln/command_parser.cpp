#include "command_parser.hpp"

namespace kiln {

CommandParser::CommandParser(std::string_view cmd_name) : cmd_name_(cmd_name) {}

CommandParser::CommandParser(std::string_view cmd_name, std::string_view subcommand) : cmd_name_(cmd_name), subcommand_(subcommand) {}

void CommandParser::positional(std::string& var, std::string_view label, bool required) {
    single_positionals_.push_back({&var, label, required, false});
}

void CommandParser::positionals(std::vector<std::string>& var, std::string_view label, bool required) {
    positional_list_ = PositionalList{&var, label, required};
}

void CommandParser::flag(std::string keyword, bool& var) {
    keywords_[keyword] = {KeywordType::FLAG, &var, keyword};
    var = false;
}

void CommandParser::value(std::string keyword, std::string& var) {
    keywords_[keyword] = {KeywordType::VALUE, &var, keyword};
}

void CommandParser::list(std::string keyword, std::vector<std::string>& var) {
    keywords_[keyword] = {KeywordType::LIST, &var, keyword};
}

void CommandParser::multi_list(std::string keyword, std::vector<std::vector<std::string>>& var) {
    keywords_[keyword] = {KeywordType::MULTI_LIST, &var, keyword};
}

void CommandParser::unparsed(std::vector<std::string>& var) {
    unparsed_ = &var;
}

std::expected<std::vector<std::string>, std::string> CommandParser::parse(std::span<const std::string> args) {
    // Build error prefix: "cmd(subcmd)" or "cmd"
    std::string error_prefix(cmd_name_);
    if (!subcommand_.empty()) {
        error_prefix += '(';
        error_prefix.append(subcommand_);
        error_prefix += ')';
    } else {
        error_prefix += "()";
    }

    std::vector<std::string> warnings;
    size_t single_pos_idx = 0;
    KeywordInfo* active_keyword = nullptr;

    for (const auto& arg : args) {
        // Check if this is a keyword
        auto it = keywords_.find(arg);
        if (it != keywords_.end()) {
            // Warn if we hit a keyword while required positionals are still unfilled
            if (!active_keyword) {
                std::string missing;
                for (size_t i = single_pos_idx; i < single_positionals_.size(); ++i) {
                    if (single_positionals_[i].required) {
                        if (!missing.empty()) missing += ", ";
                        missing += '<';
                        missing.append(single_positionals_[i].label);
                        missing += '>';
                    }
                }
                if (missing.empty() && positional_list_ && positional_list_->required && positional_list_->var->empty()) {
                    missing = "<";
                    missing.append(positional_list_->label);
                    missing += ">...";
                }
                if (!missing.empty()) {
                    warnings.push_back("While parsing " + error_prefix + ", encountered known keyword '" + arg + "' where positional "
                                       + missing + " was expected. This likely means a required argument was omitted.");
                }
            }

            // Reset active_keyword before setting new one - this ensures empty lists are allowed
            active_keyword = nullptr;

            if (it->second.type == KeywordType::FLAG) {
                *static_cast<bool*>(it->second.target) = true;
                // Flags don't become active (they take no arguments)
            } else {
                active_keyword = &it->second;
                if (active_keyword->type == KeywordType::MULTI_LIST) {
                    auto* vec = static_cast<std::vector<std::vector<std::string>>*>(active_keyword->target);
                    vec->emplace_back();
                }
            }
            continue;
        }

        // Not a keyword - determine where this argument goes
        if (!active_keyword) {
            // Still in positional territory
            if (single_pos_idx < single_positionals_.size()) {
                // Fill single positionals first
                *single_positionals_[single_pos_idx].var = arg;
                single_positionals_[single_pos_idx].filled = true;
                single_pos_idx++;
            } else if (positional_list_) {
                // Then fill the positional list
                positional_list_->var->push_back(arg);
            } else if (unparsed_) {
                unparsed_->push_back(arg);
            } else {
                return std::unexpected(error_prefix + ": unexpected argument '" + arg + "'");
            }
        } else {
            // In keyword territory
            if (active_keyword->type == KeywordType::VALUE) {
                auto* val = static_cast<std::string*>(active_keyword->target);
                if (!val->empty()) {
                    return std::unexpected(error_prefix + ": keyword " + active_keyword->keyword + " given multiple values");
                }
                *val = arg;
            } else if (active_keyword->type == KeywordType::LIST) {
                auto* vec = static_cast<std::vector<std::string>*>(active_keyword->target);
                // Skip empty strings — CMake expands "${EMPTY_VAR}" to "" which
                // should not produce a list entry (matches CMake behavior)
                if (!arg.empty()) vec->push_back(arg);
            } else if (active_keyword->type == KeywordType::MULTI_LIST) {
                auto* vec = static_cast<std::vector<std::vector<std::string>>*>(active_keyword->target);
                if (vec->empty()) vec->emplace_back();
                if (!arg.empty()) vec->back().push_back(arg);
            }
        }
    }

    // Helper to prepend warnings to an error message for context
    auto make_error = [&](std::string msg) -> std::unexpected<std::string> {
        for (const auto& w : warnings) { msg += "\nnote: " + w; }
        return std::unexpected(std::move(msg));
    };

    // Validate required single positionals are filled
    for (const auto& pos : single_positionals_) {
        if (pos.required && !pos.filled) {
            return make_error(error_prefix + ": missing required argument <" + std::string(pos.label) + ">");
        }
    }

    // Validate positional list if required
    if (positional_list_ && positional_list_->required && positional_list_->var->empty()) {
        return make_error(error_prefix + ": requires at least one <" + std::string(positional_list_->label) + ">");
    }

    return warnings;
}

std::string CommandParser::get_syntax() const {
    std::string syntax(cmd_name_);
    syntax += '(';
    bool first = true;

    // Subcommand comes first if present
    if (!subcommand_.empty()) {
        syntax.append(subcommand_);
        first = false;
    }

    // Single positionals
    for (const auto& pos : single_positionals_) {
        if (!first) syntax += " ";
        if (!pos.required) syntax += "[";
        syntax += '<';
        syntax.append(pos.label);
        syntax += '>';
        if (!pos.required) syntax += "]";
        first = false;
    }

    // Positional list
    if (positional_list_) {
        if (!first) syntax += " ";
        if (!positional_list_->required) syntax += "[";
        syntax += '<';
        syntax.append(positional_list_->label);
        syntax += ">...";
        if (!positional_list_->required) syntax += "]";
        first = false;
    }

    // Keywords
    for (const auto& [kw, info] : keywords_) {
        if (!first) syntax += " ";
        syntax += "[" + kw;
        if (info.type == KeywordType::VALUE) {
            syntax += " <value>";
        } else if (info.type == KeywordType::LIST || info.type == KeywordType::MULTI_LIST) {
            syntax += " <args>...";
        }
        syntax += "]";
        first = false;
    }

    syntax += ")";
    return syntax;
}

} // namespace kiln
