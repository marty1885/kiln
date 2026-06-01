#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <map>
#include "language.hpp"

namespace kiln {

// Compile feature information
struct CompileFeature {
    std::string name;
    Language language;
    int required_standard; // e.g., 11 for C++11, 99 for C99, 0 for meta-features
    bool is_meta_feature;  // true for cxx_std_XX, c_std_XX
};

class CompileFeatures {
public:
    // Get singleton instance
    static const CompileFeatures& instance();

    // Check if a feature is known
    bool is_known_feature(std::string_view feature) const;

    // Get feature info (returns nullptr if not found)
    const CompileFeature* get_feature_info(std::string_view feature) const;

    // Extract the minimum required standard from a list of features for a given language
    // Returns 0 if no standard requirement found
    int get_required_standard(const std::vector<std::string>& features, Language lang) const;

    // Get all known C++ features
    const std::vector<CompileFeature>& get_cxx_features() const { return cxx_features_; }

    // Get all known C features
    const std::vector<CompileFeature>& get_c_features() const { return c_features_; }

    // Get all known cuda features
    const std::vector<CompileFeature>& get_cuda_features() const { return cuda_features_; }

private:
    CompileFeatures();

    std::vector<CompileFeature> cxx_features_;
    std::vector<CompileFeature> c_features_;
    std::vector<CompileFeature> cuda_features_;
    std::map<std::string, const CompileFeature*> feature_map_;

    void register_cxx_features();
    void register_c_features();
    void register_cuda_features();
};

} // namespace kiln
