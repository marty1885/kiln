#include "install_executor.hpp"
#include "interperter.hpp"
#include "target.hpp"
#include "genex_evaluator.hpp"
#include "printing.hpp"
#include "utils.hpp"
#include "builtins/export_generator.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstring>
#include <sys/stat.h>

namespace kiln {

namespace {

bool matches_glob(const std::string& text, const std::string& pattern) {
    size_t ti = 0, pi = 0;
    size_t star_p = std::string::npos, star_t = 0;
    while (ti < text.size()) {
        if (pi < pattern.size() && (pattern[pi] == '?' || pattern[pi] == text[ti])) {
            ++ti;
            ++pi;
        } else if (pi < pattern.size() && pattern[pi] == '*') {
            star_p = pi++;
            star_t = ti;
        } else if (star_p != std::string::npos) {
            pi = star_p + 1;
            ti = ++star_t;
        } else {
            return false;
        }
    }
    while (pi < pattern.size() && pattern[pi] == '*') ++pi;
    return pi == pattern.size();
}

bool files_have_same_content(const std::filesystem::path& a, const std::filesystem::path& b) {
    std::error_code ec;
    auto sa = std::filesystem::file_size(a, ec);
    if (ec) return false;
    auto sb = std::filesystem::file_size(b, ec);
    if (ec) return false;
    if (sa != sb) return false;
    std::ifstream fa(a, std::ios::binary);
    std::ifstream fb(b, std::ios::binary);
    if (!fa || !fb) return false;
    constexpr size_t BUF = 64 * 1024;
    std::vector<char> ba(BUF), bb(BUF);
    while (fa && fb) {
        fa.read(ba.data(), BUF);
        fb.read(bb.data(), BUF);
        auto na = fa.gcount();
        auto nb = fb.gcount();
        if (na != nb) return false;
        if (na == 0) break;
        if (std::memcmp(ba.data(), bb.data(), static_cast<size_t>(na)) != 0) return false;
    }
    return true;
}

bool file_has_perms(const std::filesystem::path& path, mode_t perms) {
    struct stat st;
    if (stat(path.string().c_str(), &st) != 0) return false;
    return (st.st_mode & 0777) == (perms & 0777);
}

bool string_matches_file_content(const std::filesystem::path& path, const std::string& content) {
    std::error_code ec;
    auto sz = std::filesystem::file_size(path, ec);
    if (ec) return false;
    if (sz != content.size()) return false;
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<char> buf(content.size());
    f.read(buf.data(), static_cast<std::streamsize>(content.size()));
    if (static_cast<size_t>(f.gcount()) != content.size()) return false;
    return std::memcmp(buf.data(), content.data(), content.size()) == 0;
}

mode_t parse_cmake_perms(const std::vector<std::string>& cmake_perms, mode_t default_mode) {
    if (cmake_perms.empty()) return default_mode;
    mode_t mode = 0;
    for (const auto& perm : cmake_perms) {
        if (perm == "OWNER_READ")
            mode |= S_IRUSR;
        else if (perm == "OWNER_WRITE")
            mode |= S_IWUSR;
        else if (perm == "OWNER_EXECUTE")
            mode |= S_IXUSR;
        else if (perm == "GROUP_READ")
            mode |= S_IRGRP;
        else if (perm == "GROUP_WRITE")
            mode |= S_IWGRP;
        else if (perm == "GROUP_EXECUTE")
            mode |= S_IXGRP;
        else if (perm == "WORLD_READ")
            mode |= S_IROTH;
        else if (perm == "WORLD_WRITE")
            mode |= S_IWOTH;
        else if (perm == "WORLD_EXECUTE")
            mode |= S_IXOTH;
    }
    return mode;
}

// Copy fields shared by every op type from an InstallDestination.
void apply_destination(InstallOp& op, const InstallDestination& dest) {
    op.component = dest.component;
    op.configurations = dest.configurations;
    op.exclude_from_all = dest.exclude_from_all;
    op.optional = dest.optional;
}

InstallOp make_copy_file_op(const std::string& src, const std::string& dest_rel, mode_t perms) {
    InstallOp op;
    op.kind = "copy_file";
    op.src = src;
    op.dest = dest_rel;
    op.perms = mode_to_rwx(perms);
    return op;
}

InstallOp make_symlink_op(const std::string& dest_rel, const std::string& target) {
    InstallOp op;
    op.kind = "symlink";
    op.dest = dest_rel;
    op.symlink_target = target;
    return op;
}

// Enumerate the symlink chain for a versioned shared library. Mirrors what
// create_library_symlinks used to do at install time.
//
// For libfoo.so.1.2.3 with version=1.2.3, soversion=1, the artifact filename
// is libfoo.so.1.2.3 and we emit:
//   libfoo.so.1   -> libfoo.so.1.2.3
//   libfoo.so     -> libfoo.so.1
// dest_dir is the install-relative directory the .so chain lives in.
std::expected<std::vector<InstallOp>, std::string> compute_lib_symlinks(const std::filesystem::path& artifact_filename,
                                                                        const std::string& dest_dir_rel, const std::string& soversion,
                                                                        const std::string& version, const InstallDestination& dest_meta) {
    std::vector<InstallOp> ops;
    std::string filename = artifact_filename.string();
    size_t so_pos = filename.find(".so");
    if (so_pos == std::string::npos) { return std::unexpected("invalid shared library filename: " + filename); }
    std::string basename = filename.substr(0, so_pos);
    std::string ext = ".so";
    std::filesystem::path dir = dest_dir_rel;

    auto push = [&](const std::string& link_name, const std::string& target_name) {
        InstallOp op = make_symlink_op((dir / link_name).generic_string(), target_name);
        apply_destination(op, dest_meta);
        ops.push_back(std::move(op));
    };

    if (!version.empty()) {
        std::string versioned = basename + ext + "." + version;
        if (!soversion.empty()) {
            std::string soversion_link = basename + ext + "." + soversion;
            push(soversion_link, versioned);
            push(basename + ext, soversion_link);
        } else {
            push(basename + ext, versioned);
        }
    } else if (!soversion.empty()) {
        std::string soversioned = basename + ext + "." + soversion;
        push(basename + ext, soversioned);
    }
    return ops;
}

std::expected<void, std::string> resolve_targets_rule(Interpreter* interp, const InstallTargetsRule& rule,
                                                      std::vector<InstallOp>& out_ops) {
    auto genex_ctx = GenexEvaluationContext::from_interpreter(*interp, interp->get_targets());
    genex_ctx.phase = GenexEvaluationContext::Phase::INSTALL;

    for (const auto& target_name : rule.targets) {
        auto* target = interp->find_target(target_name);
        if (!target) { return std::unexpected("unknown target in install(TARGETS): " + target_name); }

        std::filesystem::path artifact_path;
        const InstallDestination* dest = nullptr;
        mode_t default_perms = 0;
        bool is_shared = false;

        switch (target->get_type()) {
        case TargetType::EXECUTABLE:
            artifact_path = target->get_output_path();
            dest = &rule.runtime_dest;
            default_perms = 0755;
            break;
        case TargetType::SHARED_LIBRARY:
            artifact_path = target->get_output_path();
            dest = &rule.library_dest;
            default_perms = 0755;
            is_shared = true;
            break;
        case TargetType::STATIC_LIBRARY:
            artifact_path = target->get_output_path();
            dest = &rule.archive_dest;
            default_perms = 0644;
            break;
        case TargetType::INTERFACE_LIBRARY:
            continue;
        default:
            continue;
        }

        if (dest->destination.empty()) continue;

        mode_t perms = parse_cmake_perms(dest->permissions, default_perms);
        std::filesystem::path dest_rel = std::filesystem::path(dest->destination) / artifact_path.filename();

        InstallOp file_op = make_copy_file_op(artifact_path.string(), dest_rel.generic_string(), perms);
        apply_destination(file_op, *dest);
        out_ops.push_back(std::move(file_op));

        auto target_ctx = genex_ctx;
        target_ctx.current_target = target;
        GenexEvaluator eval(target_ctx);

        if (is_shared) {
            std::string version = eval.evaluate_target_property(*target, "VERSION");
            std::string soversion = eval.evaluate_target_property(*target, "SOVERSION");
            if (!version.empty() || !soversion.empty()) {
                auto links = compute_lib_symlinks(artifact_path.filename(), dest->destination, soversion, version, *dest);
                if (!links) return std::unexpected(links.error());
                for (auto& op : *links) out_ops.push_back(std::move(op));
            }
        }

        // PUBLIC_HEADER fan-out.
        if (!rule.public_header_dest.destination.empty()) {
            std::string public_headers = eval.evaluate_target_property(*target, "PUBLIC_HEADER");
            if (!public_headers.empty()) {
                mode_t header_perms = parse_cmake_perms(rule.public_header_dest.permissions, 0644);
                std::istringstream ss(public_headers);
                std::string header;
                while (std::getline(ss, header, ';')) {
                    if (header.empty()) continue;
                    std::filesystem::path header_path = header;
                    if (!header_path.is_absolute()) { header_path = std::filesystem::path(target->get_source_dir()) / header_path; }
                    std::filesystem::path header_dest = std::filesystem::path(rule.public_header_dest.destination) / header_path.filename();
                    InstallOp op = make_copy_file_op(header_path.string(), header_dest.generic_string(), header_perms);
                    apply_destination(op, rule.public_header_dest);
                    out_ops.push_back(std::move(op));
                }
            }
        }
    }
    return {};
}

void resolve_files_rule(const InstallFilesRule& rule, std::vector<InstallOp>& out_ops) {
    mode_t default_perms = rule.is_programs ? 0755 : 0644;
    mode_t perms = parse_cmake_perms(rule.destination.permissions, default_perms);
    std::filesystem::path dest_dir = rule.destination.destination;
    for (const auto& source_file : rule.files) {
        std::filesystem::path source = source_file;
        std::filesystem::path dest = rule.rename.empty() ? dest_dir / source.filename() : dest_dir / rule.rename;
        InstallOp op = make_copy_file_op(source.string(), dest.generic_string(), perms);
        apply_destination(op, rule.destination);
        out_ops.push_back(std::move(op));
    }
}

void resolve_directory_rule(const InstallDirectoryRule& rule, std::vector<InstallOp>& out_ops) {
    for (const auto& source_dir : rule.directories) {
        std::filesystem::path src_path = source_dir;
        bool install_contents = !source_dir.empty() && source_dir.back() == '/';

        InstallOp op;
        op.kind = "copy_directory";
        op.src = src_path.string();
        op.dest = rule.destination.destination;
        op.patterns = rule.file_patterns;
        op.excludes = rule.exclude_patterns;
        op.use_source_permissions = rule.use_source_permissions;
        op.preserve_dir_name = !install_contents;
        mode_t file_perms = parse_cmake_perms(rule.destination.permissions, 0644);
        op.file_perms = mode_to_rwx(file_perms);
        op.dir_perms = mode_to_rwx(0755);
        apply_destination(op, rule.destination);
        out_ops.push_back(std::move(op));
    }
}

std::expected<void, std::string> resolve_export_rule(Interpreter* interp, const InstallExportRule& rule, const std::string& default_prefix,
                                                     const std::string& current_config, std::vector<InstallOp>& out_ops) {
    const auto& export_sets = interp->get_export_sets();
    auto it = export_sets.find(rule.export_name);
    if (it == export_sets.end() || it->second.empty()) return {};

    std::vector<Target*> targets_to_export;
    std::unordered_map<std::string, ExportContext::InstallDests> dests;
    for (const auto& entry : it->second) {
        auto* target = interp->find_target(entry.target_name);
        if (target) {
            targets_to_export.push_back(target);
            dests[entry.target_name] = {entry.archive_dest, entry.library_dest, entry.runtime_dest};
        }
    }
    if (targets_to_export.empty()) return {};

    std::string filename = rule.file_name.empty() ? rule.export_name + ".cmake" : rule.file_name;

    ExportContext ctx;
    ctx.for_install = true;
    ctx.namespace_prefix = rule.namespace_prefix;
    ctx.destination = rule.destination;
    ctx.install_prefix = default_prefix;
    ctx.build_type = interp->get_variable("CMAKE_BUILD_TYPE");
    ctx.config = current_config;
    ctx.system_name = interp->get_variable("CMAKE_SYSTEM_NAME");
    ctx.cxx_compiler_id = interp->get_variable("CMAKE_CXX_COMPILER_ID");
    ctx.c_compiler_id = interp->get_variable("CMAKE_C_COMPILER_ID");
    ctx.cxx_compiler_version = interp->get_variable("CMAKE_CXX_COMPILER_VERSION");
    ctx.c_compiler_version = interp->get_variable("CMAKE_C_COMPILER_VERSION");
    ctx.all_targets = &interp->get_targets();
    ctx.target_aliases = &interp->get_target_aliases();
    ctx.target_install_dests = std::move(dests);

    std::string main_content = generate_export_content(ctx, targets_to_export);
    std::string config_content = generate_config_export_content(ctx, targets_to_export, default_prefix);

    // Per-config filename (matches the executor's prior behaviour).
    std::string config_lower = current_config;
    std::transform(config_lower.begin(), config_lower.end(), config_lower.begin(), ::tolower);
    std::string base_name = filename;
    if (base_name.size() > 6 && base_name.substr(base_name.size() - 6) == ".cmake") {
        base_name = base_name.substr(0, base_name.size() - 6);
    }
    std::string config_filename = base_name + "-" + (config_lower.empty() ? "noconfig" : config_lower) + ".cmake";

    std::filesystem::path dest_dir = rule.destination;

    InstallOp main_op;
    main_op.kind = "write_content";
    main_op.dest = (dest_dir / filename).generic_string();
    main_op.content = std::move(main_content);
    main_op.perms = mode_to_rwx(0644);
    main_op.component = rule.component;
    out_ops.push_back(std::move(main_op));

    InstallOp cfg_op;
    cfg_op.kind = "write_content";
    cfg_op.dest = (dest_dir / config_filename).generic_string();
    cfg_op.content = std::move(config_content);
    cfg_op.perms = mode_to_rwx(0644);
    cfg_op.component = rule.component;
    out_ops.push_back(std::move(cfg_op));
    return {};
}

// ---- execute side --------------------------------------------------------

bool config_matches(const std::vector<std::string>& configs, const std::string& current) {
    if (configs.empty()) return true;
    for (const auto& c : configs) {
        if (to_lower(c) == to_lower(current)) return true;
    }
    return false;
}

bool op_filtered_out(const InstallOp& op, const std::string& current_config, const std::string& component_filter) {
    if (op.exclude_from_all && component_filter.empty()) return true;
    if (!component_filter.empty() && op.component != component_filter) return true;
    if (!config_matches(op.configurations, current_config)) return true;
    return false;
}

std::expected<void, std::string> chmod_path(const std::filesystem::path& path, mode_t mode) {
    if (chmod(path.string().c_str(), mode) != 0) { return std::unexpected("failed to set permissions on " + path.string()); }
    return {};
}

std::expected<void, std::string> copy_one_file(const std::filesystem::path& src, const std::filesystem::path& dest, mode_t perms,
                                               bool optional, const OutputCtx& out) {
    if (!std::filesystem::exists(src)) {
        if (optional) return {};
        return std::unexpected("source file does not exist: " + src.string());
    }
    std::error_code equiv_ec;
    if (std::filesystem::equivalent(src, dest, equiv_ec)) {
        print_action(out, "Up-to-date", dest.string());
        return {};
    }
    if (std::filesystem::exists(dest) && file_has_perms(dest, perms) && files_have_same_content(src, dest)) {
        print_action(out, "Up-to-date", dest.string());
        return {};
    }
    std::filesystem::path dest_dir = dest.parent_path();
    if (!dest_dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(dest_dir, ec);
        if (ec) return std::unexpected("failed to create directory " + dest_dir.string() + ": " + ec.message());
    }
    std::error_code ec;
    std::filesystem::copy_file(src, dest, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) return std::unexpected("failed to copy " + src.string() + " to " + dest.string() + ": " + ec.message());
    if (auto r = chmod_path(dest, perms); !r) return r;
    print_action(out, "Installing", dest.string());
    return {};
}

std::expected<void, std::string> execute_copy_file(const InstallOp& op, const std::filesystem::path& prefix, const OutputCtx& out) {
    auto perms_or = rwx_to_mode(op.perms);
    if (!perms_or) return std::unexpected(perms_or.error());
    return copy_one_file(op.src, prefix / op.dest, *perms_or, op.optional, out);
}

std::expected<void, std::string> execute_symlink(const InstallOp& op, const std::filesystem::path& prefix, const OutputCtx& out) {
    std::filesystem::path dest = prefix / op.dest;
    {
        std::error_code ec;
        if (std::filesystem::is_symlink(dest, ec)) {
            auto existing = std::filesystem::read_symlink(dest, ec);
            if (!ec && existing == std::filesystem::path(op.symlink_target)) {
                print_action(out, "Up-to-date", dest.string());
                return {};
            }
        }
    }
    std::filesystem::path dest_dir = dest.parent_path();
    if (!dest_dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(dest_dir, ec);
        if (ec) return std::unexpected("failed to create directory " + dest_dir.string() + ": " + ec.message());
    }
    std::error_code ec;
    std::filesystem::remove(dest, ec);
    std::filesystem::create_symlink(op.symlink_target, dest, ec);
    if (ec) return std::unexpected("failed to create symlink " + dest.string() + ": " + ec.message());
    print_action(out, "Installing", dest.string());
    return {};
}

std::expected<void, std::string> execute_write_content(const InstallOp& op, const std::filesystem::path& prefix, const OutputCtx& out) {
    std::filesystem::path dest = prefix / op.dest;
    std::optional<mode_t> want_perms;
    if (!op.perms.empty()) {
        auto perms_or = rwx_to_mode(op.perms);
        if (!perms_or) return std::unexpected(perms_or.error());
        want_perms = *perms_or;
    }
    if (std::filesystem::exists(dest) && string_matches_file_content(dest, op.content)
        && (!want_perms || file_has_perms(dest, *want_perms))) {
        print_action(out, "Up-to-date", dest.string());
        return {};
    }
    std::filesystem::path dest_dir = dest.parent_path();
    if (!dest_dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(dest_dir, ec);
        if (ec) return std::unexpected("failed to create directory " + dest_dir.string() + ": " + ec.message());
    }
    std::ofstream f(dest);
    if (!f) return std::unexpected("failed to open " + dest.string() + " for writing");
    f << op.content;
    if (!f) return std::unexpected("failed to write " + dest.string());
    f.close();
    if (want_perms) {
        if (auto r = chmod_path(dest, *want_perms); !r) return r;
    }
    print_action(out, "Installing", dest.string());
    return {};
}

std::expected<void, std::string> execute_copy_directory(const InstallOp& op, const std::filesystem::path& prefix, const OutputCtx& out) {
    std::filesystem::path src_path = op.src;
    if (!std::filesystem::exists(src_path)) {
        if (op.optional) return {};
        return std::unexpected("source directory does not exist: " + op.src);
    }
    auto file_perms_or = rwx_to_mode(op.file_perms.empty() ? "rw-r--r--" : op.file_perms);
    if (!file_perms_or) return std::unexpected(file_perms_or.error());
    mode_t default_perms = *file_perms_or;

    std::filesystem::path base_dest = prefix / op.dest;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(src_path)) {
        if (!entry.is_regular_file()) continue;
        std::filesystem::path file_path = entry.path();
        std::filesystem::path relative = std::filesystem::relative(file_path, src_path);

        bool matches = op.patterns.empty();
        for (const auto& pat : op.patterns) {
            if (matches_glob(file_path.filename().string(), pat)) {
                matches = true;
                break;
            }
        }
        for (const auto& pat : op.excludes) {
            if (matches_glob(file_path.filename().string(), pat)) {
                matches = false;
                break;
            }
        }
        if (!matches) continue;

        std::filesystem::path dest = op.preserve_dir_name ? (base_dest / src_path.filename() / relative) : (base_dest / relative);

        mode_t perms;
        if (op.use_source_permissions) {
            struct stat st;
            perms = (stat(file_path.string().c_str(), &st) == 0) ? (st.st_mode & 0777) : default_perms;
        } else {
            perms = default_perms;
        }
        if (auto r = copy_one_file(file_path, dest, perms, op.optional, out); !r) return r;
    }
    return {};
}

} // namespace

