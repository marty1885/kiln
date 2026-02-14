#include <catch2/catch_test_macros.hpp>
#include "../dmake/genex_parser.hpp"
#include "../dmake/genex_evaluator.hpp"
#include "../dmake/target.hpp"
#include "../dmake/build_system.hpp"
#include <map>
#include <memory>

using namespace dmake;

TEST_CASE("GenexParser - Simple literal", "[genex][parser]") {
    GenexParser parser;
    auto result = parser.parse("no genex here");
    REQUIRE(result.has_value());
    REQUIRE(result->has_genex == false);
    REQUIRE(result->nodes.size() == 1);
    REQUIRE(result->nodes[0]->type == GenexNodeType::LITERAL);
    REQUIRE(result->nodes[0]->raw_content == "no genex here");
}

TEST_CASE("GenexParser - BUILD_INTERFACE", "[genex][parser]") {
    GenexParser parser;
    auto result = parser.parse("$<BUILD_INTERFACE:/path/to/include>");
    REQUIRE(result.has_value());
    REQUIRE(result->has_genex == true);
    REQUIRE(result->nodes.size() == 1);
    REQUIRE(result->nodes[0]->type == GenexNodeType::BUILD_INTERFACE);
}

TEST_CASE("GenexParser - INSTALL_INTERFACE", "[genex][parser]") {
    GenexParser parser;
    auto result = parser.parse("$<INSTALL_INTERFACE:include>");
    REQUIRE(result.has_value());
    REQUIRE(result->has_genex == true);
    REQUIRE(result->nodes.size() == 1);
    REQUIRE(result->nodes[0]->type == GenexNodeType::INSTALL_INTERFACE);
}

TEST_CASE("GenexParser - CONFIG", "[genex][parser]") {
    GenexParser parser;
    auto result = parser.parse("$<CONFIG:Debug>");
    REQUIRE(result.has_value());
    REQUIRE(result->has_genex == true);
    REQUIRE(result->nodes.size() == 1);
    REQUIRE(result->nodes[0]->type == GenexNodeType::CONFIG);
    REQUIRE(result->nodes[0]->raw_content == "Debug");
}

TEST_CASE("GenexParser - Mixed literal and genex", "[genex][parser]") {
    GenexParser parser;
    auto result = parser.parse("prefix_$<CONFIG:Debug>_suffix");
    REQUIRE(result.has_value());
    REQUIRE(result->has_genex == true);
    REQUIRE(result->nodes.size() == 3);
    REQUIRE(result->nodes[0]->type == GenexNodeType::LITERAL);
    REQUIRE(result->nodes[0]->raw_content == "prefix_");
    REQUIRE(result->nodes[1]->type == GenexNodeType::CONFIG);
    REQUIRE(result->nodes[2]->type == GenexNodeType::LITERAL);
    REQUIRE(result->nodes[2]->raw_content == "_suffix");
}

TEST_CASE("GenexParser - Nested genex", "[genex][parser]") {
    GenexParser parser;
    auto result = parser.parse("$<$<CONFIG:Debug>:-g>");
    REQUIRE(result.has_value());
    REQUIRE(result->has_genex == true);
}

TEST_CASE("GenexParser - Validation - supported genex", "[genex][parser][validation]") {
    auto result = GenexParser::validate_genex_support("$<BUILD_INTERFACE:/path>");
    REQUIRE(result.has_value());
}

TEST_CASE("GenexParser - Validation - unsupported genex", "[genex][parser][validation]") {
    auto result = GenexParser::validate_genex_support("$<THIS_GENEX_DOES_NOT_EXIST:foo>");
    REQUIRE(!result.has_value());
    REQUIRE(result.error().find("Unsupported") != std::string::npos);
}

TEST_CASE("GenexParser - Validation - malformed genex", "[genex][parser][validation]") {
    auto result = GenexParser::validate_genex_support("$<CONFIG:Debug");
    REQUIRE(!result.has_value());
    REQUIRE(result.error().find("Unmatched") != std::string::npos);
}

TEST_CASE("GenexEvaluator - BUILD_INTERFACE in BUILD phase", "[genex][evaluator]") {
    GenexEvaluationContext ctx;
    ctx.phase = GenexEvaluationContext::Phase::BUILD;
    GenexEvaluator eval(ctx);

    auto result = eval.evaluate("$<BUILD_INTERFACE:/path/to/include>");
    REQUIRE(result.has_value());
    REQUIRE(*result == "/path/to/include");
}

