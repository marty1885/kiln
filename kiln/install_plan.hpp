#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <sys/types.h>
#include <vector>

namespace kiln {

// Schema version. Bump on any breaking change. kiln install hard-rejects
// mismatched plans and tells the user to re-run `kiln build`.
inline constexpr uint32_t INSTALL_PLAN_VERSION = 1;

// One install operation. A single struct (rather than a variant) because glaze
// reflects it trivially and the JSON is human-inspectable.
//
// `kind` discriminates the operation:
//   "copy_file"      - copy src -> dest, chmod to perms
//   "symlink"        - create symlink at dest pointing to symlink_target
//   "copy_directory" - walk src, copy files matching patterns/excludes
//   "write_content"  - write content (literal string) to dest, chmod to perms
struct InstallOp {
    std::string kind;

    // Common to all ops
    std::string dest;                        // path relative to install prefix
    std::string component;                   // empty == "Unspecified"
    std::vector<std::string> configurations; // empty == all configs
    bool exclude_from_all = false;
    bool optional = false;

    // copy_file, copy_directory
    std::string src; // absolute source path

    // copy_file, write_content
    std::string perms; // rwxr-xr-x style, empty == default

    // write_content
    std::string content;

    // symlink
    std::string symlink_target; // the link target (not relative to prefix)

    // copy_directory
    std::vector<std::string> patterns;
    std::vector<std::string> excludes;
    bool use_source_permissions = false;
    bool preserve_dir_name = false; // false: install contents; true: install dir itself
    std::string file_perms;         // rwx string, empty == default
    std::string dir_perms;          // rwx string, empty == default
};

struct InstallPlan {
    uint32_t version = INSTALL_PLAN_VERSION;
    std::string kiln_version;   // for diagnostics
    std::string config;         // build config the plan was made for
    std::string default_prefix; // CMAKE_INSTALL_PREFIX at build time
    std::vector<InstallOp> ops;
};

// rwx <-> mode_t helpers. rwx is the canonical wire format for permissions.
//   mode_to_rwx(0755) -> "rwxr-xr-x"
//   rwx_to_mode("rwxr-xr-x") -> 0755
// A dash in any position means the bit is cleared (e.g. "rw-r--r--" == 0644).
std::string mode_to_rwx(mode_t mode);
std::expected<mode_t, std::string> rwx_to_mode(const std::string& rwx);

std::expected<void, std::string> save_install_plan(const InstallPlan& plan, const std::filesystem::path& path);
std::expected<InstallPlan, std::string> load_install_plan(const std::filesystem::path& path);

} // namespace kiln
