#include <catch2/catch_test_macros.hpp>
#include "kiln/build_system.hpp"
#include "kiln/target.hpp"
#include "kiln/toolchain.hpp"
#include "kiln/gnu_compiler.hpp"
#include "kiln/language.hpp"
#include "kiln/interperter.hpp"       // For Interpreter
#include "kiln/cmake-language.hpp"    // For Parser
#include "kiln/builtins/registry.hpp" // For register_target_builtins
#include <filesystem>
#include <string>
#include <algorithm>
#include <iostream>
#include <fstream> // For std::ofstream

using namespace kiln;

TEST_CASE("PCH Task Generation", "[target][pch]") {
    // Create a dummy environment
    std::string temp_dir = "build_test_pch";
    std::filesystem::create_directories(temp_dir);
    std::filesystem::create_directories(temp_dir + "/objs");

    // Create dummy source files for existence checks
    {
        std::ofstream main_cpp("main.cpp");
        main_cpp << "int main() { return 0; }\n";
        std::ofstream my_lib_cpp("my_lib.cpp");
        my_lib_cpp << "void foo() {}\n";
        std::ofstream my_pch_h("my_pch.h");
        my_pch_h << "#define PCH_TEST\n";
    }

    Interpreter interp(".", &std::cout, &std::cerr, temp_dir);

    // Enable C++ compiler (required for task generation)
    interp.enable_compiler_for_language("CXX");

    // Register builtins
    register_target_builtins(interp);

    // Setup a project with PCH
    std::string script = R"(
        add_library(my_lib my_lib.cpp)
        target_precompile_headers(my_lib PRIVATE my_pch.h)
    )";

    interp.set_source_view(script);
    Parser parser(script);
    auto ast_or_error = parser.parse();
    REQUIRE(ast_or_error.has_value());

    auto result = interp.interpret(ast_or_error.value());
    if (!result) { std::cerr << "Interpreter error: " << result.error().message << std::endl; }
    REQUIRE(result.has_value());

    auto& targets = interp.get_targets();
    REQUIRE(targets.count("my_lib") > 0);
    auto lib = targets["my_lib"];

    BuildGraph graph;
    auto txn = graph.begin();
    REQUIRE(lib->generate_tasks(txn, interp.get_toolchain(), interp.get_targets(), interp));
    REQUIRE(txn.commit());

    // Build directory as used by the interpreter
    std::string build_dir_abs = std::filesystem::absolute(temp_dir).lexically_normal().string();

    // Check if PCH task was generated
    std::string pch_wrapper_expected = std::filesystem::path(build_dir_abs) / "objs" / "my_lib_pch.hxx";
    pch_wrapper_expected = std::filesystem::path(pch_wrapper_expected).lexically_normal().string();
    std::string pch_gch_expected = pch_wrapper_expected + ".gch";

    REQUIRE(graph.has_task(pch_gch_expected));
    auto& pch_task = graph.get_task(pch_gch_expected);

    // PCH task should have the wrapper as input
    bool has_wrapper = false;
    for (const auto& in : pch_task.inputs) {
        if (in == pch_wrapper_expected) has_wrapper = true;
    }
    REQUIRE(has_wrapper);

    // Check if the object file task depends on the PCH
    // Object files are stored in: build_dir/objs/target_name/source.cpp.o
    std::string obj_file = std::filesystem::path(build_dir_abs) / "objs" / "my_lib" / "my_lib.cpp.o";
    obj_file = std::filesystem::path(obj_file).lexically_normal().string();
    REQUIRE(graph.has_task(obj_file));
    auto& obj_task = graph.get_task(obj_file);

    // After transaction commit, explicit_deps are resolved into pointer-based dependencies
    REQUIRE(std::find(obj_task.dependencies.begin(), obj_task.dependencies.end(), &pch_task) != obj_task.dependencies.end());
    REQUIRE(std::find_if(obj_task.commands.begin(), obj_task.commands.end(),
                         [](const auto& command) { return std::find(command.begin(), command.end(), "-include") != command.end(); })
            != obj_task.commands.end());
    REQUIRE(std::find_if(obj_task.commands.begin(), obj_task.commands.end(),
                         [pch_wrapper_expected](const auto& command) {
                             return std::find(command.begin(), command.end(), pch_wrapper_expected) != command.end();
                         })
            != obj_task.commands.end());

    // Cleanup
    std::filesystem::remove_all(temp_dir);
    std::filesystem::remove("main.cpp");
    std::filesystem::remove("my_lib.cpp");
    std::filesystem::remove("my_pch.h");
}