TEST_CASE("GenexEvaluator - INSTALL_INTERFACE in BUILD phase", "[genex][evaluator]") {
    GenexEvaluationContext ctx;
    ctx.phase = GenexEvaluationContext::Phase::BUILD;
    GenexEvaluator eval(ctx);

    auto result = eval.evaluate("$<INSTALL_INTERFACE:include>");
    REQUIRE(result.has_value());
    REQUIRE(*result == "");  // Empty in BUILD phase
}

TEST_CASE("GenexEvaluator - CONFIG matching", "[genex][evaluator]") {
    GenexEvaluationContext ctx;
    ctx.build_type = "Debug";
    GenexEvaluator eval(ctx);

    auto result = eval.evaluate("$<CONFIG:Debug>");
    REQUIRE(result.has_value());
    REQUIRE(*result == "1");

    result = eval.evaluate("$<CONFIG:Release>");
    REQUIRE(result.has_value());
    REQUIRE(*result == "0");
}

TEST_CASE("GenexEvaluator - CONFIG case-insensitive", "[genex][evaluator]") {
    GenexEvaluationContext ctx;
    ctx.build_type = "Debug";
    GenexEvaluator eval(ctx);

    auto result = eval.evaluate("$<CONFIG:debug>");
    REQUIRE(result.has_value());
    REQUIRE(*result == "1");
}

TEST_CASE("GenexEvaluator - Nested CONFIG conditional", "[genex][evaluator]") {
    GenexEvaluationContext ctx;
    ctx.build_type = "Debug";
    GenexEvaluator eval(ctx);

    auto result = eval.evaluate("$<$<CONFIG:Debug>:-g>");
    REQUIRE(result.has_value());
    REQUIRE(*result == "-g");

    ctx.build_type = "Release";
    GenexEvaluator eval2(ctx);
    result = eval2.evaluate("$<$<CONFIG:Debug>:-g>");
    REQUIRE(result.has_value());
    REQUIRE(*result == "");
}

TEST_CASE("GenexEvaluator - BOOL expression", "[genex][evaluator]") {
    GenexEvaluationContext ctx;
    GenexEvaluator eval(ctx);

    // Truthy values
    REQUIRE(eval.evaluate("$<BOOL:ON>").value() == "1");
    REQUIRE(eval.evaluate("$<BOOL:YES>").value() == "1");
    REQUIRE(eval.evaluate("$<BOOL:TRUE>").value() == "1");
    REQUIRE(eval.evaluate("$<BOOL:1>").value() == "1");
    REQUIRE(eval.evaluate("$<BOOL:Y>").value() == "1");
    REQUIRE(eval.evaluate("$<BOOL:anything>").value() == "1");

    // Falsy values
    REQUIRE(eval.evaluate("$<BOOL:>").value() == "0");
    REQUIRE(eval.evaluate("$<BOOL:0>").value() == "0");
    REQUIRE(eval.evaluate("$<BOOL:OFF>").value() == "0");
    REQUIRE(eval.evaluate("$<BOOL:NO>").value() == "0");
    REQUIRE(eval.evaluate("$<BOOL:FALSE>").value() == "0");
    REQUIRE(eval.evaluate("$<BOOL:N>").value() == "0");
    REQUIRE(eval.evaluate("$<BOOL:IGNORE>").value() == "0");
    REQUIRE(eval.evaluate("$<BOOL:NOTFOUND>").value() == "0");
}

TEST_CASE("GenexEvaluator - NOT expression", "[genex][evaluator]") {
    GenexEvaluationContext ctx;
    GenexEvaluator eval(ctx);

    REQUIRE(eval.evaluate("$<NOT:ON>").value() == "0");
    REQUIRE(eval.evaluate("$<NOT:OFF>").value() == "1");
    REQUIRE(eval.evaluate("$<NOT:0>").value() == "1");
    REQUIRE(eval.evaluate("$<NOT:1>").value() == "0");
}

