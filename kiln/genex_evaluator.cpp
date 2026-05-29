#include "genex_evaluator.hpp"
#include "interperter.hpp"
#include "target.hpp"
#include "CMakeArray.hpp"
#include "language.hpp"
#include "path.hpp"
#include "utils.hpp"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <regex>
#include <sstream>

namespace kiln {

GenexEvaluationContext GenexEvaluationContext::from_interpreter(const Interpreter& interp, const TargetMap& all_targets) {
    GenexEvaluationContext ctx;
    ctx.build_type = interp.get_variable("CMAKE_BUILD_TYPE");
    ctx.system_name = interp.get_variable("CMAKE_SYSTEM_NAME");
    ctx.cxx_compiler_id = interp.get_variable("CMAKE_CXX_COMPILER_ID");
    ctx.c_compiler_id = interp.get_variable("CMAKE_C_COMPILER_ID");
    ctx.cxx_compiler_version = interp.get_variable("CMAKE_CXX_COMPILER_VERSION");
    ctx.c_compiler_version = interp.get_variable("CMAKE_C_COMPILER_VERSION");
    ctx.all_targets = &all_targets;
    ctx.target_aliases = &interp.get_target_aliases();
    ctx.install_prefix = interp.get_variable("CMAKE_INSTALL_PREFIX");
    ctx.static_library_prefix = interp.get_variable("CMAKE_STATIC_LIBRARY_PREFIX");
    ctx.static_library_suffix = interp.get_variable("CMAKE_STATIC_LIBRARY_SUFFIX");
    ctx.shared_library_prefix = interp.get_variable("CMAKE_SHARED_LIBRARY_PREFIX");
    ctx.shared_library_suffix = interp.get_variable("CMAKE_SHARED_LIBRARY_SUFFIX");
    ctx.executable_suffix = interp.get_variable("CMAKE_EXECUTABLE_SUFFIX");
    ctx.phase = Phase::BUILD;
    ctx.source_properties = &interp.get_source_properties();
    ctx.interp = &interp;
    return ctx;
}

std::string GenexEvaluator::to_lower(const std::string& str) const {
    std::string result = str;
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return std::tolower(c); });
    return result;
}

Target* GenexEvaluator::find_target(const std::string& name) const {
    auto it = ctx_.all_targets->find(name);
    if (it != ctx_.all_targets->end()) return it->second.get();
    if (ctx_.target_aliases) {
        auto alias_it = ctx_.target_aliases->find(name);
        if (alias_it != ctx_.target_aliases->end()) {
            it = ctx_.all_targets->find(alias_it->second);
            if (it != ctx_.all_targets->end()) return it->second.get();
        }
    }
    return nullptr;
}

bool GenexEvaluator::is_truthy(const std::string& value) const {
    // Inverse of Interpreter::is_falsy — avoids to_lower allocation
    return !Interpreter::is_falsy(value);
}

std::expected<std::string, std::string> GenexEvaluator::evaluate_nodes(const std::vector<std::shared_ptr<GenexNode>>& nodes) {
    std::string result;
    for (const auto& node : nodes) {
        auto eval_result = evaluate_node(*node);
        if (!eval_result) { return eval_result; }
        result += *eval_result;
    }
    return result;
}

