#include "compile_features.hpp"
#include <algorithm>

namespace kiln {

const CompileFeatures& CompileFeatures::instance() {
    static CompileFeatures inst;
    return inst;
}

CompileFeatures::CompileFeatures() {
    register_cxx_features();
    register_c_features();
}

void CompileFeatures::register_cxx_features() {
    // C++ Meta-features (high-level standard support)
    cxx_features_.push_back({"cxx_std_98", Language::CXX, 98, true});
    cxx_features_.push_back({"cxx_std_11", Language::CXX, 11, true});
    cxx_features_.push_back({"cxx_std_14", Language::CXX, 14, true});
    cxx_features_.push_back({"cxx_std_17", Language::CXX, 17, true});
    cxx_features_.push_back({"cxx_std_20", Language::CXX, 20, true});
    cxx_features_.push_back({"cxx_std_23", Language::CXX, 23, true});
    cxx_features_.push_back({"cxx_std_26", Language::CXX, 26, true});

    // C++98 features
    cxx_features_.push_back({"cxx_template_template_parameters", Language::CXX, 98, false});

    // C++11 features
    cxx_features_.push_back({"cxx_alias_templates", Language::CXX, 11, false});
    cxx_features_.push_back({"cxx_alignas", Language::CXX, 11, false});
    cxx_features_.push_back({"cxx_alignof", Language::CXX, 11, false});
    cxx_features_.push_back({"cxx_attributes", Language::CXX, 11, false});
    cxx_features_.push_back({"cxx_auto_type", Language::CXX, 11, false});
    cxx_features_.push_back({"cxx_constexpr", Language::CXX, 11, false});
    cxx_features_.push_back({"cxx_decltype", Language::CXX, 11, false});
    cxx_features_.push_back({"cxx_decltype_incomplete_return_types", Language::CXX, 11, false});
    cxx_features_.push_back({"cxx_default_function_template_args", Language::CXX, 11, false});
    cxx_features_.push_back({"cxx_defaulted_functions", Language::CXX, 11, false});
    cxx_features_.push_back({"cxx_defaulted_move_initializers", Language::CXX, 11, false});
    cxx_features_.push_back({"cxx_delegating_constructors", Language::CXX, 11, false});
    cxx_features_.push_back({"cxx_deleted_functions", Language::CXX, 11, false});
    cxx_features_.push_back({"cxx_enum_forward_declarations", Language::CXX, 11, false});
    cxx_features_.push_back({"cxx_explicit_conversions", Language::CXX, 11, false});
    cxx_features_.push_back({"cxx_extended_friend_declarations", Language::CXX, 11, false});
    cxx_features_.push_back({"cxx_extern_templates", Language::CXX, 11, false});
    cxx_features_.push_back({"cxx_final", Language::CXX, 11, false});
    cxx_features_.push_back({"cxx_func_identifier", Language::CXX, 11, false});
    cxx_features_.push_back({"cxx_generalized_initializers", Language::CXX, 11, false});
    cxx_features_.push_back({"cxx_inheriting_constructors", Language::CXX, 11, false});
    cxx_features_.push_back({"cxx_inline_namespaces", Language::CXX, 11, false});
    cxx_features_.push_back({"cxx_lambdas", Language::CXX, 11, false});
    cxx_features_.push_back({"cxx_local_type_template_args", Language::CXX, 11, false});
    cxx_features_.push_back({"cxx_long_long_type", Language::CXX, 11, false});
    cxx_features_.push_back({"cxx_noexcept", Language::CXX, 11, false});
    cxx_features_.push_back({"cxx_nonstatic_member_init", Language::CXX, 11, false});
    cxx_features_.push_back({"cxx_nullptr", Language::CXX, 11, false});
    cxx_features_.push_back({"cxx_override", Language::CXX, 11, false});
    cxx_features_.push_back({"cxx_range_for", Language::CXX, 11, false});
    cxx_features_.push_back({"cxx_raw_string_literals", Language::CXX, 11, false});
    cxx_features_.push_back({"cxx_reference_qualified_functions", Language::CXX, 11, false});
    cxx_features_.push_back({"cxx_right_angle_brackets", Language::CXX, 11, false});
    cxx_features_.push_back({"cxx_rvalue_references", Language::CXX, 11, false});
    cxx_features_.push_back({"cxx_sizeof_member", Language::CXX, 11, false});
    cxx_features_.push_back({"cxx_static_assert", Language::CXX, 11, false});
    cxx_features_.push_back({"cxx_strong_enums", Language::CXX, 11, false});
    cxx_features_.push_back({"cxx_thread_local", Language::CXX, 11, false});
    cxx_features_.push_back({"cxx_trailing_return_types", Language::CXX, 11, false});
    cxx_features_.push_back({"cxx_unicode_literals", Language::CXX, 11, false});
    cxx_features_.push_back({"cxx_uniform_initialization", Language::CXX, 11, false});
    cxx_features_.push_back({"cxx_unrestricted_unions", Language::CXX, 11, false});
    cxx_features_.push_back({"cxx_user_literals", Language::CXX, 11, false});
    cxx_features_.push_back({"cxx_variadic_macros", Language::CXX, 11, false});
    cxx_features_.push_back({"cxx_variadic_templates", Language::CXX, 11, false});

    // C++14 features
    cxx_features_.push_back({"cxx_aggregate_default_initializers", Language::CXX, 14, false});
    cxx_features_.push_back({"cxx_attribute_deprecated", Language::CXX, 14, false});
    cxx_features_.push_back({"cxx_binary_literals", Language::CXX, 14, false});
    cxx_features_.push_back({"cxx_contextual_conversions", Language::CXX, 14, false});
    cxx_features_.push_back({"cxx_decltype_auto", Language::CXX, 14, false});
    cxx_features_.push_back({"cxx_digit_separators", Language::CXX, 14, false});
    cxx_features_.push_back({"cxx_generic_lambdas", Language::CXX, 14, false});
    cxx_features_.push_back({"cxx_lambda_init_captures", Language::CXX, 14, false});
    cxx_features_.push_back({"cxx_relaxed_constexpr", Language::CXX, 14, false});
    cxx_features_.push_back({"cxx_return_type_deduction", Language::CXX, 14, false});
    cxx_features_.push_back({"cxx_variable_templates", Language::CXX, 14, false});

    // Note: No individual features for C++17, C++20, C++23, C++26
    // Only meta-features (cxx_std_XX) are provided for these standards

    // Build feature map for fast lookup
    for (const auto& feat : cxx_features_) { feature_map_[feat.name] = &feat; }
}

void CompileFeatures::register_c_features() {
    // C Meta-features (high-level standard support)
    c_features_.push_back({"c_std_90", Language::C, 90, true});
    c_features_.push_back({"c_std_99", Language::C, 99, true});
    c_features_.push_back({"c_std_11", Language::C, 11, true});
    c_features_.push_back({"c_std_17", Language::C, 17, true});
    c_features_.push_back({"c_std_23", Language::C, 23, true});

    // Individual C features
    c_features_.push_back({"c_function_prototypes", Language::C, 90, false});
    c_features_.push_back({"c_restrict", Language::C, 99, false});
    c_features_.push_back({"c_variadic_macros", Language::C, 99, false});
    c_features_.push_back({"c_static_assert", Language::C, 11, false});

    // Build feature map for fast lookup
    for (const auto& feat : c_features_) { feature_map_[feat.name] = &feat; }
}

bool CompileFeatures::is_known_feature(std::string_view feature) const {
    return feature_map_.find(std::string(feature)) != feature_map_.end();
}

const CompileFeature* CompileFeatures::get_feature_info(std::string_view feature) const {
    auto it = feature_map_.find(std::string(feature));
    if (it != feature_map_.end()) { return it->second; }
    return nullptr;
}

int CompileFeatures::get_required_standard(const std::vector<std::string>& features, Language lang) const {
    int max_standard = 0;

    // Helper to convert standard version to a comparable value
    // For C: 90, 99, 11, 17, 23 -> need ordering 90 < 99 < 11 < 17 < 23
    // Map to year-based ordering for proper comparison
    auto to_comparable = [lang](int std) -> int {
        if (lang == Language::C) {
            // C standards: map to actual years for proper ordering
            if (std == 90) return 1990;
            if (std == 99) return 1999;
            if (std == 11) return 2011;
            if (std == 17) return 2017;
            if (std == 23) return 2023;
        }
        // For C++, the numbers already order correctly (98 < 11 < 14 < 17 < 20 < 23 < 26)
        // But map to years for consistency
        if (lang == Language::CXX) {
            if (std == 98) return 1998;
            if (std == 11) return 2011;
            if (std == 14) return 2014;
            if (std == 17) return 2017;
            if (std == 20) return 2020;
            if (std == 23) return 2023;
            if (std == 26) return 2026;
        }
        return std;
    };

    int max_comparable = 0;
    int result_std = 0;

    for (const auto& feature : features) {
        auto* info = get_feature_info(feature);
        if (info && info->language == lang) {
            int comparable = to_comparable(info->required_standard);
            if (comparable > max_comparable) {
                max_comparable = comparable;
                result_std = info->required_standard;
            }
        }
    }

    return result_std;
}

} // namespace kiln