TEST_CASE("GenexEvaluator - AND expression", "[genex][evaluator]") {
    GenexEvaluationContext ctx;
    GenexEvaluator eval(ctx);

    REQUIRE(eval.evaluate("$<AND:1,1>").value() == "1");
    REQUIRE(eval.evaluate("$<AND:1,0>").value() == "0");
    REQUIRE(eval.evaluate("$<AND:0,1>").value() == "0");
    REQUIRE(eval.evaluate("$<AND:0,0>").value() == "0");
    REQUIRE(eval.evaluate("$<AND:1,1,1>").value() == "1");
    REQUIRE(eval.evaluate("$<AND:1,1,0>").value() == "0");
}

TEST_CASE("GenexEvaluator - OR expression", "[genex][evaluator]") {
    GenexEvaluationContext ctx;
    GenexEvaluator eval(ctx);

    REQUIRE(eval.evaluate("$<OR:1,1>").value() == "1");
    REQUIRE(eval.evaluate("$<OR:1,0>").value() == "1");
    REQUIRE(eval.evaluate("$<OR:0,1>").value() == "1");
    REQUIRE(eval.evaluate("$<OR:0,0>").value() == "0");
    REQUIRE(eval.evaluate("$<OR:0,0,1>").value() == "1");
}

TEST_CASE("GenexEvaluator - STREQUAL expression", "[genex][evaluator]") {
    GenexEvaluationContext ctx;
    GenexEvaluator eval(ctx);

    REQUIRE(eval.evaluate("$<STREQUAL:foo,foo>").value() == "1");
    REQUIRE(eval.evaluate("$<STREQUAL:foo,bar>").value() == "0");
    REQUIRE(eval.evaluate("$<STREQUAL:,>").value() == "1");
}

TEST_CASE("GenexEvaluator - IF expression", "[genex][evaluator]") {
    GenexEvaluationContext ctx;
    GenexEvaluator eval(ctx);

    REQUIRE(eval.evaluate("$<IF:1,true_val,false_val>").value() == "true_val");
    REQUIRE(eval.evaluate("$<IF:0,true_val,false_val>").value() == "false_val");
}

TEST_CASE("GenexEvaluator - TARGET_EXISTS", "[genex][evaluator]") {
    std::map<std::string, std::shared_ptr<Target>> targets;
    targets["mylib"] = std::make_shared<Target>("mylib", TargetType::STATIC_LIBRARY, "/src", "/build");

    GenexEvaluationContext ctx;
    ctx.all_targets = &targets;
    GenexEvaluator eval(ctx);

    REQUIRE(eval.evaluate("$<TARGET_EXISTS:mylib>").value() == "1");
    REQUIRE(eval.evaluate("$<TARGET_EXISTS:notexist>").value() == "0");
}

TEST_CASE("GenexEvaluator - TARGET_FILE", "[genex][evaluator]") {
    std::map<std::string, std::shared_ptr<Target>> targets;
    targets["myexe"] = std::make_shared<Target>("myexe", TargetType::EXECUTABLE, "/src", "/build");
    targets["mylib"] = std::make_shared<Target>("mylib", TargetType::STATIC_LIBRARY, "/src", "/build");

    GenexEvaluationContext ctx;
    ctx.all_targets = &targets;
    GenexEvaluator eval(ctx);

    // TARGET_FILE returns full path
    auto exe_path = eval.evaluate("$<TARGET_FILE:myexe>");
    REQUIRE(exe_path.has_value());
    REQUIRE(exe_path->find("myexe") != std::string::npos);

    // TARGET_FILE_NAME returns just the filename
    auto exe_name = eval.evaluate("$<TARGET_FILE_NAME:myexe>");
    REQUIRE(exe_name.has_value());
    REQUIRE(*exe_name == "myexe");

    // TARGET_FILE_DIR returns the directory
    auto exe_dir = eval.evaluate("$<TARGET_FILE_DIR:myexe>");
    REQUIRE(exe_dir.has_value());
    REQUIRE(exe_dir->find("/build") != std::string::npos);

    // Error for non-existent target
    auto bad = eval.evaluate("$<TARGET_FILE:notexist>");
    REQUIRE(!bad.has_value());
    REQUIRE(bad.error().find("not found") != std::string::npos);
}

