#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include "kiln/interperter.hpp"
#include "kiln/cmake-language.hpp"
#include <sstream>
#include <fstream>
#include <filesystem>

// Helper to run a script and return output
static std::string run_script(const std::string& src) {
    std::stringstream output;
    kiln::Interpreter interpreter("", &output);

    interpreter.set_source_view(src);
    kiln::Parser parser(src);
    auto ast_or_error = parser.parse();
    if (!ast_or_error.has_value()) {
        const auto error = ast_or_error.error();
        throw std::runtime_error("Failed to parse script - " + error.reason + " on line " + std::to_string(error.row));
    }

    auto result = interpreter.interpret(ast_or_error.value());
    if (!result) { throw std::runtime_error(result.error().message); }

    return output.str();
}

// Helper to get a variable after running a script
static std::string get_variable(const std::string& src, const std::string& var_name) {
    std::stringstream output;
    kiln::Interpreter interpreter("", &output);

    interpreter.set_source_view(src);
    kiln::Parser parser(src);
    auto ast_or_error = parser.parse();
    if (!ast_or_error.has_value()) {
        const auto error = ast_or_error.error();
        throw std::runtime_error("Failed to parse script - " + error.reason + " on line " + std::to_string(error.row));
    }

    auto result = interpreter.interpret(ast_or_error.value());
    if (!result) { throw std::runtime_error(result.error().message); }

    return interpreter.get_variable(var_name);
}

TEST_CASE("set_source_files_properties basic", "[source_properties]") {
    SECTION("Set single property on single file") {
        auto result = get_variable(R"(
            set_source_files_properties(test.cpp PROPERTIES GENERATED TRUE)
            get_source_file_property(val test.cpp GENERATED)
        )",
                                   "val");
        REQUIRE(result == "TRUE");
    }

    SECTION("Set multiple properties on single file") {
        std::stringstream output;
        kiln::Interpreter interpreter("", &output);

        std::string src = R"(
            set_source_files_properties(test.cpp PROPERTIES
                GENERATED TRUE
                COMPILE_FLAGS "-Wall -Wextra")
            get_source_file_property(gen test.cpp GENERATED)
            get_source_file_property(flags test.cpp COMPILE_FLAGS)
        )";

        interpreter.set_source_view(src);
        kiln::Parser parser(src);
        auto ast = parser.parse();
        REQUIRE(ast.has_value());
        auto result = interpreter.interpret(*ast);
        REQUIRE(result.has_value());

        REQUIRE(interpreter.get_variable("gen") == "TRUE");
        REQUIRE(interpreter.get_variable("flags") == "-Wall -Wextra");
    }

    SECTION("Set property on multiple files") {
        std::stringstream output;
        kiln::Interpreter interpreter("", &output);

        std::string src = R"(
            set_source_files_properties(a.cpp b.cpp c.cpp PROPERTIES GENERATED TRUE)
            get_source_file_property(a_gen a.cpp GENERATED)
            get_source_file_property(b_gen b.cpp GENERATED)
            get_source_file_property(c_gen c.cpp GENERATED)
        )";

        interpreter.set_source_view(src);
        kiln::Parser parser(src);
        auto ast = parser.parse();
        REQUIRE(ast.has_value());
        auto result = interpreter.interpret(*ast);
        REQUIRE(result.has_value());

        REQUIRE(interpreter.get_variable("a_gen") == "TRUE");
        REQUIRE(interpreter.get_variable("b_gen") == "TRUE");
        REQUIRE(interpreter.get_variable("c_gen") == "TRUE");
    }
}

TEST_CASE("get_source_file_property", "[source_properties]") {
    SECTION("Returns NOTFOUND for unset property") {
        auto result = get_variable(R"(
            get_source_file_property(val test.cpp SOME_PROPERTY)
        )",
                                   "val");
        REQUIRE(result == "NOTFOUND");
    }

    SECTION("Returns value for set property") {
        auto result = get_variable(R"(
            set_source_files_properties(test.cpp PROPERTIES LANGUAGE CXX)
            get_source_file_property(val test.cpp LANGUAGE)
        )",
                                   "val");
        REQUIRE(result == "CXX");
    }
}

