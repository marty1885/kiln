#include "kiln/path.hpp"
#include <unistd.h>

namespace kiln {

// --- Helpers ---

// Parse the root prefix length: "/" for Unix, "X:/" for drive, "//" for UNC.
static size_t root_prefix_len(std::string_view path) noexcept {
    if (path.empty()) return 0;
    // Windows drive: X:/
    if (path.size() >= 3 && ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) && path[1] == ':'
        && path[2] == '/') {
        return 3;
    }
    // UNC: //server (at least //x)
    if (path.size() >= 3 && path[0] == '/' && path[1] == '/') {
        auto pos = path.find('/', 2);
        if (pos == std::string_view::npos) return path.size();
        return pos + 1;
    }
    // Unix root
    if (path[0] == '/') return 1;
    return 0;
}

// --- View-returning methods ---

bool Path::is_absolute() const noexcept {
    if (path_.empty()) return false;
    if (path_[0] == '/') return true;
    if (path_.size() >= 3 && ((path_[0] >= 'A' && path_[0] <= 'Z') || (path_[0] >= 'a' && path_[0] <= 'z')) && path_[1] == ':'
        && path_[2] == '/')
        return true;
    return false;
}

std::string_view Path::filename() const noexcept {
    if (path_.empty()) return {};

    // Match std::filesystem::path::filename() — trailing slash means empty filename
    if (path_.back() == '/') return {};

    size_t pos = path_.rfind('/');
    if (pos == std::string_view::npos) return path_;
    return std::string_view(path_).substr(pos + 1);
}

std::string_view Path::parent_path() const noexcept {
    if (path_.empty()) return {};

    size_t root_len = root_prefix_len(path_);

    // Match std::filesystem::path::parent_path() — trailing slash means
    // empty filename, so parent is the path with trailing slashes stripped.
    if (path_.size() > root_len && path_.back() == '/') {
        size_t end = path_.size();
        while (end > root_len && path_[end - 1] == '/') --end;
        if (end <= root_len) {
            // All slashes after root — parent is root
            return std::string_view(path_).substr(0, root_len);
        }
        return std::string_view(path_).substr(0, end);
    }

    size_t pos = path_.rfind('/');
    if (pos == std::string_view::npos || pos < root_len) {
        if (root_len > 0) return std::string_view(path_).substr(0, root_len);
        return {};
    }
    if (pos == 0) return std::string_view(path_).substr(0, 1);
    return std::string_view(path_).substr(0, pos);
}

std::string_view Path::extension() const noexcept {
    auto fname = filename();
    if (fname.empty() || fname == "." || fname == "..") return {};

    auto dot = fname.rfind('.');
    if (dot == std::string_view::npos || dot == 0) return {};
    return fname.substr(dot);
}

std::string_view Path::stem() const noexcept {
    auto fname = filename();
    if (fname.empty() || fname == "." || fname == "..") return fname;

    auto dot = fname.rfind('.');
    if (dot == std::string_view::npos || dot == 0) return fname;
    return fname.substr(0, dot);
}

bool Path::has_extension() const noexcept {
    return !extension().empty();
}

std::string_view Path::relative_path() const noexcept {
    size_t root_len = root_prefix_len(path_);
    return std::string_view(path_).substr(root_len);
}

// --- Allocating methods ---

Path Path::operator/(std::string_view rhs) const {
    return Path(join(path_, rhs));
}