TEST_CASE("GenexEvaluator - COMPILE_LANGUAGE", "[genex][evaluator]") {
    GenexEvaluationContext ctx;
    ctx.compile_language = Language::CXX;
    GenexEvaluator eval(ctx);

    REQUIRE(eval.evaluate("$<COMPILE_LANGUAGE:CXX>").value() == "1");
    REQUIRE(eval.evaluate("$<COMPILE_LANGUAGE:C>").value() == "0");

    ctx.compile_language = Language::C;
    GenexEvaluator eval2(ctx);
    REQUIRE(eval2.evaluate("$<COMPILE_LANGUAGE:C>").value() == "1");
    REQUIRE(eval2.evaluate("$<COMPILE_LANGUAGE:CXX>").value() == "0");
}

TEST_CASE("GenexEvaluator - COMPILE_LANG_AND_ID", "[genex][evaluator]") {
    SECTION("CXX language with matching compiler ID") {
        GenexEvaluationContext ctx;
        ctx.compile_language = Language::CXX;
        ctx.cxx_compiler_id = "GNU";
        GenexEvaluator eval(ctx);

        REQUIRE(eval.evaluate("$<COMPILE_LANG_AND_ID:CXX,GNU>").value() == "1");
        REQUIRE(eval.evaluate("$<COMPILE_LANG_AND_ID:CXX,Clang>").value() == "0");
    }

    SECTION("CXX language with multiple compiler IDs") {
        GenexEvaluationContext ctx;
        ctx.compile_language = Language::CXX;
        ctx.cxx_compiler_id = "Clang";
        GenexEvaluator eval(ctx);

        // Should match Clang in the list
        REQUIRE(eval.evaluate("$<COMPILE_LANG_AND_ID:CXX,AppleClang,Clang>").value() == "1");
        REQUIRE(eval.evaluate("$<COMPILE_LANG_AND_ID:CXX,GNU,MSVC>").value() == "0");
    }

    SECTION("C language with matching compiler ID") {
        GenexEvaluationContext ctx;
        ctx.compile_language = Language::C;
        ctx.c_compiler_id = "GNU";
        GenexEvaluator eval(ctx);

        REQUIRE(eval.evaluate("$<COMPILE_LANG_AND_ID:C,GNU>").value() == "1");
        REQUIRE(eval.evaluate("$<COMPILE_LANG_AND_ID:C,Clang>").value() == "0");
    }

    SECTION("Language mismatch returns 0") {
        GenexEvaluationContext ctx;
        ctx.compile_language = Language::C;
        ctx.c_compiler_id = "GNU";
        ctx.cxx_compiler_id = "GNU";
        GenexEvaluator eval(ctx);

        // Compiling C, but asking for CXX - should be 0
        REQUIRE(eval.evaluate("$<COMPILE_LANG_AND_ID:CXX,GNU>").value() == "0");
    }

    SECTION("Used in conditional expression") {
        GenexEvaluationContext ctx;
        ctx.compile_language = Language::CXX;
        ctx.cxx_compiler_id = "Clang";
        GenexEvaluator eval(ctx);

        auto result = eval.evaluate("$<$<COMPILE_LANG_AND_ID:CXX,AppleClang,Clang>:COMPILING_CXX_WITH_CLANG>");
        REQUIRE(result.has_value());
        REQUIRE(*result == "COMPILING_CXX_WITH_CLANG");

        // Change to Intel, should not match
        ctx.cxx_compiler_id = "Intel";
        GenexEvaluator eval2(ctx);
        result = eval2.evaluate("$<$<COMPILE_LANG_AND_ID:CXX,AppleClang,Clang>:COMPILING_CXX_WITH_CLANG>");
        REQUIRE(result.has_value());
        REQUIRE(*result == "");
    }

    SECTION("Deferred evaluation when no compile language") {
        GenexEvaluationContext ctx;
        ctx.allow_deferred_compile_language = true;
        // No compile_language set
        GenexEvaluator eval(ctx);

        auto result = eval.evaluate("$<COMPILE_LANG_AND_ID:CXX,GNU>");
        REQUIRE(result.has_value());
        REQUIRE(*result == "$<COMPILE_LANG_AND_ID:CXX,GNU>");  // Returned as-is for deferred evaluation
    }
}

TEST_CASE("GenexEvaluator - PLATFORM_ID", "[genex][evaluator]") {
    GenexEvaluationContext ctx;
    ctx.system_name = "Linux";
    GenexEvaluator eval(ctx);

    REQUIRE(eval.evaluate("$<PLATFORM_ID:Linux>").value() == "1");
    REQUIRE(eval.evaluate("$<PLATFORM_ID:Windows>").value() == "0");
}

