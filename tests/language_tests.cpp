#include <catch2/catch_test_macros.hpp>
#include "kiln/language.hpp"

using namespace kiln;

TEST_CASE("LanguageClassifier: CXX detection", "[language]") {
    CHECK(LanguageClassifier::from_extension(".cpp").lang == Language::CXX);
    CHECK(LanguageClassifier::from_extension(".cc").lang == Language::CXX);
    CHECK(LanguageClassifier::from_extension(".cxx").lang == Language::CXX);
    CHECK(LanguageClassifier::from_extension(".C").lang == Language::CXX);

    CHECK(LanguageClassifier::from_path("main.cpp").lang == Language::CXX);
    CHECK(LanguageClassifier::from_path("src/lib.C").lang == Language::CXX);
}

TEST_CASE("LanguageClassifier: C detection", "[language]") {
    CHECK(LanguageClassifier::from_extension(".c").lang == Language::C);
    CHECK(LanguageClassifier::from_path("main.c").lang == Language::C);
}

TEST_CASE("LanguageClassifier: Header detection", "[language]") {
    auto info = LanguageClassifier::from_extension(".h");
    CHECK(info.lang == Language::HEADER);
    CHECK(info.is_header == true);
    CHECK(info.is_compileable == false);

    CHECK(LanguageClassifier::from_extension(".hpp").lang == Language::HEADER);
    CHECK(LanguageClassifier::from_extension(".hxx").lang == Language::HEADER);
    CHECK(LanguageClassifier::from_extension(".hh").lang == Language::HEADER);
}

TEST_CASE("LanguageClassifier: Unknown detection", "[language]") {
    auto info = LanguageClassifier::from_extension(".java");
    CHECK(info.lang == Language::UNKNOWN);
    CHECK(info.is_compileable == false);
    CHECK(info.is_header == false);
}

TEST_CASE("LanguageClassifier: CUDA detection", "[language][cuda]") {
    auto info = LanguageClassifier::from_extension(".cu");
    CHECK(info.lang == Language::CUDA);
    CHECK(info.name == "CUDA");
    CHECK(info.is_compileable == true);
    CHECK(info.is_header == false);
    CHECK(info.is_module_interface == false);
    CHECK(LanguageClassifier::from_path("kernel.cu").lang == Language::CUDA);
    CHECK(language_name(Language::CUDA) == "CUDA");
    CHECK(language_has_compiler(Language::CUDA) == true);
}

TEST_CASE("LanguageClassifier: CUDA does not interfere with other languages", "[language][cuda]") {
    CHECK(LanguageClassifier::from_extension(".c").lang == Language::C);
    CHECK(LanguageClassifier::from_extension(".cpp").lang == Language::CXX);
    CHECK(LanguageClassifier::from_extension(".cu").lang == Language::CUDA);
}