Path Path::lexically_normal() const {
    if (path_.empty()) return Path(".");

    // Fast path: if the path has no . or .. components and no double slashes
    // or trailing slash, it's already normal — return as-is without allocating.
    {
        bool dominated_by_slash = false; // previous char was '/'
        bool clean = true;
        for (size_t i = 0, n = path_.size(); i < n; ++i) {
            char c = path_[i];
            if (c == '/') {
                if (dominated_by_slash) {
                    clean = false;
                    break;
                } // "//"
                dominated_by_slash = true;
            } else if (c == '.' && dominated_by_slash) {
                // Check for "/." or "/.."
                size_t next = i + 1;
                if (next == n || path_[next] == '/') {
                    clean = false;
                    break;
                } // "/." at end or "/./"
                if (path_[next] == '.' && (next + 1 == n || path_[next + 1] == '/')) { // "/.."
                    clean = false;
                    break;
                }
                dominated_by_slash = false;
            } else {
                dominated_by_slash = false;
            }
        }
        // Trailing slash on non-root paths is not normal
        if (clean && path_.size() > 1 && path_.back() == '/') clean = false;
        if (clean) return *this;
    }

    size_t root_len = root_prefix_len(path_);
    std::string_view root = std::string_view(path_).substr(0, root_len);
    std::string_view rest = std::string_view(path_).substr(root_len);

    struct Component {
        std::string_view sv;
    };
    Component stack[256];
    size_t stack_size = 0;

    size_t pos = 0;
    while (pos < rest.size()) {
        while (pos < rest.size() && rest[pos] == '/') ++pos;
        if (pos >= rest.size()) break;

        size_t end = pos;
        while (end < rest.size() && rest[end] != '/') ++end;

        auto comp = rest.substr(pos, end - pos);
        pos = end;

        if (comp == ".") {
            // Skip
        } else if (comp == "..") {
            if (stack_size > 0 && stack[stack_size - 1].sv != "..") {
                --stack_size;
            } else if (root_len == 0) {
                if (stack_size < 256) stack[stack_size++] = {comp};
            }
        } else {
            if (stack_size < 256) stack[stack_size++] = {comp};
        }
    }

    std::string result;
    result.reserve(path_.size());
    result.append(root);

    for (size_t i = 0; i < stack_size; ++i) {
        if (i > 0 || root_len > 0) {
            if (i > 0 || (root_len > 0 && !root.empty() && root.back() != '/')) result += '/';
        }
        result.append(stack[i].sv);
    }

    if (result.empty()) return Path(".");
    if (result == root && root_len > 0) return Path(std::string(root));

    return Path(std::move(result));
}

Path Path::replace_extension(std::string_view new_ext) const {
    auto ext = extension();
    std::string result;

    if (ext.empty()) {
        // No existing extension — append
        result = path_;
        if (!new_ext.empty() && new_ext[0] != '.') result += '.';
        result.append(new_ext);
    } else {
        // Replace from extension start
        size_t ext_start = ext.data() - path_.data();
        result = path_.substr(0, ext_start);
        if (!new_ext.empty() && new_ext[0] != '.') result += '.';
        result.append(new_ext);
    }

    return Path(std::move(result));
}

// --- Static helpers ---

std::string Path::join(std::string_view base, std::string_view rel) {
    if (rel.empty()) return std::string(base);
    if (base.empty()) return std::string(rel);
    // Check if rel is absolute
    if (!rel.empty()) {
        if (rel[0] == '/') return std::string(rel);
        if (rel.size() >= 3 && ((rel[0] >= 'A' && rel[0] <= 'Z') || (rel[0] >= 'a' && rel[0] <= 'z')) && rel[1] == ':' && rel[2] == '/')
            return std::string(rel);
    }

    std::string result;
    result.reserve(base.size() + 1 + rel.size());
    result.append(base);
    if (result.back() != '/') result += '/';
    result.append(rel);
    return result;
}

std::string Path::make_absolute_and_normal(std::string_view base, std::string_view path) {
    Path p(path);
    if (p.is_absolute()) return p.lexically_normal().str();
    return Path(join(base, path)).lexically_normal().str();
}

std::string Path::absolute(std::string_view path) {
    if (Path(path).is_absolute()) return std::string(path);
    char buf[4096];
    if (!getcwd(buf, sizeof(buf))) return {};
    return join(std::string_view(buf), path);
}

} // namespace kiln