std::expected<InstallPlan, std::string> build_install_plan(Interpreter* interp, const std::vector<std::shared_ptr<InstallRule>>& rules,
                                                           const std::string& default_prefix, const std::string& current_config,
                                                           const OutputCtx& out) {
    InstallPlan plan;
    plan.version = INSTALL_PLAN_VERSION;
    plan.config = current_config;
    plan.default_prefix = default_prefix;

    for (const auto& rule : rules) {
        switch (rule->type) {
        case InstallRuleType::TARGETS:
            if (auto r = resolve_targets_rule(interp, *rule->targets_rule, plan.ops); !r) return std::unexpected(r.error());
            break;
        case InstallRuleType::FILES:
        case InstallRuleType::PROGRAMS:
            resolve_files_rule(*rule->files_rule, plan.ops);
            break;
        case InstallRuleType::DIRECTORY:
            resolve_directory_rule(*rule->directory_rule, plan.ops);
            break;
        case InstallRuleType::EXPORT:
            if (auto r = resolve_export_rule(interp, *rule->export_rule, default_prefix, current_config, plan.ops); !r)
                return std::unexpected(r.error());
            break;
        case InstallRuleType::SCRIPT:
        case InstallRuleType::CODE:
            print_message(out, "WARNING", "install(SCRIPT/CODE) is not yet implemented; skipping");
            break;
        }
    }
    return plan;
}

