#pragma once
#include <string>
#include <string_view>

namespace kiln {

enum class Language { C, CXX, CUDA, ASM, HEADER, UNKNOWN };

// Canonical CMake-facing name for a language ("C", "CXX", "CUDA", "ASM",
// "HEADER", "UNKNOWN").
std::string_view language_name(Language lang);

// Whether `lang` is a source-compileable language (i.e. has an associated
// CMAKE_<LANG>_COMPILER). False for HEADER and UNKNOWN.
bool language_has_compiler(Language lang);

struct LanguageInfo {
    Language lang;
    std::string_view name;
    bool is_compileable;
    bool is_header;
    bool is_module_interface = false; // True for .ixx, .cppm, etc.
};

class LanguageClassifier {
public:
    static LanguageInfo from_extension(std::string_view ext);
    static LanguageInfo from_path(std::string_view path);
};

} // namespace kiln