TEST_CASE("GenexEvaluator - CXX_COMPILER_ID", "[genex][evaluator]") {
    GenexEvaluationContext ctx;
    ctx.cxx_compiler_id = "GNU";
    GenexEvaluator eval(ctx);

    REQUIRE(eval.evaluate("$<CXX_COMPILER_ID:GNU>").value() == "1");
    REQUIRE(eval.evaluate("$<CXX_COMPILER_ID:Clang>").value() == "0");
}

TEST_CASE("GenexEvaluator - C_COMPILER_ID", "[genex][evaluator]") {
    GenexEvaluationContext ctx;
    ctx.c_compiler_id = "GNU";
    GenexEvaluator eval(ctx);

    REQUIRE(eval.evaluate("$<C_COMPILER_ID:GNU>").value() == "1");
    REQUIRE(eval.evaluate("$<C_COMPILER_ID:Clang>").value() == "0");
}

TEST_CASE("GenexEvaluator - Property list evaluation", "[genex][evaluator]") {
    GenexEvaluationContext ctx;
    ctx.build_type = "Debug";
    ctx.phase = GenexEvaluationContext::Phase::BUILD;
    GenexEvaluator eval(ctx);

    std::vector<std::string> values = {
        "$<BUILD_INTERFACE:/include>",
        "$<INSTALL_INTERFACE:include>",
        "$<$<CONFIG:Debug>:/debug/include>",
        "/always/include"
    };

    auto result = eval.evaluate_property_list(values);
    REQUIRE(result.has_value());
    REQUIRE(result->size() == 3);  // INSTALL_INTERFACE and non-matching CONFIG are removed
    REQUIRE((*result)[0] == "/include");
    REQUIRE((*result)[1] == "/debug/include");
    REQUIRE((*result)[2] == "/always/include");
}

TEST_CASE("GenexEvaluator - Complex nested expression", "[genex][evaluator]") {
    GenexEvaluationContext ctx;
    ctx.build_type = "Debug";
    ctx.cxx_compiler_id = "GNU";
    GenexEvaluator eval(ctx);

    // $<$<AND:$<CONFIG:Debug>,$<CXX_COMPILER_ID:GNU>>:-g -O0>
    auto result = eval.evaluate("$<$<AND:$<CONFIG:Debug>,$<CXX_COMPILER_ID:GNU>>:-g -O0>");
    REQUIRE(result.has_value());
    REQUIRE(*result == "-g -O0");

    ctx.cxx_compiler_id = "Clang";
    GenexEvaluator eval2(ctx);
    result = eval2.evaluate("$<$<AND:$<CONFIG:Debug>,$<CXX_COMPILER_ID:GNU>>:-g -O0>");
    REQUIRE(result.has_value());
    REQUIRE(*result == "");
}

TEST_CASE("GenexParser - Extract genex types", "[genex][parser]") {
    auto result = GenexParser::extract_genex_types("$<BUILD_INTERFACE:/path> $<CONFIG:Debug>");
    REQUIRE(result.has_value());
    REQUIRE(result->size() == 2);
    REQUIRE(result->count(GenexNodeType::BUILD_INTERFACE) == 1);
    REQUIRE(result->count(GenexNodeType::CONFIG) == 1);
}

// ============================================================================
// LINK_ONLY Tests
// ============================================================================

TEST_CASE("GenexParser - LINK_ONLY", "[genex][parser][link_only]") {
    GenexParser parser;
    auto result = parser.parse("$<LINK_ONLY:pthread>");
    REQUIRE(result.has_value());
    REQUIRE(result->has_genex == true);
    REQUIRE(result->nodes.size() == 1);
    REQUIRE(result->nodes[0]->type == GenexNodeType::LINK_ONLY);
}

TEST_CASE("GenexEvaluator - LINK_ONLY basic evaluation", "[genex][evaluator][link_only]") {
    GenexEvaluationContext ctx;
    GenexEvaluator eval(ctx);

    // LINK_ONLY should evaluate to its content
    auto result = eval.evaluate("$<LINK_ONLY:pthread>");
    REQUIRE(result.has_value());
    REQUIRE(*result == "pthread");
}

