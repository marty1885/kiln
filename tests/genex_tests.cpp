#include <catch2/catch_test_macros.hpp>
#include "../kiln/genex_parser.hpp"
#include "../kiln/genex_evaluator.hpp"
#include "../kiln/target.hpp"
#include "../kiln/build_system.hpp"
#include "../kiln/interperter.hpp"
#include <cstdlib>
#include <map>
#include <memory>
#include <random>
#include <sstream>

using namespace kiln;

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
    REQUIRE(*result == ""); // Empty in BUILD phase
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

TEST_CASE("GenexEvaluator - LINK_LANGUAGE infers from sources", "[genex][evaluator]") {
    auto cxx_lib = std::make_shared<Target>("cxx_lib", TargetType::STATIC_LIBRARY, "/src", "/build");
    cxx_lib->append_property("SOURCES", {"foo.cpp", "bar.cpp"}, PropertyVisibility::PRIVATE);

    auto c_lib = std::make_shared<Target>("c_lib", TargetType::STATIC_LIBRARY, "/src", "/build");
    c_lib->append_property("SOURCES", {"a.c"}, PropertyVisibility::PRIVATE);

    auto explicit_lib = std::make_shared<Target>("explicit_lib", TargetType::STATIC_LIBRARY, "/src", "/build");
    explicit_lib->append_property("SOURCES", {"x.cpp"}, PropertyVisibility::PRIVATE);
    explicit_lib->set_property("LINKER_LANGUAGE", "C");

    TargetMap targets;
    targets["cxx_lib"] = cxx_lib;
    targets["c_lib"] = c_lib;
    targets["explicit_lib"] = explicit_lib;

    auto eval_for = [&](Target* t) {
        GenexEvaluationContext ctx;
        ctx.all_targets = &targets;
        ctx.current_target = t;
        return GenexEvaluator(ctx);
    };

    {
        auto e = eval_for(cxx_lib.get());
        REQUIRE(e.evaluate("$<LINK_LANGUAGE>").value() == "CXX");
        REQUIRE(e.evaluate("$<LINK_LANGUAGE:C,CXX>").value() == "1");
        REQUIRE(e.evaluate("$<LINK_LANGUAGE:C>").value() == "0");
    }
    {
        auto e = eval_for(c_lib.get());
        REQUIRE(e.evaluate("$<LINK_LANGUAGE>").value() == "C");
        REQUIRE(e.evaluate("$<LINK_LANGUAGE:CXX>").value() == "0");
    }
    {
        // LINKER_LANGUAGE property overrides source-based inference.
        auto e = eval_for(explicit_lib.get());
        REQUIRE(e.evaluate("$<LINK_LANGUAGE>").value() == "C");
    }
    {
        // No target -> error.
        GenexEvaluationContext ctx;
        ctx.all_targets = &targets;
        GenexEvaluator e(ctx);
        auto r = e.evaluate("$<LINK_LANGUAGE>");
        REQUIRE_FALSE(r.has_value());
    }
}

TEST_CASE("GenexEvaluator - LINK_GROUP wraps libs with feature prologue/epilogue", "[genex][evaluator]") {
    std::ostringstream out;
    Interpreter interp("", &out);
    interp.set_variable("CMAKE_LINK_GROUP_USING_no_as_needed", "LINKER:--push-state,--no-as-needed;LINKER:--pop-state");

    GenexEvaluationContext ctx;
    ctx.interp = &interp;
    GenexEvaluator eval(ctx);

    auto r = eval.evaluate("$<LINK_GROUP:no_as_needed,Fontconfig::Fontconfig,Other::Lib>");
    REQUIRE(r.has_value());
    // Result is a CMake list (semicolon separated): prologue;libs...;epilogue.
    REQUIRE(*r == "LINKER:--push-state,--no-as-needed;Fontconfig::Fontconfig;Other::Lib;LINKER:--pop-state");

    // Single-arg wrapping (only prologue defined) — no epilogue emitted.
    interp.set_variable("CMAKE_LINK_GROUP_USING_only_pre", "LINKER:--start-group");
    auto r2 = eval.evaluate("$<LINK_GROUP:only_pre,libA>");
    REQUIRE(r2.has_value());
    REQUIRE(*r2 == "LINKER:--start-group;libA");

    // Undefined feature: emit libs without wrapping (degrade gracefully).
    auto r3 = eval.evaluate("$<LINK_GROUP:does_not_exist,libA,libB>");
    REQUIRE(r3.has_value());
    REQUIRE(*r3 == "libA;libB");

    // Missing interpreter -> error.
    GenexEvaluationContext bad_ctx;
    GenexEvaluator bad_eval(bad_ctx);
    auto r4 = bad_eval.evaluate("$<LINK_GROUP:no_as_needed,libA>");
    REQUIRE_FALSE(r4.has_value());
}

TEST_CASE("GenexEvaluator - TARGET_EXISTS", "[genex][evaluator]") {
    TargetMap targets;
    targets["mylib"] = std::make_shared<Target>("mylib", TargetType::STATIC_LIBRARY, "/src", "/build");

    GenexEvaluationContext ctx;
    ctx.all_targets = &targets;
    GenexEvaluator eval(ctx);

    REQUIRE(eval.evaluate("$<TARGET_EXISTS:mylib>").value() == "1");
    REQUIRE(eval.evaluate("$<TARGET_EXISTS:notexist>").value() == "0");
}