std::expected<std::string, std::string> GenexEvaluator::evaluate_node(const GenexNode& node) {
    switch (node.type) {
    case GenexNodeType::LITERAL:
        return node.raw_content;

    case GenexNodeType::BUILD_INTERFACE:
        if (ctx_.phase == GenexEvaluationContext::Phase::BUILD) { return evaluate_nodes(node.children); }
        return std::string(""); // Empty for INSTALL phase

    case GenexNodeType::INSTALL_INTERFACE:
        if (ctx_.phase == GenexEvaluationContext::Phase::INSTALL) { return evaluate_nodes(node.children); }
        return std::string(""); // Empty for BUILD phase

    case GenexNodeType::LINK_ONLY:
        // LINK_ONLY evaluates to its content - the semantic meaning
        // (skip INTERFACE propagation) is handled by evaluate_link_library()
        return evaluate_nodes(node.children);

    case GenexNodeType::INSTALL_PREFIX:
        // $<INSTALL_PREFIX> - resolves to CMAKE_INSTALL_PREFIX
        return ctx_.install_prefix.empty() ? "/usr/local" : ctx_.install_prefix;

    case GenexNodeType::CONFIG: {
        // $<CONFIG> (no argument) returns CMAKE_BUILD_TYPE
        // $<CONFIG:cfg> returns 1 if CMAKE_BUILD_TYPE matches (case-insensitive), 0 otherwise
        if (node.raw_content.empty() && node.children.empty()) { return ctx_.build_type; }
        std::string config = to_lower(node.raw_content);
        std::string build_type = to_lower(ctx_.build_type);
        return (config == build_type) ? "1" : "0";
    }

    case GenexNodeType::BOOL: {
        // $<BOOL:string> returns 1 if truthy, 0 otherwise
        // If there are children (nested genex), evaluate them first
        if (!node.children.empty()) {
            auto value_result = evaluate_nodes(node.children);
            if (!value_result) { return value_result; }
            return is_truthy(*value_result) ? "1" : "0";
        }
        // No children - raw_content may still contain unevaluated genex
        if (GenexParser::contains_genex(node.raw_content)) {
            auto eval_result = evaluate(node.raw_content);
            if (!eval_result) { return eval_result; }
            return is_truthy(*eval_result) ? "1" : "0";
        }
        // Plain string
        return is_truthy(node.raw_content) ? "1" : "0";
    }

    case GenexNodeType::IF: {
        // $<IF:cond,true_val,false_val>
        // Children should be organized as: condition nodes, then true_val nodes, then false_val nodes
        // But we need to split by commas in the original content
        auto args = GenexParser().split_genex_args(node.raw_content);
        if (args.size() != 3) { return std::unexpected("$<IF:...> requires exactly 3 arguments"); }

        // Evaluate condition
        GenexParser parser;
        auto cond_result = parser.parse(args[0]);
        if (!cond_result) { return std::unexpected(cond_result.error()); }
        auto cond_val = evaluate_nodes(cond_result->nodes);
        if (!cond_val) { return cond_val; }

        // Evaluate true or false branch based on condition
        if (is_truthy(*cond_val)) {
            auto true_result = parser.parse(args[1]);
            if (!true_result) { return std::unexpected(true_result.error()); }
            return evaluate_nodes(true_result->nodes);
        } else {
            auto false_result = parser.parse(args[2]);
            if (!false_result) { return std::unexpected(false_result.error()); }
            return evaluate_nodes(false_result->nodes);
        }
    }

    case GenexNodeType::AND: {
        // $<AND:expr1,expr2,...> returns 1 if all expressions are truthy, 0 otherwise
        auto args = GenexParser().split_genex_args(node.raw_content);
        GenexParser parser;

        for (const auto& arg : args) {
            auto arg_result = parser.parse(arg);
            if (!arg_result) { return std::unexpected(arg_result.error()); }
            auto arg_val = evaluate_nodes(arg_result->nodes);
            if (!arg_val) { return arg_val; }
            if (!is_truthy(*arg_val)) { return "0"; }
        }
        return "1";
    }

    case GenexNodeType::OR: {
        // $<OR:expr1,expr2,...> returns 1 if any expression is truthy, 0 otherwise
        auto args = GenexParser().split_genex_args(node.raw_content);
        GenexParser parser;

        for (const auto& arg : args) {
            auto arg_result = parser.parse(arg);
            if (!arg_result) { return std::unexpected(arg_result.error()); }
            auto arg_val = evaluate_nodes(arg_result->nodes);
            if (!arg_val) { return arg_val; }
            if (is_truthy(*arg_val)) { return "1"; }
        }
        return "0";
    }

    case GenexNodeType::NOT: {
        // $<NOT:expr> returns 1 if expr is falsy, 0 if truthy
        if (!node.children.empty()) {
            auto val_result = evaluate_nodes(node.children);
            if (!val_result) { return val_result; }
            return is_truthy(*val_result) ? "0" : "1";
        }
        // No children, use raw_content directly
        return is_truthy(node.raw_content) ? "0" : "1";
    }

    case GenexNodeType::STREQUAL: {
        // $<STREQUAL:a,b> returns 1 if strings are equal, 0 otherwise
        auto args = GenexParser().split_genex_args(node.raw_content);
        if (args.size() != 2) { return std::unexpected("$<STREQUAL:...> requires exactly 2 arguments"); }

        GenexParser parser;
        auto arg1_result = parser.parse(args[0]);
        if (!arg1_result) { return std::unexpected(arg1_result.error()); }
        auto arg1_val = evaluate_nodes(arg1_result->nodes);
        if (!arg1_val) { return arg1_val; }

        auto arg2_result = parser.parse(args[1]);
        if (!arg2_result) { return std::unexpected(arg2_result.error()); }
        auto arg2_val = evaluate_nodes(arg2_result->nodes);
        if (!arg2_val) { return arg2_val; }

        return (*arg1_val == *arg2_val) ? "1" : "0";
    }

    case GenexNodeType::EQUAL: {
        // $<EQUAL:a,b> returns 1 if values are numerically equal, 0 otherwise.
        // Real CMake parses these as integers (decimal/0x hex/0 octal).
        auto args = GenexParser().split_genex_args(node.raw_content);
        if (args.size() != 2) { return std::unexpected("$<EQUAL:...> requires exactly 2 arguments"); }
        GenexParser parser;
        auto arg1_result = parser.parse(args[0]);
        if (!arg1_result) return std::unexpected(arg1_result.error());
        auto arg1_val = evaluate_nodes(arg1_result->nodes);
        if (!arg1_val) return arg1_val;
        auto arg2_result = parser.parse(args[1]);
        if (!arg2_result) return std::unexpected(arg2_result.error());
        auto arg2_val = evaluate_nodes(arg2_result->nodes);
        if (!arg2_val) return arg2_val;
        auto to_int = [](std::string s) -> std::expected<long long, std::string> {
            size_t b = 0, e = s.size();
            while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
            while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
            std::string t = s.substr(b, e - b);
            if (t.empty()) return std::unexpected("$<EQUAL:...> empty argument");
            try {
                size_t pos = 0;
                long long v = std::stoll(t, &pos, 0);
                if (pos != t.size()) return std::unexpected("$<EQUAL:...> non-numeric argument: " + t);
                return v;
            } catch (const std::exception&) { return std::unexpected("$<EQUAL:...> non-numeric argument: " + t); }
        };
        auto a = to_int(*arg1_val);
        if (!a) return std::unexpected(a.error());
        auto b = to_int(*arg2_val);
        if (!b) return std::unexpected(b.error());
        return (*a == *b) ? "1" : "0";
    }

    case GenexNodeType::VERSION_LESS:
    case GenexNodeType::VERSION_GREATER:
    case GenexNodeType::VERSION_EQUAL:
    case GenexNodeType::VERSION_LESS_EQUAL:
    case GenexNodeType::VERSION_GREATER_EQUAL: {
        // Version comparison operators: $<VERSION_LESS:v1,v2> etc.
        auto args = GenexParser().split_genex_args(node.raw_content);
        if (args.size() != 2) { return std::unexpected("Version comparison requires exactly 2 arguments"); }

        GenexParser parser;
        auto arg1_result = parser.parse(args[0]);
        if (!arg1_result) { return std::unexpected(arg1_result.error()); }
        auto arg1_val = evaluate_nodes(arg1_result->nodes);
        if (!arg1_val) { return arg1_val; }

        auto arg2_result = parser.parse(args[1]);
        if (!arg2_result) { return std::unexpected(arg2_result.error()); }
        auto arg2_val = evaluate_nodes(arg2_result->nodes);
        if (!arg2_val) { return arg2_val; }

        int cmp = compare_versions(*arg1_val, *arg2_val);

        switch (node.type) {
        case GenexNodeType::VERSION_LESS:
            return (cmp < 0) ? "1" : "0";
        case GenexNodeType::VERSION_GREATER:
            return (cmp > 0) ? "1" : "0";
        case GenexNodeType::VERSION_EQUAL:
            return (cmp == 0) ? "1" : "0";
        case GenexNodeType::VERSION_LESS_EQUAL:
            return (cmp <= 0) ? "1" : "0";
        case GenexNodeType::VERSION_GREATER_EQUAL:
            return (cmp >= 0) ? "1" : "0";
        default:
            return std::unexpected("Unknown version comparison operator");
        }
    }

    case GenexNodeType::TARGET_EXISTS: {
        // $<TARGET_EXISTS:target> returns 1 if target exists, 0 otherwise
        if (!ctx_.all_targets) { return std::unexpected("TARGET_EXISTS requires all_targets context"); }
        auto name = evaluate(node.raw_content);
        if (!name) return name;
        return find_target(*name) ? "1" : "0";
    }

    case GenexNodeType::TARGET_NAME: {
        // $<TARGET_NAME:target> returns the target name (resolves aliases)
        // For now, just pass through the name since kiln doesn't have alias resolution
        return evaluate(node.raw_content);
    }

    case GenexNodeType::TARGET_NAME_IF_EXISTS: {
        // $<TARGET_NAME_IF_EXISTS:target> returns target name if exists, empty string otherwise
        if (!ctx_.all_targets) { return std::unexpected("TARGET_NAME_IF_EXISTS requires all_targets context"); }
        auto name = evaluate(node.raw_content);
        if (!name) return name;
        return find_target(*name) ? *name : "";
    }

    case GenexNodeType::TARGET_FILE: {
        // $<TARGET_FILE:target> returns full path to target output
        if (!ctx_.all_targets) { return std::unexpected("TARGET_FILE requires all_targets context"); }
        auto name = evaluate(node.raw_content);
        if (!name) return name;
        auto* target = find_target(*name);
        if (!target) { return std::unexpected("TARGET_FILE: target '" + *name + "' not found"); }
        return target->get_output_path();
    }

    case GenexNodeType::TARGET_FILE_NAME: {
        // $<TARGET_FILE_NAME:target> returns filename of target output
        if (!ctx_.all_targets) { return std::unexpected("TARGET_FILE_NAME requires all_targets context"); }
        auto name = evaluate(node.raw_content);
        if (!name) return name;
        auto* target = find_target(*name);
        if (!target) { return std::unexpected("TARGET_FILE_NAME: target '" + *name + "' not found"); }
        return std::string(Path(target->get_output_path()).filename());
    }

    case GenexNodeType::TARGET_FILE_DIR: {
        // $<TARGET_FILE_DIR:target> returns directory of target output
        if (!ctx_.all_targets) { return std::unexpected("TARGET_FILE_DIR requires all_targets context"); }
        auto name = evaluate(node.raw_content);
        if (!name) return name;
        auto* target = find_target(*name);
        if (!target) { return std::unexpected("TARGET_FILE_DIR: target '" + *name + "' not found"); }
        return std::string(Path(target->get_output_path()).parent_path());
    }

    case GenexNodeType::TARGET_LINKER_FILE: {
        // $<TARGET_LINKER_FILE:target> - full path to linker file (same as output on Linux)
        if (!ctx_.all_targets) { return std::unexpected("TARGET_LINKER_FILE requires all_targets context"); }
        auto* target = find_target(node.raw_content);
        if (!target) { return std::unexpected("TARGET_LINKER_FILE: target '" + node.raw_content + "' not found"); }
        return target->get_output_path();
    }

    case GenexNodeType::TARGET_LINKER_FILE_NAME: {
        // $<TARGET_LINKER_FILE_NAME:target> - filename of linker file
        if (!ctx_.all_targets) { return std::unexpected("TARGET_LINKER_FILE_NAME requires all_targets context"); }
        auto* target = find_target(node.raw_content);
        if (!target) { return std::unexpected("TARGET_LINKER_FILE_NAME: target '" + node.raw_content + "' not found"); }
        return std::string(Path(target->get_output_path()).filename());
    }

    case GenexNodeType::TARGET_LINKER_FILE_DIR: {
        // $<TARGET_LINKER_FILE_DIR:target> - directory of linker file
        if (!ctx_.all_targets) { return std::unexpected("TARGET_LINKER_FILE_DIR requires all_targets context"); }
        auto* target = find_target(node.raw_content);
        if (!target) { return std::unexpected("TARGET_LINKER_FILE_DIR: target '" + node.raw_content + "' not found"); }
        return std::string(Path(target->get_output_path()).parent_path());
    }

    case GenexNodeType::TARGET_OBJECTS: {
        // $<TARGET_OBJECTS:target> returns object files from OBJECT_LIBRARY
        if (!ctx_.all_targets) { return std::unexpected("TARGET_OBJECTS requires all_targets context"); }
        auto* target = find_target(node.raw_content);
        if (!target) { return std::unexpected("TARGET_OBJECTS: target '" + node.raw_content + "' not found"); }
        // Since CMake 3.21, TARGET_OBJECTS works on any library with compiled sources
        auto ttype = target->get_type();
        if (ttype == TargetType::INTERFACE_LIBRARY || ttype == TargetType::CUSTOM) {
            return std::unexpected("TARGET_OBJECTS: target '" + node.raw_content + "' has no compiled sources");
        }

        // Collect object file paths for all sources in the target
        std::string result;
        auto sources = target->get_property_list("SOURCES", TargetPropertyScope::BUILD);
        std::string binary_dir = target->get_binary_dir();
        std::string target_name = target->get_name();
        std::string source_dir = target->get_source_dir();

        for (const auto& src : sources) {
            // Skip genex in source paths (they would need recursive evaluation)
            if (GenexParser::contains_genex(src)) { continue; }

            // Skip header files - they don't produce object files
            auto lang_info = LanguageClassifier::from_path(src);
            if (lang_info.is_header || !lang_info.is_compileable) { continue; }

            // Skip sources marked HEADER_FILE_ONLY (e.g. unity build originals)
            if (ctx_.source_properties) {
                std::string abs = Path::make_absolute_and_normal(source_dir, src);
                auto sp_it = ctx_.source_properties->find(abs);
                if (sp_it != ctx_.source_properties->end()) {
                    auto hfo = sp_it->second.find("HEADER_FILE_ONLY");
                    if (hfo != sp_it->second.end() && hfo->second != "0" && hfo->second != "OFF" && hfo->second != "FALSE"
                        && hfo->second != "NO" && hfo->second != "N" && !hfo->second.empty()) {
                        continue;
                    }
                }
            }

            std::string obj_str = get_obj_path(binary_dir, target_name, src);

            if (!result.empty()) { result += ";"; }
            result += obj_str;
        }
        return result;
    }

    case GenexNodeType::TARGET_PROPERTY: {
        // $<TARGET_PROPERTY:tgt,prop> or $<TARGET_PROPERTY:prop>
        auto args = GenexParser().split_genex_args(node.raw_content);
        if (args.size() != 1 && args.size() != 2) { return std::unexpected("$<TARGET_PROPERTY:...> requires 1 or 2 arguments"); }

        if (!ctx_.all_targets) { return std::unexpected("TARGET_PROPERTY requires all_targets context"); }

        GenexParser parser;
        std::string target_name;
        std::string property_name;

        if (args.size() == 1) {
            // Single argument: property name, use current target
            if (!ctx_.current_target) {
                return std::unexpected("$<TARGET_PROPERTY:" + args[0]
                                       + "> requires a target context. "
                                         "The single-argument form $<TARGET_PROPERTY:prop> may only be used "
                                         "with binary targets. Use the two-argument form "
                                         "$<TARGET_PROPERTY:tgt,prop> instead. If this appears in file(GENERATE), "
                                         "either add the TARGET keyword or ensure the surrounding code is guarded "
                                         "by a condition that prevents this path from being reached.");
            }
            target_name = ctx_.current_target->get_name();

            auto prop_result = parser.parse(args[0]);
            if (!prop_result) { return std::unexpected(prop_result.error()); }
            auto prop_val = evaluate_nodes(prop_result->nodes);
            if (!prop_val) { return prop_val; }
            property_name = *prop_val;
        } else {
            // Two arguments: target name, property name
            auto tgt_result = parser.parse(args[0]);
            if (!tgt_result) { return std::unexpected(tgt_result.error()); }
            auto tgt_val = evaluate_nodes(tgt_result->nodes);
            if (!tgt_val) { return tgt_val; }
            target_name = *tgt_val;

            auto prop_result = parser.parse(args[1]);
            if (!prop_result) { return std::unexpected(prop_result.error()); }
            auto prop_val = evaluate_nodes(prop_result->nodes);
            if (!prop_val) { return prop_val; }
            property_name = *prop_val;
        }

        // Look up the target (resolving aliases)
        auto* target = find_target(target_name);
        if (!target) { return std::unexpected("TARGET_PROPERTY: target '" + target_name + "' not found"); }

        // Handle built-in pseudo-properties first
        std::string prop_value;
        if (property_name == "NAME") {
            prop_value = target->get_name();
        } else if (property_name == "SOURCE_DIR") {
            prop_value = target->get_source_dir();
        } else if (property_name == "BINARY_DIR") {
            prop_value = target->get_binary_dir();
        } else if (property_name == "IMPORTED") {
            prop_value = target->is_imported() ? "TRUE" : "FALSE";
        } else if (property_name == "IMPORTED_LOCATION") {
            prop_value = target->get_imported_location();
        } else {
            // Check list properties then generic
            prop_value = target->get_property_combined(property_name);
        }

        // Recursively evaluate any nested genex in individual list elements.
        // Per CMake semantics, the looked-up target becomes the "head target"
        // for nested genexes (e.g. the single-arg $<TARGET_PROPERTY:prop>
        // forms that appear in INTERFACE_SOURCES of linked Qt targets).
        if (prop_value.find("$<") != std::string::npos) {
            GenexEvaluator nested(ctx_);
            nested.ctx_.current_target = target;
            std::string result;
            for (auto sv : CMakeArrayIterator(prop_value)) {
                auto eval = nested.evaluate(std::string(sv));
                if (!eval) return eval;
                if (!eval->empty()) {
                    if (!result.empty()) result += ';';
                    result += *eval;
                }
            }
            return result;
        }
        return prop_value;
    }

    case GenexNodeType::TARGET_FILE_BASE_NAME:
    case GenexNodeType::TARGET_LINKER_FILE_BASE_NAME: {
        // $<TARGET_FILE_BASE_NAME:target> / $<TARGET_LINKER_FILE_BASE_NAME:target>
        if (!ctx_.all_targets) { return std::unexpected("TARGET_FILE_BASE_NAME requires all_targets context"); }
        auto* target = find_target(node.raw_content);
        if (!target) { return std::unexpected("TARGET_FILE_BASE_NAME: target '" + node.raw_content + "' not found"); }
        std::string base = target->get_property("OUTPUT_NAME");
        if (base.empty()) { base = target->get_name(); }
        return base;
    }

    case GenexNodeType::TARGET_FILE_PREFIX:
    case GenexNodeType::TARGET_LINKER_FILE_PREFIX: {
        if (!ctx_.all_targets) { return std::unexpected("TARGET_FILE_PREFIX requires all_targets context"); }
        auto* target = find_target(node.raw_content);
        if (!target) { return std::unexpected("TARGET_FILE_PREFIX: target '" + node.raw_content + "' not found"); }
        std::string prefix_prop = target->get_property("PREFIX");
        if (!prefix_prop.empty()) { return prefix_prop; }
        auto ttype = target->get_type();
        if (ttype == TargetType::STATIC_LIBRARY) {
            return ctx_.static_library_prefix;
        } else if (ttype == TargetType::SHARED_LIBRARY) {
            return ctx_.shared_library_prefix;
        }
        return std::string("");
    }

    case GenexNodeType::TARGET_FILE_SUFFIX:
    case GenexNodeType::TARGET_LINKER_FILE_SUFFIX: {
        if (!ctx_.all_targets) { return std::unexpected("TARGET_FILE_SUFFIX requires all_targets context"); }
        auto* target = find_target(node.raw_content);
        if (!target) { return std::unexpected("TARGET_FILE_SUFFIX: target '" + node.raw_content + "' not found"); }
        auto ttype = target->get_type();
        std::string suffix_prop = target->get_property("SUFFIX");
        if (!suffix_prop.empty()) { return suffix_prop; }
        if (ttype == TargetType::STATIC_LIBRARY) {
            return ctx_.static_library_suffix;
        } else if (ttype == TargetType::SHARED_LIBRARY) {
            return ctx_.shared_library_suffix;
        } else if (ttype == TargetType::EXECUTABLE) {
            return ctx_.executable_suffix;
        }
        return std::string("");
    }

    case GenexNodeType::GENEX_EVAL: {
        // $<GENEX_EVAL:expr> - evaluate the argument, then re-evaluate as genex
        auto inner = evaluate_nodes(node.children);
        if (!inner) return inner;
        // Re-evaluate the result as a genex expression
        return evaluate(*inner);
    }

    case GenexNodeType::TARGET_GENEX_EVAL: {
        // $<TARGET_GENEX_EVAL:target,expr> - evaluate expr with target context
        auto args = GenexParser().split_genex_args(node.raw_content);
        if (args.size() != 2) { return std::unexpected("$<TARGET_GENEX_EVAL:...> requires exactly 2 arguments"); }

        // Evaluate target name
        GenexParser parser;
        auto tgt_parsed = parser.parse(args[0]);
        if (!tgt_parsed) return std::unexpected(tgt_parsed.error());
        auto tgt_name = evaluate_nodes(tgt_parsed->nodes);
        if (!tgt_name) return tgt_name;

        if (!ctx_.all_targets) { return std::unexpected("TARGET_GENEX_EVAL requires all_targets context"); }
        auto* target = find_target(*tgt_name);
        if (!target) { return std::unexpected("TARGET_GENEX_EVAL: target '" + *tgt_name + "' not found"); }

        // Evaluate the expression
        auto expr_parsed = parser.parse(args[1]);
        if (!expr_parsed) return std::unexpected(expr_parsed.error());
        auto expr_val = evaluate_nodes(expr_parsed->nodes);
        if (!expr_val) return expr_val;

        // Re-evaluate with target context
        GenexEvaluationContext target_ctx = ctx_;
        target_ctx.current_target = target;
        GenexEvaluator target_evaluator(target_ctx);
        return target_evaluator.evaluate(*expr_val);
    }

    case GenexNodeType::JOIN: {
        // $<JOIN:list,glue> - join semicolon-separated list with glue
        auto args = GenexParser().split_genex_args(node.raw_content);
        if (args.size() != 2) { return std::unexpected("$<JOIN:...> requires exactly 2 arguments"); }

        GenexParser parser;
        auto list_parsed = parser.parse(args[0]);
        if (!list_parsed) return std::unexpected(list_parsed.error());
        auto list_val = evaluate_nodes(list_parsed->nodes);
        if (!list_val) return list_val;

        auto glue_parsed = parser.parse(args[1]);
        if (!glue_parsed) return std::unexpected(glue_parsed.error());
        auto glue_val = evaluate_nodes(glue_parsed->nodes);
        if (!glue_val) return glue_val;

        // Split by semicolons and rejoin with glue
        std::string result;
        bool first = true;
        for (auto sv : CMakeArrayIterator(*list_val)) {
            if (sv.empty()) continue;
            if (!first) result += *glue_val;
            result += sv;
            first = false;
        }
        return result;
    }

    case GenexNodeType::REMOVE_DUPLICATES: {
        // $<REMOVE_DUPLICATES:list> - remove duplicate entries
        auto inner = evaluate_nodes(node.children);
        if (!inner) return inner;

        std::vector<std::string> seen;
        std::string result;
        for (auto sv : CMakeArrayIterator(*inner)) {
            std::string item(sv);
            if (std::find(seen.begin(), seen.end(), item) == seen.end()) {
                seen.push_back(item);
                if (!result.empty()) result += ';';
                result += item;
            }
        }
        return result;
    }

    case GenexNodeType::FILTER: {
        // $<FILTER:list,INCLUDE|EXCLUDE,regex>
        auto args = GenexParser().split_genex_args(node.raw_content);
        if (args.size() != 3) { return std::unexpected("$<FILTER:...> requires exactly 3 arguments"); }

        GenexParser parser;
        auto list_parsed = parser.parse(args[0]);
        if (!list_parsed) return std::unexpected(list_parsed.error());
        auto list_val = evaluate_nodes(list_parsed->nodes);
        if (!list_val) return list_val;

        bool include = (args[1] == "INCLUDE");

        auto regex_parsed = parser.parse(args[2]);
        if (!regex_parsed) return std::unexpected(regex_parsed.error());
        auto regex_val = evaluate_nodes(regex_parsed->nodes);
        if (!regex_val) return regex_val;

        std::regex re(*regex_val);
        std::string result;
        for (auto sv : CMakeArrayIterator(*list_val)) {
            std::string item(sv);
            bool matches = std::regex_search(item, re);
            if (matches == include) {
                if (!result.empty()) result += ';';
                result += item;
            }
        }
        return result;
    }

    case GenexNodeType::IN_LIST: {
        // $<IN_LIST:value,list> - returns 1 if value is in semicolon-separated list
        auto args = GenexParser().split_genex_args(node.raw_content);
        if (args.size() != 2) { return std::unexpected("$<IN_LIST:...> requires exactly 2 arguments"); }

        GenexParser parser;
        auto val_parsed = parser.parse(args[0]);
        if (!val_parsed) return std::unexpected(val_parsed.error());
        auto val_result = evaluate_nodes(val_parsed->nodes);
        if (!val_result) return val_result;

        auto list_parsed = parser.parse(args[1]);
        if (!list_parsed) return std::unexpected(list_parsed.error());
        auto list_val = evaluate_nodes(list_parsed->nodes);
        if (!list_val) return list_val;

        for (auto sv : CMakeArrayIterator(*list_val)) {
            if (sv == *val_result) return "1";
        }
        return "0";
    }

    case GenexNodeType::LOWER_CASE: {
        // $<LOWER_CASE:string>
        auto inner = evaluate_nodes(node.children);
        if (!inner) return inner;
        return to_lower(*inner);
    }

    case GenexNodeType::UPPER_CASE: {
        // $<UPPER_CASE:string>
        auto inner = evaluate_nodes(node.children);
        if (!inner) return inner;
        std::string result = *inner;
        std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return std::toupper(c); });
        return result;
    }

    case GenexNodeType::COMPILE_LANGUAGE: {
        // $<COMPILE_LANGUAGE:lang[,lang2,...]> returns 1 if compiling with any listed language
        if (ctx_.allow_deferred_compile_language && !ctx_.compile_language) {
            // Return original expression for deferred evaluation
            return "$<COMPILE_LANGUAGE:" + node.raw_content + ">";
        }

        if (!ctx_.compile_language) { return std::unexpected("COMPILE_LANGUAGE requires compile_language context"); }

        std::string current_lang;
        switch (*ctx_.compile_language) {
        case Language::C:
            current_lang = "C";
            break;
        case Language::CXX:
            current_lang = "CXX";
            break;
        case Language::CUDA:
            current_lang = "CUDA";
            break;
        default:
            current_lang = "UNKNOWN";
            break;
        }

        // Support comma-separated language list: $<COMPILE_LANGUAGE:CXX,OBJCXX>
        auto langs = GenexParser().split_genex_args(node.raw_content);
        for (const auto& lang : langs) {
            std::string lang_upper = lang;
            std::transform(lang_upper.begin(), lang_upper.end(), lang_upper.begin(), [](unsigned char c) { return std::toupper(c); });
            if (lang_upper == current_lang) return std::string("1");
        }
        return std::string("0");
    }

    case GenexNodeType::LINK_LANGUAGE: {
        // $<LINK_LANGUAGE[:lang[,lang2,...]]>
        // No-arg form returns the linker language; arg form returns
        // 1 if any listed language matches, else 0.
        // Determined by Target::get_linker_language() (LINKER_LANGUAGE
        // property if set, else inferred from the target's own sources).
        if (!ctx_.current_target) { return std::unexpected("LINK_LANGUAGE requires a target context"); }
        std::string current_lang;
        switch (ctx_.current_target->get_linker_language()) {
        case Language::C:
            current_lang = "C";
            break;
        case Language::CXX:
            current_lang = "CXX";
            break;
        case Language::CUDA:
            current_lang = "CUDA";
            break;
        case Language::ASM:
            current_lang = "ASM";
            break;
        default:
            current_lang = "CXX";
            break;
        }

        if (node.raw_content.empty()) return current_lang;

        for (const auto& lang : GenexParser().split_genex_args(node.raw_content)) {
            std::string upper = lang;
            std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return std::toupper(c); });
            if (upper == current_lang) return std::string("1");
        }
        return std::string("0");
    }

    case GenexNodeType::LINK_GROUP: {
        // $<LINK_GROUP:feature,arg1,arg2,...>
        // Wraps args with linker prologue/epilogue from
        // CMAKE_LINK_GROUP_USING_<feature> (a list of typically two
        // entries: the prologue and epilogue strings). Result is a
        // CMake list so each item appears as a separate link argument.
        if (!ctx_.interp) { return std::unexpected("LINK_GROUP requires an interpreter context"); }
        auto args = GenexParser().split_genex_args(node.raw_content);
        if (args.empty()) { return std::unexpected("LINK_GROUP requires at least a feature name"); }
        const std::string& feature = args[0];

        // CMAKE_LINK_GROUP_USING_<feature> stores the wrapping (a list).
        std::string wrapping = ctx_.interp->get_variable("CMAKE_LINK_GROUP_USING_" + feature);

        std::vector<std::string> result;
        std::string prologue, epilogue;

        if (!wrapping.empty()) {
            std::vector<std::string> parts;
            for (auto sv : CMakeArrayIterator(wrapping)) parts.emplace_back(sv);
            if (parts.size() == 1) {
                prologue = std::move(parts[0]);
            } else if (parts.size() >= 2) {
                prologue = std::move(parts.front());
                epilogue = std::move(parts.back());
            }
        }
        // If the feature isn't defined, emit the libs without wrapping.
        // CMake errors in that case; we degrade gracefully — the link
        // still happens, just without the requested grouping semantics.

        if (!prologue.empty()) result.push_back(prologue);
        for (size_t i = 1; i < args.size(); ++i) result.push_back(args[i]);
        if (!epilogue.empty()) result.push_back(epilogue);

        std::string joined;
        for (size_t i = 0; i < result.size(); ++i) {
            if (i > 0) joined += ";";
            joined += result[i];
        }
        return joined;
    }

    case GenexNodeType::COMPILE_LANG_AND_ID: {
        // $<COMPILE_LANG_AND_ID:language,compiler_id1,compiler_id2,...>
        // Returns 1 if:
        //   - compilation language matches the first argument
        //   - AND compiler ID for that language matches any of the remaining arguments
        if (ctx_.allow_deferred_compile_language && !ctx_.compile_language) {
            // Return original expression for deferred evaluation
            return "$<COMPILE_LANG_AND_ID:" + node.raw_content + ">";
        }

        if (!ctx_.compile_language) { return std::unexpected("COMPILE_LANG_AND_ID requires compile_language context"); }

        auto args = GenexParser().split_genex_args(node.raw_content);
        if (args.size() < 2) { return std::unexpected("$<COMPILE_LANG_AND_ID:...> requires at least 2 arguments (language,compiler_id)"); }

        // First argument is the language
        std::string lang_upper = args[0];
        std::transform(lang_upper.begin(), lang_upper.end(), lang_upper.begin(), [](unsigned char c) { return std::toupper(c); });

        std::string current_lang;
        switch (*ctx_.compile_language) {
        case Language::C:
            current_lang = "C";
            break;
        case Language::CXX:
            current_lang = "CXX";
            break;
        case Language::CUDA:
            current_lang = "CUDA";
            break;
        default:
            current_lang = "UNKNOWN";
            break;
        }

        // If language doesn't match, return 0
        if (lang_upper != current_lang) { return "0"; }

        // Get the compiler ID for the matched language
        std::string compiler_id;
        if (current_lang == "C") {
            compiler_id = ctx_.c_compiler_id;
        } else if (current_lang == "CXX") {
            compiler_id = ctx_.cxx_compiler_id;
        } else {
            // For other languages, we don't have compiler ID in context yet
            return "0";
        }

        // Check if compiler ID matches any of the provided IDs
        for (size_t i = 1; i < args.size(); ++i) {
            if (args[i] == compiler_id) { return "1"; }
        }

        return "0";
    }

    case GenexNodeType::PLATFORM_ID: {
        // $<PLATFORM_ID:platform> returns 1 if CMAKE_SYSTEM_NAME matches
        return (node.raw_content == ctx_.system_name) ? "1" : "0";
    }

    case GenexNodeType::CXX_COMPILER_ID: {
        // $<CXX_COMPILER_ID:id> returns 1 if CMAKE_CXX_COMPILER_ID matches
        return (node.raw_content == ctx_.cxx_compiler_id) ? "1" : "0";
    }

    case GenexNodeType::C_COMPILER_ID: {
        // $<C_COMPILER_ID:id> returns 1 if CMAKE_C_COMPILER_ID matches
        return (node.raw_content == ctx_.c_compiler_id) ? "1" : "0";
    }

    case GenexNodeType::CXX_COMPILER_VERSION: {
        // $<CXX_COMPILER_VERSION> returns CMAKE_CXX_COMPILER_VERSION
        return ctx_.cxx_compiler_version;
    }

    case GenexNodeType::C_COMPILER_VERSION: {
        // $<C_COMPILER_VERSION> returns CMAKE_C_COMPILER_VERSION
        return ctx_.c_compiler_version;
    }

    case GenexNodeType::CONDITIONAL: {
        // $<cond:text> - first child is condition, remaining children are value.
        if (node.children.empty()) { return std::unexpected("CONDITIONAL genex requires condition and value"); }

        auto cond_val = evaluate_node(*node.children[0]);
        if (!cond_val) { return cond_val; }

        // Deferred genex in condition (e.g. $<COMPILE_LANG_AND_ID:...>):
        // reconstruct the expression for per-source evaluation later.
        if (cond_val->find("$<") != std::string::npos) { return "$<" + *cond_val + ":" + node.raw_content + ">"; }

        if (is_truthy(*cond_val)) {
            std::string result;
            for (size_t i = 1; i < node.children.size(); ++i) {
                auto val_result = evaluate_node(*node.children[i]);
                if (!val_result) { return val_result; }
                result += *val_result;
            }
            return result;
        }

        return std::string("");
    }

    case GenexNodeType::UNSUPPORTED:
        return std::unexpected("Unsupported generator expression: " + node.raw_content);

    default:
        return std::unexpected("Unknown generator expression type");
    }
}

