#include "kiln/tool_mode.hpp"
#include "kiln/parse_number.hpp"
#include "kiln/build_system.hpp"
#include <signal.h>
#include <cerrno>

#include <CLI/CLI.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <archive.h>
#include <archive_entry.h>

namespace fs = std::filesystem;

static char** get_environ_ptr() {
    extern char** environ;
    return environ;
}

namespace kiln {

namespace {

using Args = std::span<const std::string>;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

int exec_command(const std::vector<std::string>& cmd, const std::string& working_dir = "") {
    if (cmd.empty()) return 1;

    pid_t pid = fork();
    if (pid < 0) {
        std::cerr << "Error: fork failed: " << strerror(errno) << std::endl;
        return 1;
    }
    if (pid == 0) {
        setpgid(0, 0);
        kiln::set_parent_death_signal(SIGTERM);
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGHUP, SIG_DFL);
        if (!working_dir.empty()) {
            if (chdir(working_dir.c_str()) != 0) { _exit(127); }
        }
        std::vector<char*> argv;
        for (auto& s : cmd) argv.push_back(const_cast<char*>(s.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    setpgid(pid, pid);
    register_child_pid(pid);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    unregister_child_pid(pid);
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 1;
}

bool copy_file_impl(const fs::path& src, const fs::path& dst) {
    std::error_code ec;
    auto actual_dst = dst;
    if (fs::is_directory(dst)) { actual_dst = dst / src.filename(); }
    fs::copy_file(src, actual_dst, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        std::cerr << "Error: Failed to copy " << src << " to " << actual_dst << ": " << ec.message() << std::endl;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Command implementations
// ---------------------------------------------------------------------------

int cmd_echo(Args args, bool newline) {
    for (size_t i = 0; i < args.size(); ++i) {
        std::cout << args[i];
        if (i + 1 < args.size()) std::cout << ' ';
    }
    if (newline) std::cout << '\n';
    return 0;
}

int cmd_touch(Args args, bool nocreate) {
    for (auto& a : args) {
        if (nocreate && !fs::exists(a)) continue;
        std::ofstream f(a, std::ios::app);
    }
    return 0;
}

int cmd_remove_paths(Args args, bool force) {
    int rc = 0;
    for (auto& a : args) {
        std::error_code ec;
        bool existed = fs::exists(a, ec);
        fs::remove_all(a, ec);
        if (!force && !existed) rc = 1;
    }
    return rc;
}

int cmd_remove_directory(Args args) {
    for (auto& a : args) {
        std::error_code ec;
        fs::remove_all(a, ec);
        if (ec) { std::cerr << "Error: Failed to remove directory " << a << ": " << ec.message() << std::endl; }
    }
    return 0;
}

int cmd_make_directory(Args args) {
    for (auto& a : args) {
        std::error_code ec;
        fs::create_directories(a, ec);
        if (ec) {
            std::cerr << "Error: Failed to create directory " << a << ": " << ec.message() << std::endl;
            return 1;
        }
    }
    return 0;
}

int cmd_create_symlink(const std::string& src, const std::string& dst) {
    if (fs::exists(dst) && !fs::is_symlink(dst)) {
        std::cerr << "Error: Link destination " << dst << " already exists and is not a symlink" << std::endl;
        return 1;
    }
    if (fs::is_symlink(dst)) return 0;
    std::error_code ec;
    fs::create_symlink(src, dst, ec);
    if (ec) {
        std::cerr << "Error: Failed to create symlink: " << ec.message() << std::endl;
        return 1;
    }
    return 0;
}

int cmd_create_hardlink(const std::string& src, const std::string& dst) {
    std::error_code ec;
    fs::create_hard_link(src, dst, ec);
    if (ec) {
        std::cerr << "Error: Failed to create hard link: " << ec.message() << std::endl;
        return 1;
    }
    return 0;
}

int cmd_rename(const std::string& oldname, const std::string& newname) {
    std::error_code ec;
    fs::rename(oldname, newname, ec);
    if (ec) {
        std::cerr << "Error: Failed to rename " << oldname << " to " << newname << ": " << ec.message() << std::endl;
        return 1;
    }
    return 0;
}

// Shared "copy <src>... <dest>" body for files. `args` has the dest as its
// final element.
int cmd_copy(Args args) {
    if (args.size() < 2) {
        std::cerr << "Error: copy requires at least one source and a destination" << std::endl;
        return 1;
    }
    auto dest = args.back();
    if (args.size() > 2 && !fs::is_directory(dest)) {
        std::cerr << "Error: Destination " << dest << " is not a directory" << std::endl;
        return 1;
    }
    for (size_t i = 0; i + 1 < args.size(); ++i) {
        if (!copy_file_impl(args[i], dest)) return 1;
    }
    return 0;
}

int cmd_copy_if_different(Args args) {
    if (args.size() < 2) {
        std::cerr << "Error: copy_if_different requires at least one source and a destination" << std::endl;
        return 1;
    }
    auto dest = args.back();
    if (args.size() > 2 && !fs::is_directory(dest)) {
        std::cerr << "Error: Destination " << dest << " is not a directory" << std::endl;
        return 1;
    }
    for (size_t i = 0; i + 1 < args.size(); ++i) {
        fs::path src_path = args[i];
        fs::path dst_path = fs::is_directory(dest) ? fs::path(dest) / src_path.filename() : fs::path(dest);
        if (fs::exists(dst_path) && fs::file_size(src_path) == fs::file_size(dst_path)) {
            std::ifstream f1(src_path, std::ios::binary);
            std::ifstream f2(dst_path, std::ios::binary);
            if (std::equal(std::istreambuf_iterator<char>(f1), std::istreambuf_iterator<char>(), std::istreambuf_iterator<char>(f2),
                           std::istreambuf_iterator<char>())) {
                continue;
            }
        }
        if (!copy_file_impl(src_path, dest)) return 1;
    }
    return 0;
}

int cmd_copy_if_newer(Args args) {
    if (args.size() < 2) {
        std::cerr << "Error: copy_if_newer requires at least one source and a destination" << std::endl;
        return 1;
    }
    auto dest = args.back();
    if (args.size() > 2 && !fs::is_directory(dest)) {
        std::cerr << "Error: Destination " << dest << " is not a directory" << std::endl;
        return 1;
    }
    for (size_t i = 0; i + 1 < args.size(); ++i) {
        fs::path src_path = args[i];
        fs::path dst_path = fs::is_directory(dest) ? fs::path(dest) / src_path.filename() : fs::path(dest);
        if (fs::exists(dst_path)) {
            auto src_time = fs::last_write_time(src_path);
            auto dst_time = fs::last_write_time(dst_path);
            if (src_time <= dst_time) continue;
        }
        if (!copy_file_impl(src_path, dest)) return 1;
    }
    return 0;
}

int cmd_copy_directory(Args args) {
    if (args.size() < 2) {
        std::cerr << "Error: copy_directory requires at least one source directory and a destination" << std::endl;
        return 1;
    }
    auto dest = fs::path(args.back());
    std::error_code ec;
    fs::create_directories(dest, ec);
    for (size_t i = 0; i + 1 < args.size(); ++i) {
        fs::copy(args[i], dest, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        if (ec) {
            std::cerr << "Error: Failed to copy directory " << args[i] << " to " << dest << ": " << ec.message() << std::endl;
            return 1;
        }
    }
    return 0;
}

int cmd_cat(Args args) {
    if (args.empty()) {
        std::cerr << "Error: cat requires at least one file" << std::endl;
        return 1;
    }
    for (auto& a : args) {
        std::ifstream f(a, std::ios::binary);
        if (!f) {
            std::cerr << "Error: Cannot open " << a << std::endl;
            return 1;
        }
        std::cout << f.rdbuf();
    }
    return 0;
}

int cmd_compare_files(bool ignore_eol, const std::string& f1_path, const std::string& f2_path) {
    std::ifstream f1(f1_path, std::ios::binary);
    std::ifstream f2(f2_path, std::ios::binary);
    if (!f1) {
        std::cerr << "Error: Cannot open " << f1_path << std::endl;
        return 1;
    }
    if (!f2) {
        std::cerr << "Error: Cannot open " << f2_path << std::endl;
        return 1;
    }

    if (ignore_eol) {
        std::string line1, line2;
        while (true) {
            bool got1 = bool(std::getline(f1, line1));
            bool got2 = bool(std::getline(f2, line2));
            if (!got1 && !got2) return 0;
            if (got1 != got2) return 1;
            if (!line1.empty() && line1.back() == '\r') line1.pop_back();
            if (!line2.empty() && line2.back() == '\r') line2.pop_back();
            if (line1 != line2) return 1;
        }
    }

    if (std::equal(std::istreambuf_iterator<char>(f1), std::istreambuf_iterator<char>(), std::istreambuf_iterator<char>(f2),
                   std::istreambuf_iterator<char>())) {
        return 0;
    }
    return 1;
}

int cmd_chdir(const std::string& dir, Args cmd) {
    if (cmd.empty()) {
        std::cerr << "Error: chdir requires a command to run" << std::endl;
        return 1;
    }
    return exec_command(std::vector<std::string>(cmd.begin(), cmd.end()), dir);
}

// `rest` is the prefix_command tail: optional KEY=VALUE pairs, an optional
// `--`, then the command and its args.
int cmd_env(const std::vector<std::string>& unsets, Args rest) {
    std::vector<std::pair<std::string, std::string>> sets;
    size_t i = 0;
    for (; i < rest.size(); ++i) {
        if (rest[i] == "--") {
            ++i;
            break;
        }
        auto eq = rest[i].find('=');
        if (eq != std::string::npos && eq > 0) {
            sets.emplace_back(rest[i].substr(0, eq), rest[i].substr(eq + 1));
            continue;
        }
        break;
    }

    if (i >= rest.size()) {
        std::cerr << "Error: env requires a command to execute" << std::endl;
        return 1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        std::cerr << "Error: fork failed: " << strerror(errno) << std::endl;
        return 1;
    }
    if (pid == 0) {
        setpgid(0, 0);
        kiln::set_parent_death_signal(SIGTERM);
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGHUP, SIG_DFL);
        for (auto& name : unsets) unsetenv(name.c_str());
        for (auto& [name, val] : sets) setenv(name.c_str(), val.c_str(), 1);
        std::vector<char*> argv;
        for (size_t j = i; j < rest.size(); ++j) argv.push_back(const_cast<char*>(rest[j].c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    setpgid(pid, pid);
    register_child_pid(pid);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    unregister_child_pid(pid);
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 1;
}

int cmd_environment() {
    for (char** env = get_environ_ptr(); *env; ++env) { std::cout << *env << '\n'; }
    return 0;
}

int cmd_sleep(Args args) {
    double total = 0;
    for (auto& a : args) {
        auto v = kiln::parse_double(a);
        if (!v) {
            std::cerr << "Error: Invalid sleep duration: " << a << std::endl;
            return 1;
        }
        total += *v;
    }
    if (total > 0) { std::this_thread::sleep_for(std::chrono::duration<double>(total)); }
    return 0;
}

int cmd_time(Args cmd) {
    if (cmd.empty()) {
        std::cerr << "Error: time requires a command" << std::endl;
        return 1;
    }
    auto start = std::chrono::high_resolution_clock::now();
    int rc = exec_command(std::vector<std::string>(cmd.begin(), cmd.end()));
    auto end = std::chrono::high_resolution_clock::now();
    double elapsed = std::chrono::duration<double>(end - start).count();
    std::cout << "Elapsed time: " << elapsed << " s" << std::endl;
    return rc;
}

int cmd_rm(bool recursive, bool force, Args paths) {
    if (paths.empty()) {
        if (force) return 0;
        std::cerr << "Error: rm requires at least one path" << std::endl;
        return 1;
    }
    for (auto& p : paths) {
        std::error_code ec;
        if (!fs::exists(p, ec)) {
            if (!force) {
                std::cerr << "Error: " << p << " does not exist" << std::endl;
                return 1;
            }
            continue;
        }
        if (fs::is_directory(p, ec) && !recursive) {
            std::cerr << "Error: " << p << " is a directory (use -r to remove)" << std::endl;
            return 1;
        }
        if (recursive) {
            fs::remove_all(p, ec);
        } else {
            fs::remove(p, ec);
        }
        if (ec && !force) {
            std::cerr << "Error: Failed to remove " << p << ": " << ec.message() << std::endl;
            return 1;
        }
    }
    return 0;
}

int cmd_tar(const std::string& flags, const std::string& archive_path, Args files) {
    bool create = flags.find('c') != std::string::npos;
    bool extract = flags.find('x') != std::string::npos;
    bool list = flags.find('t') != std::string::npos;
    bool verbose = flags.find('v') != std::string::npos;
    bool gzip = flags.find('z') != std::string::npos;
    bool bzip2 = flags.find('j') != std::string::npos;
    bool xz = flags.find('J') != std::string::npos;

    int mode_count = (int) create + (int) extract + (int) list;
    if (mode_count != 1) {
        std::cerr << "Error: tar requires exactly one of c, x, or t in the flag string" << std::endl;
        return 1;
    }

    if (extract || list) {
        struct archive* a = archive_read_new();
        archive_read_support_format_all(a);
        archive_read_support_filter_all(a);

        if (archive_read_open_filename(a, archive_path.c_str(), 16384) != ARCHIVE_OK) {
            std::cerr << "Error: could not open archive: " << archive_path << ": " << archive_error_string(a) << std::endl;
            archive_read_free(a);
            return 1;
        }

        struct archive* ext = nullptr;
        if (extract) {
            ext = archive_write_disk_new();
            archive_write_disk_set_options(ext, ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM | ARCHIVE_EXTRACT_ACL | ARCHIVE_EXTRACT_FFLAGS);
            archive_write_disk_set_standard_lookup(ext);
        }

        struct archive_entry* entry;
        while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
            if (verbose || list) { std::cout << archive_entry_pathname(entry) << std::endl; }
            if (extract) {
                int r = archive_write_header(ext, entry);
                if (r != ARCHIVE_OK) {
                    std::cerr << "Error: " << archive_error_string(ext) << std::endl;
                } else {
                    const void* buff;
                    size_t size;
                    la_int64_t offset;
                    while (archive_read_data_block(a, &buff, &size, &offset) == ARCHIVE_OK) {
                        archive_write_data_block(ext, buff, size, offset);
                    }
                }
                archive_write_finish_entry(ext);
            } else {
                archive_read_data_skip(a);
            }
        }

        if (ext) {
            archive_write_close(ext);
            archive_write_free(ext);
        }
        archive_read_close(a);
        archive_read_free(a);
        return 0;
    }

    // create mode
    struct archive* a = archive_write_new();
    if (gzip)
        archive_write_add_filter_gzip(a);
    else if (bzip2)
        archive_write_add_filter_bzip2(a);
    else if (xz)
        archive_write_add_filter_xz(a);
    else
        archive_write_add_filter_none(a);
    archive_write_set_format_pax_restricted(a);

    if (archive_write_open_filename(a, archive_path.c_str()) != ARCHIVE_OK) {
        std::cerr << "Error: could not create archive: " << archive_path << ": " << archive_error_string(a) << std::endl;
        archive_write_free(a);
        return 1;
    }

    struct archive* disk = archive_read_disk_new();
    archive_read_disk_set_standard_lookup(disk);

    for (auto& f : files) {
        archive_read_disk_open(disk, f.c_str());
        struct archive_entry* entry;
        while (archive_read_next_header2(disk, entry = archive_entry_new()) == ARCHIVE_OK) {
            archive_read_disk_descend(disk);
            if (verbose) { std::cout << archive_entry_pathname(entry) << std::endl; }
            archive_write_header(a, entry);
            int fd = open(archive_entry_sourcepath(entry), O_RDONLY);
            if (fd >= 0) {
                char buff[16384];
                ssize_t len;
                while ((len = read(fd, buff, sizeof(buff))) > 0) { archive_write_data(a, buff, len); }
                close(fd);
            }
            archive_entry_free(entry);
        }
        archive_read_close(disk);
    }
    archive_read_free(disk);
    archive_write_close(a);
    archive_write_free(a);
    return 0;
}

// ---------------------------------------------------------------------------
// Subcommand registration
// ---------------------------------------------------------------------------

// Per-subcommand option storage. CLI11 stores raw pointers into these
// fields, so they need a lifetime that outlives parsing. One instance per
// process is fine — `kiln tool` runs once and exits.
struct ToolOpts {
    std::vector<std::string> echo_args;
    std::vector<std::string> echo_append_args;
    std::vector<std::string> touch_files;
    std::vector<std::string> touch_nocreate_files;
    std::vector<std::string> remove_files;
    bool remove_force = false;
    std::vector<std::string> remove_directory_dirs;
    std::vector<std::string> make_directory_dirs;
    std::string symlink_old, symlink_new;
    std::string hardlink_old, hardlink_new;
    std::string rename_old, rename_new;
    std::vector<std::string> copy_args;
    std::vector<std::string> copy_if_different_args;
    std::vector<std::string> copy_if_newer_args;
    std::vector<std::string> copy_directory_args;
    std::vector<std::string> copy_directory_if_different_args;
    std::vector<std::string> copy_directory_if_newer_args;
    std::vector<std::string> cat_files;
    bool compare_ignore_eol = false;
    std::string compare_f1, compare_f2;
    std::vector<std::string> env_unsets;
    std::vector<std::string> sleep_durations;
    bool rm_recursive = false;
    bool rm_force = false;
    std::vector<std::string> rm_paths;
    std::string tar_flags;
    std::string tar_archive;
    std::vector<std::string> tar_files;
};

} // namespace

void register_tool_subcommands(CLI::App* tool, int& rc) {
    static ToolOpts o;

    auto add = [&](const char* name, const char* desc) { return tool->add_subcommand(name, desc); };

    CLI::App* c = nullptr;

    c = add("echo", "Display arguments as text, separated by spaces, with a trailing newline");
    c->add_option("strings", o.echo_args, "Strings to print");
    c->callback([&] { rc = cmd_echo(o.echo_args, /*newline=*/true); });

    c = add("echo_append", "Display arguments as text without a trailing newline");
    c->add_option("strings", o.echo_append_args, "Strings to print");
    c->callback([&] { rc = cmd_echo(o.echo_append_args, /*newline=*/false); });

    c = add("touch", "Create files if missing and update their modification time");
    c->add_option("files", o.touch_files, "Files to touch")->required();
    c->callback([&] { rc = cmd_touch(o.touch_files, /*nocreate=*/false); });

    c = add("touch_nocreate", "Update modification time of existing files; do not create");
    c->add_option("files", o.touch_nocreate_files, "Files to touch if they exist")->required();
    c->callback([&] { rc = cmd_touch(o.touch_nocreate_files, /*nocreate=*/true); });

    c = add("remove", "Remove the given files (deprecated: use rm)");
    c->add_flag("-f", o.remove_force, "Continue and exit 0 even if files do not exist");
    c->add_option("files", o.remove_files, "Files to remove")->required();
    c->callback([&] { rc = cmd_remove_paths(o.remove_files, o.remove_force); });

    c = add("remove_directory", "Remove directories and their contents (deprecated: use rm -r)");
    c->add_option("dirs", o.remove_directory_dirs, "Directories to remove")->required();
    c->callback([&] { rc = cmd_remove_directory(o.remove_directory_dirs); });

    c = add("make_directory", "Create directories, including parents as needed");
    c->add_option("dirs", o.make_directory_dirs, "Directories to create")->required();
    c->callback([&] { rc = cmd_make_directory(o.make_directory_dirs); });

    c = add("create_symlink", "Create a symbolic link new -> old");
    c->add_option("old", o.symlink_old, "Existing target path the link points at")->required();
    c->add_option("new", o.symlink_new, "Path of the new symlink to create")->required();
    c->callback([&] { rc = cmd_create_symlink(o.symlink_old, o.symlink_new); });

    c = add("create_hardlink", "Create a hard link new -> old");
    c->add_option("old", o.hardlink_old, "Existing file to link to")->required();
    c->add_option("new", o.hardlink_new, "Path of the new hard link to create")->required();
    c->callback([&] { rc = cmd_create_hardlink(o.hardlink_old, o.hardlink_new); });

    c = add("rename", "Rename a file or directory on a single volume");
    c->add_option("oldname", o.rename_old, "Existing path")->required();
    c->add_option("newname", o.rename_new, "New path")->required();
    c->callback([&] { rc = cmd_rename(o.rename_old, o.rename_new); });

    // copy family. Each takes a single variadic positional list where the
    // last element is the destination — same convention CMake uses. Accepting
    // it as one vector keeps the parser simple; the cmd_* body splits it.
    auto add_copy = [&](const char* name, const char* desc, std::vector<std::string>& store, int (*fn)(Args)) {
        c = add(name, desc);
        c->add_option("paths", store, "<source>... <destination>")->required();
        c->callback([&store, fn, &rc] { rc = fn(store); });
    };
    add_copy("copy", "Copy files to a destination (file or directory)", o.copy_args, cmd_copy);
    add_copy("copy_if_different", "Copy files only if their contents differ from destination", o.copy_if_different_args,
             cmd_copy_if_different);
    add_copy("copy_if_newer", "Copy files only if the source is newer than destination", o.copy_if_newer_args, cmd_copy_if_newer);
    add_copy("copy_directory", "Recursively copy directory contents into destination", o.copy_directory_args, cmd_copy_directory);
    add_copy("copy_directory_if_different", "Recursively copy directory contents (changed files only)", o.copy_directory_if_different_args,
             cmd_copy_directory);
    add_copy("copy_directory_if_newer", "Recursively copy directory contents (newer files only)", o.copy_directory_if_newer_args,
             cmd_copy_directory);

    c = add("cat", "Concatenate files and print to standard output");
    c->add_option("files", o.cat_files, "Files to concatenate")->required();
    c->callback([&] { rc = cmd_cat(o.cat_files); });

    c = add("compare_files", "Check whether two files have identical contents (exit 0 if equal)");
    c->add_flag("--ignore-eol", o.compare_ignore_eol, "Compare line-by-line, ignoring line-ending differences");
    c->add_option("file1", o.compare_f1, "First file")->required();
    c->add_option("file2", o.compare_f2, "Second file")->required();
    c->callback([&] { rc = cmd_compare_files(o.compare_ignore_eol, o.compare_f1, o.compare_f2); });

    // chdir / env / time use prefix_command so the trailing argv is forwarded
    // verbatim to exec. Each captures its own subcommand pointer by value so
    // later reassignment of `c` doesn't disturb the lambda.
    c = add("chdir", "Run a command in a given working directory");
    c->prefix_command();
    c->callback([&, sub = c] {
        auto rest = sub->remaining();
        if (rest.empty()) {
            std::cerr << "Error: chdir requires a directory and command" << std::endl;
            rc = 1;
            return;
        }
        std::string dir = rest.front();
        std::vector<std::string> cmd(rest.begin() + 1, rest.end());
        rc = cmd_chdir(dir, cmd);
    });

    // env — `--unset=NAME` is parsed by CLI11; KEY=VALUE pairs and the
    // command live in the prefix_command tail.
    c = add("env", "Run a command with a modified environment");
    c->prefix_command();
    c->add_option("--unset", o.env_unsets, "Environment variable to unset (repeatable)");
    c->callback([&, sub = c] {
        auto rest = sub->remaining();
        rc = cmd_env(o.env_unsets, rest);
    });

    c = add("environment", "Print the current environment, one variable per line");
    c->callback([&] { rc = cmd_environment(); });

    c = add("sleep", "Sleep for the given number of seconds (sum of all arguments)");
    c->add_option("seconds", o.sleep_durations, "Durations to sleep, summed")->required();
    c->callback([&] { rc = cmd_sleep(o.sleep_durations); });

    c = add("time", "Run a command and report elapsed wall-clock time");
    c->prefix_command();
    c->callback([&, sub = c] {
        auto rest = sub->remaining();
        rc = cmd_time(rest);
    });

    c = add("rm", "Remove files or directories");
    c->add_flag("-r,-R", o.rm_recursive, "Recurse into directories");
    c->add_flag("-f", o.rm_force, "Ignore missing files; never error");
    c->add_option("paths", o.rm_paths, "Files or directories to remove");
    c->callback([&] { rc = cmd_rm(o.rm_recursive, o.rm_force, o.rm_paths); });

    // tar — the [cxt][vf][zjJ] flag glob isn't a normal option set, so it
    // stays a positional that we parse character-by-character ourselves.
    c = add("tar", "Create or extract a tar/zip archive");
    c->add_option("flags", o.tar_flags, "Mode flags: c=create, x=extract, t=list; v=verbose; z=gzip, j=bzip2, J=xz")->required();
    c->add_option("archive", o.tar_archive, "Archive file path")->required();
    c->add_option("files", o.tar_files, "Files or directories (for create mode)");
    c->callback([&] { rc = cmd_tar(o.tar_flags, o.tar_archive, o.tar_files); });

    c = add("capabilities", "Report cmake-compatible capabilities as JSON");
    c->callback([&] {
        std::cout << "{}" << std::endl;
        rc = 0;
    });

    c = add("true", "Do nothing and exit with status 0");
    c->callback([&] { rc = 0; });

    c = add("false", "Do nothing and exit with status 1");
    c->callback([&] { rc = 1; });
}

} // namespace kiln