std::expected<void, std::string> execute_install_plan(const InstallPlan& plan, const std::string& install_prefix,
                                                      const std::string& current_config, const std::string& component_filter,
                                                      const OutputCtx& out) {
    std::filesystem::path prefix = install_prefix;

    // Pre-flight: stat every copy_file source. Catches the wiped-build-dir case
    // before any writes touch the prefix.
    std::vector<std::string> missing;
    for (const auto& op : plan.ops) {
        if (op_filtered_out(op, current_config, component_filter)) continue;
        if (op.kind == "copy_file" && !op.optional && !std::filesystem::exists(op.src)) { missing.push_back(op.src); }
    }
    if (!missing.empty()) {
        std::string msg = "install plan references missing build artifacts (run `kiln build` first):";
        for (const auto& m : missing) msg += "\n  " + m;
        return std::unexpected(msg);
    }

    for (const auto& op : plan.ops) {
        if (op_filtered_out(op, current_config, component_filter)) continue;
        std::expected<void, std::string> r;
        if (op.kind == "copy_file")
            r = execute_copy_file(op, prefix, out);
        else if (op.kind == "symlink")
            r = execute_symlink(op, prefix, out);
        else if (op.kind == "write_content")
            r = execute_write_content(op, prefix, out);
        else if (op.kind == "copy_directory")
            r = execute_copy_directory(op, prefix, out);
        else
            r = std::unexpected("unknown install op kind: " + op.kind);
        if (!r) return r;
    }
    return {};
}

} // namespace kiln