std::expected<std::string, std::string> GenexEvaluator::evaluate(const std::string& input) {
    if (!GenexParser::contains_genex(input)) { return input; }

    GenexParser parser;
    parser.set_recovery(true);
    auto parse_result = parser.parse(input);
    if (!parse_result) { return std::unexpected(parse_result.error()); }

    if (!parse_result->has_genex) { return input; }

    return evaluate_nodes(parse_result->nodes);
}

std::expected<std::vector<std::string>, std::string> GenexEvaluator::evaluate_property_list(const std::vector<std::string>& values) {
    std::vector<std::string> result;

    // Reassemble fragmented genex: multi-line genex like $<$<BOOL:TRUE>:\n-Wall\n>
    // are stored as separate list items. Join fragments with semicolons until
    // angle brackets balance, matching CMake's behavior.
    std::vector<std::string> reassembled;
    std::string pending;
    int depth = 0;
    for (const auto& value : values) {
        if (depth > 0) {
            // We're inside a fragmented genex, join with semicolon
            pending += ';';
            pending += value;
        } else {
            pending = value;
        }
        // Count unbalanced $< and > in this item
        for (size_t i = 0; i < value.size(); ++i) {
            if (value[i] == '$' && i + 1 < value.size() && value[i + 1] == '<') {
                depth++;
                i++; // skip '<'
            } else if (value[i] == '>' && depth > 0) {
                depth--;
            }
        }
        if (depth == 0) {
            reassembled.push_back(std::move(pending));
            pending.clear();
        }
    }
    // If there's still a pending fragment (truly unbalanced), add it as-is
    if (!pending.empty()) { reassembled.push_back(std::move(pending)); }

    for (const auto& value : reassembled) {
        // Fast path: no genex marker at all — skip parsing entirely
        if (!GenexParser::contains_genex(value)) {
            if (!value.empty()) { result.push_back(value); }
            continue;
        }

        // Parse once — reuse for both genex classification and evaluation
        GenexParser parser;
        parser.set_recovery(true);
        auto parse_result = parser.parse(value);
        if (!parse_result) { return std::unexpected(parse_result.error()); }

        // Check if the value is ONLY a generator expression (single non-literal node)
        // If so, split the result by whitespace (like unquoted arguments in CMake)
        // Otherwise, keep it as a single value (like quoted arguments or mixed content)
        bool is_pure_genex =
            parse_result->has_genex && parse_result->nodes.size() == 1 && parse_result->nodes[0]->type != GenexNodeType::LITERAL;

        auto eval_result = evaluate_nodes(parse_result->nodes);
        if (!eval_result) { return std::unexpected(eval_result.error()); }

        // Only add non-empty results
        if (!eval_result->empty()) {
            // If result still contains deferred genex (e.g., $<COMPILE_LANG_AND_ID:...>),
            // don't split it - keep it intact for per-source evaluation
            if (eval_result->find("$<") != std::string::npos) {
                result.push_back(*eval_result);
            } else if (is_pure_genex) {
                // Split the evaluated result by semicolons first (CMake list separator),
                // then by whitespace (treats genex output like unquoted arguments).
                // This allows:
                //   - $<1:-Wall -Wextra> to expand into multiple separate flags
                //   - $<$<BOOL:ON>:${LIST_VAR}> to expand semicolon-separated lists
                for (auto sv : CMakeArrayIterator(*eval_result)) {
                    // Further split by whitespace
                    std::istringstream iss{std::string(sv)};
                    std::string token;
                    while (iss >> token) { result.push_back(token); }
                }
            } else {
                // Keep as a single value (like a quoted argument or mixed content)
                result.push_back(*eval_result);
            }
        }
    }

    return result;
}

std::expected<LinkLibraryResult, std::string> GenexEvaluator::evaluate_link_library(const std::string& input) {
    // Fast path: no genex — just a plain library name
    if (!GenexParser::contains_genex(input)) { return LinkLibraryResult{.value = input, .link_only = false}; }

    GenexParser parser;
    parser.set_recovery(true);
    auto parse_result = parser.parse(input);
    if (!parse_result) { return std::unexpected(parse_result.error()); }

    LinkLibraryResult result;

    // Check if the top-level is a LINK_ONLY wrapper
    if (parse_result->nodes.size() == 1 && parse_result->nodes[0]->type == GenexNodeType::LINK_ONLY) {
        result.link_only = true;
        // Evaluate the inner content
        auto inner_result = evaluate_nodes(parse_result->nodes[0]->children);
        if (!inner_result) { return std::unexpected(inner_result.error()); }
        result.value = *inner_result;
    } else {
        // Normal evaluation
        auto eval_result = evaluate_nodes(parse_result->nodes);
        if (!eval_result) { return std::unexpected(eval_result.error()); }
        result.value = *eval_result;
    }

    return result;
}

} // namespace kiln
