#include "ast_cache.hpp"
#include <filesystem>
#include <array>
#include <algorithm>

namespace kiln {

bool AstCache::is_cacheable(const std::string& abs_path) {
    // Only cache shared infrastructure modules that get included repeatedly
    // by different find_package() calls. Other system modules (e.g., FindZLIB.cmake)
    // only get included once per build and don't benefit from caching.
    static constexpr std::array cacheable_basenames = {
        "FindPackageHandleStandardArgs.cmake",
        "FindPackageMessage.cmake",
        "FindPkgConfig.cmake",
        "CMakeParseArguments.cmake",
    };

    auto basename = std::filesystem::path(abs_path).filename().string();
    return std::find(cacheable_basenames.begin(), cacheable_basenames.end(), basename) != cacheable_basenames.end();
}

const std::vector<AstNode>* AstCache::get(const std::string& abs_path) const {
    auto it = cache_.find(abs_path);
    if (it != cache_.end()) { return &it->second; }
    return nullptr;
}

void AstCache::put(const std::string& abs_path, std::vector<AstNode> ast) {
    if (is_cacheable(abs_path)) { cache_.emplace(abs_path, std::move(ast)); }
}

} // namespace kiln
