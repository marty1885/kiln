#include "language.hpp"
#include <filesystem>
#include <map>

namespace kiln {

LanguageInfo LanguageClassifier::from_extension(std::string_view ext) {
    // C++20 module interface files
    if (ext == ".ixx" || ext == ".cppm" || ext == ".ccm" || ext == ".cxxm" || ext == ".mpp") {
        return {Language::CXX, "CXX", true, false, true};
    }
    if (ext == ".cpp" || ext == ".cc" || ext == ".cxx" || ext == ".C" || ext == ".c++" || ext == ".c+") {
        return {Language::CXX, "CXX", true, false, false};
    }
    if (ext == ".c") { return {Language::C, "C", true, false, false}; }
    if (ext == ".cu") { return {Language::CUDA, "CUDA", true, false, false}; }
    if (ext == ".s" || ext == ".S" || ext == ".asm" || ext == ".sx") { return {Language::ASM, "ASM", true, false, false}; }
    if (ext == ".h" || ext == ".hpp" || ext == ".hxx" || ext == ".hh") { return {Language::HEADER, "HEADER", false, true, false}; }
    return {Language::UNKNOWN, "UNKNOWN", false, false, false};
}

std::string_view language_name(Language lang) {
    switch (lang) {
    case Language::C:
        return "C";
    case Language::CXX:
        return "CXX";
    case Language::CUDA:
        return "CUDA";
    case Language::ASM:
        return "ASM";
    case Language::HEADER:
        return "HEADER";
    case Language::UNKNOWN:
        return "UNKNOWN";
    }
    return "UNKNOWN";
}

bool language_has_compiler(Language lang) {
    switch (lang) {
    case Language::C:
    case Language::CXX:
    case Language::CUDA:
    case Language::ASM:
        return true;
    case Language::HEADER:
    case Language::UNKNOWN:
        return false;
    }
    return false;
}

LanguageInfo LanguageClassifier::from_path(std::string_view path) {
    std::filesystem::path p(path);
    return from_extension(p.extension().string());
}

} // namespace kiln