TEST_CASE("Source property types", "[source_properties]") {
    SECTION("GENERATED property") {
        auto result = get_variable(R"(
            set_source_files_properties(gen.cpp PROPERTIES GENERATED TRUE)
            get_source_file_property(val gen.cpp GENERATED)
        )",
                                   "val");
        REQUIRE(result == "TRUE");
    }

    SECTION("COMPILE_FLAGS property (space-separated string)") {
        auto result = get_variable(R"(
            set_source_files_properties(src.cpp PROPERTIES COMPILE_FLAGS "-O3 -march=native")
            get_source_file_property(val src.cpp COMPILE_FLAGS)
        )",
                                   "val");
        REQUIRE(result == "-O3 -march=native");
    }

    SECTION("COMPILE_OPTIONS property (semicolon-separated list)") {
        auto result = get_variable(R"(
            set_source_files_properties(src.cpp PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-pedantic")
            get_source_file_property(val src.cpp COMPILE_OPTIONS)
        )",
                                   "val");
        REQUIRE(result == "-Wall;-Wextra;-pedantic");
    }

    SECTION("COMPILE_DEFINITIONS property") {
        auto result = get_variable(R"(
            set_source_files_properties(src.cpp PROPERTIES COMPILE_DEFINITIONS "DEBUG;VERSION=1.0")
            get_source_file_property(val src.cpp COMPILE_DEFINITIONS)
        )",
                                   "val");
        REQUIRE(result == "DEBUG;VERSION=1.0");
    }

    SECTION("INCLUDE_DIRECTORIES property") {
        auto result = get_variable(R"(
            set_source_files_properties(src.cpp PROPERTIES INCLUDE_DIRECTORIES "/usr/include;/opt/include")
            get_source_file_property(val src.cpp INCLUDE_DIRECTORIES)
        )",
                                   "val");
        REQUIRE(result == "/usr/include;/opt/include");
    }

    SECTION("LANGUAGE property") {
        auto result = get_variable(R"(
            set_source_files_properties(file.c PROPERTIES LANGUAGE CXX)
            get_source_file_property(val file.c LANGUAGE)
        )",
                                   "val");
        REQUIRE(result == "CXX");
    }

    SECTION("HEADER_FILE_ONLY property") {
        auto result = get_variable(R"(
            set_source_files_properties(impl.h PROPERTIES HEADER_FILE_ONLY TRUE)
            get_source_file_property(val impl.h HEADER_FILE_ONLY)
        )",
                                   "val");
        REQUIRE(result == "TRUE");
    }

    SECTION("OBJECT_DEPENDS property") {
        auto result = get_variable(R"(
            set_source_files_properties(src.cpp PROPERTIES OBJECT_DEPENDS "generated.h;other.h")
            get_source_file_property(val src.cpp OBJECT_DEPENDS)
        )",
                                   "val");
        REQUIRE(result == "generated.h;other.h");
    }
}

TEST_CASE("set_source_files_properties error handling", "[source_properties]") {
    SECTION("Error on missing PROPERTIES keyword") {
        std::stringstream output;
        kiln::Interpreter interpreter("", &output);

        std::string src = R"(
            set_source_files_properties(test.cpp GENERATED TRUE)
        )";

        interpreter.set_source_view(src);
        kiln::Parser parser(src);
        auto ast = parser.parse();
        REQUIRE(ast.has_value());
        auto result = interpreter.interpret(*ast);
        REQUIRE_FALSE(result.has_value());
        REQUIRE(result.error().message.find("PROPERTIES") != std::string::npos);
    }

    SECTION("Empty file list is accepted with warning") {
        // Undocumented CMake behavior: empty file list is silently accepted
        std::stringstream output;
        kiln::Interpreter interpreter("", &output);

        std::string src = R"(
            set_source_files_properties(PROPERTIES GENERATED TRUE)
        )";

        interpreter.set_source_view(src);
        kiln::Parser parser(src);
        auto ast = parser.parse();
        REQUIRE(ast.has_value());
        auto result = interpreter.interpret(*ast);
        REQUIRE(result.has_value());
    }

    SECTION("Error on odd property arguments") {
        std::stringstream output;
        kiln::Interpreter interpreter("", &output);

        std::string src = R"(
            set_source_files_properties(test.cpp PROPERTIES GENERATED)
        )";

        interpreter.set_source_view(src);
        kiln::Parser parser(src);
        auto ast = parser.parse();
        REQUIRE(ast.has_value());
        auto result = interpreter.interpret(*ast);
        REQUIRE_FALSE(result.has_value());
    }
}

TEST_CASE("Path normalization", "[source_properties]") {
    SECTION("Relative paths are normalized") {
        std::stringstream output;
        kiln::Interpreter interpreter("", &output);

        // Set CMAKE_CURRENT_SOURCE_DIR for path resolution
        interpreter.set_variable("CMAKE_CURRENT_SOURCE_DIR", "/project/src");

        std::string src = R"(
            set_source_files_properties(subdir/../file.cpp PROPERTIES GENERATED TRUE)
            get_source_file_property(val file.cpp GENERATED)
        )";

        interpreter.set_source_view(src);
        kiln::Parser parser(src);
        auto ast = parser.parse();
        REQUIRE(ast.has_value());
        auto result = interpreter.interpret(*ast);
        REQUIRE(result.has_value());

        // The property should be found when querying with normalized path
        REQUIRE(interpreter.get_variable("val") == "TRUE");
    }
}
