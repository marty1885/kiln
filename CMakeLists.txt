cmake_minimum_required(VERSION 3.15)
project(kiln)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(PCRE2 REQUIRED IMPORTED_TARGET libpcre2-8)

find_package(CURL REQUIRED)
find_package(LibArchive REQUIRED)
find_package(pugixml REQUIRED)
find_package(CLI11 CONFIG REQUIRED)


add_library(kiln
    kiln/cmake-language.cpp
    kiln/interperter.cpp
    kiln/target.cpp
    kiln/build_system.cpp
    kiln/utils.cpp
    kiln/command_parser.cpp
    kiln/language.cpp
    kiln/module_scanner.cpp
    kiln/cache_store.cpp
    kiln/genex_parser.cpp
    kiln/genex_evaluator.cpp
    kiln/compile_features.cpp
    kiln/inner/blake2b.c
    kiln/inner/sha256.cpp
    kiln/inner/md5.cpp
    kiln/inner/sha1.cpp
    kiln/builtins/message.cpp
    kiln/builtins/variable.cpp
    kiln/builtins/list.cpp
    kiln/builtins/target.cpp
    kiln/builtins/project.cpp
    kiln/builtins/file.cpp
    kiln/builtins/find_package.cpp
    kiln/builtins/find_commands.cpp
    kiln/builtins/math.cpp
    kiln/builtins/string.cpp
    kiln/builtins/process.cpp
    kiln/builtins/property.cpp
    kiln/builtins/try_compile.cpp
    kiln/builtins/path.cpp
    kiln/builtins/install.cpp
    kiln/builtins/export_generator.cpp
    kiln/builtins/source_properties.cpp
    kiln/builtins/system_info.cpp
    kiln/intercept/download_utils.cpp
    kiln/intercept/external_project.cpp
    kiln/intercept/external_project_target.cpp
    kiln/intercept/fetch_content.cpp
    kiln/install_executor.cpp
    kiln/printing.cpp
    kiln/progress_bar.cpp
    kiln/profiler.cpp
    kiln/regex.cpp
    kiln/CMakeArray.cpp
    kiln/tool_mode.cpp
    kiln/condition_evaluator.cpp
    kiln/ast_cache.cpp
    kiln/path.cpp
    kiln/debugger.cpp
    kiln/autogen.cpp
    kiln/presets.cpp
)

target_include_directories(kiln PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(kiln PUBLIC
    PkgConfig::PCRE2
    CURL::libcurl
    LibArchive::LibArchive
    pugixml::pugixml
)

target_precompile_headers(kiln PRIVATE kiln/pch.hpp)

# --- Executable ---

add_executable(kiln_cli
    kiln-cli/main.cpp
    kiln-cli/linenoise-ng/src/linenoise.cpp
    kiln-cli/linenoise-ng/src/ConvertUTF.cpp
    kiln-cli/linenoise-ng/src/wcwidth.cpp
)
target_include_directories(kiln_cli PRIVATE kiln-cli/linenoise-ng/include)
target_link_libraries(kiln_cli PRIVATE kiln CLI11::CLI11)
set_target_properties(kiln_cli PROPERTIES OUTPUT_NAME kiln)

# --- Install rules ---

install(TARGETS kiln_cli kiln
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
)

# --- Testing ---

include(CTest)
if(BUILD_TESTING)
  find_package(Catch2 3 REQUIRED)
  add_executable(kiln_tests tests/main.cpp
        tests/interpreter_tests.cpp
        tests/pch_tests.cpp
        tests/command_parser.cpp
        tests/language_tests.cpp
        tests/compiler_tests.cpp
        tests/target_sources_tests.cpp
        tests/shadow_map_tests.cpp
        tests/genex_tests.cpp
        tests/compile_features_tests.cpp
        tests/source_properties_tests.cpp
        tests/cmake_array_tests.cpp
        tests/resolve_tests.cpp
        tests/path_tests.cpp
        tests/version_compare_tests.cpp)
  target_link_libraries(kiln_tests PRIVATE kiln Catch2::Catch2WithMain)

  add_test(NAME interpreter_tests COMMAND kiln_tests)
endif()
