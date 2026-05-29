#include "install_plan.hpp"
#include <fstream>
#include <sys/stat.h>
#include "glaze/glaze.hpp"

template <> struct glz::meta<kiln::InstallOp> {
    using T = kiln::InstallOp;
    static constexpr auto value = glz::object(
        "kind", &T::kind, "dest", &T::dest, "component", &T::component, "configurations", &T::configurations, "exclude_from_all",
        &T::exclude_from_all, "optional", &T::optional, "src", &T::src, "perms", &T::perms, "content", &T::content, "symlink_target",
        &T::symlink_target, "patterns", &T::patterns, "excludes", &T::excludes, "use_source_permissions", &T::use_source_permissions,
        "preserve_dir_name", &T::preserve_dir_name, "file_perms", &T::file_perms, "dir_perms", &T::dir_perms);
};

template <> struct glz::meta<kiln::InstallPlan> {
    using T = kiln::InstallPlan;
    static constexpr auto value = glz::object("version", &T::version, "kiln_version", &T::kiln_version, "config", &T::config,
                                              "default_prefix", &T::default_prefix, "ops", &T::ops);
};

namespace kiln {

std::string mode_to_rwx(mode_t mode) {
    std::string s(9, '-');
    if (mode & S_IRUSR) s[0] = 'r';
    if (mode & S_IWUSR) s[1] = 'w';
    if (mode & S_IXUSR) s[2] = 'x';
    if (mode & S_IRGRP) s[3] = 'r';
    if (mode & S_IWGRP) s[4] = 'w';
    if (mode & S_IXGRP) s[5] = 'x';
    if (mode & S_IROTH) s[6] = 'r';
    if (mode & S_IWOTH) s[7] = 'w';
    if (mode & S_IXOTH) s[8] = 'x';
    return s;
}

std::expected<mode_t, std::string> rwx_to_mode(const std::string& rwx) {
    if (rwx.size() != 9) { return std::unexpected("invalid rwx string (expected 9 chars): " + rwx); }
    mode_t mode = 0;
    constexpr char expected[9] = {'r', 'w', 'x', 'r', 'w', 'x', 'r', 'w', 'x'};
    constexpr mode_t bits[9] = {
        S_IRUSR, S_IWUSR, S_IXUSR, S_IRGRP, S_IWGRP, S_IXGRP, S_IROTH, S_IWOTH, S_IXOTH,
    };
    for (size_t i = 0; i < 9; ++i) {
        if (rwx[i] == expected[i]) {
            mode |= bits[i];
        } else if (rwx[i] != '-') {
            return std::unexpected("invalid rwx char at position " + std::to_string(i) + ": " + rwx);
        }
    }
    return mode;
}

std::expected<void, std::string> save_install_plan(const InstallPlan& plan, const std::filesystem::path& path) {
    std::string json_str;
    auto write_result = glz::write<glz::opts{.prettify = true}>(plan, json_str);
    if (write_result) { return std::unexpected("failed to serialize install plan: " + glz::format_error(write_result)); }

    std::filesystem::path parent = path.parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec) { return std::unexpected("failed to create plan directory: " + ec.message()); }
    }

    std::filesystem::path tmp = path.string() + ".tmp";
    {
        std::ofstream f(tmp);
        if (!f) { return std::unexpected("failed to open temp plan file: " + tmp.string()); }
        f << json_str;
        if (!f) { return std::unexpected("failed to write temp plan file: " + tmp.string()); }
    }

    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) { return std::unexpected("failed to rename plan file: " + ec.message()); }
    return {};
}

std::expected<InstallPlan, std::string> load_install_plan(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        return std::unexpected("install plan not found: " + path.string() + " (run `kiln build` first)");
    }
    std::ifstream f(path, std::ios::ate | std::ios::binary);
    if (!f) { return std::unexpected("failed to open install plan: " + path.string()); }
    auto sz = f.tellg();
    f.seekg(0);
    std::string json_str(static_cast<size_t>(sz), '\0');
    f.read(json_str.data(), sz);

    InstallPlan plan;
    auto parse_result = glz::read_json(plan, json_str);
    if (parse_result) {
        return std::unexpected("install plan is corrupted: " + glz::format_error(parse_result, json_str)
                               + " (run `kiln build` to regenerate)");
    }

    if (plan.version != INSTALL_PLAN_VERSION) {
        return std::unexpected("install plan version " + std::to_string(plan.version) + ", kiln expects "
                               + std::to_string(INSTALL_PLAN_VERSION) + " (run `kiln build` to regenerate)");
    }
    return plan;
}

} // namespace kiln