TEST_CASE("GenexEvaluator - LINK_ONLY with nested genex", "[genex][evaluator][link_only]") {
    GenexEvaluationContext ctx;
    ctx.build_type = "Debug";
    GenexEvaluator eval(ctx);

    // LINK_ONLY can wrap other genex
    auto result = eval.evaluate("$<LINK_ONLY:$<$<CONFIG:Debug>:debug_lib>>");
    REQUIRE(result.has_value());
    REQUIRE(*result == "debug_lib");

    ctx.build_type = "Release";
    GenexEvaluator eval2(ctx);
    result = eval2.evaluate("$<LINK_ONLY:$<$<CONFIG:Debug>:debug_lib>>");
    REQUIRE(result.has_value());
    REQUIRE(*result == "");
}

TEST_CASE("GenexEvaluator - evaluate_link_library returns metadata", "[genex][evaluator][link_only]") {
    GenexEvaluationContext ctx;
    GenexEvaluator eval(ctx);

    // Regular library - link_only should be false
    auto result = eval.evaluate_link_library("pthread");
    REQUIRE(result.has_value());
    REQUIRE(result->value == "pthread");
    REQUIRE(result->link_only == false);

    // LINK_ONLY wrapped - link_only should be true
    result = eval.evaluate_link_library("$<LINK_ONLY:pthread>");
    REQUIRE(result.has_value());
    REQUIRE(result->value == "pthread");
    REQUIRE(result->link_only == true);
}

TEST_CASE("GenexEvaluator - evaluate_link_library with complex genex", "[genex][evaluator][link_only]") {
    GenexEvaluationContext ctx;
    ctx.build_type = "Debug";
    ctx.phase = GenexEvaluationContext::Phase::BUILD;
    GenexEvaluator eval(ctx);

    // BUILD_INTERFACE - not link_only
    auto result = eval.evaluate_link_library("$<BUILD_INTERFACE:mylib>");
    REQUIRE(result.has_value());
    REQUIRE(result->value == "mylib");
    REQUIRE(result->link_only == false);

    // LINK_ONLY wrapping BUILD_INTERFACE
    result = eval.evaluate_link_library("$<LINK_ONLY:$<BUILD_INTERFACE:mylib>>");
    REQUIRE(result.has_value());
    REQUIRE(result->value == "mylib");
    REQUIRE(result->link_only == true);
}

TEST_CASE("GenexParser - Multi-line genex in unquoted arguments", "[genex][parser][multiline]") {
    // This tests that the CMake parser correctly handles genex that span multiple lines
    // The parser should track genex depth and not stop at whitespace/newlines inside genex
    std::string input = R"($<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>:
        -Wall -Wextra -Wpedantic -Werror>)";

    GenexParser parser;
    auto result = parser.parse(input);

    // Should parse successfully without "Unmatched '<'" error
    REQUIRE(result.has_value());
    REQUIRE(result->has_genex == true);

    // The entire thing should be parsed as one genex node (the outer conditional)
    REQUIRE(result->nodes.size() == 1);
    REQUIRE(result->nodes[0]->type == GenexNodeType::CONDITIONAL);
}

TEST_CASE("GenexParser - Complex multi-line genex from real usage", "[genex][parser][multiline]") {
    // Real-world example: multi-compiler warning flags spanning multiple lines
    std::string input = R"($<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:-Wall -Wextra>)";

    GenexParser parser;
    auto result = parser.parse(input);

    REQUIRE(result.has_value());
    REQUIRE(result->has_genex == true);

    // Validate that it can be evaluated
    GenexEvaluationContext ctx;
    ctx.cxx_compiler_id = "GNU";
    GenexEvaluator eval(ctx);

    auto eval_result = eval.evaluate(input);
    REQUIRE(eval_result.has_value());
    REQUIRE(*eval_result == "-Wall -Wextra");
}

