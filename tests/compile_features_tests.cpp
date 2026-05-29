#include <catch2/catch_test_macros.hpp>
#include "../kiln/compile_features.hpp"

using namespace kiln;

TEST_CASE("CompileFeatures - C++ meta-features", "[compile_features]") {
    const auto& features = CompileFeatures::instance();

    SECTION("C++ standard meta-features are known") {
        CHECK(features.is_known_feature("cxx_std_98"));
        CHECK(features.is_known_feature("cxx_std_11"));
        CHECK(features.is_known_feature("cxx_std_14"));
        CHECK(features.is_known_feature("cxx_std_17"));
        CHECK(features.is_known_feature("cxx_std_20"));
        CHECK(features.is_known_feature("cxx_std_23"));
        CHECK(features.is_known_feature("cxx_std_26"));
    }

    SECTION("Unknown features are not recognized") {
        CHECK_FALSE(features.is_known_feature("cxx_std_99"));
        CHECK_FALSE(features.is_known_feature("invalid_feature"));
        CHECK_FALSE(features.is_known_feature(""));
    }

    SECTION("C++ individual features from C++11") {
        CHECK(features.is_known_feature("cxx_lambdas"));
        CHECK(features.is_known_feature("cxx_variadic_templates"));
        CHECK(features.is_known_feature("cxx_nullptr"));
        CHECK(features.is_known_feature("cxx_auto_type"));
        CHECK(features.is_known_feature("cxx_static_assert"));
    }

    SECTION("C++ individual features from C++14") {
        CHECK(features.is_known_feature("cxx_generic_lambdas"));
        CHECK(features.is_known_feature("cxx_lambda_init_captures"));
        CHECK(features.is_known_feature("cxx_decltype_auto"));
        CHECK(features.is_known_feature("cxx_return_type_deduction"));
    }
}

TEST_CASE("CompileFeatures - C features", "[compile_features]") {
    const auto& features = CompileFeatures::instance();

    SECTION("C standard meta-features are known") {
        CHECK(features.is_known_feature("c_std_90"));
        CHECK(features.is_known_feature("c_std_99"));
        CHECK(features.is_known_feature("c_std_11"));
        CHECK(features.is_known_feature("c_std_17"));
        CHECK(features.is_known_feature("c_std_23"));
    }

    SECTION("C individual features") {
        CHECK(features.is_known_feature("c_function_prototypes"));
        CHECK(features.is_known_feature("c_restrict"));
        CHECK(features.is_known_feature("c_variadic_macros"));
        CHECK(features.is_known_feature("c_static_assert"));
    }
}

TEST_CASE("CompileFeatures - get_feature_info", "[compile_features]") {
    const auto& features = CompileFeatures::instance();

    SECTION("Meta-feature info is correct") {
        auto* cxx17 = features.get_feature_info("cxx_std_17");
        REQUIRE(cxx17 != nullptr);
        CHECK(cxx17->name == "cxx_std_17");
        CHECK(cxx17->language == Language::CXX);
        CHECK(cxx17->required_standard == 17);
        CHECK(cxx17->is_meta_feature == true);
    }

    SECTION("Individual feature info is correct") {
        auto* lambdas = features.get_feature_info("cxx_lambdas");
        REQUIRE(lambdas != nullptr);
        CHECK(lambdas->name == "cxx_lambdas");
        CHECK(lambdas->language == Language::CXX);
        CHECK(lambdas->required_standard == 11);
        CHECK(lambdas->is_meta_feature == false);
    }

    SECTION("C feature info is correct") {
        auto* c99 = features.get_feature_info("c_std_99");
        REQUIRE(c99 != nullptr);
        CHECK(c99->name == "c_std_99");
        CHECK(c99->language == Language::C);
        CHECK(c99->required_standard == 99);
        CHECK(c99->is_meta_feature == true);
    }

    SECTION("Unknown feature returns nullptr") {
        auto* unknown = features.get_feature_info("nonexistent");
        CHECK(unknown == nullptr);
    }
}

TEST_CASE("CompileFeatures - get_required_standard", "[compile_features]") {
    const auto& features = CompileFeatures::instance();

    SECTION("Single meta-feature") {
        std::vector<std::string> feats = {"cxx_std_17"};
        CHECK(features.get_required_standard(feats, Language::CXX) == 17);
    }

    SECTION("Multiple features - picks highest") {
        std::vector<std::string> feats = {"cxx_std_11", "cxx_std_20", "cxx_std_14"};
        CHECK(features.get_required_standard(feats, Language::CXX) == 20);
    }

    SECTION("Individual features require their standard") {
        std::vector<std::string> feats = {"cxx_lambdas"};
        CHECK(features.get_required_standard(feats, Language::CXX) == 11);

        std::vector<std::string> feats14 = {"cxx_generic_lambdas"};
        CHECK(features.get_required_standard(feats14, Language::CXX) == 14);
    }

    SECTION("Mixed meta and individual features") {
        std::vector<std::string> feats = {"cxx_lambdas", "cxx_std_17", "cxx_generic_lambdas"};
        CHECK(features.get_required_standard(feats, Language::CXX) == 17);
    }

    SECTION("Empty list returns 0") {
        std::vector<std::string> feats;
        CHECK(features.get_required_standard(feats, Language::CXX) == 0);
    }

    SECTION("Only filters by language") {
        std::vector<std::string> feats = {"cxx_std_17", "c_std_99"};
        CHECK(features.get_required_standard(feats, Language::CXX) == 17);
        CHECK(features.get_required_standard(feats, Language::C) == 99);
    }

    SECTION("C features") {
        // c_variadic_macros requires C99, c_std_11 requires C11
        // C11 is newer than C99, so should pick 11
        std::vector<std::string> feats = {"c_std_11", "c_variadic_macros"};
        CHECK(features.get_required_standard(feats, Language::C) == 11);

        // c_std_17 is newer than c_restrict (C99)
        std::vector<std::string> feats2 = {"c_std_17", "c_restrict"};
        CHECK(features.get_required_standard(feats2, Language::C) == 17);
    }
}

TEST_CASE("CompileFeatures - comprehensive feature list", "[compile_features]") {
    const auto& features = CompileFeatures::instance();

    SECTION("All C++11 features are present") {
        // Just check a representative sample
        std::vector<std::string> cpp11_features = {"cxx_alias_templates",   "cxx_alignas",   "cxx_alignof",  "cxx_attributes",
                                                   "cxx_auto_type",         "cxx_constexpr", "cxx_decltype", "cxx_defaulted_functions",
                                                   "cxx_deleted_functions", "cxx_final",     "cxx_override", "cxx_nullptr",
                                                   "cxx_rvalue_references"};

        for (const auto& feat : cpp11_features) {
            CHECK(features.is_known_feature(feat));
            auto* info = features.get_feature_info(feat);
            REQUIRE(info != nullptr);
            CHECK(info->required_standard == 11);
        }
    }

    SECTION("All C++14 features are present") {
        std::vector<std::string> cpp14_features = {"cxx_aggregate_default_initializers",
                                                   "cxx_attribute_deprecated",
                                                   "cxx_binary_literals",
                                                   "cxx_decltype_auto",
                                                   "cxx_generic_lambdas",
                                                   "cxx_lambda_init_captures",
                                                   "cxx_relaxed_constexpr",
                                                   "cxx_return_type_deduction",
                                                   "cxx_variable_templates"};

        for (const auto& feat : cpp14_features) {
            CHECK(features.is_known_feature(feat));
            auto* info = features.get_feature_info(feat);
            REQUIRE(info != nullptr);
            CHECK(info->required_standard == 14);
        }
    }
}