TEST_CASE("GenexEvaluator - TARGET_FILE", "[genex][evaluator]") {
    TargetMap targets;
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
        REQUIRE(*result == "$<COMPILE_LANG_AND_ID:CXX,GNU>"); // Returned as-is for deferred evaluation
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

    std::vector<std::string> values = {"$<BUILD_INTERFACE:/include>", "$<INSTALL_INTERFACE:include>", "$<$<CONFIG:Debug>:/debug/include>",
                                       "/always/include"};

    auto result = eval.evaluate_property_list(values);
    REQUIRE(result.has_value());
    REQUIRE(result->size() == 3); // INSTALL_INTERFACE and non-matching CONFIG are removed
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
        std::vector<std::string> values = {"$<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra -Wshadow>",
                                           "$<$<CXX_COMPILER_ID:GNU>:-pedantic -pedantic-errors>"};

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
        std::vector<std::string> values = {"prefix$<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra>suffix"};

        auto result = eval.evaluate_property_list(values);
        REQUIRE(result.has_value());

        // Should remain as single value
        REQUIRE(result->size() == 1);
        REQUIRE((*result)[0] == "prefix-Wall -Wextrasuffix");
    }

    SECTION("Literal values should NOT split on whitespace") {
        // Plain literals should remain as single values
        std::vector<std::string> values = {"-Wall -Wextra -Wshadow"};

        auto result = eval.evaluate_property_list(values);
        REQUIRE(result.has_value());

        // Should remain as single value
        REQUIRE(result->size() == 1);
        REQUIRE((*result)[0] == "-Wall -Wextra -Wshadow");
    }

    SECTION("Pure genex evaluating to empty should be skipped") {
        // Genex that evaluates to empty string should not contribute to result
        std::vector<std::string> values = {
            "$<$<CXX_COMPILER_ID:MSVC>:-Wall -Wextra>", // Evaluates to empty (not MSVC)
            "$<$<CXX_COMPILER_ID:GNU>:-pedantic>"       // Evaluates to "-pedantic"
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
        std::vector<std::string> values = {"$<$<BOOL:ON>:src/a.cpp;src/b.cpp;src/c.cpp>"};

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
        std::vector<std::string> values = {"$<$<BOOL:ON>:src/a.cpp;src/b.cpp -flag1 -flag2>"};

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

// ============================================================================
// CMake-verified genex evaluation tests
// Reference values captured from CMake 4.2.3 with file(GENERATE)
// ============================================================================

TEST_CASE("GenexEvaluator - bare CONFIG", "[genex][evaluator]") {
    GenexEvaluationContext ctx;
    ctx.build_type = "Debug";
    GenexEvaluator eval(ctx);

    // CMake ref: "Debug"
    REQUIRE(eval.evaluate("$<CONFIG>").value() == "Debug");
}

TEST_CASE("GenexEvaluator - LOWER_CASE", "[genex][evaluator]") {
    GenexEvaluationContext ctx;
    GenexEvaluator eval(ctx);

    // CMake ref: "hello"
    REQUIRE(eval.evaluate("$<LOWER_CASE:HeLLo>").value() == "hello");
    // CMake ref: ""
    REQUIRE(eval.evaluate("$<LOWER_CASE:>").value() == "");
}

TEST_CASE("GenexEvaluator - UPPER_CASE", "[genex][evaluator]") {
    GenexEvaluationContext ctx;
    GenexEvaluator eval(ctx);

    // CMake ref: "HELLO"
    REQUIRE(eval.evaluate("$<UPPER_CASE:HeLLo>").value() == "HELLO");
    // CMake ref: ""
    REQUIRE(eval.evaluate("$<UPPER_CASE:>").value() == "");
}

TEST_CASE("GenexEvaluator - JOIN", "[genex][evaluator]") {
    GenexEvaluationContext ctx;
    GenexEvaluator eval(ctx);

    // CMake ref: "a-b-c"
    REQUIRE(eval.evaluate("$<JOIN:a;b;c,->").value() == "a-b-c");
    // CMake ref: "single"
    REQUIRE(eval.evaluate("$<JOIN:single,->").value() == "single");
    // CMake ref: ""
    REQUIRE(eval.evaluate("$<JOIN:,->").value() == "");
}

TEST_CASE("GenexEvaluator - REMOVE_DUPLICATES", "[genex][evaluator]") {
    GenexEvaluationContext ctx;
    GenexEvaluator eval(ctx);

    // CMake ref: "a;b;c"
    REQUIRE(eval.evaluate("$<REMOVE_DUPLICATES:a;b;a;c;b>").value() == "a;b;c");
    // CMake ref: ""
    REQUIRE(eval.evaluate("$<REMOVE_DUPLICATES:>").value() == "");
    // CMake ref: "x"
    REQUIRE(eval.evaluate("$<REMOVE_DUPLICATES:x>").value() == "x");
}

TEST_CASE("GenexEvaluator - FILTER", "[genex][evaluator]") {
    GenexEvaluationContext ctx;
    GenexEvaluator eval(ctx);

    // CMake ref: "apple;apricot"
    REQUIRE(eval.evaluate("$<FILTER:apple;banana;apricot,INCLUDE,^ap>").value() == "apple;apricot");
    // CMake ref: "banana"
    REQUIRE(eval.evaluate("$<FILTER:apple;banana;apricot,EXCLUDE,^ap>").value() == "banana");
    // CMake ref: "a;b;c"
    REQUIRE(eval.evaluate("$<FILTER:a;b;c,INCLUDE,.*>").value() == "a;b;c");
}

TEST_CASE("GenexEvaluator - IN_LIST", "[genex][evaluator]") {
    GenexEvaluationContext ctx;
    GenexEvaluator eval(ctx);

    // CMake ref: "1"
    REQUIRE(eval.evaluate("$<IN_LIST:b,a;b;c>").value() == "1");
    // CMake ref: "0"
    REQUIRE(eval.evaluate("$<IN_LIST:x,a;b;c>").value() == "0");
    // CMake ref: "1"
    REQUIRE(eval.evaluate("$<IN_LIST:a,a>").value() == "1");
}

TEST_CASE("GenexEvaluator - GENEX_EVAL", "[genex][evaluator]") {
    GenexEvaluationContext ctx;
    ctx.build_type = "Debug";
    GenexEvaluator eval(ctx);

    // CMake ref: "hello"
    REQUIRE(eval.evaluate("$<GENEX_EVAL:$<LOWER_CASE:HELLO>>").value() == "hello");
    // CMake ref: "Debug"
    REQUIRE(eval.evaluate("$<GENEX_EVAL:$<CONFIG>>").value() == "Debug");
    // CMake ref: "no-genex-here"
    REQUIRE(eval.evaluate("$<GENEX_EVAL:no-genex-here>").value() == "no-genex-here");
}

TEST_CASE("GenexEvaluator - TARGET_GENEX_EVAL", "[genex][evaluator]") {
    TargetMap targets;
    targets["mylib"] = std::make_shared<Target>("mylib", TargetType::STATIC_LIBRARY, "/src", "/build");

    GenexEvaluationContext ctx;
    ctx.build_type = "Debug";
    ctx.all_targets = &targets;
    GenexEvaluator eval(ctx);

    // Evaluate expression in target context
    REQUIRE(eval.evaluate("$<TARGET_GENEX_EVAL:mylib,$<LOWER_CASE:HELLO>>").value() == "hello");

    // Error for non-existent target
    auto bad = eval.evaluate("$<TARGET_GENEX_EVAL:nonexist,$<CONFIG>>");
    REQUIRE(!bad.has_value());
    REQUIRE(bad.error().find("not found") != std::string::npos);
}

TEST_CASE("GenexEvaluator - TARGET_FILE_BASE_NAME", "[genex][evaluator]") {
    TargetMap targets;
    auto mylib = std::make_shared<Target>("mylib", TargetType::STATIC_LIBRARY, "/src", "/build");
    mylib->set_property("OUTPUT_NAME", "custom_mylib");
    targets["mylib"] = mylib;

    auto plain = std::make_shared<Target>("plain", TargetType::STATIC_LIBRARY, "/src", "/build");
    targets["plain"] = plain;

    GenexEvaluationContext ctx;
    ctx.all_targets = &targets;
    GenexEvaluator eval(ctx);

    // CMake ref: "custom_mylib"
    REQUIRE(eval.evaluate("$<TARGET_FILE_BASE_NAME:mylib>").value() == "custom_mylib");
    // No OUTPUT_NAME — falls back to target name
    REQUIRE(eval.evaluate("$<TARGET_FILE_BASE_NAME:plain>").value() == "plain");
}

TEST_CASE("GenexEvaluator - TARGET_FILE_PREFIX", "[genex][evaluator]") {
    TargetMap targets;
    targets["mylib"] = std::make_shared<Target>("mylib", TargetType::STATIC_LIBRARY, "/src", "/build");
    targets["myshared"] = std::make_shared<Target>("myshared", TargetType::SHARED_LIBRARY, "/src", "/build");
    targets["myexe"] = std::make_shared<Target>("myexe", TargetType::EXECUTABLE, "/src", "/build");
    auto custom = std::make_shared<Target>("custom", TargetType::STATIC_LIBRARY, "/src", "/build");
    custom->set_property("PREFIX", "pfx_");
    targets["custom"] = custom;

    GenexEvaluationContext ctx;
    ctx.all_targets = &targets;
    ctx.static_library_prefix = "lib";
    ctx.static_library_suffix = ".a";
    ctx.shared_library_prefix = "lib";
    ctx.shared_library_suffix = ".so";
    GenexEvaluator eval(ctx);

    // CMake ref: "lib"
    REQUIRE(eval.evaluate("$<TARGET_FILE_PREFIX:mylib>").value() == "lib");
    // CMake ref: "lib"
    REQUIRE(eval.evaluate("$<TARGET_FILE_PREFIX:myshared>").value() == "lib");
    // CMake ref: ""
    REQUIRE(eval.evaluate("$<TARGET_FILE_PREFIX:myexe>").value() == "");
    // CMake ref: "pfx_" (per-target PREFIX property)
    REQUIRE(eval.evaluate("$<TARGET_FILE_PREFIX:custom>").value() == "pfx_");
}

TEST_CASE("GenexEvaluator - TARGET_FILE_SUFFIX", "[genex][evaluator]") {
    TargetMap targets;
    targets["mylib"] = std::make_shared<Target>("mylib", TargetType::STATIC_LIBRARY, "/src", "/build");
    targets["myshared"] = std::make_shared<Target>("myshared", TargetType::SHARED_LIBRARY, "/src", "/build");
    targets["myexe"] = std::make_shared<Target>("myexe", TargetType::EXECUTABLE, "/src", "/build");
    auto custom = std::make_shared<Target>("custom", TargetType::STATIC_LIBRARY, "/src", "/build");
    custom->set_property("SUFFIX", ".custom");
    targets["custom"] = custom;

    GenexEvaluationContext ctx;
    ctx.all_targets = &targets;
    ctx.static_library_prefix = "lib";
    ctx.static_library_suffix = ".a";
    ctx.shared_library_prefix = "lib";
    ctx.shared_library_suffix = ".so";
    GenexEvaluator eval(ctx);

    // CMake ref: ".a"
    REQUIRE(eval.evaluate("$<TARGET_FILE_SUFFIX:mylib>").value() == ".a");
    // CMake ref: ".so"
    REQUIRE(eval.evaluate("$<TARGET_FILE_SUFFIX:myshared>").value() == ".so");
    // CMake ref: ""
    REQUIRE(eval.evaluate("$<TARGET_FILE_SUFFIX:myexe>").value() == "");
    // CMake ref: ".custom" (per-target SUFFIX property)
    REQUIRE(eval.evaluate("$<TARGET_FILE_SUFFIX:custom>").value() == ".custom");
}

TEST_CASE("GenexEvaluator - TARGET_FILE_PREFIX respects CMAKE variables", "[genex][evaluator]") {
    TargetMap targets;
    targets["mylib"] = std::make_shared<Target>("mylib", TargetType::STATIC_LIBRARY, "/src", "/build");
    targets["myshared"] = std::make_shared<Target>("myshared", TargetType::SHARED_LIBRARY, "/src", "/build");

    GenexEvaluationContext ctx;
    ctx.all_targets = &targets;
    ctx.static_library_prefix = "xxx_";
    ctx.static_library_suffix = ".yyy";
    ctx.shared_library_prefix = "shared_";
    ctx.shared_library_suffix = ".dyn";
    GenexEvaluator eval(ctx);

    // CMake ref (with CMAKE_STATIC_LIBRARY_PREFIX=xxx_): "xxx_"
    REQUIRE(eval.evaluate("$<TARGET_FILE_PREFIX:mylib>").value() == "xxx_");
    // CMake ref (with CMAKE_STATIC_LIBRARY_SUFFIX=.yyy): ".yyy"
    REQUIRE(eval.evaluate("$<TARGET_FILE_SUFFIX:mylib>").value() == ".yyy");
    // CMake ref (with CMAKE_SHARED_LIBRARY_PREFIX=shared_): "shared_"
    REQUIRE(eval.evaluate("$<TARGET_FILE_PREFIX:myshared>").value() == "shared_");
    // CMake ref (with CMAKE_SHARED_LIBRARY_SUFFIX=.dyn): ".dyn"
    REQUIRE(eval.evaluate("$<TARGET_FILE_SUFFIX:myshared>").value() == ".dyn");
    // Linker variants should match
    REQUIRE(eval.evaluate("$<TARGET_LINKER_FILE_PREFIX:myshared>").value() == "shared_");
    REQUIRE(eval.evaluate("$<TARGET_LINKER_FILE_SUFFIX:myshared>").value() == ".dyn");
}

TEST_CASE("GenexEvaluator - TARGET_LINKER_FILE_PREFIX/SUFFIX", "[genex][evaluator]") {
    TargetMap targets;
    targets["mylib"] = std::make_shared<Target>("mylib", TargetType::STATIC_LIBRARY, "/src", "/build");
    targets["myshared"] = std::make_shared<Target>("myshared", TargetType::SHARED_LIBRARY, "/src", "/build");

    GenexEvaluationContext ctx;
    ctx.all_targets = &targets;
    ctx.static_library_prefix = "lib";
    ctx.static_library_suffix = ".a";
    ctx.shared_library_prefix = "lib";
    ctx.shared_library_suffix = ".so";
    GenexEvaluator eval(ctx);

    // CMake ref: "lib"
    REQUIRE(eval.evaluate("$<TARGET_LINKER_FILE_PREFIX:mylib>").value() == "lib");
    REQUIRE(eval.evaluate("$<TARGET_LINKER_FILE_PREFIX:myshared>").value() == "lib");
    // CMake ref: ".a"
    REQUIRE(eval.evaluate("$<TARGET_LINKER_FILE_SUFFIX:mylib>").value() == ".a");
    // CMake ref: ".so"
    REQUIRE(eval.evaluate("$<TARGET_LINKER_FILE_SUFFIX:myshared>").value() == ".so");
}

TEST_CASE("GenexEvaluator - literal boolean conditionals", "[genex][evaluator]") {
    GenexEvaluationContext ctx;
    GenexEvaluator eval(ctx);

    // CMake ref: "visible"
    REQUIRE(eval.evaluate("$<1:visible>").value() == "visible");
    // CMake ref: ""
    REQUIRE(eval.evaluate("$<0:hidden>").value() == "");
    // CMake ref: ""
    REQUIRE(eval.evaluate("$<1:>").value() == "");
    // CMake ref: ""
    REQUIRE(eval.evaluate("$<0:>").value() == "");
}

TEST_CASE("GenexEvaluator - nested composition", "[genex][evaluator]") {
    GenexEvaluationContext ctx;
    ctx.build_type = "Debug";
    GenexEvaluator eval(ctx);

    // CMake ref: "hello"
    REQUIRE(eval.evaluate("$<$<BOOL:ON>:$<LOWER_CASE:HELLO>>").value() == "hello");
    // CMake ref: "found"
    REQUIRE(eval.evaluate("$<$<IN_LIST:b,a;b;c>:found>").value() == "found");
    // CMake ref: ""
    REQUIRE(eval.evaluate("$<$<IN_LIST:x,a;b;c>:found>").value() == "");
    // CMake ref: "YES"
    REQUIRE(eval.evaluate("$<IF:1,$<UPPER_CASE:yes>,$<UPPER_CASE:no>>").value() == "YES");
    // CMake ref: "a b"
    REQUIRE(eval.evaluate("$<JOIN:$<REMOVE_DUPLICATES:a;b;a>, >").value() == "a b");
}

TEST_CASE("GenexParser - new types classification", "[genex][parser]") {
    GenexParser parser;

    SECTION("LOWER_CASE") {
        auto result = parser.parse("$<LOWER_CASE:Hello>");
        REQUIRE(result.has_value());
        REQUIRE(result->nodes.size() == 1);
        REQUIRE(result->nodes[0]->type == GenexNodeType::LOWER_CASE);
    }

    SECTION("UPPER_CASE") {
        auto result = parser.parse("$<UPPER_CASE:Hello>");
        REQUIRE(result.has_value());
        REQUIRE(result->nodes.size() == 1);
        REQUIRE(result->nodes[0]->type == GenexNodeType::UPPER_CASE);
    }

    SECTION("JOIN") {
        auto result = parser.parse("$<JOIN:a;b,->");
        REQUIRE(result.has_value());
        REQUIRE(result->nodes.size() == 1);
        REQUIRE(result->nodes[0]->type == GenexNodeType::JOIN);
    }

    SECTION("REMOVE_DUPLICATES") {
        auto result = parser.parse("$<REMOVE_DUPLICATES:a;b;a>");
        REQUIRE(result.has_value());
        REQUIRE(result->nodes.size() == 1);
        REQUIRE(result->nodes[0]->type == GenexNodeType::REMOVE_DUPLICATES);
    }

    SECTION("FILTER") {
        auto result = parser.parse("$<FILTER:a;b;c,INCLUDE,.*>");
        REQUIRE(result.has_value());
        REQUIRE(result->nodes.size() == 1);
        REQUIRE(result->nodes[0]->type == GenexNodeType::FILTER);
    }

    SECTION("IN_LIST") {
        auto result = parser.parse("$<IN_LIST:x,a;b>");
        REQUIRE(result.has_value());
        REQUIRE(result->nodes.size() == 1);
        REQUIRE(result->nodes[0]->type == GenexNodeType::IN_LIST);
    }

    SECTION("GENEX_EVAL") {
        auto result = parser.parse("$<GENEX_EVAL:hello>");
        REQUIRE(result.has_value());
        REQUIRE(result->nodes.size() == 1);
        REQUIRE(result->nodes[0]->type == GenexNodeType::GENEX_EVAL);
    }

    SECTION("TARGET_GENEX_EVAL") {
        auto result = parser.parse("$<TARGET_GENEX_EVAL:tgt,expr>");
        REQUIRE(result.has_value());
        REQUIRE(result->nodes.size() == 1);
        REQUIRE(result->nodes[0]->type == GenexNodeType::TARGET_GENEX_EVAL);
    }

    SECTION("TARGET_FILE_BASE_NAME") {
        auto result = parser.parse("$<TARGET_FILE_BASE_NAME:tgt>");
        REQUIRE(result.has_value());
        REQUIRE(result->nodes.size() == 1);
        REQUIRE(result->nodes[0]->type == GenexNodeType::TARGET_FILE_BASE_NAME);
    }

    SECTION("TARGET_FILE_PREFIX") {
        auto result = parser.parse("$<TARGET_FILE_PREFIX:tgt>");
        REQUIRE(result.has_value());
        REQUIRE(result->nodes.size() == 1);
        REQUIRE(result->nodes[0]->type == GenexNodeType::TARGET_FILE_PREFIX);
    }

    SECTION("TARGET_FILE_SUFFIX") {
        auto result = parser.parse("$<TARGET_FILE_SUFFIX:tgt>");
        REQUIRE(result.has_value());
        REQUIRE(result->nodes.size() == 1);
        REQUIRE(result->nodes[0]->type == GenexNodeType::TARGET_FILE_SUFFIX);
    }

    SECTION("TARGET_LINKER_FILE_PREFIX") {
        auto result = parser.parse("$<TARGET_LINKER_FILE_PREFIX:tgt>");
        REQUIRE(result.has_value());
        REQUIRE(result->nodes.size() == 1);
        REQUIRE(result->nodes[0]->type == GenexNodeType::TARGET_LINKER_FILE_PREFIX);
    }

    SECTION("TARGET_LINKER_FILE_SUFFIX") {
        auto result = parser.parse("$<TARGET_LINKER_FILE_SUFFIX:tgt>");
        REQUIRE(result.has_value());
        REQUIRE(result->nodes.size() == 1);
        REQUIRE(result->nodes[0]->type == GenexNodeType::TARGET_LINKER_FILE_SUFFIX);
    }

    SECTION("TARGET_LINKER_FILE_BASE_NAME") {
        auto result = parser.parse("$<TARGET_LINKER_FILE_BASE_NAME:tgt>");
        REQUIRE(result.has_value());
        REQUIRE(result->nodes.size() == 1);
        REQUIRE(result->nodes[0]->type == GenexNodeType::TARGET_LINKER_FILE_BASE_NAME);
    }

    SECTION("INSTALL_PREFIX") {
        auto result = parser.parse("$<INSTALL_PREFIX>");
        REQUIRE(result.has_value());
        REQUIRE(result->nodes.size() == 1);
        REQUIRE(result->nodes[0]->type == GenexNodeType::INSTALL_PREFIX);
    }
}

// ============================================================================
// CMake-verified edge cases: bare '<', escapes, ANGLE-R, COMMA, SEMICOLON
// Reference values captured from CMake 4.0.1 with file(GENERATE)
// ============================================================================

TEST_CASE("GenexEvaluator - constant literal escapes", "[genex][evaluator][escapes]") {
    GenexEvaluationContext ctx;
    GenexEvaluator eval(ctx);

    // CMake ref: ">"
    REQUIRE(eval.evaluate("$<ANGLE-R>").value() == ">");
    // CMake ref: ","
    REQUIRE(eval.evaluate("$<COMMA>").value() == ",");
    // CMake ref: ";"
    REQUIRE(eval.evaluate("$<SEMICOLON>").value() == ";");
    // CMake ref: "\""
    REQUIRE(eval.evaluate("$<QUOTE>").value() == "\"");
    // CMake ref: ">>>"
    REQUIRE(eval.evaluate("$<ANGLE-R>$<ANGLE-R>$<ANGLE-R>").value() == ">>>");
}

TEST_CASE("GenexEvaluator - STREQUAL with escape genexes", "[genex][evaluator][escapes]") {
    GenexEvaluationContext ctx;
    GenexEvaluator eval(ctx);

    // CMake ref: "1"
    REQUIRE(eval.evaluate("$<STREQUAL:$<ANGLE-R>,$<ANGLE-R>>").value() == "1");
    // CMake ref: "0"
    REQUIRE(eval.evaluate("$<STREQUAL:$<ANGLE-R>,x>").value() == "0");
    // CMake ref: "1"
    REQUIRE(eval.evaluate("$<STREQUAL:$<COMMA>,$<COMMA>>").value() == "1");
    // CMake ref: "0"
    REQUIRE(eval.evaluate("$<STREQUAL:$<COMMA>,x>").value() == "0");
}

TEST_CASE("GenexEvaluator - bare < in genex content", "[genex][evaluator][bare-angle]") {
    GenexEvaluationContext ctx;
    GenexEvaluator eval(ctx);

    // Bare '<' is just text — does NOT affect nesting depth.
    // CMake ref: "1" ($<STREQUAL:a<b,a<b>)
    REQUIRE(eval.evaluate("$<STREQUAL:a<b,a<b>").value() == "1");
    // CMake ref: "0" ($<STREQUAL:a<b,a>)
    REQUIRE(eval.evaluate("$<STREQUAL:a<b,a>").value() == "0");
    // CMake ref: "a<b" ($<LOWER_CASE:A<B>)
    REQUIRE(eval.evaluate("$<LOWER_CASE:A<B>").value() == "a<b");
    // CMake ref: "A<B" ($<UPPER_CASE:a<b>)
    REQUIRE(eval.evaluate("$<UPPER_CASE:a<b>").value() == "A<B");
    // CMake ref: "a<b<c" ($<LOWER_CASE:A<B<C>)
    REQUIRE(eval.evaluate("$<LOWER_CASE:A<B<C>").value() == "a<b<c");
    // CMake ref: "1" ($<BOOL:a<b>)
    REQUIRE(eval.evaluate("$<BOOL:a<b>").value() == "1");
}

TEST_CASE("GenexEvaluator - bare < in IF/conditional", "[genex][evaluator][bare-angle]") {
    GenexEvaluationContext ctx;
    GenexEvaluator eval(ctx);

    // CMake ref: "a<b" ($<IF:1,a<b,c>)
    REQUIRE(eval.evaluate("$<IF:1,a<b,c>").value() == "a<b");
    // CMake ref: "c" ($<IF:0,a<b,c>)
    REQUIRE(eval.evaluate("$<IF:0,a<b,c>").value() == "c");
}

TEST_CASE("GenexEvaluator - ANGLE-R in nested contexts", "[genex][evaluator][escapes]") {
    GenexEvaluationContext ctx;
    GenexEvaluator eval(ctx);

    // CMake ref: ">" ($<IF:1,$<ANGLE-R>,no>)
    REQUIRE(eval.evaluate("$<IF:1,$<ANGLE-R>,no>").value() == ">");
    // CMake ref: "," ($<IF:1,$<COMMA>,no>)
    REQUIRE(eval.evaluate("$<IF:1,$<COMMA>,no>").value() == ",");
    // CMake ref: ">" ($<$<BOOL:ON>:$<ANGLE-R>>)
    REQUIRE(eval.evaluate("$<$<BOOL:ON>:$<ANGLE-R>>").value() == ">");
    // CMake ref: "a,b" ($<$<BOOL:ON>:a$<COMMA>b>)
    REQUIRE(eval.evaluate("$<$<BOOL:ON>:a$<COMMA>b>").value() == "a,b");
    // CMake ref: "a;b" ($<$<BOOL:ON>:a$<SEMICOLON>b>)
    REQUIRE(eval.evaluate("$<$<BOOL:ON>:a$<SEMICOLON>b>").value() == "a;b");
    // CMake ref: "before>" ($<$<BOOL:ON>:before$<ANGLE-R>>)
    REQUIRE(eval.evaluate("$<$<BOOL:ON>:before$<ANGLE-R>>").value() == "before>");
}

TEST_CASE("GenexEvaluator - STREQUAL bare > closes genex early (CMake compat)", "[genex][evaluator][escapes]") {
    GenexEvaluationContext ctx;
    GenexEvaluator eval(ctx);

    // In CMake: $<STREQUAL:a$<ANGLE-R>b,a>b> evaluates as:
    //   STREQUAL("a>b", "a") => "0", then literal "b>" appended
    // CMake ref: "0b>"
    REQUIRE(eval.evaluate("$<STREQUAL:a$<ANGLE-R>b,a>b>").value() == "0b>");
}

TEST_CASE("GenexParser - bare < does not break parsing", "[genex][parser][bare-angle]") {
    GenexParser parser;

    SECTION("bare < in STREQUAL content") {
        auto result = parser.parse("$<STREQUAL:a<b,a<b>");
        REQUIRE(result.has_value());
        REQUIRE(result->has_genex == true);
        REQUIRE(result->nodes[0]->type == GenexNodeType::STREQUAL);
    }

    SECTION("bare < in LOWER_CASE") {
        auto result = parser.parse("$<LOWER_CASE:A<B>");
        REQUIRE(result.has_value());
        REQUIRE(result->has_genex == true);
        REQUIRE(result->nodes[0]->type == GenexNodeType::LOWER_CASE);
    }

    SECTION("multiple bare < in content") {
        auto result = parser.parse("$<LOWER_CASE:A<B<C>");
        REQUIRE(result.has_value());
        REQUIRE(result->has_genex == true);
    }

    SECTION("bare < in IF args") {
        auto result = parser.parse("$<IF:1,a<b,c>");
        REQUIRE(result.has_value());
        REQUIRE(result->has_genex == true);
        REQUIRE(result->nodes[0]->type == GenexNodeType::IF);
    }

    SECTION("bare < in BOOL") {
        auto result = parser.parse("$<BOOL:a<b>");
        REQUIRE(result.has_value());
        REQUIRE(result->has_genex == true);
        REQUIRE(result->nodes[0]->type == GenexNodeType::BOOL);
    }
}

TEST_CASE("GenexParser - split_genex_args with bare <", "[genex][parser][bare-angle]") {
    GenexParser parser;

    // Bare '<' should NOT prevent comma splitting
    auto args = parser.split_genex_args("a<b,c");
    REQUIRE(args.size() == 2);
    REQUIRE(args[0] == "a<b");
    REQUIRE(args[1] == "c");

    // But $< ... > should still protect commas
    args = parser.split_genex_args("$<COMMA>,c");
    REQUIRE(args.size() == 2);
    REQUIRE(args[0] == "$<COMMA>");
    REQUIRE(args[1] == "c");

    // Nested genex protects commas
    args = parser.split_genex_args("$<IF:1,a,b>,c");
    REQUIRE(args.size() == 2);
    REQUIRE(args[0] == "$<IF:1,a,b>");
    REQUIRE(args[1] == "c");
}

// ============================================================================
// Recovery mode: unmatched $< degrades to literal text (CMake compat)
// CMake handles structurally-unbalanced $</$> (e.g. from $<ANGLE-R>) via
// recovery: the unmatched $< becomes literal "$<" while inner genex evaluate.
// ============================================================================

TEST_CASE("GenexParser - recovery mode degrades unmatched $< to literal", "[genex][parser][recovery]") {
    SECTION("strict mode (default) errors on unmatched $<") {
        GenexParser parser;
        auto result = parser.parse("$<CONFIG:Debug");
        REQUIRE(!result.has_value());
    }

    SECTION("recovery mode treats unmatched $< as literal") {
        GenexParser parser;
        parser.set_recovery(true);
        auto result = parser.parse("$<CONFIG:Debug");
        REQUIRE(result.has_value());
        REQUIRE(result->has_genex == false);
        REQUIRE(result->nodes.size() == 1);
        REQUIRE(result->nodes[0]->type == GenexNodeType::LITERAL);
        REQUIRE(result->nodes[0]->raw_content == "$<CONFIG:Debug");
    }

    SECTION("recovery: $<SEMICOLON$<ANGLE-R>rest") {
        // $< starts, SEMICOLON is not a valid keyword with content "$<ANGLE-R>rest",
        // but parse_genex sees "SEMICOLON$<ANGLE-R" as keyword (up to first >),
        // the outer $< is unbalanced → recovers to literal "$<", then SEMICOLON is text,
        // then $<ANGLE-R> evaluates to ">", then "rest" is literal
        GenexParser parser;
        parser.set_recovery(true);
        auto result = parser.parse("$<SEMICOLON$<ANGLE-R>rest");
        REQUIRE(result.has_value());
    }
}

TEST_CASE("GenexEvaluator - recovery: unmatched $< with ANGLE-R", "[genex][evaluator][recovery]") {
    GenexEvaluationContext ctx;
    GenexEvaluator eval(ctx);

    SECTION("R1: $<SEMICOLON$<ANGLE-R>rest → $<SEMICOLON>rest") {
        // Outer $< fails (SEMICOLON$<ANGLE-R is not a valid genex keyword with balanced content)
        // → "$<" literal, then "SEMICOLON" text, then $<ANGLE-R>→">" , then "rest"
        auto result = eval.evaluate("$<SEMICOLON$<ANGLE-R>rest");
        REQUIRE(result.has_value());
        REQUIRE(*result == "$<SEMICOLON>rest");
    }

    SECTION("R2: $<$<BOOL:1$<ANGLE-R>:yes$<ANGLE-R> → $<$<BOOL:1>:yes>") {
        // Both outer $< recover to literal, ANGLE-R genexes evaluate to >
        auto result = eval.evaluate("$<$<BOOL:1$<ANGLE-R>:yes$<ANGLE-R>");
        REQUIRE(result.has_value());
        REQUIRE(*result == "$<$<BOOL:1>:yes>");
    }

    SECTION("constant literals with ANGLE-R still work normally") {
        // $<ANGLE-R> → ">"
        REQUIRE(eval.evaluate("a$<SEMICOLON>b$<COMMA>c$<ANGLE-R>d").value() == "a;b,c>d");
    }

    SECTION("normal nested genex still evaluates") {
        REQUIRE(eval.evaluate("$<$<BOOL:1>:yes>").value() == "yes");
        REQUIRE(eval.evaluate("$<$<BOOL:0>:no>").value() == "");
        REQUIRE(eval.evaluate("$<$<BOOL:>:no>").value() == "");
        REQUIRE(eval.evaluate("$<IF:$<BOOL:1>,yes,no>").value() == "yes");
        REQUIRE(eval.evaluate("$<$<NOT:$<BOOL:>>:empty_is_false>").value() == "empty_is_false");
    }

    SECTION("ANGLE-R in evaluated content") {
        REQUIRE(eval.evaluate("$<$<BOOL:1>:a$<ANGLE-R>b>").value() == "a>b");
        REQUIRE(eval.evaluate("$<JOIN:a;b;c,$<COMMA>>").value() == "a,b,c");
        REQUIRE(eval.evaluate("$<1:included>$<0:excluded>").value() == "included");
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
        REQUIRE(txn.add(std::move(task)));
        REQUIRE(txn.commit());

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
        REQUIRE(txn.add(std::move(task)));
        REQUIRE(txn.commit());

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
        REQUIRE(txn.add(std::move(task)));
        REQUIRE(txn.commit());

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
        REQUIRE(txn.add(std::move(task)));
        REQUIRE(txn.commit());

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
        REQUIRE(txn.add(std::move(task)));
        REQUIRE(txn.commit());

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
        task.commands = {{"echo", "$<$<CONFIG:Debug>:cmd1>"}, {"echo", "$<$<CONFIG:Debug>:cmd2>"}};

        auto txn = graph.begin();
        REQUIRE(txn.add(std::move(task)));
        REQUIRE(txn.commit());

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

    SECTION("inputs with genex are evaluated") {
        BuildTask task;
        task.id = "test_task";
        task.commands = {{"echo", "hello"}};
        task.inputs = {"$<$<CONFIG:Debug>:/path/to/debug_dep>", "/always_dep"};

        auto txn = graph.begin();
        REQUIRE(txn.add(std::move(task)));
        REQUIRE(txn.commit());

        GenexEvaluationContext ctx;
        ctx.build_type = "Debug";
        ctx.phase = GenexEvaluationContext::Phase::BUILD;

        auto result = graph.evaluate_genex(ctx);
        REQUIRE(result.has_value());

        auto& finalized = graph.get_task("test_task");
        REQUIRE(finalized.inputs.size() == 2);
        REQUIRE(finalized.inputs[0] == "/path/to/debug_dep");
        REQUIRE(finalized.inputs[1] == "/always_dep");
    }

    SECTION("inputs - empty genex results are dropped") {
        BuildTask task;
        task.id = "test_task";
        task.commands = {{"echo", "hello"}};
        task.inputs = {"$<$<CONFIG:Release>:/release_dep>", "/always_dep"};

        auto txn = graph.begin();
        REQUIRE(txn.add(std::move(task)));
        REQUIRE(txn.commit());

        GenexEvaluationContext ctx;
        ctx.build_type = "Debug";
        ctx.phase = GenexEvaluationContext::Phase::BUILD;

        auto result = graph.evaluate_genex(ctx);
        REQUIRE(result.has_value());

        auto& finalized = graph.get_task("test_task");
        REQUIRE(finalized.inputs.size() == 1);
        REQUIRE(finalized.inputs[0] == "/always_dep");
    }
}

TEST_CASE("GenexEvaluator - TARGET_PROPERTY 1-arg with current_target", "[genex][evaluator]") {
    TargetMap targets;
    auto mylib = std::make_shared<Target>("mylib", TargetType::STATIC_LIBRARY, "/src", "/build");
    mylib->set_property("MY_CUSTOM_PROP", "hello_world");
    targets["mylib"] = mylib;

    GenexEvaluationContext ctx;
    ctx.all_targets = &targets;
    ctx.current_target = mylib.get();
    GenexEvaluator eval(ctx);

    // 1-arg form uses current_target
    auto result = eval.evaluate("$<TARGET_PROPERTY:MY_CUSTOM_PROP>");
    REQUIRE(result.has_value());
    REQUIRE(*result == "hello_world");
}

TEST_CASE("GenexEvaluator - TARGET_PROPERTY 1-arg without current_target errors", "[genex][evaluator]") {
    TargetMap targets;
    auto mylib = std::make_shared<Target>("mylib", TargetType::STATIC_LIBRARY, "/src", "/build");
    targets["mylib"] = mylib;

    GenexEvaluationContext ctx;
    ctx.all_targets = &targets;
    // current_target deliberately not set
    GenexEvaluator eval(ctx);

    auto result = eval.evaluate("$<TARGET_PROPERTY:MY_CUSTOM_PROP>");
    REQUIRE(!result.has_value());
    REQUIRE(result.error().find("requires a target context") != std::string::npos);
}

TEST_CASE("GenexEvaluator - TARGET_PROPERTY built-in pseudo-properties", "[genex][evaluator]") {
    TargetMap targets;
    auto mylib = std::make_shared<Target>("mylib", TargetType::STATIC_LIBRARY, "/mysrc", "/mybuild");
    targets["mylib"] = mylib;

    GenexEvaluationContext ctx;
    ctx.all_targets = &targets;
    ctx.current_target = mylib.get();
    GenexEvaluator eval(ctx);

    // 1-arg form for built-in properties
    REQUIRE(eval.evaluate("$<TARGET_PROPERTY:NAME>").value() == "mylib");
    REQUIRE(eval.evaluate("$<TARGET_PROPERTY:SOURCE_DIR>").value() == "/mysrc");
    REQUIRE(eval.evaluate("$<TARGET_PROPERTY:BINARY_DIR>").value() == "/mybuild");
    REQUIRE(eval.evaluate("$<TARGET_PROPERTY:IMPORTED>").value() == "FALSE");

    // 2-arg form for built-in properties
    REQUIRE(eval.evaluate("$<TARGET_PROPERTY:mylib,NAME>").value() == "mylib");
    REQUIRE(eval.evaluate("$<TARGET_PROPERTY:mylib,SOURCE_DIR>").value() == "/mysrc");
    REQUIRE(eval.evaluate("$<TARGET_PROPERTY:mylib,BINARY_DIR>").value() == "/mybuild");
    REQUIRE(eval.evaluate("$<TARGET_PROPERTY:mylib,IMPORTED>").value() == "FALSE");
}

// Monkey test: throw a large randomized corpus of generator expressions at the
// evaluator and assert two invariants:
//   1. The evaluator never throws / aborts. It must return std::expected.
//   2. On success, no '$<' remains in the result (anything left would mean we
//      pretended to evaluate while leaking unhandled genex into a build path).
//
// Deterministic by design: a fixed seed makes failures reproducible. A failing
// input is reported via INFO so the seed alone reconstructs the case.
TEST_CASE("GenexEvaluator - monkey test", "[genex][evaluator][monkey]") {
    using std::string;

    // Atomic patterns spanning every supported genex node. Each is independently
    // valid against the test context built below.
    const std::vector<string> atoms = {
        "literal_text",
        "$<CONFIG:Debug>",
        "$<CONFIG:Release>",
        "$<CONFIG>",
        "$<CONFIGURATION>",
        "$<BOOL:1>",
        "$<BOOL:0>",
        "$<BOOL:>",
        "$<BOOL:TRUE>",
        "$<NOT:1>",
        "$<NOT:0>",
        "$<AND:1,1,0>",
        "$<AND:1,1>",
        "$<OR:0,0,1>",
        "$<OR:0,0>",
        "$<IF:1,yes,no>",
        "$<IF:0,yes,no>",
        "$<STREQUAL:foo,foo>",
        "$<STREQUAL:foo,bar>",
        "$<EQUAL:1,1>",
        "$<EQUAL:1,2>",
        "$<VERSION_LESS:1.0,2.0>",
        "$<VERSION_GREATER:2.0,1.0>",
        "$<VERSION_EQUAL:1.0,1.0>",
        "$<LOWER_CASE:HELLO>",
        "$<UPPER_CASE:hello>",
        "$<BUILD_INTERFACE:/abs/include>",
        "$<INSTALL_INTERFACE:include>",
        "$<LINK_ONLY:foo>",
        "$<COMPILE_LANGUAGE:CXX>",
        "$<CXX_COMPILER_ID:GNU>",
        "$<C_COMPILER_ID:GNU>",
        "$<TARGET_EXISTS:mylib>",
        "$<TARGET_EXISTS:nope>",
        "$<TARGET_PROPERTY:mylib,NAME>",
        "$<TARGET_PROPERTY:mylib,SOURCE_DIR>",
        "$<INSTALL_PREFIX>",
        "$<COMMA>",
        "$<SEMICOLON>",
        "$<ANGLE-R>",
    };

    // Wrappers that take one inner expression. Used to build nested cases.
    const std::vector<string> wrappers_unary = {
        "$<NOT:{}>",
        "$<BOOL:{}>",
        "$<LOWER_CASE:{}>",
        "$<UPPER_CASE:{}>",
        "$<BUILD_INTERFACE:{}>",
        "$<INSTALL_INTERFACE:{}>",
        "prefix-{}-suffix", // genex embedded in literal context
        "{}",               // identity
    };

    auto substitute = [](string tmpl, const string& inner) -> string {
        auto pos = tmpl.find("{}");
        if (pos == string::npos) return tmpl;
        return tmpl.replace(pos, 2, inner);
    };

    // Evaluation context: representative state covering the atoms above.
    TargetMap targets;
    auto mylib = std::make_shared<Target>("mylib", TargetType::STATIC_LIBRARY, "/src", "/bld");
    targets["mylib"] = mylib;
    GenexEvaluationContext ctx;
    ctx.build_type = "Debug";
    ctx.cxx_compiler_id = "GNU";
    ctx.c_compiler_id = "GNU";
    ctx.compile_language = Language::CXX;
    ctx.system_name = "Linux";
    ctx.install_prefix = "/usr/local";
    ctx.all_targets = &targets;
    ctx.current_target = mylib.get();
    GenexEvaluator eval(ctx);

    // Seeded RNG: change the seed via the env var KILN_GENEX_MONKEY_SEED to
    // reproduce a CI failure locally.
    uint64_t seed = 0xC0FFEEULL;
    if (const char* s = std::getenv("KILN_GENEX_MONKEY_SEED")) {
        try {
            seed = std::stoull(s);
        } catch (...) {}
    }
    std::mt19937_64 rng(seed);

    auto pick = [&](const auto& v) -> const string& { return v[std::uniform_int_distribution<size_t>(0, v.size() - 1)(rng)]; };

    // Generate `iterations` random expressions, nesting up to max_depth wrappers
    // around a randomly-chosen atom.
    constexpr int iterations = 2000;
    constexpr int max_depth = 4;
    int errors = 0;
    int leaks = 0;
    for (int i = 0; i < iterations; ++i) {
        string expr = pick(atoms);
        int depth = std::uniform_int_distribution<int>(0, max_depth)(rng);
        for (int d = 0; d < depth; ++d) { expr = substitute(pick(wrappers_unary), expr); }

        std::expected<string, string> result;
        try {
            result = eval.evaluate(expr);
        } catch (const std::exception& e) {
            INFO("seed=" << seed << " iter=" << i << " expr=" << expr << " threw: " << e.what());
            FAIL("evaluator threw on monkey input");
        } catch (...) {
            INFO("seed=" << seed << " iter=" << i << " expr=" << expr);
            FAIL("evaluator threw unknown exception on monkey input");
        }

        if (!result.has_value()) {
            // Errors are acceptable (some random nesting may be ill-typed),
            // but the error string must be non-empty for diagnostics.
            INFO("seed=" << seed << " iter=" << i << " expr=" << expr);
            REQUIRE_FALSE(result.error().empty());
            ++errors;
            continue;
        }

        if (result->find("$<") != string::npos) {
            INFO("seed=" << seed << " iter=" << i << " expr=" << expr << " result=" << *result);
            FAIL("evaluator returned success but result contains unevaluated genex");
            ++leaks;
        }
    }

    // Sanity: with this corpus we expect a healthy mix — most should succeed.
    // If everything errors, the test isn't actually exercising the evaluator.
    REQUIRE(errors < iterations);
    REQUIRE(leaks == 0);
}