TEST_CASE("GenexEvaluator - Pure genex whitespace splitting", "[genex][evaluator][whitespace]") {
    // When a value is ONLY a generator expression (pure genex), the result should be split by whitespace
    // This matches CMake's behavior where genex acts like unquoted arguments
    GenexEvaluationContext ctx;
    ctx.cxx_compiler_id = "GNU";
    GenexEvaluator eval(ctx);

    SECTION("Pure genex should split on whitespace") {
        std::vector<std::string> values = {
            "$<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra -Wshadow>",
            "$<$<CXX_COMPILER_ID:GNU>:-pedantic -pedantic-errors>"
        };

        auto result = eval.evaluate_property_list(values);
        REQUIRE(result.has_value());

        // Should be split into 5 separate flags
        REQUIRE(result->size() == 5);
        REQUIRE((*result)[0] == "-Wall");
        REQUIRE((*result)[1] == "-Wextra");
        REQUIRE((*result)[2] == "-Wshadow");
        REQUIRE((*result)[3] == "-pedantic");
        REQUIRE((*result)[4] == "-pedantic-errors");
    }

    SECTION("Mixed content should NOT split on whitespace") {
        // If genex is mixed with other text, keep as single value (like quoted argument)
        std::vector<std::string> values = {
            "prefix$<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra>suffix"
        };

        auto result = eval.evaluate_property_list(values);
        REQUIRE(result.has_value());

        // Should remain as single value
        REQUIRE(result->size() == 1);
        REQUIRE((*result)[0] == "prefix-Wall -Wextrasuffix");
    }

    SECTION("Literal values should NOT split on whitespace") {
        // Plain literals should remain as single values
        std::vector<std::string> values = {
            "-Wall -Wextra -Wshadow"
        };

        auto result = eval.evaluate_property_list(values);
        REQUIRE(result.has_value());

        // Should remain as single value
        REQUIRE(result->size() == 1);
        REQUIRE((*result)[0] == "-Wall -Wextra -Wshadow");
    }

    SECTION("Pure genex evaluating to empty should be skipped") {
        // Genex that evaluates to empty string should not contribute to result
        std::vector<std::string> values = {
            "$<$<CXX_COMPILER_ID:MSVC>:-Wall -Wextra>",  // Evaluates to empty (not MSVC)
            "$<$<CXX_COMPILER_ID:GNU>:-pedantic>"        // Evaluates to "-pedantic"
        };

        auto result = eval.evaluate_property_list(values);
        REQUIRE(result.has_value());

        // Only the second one should be present
        REQUIRE(result->size() == 1);
        REQUIRE((*result)[0] == "-pedantic");
    }

    SECTION("Pure genex should split on semicolons (CMake list separator)") {
        // When genex output contains semicolons, it represents a CMake list
        // Each list item should become a separate entry (like yaml-cpp's ${yaml-cpp-contrib-sources})
        std::vector<std::string> values = {
            "$<$<BOOL:ON>:src/a.cpp;src/b.cpp;src/c.cpp>"
        };

        auto result = eval.evaluate_property_list(values);
        REQUIRE(result.has_value());

        // Should be split into 3 separate sources
        REQUIRE(result->size() == 3);
        REQUIRE((*result)[0] == "src/a.cpp");
        REQUIRE((*result)[1] == "src/b.cpp");
        REQUIRE((*result)[2] == "src/c.cpp");
    }

    SECTION("Pure genex should split semicolons AND whitespace") {
        // Both semicolons and whitespace should cause splitting
        std::vector<std::string> values = {
            "$<$<BOOL:ON>:src/a.cpp;src/b.cpp -flag1 -flag2>"
        };

        auto result = eval.evaluate_property_list(values);
        REQUIRE(result.has_value());

        // Should be split into 4 separate items
        REQUIRE(result->size() == 4);
        REQUIRE((*result)[0] == "src/a.cpp");
        REQUIRE((*result)[1] == "src/b.cpp");
        REQUIRE((*result)[2] == "-flag1");
        REQUIRE((*result)[3] == "-flag2");
    }
}

// ============================================================================
// BuildGraph::evaluate_genex() Tests
// ============================================================================

TEST_CASE("BuildGraph::evaluate_genex evaluates genex in commands", "[genex][evaluate_genex]") {
    BuildGraph graph;

    SECTION("CONFIG genex in command arguments") {
        BuildTask task;
        task.id = "test_task";
        task.commands = {{"echo", "Config: $<CONFIG:Debug>", "$<$<CONFIG:Debug>:DEBUG_MODE>"}};

        auto txn = graph.begin();
        txn.add(std::move(task));
        txn.commit();

        GenexEvaluationContext ctx;
        ctx.build_type = "Debug";
        ctx.phase = GenexEvaluationContext::Phase::BUILD;

        auto result = graph.evaluate_genex(ctx);
        REQUIRE(result.has_value());

        auto& finalized = graph.get_task("test_task");
        REQUIRE(finalized.commands.size() == 1);
        REQUIRE(finalized.commands[0].size() == 3);
        REQUIRE(finalized.commands[0][0] == "echo");
        REQUIRE(finalized.commands[0][1] == "Config: 1");
        REQUIRE(finalized.commands[0][2] == "DEBUG_MODE");
    }

    SECTION("Empty genex results are omitted") {
        BuildTask task;
        task.id = "test_task";
        task.commands = {{"echo", "$<$<CONFIG:Release>:RELEASE_MODE>", "always"}};

        auto txn = graph.begin();
        txn.add(std::move(task));
        txn.commit();

        GenexEvaluationContext ctx;
        ctx.build_type = "Debug";
        ctx.phase = GenexEvaluationContext::Phase::BUILD;

        auto result = graph.evaluate_genex(ctx);
        REQUIRE(result.has_value());

        auto& finalized = graph.get_task("test_task");
        REQUIRE(finalized.commands.size() == 1);
        REQUIRE(finalized.commands[0].size() == 2);
        REQUIRE(finalized.commands[0][0] == "echo");
        REQUIRE(finalized.commands[0][1] == "always");
    }

    SECTION("working_dir genex evaluation") {
        BuildTask task;
        task.id = "test_task";
        task.commands = {{"ls"}};
        task.working_dir = "/build/$<$<CONFIG:Debug>:debug>";

        auto txn = graph.begin();
        txn.add(std::move(task));
        txn.commit();

        GenexEvaluationContext ctx;
        ctx.build_type = "Debug";
        ctx.phase = GenexEvaluationContext::Phase::BUILD;

        auto result = graph.evaluate_genex(ctx);
        REQUIRE(result.has_value());

        auto& finalized = graph.get_task("test_task");
        REQUIRE(finalized.working_dir == "/build/debug");
    }

    SECTION("COMPILE_LANGUAGE genex with task-specific language") {
        BuildTask task;
        task.id = "test_task";
        task.kind = CompileTask{"file.cpp", Language::CXX};
        task.commands = {{"g++", "$<$<COMPILE_LANGUAGE:CXX>:-std=c++17>", "file.cpp"}};

        auto txn = graph.begin();
        txn.add(std::move(task));
        txn.commit();

        GenexEvaluationContext ctx;
        ctx.phase = GenexEvaluationContext::Phase::BUILD;

        auto result = graph.evaluate_genex(ctx);
        REQUIRE(result.has_value());

        auto& finalized = graph.get_task("test_task");
        REQUIRE(finalized.commands.size() == 1);
        REQUIRE(finalized.commands[0].size() == 3);
        REQUIRE(finalized.commands[0][1] == "-std=c++17");
    }

    SECTION("Fast path - strings without genex are unchanged") {
        BuildTask task;
        task.id = "test_task";
        task.commands = {{"g++", "-O2", "-Wall", "file.cpp"}};

        auto txn = graph.begin();
        txn.add(std::move(task));
        txn.commit();

        GenexEvaluationContext ctx;
        ctx.phase = GenexEvaluationContext::Phase::BUILD;

        auto result = graph.evaluate_genex(ctx);
        REQUIRE(result.has_value());

        auto& finalized = graph.get_task("test_task");
        REQUIRE(finalized.commands[0] == std::vector<std::string>{"g++", "-O2", "-Wall", "file.cpp"});
    }

    SECTION("Multiple commands are all evaluated") {
        BuildTask task;
        task.id = "test_task";
        task.commands = {
            {"echo", "$<$<CONFIG:Debug>:cmd1>"},
            {"echo", "$<$<CONFIG:Debug>:cmd2>"}
        };

        auto txn = graph.begin();
        txn.add(std::move(task));
        txn.commit();

        GenexEvaluationContext ctx;
        ctx.build_type = "Debug";
        ctx.phase = GenexEvaluationContext::Phase::BUILD;

        auto result = graph.evaluate_genex(ctx);
        REQUIRE(result.has_value());

        auto& finalized = graph.get_task("test_task");
        REQUIRE(finalized.commands.size() == 2);
        REQUIRE(finalized.commands[0][1] == "cmd1");
        REQUIRE(finalized.commands[1][1] == "cmd2");
    }
}
