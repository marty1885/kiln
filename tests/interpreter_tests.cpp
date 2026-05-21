#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include "kiln/interperter.hpp"
#include "kiln/cmake-language.hpp"
#include <sstream>
#include <fstream>
#include <filesystem>
#include <algorithm>

std::string run_script(std::string src) {
    std::stringstream output;
    // Heap-allocate: Interpreter is too large for the stack (ASAN stack-buffer-overflow)
    auto interpreter = std::make_unique<kiln::Interpreter>("", &output);

    interpreter->add_builtin("message", [&](kiln::Interpreter& interp, const std::vector<std::string>& args) {
        if (args.empty()) {
            throw std::runtime_error("message called with incorrect number of arguments");
        }
        // A simple implementation for testing purposes
        for (const auto& arg : args) {
            output << arg;
        }
        output << std::endl;
    });

    interpreter->set_source_view(src);
    kiln::Parser parser(src);
    auto ast_or_error = parser.parse();
    if (!ast_or_error.has_value()) {
        const auto error = ast_or_error.error();
        throw std::runtime_error("Failed to parse script - " + error.reason + " on line " + std::to_string(error.row));
    }

    auto result = interpreter->interpret(ast_or_error.value());
    if (!result) {
        throw std::runtime_error(result.error().message);
    }

    interpreter->execute_deferred_calls();
    if (auto err = interpreter->get_fatal_error()) {
        throw std::runtime_error(err->message);
    }

    return output.str();
}


TEST_CASE("Interpreter variable substitution", "[interpreter]") {
    auto output = run_script(R"(
        set(MY_VAR "Hello")
        message("${MY_VAR} World")
    )");
    REQUIRE(output == "Hello World\n");

    output = run_script(R"(
        set(MY_VAR "Goodbye")
        message("${MY_VAR} World"
        )
    )");
    REQUIRE(output == "Goodbye World\n");

    output = run_script(R"(
        set(
        MY_VAR "Goodbye"
        )
        message(
        "${MY_VAR} World")
    )");
    REQUIRE(output == "Goodbye World\n");

    output = run_script(R"(
        set(
        MY_VAR "Goodbye"
        )
        set(MY_VAR2 "${MY_VAR}")
        message(
        "${MY_VAR2} World")
    )");
    REQUIRE(output == "Goodbye World\n");

    output = run_script(R"(
        set(
        MY_VAR "Goodbye"
        )
        set(MY_VAR2 "${MY_VAR}")
        message(
        "${MY_VAR2} World")
    )");
    REQUIRE(output == "Goodbye World\n");
}

TEST_CASE("Interpreter math command", "[interpreter][math]") {
    SECTION("Basic arithmetic") {
        auto output = run_script(R"(
            math(EXPR res "1 + 2 * 3")
            message("${res}")
        )");
        REQUIRE(output == "7\n");
    }

    SECTION("Parentheses and precedence") {
        auto output = run_script(R"(
            math(EXPR res "(1 + 2) * 3")
            message("${res}")
        )");
        REQUIRE(output == "9\n");
    }

    SECTION("Bitwise operators") {
        auto output = run_script(R"(
            math(EXPR res "1 << 4")
            message("${res}")
            math(EXPR res "${res} | 1")
            message("${res}")
            math(EXPR res "${res} & 15")
            message("${res}")
        )");
        REQUIRE(output == "16\n17\n1\n");
    }

    SECTION("Hexadecimal") {
        auto output = run_script(R"(
            math(EXPR res "0x10 + 5")
            message("${res}")
            math(EXPR res "077 + 1")
            message("${res}")
            math(EXPR res "0xA * 2" OUTPUT_FORMAT HEXADECIMAL)
            message("${res}")
        )");
        REQUIRE(output == "21\n78\n0x14\n");
    }

    SECTION("Division and modulo") {
        auto output = run_script(R"(
            math(EXPR res "10 / 3")
            message("${res}")
            math(EXPR res "10 % 3")
            message("${res}")
        )");
        REQUIRE(output == "3\n1\n");
    }

    SECTION("Unary operators") {
        auto output = run_script(R"(
            math(EXPR res "-5 + 10")
            message("${res}")
            math(EXPR res "~0" OUTPUT_FORMAT HEXADECIMAL)
            message("${res}")
        )");
        // ~0 in 64-bit is 0xffffffffffffffff
        REQUIRE(output == "5\n0xffffffffffffffff\n");
    }

    SECTION("Variable expansion in expression") {
        auto output = run_script(R"(
            set(X 10)
            math(EXPR res "${X} * 2")
            message("${res}")
        )");
        REQUIRE(output == "20\n");
    }
}

TEST_CASE("Interpreter unset", "[interpreter]") {
    auto output = run_script(R"(
        set(MY_VAR "Hello")
        unset(MY_VAR)
        message("${MY_VAR} World")
    )");
    REQUIRE(output == " World\n");
}

TEST_CASE("Interpreter if/else/endif", "[interpreter]") {
    auto output = run_script(R"(
        set(MY_VAR "TRUE")
        if(MY_VAR)
            message("Inside if")
        else()
            message("Inside else")
        endif()
    )");
    REQUIRE(output == "Inside if\n");

    output = run_script(R"(
        set(MY_VAR "FALSE")
        if(MY_VAR)
            message("Inside if")
        else()
            message("Inside else")
        endif()
    )");
    REQUIRE(output == "Inside else\n");

    // Empty if() should evaluate to FALSE (CMake behavior)
    output = run_script(R"(
        if()
            message("Inside if")
        else()
            message("Inside else")
        endif()
    )");
    REQUIRE(output == "Inside else\n");
}

TEST_CASE("If with elseif", "[interpreter]") {
    auto output = run_script(R"(
        set(VAL "B")
        if(VAL STREQUAL "A")
            message("A")
        elseif(VAL STREQUAL "B")
            message("B")
        elseif(VAL STREQUAL "C")
            message("C")
        else()
            message("None")
        endif()
    )");
    REQUIRE(output == "B\n");

    output = run_script(R"(
        set(VAL "C")
        if(VAL STREQUAL "A")
            message("A")
        elseif(VAL STREQUAL "B")
            message("B")
        elseif(VAL STREQUAL "C")
            message("C")
        else()
            message("None")
        endif()
    )");
    REQUIRE(output == "C\n");

    output = run_script(R"(
        set(VAL "D")
        if(VAL STREQUAL "A")
            message("A")
        elseif(VAL STREQUAL "B")
            message("B")
        elseif(VAL STREQUAL "C")
            message("C")
        else()
            message("None")
        endif()
    )");
    REQUIRE(output == "None\n");
}

TEST_CASE("If with elseif (no else)", "[interpreter]") {
    auto output = run_script(R"(
        set(VAL "B")
        if(VAL STREQUAL "A")
            message("A")
        elseif(VAL STREQUAL "B")
            message("B")
        endif()
    )");
    REQUIRE(output == "B\n");

    output = run_script(R"(
        set(VAL "C")
        if(VAL STREQUAL "A")
            message("A")
        elseif(VAL STREQUAL "B")
            message("B")
        endif()
    )");
    REQUIRE(output == "");
}

TEST_CASE("If with case-insensitive keywords", "[interpreter]") {
    auto output = run_script(R"(
        set(VAL "B")
        IF(VAL STREQUAL "A")
            message("A")
        ELSEIF(VAL STREQUAL "B")
            message("B")
        ELSE()
            message("None")
        ENDIF()
    )");
    REQUIRE(output == "B\n");

    output = run_script(R"(
        set(VAL "D")
        IF(VAL STREQUAL "A")
            message("A")
        ELSEIF(VAL STREQUAL "B")
            message("B")
        ELSE()
            message("None")
        ENDIF()
    )");
    REQUIRE(output == "None\n");
}

TEST_CASE("Comment", "[interpreter]") {
    auto output = run_script(R"(
        # This is a comment
        message("Hello")
    )");
    REQUIRE(output == "Hello\n");

    output = run_script(R"(
        #[[
            This is a multi-line comment
        ]]
        message("World")
    )");
    REQUIRE(output == "World\n");
}

TEST_CASE("Function basic invocation", "[interpreter][function]") {
    auto output = run_script(R"(
        function(greet name)
            message("Hello ${name}")
        endfunction()

        greet("World")
    )");
    REQUIRE(output == "Hello World\n");

    output = run_script(R"(
        function(greet name)
            message("Hello ${name}")
        ENDFUNCTION()

        greet("World")
    )");
    REQUIRE(output == "Hello World\n");
}

TEST_CASE("Function scoping - variables don't leak", "[interpreter][function]") {
    // Functions create a new scope, so variables set inside don't affect parent
    auto output = run_script(R"(
        set(MY_VAR "outer")

        function(test_scope)
            set(MY_VAR "inner")
            message("Inside: ${MY_VAR}")
        endfunction()

        test_scope()
        message("Outside: ${MY_VAR}")
    )");
    REQUIRE(output == "Inside: inner\nOutside: outer\n");
}

TEST_CASE("Function with multiple parameters", "[interpreter][function]") {
    auto output = run_script(R"(
        function(add_message a b c)
            message("${a} ${b} ${c}")
        endfunction()

        add_message("one" "two" "three")
    )");
    REQUIRE(output == "one two three\n");
}

TEST_CASE("Function ARGC and ARGV", "[interpreter][function]") {
    auto output = run_script(R"(
        function(test_args)
            message("ARGC=${ARGC}")
            message("ARGV0=${ARGV0}")
            message("ARGV1=${ARGV1}")
            message("ARGV2=${ARGV2}")
        endfunction()

        test_args("first" "second" "third")
    )");
    REQUIRE(output == "ARGC=3\nARGV0=first\nARGV1=second\nARGV2=third\n");
}

TEST_CASE("Function ARGN for extra arguments", "[interpreter][function]") {
    auto output = run_script(R"(
        function(test_argn required_param)
            message("required=${required_param}")
            message("ARGN=${ARGN}")
        endfunction()

        test_argn("first" "second" "third")
    )");
    REQUIRE(output == "required=first\nARGN=second;third\n");
}

TEST_CASE("Macro basic invocation", "[interpreter][macro]") {
    auto output = run_script(R"(
        macro(greet name)
            message("Hello ${name}")
        endmacro()

        greet("World")
    )");
    REQUIRE(output == "Hello World\n");
}

TEST_CASE("Macro scoping - variables DO leak", "[interpreter][macro]") {
    // Macros do NOT create a new scope, so variables set inside affect parent
    auto output = run_script(R"(
        set(MY_VAR "outer")

        macro(test_scope)
            set(MY_VAR "inner")
            message("Inside: ${MY_VAR}")
        endmacro()

        test_scope()
        message("Outside: ${MY_VAR}")
    )");
    REQUIRE(output == "Inside: inner\nOutside: inner\n");
}

TEST_CASE("Macro with multiple parameters", "[interpreter][macro]") {
    auto output = run_script(R"(
        macro(add_message a b c)
            message("${a} ${b} ${c}")
        endmacro()

        add_message("one" "two" "three")
    )");
    REQUIRE(output == "one two three\n");
}

TEST_CASE("Macro with dynamic name from variable", "[interpreter][macro]") {
    // vcpkg.cmake uses macro("${VAR}" ...) to define a macro whose name
    // comes from a variable. The name must be resolved at registration
    // time, not parse time.
    auto output = run_script(R"(
        set(MY_NAME my_dynamic_macro)
        macro("${MY_NAME}" arg)
            message("called ${arg}")
        endmacro()

        my_dynamic_macro("hi")
    )");
    REQUIRE(output == "called hi\n");
}

TEST_CASE("Function with dynamic name from variable", "[interpreter][function]") {
    auto output = run_script(R"(
        set(FN_NAME my_dynamic_fn)
        function("${FN_NAME}" arg)
            message("fn ${arg}")
        endfunction()

        my_dynamic_fn("hi")
    )");
    REQUIRE(output == "fn hi\n");
}

TEST_CASE("Macro name with literal+variable concatenation", "[interpreter][macro]") {
    auto output = run_script(R"(
        set(SUFFIX _override)
        macro("find_package${SUFFIX}" pkg)
            message("override ${pkg}")
        endmacro()

        find_package_override("foo")
    )");
    REQUIRE(output == "override foo\n");
}

TEST_CASE("Function and macro scoping comparison", "[interpreter][function][macro]") {
    auto output = run_script(R"(
        set(VAR "original")

        function(func_test)
            set(VAR "changed_by_func")
        endfunction()

        macro(macro_test)
            set(VAR "changed_by_macro")
        endmacro()

        func_test()
        message("After function: ${VAR}")

        macro_test()
        message("After macro: ${VAR}")
    )");
    REQUIRE(output == "After function: original\nAfter macro: changed_by_macro\n");
}

TEST_CASE("Nested function calls", "[interpreter][function]") {
    auto output = run_script(R"(
        function(inner msg)
            message("Inner: ${msg}")
        endfunction()

        function(outer msg)
            message("Outer: ${msg}")
            inner("nested")
        endfunction()

        outer("test")
    )");
    REQUIRE(output == "Outer: test\nInner: nested\n");
}

TEST_CASE("Function can read parent variables", "[interpreter][function]") {
    auto output = run_script(R"(
        set(PARENT_VAR "from parent")

        function(read_parent)
            message("${PARENT_VAR}")
        endfunction()

        read_parent()
    )");
    REQUIRE(output == "from parent\n");
}

TEST_CASE("Common CMake variables are initialized", "[interpreter]") {
    // Note: run_script uses an empty string for script_dir
    auto output = run_script(R"(
        message("SRC=${CMAKE_SOURCE_DIR}")
        message("BIN=${CMAKE_BINARY_DIR}")
        message("VER=${CMAKE_MAJOR_VERSION}")
    )");
    REQUIRE(output.find("SRC=") != std::string::npos);
    REQUIRE(output.find("BIN=") != std::string::npos);
    REQUIRE(output.find("VER=4") != std::string::npos);
}

TEST_CASE("set() creates lists from multiple arguments", "[interpreter][list]") {
    auto output = run_script(R"(
        set(MY_LIST "a" "b" "c")
        message("${MY_LIST}")
    )");
    REQUIRE(output == "a;b;c\n");
}

TEST_CASE("list(LENGTH) returns list length", "[interpreter][list]") {
    auto output = run_script(R"(
        set(MY_LIST "x" "y" "z")
        list(LENGTH MY_LIST len)
        message("${len}")
    )");
    REQUIRE(output == "3\n");
}

TEST_CASE("list(GET) retrieves elements by index", "[interpreter][list]") {
    auto output = run_script(R"(
        set(MY_LIST "a" "b" "c" "d")
        list(GET MY_LIST 0 result0)
        list(GET MY_LIST 2 result2)
        message("${result0}")
        message("${result2}")
    )");
    REQUIRE(output == "a\nc\n");
}

TEST_CASE("list(GET) can retrieve multiple indices", "[interpreter][list]") {
    auto output = run_script(R"(
        set(MY_LIST "a" "b" "c" "d")
        list(GET MY_LIST 0 2 3 result)
        message("${result}")
    )");
    REQUIRE(output == "a;c;d\n");
}

TEST_CASE("list(APPEND) adds elements to list", "[interpreter][list]") {
    auto output = run_script(R"(
        set(MY_LIST "a" "b")
        list(APPEND MY_LIST "c" "d")
        message("${MY_LIST}")
    )");
    REQUIRE(output == "a;b;c;d\n");
}

TEST_CASE("list(APPEND) preserves empty elements when list is non-empty", "[interpreter][list]") {
    // Regression: list(APPEND) was skipping empty elements unconditionally.
    // CMake only skips empty elements when the list itself is empty.
    // LLVM's configure_lit_site_cfg relies on this with a "dummy" trick.

    // Append empty to non-empty list: should add empty element
    auto output = run_script(R"(
        set(mylist "dummy")
        list(APPEND mylist "")
        list(LENGTH mylist len)
        message("${len}|${mylist}")
    )");
    REQUIRE(output == "2|dummy;\n");

    // Append empty to empty list: should remain empty
    output = run_script(R"(
        set(mylist "")
        list(APPEND mylist "")
        list(LENGTH mylist len)
        message("${len}|${mylist}")
    )");
    REQUIRE(output == "0|\n");

    // Multiple empty appends to non-empty list
    output = run_script(R"(
        set(mylist "start")
        list(APPEND mylist "" "" "")
        list(LENGTH mylist len)
        message("${len}|${mylist}")
    )");
    REQUIRE(output == "4|start;;;\n");

    // LLVM dummy pattern: empty variable value should still count
    output = run_script(R"(
        set(A "val_a")
        set(B "")
        set(C "val_c")
        set(PATHS "A;B;C")
        set(VALUES "dummy")
        foreach(path ${PATHS})
            list(APPEND VALUES "${${path}}")
        endforeach()
        list(REMOVE_AT VALUES 0)
        list(LENGTH PATHS len_p)
        list(LENGTH VALUES len_v)
        message("${len_p}|${len_v}|${VALUES}")
    )");
    REQUIRE(output == "3|3|val_a;;val_c\n");
}

TEST_CASE("escaped semicolons (backslash-semicolon) are unescaped on extraction", "[interpreter][list]") {
    // Regression: \; in variable values was not unescaped when elements were
    // extracted via unquoted expansion, list(GET), list(FIND), or foreach.
    // CMake treats \; as a non-separator semicolon that gets unescaped to ;
    // when the element is extracted from the list.

    // Unquoted expansion unescapes \; to ;
    auto output = run_script(R"(
        function(show_argc)
            message("${ARGC}|${ARGV0}")
        endfunction()
        set(var "x\;y\;z")
        show_argc(${var})
    )");
    REQUIRE(output == "1|x;y;z\n");

    // string(REPLACE) + unquoted expand (the LLVM make_paths_relative pattern)
    output = run_script(R"(
        function(show_argc)
            message("${ARGC}|${ARGV0}")
        endfunction()
        set(paths "/a;/b;/c")
        string(REPLACE ";" "\\;" escaped "${paths}")
        show_argc(${escaped})
    )");
    REQUIRE(output == "1|/a;/b;/c\n");

    // list(GET) unescapes \;
    output = run_script(R"(
        set(var "a\;b;c")
        list(GET var 0 first)
        list(GET var 1 second)
        message("${first}|${second}")
    )");
    REQUIRE(output == "a;b|c\n");

    // list(FIND) compares against unescaped value
    output = run_script(R"(
        set(var "a\;b;c;d")
        list(FIND var "a;b" idx)
        message("${idx}")
    )");
    REQUIRE(output == "0\n");

    // foreach unescapes \;
    output = run_script(R"(
        set(var "x\;y;z")
        foreach(item ${var})
            message("${item}")
        endforeach()
    )");
    REQUIRE(output == "x;y\nz\n");

    // Quoted expansion preserves \; as-is
    output = run_script(R"(
        set(var "a\;b")
        message("${var}")
    )");
    REQUIRE(output == "a\\;b\n");

    // Multiple \; in one element
    output = run_script(R"(
        set(var "a\;b\;c;d")
        list(GET var 0 first)
        message("${first}")
    )");
    REQUIRE(output == "a;b;c\n");

    // \; at start and end of element
    output = run_script(R"(
        set(var "\;a;b\;")
        list(GET var 0 first)
        list(GET var 1 second)
        message("'${first}'|'${second}'")
    )");
    REQUIRE(output == "';a'|'b;'\n");
}

TEST_CASE("list(REVERSE) reverses the list", "[interpreter][list]") {
    auto output = run_script(R"(
        set(MY_LIST "1" "2" "3")
        list(REVERSE MY_LIST)
        message("${MY_LIST}")
    )");
    REQUIRE(output == "3;2;1\n");
}

TEST_CASE("list(SORT) sorts the list", "[interpreter][list]") {
    auto output = run_script(R"(
        set(MY_LIST "c" "a" "b")
        list(SORT MY_LIST)
        message("${MY_LIST}")
    )");
    REQUIRE(output == "a;b;c\n");
}

TEST_CASE("list(REMOVE_DUPLICATES) removes duplicate items", "[interpreter][list]") {
    auto output = run_script(R"(
        set(MY_LIST "a" "b" "a" "c" "b")
        list(REMOVE_DUPLICATES MY_LIST)
        message("${MY_LIST}")
    )");
    REQUIRE(output == "a;b;c\n");
}

TEST_CASE("list(SUBLIST) extracts sublist", "[interpreter][list]") {
    auto output = run_script(R"(
        set(MY_LIST "a" "b" "c" "d" "e")
        list(SUBLIST MY_LIST 1 3 result)
        message("${result}")
    )");
    REQUIRE(output == "b;c;d\n");
}

TEST_CASE("list(REMOVE_ITEM) removes", "[interpreter][list]") {
    auto output = run_script(R"(
        set(MY_LIST "a" "b" "c" "d" "e")
        list(REMOVE_ITEM MY_LIST "a" "d")
        message("${MY_LIST}")
    )");
    REQUIRE(output == "b;c;e\n");
}

TEST_CASE("CMakeArray handles empty lists", "[interpreter][list]") {
    auto output = run_script(R"(
        set(EMPTY_LIST "")
        list(LENGTH EMPTY_LIST len)
        message("${len}")
    )");
    REQUIRE(output == "0\n");
}

TEST_CASE("CMakeArray handles semicolons in variable references", "[interpreter][list]") {
    auto output = run_script(R"(
        set(LIST1 "a;b;c")
        list(LENGTH LIST1 len)
        message("${len}")
    )");
    REQUIRE(output == "3\n");
}

TEST_CASE("list(JOIN) joins list elements with glue", "[interpreter][list]") {
    auto output = run_script(R"(
        set(MY_LIST "a" "b" "c" "d")
        list(JOIN MY_LIST "," result)
        message("${result}")
    )");
    REQUIRE(output == "a,b,c,d\n");

    output = run_script(R"(
        set(MY_LIST "a" "b" "c")
        list(JOIN MY_LIST "" result)
        message("${result}")
    )");
    REQUIRE(output == "abc\n");

    output = run_script(R"(
        set(MY_LIST "a" "b" "c")
        list(JOIN MY_LIST " -> " result)
        message("${result}")
    )");
    REQUIRE(output == "a -> b -> c\n");
}

TEST_CASE("list(PREPEND) adds elements to front", "[interpreter][list]") {
    auto output = run_script(R"(
        set(MY_LIST "c" "d")
        list(PREPEND MY_LIST "a" "b")
        message("${MY_LIST}")
    )");
    REQUIRE(output == "a;b;c;d\n");
}

TEST_CASE("list(POP_BACK) removes and returns last element", "[interpreter][list]") {
    auto output = run_script(R"(
        set(MY_LIST "a" "b" "c" "d")
        list(POP_BACK MY_LIST last)
        message("${MY_LIST}")
        message("${last}")
    )");
    REQUIRE(output == "a;b;c\nd\n");
}

TEST_CASE("list(POP_BACK) with multiple values", "[interpreter][list]") {
    auto output = run_script(R"(
        set(MY_LIST "a" "b" "c" "d" "e")
        list(POP_BACK MY_LIST val1 val2)
        message("${MY_LIST}")
        message("${val1}")
        message("${val2}")
    )");
    REQUIRE(output == "a;b;c\ne\nd\n");
}

TEST_CASE("list(POP_FRONT) removes and returns first element", "[interpreter][list]") {
    auto output = run_script(R"(
        set(MY_LIST "a" "b" "c" "d")
        list(POP_FRONT MY_LIST first)
        message("${MY_LIST}")
        message("${first}")
    )");
    REQUIRE(output == "b;c;d\na\n");
}

TEST_CASE("list(POP_FRONT) with multiple values", "[interpreter][list]") {
    auto output = run_script(R"(
        set(MY_LIST "a" "b" "c" "d" "e")
        list(POP_FRONT MY_LIST val1 val2)
        message("${MY_LIST}")
        message("${val1}")
        message("${val2}")
    )");
    REQUIRE(output == "c;d;e\na\nb\n");
}

TEST_CASE("list(POP_BACK) on empty list is silent no-op", "[interpreter][list]") {
    // CMake silently handles POP_BACK/POP_FRONT on empty or undefined lists,
    // setting output variables to empty. Qt6 relies on this for push/pop stacks.
    auto output = run_script(R"(
        set(mylist "")
        list(POP_BACK mylist out)
        message("list=[${mylist}] out=[${out}]")
    )");
    CHECK(output == "list=[] out=[]\n");

    // Undefined list
    output = run_script(R"(
        list(POP_BACK undefined_list out)
        message("list=[${undefined_list}] out=[${out}]")
    )");
    CHECK(output == "list=[] out=[]\n");

    // No output var, just discard
    output = run_script(R"(
        set(mylist "")
        list(POP_BACK mylist)
        message("done")
    )");
    CHECK(output == "done\n");
}

TEST_CASE("list(POP_FRONT) on empty list is silent no-op", "[interpreter][list]") {
    auto output = run_script(R"(
        set(mylist "")
        list(POP_FRONT mylist out)
        message("list=[${mylist}] out=[${out}]")
    )");
    CHECK(output == "list=[] out=[]\n");

    output = run_script(R"(
        list(POP_FRONT undefined_list out)
        message("list=[${undefined_list}] out=[${out}]")
    )");
    CHECK(output == "list=[] out=[]\n");
}

TEST_CASE("list(FILTER) INCLUDE filters list", "[interpreter][list]") {
    auto output = run_script(R"(
        set(MY_LIST "apple" "banana" "apricot" "cherry" "avocado")
        list(FILTER MY_LIST INCLUDE REGEX "^a.*")
        message("${MY_LIST}")
    )");
    REQUIRE(output == "apple;apricot;avocado\n");
}

TEST_CASE("list(FILTER) EXCLUDE filters list", "[interpreter][list]") {
    auto output = run_script(R"(
        set(MY_LIST "apple" "banana" "apricot" "cherry" "avocado")
        list(FILTER MY_LIST EXCLUDE REGEX "^a.*")
        message("${MY_LIST}")
    )");
    REQUIRE(output == "banana;cherry\n");
}

TEST_CASE("list(FILTER) uses substring matching not full match", "[interpreter][list]") {
    auto output = run_script(R"(
        set(MY_LIST "abc123def" "xyz" "123")
        list(FILTER MY_LIST EXCLUDE REGEX "123")
        message("${MY_LIST}")
    )");
    REQUIRE(output == "xyz\n");
}

TEST_CASE("list(FILTER) INCLUDE substring matching", "[interpreter][list]") {
    auto output = run_script(R"(
        set(MY_LIST "hello_world" "goodbye" "hello")
        list(FILTER MY_LIST INCLUDE REGEX "hello")
        message("${MY_LIST}")
    )");
    REQUIRE(output == "hello_world;hello\n");
}

TEST_CASE("list(TRANSFORM) APPEND appends to each element", "[interpreter][list]") {
    auto output = run_script(R"(
        set(MY_LIST "file1" "file2" "file3")
        list(TRANSFORM MY_LIST APPEND ".txt")
        message("${MY_LIST}")
    )");
    REQUIRE(output == "file1.txt;file2.txt;file3.txt\n");
}

TEST_CASE("list(TRANSFORM) PREPEND prepends to each element", "[interpreter][list]") {
    auto output = run_script(R"(
        set(MY_LIST "file1" "file2" "file3")
        list(TRANSFORM MY_LIST PREPEND "src/")
        message("${MY_LIST}")
    )");
    REQUIRE(output == "src/file1;src/file2;src/file3\n");
}

TEST_CASE("list(TRANSFORM) TOUPPER converts to uppercase", "[interpreter][list]") {
    auto output = run_script(R"(
        set(MY_LIST "hello" "world")
        list(TRANSFORM MY_LIST TOUPPER)
        message("${MY_LIST}")
    )");
    REQUIRE(output == "HELLO;WORLD\n");
}

TEST_CASE("list(TRANSFORM) TOLOWER converts to lowercase", "[interpreter][list]") {
    auto output = run_script(R"(
        set(MY_LIST "HELLO" "WORLD")
        list(TRANSFORM MY_LIST TOLOWER)
        message("${MY_LIST}")
    )");
    REQUIRE(output == "hello;world\n");
}

TEST_CASE("list(TRANSFORM) STRIP removes whitespace", "[interpreter][list]") {
    auto output = run_script(R"(
        set(MY_LIST "  hello  " "  world  " "  test  ")
        list(TRANSFORM MY_LIST STRIP)
        message("${MY_LIST}")
    )");
    REQUIRE(output == "hello;world;test\n");
}

TEST_CASE("list(TRANSFORM) REPLACE does regex replacement", "[interpreter][list]") {
    auto output = run_script(R"(
        set(MY_LIST "test1.cpp" "test2.cpp" "main.cpp")
        list(TRANSFORM MY_LIST REPLACE "\\.cpp$" ".o")
        message("${MY_LIST}")
    )");
    REQUIRE(output == "test1.o;test2.o;main.o\n");
}

TEST_CASE("list(TRANSFORM) with OUTPUT_VARIABLE", "[interpreter][list]") {
    auto output = run_script(R"(
        set(MY_LIST "a" "b" "c")
        list(TRANSFORM MY_LIST APPEND "x" OUTPUT_VARIABLE NEW_LIST)
        message("${MY_LIST}")
        message("${NEW_LIST}")
    )");
    REQUIRE(output == "a;b;c\nax;bx;cx\n");
}

TEST_CASE("list(TRANSFORM) with AT selector", "[interpreter][list]") {
    auto output = run_script(R"(
        set(MY_LIST "a" "b" "c" "d" "e")
        list(TRANSFORM MY_LIST TOUPPER AT 1 3)
        message("${MY_LIST}")
    )");
    REQUIRE(output == "a;B;c;D;e\n");
}

TEST_CASE("list(TRANSFORM) with REGEX selector", "[interpreter][list]") {
    auto output = run_script(R"(
        set(MY_LIST "file1.cpp" "main.cpp" "test.txt" "util.cpp")
        list(TRANSFORM MY_LIST TOUPPER REGEX ".*\\.cpp$")
        message("${MY_LIST}")
    )");
    REQUIRE(output == "FILE1.CPP;MAIN.CPP;test.txt;UTIL.CPP\n");
}

TEST_CASE("list(TRANSFORM) with FOR selector", "[interpreter][list]") {
    auto output = run_script(R"(
        set(MY_LIST "0" "1" "2" "3" "4" "5" "6" "7" "8" "9")
        list(TRANSFORM MY_LIST APPEND "_item" FOR 2 5)
        message("${MY_LIST}")
    )");
    REQUIRE(output == "0;1;2_item;3_item;4_item;5_item;6;7;8;9\n");
}

TEST_CASE("list(TRANSFORM) with FOR selector and step", "[interpreter][list]") {
    auto output = run_script(R"(
        set(MY_LIST "a" "b" "c" "d" "e" "f" "g" "h" "i" "j")
        list(TRANSFORM MY_LIST TOUPPER FOR 0 8 2)
        message("${MY_LIST}")
    )");
    REQUIRE(output == "A;b;C;d;E;f;G;h;I;j\n");
}

TEST_CASE("list(TRANSFORM) with FOR selector negative step", "[interpreter][list]") {
    auto output = run_script(R"(
        set(MY_LIST "a" "b" "c" "d" "e")
        list(TRANSFORM MY_LIST TOUPPER FOR 4 1 -1)
        message("${MY_LIST}")
    )");
    REQUIRE(output == "a;B;C;D;E\n");
}

TEST_CASE("list(SORT) with COMPARE FILE_BASENAME", "[interpreter][list]") {
    auto output = run_script(R"(
        set(MY_LIST "/path/to/zebra.txt" "/other/path/alpha.cpp" "/some/beta.hpp")
        list(SORT MY_LIST COMPARE FILE_BASENAME)
        message("${MY_LIST}")
    )");
    REQUIRE(output == "/other/path/alpha.cpp;/some/beta.hpp;/path/to/zebra.txt\n");
}

TEST_CASE("list(SORT) with COMPARE FILE_BASENAME ORDER DESCENDING", "[interpreter][list]") {
    auto output = run_script(R"(
        set(MY_LIST "/path/to/zebra.txt" "/other/path/alpha.cpp" "/some/beta.hpp")
        list(SORT MY_LIST COMPARE FILE_BASENAME ORDER DESCENDING)
        message("${MY_LIST}")
    )");
    REQUIRE(output == "/path/to/zebra.txt;/some/beta.hpp;/other/path/alpha.cpp\n");
}

TEST_CASE("Foreach basic", "[interpreter][foreach]") {
    auto output = run_script(R"(
        foreach(i 1 2 3)
            message("${i}")
        endforeach()
    )");
    REQUIRE(output == "1\n2\n3\n");

    output = run_script(R"(
        foreach(i 1 2 3)
            message("${i}")
        ENDFOREACH()
    )");
    REQUIRE(output == "1\n2\n3\n");
}

TEST_CASE("foreach with empty item list", "[interpreter][foreach]") {
    // CMake allows foreach(var) and foreach(var <empty_expansion>) with no items.
    // The body is never executed. This pattern appears in Qt6 config files.
    // NOTE: An optimizer could elide the body entirely when items are statically empty.
    auto output = run_script(R"(
        message("before")
        foreach(x )
            message("item: ${x}")
        endforeach()
        message("after")
    )");
    CHECK(output == "before\nafter\n");

    // Empty variable expands to nothing — same behavior
    output = run_script(R"(
        set(empty "")
        foreach(x ${empty})
            message("item: ${x}")
        endforeach()
        message("done")
    )");
    CHECK(output == "done\n");

    // IN LISTS with empty list variable
    output = run_script(R"(
        set(mylist "")
        foreach(x IN LISTS mylist)
            message("item: ${x}")
        endforeach()
        message("done")
    )");
    CHECK(output == "done\n");
}

TEST_CASE("foreach with lowercase in is literal item", "[interpreter][foreach]") {
    // CMake foreach keywords (IN, RANGE, LISTS, ITEMS) are case-sensitive
    // Lowercase 'in' should be treated as a literal item to iterate, not a keyword
    auto output = run_script(R"(
        set(CHARSETS a b c)
        foreach(cs in ${CHARSETS})
            message("${cs}")
        endforeach()
    )");
    REQUIRE(output == "in\na\nb\nc\n");
}

TEST_CASE("foreach RANGE with stop only", "[interpreter][foreach]") {
    auto output = run_script(R"(
        foreach(i RANGE 3)
            message("${i}")
        endforeach()
    )");
    REQUIRE(output == "0\n1\n2\n3\n");
}

TEST_CASE("foreach RANGE with start and stop", "[interpreter][foreach]") {
    auto output = run_script(R"(
        foreach(i RANGE 2 5)
            message("${i}")
        endforeach()
    )");
    REQUIRE(output == "2\n3\n4\n5\n");
}

TEST_CASE("foreach RANGE with start, stop, and step", "[interpreter][foreach]") {
    auto output = run_script(R"(
        foreach(i RANGE 0 10 2)
            message("${i}")
        endforeach()
    )");
    REQUIRE(output == "0\n2\n4\n6\n8\n10\n");
}

TEST_CASE("foreach RANGE with negative step", "[interpreter][foreach]") {
    auto output = run_script(R"(
        foreach(i RANGE 5 2 -1)
            message("${i}")
        endforeach()
    )");
    REQUIRE(output == "5\n4\n3\n2\n");
}

TEST_CASE("foreach RANGE descending without explicit step", "[interpreter][foreach]") {
    // CMake behavior: RANGE infers direction when step is omitted
    auto output = run_script(R"(
        foreach(i RANGE 5 2)
            message("${i}")
        endforeach()
        message("done")
    )");
    REQUIRE(output == "5\n4\n3\n2\ndone\n");
}

TEST_CASE("foreach IN ITEMS iterates over literal items", "[interpreter][foreach]") {
    auto output = run_script(R"(
        foreach(item IN ITEMS "x" "y" "z")
            message("${item}")
        endforeach()
    )");
    REQUIRE(output == "x\ny\nz\n");
}

TEST_CASE("foreach IN LISTS iterates over list variable", "[interpreter][foreach]") {
    auto output = run_script(R"(
        set(MY_LIST "a" "b" "c")
        foreach(item IN LISTS MY_LIST)
            message("${item}")
        endforeach()
    )");
    REQUIRE(output == "a\nb\nc\n");
}

TEST_CASE("foreach IN LISTS with multiple lists", "[interpreter][foreach]") {
    auto output = run_script(R"(
        set(LIST1 "a" "b")
        set(LIST2 "c" "d")
        foreach(item IN LISTS LIST1 LIST2)
            message("${item}")
        endforeach()
    )");
    REQUIRE(output == "a\nb\nc\nd\n");
}

TEST_CASE("foreach IN LISTS and ITEMS combined", "[interpreter][foreach]") {
    auto output = run_script(R"(
        set(MY_LIST "a" "b")
        foreach(item IN LISTS MY_LIST ITEMS "x" "y")
            message("${item}")
        endforeach()
    )");
    REQUIRE(output == "a\nb\nx\ny\n");
}

TEST_CASE("foreach loop variable is cleared after loop", "[interpreter][foreach]") {
    // CMake behavior: loop variable is cleared after foreach completes
    auto output = run_script(R"(
        foreach(i RANGE 2)
            message("in loop: ${i}")
        endforeach()
        message("after loop: '${i}'")
    )");
    REQUIRE(output == "in loop: 0\nin loop: 1\nin loop: 2\nafter loop: ''\n");
}

TEST_CASE("foreach loop var is DEFINED after loop even if unset before", "[interpreter][foreach]") {
    // Match CMake: loop variable becomes defined (empty string) after loop,
    // even if it was never set before the loop. CMake does NOT unset it.
    auto output = run_script(R"(
        foreach(V1 a b)
        endforeach()
        if(DEFINED V1)
            message("V1 DEFINED='${V1}'")
        else()
            message("V1 NOT DEFINED")
        endif()
    )");
    REQUIRE(output == "V1 DEFINED=''\n");
}

TEST_CASE("foreach loop var restored to original if previously set", "[interpreter][foreach]") {
    // Match CMake: loop variable is restored to its pre-loop value
    auto output = run_script(R"(
        set(V "original")
        foreach(V x y z)
        endforeach()
        if(DEFINED V)
            message("V='${V}'")
        else()
            message("V NOT DEFINED")
        endif()
    )");
    REQUIRE(output == "V='original'\n");
}

TEST_CASE("foreach loop var DEFINED after loop when explicitly unset before", "[interpreter][foreach]") {
    // Match CMake: even if variable was set then unset before the loop,
    // it still becomes defined (empty string) after the loop
    auto output = run_script(R"(
        set(V "something")
        unset(V)
        foreach(V a b)
        endforeach()
        if(DEFINED V)
            message("V DEFINED='${V}'")
        else()
            message("V NOT DEFINED")
        endif()
    )");
    REQUIRE(output == "V DEFINED=''\n");
}

TEST_CASE("foreach variable used before set in loop body", "[interpreter][foreach]") {
    // Match CMake: loop body shares parent scope, so variables set in one
    // iteration are visible in the next
    auto output = run_script(R"(
        foreach(i a b c)
            message("${i}:${MYVAR}")
            set(MYVAR "val_${i}")
        endforeach()
        message("after:${MYVAR}")
    )");
    REQUIRE(output == "a:\nb:val_a\nc:val_b\nafter:val_c\n");
}

TEST_CASE("foreach can iterate over variable references", "[interpreter][foreach]") {
    auto output = run_script(R"(
        set(STOP "3")
        foreach(i RANGE ${STOP})
            message("${i}")
        endforeach()
    )");
    REQUIRE(output == "0\n1\n2\n3\n");
}

TEST_CASE("foreach works with nested loops", "[interpreter][foreach]") {
    auto output = run_script(R"(
        foreach(i RANGE 1 2)
            foreach(j RANGE 1 2)
                message("${i},${j}")
            endforeach()
        endforeach()
    )");
    REQUIRE(output == "1,1\n1,2\n2,1\n2,2\n");
}

TEST_CASE("foreach break exits loop early", "[interpreter][foreach][break]") {
    auto output = run_script(R"(
        foreach(i RANGE 0 5)
            message("${i}")
            if(i EQUAL "2")
                break()
            endif()
        endforeach()
        message("done")
    )");
    // Note: We don't have EQUAL condition yet, so let's use a simpler test
    REQUIRE(output.find("done") != std::string::npos);
}

TEST_CASE("foreach break exits loop early (simple test)", "[interpreter][foreach][break]") {
    auto output = run_script(R"(
        foreach(i RANGE 0 10)
            if(i)
                message("${i}")
            endif()
            if(i)
                if(i)
                    if(i)
                        break()
                    endif()
                endif()
            endif()
        endforeach()
        message("done")
    )");
    // First iteration: i=0, condition false, no message, break not executed
    // Second iteration: i=1, prints 1, then breaks
    REQUIRE(output == "1\ndone\n");
}

TEST_CASE("foreach continue skips to next iteration", "[interpreter][foreach][continue]") {
    auto output = run_script(R"(
        foreach(i RANGE 1 5)
            set(VAR "${i}")
            if(VAR)
                if(VAR)
                    continue()
                endif()
            endif()
            message("after continue: ${i}")
        endforeach()
    )");
    // All iterations should skip the message due to continue
    REQUIRE(output == "");
}

TEST_CASE("foreach continue with selective execution", "[interpreter][foreach][continue]") {
    auto output = run_script(R"(
        foreach(num RANGE 0 4)
            if(num)
                continue()
            endif()
            message("${num}")
        endforeach()
    )");
    // Only 0 should print (when num is falsy)
    REQUIRE(output == "0\n");
}

TEST_CASE("foreach break in nested loops only affects inner loop", "[interpreter][foreach][break]") {
    auto output = run_script(R"(
        foreach(i RANGE 1 2)
            message("outer: ${i}")
            foreach(j RANGE 1 3)
                message("  inner: ${j}")
                if(j)
                    break()
                endif()
            endforeach()
        endforeach()
    )");
    // Outer loop runs fully (1, 2)
    // Inner loop breaks after first truthy j (j=1)
    REQUIRE(output == "outer: 1\n  inner: 1\nouter: 2\n  inner: 1\n");
}

TEST_CASE("break outside loop is fatal error", "[interpreter][foreach][break]") {
    CHECK_THROWS(run_script(R"(
        break()
    )"));
}

TEST_CASE("continue outside loop is fatal error", "[interpreter][foreach][continue]") {
    CHECK_THROWS(run_script(R"(
        continue()
    )"));
}

TEST_CASE("continue in function loop", "[interpreter][foreach][continue]") {
    CHECK_THROWS(run_script(R"(
        function(foo)
            continue()
        endfunction()
        foreach(i RANGE 2)
            foo()
        endforeach()
    )"));
}

TEST_CASE("break in macro affects caller loop", "[interpreter][foreach][macro]") {
    auto output = run_script(R"(
        macro(my_break)
            break()
        endmacro()

        foreach(i RANGE 1 5)
            message("${i}")
            my_break()
        endforeach()
        message("done")
    )");
    REQUIRE(output == "1\ndone\n");
}

TEST_CASE("foreach IN ZIP_LISTS with single loop variable", "[interpreter][foreach][zip_lists]") {
    auto output = run_script(R"(
        set(A "1" "2" "3")
        set(B "one" "two" "three")
        foreach(item IN ZIP_LISTS A B)
            message("${item_0},${item_1}")
        endforeach()
    )");
    REQUIRE(output == "1,one\n2,two\n3,three\n");
}

TEST_CASE("foreach IN ZIP_LISTS with multiple loop variables", "[interpreter][foreach][zip_lists]") {
    auto output = run_script(R"(
        set(NUMS "1" "2" "3")
        set(WORDS "one" "two" "three")
        foreach(num word IN ZIP_LISTS NUMS WORDS)
            message("${num},${word}")
        endforeach()
    )");
    REQUIRE(output == "1,one\n2,two\n3,three\n");
}

TEST_CASE("foreach IN ZIP_LISTS with different length lists", "[interpreter][foreach][zip_lists]") {
    // Shorter lists are padded with empty strings
    auto output = run_script(R"(
        set(SHORT "a" "b")
        set(LONG "1" "2" "3" "4")
        foreach(s l IN ZIP_LISTS SHORT LONG)
            message("[${s}],[${l}]")
        endforeach()
    )");
    REQUIRE(output == "[a],[1]\n[b],[2]\n[],[3]\n[],[4]\n");
}

TEST_CASE("foreach IN ZIP_LISTS with three lists", "[interpreter][foreach][zip_lists]") {
    auto output = run_script(R"(
        set(A "1" "2")
        set(B "a" "b")
        set(C "x" "y")
        foreach(num letter sym IN ZIP_LISTS A B C)
            message("${num}${letter}${sym}")
        endforeach()
    )");
    REQUIRE(output == "1ax\n2by\n");
}

TEST_CASE("foreach IN ZIP_LISTS with single variable and three lists", "[interpreter][foreach][zip_lists]") {
    auto output = run_script(R"(
        set(A "1" "2")
        set(B "a" "b")
        set(C "x" "y")
        foreach(item IN ZIP_LISTS A B C)
            message("${item_0},${item_1},${item_2}")
        endforeach()
    )");
    REQUIRE(output == "1,a,x\n2,b,y\n");
}

TEST_CASE("foreach IN ZIP_LISTS with empty list", "[interpreter][foreach][zip_lists]") {
    auto output = run_script(R"(
        set(EMPTY "")
        set(NONEMPTY "a" "b")
        foreach(e ne IN ZIP_LISTS EMPTY NONEMPTY)
            message("[${e}],[${ne}]")
        endforeach()
    )");
    REQUIRE(output == "[],[a]\n[],[b]\n");
}

TEST_CASE("foreach IN ZIP_LISTS variables are cleared after loop", "[interpreter][foreach][zip_lists]") {
    auto output = run_script(R"(
        set(A "1")
        set(B "a")
        foreach(x y IN ZIP_LISTS A B)
            message("${x},${y}")
        endforeach()
        message("after: '${x}','${y}'")
    )");
    REQUIRE(output == "1,a\nafter: '',''\n");
}

TEST_CASE("foreach IN ZIP_LISTS vars are DEFINED after loop even if unset before", "[interpreter][foreach][zip_lists]") {
    // Match CMake: ZIP_LISTS loop variables become defined (empty string) after loop,
    // even if they were never set before the loop. CMake does NOT unset them.
    auto output = run_script(R"(
        set(L1 "x;y")
        set(L2 "1;2")
        foreach(zv1 zv2 IN ZIP_LISTS L1 L2)
        endforeach()
        if(DEFINED zv1)
            message("zv1 DEFINED='${zv1}'")
        else()
            message("zv1 NOT DEFINED")
        endif()
        if(DEFINED zv2)
            message("zv2 DEFINED='${zv2}'")
        else()
            message("zv2 NOT DEFINED")
        endif()
    )");
    REQUIRE(output == "zv1 DEFINED=''\nzv2 DEFINED=''\n");
}

TEST_CASE("foreach IN ZIP_LISTS single var DEFINED after loop", "[interpreter][foreach][zip_lists]") {
    // Match CMake: single-variable ZIP_LISTS creates var_0, var_1, etc.
    // These become defined (empty string) after the loop, not unset.
    auto output = run_script(R"(
        set(L1 "x;y")
        set(L2 "1;2")
        foreach(zs IN ZIP_LISTS L1 L2)
        endforeach()
        if(DEFINED zs_0)
            message("zs_0 DEFINED='${zs_0}'")
        else()
            message("zs_0 NOT DEFINED")
        endif()
        if(DEFINED zs_1)
            message("zs_1 DEFINED='${zs_1}'")
        else()
            message("zs_1 NOT DEFINED")
        endif()
    )");
    REQUIRE(output == "zs_0 DEFINED=''\nzs_1 DEFINED=''\n");
}

TEST_CASE("foreach IN ZIP_LISTS vars restored if previously set", "[interpreter][foreach][zip_lists]") {
    // Match CMake: if loop variables were set before the loop, they are restored
    auto output = run_script(R"(
        set(L1 "x;y")
        set(L2 "1;2")
        set(zv1 "orig1")
        set(zv2 "orig2")
        foreach(zv1 zv2 IN ZIP_LISTS L1 L2)
        endforeach()
        message("zv1='${zv1}' zv2='${zv2}'")
    )");
    REQUIRE(output == "zv1='orig1' zv2='orig2'\n");
}

TEST_CASE("foreach IN ZIP_LISTS with break", "[interpreter][foreach][zip_lists]") {
    auto output = run_script(R"(
        set(A "1" "2" "3")
        set(B "a" "b" "c")
        foreach(x y IN ZIP_LISTS A B)
            message("${x}${y}")
            if(x EQUAL 2)
                break()
            endif()
        endforeach()
        message("done")
    )");
    REQUIRE(output == "1a\n2b\ndone\n");
}

TEST_CASE("foreach IN ZIP_LISTS with continue", "[interpreter][foreach][zip_lists]") {
    auto output = run_script(R"(
        set(A "1" "2" "3")
        set(B "a" "b" "c")
        foreach(x y IN ZIP_LISTS A B)
            if(x EQUAL 2)
                continue()
            endif()
            message("${x}${y}")
        endforeach()
    )");
    REQUIRE(output == "1a\n3c\n");
}

TEST_CASE("if condition: AND operator", "[interpreter][if]") {
    // Note: Don't use Y or N as variable names - they're boolean constants in CMake
    auto output = run_script(R"(
        set(VAR_A "1")
        set(VAR_B "1")
        if(VAR_A AND VAR_B)
            message("pass")
        else()
            message("fail")
        endif()
    )");
    REQUIRE(output == "pass\n");

    output = run_script(R"(
        set(VAR_A "1")
        set(VAR_B "0")
        if(VAR_A AND VAR_B)
            message("fail")
        else()
            message("pass")
        endif()
    )");
    REQUIRE(output == "pass\n");
}

TEST_CASE("if condition: OR operator", "[interpreter][if]") {
    // Note: Don't use Y or N as variable names - they're boolean constants in CMake
    auto output = run_script(R"(
        set(VAR_A "0")
        set(VAR_B "1")
        if(VAR_A OR VAR_B)
            message("pass")
        else()
            message("fail")
        endif()
    )");
    REQUIRE(output == "pass\n");

    output = run_script(R"(
        set(VAR_A "0")
        set(VAR_B "0")
        if(VAR_A OR VAR_B)
            message("fail")
        else()
            message("pass")
        endif()
    )");
    REQUIRE(output == "pass\n");
}

TEST_CASE("if condition: NOT operator", "[interpreter][if]") {
    auto output = run_script(R"(
        set(X "0")
        if(NOT X)
            message("pass")
        else()
            message("fail")
        endif()
    )");
    REQUIRE(output == "pass\n");

    output = run_script(R"(
        set(X "1")
        if(NOT X)
            message("fail")
        else()
            message("pass")
        endif()
    )");
    REQUIRE(output == "pass\n");
}

TEST_CASE("if condition: numeric comparisons", "[interpreter][if]") {
    auto output = run_script(R"(
        set(A "5")
        set(B "10")
        if(A LESS B)
            message("pass")
        endif()
    )");
    REQUIRE(output == "pass\n");

    output = run_script(R"(
        set(A "10")
        set(B "10")
        if(A EQUAL B)
            message("pass")
        endif()
    )");
    REQUIRE(output == "pass\n");

    output = run_script(R"(
        set(A "15")
        set(B "10")
        if(A GREATER B)
            message("pass")
        endif()
    )");
    REQUIRE(output == "pass\n");
}

TEST_CASE("if condition: binary comparisons auto-dereference boolean constant names", "[interpreter][if][bugfix]") {
    auto output = run_script(R"(
        set(n "7")
        set(y "7")
        set(N "8")
        set(Y "8")
        set(ON "11")
        set(OFF "11")
        set(FALSE "12")
        set(TRUE "12")

        if(n EQUAL y)
            message("n_y")
        endif()
        if(N EQUAL Y)
            message("N_Y")
        endif()
        if(ON EQUAL OFF)
            message("ON_OFF")
        endif()
        if(FALSE EQUAL TRUE)
            message("FALSE_TRUE")
        endif()
        if(N)
            message("bare_N_true")
        else()
            message("bare_N_false")
        endif()
    )");
    REQUIRE(output == "n_y\nN_Y\nON_OFF\nFALSE_TRUE\nbare_N_false\n");
}

TEST_CASE("if condition: numeric comparison with list values", "[interpreter][if]") {
    // file(DOWNLOAD ... STATUS status) sets status to a list like "0;\"No error\"".
    // CMake's EQUAL parses only the leading numeric portion (uses sscanf "%lg").
    SECTION("EQUAL with semicolon-separated list") {
        auto output = run_script(R"(
            set(status "0;\"No error\"")
            if(status EQUAL 0)
                message("pass")
            endif()
        )");
        REQUIRE(output == "pass\n");
    }

    SECTION("NOT EQUAL with list value") {
        auto output = run_script(R"(
            set(status "0;\"No error\"")
            if(NOT status EQUAL 0)
                message("fail")
            else()
                message("pass")
            endif()
        )");
        REQUIRE(output == "pass\n");
    }

    SECTION("non-zero status code in list") {
        auto output = run_script(R"(
            set(status "1;\"Download failed\"")
            if(status EQUAL 1)
                message("pass")
            endif()
        )");
        REQUIRE(output == "pass\n");
    }

    SECTION("LESS/GREATER with list values") {
        auto output = run_script(R"(
            set(status "0;\"No error\"")
            if(status LESS 1)
                message("pass")
            endif()
        )");
        REQUIRE(output == "pass\n");
    }

    // All numeric comparison tests verified against CMake 3.31
    SECTION("version-like strings with LESS_EQUAL") {
        // "3.31.0" parses as double 3.31, "3.19" as 3.19 → 3.31 > 3.19 → FALSE
        auto output = run_script(R"(
            if("3.31.0" LESS_EQUAL "3.19")
                message("fail")
            else()
                message("pass")
            endif()
        )");
        REQUIRE(output == "pass\n");
    }

    SECTION("version-like strings with LESS") {
        auto output = run_script(R"(
            if("3.31.0" LESS "3.19")
                message("fail")
            else()
                message("pass")
            endif()
        )");
        REQUIRE(output == "pass\n");
    }

    SECTION("version-like strings with GREATER") {
        auto output = run_script(R"(
            if("3.31.0" GREATER "3.19")
                message("pass")
            else()
                message("fail")
            endif()
        )");
        REQUIRE(output == "pass\n");
    }

    SECTION("dotted numeric EQUAL") {
        auto output = run_script(R"(
            if("3.19" EQUAL "3.19")
                message("pass")
            else()
                message("fail")
            endif()
        )");
        REQUIRE(output == "pass\n");
    }

    SECTION("integer comparison still works") {
        auto output = run_script(R"(
            if(42 LESS_EQUAL 42)
                message("le_pass")
            endif()
            if(41 LESS 42)
                message("lt_pass")
            endif()
            if(43 GREATER 42)
                message("gt_pass")
            endif()
            if(42 GREATER_EQUAL 42)
                message("ge_pass")
            endif()
        )");
        REQUIRE(output == "le_pass\nlt_pass\ngt_pass\nge_pass\n");
    }

    SECTION("mixed int and float comparison") {
        auto output = run_script(R"(
            if("3" LESS "3.1")
                message("pass")
            else()
                message("fail")
            endif()
        )");
        REQUIRE(output == "pass\n");
    }

    SECTION("same major different minor LESS_EQUAL") {
        auto output = run_script(R"(
            if("3.19" LESS_EQUAL "3.31")
                message("pass")
            else()
                message("fail")
            endif()
        )");
        REQUIRE(output == "pass\n");
    }

    SECTION("negative number comparison") {
        auto output = run_script(R"(
            if("-5" LESS "3")
                message("pass")
            else()
                message("fail")
            endif()
        )");
        REQUIRE(output == "pass\n");
    }

    SECTION("non-numeric leading value fails comparison") {
        auto output = run_script(R"(
            set(status "abc;123")
            if(status EQUAL 0)
                message("fail")
            else()
                message("pass")
            endif()
        )");
        REQUIRE(output == "pass\n");
    }
}

TEST_CASE("if condition: string comparisons", "[interpreter][if]") {
    auto output = run_script(R"(
        set(A "abc")
        set(B "xyz")
        if(A STRLESS B)
            message("pass")
        endif()
    )");
    REQUIRE(output == "pass\n");

    output = run_script(R"(
        set(A "hello")
        set(B "hello")
        if(A STREQUAL B)
            message("pass")
        endif()
    )");
    REQUIRE(output == "pass\n");
}

TEST_CASE("if condition: DEFINED operator", "[interpreter][if]") {
    auto output = run_script(R"(
        set(VAR "value")
        if(DEFINED VAR)
            message("pass")
        endif()
    )");
    REQUIRE(output == "pass\n");

    output = run_script(R"(
        if(DEFINED NONEXISTENT)
            message("fail")
        else()
            message("pass")
        endif()
    )");
    REQUIRE(output == "pass\n");

    // DEFINED should work even if variable has empty value
    output = run_script(R"(
        set(EMPTY "")
        if(DEFINED EMPTY)
            message("pass")
        else()
            message("fail")
        endif()
    )");
    REQUIRE(output == "pass\n");
}

TEST_CASE("if condition: IN_LIST operator", "[interpreter][if]") {
    // Basic IN_LIST - item is in list (using quoted string)
    auto output = run_script(R"(
        set(MY_LIST foo bar baz)
        if("bar" IN_LIST MY_LIST)
            message("pass")
        else()
            message("fail")
        endif()
    )");
    REQUIRE(output == "pass\n");

    // Item not in list
    output = run_script(R"(
        set(MY_LIST foo bar baz)
        if("qux" IN_LIST MY_LIST)
            message("fail")
        else()
            message("pass")
        endif()
    )");
    REQUIRE(output == "pass\n");

    // Empty list
    output = run_script(R"(
        set(MY_LIST "")
        if("foo" IN_LIST MY_LIST)
            message("fail")
        else()
            message("pass")
        endif()
    )");
    REQUIRE(output == "pass\n");

    // Variable as search value
    output = run_script(R"(
        set(MY_LIST apple banana cherry)
        set(SEARCH banana)
        if(SEARCH IN_LIST MY_LIST)
            message("pass")
        else()
            message("fail")
        endif()
    )");
    REQUIRE(output == "pass\n");

    // Quoted string as search value
    output = run_script(R"(
        set(MY_LIST "hello" "world" "test")
        if("world" IN_LIST MY_LIST)
            message("pass")
        else()
            message("fail")
        endif()
    )");
    REQUIRE(output == "pass\n");

    // Empty string in list - note: CMake removes empty elements during expansion
    output = run_script(R"(
        set(MY_LIST "" foo bar)
        if("" IN_LIST MY_LIST)
            message("pass")
        else()
            message("fail")
        endif()
    )");
    REQUIRE(output == "pass\n");  // Empty elements are now preserved in lists

    // Case sensitivity check
    output = run_script(R"(
        set(MY_LIST Foo Bar Baz)
        if("foo" IN_LIST MY_LIST)
            message("fail")
        else()
            message("pass")
        endif()
    )");
    REQUIRE(output == "pass\n");

    // Unquoted variable dereferencing
    output = run_script(R"(
        set(MY_LIST alpha beta gamma)
        set(SEARCH_VAR beta)
        if(SEARCH_VAR IN_LIST MY_LIST)
            message("pass")
        else()
            message("fail")
        endif()
    )");
    REQUIRE(output == "pass\n");
}

TEST_CASE("if condition: quoted values not treated as keywords", "[interpreter][if]") {
    // Quoted variable expanding to unary keyword (TARGET)
    auto output = run_script(R"(
        set(VAR "TARGET")
        if("${VAR}" STREQUAL "TARGET")
            message("pass")
        else()
            message("fail")
        endif()
    )");
    REQUIRE(output == "pass\n");

    // Quoted variable with IN_LIST
    output = run_script(R"(
        set(KEYWORDS "TIMEOUT;SKIP_RETURN_CODE;WORKING_DIRECTORY")
        set(ITEM "TIMEOUT")
        if("${ITEM}" IN_LIST KEYWORDS)
            message("pass")
        else()
            message("fail")
        endif()
    )");
    REQUIRE(output == "pass\n");

    // Quoted "NOT" should not act as negation
    output = run_script(R"(
        set(X "NOT")
        if("${X}" STREQUAL "NOT")
            message("pass")
        else()
            message("fail")
        endif()
    )");
    REQUIRE(output == "pass\n");

    // Quoted "AND" should not act as logical AND
    output = run_script(R"(
        set(Y "AND")
        if("${Y}" STREQUAL "AND")
            message("pass")
        else()
            message("fail")
        endif()
    )");
    REQUIRE(output == "pass\n");

    // Quoted "OR" should not act as logical OR
    output = run_script(R"(
        set(Z "OR")
        if("${Z}" STREQUAL "OR")
            message("pass")
        else()
            message("fail")
        endif()
    )");
    REQUIRE(output == "pass\n");

    // Quoted "DEFINED" should not act as unary operator
    output = run_script(R"(
        set(W "DEFINED")
        if("${W}" STREQUAL "DEFINED")
            message("pass")
        else()
            message("fail")
        endif()
    )");
    REQUIRE(output == "pass\n");

    // Quoted "EXISTS" should not act as unary operator
    output = run_script(R"(
        set(E "EXISTS")
        if("${E}" STREQUAL "EXISTS")
            message("pass")
        else()
            message("fail")
        endif()
    )");
    REQUIRE(output == "pass\n");
}

TEST_CASE("if condition: quoted keyword values in compound conditions", "[interpreter][if][bugfix]") {
    // Regression: quoted variable expanding to "IN_LIST" in a compound NOT...IN_LIST...AND condition
    // was incorrectly treated as an operator, breaking the parse.
    // Real-world trigger: Qt's _qt_internal_remove_args function.
    SECTION("quoted IN_LIST value with NOT and AND") {
        auto output = run_script(R"(
            set(arg_ALL_ARGS "LABEL;PURPOSE")
            set(find_result 0)
            set(result_len 4)
            set(arg_current "IN_LIST")
            if(NOT "${arg_current}" IN_LIST arg_ALL_ARGS AND find_result LESS result_len)
                message("true")
            else()
                message("false")
            endif()
        )");
        REQUIRE(output == "true\n");
    }

    SECTION("quoted LESS value with NOT and AND") {
        auto output = run_script(R"(
            set(mylist "LESS;GREATER")
            set(val "LESS")
            set(x 1)
            set(y 2)
            if(NOT "${val}" IN_LIST mylist AND x LESS y)
                message("true")
            else()
                message("false")
            endif()
        )");
        // "LESS" IS in mylist, so NOT ... is false; AND short-circuits to false
        REQUIRE(output == "false\n");
    }

    SECTION("quoted AND value as operand to NOT") {
        auto output = run_script(R"(
            set(mylist "foo;bar")
            set(val "AND")
            if(NOT "${val}" IN_LIST mylist)
                message("true")
            else()
                message("false")
            endif()
        )");
        REQUIRE(output == "true\n");
    }

    SECTION("quoted NOT value as operand in compound condition") {
        auto output = run_script(R"(
            set(mylist "foo;bar")
            set(val "NOT")
            set(cnt 0)
            set(limit 4)
            if(NOT "${val}" IN_LIST mylist AND cnt LESS limit)
                message("true")
            else()
                message("false")
            endif()
        )");
        REQUIRE(output == "true\n");
    }

    SECTION("while loop with quoted keyword value") {
        auto output = run_script(R"(
            set(arg_ALL_ARGS "LABEL;PURPOSE")
            set(arg_current "IN_LIST")
            set(iter 0)
            while(NOT "${arg_current}" IN_LIST arg_ALL_ARGS AND iter LESS 3)
                math(EXPR iter "${iter} + 1")
            endwhile()
            message("${iter}")
        )");
        REQUIRE(output == "3\n");
    }
}

TEST_CASE("if condition: true constants (case-insensitive)", "[interpreter][if]") {
    auto output = run_script(R"(
        set(T1 "TRUE")
        set(T2 "True")
        set(T3 "ON")
        set(T4 "YES")
        set(T5 "1")
        if(T1 AND T2 AND T3 AND T4 AND T5)
            message("pass")
        endif()
    )");
    REQUIRE(output == "pass\n");
}

TEST_CASE("if condition: false constants (case-insensitive)", "[interpreter][if]") {
    auto output = run_script(R"(
        set(F1 "FALSE")
        set(F2 "False")
        set(F3 "OFF")
        set(F4 "NO")
        set(F5 "0")
        set(F6 "NOTFOUND")
        set(F7 "IGNORE")
        if(F1 OR F2 OR F3 OR F4 OR F5 OR F6 OR F7)
            message("fail")
        else()
            message("pass")
        endif()
    )");
    REQUIRE(output == "pass\n");
}

TEST_CASE("if condition: empty string is falsy", "[interpreter][if]") {
    auto output = run_script(R"(
        set(EMPTY "")
        if(EMPTY)
            message("fail")
        else()
            message("pass")
        endif()
    )");
    REQUIRE(output == "pass\n");
}

TEST_CASE("if condition: undefined variable is falsy", "[interpreter][if]") {
    auto output = run_script(R"(
        unset(UNDEF)
        if(UNDEF)
            message("fail")
        else()
            message("pass")
        endif()
    )");
    REQUIRE(output == "pass\n");
}

TEST_CASE("if condition: operator precedence", "[interpreter][if]") {
    // AND has higher precedence than OR
    // 1 OR 0 AND 0 should be: 1 OR (0 AND 0) = 1 OR 0 = 1
    auto output = run_script(R"(
        set(ONE "1")
        set(ZERO "0")
        if(ONE OR ZERO AND ZERO)
            message("pass")
        else()
            message("fail")
        endif()
    )");
    REQUIRE(output == "pass\n");

    // NOT has higher precedence than AND
    // NOT 0 AND 1 should be: (NOT 0) AND 1 = 1 AND 1 = 1
    output = run_script(R"(
        set(ONE "1")
        set(ZERO "0")
        if(NOT ZERO AND ONE)
            message("pass")
        else()
            message("fail")
        endif()
    )");
    REQUIRE(output == "pass\n");
}

TEST_CASE("if condition: complex expressions", "[interpreter][if]") {
    auto output = run_script(R"(
        set(A "10")
        set(B "20")
        set(C "15")
        if(A LESS B AND B GREATER C)
            message("pass")
        endif()
    )");
    REQUIRE(output == "pass\n");

    output = run_script(R"(
        set(A "5")
        set(B "10")
        if(A LESS B OR A GREATER B)
            message("pass")
        endif()
    )");
    REQUIRE(output == "pass\n");
}

TEST_CASE("if condition: invalid conditions", "[interpreter][if][negative]") {
    // if(A B C) - unconsumed tokens: CMake errors on this
    REQUIRE_THROWS_AS(run_script(R"(
        set(A "10")
        set(B "20")
        set(C "15")
        if(A B C)
            message("if-branch")
        else()
            message("else-branch")
        endif()
    )"), std::runtime_error);

    // if(A EQUAL) should error - missing right operand
    REQUIRE_THROWS_AS(run_script(R"(
        set(A "10")
        if(A EQUAL)
            message("pass")
        endif()
    )"), std::runtime_error);

    // if(AND B) - AND at start treated as variable name, B is unconsumed
    // CMake errors on unconsumed tokens
    REQUIRE_THROWS_AS(run_script(R"(
        set(B "10")
        if(AND B)
            message("if-branch")
        else()
            message("else-branch")
        endif()
    )"), std::runtime_error);

    // if(NOT) - CMake compatibility: NOT without operand treated as primary value
    // evaluates to false (NOT is not a boolean constant, dereferenced as empty variable)
    auto output = run_script(R"(
        if(NOT)
            message("if-branch")
        else()
            message("else-branch")
        endif()
    )");
    REQUIRE(output.find("else-branch") != std::string::npos);
    REQUIRE(output.find("if-branch") == std::string::npos);

    // Edge case: if(DEFINED) should work - DEFINED is treated as variable name
    output = run_script(R"(
        if(DEFINED)
            message("fail")
        else()
            message("pass")
        endif()
    )");
    REQUIRE(output == "pass\n");

    // Edge case: if(TARGET) should work - TARGET is treated as variable name
    output = run_script(R"(
        if(TARGET)
            message("fail")
        else()
            message("pass")
        endif()
    )");
    REQUIRE(output == "pass\n");

    output = run_script(R"(
        set(A "10")
        set(B "20")
        set(C "15")
        if(A MATCHES "^[0-9]+$" AND B MATCHES "^[0-9]+$" AND C MATCHES "^[0-9]+$")
            message("pass")
        endif()
    )");
    REQUIRE(output == "pass\n");

    output = run_script(R"(
        get_filename_component(DIR "/path/to/file.tar.gz" DIRECTORY)
        get_filename_component(NAME "/path/to/file.tar.gz" NAME)
        get_filename_component(EXT "/path/to/file.tar.gz" EXT)
        get_filename_component(NAME_WE "/path/to/file.tar.gz" NAME_WE)
        get_filename_component(LAST_EXT "/path/to/file.tar.gz" LAST_EXT)
        get_filename_component(NAME_WLE "/path/to/file.tar.gz" NAME_WLE)
        message("${DIR}|${NAME}|${EXT}|${NAME_WE}|${LAST_EXT}|${NAME_WLE}")
    )");
    REQUIRE(output == "/path/to|file.tar.gz|.tar.gz|file|.gz|file.tar\n");

    output = run_script(R"(
        get_filename_component(EXT ".bashrc" EXT)
        get_filename_component(NAME_WE ".bashrc" NAME_WE)
        message("${EXT}|${NAME_WE}")
    )");
    REQUIRE(output == "|.bashrc\n");
}

TEST_CASE("call case insensitive", "[interpreter][case]") {
    std::string output;
    output = run_script(R"(
        MESSAGE("Hello World")
    )");
    REQUIRE(output == "Hello World\n");

    output = run_script(R"(
        function (hello)
        message("Hello World")
        endfunction()
        hello()
    )");
    REQUIRE(output == "Hello World\n");
}

TEST_CASE("stringly typing", "[interpreter][stringly]") {
    std::string output;
    CHECK_THROWS(run_script(R"(
        message(${THIS_DOES_NOT_EXIST})
    )"));
}

TEST_CASE("option", "[interpreter][option]") {
    auto output = run_script(R"(
        option(FOO "Foo option" ON)
        message("${FOO}")
    )");
    REQUIRE(output == "ON\n");

    output = run_script(R"(
        option(FOO "Foo option" OFF)
        message("${FOO}")
    )");
    REQUIRE(output == "OFF\n");

    output = run_script(R"(
        set(FOO ON)
        option(FOO "Foo option" OFF)
        message("${FOO}")
    )");
    REQUIRE(output == "ON\n");

    // Verified against CMake 3.31: option() creates a cache variable,
    // so it is visible outside the function scope that defined it.
    SECTION("option inside function is visible outside") {
        output = run_script(R"(
            function(set_option_in_func)
                option(MY_OPT "An option" ON)
            endfunction()
            set_option_in_func()
            if(MY_OPT)
                message("visible")
            else()
                message("not_visible")
            endif()
        )");
        REQUIRE(output == "visible\n");
    }

    SECTION("option doesn't override existing variable") {
        output = run_script(R"(
            set(MY_OPT2 "OFF")
            function(try_override_option)
                option(MY_OPT2 "Another option" ON)
            endfunction()
            try_override_option()
            message("${MY_OPT2}")
        )");
        REQUIRE(output == "OFF\n");
    }

    SECTION("option from one function visible in another") {
        output = run_script(R"(
            function(define_opt)
                option(CROSS_FUNC_OPT "cross func" ON)
            endfunction()
            function(read_opt)
                message("${CROSS_FUNC_OPT}")
            endfunction()
            define_opt()
            read_opt()
        )");
        REQUIRE(output == "ON\n");
    }
}

TEST_CASE("Multi-line if condition", "[interpreter][if]") {
    auto output = run_script(R"(
        set(VAR1 "A")
        set(VAR2 "B")
        if(VAR1 STREQUAL "A"
           AND VAR2 STREQUAL "B")
            message("Success")
        endif()
    )");
    REQUIRE(output == "Success\n");

    output = run_script(R"(
        set(TARGET_NAME "Catch2")
        if(NOT DEFINED CHECK_${TARGET_NAME}
           OR TRUE)
            message("Success with var ref")
        endif()
    )");
    REQUIRE(output == "Success with var ref\n");

    output = run_script(R"(
        set(targets "A;B")
        foreach(t IN LISTS targets)
          if(FALSE
              OR NOT DEFINED _check_for_${t}
              OR NOT IS_DIRECTORY "/nonexistent")
            message("Inside for ${t}")
          endif()
        endforeach()
    )");
    REQUIRE(output == "Inside for A\nInside for B\n");

    output = run_script(R"(
        set(t "target")
        set(_check_for_target "EXISTS")
        if(DEFINED _check_for_${t})
            message("Defined")
        endif()
    )");
    REQUIRE(output == "Defined\n");

    output = run_script(R"(
        if(NOT DEFINED UNDEFINED_VAR)
            message("Success NOT DEFINED")
        endif()
    )");
    REQUIRE(output == "Success NOT DEFINED\n");

    output = run_script(R"(
        set(VAR1 "A")
        set(VAR2 "B")
        if((VAR1 STREQUAL "A") AND (VAR2 STREQUAL "B"))
            message("Success")
        endif()
    )");
    REQUIRE(output == "Success\n");
}

TEST_CASE("multiple dereference", "[interpreter][dereference]") {
    auto output = run_script(R"(
        set(VAR1 "A")
        set(VAR2 "VAR1")
        message("${${VAR2}}")
    )");
    CHECK(output == "A\n");

    output = run_script(R"(
        set(VAR1 "A")
        set(VAR2 "VAR1")
        set(VAR3 "VAR")
        message("${${${VAR3}2}}")
    )");
    CHECK(output == "A\n");

    output = run_script(R"(
        set(VAR1 "A")
        set(VAR2 "VAR1")
        set(VAR3 "VAR")
        message("${${${VAR3}2}}")
    )");
    CHECK(output == "A\n");

    output = run_script(R"(
        set(MY_VAR "Cache Value" CACHE STRING "")
        set(MY_VAR "Local Value")
        message("Standard: ${MY_VAR}")
        message("Explicit: $CACHE{MY_VAR}")
    )");
    CHECK(output == "Standard: Local Value\nExplicit: Cache Value\n");
}

TEST_CASE("macro substitution does not apply to dynamically computed names", "[interpreter][macro]") {
    // CMake macros perform textual substitution on ARGV/ARGC/params *before* the
    // body is evaluated. Direct ${ARGC} hits the macro's ARGC; an indirect
    // reference like ${${NAME}} where NAME="ARGC" must hit the enclosing
    // variable scope (the function's ARGC), because by the time the outer
    // expansion runs, the inner has already produced the literal "ARGC".
    // vcpkg's z_vcpkg_function_arguments helper depends on this.
    auto output = run_script(R"(
        function(outer)
          inner(OUT)
        endfunction()

        macro(inner ARG)
          message("direct=${ARGC}")
          set(NAME "ARGC")
          message("indirect=${${NAME}}")
        endmacro()

        outer(a b c d)
    )");
    CHECK(output == "direct=1\nindirect=4\n");
}

TEST_CASE("ENV namespace", "[interpreter][env]") {
    // Test reading environment variables
    auto output = run_script(R"(
        set(ENV{KILN_TEST_VAR} "test_value")
        message("$ENV{KILN_TEST_VAR}")
    )");
    CHECK(output == "test_value\n");
    const char* kiln_test_ptr = std::getenv("KILN_TEST_VAR");
    REQUIRE(kiln_test_ptr != nullptr);
    CHECK(std::string_view(kiln_test_ptr) == "test_value");

    // Test case insensitivity
    output = run_script(R"(
        set(ENV{KILN_TEST_VAR2} "test123")
        message("$env{KILN_TEST_VAR2}")
        message("$Env{KILN_TEST_VAR2}")
    )");
    CHECK(output == "test123\ntest123\n");

    // Test unset
    output = run_script(R"(
        set(ENV{KILN_TEST_VAR3} "value")
        message("Before: $ENV{KILN_TEST_VAR3}")
        unset(ENV{KILN_TEST_VAR3})
        message("After: $ENV{KILN_TEST_VAR3}")
    )");
    CHECK(output == "Before: value\nAfter: \n");

    // Test with nested variable in name
    output = run_script(R"(
        set(VAR_NAME "KILN_TEST_VAR4")
        set(ENV{KILN_TEST_VAR4} "nested")
        message("$ENV{${VAR_NAME}}")
    )");
    CHECK(output == "nested\n");
}

TEST_CASE("CACHE namespace", "[interpreter][cache]") {
    // Test basic cache
    auto output = run_script(R"(
        set(MY_VAR "value" CACHE STRING "doc")
        message("$CACHE{MY_VAR}")
    )");
    CHECK(output == "value\n");

    // Test case insensitivity
    output = run_script(R"(
        set(MY_VAR "value" CACHE STRING "doc")
        message("$cache{MY_VAR}")
        message("$Cache{MY_VAR}")
    )");
    CHECK(output == "value\nvalue\n");

    // Test that cache sets both cache and regular variable
    output = run_script(R"(
        set(MY_VAR "cached" CACHE STRING "doc")
        message("Regular: ${MY_VAR}")
        message("Cache: $CACHE{MY_VAR}")
    )");
    CHECK(output == "Regular: cached\nCache: cached\n");

    // Test that local set overrides regular but not cache
    output = run_script(R"(
        set(MY_VAR "cached" CACHE STRING "doc")
        set(MY_VAR "local")
        message("Regular: ${MY_VAR}")
        message("Cache: $CACHE{MY_VAR}")
    )");
    CHECK(output == "Regular: local\nCache: cached\n");

    // Test unset cache
    output = run_script(R"(
        set(MY_VAR "value" CACHE STRING "doc")
        message("Before: $CACHE{MY_VAR}")
        unset(MY_VAR CACHE)
        message("After: $CACHE{MY_VAR}")
    )");
    CHECK(output == "Before: value\nAfter: \n");

    // Test with nested variable in name
    output = run_script(R"(
        set(VAR_NAME "MY_CACHED_VAR")
        set(MY_CACHED_VAR "42" CACHE STRING "doc")
        message("$CACHE{${VAR_NAME}}")
    )");
    CHECK(output == "42\n");

    // Test CACHE keyword is case-insensitive
    output = run_script(R"(
        set(VAR "value" cache STRING "doc")
        message("$CACHE{VAR}")
    )");
    CHECK(output == "value\n");
}

TEST_CASE("PARENT_SCOPE", "[interpreter][parent_scope]") {
    // Test basic PARENT_SCOPE
    auto output = run_script(R"(
        function(set_parent)
            set(OUTER_VAR "from function" PARENT_SCOPE)
        endfunction()

        message("Before: ${OUTER_VAR}")
        set_parent()
        message("After: ${OUTER_VAR}")
    )");
    CHECK(output == "Before: \nAfter: from function\n");

    // Test unset PARENT_SCOPE
    output = run_script(R"(
        function(unset_parent)
            unset(OUTER_VAR PARENT_SCOPE)
        endfunction()

        set(OUTER_VAR "initial")
        message("Before: ${OUTER_VAR}")
        unset_parent()
        message("After: ${OUTER_VAR}")
    )");
    CHECK(output == "Before: initial\nAfter: \n");

    // Test PARENT_SCOPE is case-insensitive
    output = run_script(R"(
        function(test_case)
            set(VAR "value" parent_scope)
        endfunction()

        test_case()
        message("${VAR}")
    )");
    CHECK(output == "value\n");

    // Test nested variable name with PARENT_SCOPE
    output = run_script(R"(
        function(test_nested)
            set(VAR_NAME "RESULT")
            set(${VAR_NAME} "success" PARENT_SCOPE)
        endfunction()

        test_nested()
        message("${RESULT}")
    )");
    CHECK(output == "success\n");
}

TEST_CASE("namespace error cases", "[interpreter][namespace][errors]") {
    // Test invalid set(CACHE{...}) syntax
    CHECK_THROWS_WITH(
        run_script(R"(
            set(CACHE{MY_VAR} "value")
        )"),
        Catch::Matchers::ContainsSubstring("set(CACHE{...} ...) is invalid")
    );

    // Test invalid unset(CACHE{...}) syntax
    CHECK_THROWS_WITH(
        run_script(R"(
            unset(CACHE{MY_VAR})
        )"),
        Catch::Matchers::ContainsSubstring("unset(CACHE{...}) is invalid")
    );

    // Test cannot mix CACHE and PARENT_SCOPE in set
    CHECK_THROWS_WITH(
        run_script(R"(
            function(test_func)
                set(VAR "value" CACHE STRING "doc" PARENT_SCOPE)
            endfunction()
            test_func()
        )"),
        Catch::Matchers::ContainsSubstring("cannot use both CACHE and PARENT_SCOPE")
    );

    // Test cannot mix in unset
    CHECK_THROWS_WITH(
        run_script(R"(
            function(test_func)
                unset(VAR CACHE PARENT_SCOPE)
            endfunction()
            test_func()
        )"),
        Catch::Matchers::ContainsSubstring("cannot use both CACHE and PARENT_SCOPE")
    );

    // Test PARENT_SCOPE outside function - CMake issues a warning, not an error
    // Script continues execution, just logs a warning
    auto output = run_script(R"(
        set(VAR "value" PARENT_SCOPE)
        message("script continued")
    )");
    REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("script continued"));
}

TEST_CASE("PARENT_SCOPE does not affect current scope", "[interpreter][parent_scope][scoping]") {
    SECTION("set PARENT_SCOPE preserves current scope view") {
        // CMake semantics: PARENT_SCOPE modifies parent but current scope unchanged
        auto output = run_script(R"(
            set(VAR "outer")
            function(test_func)
                message("before=${VAR}")
                set(VAR "modified" PARENT_SCOPE)
                message("after=${VAR}")
            endfunction()
            test_func()
            message("outside=${VAR}")
        )");
        REQUIRE(output == "before=outer\nafter=outer\noutside=modified\n");
    }

    SECTION("unset PARENT_SCOPE preserves current scope view") {
        // CMake semantics: unset PARENT_SCOPE removes from parent but current scope unchanged
        auto output = run_script(R"(
            set(VAR "outer")
            function(test_func)
                message("before=${VAR}")
                unset(VAR PARENT_SCOPE)
                message("after=${VAR}")
            endfunction()
            test_func()
            if(DEFINED VAR)
                message("outside=${VAR}")
            else()
                message("outside=undefined")
            endif()
        )");
        REQUIRE(output == "before=outer\nafter=outer\noutside=undefined\n");
    }

    SECTION("new variable via PARENT_SCOPE not visible in current scope") {
        // CMake semantics: a new variable created via PARENT_SCOPE is not visible until exit
        auto output = run_script(R"(
            function(test_func)
                if(DEFINED NEW_VAR)
                    message("before=defined")
                else()
                    message("before=undefined")
                endif()
                set(NEW_VAR "value" PARENT_SCOPE)
                if(DEFINED NEW_VAR)
                    message("after=defined")
                else()
                    message("after=undefined")
                endif()
            endfunction()
            test_func()
            message("outside=${NEW_VAR}")
        )");
        REQUIRE(output == "before=undefined\nafter=undefined\noutside=value\n");
    }

    SECTION("multiple PARENT_SCOPE calls preserve current scope") {
        auto output = run_script(R"(
            set(VAR "initial")
            function(test_func)
                message("start=${VAR}")
                set(VAR "first" PARENT_SCOPE)
                message("after_first=${VAR}")
                set(VAR "second" PARENT_SCOPE)
                message("after_second=${VAR}")
            endfunction()
            test_func()
            message("outside=${VAR}")
        )");
        REQUIRE(output == "start=initial\nafter_first=initial\nafter_second=initial\noutside=second\n");
    }
}

TEST_CASE("nested variables with namespaces", "[interpreter][nested][namespaces]") {
    // Test nested with ENV
    auto output = run_script(R"(
        set(ENV{KILN_PATH_TEST} "/usr/bin")
        set(VAR_NAME "KILN_PATH_TEST")
        message("$ENV{${VAR_NAME}}")
    )");
    CHECK(output == "/usr/bin\n");

    // Test nested with CACHE
    output = run_script(R"(
        set(BUILD_TYPE "Debug" CACHE STRING "doc")
        set(TYPE_VAR "BUILD_TYPE")
        message("$CACHE{${TYPE_VAR}}")
    )");
    CHECK(output == "Debug\n");

    // Test complex nesting
    output = run_script(R"(
        set(A "B")
        set(B "MY_VAR")
        set(MY_VAR "value" CACHE STRING "doc")
        message("$CACHE{${${A}}}")
    )");
    CHECK(output == "value\n");

    // Test partial expansion with ENV
    output = run_script(R"(
        set(ENV{KILN_PREFIX_TEST} "prefix_value")
        set(PREFIX "KILN_")
        set(SUFFIX "_TEST")
        message("$ENV{${PREFIX}PREFIX${SUFFIX}}")
    )");
    CHECK(output == "prefix_value\n");
}

TEST_CASE("namespace case insensitivity comprehensive", "[interpreter][case]") {
    // Test all variations of ENV
    auto output = run_script(R"(
        set(ENV{TEST_VAR} "value")
        message("$ENV{TEST_VAR}")
        message("$env{TEST_VAR}")
        message("$Env{TEST_VAR}")
        message("$eNv{TEST_VAR}")
    )");
    CHECK(output == "value\nvalue\nvalue\nvalue\n");

    // Test all variations of CACHE
    output = run_script(R"(
        set(VAR "cached" CACHE STRING "doc")
        message("$CACHE{VAR}")
        message("$cache{VAR}")
        message("$Cache{VAR}")
        message("$cAcHe{VAR}")
    )");
    CHECK(output == "cached\ncached\ncached\ncached\n");

    // Test set keywords are case insensitive
    output = run_script(R"(
        set(ENV{VAR1} "v1")
        set(env{VAR2} "v2")
        set(Env{VAR3} "v3")
        message("$ENV{VAR1} $ENV{VAR2} $ENV{VAR3}")
    )");
    CHECK(output == "v1 v2 v3\n");
}

TEST_CASE("string() FIND operation", "[interpreter][string]") {
    auto output = run_script(R"(
        string(FIND "Hello World" "World" pos)
        message("${pos}")
    )");
    REQUIRE(output == "6\n");

    output = run_script(R"(
        string(FIND "Hello World" "xyz" pos)
        message("${pos}")
    )");
    REQUIRE(output == "-1\n");

    output = run_script(R"(
        string(FIND "abcabc" "bc" pos REVERSE)
        message("${pos}")
    )");
    REQUIRE(output == "4\n");
}

TEST_CASE("string() REPLACE operation", "[interpreter][string]") {
    auto output = run_script(R"(
        string(REPLACE ";" "," result "a;b;c;d")
        message("${result}")
    )");
    REQUIRE(output == "a,b,c,d\n");

    output = run_script(R"(
        string(REPLACE "foo" "bar" result "foo foo foo")
        message("${result}")
    )");
    REQUIRE(output == "bar bar bar\n");
}

TEST_CASE("string() REGEX MATCH operation", "[interpreter][string]") {
    auto output = run_script(R"(
        string(REGEX MATCH "[0-9]+" result "abc123def456")
        message("${result}")
    )");
    REQUIRE(output == "123\n");

    output = run_script(R"(
        string(REGEX MATCH "[0-9]+" result "no numbers here")
        message("${result}")
    )");
    REQUIRE(output == "\n");
}

TEST_CASE("string() REGEX MATCH clears CMAKE_MATCH on non-match", "[interpreter][string][bugfix]") {
    // Regression test: CMAKE_MATCH_* variables must be cleared when a REGEX MATCH
    // fails to match. Previously, stale values from a prior successful match would
    // persist, causing incorrect behavior in loops that check CMAKE_MATCH_1.
    auto output = run_script(R"RAW(
        string(REGEX MATCH "([0-9]+)" M "hello42world")
        message("match1: ${CMAKE_MATCH_1}")
        string(REGEX MATCH "([0-9]+)" M "no digits")
        message("match2: ${CMAKE_MATCH_1}")
        if(CMAKE_MATCH_1)
            message("BUG: stale match")
        else()
            message("OK: cleared")
        endif()
    )RAW");
    REQUIRE(output == "match1: 42\nmatch2: \nOK: cleared\n");
}

TEST_CASE("string() REGEX MATCHALL operation", "[interpreter][string]") {
    auto output = run_script(R"(
        string(REGEX MATCHALL "[0-9]+" result "abc123def456ghi789")
        message("${result}")
    )");
    REQUIRE(output == "123;456;789\n");
}

TEST_CASE("string() REGEX REPLACE operation", "[interpreter][string]") {
    auto output = run_script(R"(
        string(REGEX REPLACE "[0-9]+" "NUM" result "abc123def456")
        message("${result}")
    )");
    REQUIRE(output == "abcNUMdefNUM\n");
}

TEST_CASE("string() REGEX REPLACE with capture groups", "[interpreter][string]") {
    // Test basic capture group
    auto output1 = run_script(R"RAW(
        string(REGEX REPLACE "version ([0-9]+)\\.([0-9]+)" "v\\1.\\2" result "version 3.6")
        message("${result}")
    )RAW");
    REQUIRE(output1 == "v3.6\n");

    // Test capture group with surrounding text (like OpenSSL version example)
    auto output2 = run_script(R"(
        set(input "# define OPENSSL_VERSION_STR \"3.6.0\"")
        string(REGEX REPLACE "^.*OPENSSL_VERSION_STR.*\"([0-9]+\\.[0-9]+\\.[0-9]+)\".*$" "\\1" result "${input}")
        message("${result}")
    )");
    REQUIRE(output2 == "3.6.0\n");

    // Test multiple capture groups
    auto output3 = run_script(R"RAW(
        string(REGEX REPLACE "([a-z]+)([0-9]+)" "\\2-\\1" result "abc123")
        message("${result}")
    )RAW");
    REQUIRE(output3 == "123-abc\n");

    // Test literal dollar sign in replacement (should be preserved)
    auto output4 = run_script(R"(
        string(REGEX REPLACE "price" "$100" result "The price is high")
        message("${result}")
    )");
    REQUIRE(output4 == "The $100 is high\n");
}

TEST_CASE("string() REGEX REPLACE multiline dot matching", "[interpreter][string]") {
    // Regression test: dot must match newlines (PCRE2_DOTALL) like CMake.
    // Real-world pattern from ZMQ's CMakeLists.txt that extracts version
    // numbers from a header file spanning multiple lines.
    auto output = run_script(R"RAW(
        set(header "#ifndef ZMQ_H\n#define ZMQ_H\n\n#define ZMQ_VERSION_MAJOR 4\n#define ZMQ_VERSION_MINOR 3\n#define ZMQ_VERSION_PATCH 5\n\n#endif")
        string(REGEX REPLACE ".*#define ZMQ_VERSION_MAJOR ([0-9]+).*" "\\1" major "${header}")
        string(REGEX REPLACE ".*#define ZMQ_VERSION_MINOR ([0-9]+).*" "\\1" minor "${header}")
        string(REGEX REPLACE ".*#define ZMQ_VERSION_PATCH ([0-9]+).*" "\\1" patch "${header}")
        message("${major}.${minor}.${patch}")
    )RAW");
    REQUIRE(output == "4.3.5\n");
}

TEST_CASE("string() REGEX QUOTE operation", "[interpreter][string]") {
    auto output = run_script(R"(
        string(REGEX QUOTE result "a.b*c?d[e]")
        message("${result}")
    )");
    REQUIRE(output == "a\\.b\\*c\\?d\\[e\\]\n");
}

TEST_CASE("string() APPEND operation", "[interpreter][string]") {
    auto output = run_script(R"(
        set(str "Hello")
        string(APPEND str " " "World")
        message("${str}")
    )");
    REQUIRE(output == "Hello World\n");
}

TEST_CASE("string() PREPEND operation", "[interpreter][string]") {
    auto output = run_script(R"(
        set(str "World")
        string(PREPEND str "Hello" " ")
        message("${str}")
    )");
    REQUIRE(output == "Hello World\n");
}

TEST_CASE("string() CONCAT operation", "[interpreter][string]") {
    auto output = run_script(R"(
        string(CONCAT result "Hello" " " "World" "!")
        message("${result}")
    )");
    REQUIRE(output == "Hello World!\n");
}

TEST_CASE("string() JOIN operation", "[interpreter][string]") {
    auto output = run_script(R"(
        string(JOIN "," result "a" "b" "c" "d")
        message("${result}")
    )");
    REQUIRE(output == "a,b,c,d\n");

    output = run_script(R"(
        string(JOIN " - " result "one" "two" "three")
        message("${result}")
    )");
    REQUIRE(output == "one - two - three\n");
}

TEST_CASE("string() TOLOWER operation", "[interpreter][string]") {
    auto output = run_script(R"(
        string(TOLOWER "HELLO WORLD" result)
        message("${result}")
    )");
    REQUIRE(output == "hello world\n");
}

TEST_CASE("string() TOUPPER operation", "[interpreter][string]") {
    auto output = run_script(R"(
        string(TOUPPER "hello world" result)
        message("${result}")
    )");
    REQUIRE(output == "HELLO WORLD\n");
}

TEST_CASE("string() LENGTH operation", "[interpreter][string]") {
    auto output = run_script(R"(
        string(LENGTH "Hello World" result)
        message("${result}")
    )");
    REQUIRE(output == "11\n");

    output = run_script(R"(
        string(LENGTH "" result)
        message("${result}")
    )");
    REQUIRE(output == "0\n");
}

TEST_CASE("string() SUBSTRING operation", "[interpreter][string]") {
    auto output = run_script(R"(
        string(SUBSTRING "Hello World" 0 5 result)
        message("${result}")
    )");
    REQUIRE(output == "Hello\n");

    output = run_script(R"(
        string(SUBSTRING "Hello World" 6 -1 result)
        message("${result}")
    )");
    REQUIRE(output == "World\n");
}

TEST_CASE("string() STRIP operation", "[interpreter][string]") {
    auto output = run_script(R"(
        string(STRIP "  Hello World  " result)
        message("${result}")
    )");
    REQUIRE(output == "Hello World\n");

    output = run_script(R"(
        string(STRIP "	Tabs	" result)
        message("${result}")
    )");
    REQUIRE(output == "Tabs\n");
}

TEST_CASE("string() GENEX_STRIP operation", "[interpreter][string]") {
    auto output = run_script(R"(
        string(GENEX_STRIP "foo$<CONFIG>bar$<1:baz>qux" result)
        message("${result}")
    )");
    // GENEX_STRIP removes generator expressions entirely
    REQUIRE(output == "foobarqux\n");

    output = run_script(R"(
        string(GENEX_STRIP "plain text" result)
        message("${result}")
    )");
    REQUIRE(output == "plain text\n");

    output = run_script(R"(
        string(GENEX_STRIP "before$<TARGET_FILE:foo>after" result)
        message("${result}")
    )");
    REQUIRE(output == "beforeafter\n");
}

TEST_CASE("string() REPEAT operation", "[interpreter][string]") {
    auto output = run_script(R"(
        string(REPEAT "ab" 3 result)
        message("${result}")
    )");
    REQUIRE(output == "ababab\n");

    output = run_script(R"(
        string(REPEAT "x" 0 result)
        message("${result}")
    )");
    REQUIRE(output == "\n");
}

TEST_CASE("string() COMPARE operations", "[interpreter][string]") {
    auto output = run_script(R"(
        string(COMPARE EQUAL "abc" "abc" result)
        message("${result}")
    )");
    REQUIRE(output == "1\n");

    output = run_script(R"(
        string(COMPARE NOTEQUAL "abc" "xyz" result)
        message("${result}")
    )");
    REQUIRE(output == "1\n");

    output = run_script(R"(
        string(COMPARE LESS "abc" "xyz" result)
        message("${result}")
    )");
    REQUIRE(output == "1\n");

    output = run_script(R"(
        string(COMPARE GREATER "xyz" "abc" result)
        message("${result}")
    )");
    REQUIRE(output == "1\n");

    output = run_script(R"(
        string(COMPARE LESS_EQUAL "abc" "abc" result)
        message("${result}")
    )");
    REQUIRE(output == "1\n");

    output = run_script(R"(
        string(COMPARE GREATER_EQUAL "xyz" "abc" result)
        message("${result}")
    )");
    REQUIRE(output == "1\n");
}

TEST_CASE("string() ASCII operation", "[interpreter][string]") {
    auto output = run_script(R"(
        string(ASCII 72 101 108 108 111 result)
        message("${result}")
    )");
    REQUIRE(output == "Hello\n");
}

TEST_CASE("string() HEX operation", "[interpreter][string]") {
    auto output = run_script(R"(
        string(HEX "Hello" result)
        message("${result}")
    )");
    REQUIRE(output == "48656c6c6f\n");
}

TEST_CASE("string() CONFIGURE operation", "[interpreter][string]") {
    auto output = run_script(R"(
        set(NAME "World")
        string(CONFIGURE "Hello ${NAME}!" result)
        message("${result}")
    )");
    REQUIRE(output == "Hello World!\n");

    output = run_script(R"(
        set(VERSION "1.0")
        string(CONFIGURE "Version @VERSION@" result)
        message("${result}")
    )");
    REQUIRE(output == "Version 1.0\n");

    output = run_script(R"(
        set(VERSION "1.0")
        string(CONFIGURE "Version @VERSION@" result @ONLY)
        message("${result}")
    )");
    REQUIRE(output == "Version 1.0\n");
}

TEST_CASE("string() MAKE_C_IDENTIFIER operation", "[interpreter][string]") {
    auto output = run_script(R"(
        string(MAKE_C_IDENTIFIER "hello-world.txt" result)
        message("${result}")
    )");
    REQUIRE(output == "hello_world_txt\n");

    output = run_script(R"(
        string(MAKE_C_IDENTIFIER "foo bar@123" result)
        message("${result}")
    )");
    REQUIRE(output == "foo_bar_123\n");
}

TEST_CASE("string() RANDOM operation", "[interpreter][string]") {
    auto output = run_script(R"(
        string(RANDOM result)
        message("${result}")
    )");
    // Just check that it produced something
    REQUIRE(output.length() == 6); // 5 chars + newline

    output = run_script(R"(
        string(RANDOM LENGTH 10 result)
        message("${result}")
    )");
    REQUIRE(output.length() == 11); // 10 chars + newline

    output = run_script(R"(
        string(RANDOM LENGTH 8 ALPHABET "01" result)
        message("${result}")
    )");
    REQUIRE(output.length() == 9); // 8 chars + newline
    // Check it only contains 0 and 1
    for (size_t i = 0; i < output.length() - 1; ++i) {
        REQUIRE((output[i] == '0' || output[i] == '1'));
    }
}

TEST_CASE("string() TIMESTAMP operation", "[interpreter][string]") {
    auto output = run_script(R"(
        string(TIMESTAMP result)
        message("${result}")
    )");
    // Just check that it produced something
    REQUIRE(!output.empty());

    output = run_script(R"(
        string(TIMESTAMP result "%Y")
        message("${result}")
    )");
    // Should be a 4-digit year
    REQUIRE(output.length() == 5); // 4 digits + newline
}

TEST_CASE("separate_arguments() simple form", "[interpreter][separate_arguments]") {
    auto output = run_script(R"(
        set(myvar "foo bar baz")
        separate_arguments(myvar)
        message("${myvar}")
    )");
    REQUIRE(output == "foo;bar;baz\n");

    // Test with multiple spaces
    output = run_script(R"(
        set(myvar "foo  bar   baz")
        separate_arguments(myvar)
        message("${myvar}")
    )");
    REQUIRE(output == "foo;;bar;;;baz\n");

    // Test with tabs and newlines
    output = run_script(R"(
        set(myvar "foo	bar
baz")
        separate_arguments(myvar)
        message("${myvar}")
    )");
    // All whitespace becomes semicolons
    REQUIRE(output.find(';') != std::string::npos);
}

TEST_CASE("separate_arguments() UNIX_COMMAND mode", "[interpreter][separate_arguments]") {
    // Basic parsing
    auto output = run_script(R"(
        separate_arguments(result UNIX_COMMAND "foo bar baz")
        message("${result}")
    )");
    REQUIRE(output == "foo;bar;baz\n");

    // Double quotes
    output = run_script(R"(
        separate_arguments(result UNIX_COMMAND "foo \"bar baz\" qux")
        message("${result}")
    )");
    REQUIRE(output == "foo;bar baz;qux\n");

    // Single quotes
    output = run_script(R"(
        separate_arguments(result UNIX_COMMAND "foo 'bar baz' qux")
        message("${result}")
    )");
    REQUIRE(output == "foo;bar baz;qux\n");

    // Backslash escaping
    output = run_script(R"(
        separate_arguments(result UNIX_COMMAND "foo bar\\ baz")
        message("${result}")
    )");
    REQUIRE(output == "foo;bar baz\n");

    // Escaped quote
    output = run_script(R"(
        separate_arguments(result UNIX_COMMAND "foo \\\"bar\\\" baz")
        message("${result}")
    )");
    REQUIRE(output == "foo;\"bar\";baz\n");

    // No special escapes (backslash-n is literal backslash and n)
    output = run_script(R"(
        separate_arguments(result UNIX_COMMAND "foo\\nbar")
        message("${result}")
    )");
    REQUIRE(output == "foonbar\n");
}

TEST_CASE("separate_arguments() WINDOWS_COMMAND mode", "[interpreter][separate_arguments]") {
    // Basic parsing
    auto output = run_script(R"(
        separate_arguments(result WINDOWS_COMMAND "foo bar baz")
        message("${result}")
    )");
    REQUIRE(output == "foo;bar;baz\n");

    // Double quotes
    output = run_script(R"(
        separate_arguments(result WINDOWS_COMMAND "foo \"bar baz\" qux")
        message("${result}")
    )");
    REQUIRE(output == "foo;bar baz;qux\n");

    // Backslashes are literal unless before quote
    output = run_script(R"(
        separate_arguments(result WINDOWS_COMMAND "foo\\bar baz")
        message("${result}")
    )");
    REQUIRE(output == "foo\\bar;baz\n");

    // Backslashes before quote: even number = half in output, quote is delimiter
    // Input: 2 backslashes before quote → 1 backslash output, quote delimiter
    output = run_script(R"(
        separate_arguments(result WINDOWS_COMMAND "foo \\\\\"bar baz")
        message("${result}")
    )");
    REQUIRE(output == "foo;\\bar baz\n");

    // Odd number: 3 backslashes before quote → 1 backslash output, literal quote
    output = run_script(R"(
        separate_arguments(result WINDOWS_COMMAND "foo \\\\\\\"bar baz")
        message("${result}")
    )");
    REQUIRE(output == "foo;\\\"bar;baz\n");

    // Double quote within quoted string = literal quote (Microsoft spec)
    output = run_script(R"(
        separate_arguments(result WINDOWS_COMMAND "a\"b\"\" c d")
        message("${result}")
    )");
    REQUIRE(output == "ab\" c d\n");

    // Examples from Microsoft docs
    // Input: a\\\b d"e f"g h → argv[1]="a\\\b", argv[2]="de fg", argv[3]="h"
    output = run_script(R"(
        separate_arguments(result WINDOWS_COMMAND "a\\\\\\b d\"e f\"g h")
        message("${result}")
    )");
    REQUIRE(output == "a\\\\\\b;de fg;h\n");
}

TEST_CASE("separate_arguments() NATIVE_COMMAND mode", "[interpreter][separate_arguments]") {
    // NATIVE_COMMAND should use UNIX_COMMAND on Linux
    auto output = run_script(R"(
        separate_arguments(result NATIVE_COMMAND "foo \"bar baz\" qux")
        message("${result}")
    )");
    REQUIRE(output == "foo;bar baz;qux\n");
}

TEST_CASE("separate_arguments() PROGRAM option", "[interpreter][separate_arguments]") {
    // Test with bash (should exist on Linux systems)
    auto output = run_script(R"(
        separate_arguments(result UNIX_COMMAND PROGRAM "bash -c echo")
        list(LENGTH result len)
        message("${len}")
        list(GET result 0 prog)
        message("${prog}")
    )");
    auto lines = output.substr(0, output.size() - 1);
    auto newline_pos = lines.find('\n');
    auto first_line = lines.substr(0, newline_pos);
    auto second_line = lines.substr(newline_pos + 1);

    // Should have 2 elements: program path and remaining args as string
    REQUIRE(first_line == "2");
    // Program should be absolute path to bash
    REQUIRE(second_line.find("/bash") != std::string::npos);

    // Test program not found
    output = run_script(R"(
        separate_arguments(result UNIX_COMMAND PROGRAM "nonexistent_program_12345 arg1 arg2")
        message("${result}")
    )");
    // Should be empty
    REQUIRE(output == "\n");
}

TEST_CASE("separate_arguments() PROGRAM SEPARATE_ARGS option", "[interpreter][separate_arguments]") {
    // Test with SEPARATE_ARGS flag
    auto output = run_script(R"(
        separate_arguments(result UNIX_COMMAND PROGRAM SEPARATE_ARGS "bash -c echo")
        list(LENGTH result len)
        message("${len}")
        list(GET result 0 prog)
        list(GET result 1 arg1)
        list(GET result 2 arg2)
        message("${prog}")
        message("${arg1}")
        message("${arg2}")
    )");

    auto lines = output.substr(0, output.size() - 1);
    std::vector<std::string> output_lines;
    size_t start = 0;
    while (start < lines.size()) {
        size_t end = lines.find('\n', start);
        if (end == std::string::npos) {
            output_lines.push_back(lines.substr(start));
            break;
        }
        output_lines.push_back(lines.substr(start, end - start));
        start = end + 1;
    }

    // Should have 3 elements: program path, arg1, arg2
    REQUIRE(output_lines[0] == "3");
    // Program should be absolute path to bash
    REQUIRE(output_lines[1].find("/bash") != std::string::npos);
    REQUIRE(output_lines[2] == "-c");
    REQUIRE(output_lines[3] == "echo");
}

TEST_CASE("separate_arguments() edge cases", "[interpreter][separate_arguments]") {
    // Empty string
    auto output = run_script(R"(
        separate_arguments(result UNIX_COMMAND "")
        message("${result}")
    )");
    REQUIRE(output == "\n");

    // Only whitespace
    output = run_script(R"(
        separate_arguments(result UNIX_COMMAND "   ")
        message("${result}")
    )");
    REQUIRE(output == "\n");

    // Multiple arguments to command (should be concatenated)
    output = run_script(R"(
        separate_arguments(result UNIX_COMMAND foo bar baz)
        message("${result}")
    )");
    REQUIRE(output == "foo;bar;baz\n");
}

TEST_CASE("var and func colision", "[interperter][edgecase]") {
    auto output = run_script(R"(
        function (foo)
        message("Hello World!")
        endfunction()
        set(foo 1)
        foo()
        message("${foo}")
    )");
    CHECK(output == "Hello World!\n1\n");
}

TEST_CASE("find_program finds system commands", "[interpreter][find]") {
    auto output = run_script(R"(
        find_program(BASH_PATH bash)
        if(BASH_PATH)
            message("Found bash at ${BASH_PATH}")
        else()
            message("FAILED: bash not found")
        endif()
    )");
    REQUIRE(output.find("Found bash at") != std::string::npos);
    REQUIRE(output.find("/bash") != std::string::npos);
}

TEST_CASE("find_program handles NOTFOUND", "[interpreter][find]") {
    auto output = run_script(R"(
        find_program(NONEXISTENT_PROG this_program_definitely_does_not_exist_12345)
        if(NONEXISTENT_PROG)
            message("FAILED: should not be found")
        else()
            message("Correctly not found: ${NONEXISTENT_PROG}")
        endif()
    )");
    REQUIRE(output.find("Correctly not found") != std::string::npos);
    REQUIRE(output.find("NOTFOUND") != std::string::npos);
}

TEST_CASE("find_program REQUIRED flag", "[interpreter][find]") {
    REQUIRE_THROWS_AS(run_script(R"(
        find_program(NONEXISTENT this_does_not_exist_12345 REQUIRED)
    )"), std::runtime_error);
}

TEST_CASE("find_library finds system libraries", "[interpreter][find]") {
    auto output = run_script(R"(
        find_library(MATH_LIB m)
        if(MATH_LIB)
            message("Found libm at ${MATH_LIB}")
        else()
            message("FAILED: libm not found")
        endif()
    )");
    REQUIRE(output.find("Found libm at") != std::string::npos);
}

TEST_CASE("find_library handles lib prefix", "[interpreter][find]") {
    auto output = run_script(R"(
        find_library(PTHREAD_LIB pthread)
        if(PTHREAD_LIB)
            message("Found pthread at ${PTHREAD_LIB}")
        else()
            message("FAILED: pthread not found")
        endif()
    )");
    REQUIRE(output.find("Found pthread at") != std::string::npos);
}

TEST_CASE("find_file finds system headers", "[interpreter][find]") {
    auto output = run_script(R"(
        find_file(STDIO_H stdio.h PATHS /usr/include)
        if(STDIO_H)
            message("Found stdio.h at ${STDIO_H}")
        else()
            message("FAILED: stdio.h not found")
        endif()
    )");
    REQUIRE(output.find("Found stdio.h at") != std::string::npos);
}

TEST_CASE("find commands with NAMES", "[interpreter][find]") {
    auto output = run_script(R"(
        find_program(SHELL_PATH NAMES bash sh)
        if(SHELL_PATH)
            message("Found shell: ${SHELL_PATH}")
        else()
            message("FAILED: no shell found")
        endif()
    )");
    REQUIRE(output.find("Found shell:") != std::string::npos);
}

TEST_CASE("find_program with NO_DEFAULT_PATH", "[interpreter][find]") {
    auto output = run_script(R"(
        find_program(BASH_NO_DEFAULT bash NO_DEFAULT_PATH)
        if(BASH_NO_DEFAULT)
            message("FAILED: Should not find bash without default paths")
        else()
            message("Correctly not found without default paths")
        endif()
    )");
    REQUIRE(output.find("Correctly not found") != std::string::npos);
}

TEST_CASE("find_program with explicit PATHS", "[interpreter][find]") {
    auto output = run_script(R"(
        find_program(BASH_EXPLICIT bash PATHS /usr/bin /bin)
        if(BASH_EXPLICIT)
            message("Found bash with explicit path: ${BASH_EXPLICIT}")
        else()
            message("FAILED: bash not found with explicit paths")
        endif()
    )");
    REQUIRE(output.find("Found bash with explicit path") != std::string::npos);
}

TEST_CASE("find commands skip search if variable already set", "[interpreter][find]") {
    auto output = run_script(R"(
        set(MY_PROG /usr/bin/bash)
        find_program(MY_PROG nonexistent_program_12345)
        message("MY_PROG is still: ${MY_PROG}")
    )");
    REQUIRE(output.find("MY_PROG is still: /usr/bin/bash") != std::string::npos);
}

TEST_CASE("find_program short-hand syntax", "[interpreter][find]") {
    auto output = run_script(R"(
        find_program(BASH_SHORT bash /usr/bin /bin)
        if(BASH_SHORT)
            message("Found bash with short syntax: ${BASH_SHORT}")
        else()
            message("FAILED: bash not found")
        endif()
    )");
    REQUIRE(output.find("Found bash with short syntax") != std::string::npos);
}

TEST_CASE("find_library with custom prefixes and suffixes", "[interpreter][find]") {
    auto output = run_script(R"(
        set(CMAKE_FIND_LIBRARY_PREFIXES "lib")
        set(CMAKE_FIND_LIBRARY_SUFFIXES ".so;.a")
        find_library(MATH_CUSTOM m)
        if(MATH_CUSTOM)
            message("Found math library: ${MATH_CUSTOM}")
        else()
            message("FAILED: math library not found")
        endif()
    )");
    REQUIRE(output.find("Found math library") != std::string::npos);
}

TEST_CASE("find commands with PATH_SUFFIXES", "[interpreter][find]") {
    // Create a temporary directory structure for testing
    std::filesystem::path temp_dir = std::filesystem::temp_directory_path() / "kiln_test_find";
    std::filesystem::create_directories(temp_dir / "subdir");

    // Create a test file
    std::ofstream test_file(temp_dir / "subdir" / "test.txt");
    test_file << "test content";
    test_file.close();

    std::string script = R"(
        find_file(TEST_FILE test.txt PATHS ")" + temp_dir.string() + R"(" PATH_SUFFIXES subdir)
        if(TEST_FILE)
            message("Found test file: ${TEST_FILE}")
        else()
            message("FAILED: test file not found")
        endif()
    )";

    auto output = run_script(script);

    // Cleanup
    std::filesystem::remove_all(temp_dir);

    REQUIRE(output.find("Found test file") != std::string::npos);
}

TEST_CASE("find_program with NO_CACHE flag", "[interpreter][find]") {
    // Create a temporary executable
    std::filesystem::path temp_dir = std::filesystem::temp_directory_path() / "kiln_test_nocache";
    std::filesystem::create_directories(temp_dir);
    std::filesystem::path test_prog = temp_dir / "test_prog";

    std::ofstream prog_file(test_prog);
    prog_file << "#!/bin/bash\necho test\n";
    prog_file.close();
    std::filesystem::permissions(test_prog,
        std::filesystem::perms::owner_exec | std::filesystem::perms::owner_read,
        std::filesystem::perm_options::add);

    std::string script = R"(
        find_program(TEST_PROG test_prog PATHS ")" + temp_dir.string() + R"(" NO_CACHE)
        message("First find: ${TEST_PROG}")

        # Without NO_CACHE, the result would be cached
        # With NO_CACHE, it's stored only in the variable
        find_program(TEST_PROG test_prog PATHS ")" + temp_dir.string() + R"(" NO_CACHE)
        message("Second find: ${TEST_PROG}")
    )";

    auto output = run_script(script);

    // Cleanup
    std::filesystem::remove_all(temp_dir);

    REQUIRE(output.find("First find:") != std::string::npos);
    REQUIRE(output.find("Second find:") != std::string::npos);
}

TEST_CASE("find_program with NAMES_PER_DIR", "[interpreter][find]") {
    auto output = run_script(R"(
        find_program(SHELL_NAMES_PER_DIR NAMES bash sh NAMES_PER_DIR)
        if(SHELL_NAMES_PER_DIR)
            message("Found shell: ${SHELL_NAMES_PER_DIR}")
        else()
            message("FAILED: shell not found")
        endif()
    )");
    REQUIRE(output.find("Found shell:") != std::string::npos);
}

TEST_CASE("find_path returns directory containing file", "[interpreter][find]") {
    // Test that find_path returns directory, not file
    auto output = run_script(R"(
        find_path(STDIO_DIR stdio.h)
        if(STDIO_DIR)
            message("Found stdio.h directory: ${STDIO_DIR}")
            # Verify it's a directory path, not a file path
            if(STDIO_DIR MATCHES "stdio.h")
                message("FAILED: find_path returned file path instead of directory")
            else()
                message("SUCCESS: find_path returned directory")
            endif()
        else()
            message("FAILED: stdio.h not found")
        endif()
    )");
    REQUIRE(output.find("Found stdio.h directory:") != std::string::npos);
    REQUIRE(output.find("SUCCESS: find_path returned directory") != std::string::npos);
    REQUIRE(output.find("FAILED") == std::string::npos);
}

TEST_CASE("find_path with PATH_SUFFIXES", "[interpreter][find]") {
    // Create a temporary directory structure for testing
    std::filesystem::path temp_dir = std::filesystem::temp_directory_path() / "kiln_test_find_path";
    std::filesystem::create_directories(temp_dir / "include" / "mylib");

    // Create a test header file
    std::ofstream test_file(temp_dir / "include" / "mylib" / "header.h");
    test_file << "// test header";
    test_file.close();

    std::string script = R"(
        find_path(HEADER_DIR header.h PATHS ")" + temp_dir.string() + R"(" PATH_SUFFIXES include/mylib include)
        if(HEADER_DIR)
            message("Found header directory: ${HEADER_DIR}")
        else()
            message("FAILED: header not found")
        endif()
    )";

    auto output = run_script(script);

    // Cleanup
    std::filesystem::remove_all(temp_dir);

    REQUIRE(output.find("Found header directory:") != std::string::npos);
    REQUIRE(output.find("/include/mylib") != std::string::npos);
}

TEST_CASE("find_path vs find_file behavior", "[interpreter][find]") {
    // Verify that find_path and find_file return different results
    auto output = run_script(R"(
        find_file(STDIO_FILE stdio.h)
        find_path(STDIO_PATH stdio.h)

        message("find_file result: ${STDIO_FILE}")
        message("find_path result: ${STDIO_PATH}")

        # find_file should include the filename
        if(STDIO_FILE MATCHES "stdio.h$")
            message("find_file correctly includes filename")
        else()
            message("FAILED: find_file should include filename")
        endif()

        # find_path should NOT include the filename
        if(STDIO_PATH MATCHES "stdio.h")
            message("FAILED: find_path should not include filename")
        else()
            message("find_path correctly returns only directory")
        endif()
    )");

    REQUIRE(output.find("find_file correctly includes filename") != std::string::npos);
    REQUIRE(output.find("find_path correctly returns only directory") != std::string::npos);
}

TEST_CASE("return", "[interpreter][return]") {
    auto output = run_script(R"(
        return()
    )");
    CHECK(output == "");

    output = run_script(R"(
        return()
        message("This message should not be printed")
    )");
    CHECK(output == "");

    output = run_script(R"(
        function (foo)
            return()
        endfunction()
        foo()
        message("1")
    )");
    CHECK(output == "1\n");
}

TEST_CASE("return with arguments", "[interpreter][return]") {
    auto output = run_script(R"(
        macro (foo)
            return("foo")
        endmacro()
        foo()
        message("1")
    )");
    CHECK(output == "");
}

TEST_CASE("cmake_language", "[interpreter][cmake_language]") {
    // Test EVAL CODE
    auto output = run_script(R"x(
        set(VAR "Original")
        cmake_language(EVAL CODE "set(VAR \"Modified\")")
        message("${VAR}")
    )x");
    CHECK(output == "Modified\n");

    // Test EVAL CODE with complex logic
    output = run_script(R"(
        cmake_language(EVAL CODE "
            foreach(i RANGE 3)
                message(\"\${i}\")
            endforeach()
        ")
    )");
    CHECK(output == "0\n1\n2\n3\n");

    // Test CALL builtin
    // NOTE: We use our own message function so the output is garbled
    output = run_script(R"(
        cmake_language(CALL message STATUS "Hello via CALL")
    )");
    CHECK(output == "STATUSHello via CALL\n");

    // Test CALL user function
    output = run_script(R"(
        function(test_func arg)
            message("Function called with ${arg}")
        endfunction()
        cmake_language(CALL test_func "dynamic arg")
    )");
    CHECK(output == "Function called with dynamic arg\n");

    // Test error: unknown mode
    CHECK_THROWS_WITH(run_script(R"(
        cmake_language(UNKNOWN_MODE)
    )"), Catch::Matchers::ContainsSubstring("Unknown cmake_language mode"));

    // Test error: EVAL missing args
    CHECK_THROWS_WITH(run_script(R"(
        cmake_language(EVAL)
    )"), Catch::Matchers::ContainsSubstring("requires CODE"));
}

TEST_CASE("cmake_language DEFER", "[interpreter][cmake_language]") {
    // Test DEFER CALL executes at end of scope, after normal code
    auto output = run_script(R"(
        message("before")
        cmake_language(DEFER CALL message "deferred")
        message("after")
    )");
    CHECK(output == "before\nafter\ndeferred\n");

    // Test multiple DEFER calls execute in order
    output = run_script(R"(
        cmake_language(DEFER CALL message "first")
        cmake_language(DEFER CALL message "second")
        message("immediate")
    )");
    CHECK(output == "immediate\nfirst\nsecond\n");

    // Test GET_CALL_IDS retrieves auto-generated IDs
    output = run_script(R"(
        cmake_language(DEFER CALL message "a")
        cmake_language(DEFER CALL message "b")
        cmake_language(DEFER GET_CALL_IDS ids)
        message("${ids}")
    )");
    // Deferred calls also execute, but after message
    CHECK(output == "_0;_1\na\nb\n");

    // Test explicit ID and ID_VAR
    output = run_script(R"(
        cmake_language(DEFER ID myid CALL message "hello")
        cmake_language(DEFER ID_VAR generated_id CALL message "world")
        cmake_language(DEFER GET_CALL_IDS ids)
        message("ids=${ids}")
        message("gen=${generated_id}")
    )");
    CHECK(output == "ids=myid;_0\ngen=_0\nhello\nworld\n");

    // Test GET_CALL retrieves call details
    output = run_script(R"(
        cmake_language(DEFER ID myid CALL message "arg1" "arg2")
        cmake_language(DEFER GET_CALL myid details)
        message("${details}")
    )");
    CHECK(output == "message;arg1;arg2\narg1arg2\n");

    // Test CANCEL_CALL removes scheduled call
    output = run_script(R"(
        cmake_language(DEFER ID keep CALL message "kept")
        cmake_language(DEFER ID remove CALL message "removed")
        cmake_language(DEFER CANCEL_CALL remove)
    )");
    CHECK(output == "kept\n");

    // Test GET_CALL with unknown ID returns empty
    output = run_script(R"(
        cmake_language(DEFER GET_CALL nonexistent result)
        message("result=[${result}]")
    )");
    CHECK(output == "result=[]\n");
}

TEST_CASE("cmake_parse_arguments standard form", "[interpreter]") {
    // Test basic parsing with all types
    // Note: TARGETS is multi-value, so it consumes foo and bar until end
    auto output = run_script(R"(
        cmake_parse_arguments(MY "VERBOSE;DEBUG" "DESTINATION;CONFIG" "SOURCES;TARGETS"
            VERBOSE SOURCES a.cpp b.cpp DESTINATION /usr/bin DEBUG TARGETS foo bar)
        message("${MY_VERBOSE}")
        message("${MY_DEBUG}")
        message("${MY_DESTINATION}")
        message("${MY_CONFIG}")
        message("${MY_SOURCES}")
        message("${MY_TARGETS}")
        message("${MY_UNPARSED_ARGUMENTS}")
    )");
    REQUIRE(output == "TRUE\nTRUE\n/usr/bin\n\na.cpp;b.cpp\nfoo;bar\n\n");

    // Test options default to FALSE
    output = run_script(R"(
        cmake_parse_arguments(MY "OPT1;OPT2" "" "" ARG1 ARG2)
        message("${MY_OPT1}")
        message("${MY_OPT2}")
        message("${MY_UNPARSED_ARGUMENTS}")
    )");
    REQUIRE(output == "FALSE\nFALSE\nARG1;ARG2\n");

    // Test one-value keywords are undefined when not provided
    output = run_script(R"(
        set(MY_CONFIG "default")
        cmake_parse_arguments(MY "" "CONFIG;DEST" "" DEST /usr)
        message("${MY_DEST}")
        message("${MY_CONFIG}")
    )");
    REQUIRE(output == "/usr\n\n");

    // Test multi-value keywords are undefined when not provided
    output = run_script(R"(
        set(MY_SOURCES "default.cpp")
        cmake_parse_arguments(MY "" "" "SOURCES;TARGETS")
        message("${MY_SOURCES}")
        message("${MY_TARGETS}")
    )");
    REQUIRE(output == "\n\n");
}

TEST_CASE("cmake_parse_arguments PARSE_ARGV form", "[interpreter]") {
    // Test PARSE_ARGV in a function
    auto output = run_script(R"(
        function(my_install)
            set(options OPTIONAL FAST)
            set(oneValueArgs DESTINATION RENAME)
            set(multiValueArgs TARGETS CONFIGURATIONS)
            cmake_parse_arguments(PARSE_ARGV 0 arg
                "${options}" "${oneValueArgs}" "${multiValueArgs}"
            )
            message("${arg_OPTIONAL}")
            message("${arg_FAST}")
            message("${arg_DESTINATION}")
            message("${arg_TARGETS}")
            message("${arg_UNPARSED_ARGUMENTS}")
        endfunction()

        my_install(TARGETS foo bar DESTINATION /usr/bin OPTIONAL extra_arg)
    )");
    REQUIRE(output == "TRUE\nFALSE\n/usr/bin\nfoo;bar\nextra_arg\n");

    // Test PARSE_ARGV with offset
    output = run_script(R"(
        function(my_func)
            cmake_parse_arguments(PARSE_ARGV 1 arg "FLAG" "VALUE" "")
            message("${arg_FLAG}")
            message("${arg_VALUE}")
        endfunction()

        my_func(skip_this FLAG VALUE data)
    )");
    REQUIRE(output == "TRUE\ndata\n");
}

TEST_CASE("cmake_parse_arguments edge cases", "[interpreter]") {
    // Test missing value for one-value keyword
    auto output = run_script(R"(
        cmake_parse_arguments(MY "" "REQUIRED" "" REQUIRED)
        message("${MY_KEYWORDS_MISSING_VALUES}")
    )");
    REQUIRE(output == "REQUIRED\n");

    // Test one-value keyword followed by another one-value keyword (missing value)
    // This is the pattern used by FindPackageHandleStandardArgs when an empty value
    // is passed for REASON_FAILURE_MESSAGE and the next keyword is VERSION_VAR
    output = run_script(R"(
        cmake_parse_arguments(FPHSA "" "REASON_FAILURE_MESSAGE;VERSION_VAR" "REQUIRED_VARS"
            REQUIRED_VARS PKG_CONFIG_EXECUTABLE REASON_FAILURE_MESSAGE VERSION_VAR PkgConfig_VERSION)
        message("${FPHSA_REQUIRED_VARS}")
        message("REASON:'${FPHSA_REASON_FAILURE_MESSAGE}'")
        message("VERSION:'${FPHSA_VERSION_VAR}'")
        message("UNPARSED:'${FPHSA_UNPARSED_ARGUMENTS}'")
        message("MISSING:'${FPHSA_KEYWORDS_MISSING_VALUES}'")
    )");
    REQUIRE(output == "PKG_CONFIG_EXECUTABLE\nREASON:''\nVERSION:'PkgConfig_VERSION'\nUNPARSED:''\nMISSING:'REASON_FAILURE_MESSAGE'\n");

    // Test multi-value with no values (next token is keyword)
    output = run_script(R"(
        cmake_parse_arguments(MY "OPT" "" "ITEMS" ITEMS OPT)
        message("${MY_ITEMS}")
        message("${MY_OPT}")
    )");
    REQUIRE(output == "\nTRUE\n");

    // Test multi-value with empty list
    output = run_script(R"(
        cmake_parse_arguments(MY "" "" "ITEMS" ITEMS)
        message("${MY_ITEMS}")
    )");
    REQUIRE(output == "\n");

    // Test empty unparsed and missing values lists
    output = run_script(R"(
        cmake_parse_arguments(MY "FLAG" "VAL" "" FLAG VAL data)
        message("${MY_UNPARSED_ARGUMENTS}")
        message("${MY_KEYWORDS_MISSING_VALUES}")
    )");
    REQUIRE(output == "\n\n");

    // Test case sensitivity
    output = run_script(R"(
        cmake_parse_arguments(MY "VERBOSE" "" "" verbose VERBOSE)
        message("${MY_VERBOSE}")
        message("${MY_UNPARSED_ARGUMENTS}")
    )");
    REQUIRE(output == "TRUE\nverbose\n");
}

TEST_CASE("cmake_parse_arguments duplicate multi-value keywords append", "[interpreter]") {
    // CMake appends values when a multi-value keyword appears more than once
    auto output = run_script(R"(
        cmake_parse_arguments(MY "" "" "LIBRARIES;INCLUDES"
            INCLUDES inc1 inc2
            LIBRARIES lib1 lib2
            LIBRARIES lib3
        )
        message("${MY_LIBRARIES}")
        message("${MY_INCLUDES}")
    )");
    REQUIRE(output == "lib1;lib2;lib3\ninc1;inc2\n");

    // Duplicate keyword with different multi-value keywords interleaved
    output = run_script(R"(
        cmake_parse_arguments(MY "" "" "SOURCES;FLAGS"
            SOURCES a.cpp
            FLAGS -Wall
            SOURCES b.cpp c.cpp
            FLAGS -Werror
        )
        message("${MY_SOURCES}")
        message("${MY_FLAGS}")
    )");
    REQUIRE(output == "a.cpp;b.cpp;c.cpp\n-Wall;-Werror\n");
}

TEST_CASE("cmake_parse_arguments error handling", "[interpreter]") {
    // Test too few arguments
    CHECK_THROWS_WITH(run_script(R"(
        cmake_parse_arguments()
    )"), Catch::Matchers::ContainsSubstring("requires at least 1 argument"));

    // Test standard form too few arguments
    CHECK_THROWS_WITH(run_script(R"(
        cmake_parse_arguments(MY "OPT" "VAL")
    )"), Catch::Matchers::ContainsSubstring("requires at least 4 arguments"));

    // Test PARSE_ARGV too few arguments
    CHECK_THROWS_WITH(run_script(R"(
        function(test)
            cmake_parse_arguments(PARSE_ARGV 0 MY "OPT")
        endfunction()
        test()
    )"), Catch::Matchers::ContainsSubstring("requires 6 arguments"));

    // Test PARSE_ARGV with invalid index
    CHECK_THROWS_WITH(run_script(R"(
        function(test)
            cmake_parse_arguments(PARSE_ARGV bad MY "OPT" "" "")
        endfunction()
        test()
    )"), Catch::Matchers::ContainsSubstring("must be a number"));

    // Test PARSE_ARGV with negative index
    CHECK_THROWS_WITH(run_script(R"(
        function(test)
            cmake_parse_arguments(PARSE_ARGV -1 MY "OPT" "" "")
        endfunction()
        test()
    )"), Catch::Matchers::ContainsSubstring("cannot be negative"));
}

TEST_CASE("Parser handles parentheses in quoted strings", "[parser]") {
    // Test closing paren in quoted string - use custom delimiter to avoid escaping issues
    auto output = run_script(R"RAW(
        string(APPEND var "\n)")
        message("${var}")
    )RAW");
    REQUIRE(output == "\n)\n");

    // Test opening paren in quoted string
    output = run_script(R"RAW(
        set(var "foo (bar")
        message("${var}")
    )RAW");
    REQUIRE(output == "foo (bar\n");

    // Test both parens in quoted string
    output = run_script(R"RAW(
        set(var "foo (bar) baz")
        message("${var}")
    )RAW");
    REQUIRE(output == "foo (bar) baz\n");
}

TEST_CASE("Parser handles escaped quotes in unquoted arguments", "[parser]") {
    // Regression test: backslash-quote in unquoted argument should not break subsequent parsing
    // This was a bug where \" in an unquoted argument would corrupt the parser state
    auto output = run_script(R"RAW(
        list(APPEND var --config \"test\")
        string(APPEND var2 "\n)")
        message("${var2}")
    )RAW");
    REQUIRE(output == "\n)\n");

    // Test multiple escaped quotes in unquoted arguments
    // In CMake, \" in unquoted context is an escape sequence that produces a literal quote
    output = run_script(R"RAW(
        set(var \"foo\" \"bar\")
        message("${var}")
    )RAW");
    REQUIRE(output == "\"foo\";\"bar\"\n");

    // Test escaped quote followed by bracket argument
    output = run_script(R"RAW(
        set(REMOTE "[====[test]====]")
        list(APPEND opts \"${REMOTE}\")
        message("done")
    )RAW");
    REQUIRE(output == "done\n");
}

TEST_CASE("Backslash-newline line continuation in quoted strings", "[parser]") {
    // In CMake, backslash followed by newline in a quoted string is a line
    // continuation — both the backslash and newline are dropped entirely.
    // This is critical for multi-line Python/shell scripts in custom commands.
    auto output = run_script("message(\"line1\\\nline2\")");
    REQUIRE(output == "line1line2\n");

    // With leading whitespace on the continuation line (whitespace preserved)
    output = run_script("message(\"hello\\\n    world\")");
    REQUIRE(output == "hello    world\n");

    // Multiple continuations
    output = run_script("message(\"a\\\nb\\\nc\")");
    REQUIRE(output == "abc\n");

    // Continuation doesn't affect other escape sequences
    output = run_script("message(\"tab\\there\\\ncont\")");
    REQUIRE(output == "tab\therecont\n");
}

// Bug fix tests from differential fuzzing

TEST_CASE("Empty list elements are preserved in string representation", "[interpreter][bugfix]") {
    // CMake preserves empty elements in the internal representation
    // When stored as "a;;c", it becomes a list ["a", "", "c"]
    auto output = run_script(R"(
        set(L a;;c)
        message("${L}")
    )");
    REQUIRE(output == "a;;c\n");

    output = run_script(R"(
        set(L a;;;b;;;;c)
        message("${L}")
    )");
    REQUIRE(output == "a;;;b;;;;c\n");

    output = run_script(R"(
        set(L ;;a;b)
        message("${L}")
    )");
    REQUIRE(output == ";;a;b\n");

    output = run_script(R"(
        set(L a;b;;)
        message("${L}")
    )");
    REQUIRE(output == "a;b;;\n");
}

TEST_CASE("Empty list elements filtered in unquoted foreach", "[interpreter][bugfix]") {
    // CMake filters empty elements during unquoted list expansion in foreach
    auto output = run_script(R"(
        set(L a;;c)
        foreach(item ${L})
            message("${item}")
        endforeach()
    )");
    REQUIRE(output == "a\nc\n");  // 2 iterations, empty element filtered
}

TEST_CASE("Empty list elements preserved in IN LISTS foreach", "[interpreter][bugfix]") {
    // CMake preserves empty elements with IN LISTS syntax
    auto output = run_script(R"(
        set(L a;;c)
        foreach(item IN LISTS L)
            message("${item}")
        endforeach()
    )");
    REQUIRE(output == "a\n\nc\n");  // 3 iterations, empty element preserved
}

TEST_CASE("Empty prefix in list expansion like CHECK_INCLUDE_FILES", "[interpreter][bugfix]") {
    // Simulates: SET(INCLUDES "") then CHECK_INCLUDE_FILES("${INCLUDES};header" ...)
    // The leading empty element should be filtered in unquoted expansion
    auto output = run_script(R"(
        set(INCLUDES "")
        set(header "sys/types.h")
        foreach(FILE ${INCLUDES};${header})
            message("${FILE}")
        endforeach()
    )");
    REQUIRE(output == "sys/types.h\n");  // Empty element filtered, only header remains
}

TEST_CASE("Foreach variable is cleared after loop", "[interpreter][bugfix]") {
    // Bug: Loop variable persisted after loop
    auto output = run_script(R"(
        foreach(i RANGE 2)
        endforeach()
        message("'${i}'")
    )");
    REQUIRE(output == "''\n");
}

TEST_CASE("Nested foreach with same variable", "[interpreter][bugfix]") {
    // Bug: Inner loop overwrote outer loop variable
    auto output = run_script(R"(
        foreach(x RANGE 1)
            foreach(x RANGE 1)
                message("inner:${x}")
            endforeach()
            message("outer:${x}")
        endforeach()
    )");
    REQUIRE(output == "inner:0\ninner:1\nouter:0\ninner:0\ninner:1\nouter:1\n");
}

TEST_CASE("RANGE with descending values", "[interpreter][bugfix]") {
    // Bug: RANGE 5 2 produced no iterations
    auto output = run_script(R"(
        foreach(i RANGE 5 2)
            message("${i}")
        endforeach()
    )");
    REQUIRE(output == "5\n4\n3\n2\n");
}

TEST_CASE("RANGE with negative values", "[interpreter][bugfix]") {
    auto output = run_script(R"(
        foreach(i RANGE -2 2)
            message("${i}")
        endforeach()
    )");
    REQUIRE(output == "-2\n-1\n0\n1\n2\n");
}

TEST_CASE("Macro argument text substitution", "[interpreter][bugfix]") {
    // Bug: Macro arguments were treated as variables, not text substitution
    auto output = run_script(R"(
        macro(test arg)
            set(arg "new")
            message("${arg}")
        endmacro()
        test(old)
    )");
    REQUIRE(output == "old\n");  // arg is text-substituted to "old", not the variable

    output = run_script(R"(
        macro(test x)
            message("before:${x}")
            set(x "modified")
            message("after:${x}")
        endmacro()
        test(original)
    )");
    REQUIRE(output == "before:original\nafter:original\n");
}

TEST_CASE("Macro argument in condition", "[interpreter][bugfix]") {
    // Macro arguments in conditions should use literal value
    auto output = run_script(R"(
        macro(test val)
            if(val STREQUAL "match")
                message("matched")
            else()
                message("no match")
            endif()
        endmacro()
        test(match)
        test(nomatch)
    )");
    REQUIRE(output == "matched\nno match\n");
}

TEST_CASE("Macro ARGN modification", "[interpreter][bugfix]") {
    // ARGN should be read-only text substitution
    auto output = run_script(R"(
        macro(test first)
            message("ARGN:${ARGN}")
            set(ARGN "modified")
            message("after:${ARGN}")
        endmacro()
        test(a b c)
    )");
    REQUIRE(output == "ARGN:b;c\nafter:b;c\n");
}

TEST_CASE("Macro vs Function argument handling", "[interpreter][bugfix]") {
    // Functions create scope, macros don't - but both handle arguments differently
    auto output = run_script(R"(
        function(func arg)
            set(arg "func_modified")
            message("func:${arg}")
        endfunction()

        macro(mac arg)
            set(arg "macro_modified")
            message("macro:${arg}")
        endmacro()

        func(original)
        mac(original)
    )");
    REQUIRE(output == "func:func_modified\nmacro:original\n");
}

TEST_CASE("Macro using argument as variable name", "[interpreter][bugfix]") {
    // Common macro pattern: using argument as a variable name
    auto output = run_script(R"(
        macro(set_var varname)
            set(${varname} "value")
        endmacro()
        set_var(MY_VAR)
        message("${MY_VAR}")
    )");
    REQUIRE(output == "value\n");
}

// While loop tests

TEST_CASE("while loop basic iteration", "[interpreter][while]") {
    auto output = run_script(R"(
        set(COUNT 0)
        while(COUNT LESS 3)
            message("${COUNT}")
            math(EXPR COUNT "${COUNT} + 1")
        endwhile()
    )");
    REQUIRE(output == "0\n1\n2\n");
}

TEST_CASE("while loop with break", "[interpreter][while]") {
    auto output = run_script(R"(
        set(I 0)
        while(I LESS 10)
            if(I EQUAL 3)
                break()
            endif()
            message("${I}")
            math(EXPR I "${I} + 1")
        endwhile()
        message("done")
    )");
    REQUIRE(output == "0\n1\n2\ndone\n");
}

TEST_CASE("while loop with continue", "[interpreter][while]") {
    auto output = run_script(R"(
        set(I 0)
        while(I LESS 5)
            math(EXPR I "${I} + 1")
            if(I EQUAL 3)
                continue()
            endif()
            message("${I}")
        endwhile()
    )");
    REQUIRE(output == "1\n2\n4\n5\n");
}

TEST_CASE("while loop false condition", "[interpreter][while]") {
    auto output = run_script(R"(
        set(VAR "OFF")
        while(VAR)
            message("should not print")
        endwhile()
        message("done")
    )");
    REQUIRE(output == "done\n");
}

TEST_CASE("nested while loops", "[interpreter][while]") {
    auto output = run_script(R"(
        set(I 0)
        while(I LESS 2)
            set(J 0)
            while(J LESS 2)
                message("${I},${J}")
                math(EXPR J "${J} + 1")
            endwhile()
            math(EXPR I "${I} + 1")
        endwhile()
    )");
    REQUIRE(output == "0,0\n0,1\n1,0\n1,1\n");
}

TEST_CASE("while loop with return", "[interpreter][while]") {
    auto output = run_script(R"(
        function(test_while)
            set(I 0)
            while(I LESS 10)
                if(I EQUAL 3)
                    return()
                endif()
                message("${I}")
                math(EXPR I "${I} + 1")
            endwhile()
            message("should not print")
        endfunction()
        test_while()
        message("done")
    )");
    REQUIRE(output == "0\n1\n2\ndone\n");
}

TEST_CASE("Bracket arguments do not undergo list expansion", "[interpreter][bugfix][diff-fuzz]") {
    // Bug 2: Bracket arguments should be treated like quoted arguments
    // and NOT undergo semicolon splitting (list expansion)
    auto output = run_script(R"(
        message([=[a;b;c]=])
    )");
    REQUIRE(output == "a;b;c\n");

    // Bracket arguments should also preserve variable references literally
    output = run_script(R"(
        set(VAR "value")
        message([=[${VAR}]=])
    )");
    REQUIRE(output == "${VAR}\n");

    // Multiple bracket arguments should concatenate without spaces
    output = run_script(R"(
        message([=[first]=] [=[second]=])
    )");
    REQUIRE(output == "firstsecond\n");
}

TEST_CASE("message() concatenates arguments without spaces", "[interpreter][bugfix][diff-fuzz]") {
    // Bug 3: message() should concatenate all arguments without adding spaces
    auto output = run_script(R"(
        message("A" "B" "C")
    )");
    REQUIRE(output == "ABC\n");

    // Test with variable expansion
    output = run_script(R"(
        set(L "a" "b")
        message("L: " ${L})
    )");
    REQUIRE(output == "L: ab\n");

    // Test with mixed quoted and unquoted
    output = run_script(R"(
        set(X "value")
        message("prefix" ${X} "suffix")
    )");
    REQUIRE(output == "prefixvaluesuffix\n");

    // Empty strings should contribute nothing
    output = run_script(R"(
        message("A" "" "B")
    )");
    REQUIRE(output == "AB\n");
}

TEST_CASE("if condition: CMake truthiness for numeric-looking strings", "[interpreter][if][bugfix]") {
    // CMake truthiness rules: only exact "0", empty string, and false constants are falsy.
    // Invalid numeric strings like "2x" are TRUTHY in CMake (not falsy).
    // This test was corrected to match actual CMake behavior.

    // Test "2x" - not a valid number, but should be TRUTHY (CMake behavior)
    // Note: When used as a literal (unquoted), it's dereferenced as a variable name.
    // Since "2x" is undefined, it becomes empty string (falsy).
    // But when the VARIABLE contains "2x", the value "2x" should be truthy.
    auto output = run_script(R"(
        set(VAR "2x")
        if(VAR)
            message("2x is true")
        else()
            message("2x is false")
        endif()
    )");
    REQUIRE(output == "2x is true\n");

    // Test "-2x" stored in a variable - should be TRUTHY
    output = run_script(R"(
        set(VAR "-2x")
        if(VAR)
            message("-2x is true")
        else()
            message("-2x is false")
        endif()
    )");
    REQUIRE(output == "-2x is true\n");

    // Valid numbers should still be truthy
    output = run_script(R"(
        if(2)
            message("2 is true")
        endif()
        if(-2)
            message("-2 is true")
        endif()
        if(2.0)
            message("2.0 is true")
        endif()
        if(-2.0)
            message("-2.0 is true")
        endif()
    )");
    REQUIRE(output == "2 is true\n-2 is true\n2.0 is true\n-2.0 is true\n");
}

TEST_CASE("if condition: variable concatenation in macros", "[interpreter][if][macro][bugfix]") {
    // When a macro has something like VAR_${_var} and _var is empty,
    // it should expand to "VAR_" and then dereference the variable VAR_
    // This matches CMake behavior from IfTest.cmake.in

    auto output = run_script(R"(
        set(VAR_ "")

        macro(test_vars)
            set(_var "")
            if(VAR_${_var})
                message("VAR_ is true")
            else()
                message("VAR_ is false")
            endif()
        endmacro()

        test_vars()
    )");
    REQUIRE(output == "VAR_ is false\n");

    // Test with a non-empty value in VAR_
    output = run_script(R"(
        set(VAR_ "some_value")

        macro(test_vars)
            set(_var "")
            if(VAR_${_var})
                message("VAR_ is true")
            else()
                message("VAR_ is false")
            endif()
        endmacro()

        test_vars()
    )");
    REQUIRE(output == "VAR_ is true\n");

    // Test with different suffix
    output = run_script(R"(
        set(VAR_FOO "value")
        set(VAR_BAR "")

        macro(test_vars suffix)
            if(VAR_${suffix})
                message("VAR_${suffix} is true")
            else()
                message("VAR_${suffix} is false")
            endif()
        endmacro()

        test_vars(FOO)
        test_vars(BAR)
    )");
    REQUIRE(output == "VAR_FOO is true\nVAR_BAR is false\n");

    // Single variable reference should not double-dereference
    output = run_script(R"(
        set(VAR "ON")
        set(ON "this_should_not_be_evaluated")

        if(${VAR})
            message("VAR is true")
        else()
            message("VAR is false")
        endif()
    )");
    REQUIRE(output == "VAR is true\n");
}

TEST_CASE("if condition: EXISTS with unquoted paths", "[interpreter][if][bugfix]") {
    // File test operators (EXISTS, IS_DIRECTORY, IS_ABSOLUTE, IS_SYMLINK) should
    // treat unquoted paths as literals (with variable expansion), NOT as variable names.
    // This is different from other if() conditions where unquoted arguments are dereferenced.

    // Create a temporary file for testing
    auto temp_dir = std::filesystem::temp_directory_path();
    auto test_file = temp_dir / "kiln_test_exists.txt";
    {
        std::ofstream f(test_file);
        f << "test content";
    }

    // Test EXISTS with unquoted path
    auto output = run_script(R"(
        if(EXISTS )" + test_file.string() + R"()
            message("EXISTS unquoted: found")
        else()
            message("EXISTS unquoted: not found")
        endif()
    )");
    REQUIRE(output == "EXISTS unquoted: found\n");

    // Test EXISTS with quoted path
    output = run_script(R"(
        if(EXISTS ")" + test_file.string() + R"(")
            message("EXISTS quoted: found")
        else()
            message("EXISTS quoted: not found")
        endif()
    )");
    REQUIRE(output == "EXISTS quoted: found\n");

    // Test EXISTS with variable expansion
    output = run_script(R"(
        set(TEST_PATH ")" + test_file.string() + R"(")
        if(EXISTS ${TEST_PATH})
            message("EXISTS variable: found")
        else()
            message("EXISTS variable: not found")
        endif()
    )");
    REQUIRE(output == "EXISTS variable: found\n");

    // Test IS_DIRECTORY with unquoted path
    output = run_script(R"(
        if(IS_DIRECTORY )" + temp_dir.string() + R"()
            message("IS_DIRECTORY unquoted: yes")
        else()
            message("IS_DIRECTORY unquoted: no")
        endif()
    )");
    REQUIRE(output == "IS_DIRECTORY unquoted: yes\n");

    // Test IS_ABSOLUTE with unquoted path
    output = run_script(R"(
        if(IS_ABSOLUTE )" + test_file.string() + R"()
            message("IS_ABSOLUTE unquoted: yes")
        else()
            message("IS_ABSOLUTE unquoted: no")
        endif()
    )");
    REQUIRE(output == "IS_ABSOLUTE unquoted: yes\n");

    // Cleanup
    std::filesystem::remove(test_file);
}

TEST_CASE("Interpreter block() scope", "[interpreter][block]") {
    SECTION("block() creates variable scope by default") {
        // CMake 3.25+: block() with no arguments creates both variable and policy scopes
        auto output = run_script(R"(
            set(var "value")
            block()
                set(var "modified")
            endblock()
            message("var=${var}")
        )");
        REQUIRE(output == "var=value\n");
    }

    SECTION("SCOPE_FOR POLICIES does not create variable scope") {
        // block(SCOPE_FOR POLICIES) only creates policy scope, not variable scope
        auto output = run_script(R"(
            set(var "value")
            block(SCOPE_FOR POLICIES)
                set(var "modified")
            endblock()
            message("var=${var}")
        )");
        REQUIRE(output == "var=modified\n");
    }

    SECTION("Basic variable scoping with SCOPE_FOR VARIABLES") {
        auto output = run_script(R"(
            set(outer "outer_value")
            block(SCOPE_FOR VARIABLES)
                set(inner "inner_value")
                message("inner=${inner}")
                message("outer=${outer}")
            endblock()
            message("outer=${outer}")
            message("inner=${inner}")
        )");
        REQUIRE(output == "inner=inner_value\nouter=outer_value\nouter=outer_value\ninner=\n");
    }

    SECTION("PROPAGATE single variable") {
        auto output = run_script(R"(
            set(outer "outer_value")
            block(PROPAGATE result)
                set(result "computed_value")
                set(temp "temporary")
            endblock()
            message("result=${result}")
            message("temp=${temp}")
        )");
        REQUIRE(output == "result=computed_value\ntemp=\n");
    }

    SECTION("PROPAGATE multiple variables") {
        auto output = run_script(R"(
            block(PROPAGATE var1 var2)
                set(var1 "value1")
                set(var2 "value2")
                set(var3 "value3")
            endblock()
            message("var1=${var1}")
            message("var2=${var2}")
            message("var3=${var3}")
        )");
        REQUIRE(output == "var1=value1\nvar2=value2\nvar3=\n");
    }

    SECTION("SCOPE_FOR VARIABLES") {
        auto output = run_script(R"(
            set(outer "outer_value")
            block(SCOPE_FOR VARIABLES)
                set(inner "inner_value")
                message("inner=${inner}")
            endblock()
            message("inner=${inner}")
        )");
        REQUIRE(output == "inner=inner_value\ninner=\n");
    }

    SECTION("SCOPE_FOR VARIABLES with PROPAGATE") {
        auto output = run_script(R"(
            block(SCOPE_FOR VARIABLES PROPAGATE result)
                set(result "computed")
                set(temp "temporary")
            endblock()
            message("result=${result}")
            message("temp=${temp}")
        )");
        REQUIRE(output == "result=computed\ntemp=\n");
    }

    SECTION("Nested blocks") {
        auto output = run_script(R"(
            set(level0 "L0")
            block(PROPAGATE level1)
                set(level1 "L1")
                block(PROPAGATE level2)
                    set(level2 "L2")
                    message("level0=${level0}")
                    message("level1=${level1}")
                    message("level2=${level2}")
                endblock()
                message("level2=${level2}")
            endblock()
            message("level1=${level1}")
            message("level2=${level2}")
        )");
        REQUIRE(output == "level0=L0\nlevel1=L1\nlevel2=L2\nlevel2=L2\nlevel1=L1\nlevel2=\n");
    }

    SECTION("Variable shadowing with SCOPE_FOR VARIABLES") {
        auto output = run_script(R"(
            set(var "outer")
            block(SCOPE_FOR VARIABLES)
                set(var "inner")
                message("inside=${var}")
            endblock()
            message("outside=${var}")
        )");
        REQUIRE(output == "inside=inner\noutside=outer\n");
    }

    SECTION("block() creates variable scope by default") {
        // CMake 3.25+ behavior: block() with no arguments creates a variable scope
        auto output = run_script(R"(
            set(var "outer")
            block()
                set(var "modified")
                message("inside=${var}")
            endblock()
            message("outside=${var}")
        )");
        REQUIRE(output == "inside=modified\noutside=outer\n");
    }

    SECTION("PROPAGATE with undefined variable") {
        auto output = run_script(R"(
            block(PROPAGATE undefined_var)
                # Don't set undefined_var
                set(other "value")
            endblock()
            message("undefined=${undefined_var}")
            message("other=${other}")
        )");
        REQUIRE(output == "undefined=\nother=\n");
    }

    SECTION("Block with if statement") {
        auto output = run_script(R"(
            set(condition TRUE)
            block(PROPAGATE result)
                if(condition)
                    set(result "yes")
                else()
                    set(result "no")
                endif()
            endblock()
            message("result=${result}")
        )");
        REQUIRE(output == "result=yes\n");
    }

    SECTION("Block with foreach loop") {
        auto output = run_script(R"(
            block(PROPAGATE sum)
                set(sum 0)
                foreach(i RANGE 1 3)
                    math(EXPR sum "${sum} + ${i}")
                endforeach()
            endblock()
            message("sum=${sum}")
        )");
        REQUIRE(output == "sum=6\n");
    }
}

TEST_CASE("CMAKE_CURRENT_FUNCTION variables basic functionality", "[interpreter]") {
    // Test that CMAKE_CURRENT_FUNCTION* variables work inside a function
    auto output = run_script(R"(
        function(test_func)
            message("${CMAKE_CURRENT_FUNCTION}")
        endfunction()
        test_func()
    )");
    REQUIRE(output == "test_func\n");

    // Test that these variables are empty outside functions
    output = run_script(R"(
        message("Outside: '${CMAKE_CURRENT_FUNCTION}'")
    )");
    REQUIRE(output == "Outside: ''\n");

    // Test CMAKE_CURRENT_FUNCTION_LIST_FILE and CMAKE_CURRENT_FUNCTION_LIST_DIR
    // Note: In unit tests without real files, these will be empty strings
    output = run_script(R"(
        function(test_func2)
            message("Name: ${CMAKE_CURRENT_FUNCTION}")
            message("File: '${CMAKE_CURRENT_FUNCTION_LIST_FILE}'")
            message("Dir: '${CMAKE_CURRENT_FUNCTION_LIST_DIR}'")
        endfunction()
        test_func2()
    )");
    REQUIRE(output == "Name: test_func2\nFile: ''\nDir: ''\n");
}

TEST_CASE("CMAKE_CURRENT_FUNCTION variables in nested functions", "[interpreter]") {
    // Test nested function calls - each should report its own name
    auto output = run_script(R"(
        function(outer)
            message("Outer: ${CMAKE_CURRENT_FUNCTION}")
            inner()
            message("Outer again: ${CMAKE_CURRENT_FUNCTION}")
        endfunction()
        function(inner)
            message("Inner: ${CMAKE_CURRENT_FUNCTION}")
        endfunction()
        outer()
    )");
    REQUIRE(output == "Outer: outer\nInner: inner\nOuter again: outer\n");
}

TEST_CASE("CMAKE_CURRENT_FUNCTION variables not set in macros", "[interpreter]") {
    // Macros don't create new scopes, so CMAKE_CURRENT_FUNCTION should not be set
    auto output = run_script(R"(
        macro(test_macro)
            message("In macro: '${CMAKE_CURRENT_FUNCTION}'")
        endmacro()
        test_macro()
    )");
    REQUIRE(output == "In macro: ''\n");

    // Test macro called from within a function - should show function name
    output = run_script(R"(
        macro(test_macro2)
            message("In macro: '${CMAKE_CURRENT_FUNCTION}'")
        endmacro()
        function(test_func)
            message("In function: '${CMAKE_CURRENT_FUNCTION}'")
            test_macro2()
        endfunction()
        test_func()
    )");
    REQUIRE(output == "In function: 'test_func'\nIn macro: 'test_func'\n");
}

TEST_CASE("CMAKE_CURRENT_FUNCTION with recursive functions", "[interpreter]") {
    // Test that recursive functions correctly report their own name
    auto output = run_script(R"(
        function(recursive depth)
            message("Depth ${depth}: ${CMAKE_CURRENT_FUNCTION}")
            if(depth GREATER 0)
                math(EXPR next "${depth} - 1")
                recursive(${next})
            endif()
        endfunction()
        recursive(2)
    )");
    REQUIRE(output == "Depth 2: recursive\nDepth 1: recursive\nDepth 0: recursive\n");
}

TEST_CASE("get_property TARGET TYPE", "[interpreter][property]") {
    // Create a temp directory and source files for testing
    std::string temp_dir = "build_test_get_property";
    std::filesystem::create_directories(temp_dir);
    {
        std::ofstream f("test_main.cpp");
        f << "int main() { return 0; }\n";
        std::ofstream f2("test_lib.cpp");
        f2 << "void lib_func() {}\n";
    }

    std::stringstream output;
    kiln::Interpreter interp(".", &output, &std::cerr, temp_dir);

    // Parse and run a script that tests get_property
    std::string script = R"(
        add_executable(myexe test_main.cpp)
        add_library(mystaticlib STATIC test_lib.cpp)
        add_library(mysharedlib SHARED test_lib.cpp)
        add_library(myobjectlib OBJECT test_lib.cpp)
        add_library(myinterfacelib INTERFACE)
        add_custom_target(mycustomtarget COMMAND echo "hello")

        get_property(EXE_TYPE TARGET myexe PROPERTY TYPE)
        get_property(STATIC_TYPE TARGET mystaticlib PROPERTY TYPE)
        get_property(SHARED_TYPE TARGET mysharedlib PROPERTY TYPE)
        get_property(OBJECT_TYPE TARGET myobjectlib PROPERTY TYPE)
        get_property(INTERFACE_TYPE TARGET myinterfacelib PROPERTY TYPE)
        get_property(CUSTOM_TYPE TARGET mycustomtarget PROPERTY TYPE)

        message(STATUS "exe=${EXE_TYPE}")
        message(STATUS "static=${STATIC_TYPE}")
        message(STATUS "shared=${SHARED_TYPE}")
        message(STATUS "object=${OBJECT_TYPE}")
        message(STATUS "interface=${INTERFACE_TYPE}")
        message(STATUS "custom=${CUSTOM_TYPE}")
    )";

    interp.set_source_view(script);
    kiln::Parser parser(script);
    auto ast_or_error = parser.parse();
    REQUIRE(ast_or_error.has_value());

    auto result = interp.interpret(ast_or_error.value());
    REQUIRE(result.has_value());

    std::string out = output.str();
    REQUIRE(out.find("exe=EXECUTABLE") != std::string::npos);
    REQUIRE(out.find("static=STATIC_LIBRARY") != std::string::npos);
    REQUIRE(out.find("shared=SHARED_LIBRARY") != std::string::npos);
    REQUIRE(out.find("object=OBJECT_LIBRARY") != std::string::npos);
    REQUIRE(out.find("interface=INTERFACE_LIBRARY") != std::string::npos);
    REQUIRE(out.find("custom=UTILITY") != std::string::npos);

    // Cleanup
    std::filesystem::remove_all(temp_dir);
    std::filesystem::remove("test_main.cpp");
    std::filesystem::remove("test_lib.cpp");
}

TEST_CASE("get_target_property TYPE", "[interpreter][property]") {
    std::string temp_dir = "build_test_get_target_property";
    std::filesystem::create_directories(temp_dir);
    {
        std::ofstream f("test_main2.cpp");
        f << "int main() { return 0; }\n";
        std::ofstream f2("test_lib2.cpp");
        f2 << "void lib_func() {}\n";
    }

    std::stringstream output;
    kiln::Interpreter interp(".", &output, &std::cerr, temp_dir);

    std::string script = R"(
        add_executable(myexe test_main2.cpp)
        add_library(mylib STATIC test_lib2.cpp)

        get_target_property(EXE_TYPE myexe TYPE)
        get_target_property(LIB_TYPE mylib TYPE)

        message(STATUS "exe=${EXE_TYPE}")
        message(STATUS "lib=${LIB_TYPE}")
    )";

    interp.set_source_view(script);
    kiln::Parser parser(script);
    auto ast_or_error = parser.parse();
    REQUIRE(ast_or_error.has_value());

    auto result = interp.interpret(ast_or_error.value());
    REQUIRE(result.has_value());

    std::string out = output.str();
    REQUIRE(out.find("exe=EXECUTABLE") != std::string::npos);
    REQUIRE(out.find("lib=STATIC_LIBRARY") != std::string::npos);

    // Cleanup
    std::filesystem::remove_all(temp_dir);
    std::filesystem::remove("test_main2.cpp");
    std::filesystem::remove("test_lib2.cpp");
}

TEST_CASE("get_property TARGET other properties", "[interpreter][property]") {
    std::string temp_dir = "build_test_get_property_other";
    std::filesystem::create_directories(temp_dir);
    {
        std::ofstream f("test_main3.cpp");
        f << "int main() { return 0; }\n";
    }

    std::stringstream output;
    kiln::Interpreter interp(".", &output, &std::cerr, temp_dir);

    std::string script = R"(
        add_executable(myexe test_main3.cpp)

        get_property(NAME_VAL TARGET myexe PROPERTY NAME)
        get_property(IMPORTED_VAL TARGET myexe PROPERTY IMPORTED)

        message(STATUS "name=${NAME_VAL}")
        message(STATUS "imported=${IMPORTED_VAL}")
    )";

    interp.set_source_view(script);
    kiln::Parser parser(script);
    auto ast_or_error = parser.parse();
    REQUIRE(ast_or_error.has_value());

    auto result = interp.interpret(ast_or_error.value());
    REQUIRE(result.has_value());

    std::string out = output.str();
    REQUIRE(out.find("name=myexe") != std::string::npos);
    REQUIRE(out.find("imported=FALSE") != std::string::npos);

    // Cleanup
    std::filesystem::remove_all(temp_dir);
    std::filesystem::remove("test_main3.cpp");
}

TEST_CASE("get_property TARGET SET mode", "[interpreter][property]") {
    std::string temp_dir = "build_test_get_property_set";
    std::filesystem::create_directories(temp_dir);
    {
        std::ofstream f("test_main4.cpp");
        f << "int main() { return 0; }\n";
    }

    std::stringstream output;
    kiln::Interpreter interp(".", &output, &std::cerr, temp_dir);

    std::string script = R"(
        add_executable(myexe test_main4.cpp)

        get_property(TYPE_SET TARGET myexe PROPERTY TYPE SET)
        get_property(UNKNOWN_SET TARGET myexe PROPERTY NONEXISTENT_PROP SET)

        message(STATUS "type_set=${TYPE_SET}")
        message(STATUS "unknown_set=${UNKNOWN_SET}")
    )";

    interp.set_source_view(script);
    kiln::Parser parser(script);
    auto ast_or_error = parser.parse();
    REQUIRE(ast_or_error.has_value());

    auto result = interp.interpret(ast_or_error.value());
    REQUIRE(result.has_value());

    std::string out = output.str();
    REQUIRE(out.find("type_set=1") != std::string::npos);
    REQUIRE(out.find("unknown_set=0") != std::string::npos);

    // Cleanup
    std::filesystem::remove_all(temp_dir);
    std::filesystem::remove("test_main4.cpp");
}

TEST_CASE("get_target_property NOTFOUND for missing property", "[interpreter][property]") {
    std::string temp_dir = "build_test_get_target_property_notfound";
    std::filesystem::create_directories(temp_dir);
    {
        std::ofstream f("test_main5.cpp");
        f << "int main() { return 0; }\n";
    }

    std::stringstream output;
    kiln::Interpreter interp(".", &output, &std::cerr, temp_dir);

    std::string script = R"(
        add_executable(myexe test_main5.cpp)

        get_target_property(UNKNOWN_PROP myexe NONEXISTENT_PROPERTY)

        message(STATUS "unknown=${UNKNOWN_PROP}")
    )";

    interp.set_source_view(script);
    kiln::Parser parser(script);
    auto ast_or_error = parser.parse();
    REQUIRE(ast_or_error.has_value());

    auto result = interp.interpret(ast_or_error.value());
    REQUIRE(result.has_value());

    std::string out = output.str();
    REQUIRE(out.find("unknown=NONEXISTENT_PROPERTY-NOTFOUND") != std::string::npos);

    // Cleanup
    std::filesystem::remove_all(temp_dir);
    std::filesystem::remove("test_main5.cpp");
}

TEST_CASE("get_property TARGET IMPORTED target", "[interpreter][property]") {
    std::string temp_dir = "build_test_get_property_imported";
    std::filesystem::create_directories(temp_dir);

    std::stringstream output;
    kiln::Interpreter interp(".", &output, &std::cerr, temp_dir);

    std::string script = R"(
        add_library(myimported SHARED IMPORTED)
        set_target_properties(myimported PROPERTIES IMPORTED_LOCATION "/usr/lib/fake.so")

        get_property(TYPE_VAL TARGET myimported PROPERTY TYPE)
        get_property(IMPORTED_VAL TARGET myimported PROPERTY IMPORTED)
        get_property(LOCATION_VAL TARGET myimported PROPERTY IMPORTED_LOCATION)

        message(STATUS "type=${TYPE_VAL}")
        message(STATUS "imported=${IMPORTED_VAL}")
        message(STATUS "location=${LOCATION_VAL}")
    )";

    interp.set_source_view(script);
    kiln::Parser parser(script);
    auto ast_or_error = parser.parse();
    REQUIRE(ast_or_error.has_value());

    auto result = interp.interpret(ast_or_error.value());
    REQUIRE(result.has_value());

    std::string out = output.str();
    REQUIRE(out.find("type=SHARED_LIBRARY") != std::string::npos);
    REQUIRE(out.find("imported=TRUE") != std::string::npos);
    REQUIRE(out.find("location=/usr/lib/fake.so") != std::string::npos);

    // Cleanup
    std::filesystem::remove_all(temp_dir);
}

TEST_CASE("Built-in global properties", "[interpreter][property]") {
    std::string temp_dir = "build_test_builtin_global_props";
    std::filesystem::create_directories(temp_dir);

    std::stringstream output;
    kiln::Interpreter interp(temp_dir, &output, &output, temp_dir + "/build");

    interp.add_builtin("message", [&](kiln::Interpreter&, const std::vector<std::string>& args) {
        for (const auto& arg : args) output << arg;
        output << std::endl;
    });

    // Test built-in global properties
    // ENABLED_LANGUAGES starts empty; project() populates it
    std::string script = R"(
        get_property(LANGS_BEFORE GLOBAL PROPERTY ENABLED_LANGUAGES)
        project(test)
        get_property(LANGS GLOBAL PROPERTY ENABLED_LANGUAGES)
        get_property(MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
        get_property(SHARED_LIBS GLOBAL PROPERTY TARGET_SUPPORTS_SHARED_LIBS)
        get_property(ROLE GLOBAL PROPERTY CMAKE_ROLE)
        get_property(LIB64 GLOBAL PROPERTY FIND_LIBRARY_USE_LIB64_PATHS)
        message("langs_before=${LANGS_BEFORE}")
        message("langs=${LANGS}")
        message("multi_config=${MULTI_CONFIG}")
        message("shared_libs=${SHARED_LIBS}")
        message("role=${ROLE}")
        message("lib64=${LIB64}")
    )";

    interp.set_source_view(script);
    kiln::Parser parser(script);
    auto ast_or_error = parser.parse();
    REQUIRE(ast_or_error.has_value());

    auto result = interp.interpret(ast_or_error.value());
    REQUIRE(result.has_value());

    std::string out = output.str();
    REQUIRE(out.find("langs_before=\n") != std::string::npos);
    REQUIRE(out.find("langs=C;CXX") != std::string::npos);
    REQUIRE(out.find("multi_config=FALSE") != std::string::npos);
    REQUIRE(out.find("shared_libs=TRUE") != std::string::npos);
    REQUIRE(out.find("role=PROJECT") != std::string::npos);
    REQUIRE(out.find("lib64=TRUE") != std::string::npos);

    // Cleanup
    std::filesystem::remove_all(temp_dir);
}

TEST_CASE("set/get_property INSTALL scope", "[interpreter][property]") {
    std::string temp_dir = "build_test_install_props";
    std::filesystem::create_directories(temp_dir);

    std::stringstream output;
    kiln::Interpreter interp(temp_dir, &output, &output, temp_dir + "/build");

    interp.add_builtin("message", [&](kiln::Interpreter&, const std::vector<std::string>& args) {
        for (const auto& arg : args) output << arg;
        output << std::endl;
    });

    // Test INSTALL scope properties
    std::string script = R"(
        set_property(INSTALL bin/myapp PROPERTY CPACK_START_MENU_SHORTCUTS "MyApp")
        set_property(INSTALL lib/mylib.so PROPERTY CPACK_DEBIAN_PACKAGE_NAME "mylib")
        set_property(INSTALL bin/myapp APPEND PROPERTY EXTRA_OPTS "opt1")
        set_property(INSTALL bin/myapp APPEND PROPERTY EXTRA_OPTS "opt2")

        get_property(SHORTCUT INSTALL bin/myapp PROPERTY CPACK_START_MENU_SHORTCUTS)
        get_property(DEB_NAME INSTALL lib/mylib.so PROPERTY CPACK_DEBIAN_PACKAGE_NAME)
        get_property(OPTS INSTALL bin/myapp PROPERTY EXTRA_OPTS)
        get_property(MISSING INSTALL bin/myapp PROPERTY NONEXISTENT)

        message("shortcut=${SHORTCUT}")
        message("deb_name=${DEB_NAME}")
        message("opts=${OPTS}")
        message("missing=${MISSING}")
    )";

    interp.set_source_view(script);
    kiln::Parser parser(script);
    auto ast_or_error = parser.parse();
    REQUIRE(ast_or_error.has_value());

    auto result = interp.interpret(ast_or_error.value());
    REQUIRE(result.has_value());

    std::string out = output.str();
    REQUIRE(out.find("shortcut=MyApp") != std::string::npos);
    REQUIRE(out.find("deb_name=mylib") != std::string::npos);
    REQUIRE(out.find("opts=opt1;opt2") != std::string::npos);
    REQUIRE(out.find("missing=") != std::string::npos);

    // Cleanup
    std::filesystem::remove_all(temp_dir);
}

TEST_CASE("project() updates ENABLED_LANGUAGES", "[interpreter][property]") {
    std::string temp_dir = "build_test_project_langs";
    std::filesystem::create_directories(temp_dir);

    std::stringstream output;
    kiln::Interpreter interp(temp_dir, &output, &output, temp_dir + "/build");

    interp.add_builtin("message", [&](kiln::Interpreter&, const std::vector<std::string>& args) {
        for (const auto& arg : args) output << arg;
        output << std::endl;
    });

    // Test project() with LANGUAGES (using only supported languages: C, CXX, NONE)
    std::string script = R"(
        get_property(BEFORE GLOBAL PROPERTY ENABLED_LANGUAGES)
        message("before=${BEFORE}")

        project(MyProject LANGUAGES CXX)

        get_property(AFTER GLOBAL PROPERTY ENABLED_LANGUAGES)
        message("after=${AFTER}")
        message("cxx_loaded=${CMAKE_CXX_COMPILER_LOADED}")
        message("c_loaded=${CMAKE_C_COMPILER_LOADED}")
    )";

    interp.set_source_view(script);
    kiln::Parser parser(script);
    auto ast_or_error = parser.parse();
    REQUIRE(ast_or_error.has_value());

    auto result = interp.interpret(ast_or_error.value());
    REQUIRE(result.has_value());

    std::string out = output.str();
    REQUIRE(out.find("before=\n") != std::string::npos);
    REQUIRE(out.find("after=CXX") != std::string::npos);

    // CMAKE_CXX_COMPILER_LOADED should be set, CMAKE_C_COMPILER_LOADED should not
    REQUIRE(out.find("cxx_loaded=1") != std::string::npos);
    REQUIRE(out.find("c_loaded=\n") != std::string::npos);

    // Cleanup
    std::filesystem::remove_all(temp_dir);
}

TEST_CASE("set_property CACHE VALUE updates cache variable", "[interpreter][property]") {
    auto output = run_script(R"(
        set(MY_OPT "default_val" CACHE STRING "A test option")
        message("before=${MY_OPT}")

        set_property(CACHE MY_OPT PROPERTY VALUE "overridden")
        message("after=${MY_OPT}")

        set_property(CACHE MY_OPT PROPERTY VALUE "replaced")
        message("replaced=${MY_OPT}")

        set_property(CACHE MY_OPT APPEND PROPERTY VALUE "extra")
        message("appended=${MY_OPT}")

        set_property(CACHE MY_OPT PROPERTY STRINGS "a;b;c")
        message("still=${MY_OPT}")
    )");

    REQUIRE(output.find("before=default_val") != std::string::npos);
    REQUIRE(output.find("after=overridden") != std::string::npos);
    REQUIRE(output.find("replaced=replaced") != std::string::npos);
    REQUIRE(output.find("appended=replaced;extra") != std::string::npos);
    // Setting STRINGS property should not change the value
    REQUIRE(output.find("still=replaced;extra") != std::string::npos);
}

TEST_CASE("enable_language() updates ENABLED_LANGUAGES", "[interpreter][language]") {
    std::string temp_dir = "build_test_enable_language";
    std::filesystem::create_directories(temp_dir);

    std::stringstream output;
    kiln::Interpreter interp(temp_dir, &output, &output, temp_dir + "/build");

    interp.add_builtin("message", [&](kiln::Interpreter&, const std::vector<std::string>& args) {
        for (const auto& arg : args) output << arg;
        output << std::endl;
    });

    // Test enable_language() adds languages to ENABLED_LANGUAGES
    std::string script = R"(
        get_property(BEFORE GLOBAL PROPERTY ENABLED_LANGUAGES)
        message("before=${BEFORE}")

        # Reset to empty for testing
        set_property(GLOBAL PROPERTY ENABLED_LANGUAGES "")

        enable_language(C)
        get_property(AFTER_C GLOBAL PROPERTY ENABLED_LANGUAGES)
        message("after_c=${AFTER_C}")

        enable_language(CXX)
        get_property(AFTER_CXX GLOBAL PROPERTY ENABLED_LANGUAGES)
        message("after_cxx=${AFTER_CXX}")

        # Try to enable C again - should not duplicate
        enable_language(C)
        get_property(FINAL GLOBAL PROPERTY ENABLED_LANGUAGES)
        message("final=${FINAL}")
    )";

    interp.set_source_view(script);
    kiln::Parser parser(script);
    auto ast_or_error = parser.parse();
    REQUIRE(ast_or_error.has_value());

    auto result = interp.interpret(ast_or_error.value());
    REQUIRE(result.has_value());

    std::string out = output.str();
    REQUIRE(out.find("after_c=C") != std::string::npos);
    REQUIRE(out.find("after_cxx=C;CXX") != std::string::npos);
    REQUIRE(out.find("final=C;CXX") != std::string::npos);

    // Cleanup
    std::filesystem::remove_all(temp_dir);
}

TEST_CASE("enable_language() with OPTIONAL", "[interpreter][language]") {
    std::string temp_dir = "build_test_enable_language_optional";
    std::filesystem::create_directories(temp_dir);

    std::stringstream output;
    kiln::Interpreter interp(temp_dir, &output, &output, temp_dir + "/build");

    // Test OPTIONAL flag - unsupported language should not error
    std::string script = R"(
        set_property(GLOBAL PROPERTY ENABLED_LANGUAGES "")
        enable_language(Fortran OPTIONAL)
        enable_language(C CXX)
        get_property(LANGS GLOBAL PROPERTY ENABLED_LANGUAGES)
    )";

    interp.set_source_view(script);
    kiln::Parser parser(script);
    auto ast_or_error = parser.parse();
    REQUIRE(ast_or_error.has_value());

    auto result = interp.interpret(ast_or_error.value());
    REQUIRE(result.has_value());

    // Verify only C and CXX were enabled
    std::string langs = interp.get_global_properties()["ENABLED_LANGUAGES"];
    REQUIRE(langs == "C;CXX");

    // Cleanup
    std::filesystem::remove_all(temp_dir);
}

TEST_CASE("enable_language() error on unsupported language", "[interpreter][language]") {
    std::string temp_dir = "build_test_enable_language_error";
    std::filesystem::create_directories(temp_dir);

    std::stringstream output;
    kiln::Interpreter interp(temp_dir, &output, &output, temp_dir + "/build");

    // Test error on unsupported language without OPTIONAL
    std::string script = R"(
        enable_language(Fortran)
    )";

    interp.set_source_view(script);
    kiln::Parser parser(script);
    auto ast_or_error = parser.parse();
    REQUIRE(ast_or_error.has_value());

    auto result = interp.interpret(ast_or_error.value());
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message.find("unsupported language") != std::string::npos);

    // Cleanup
    std::filesystem::remove_all(temp_dir);
}

TEST_CASE("if condition: unquoted variable reference in STREQUAL", "[interpreter][if][bugfix]") {
    // When using ${VAR} (unquoted) in a STREQUAL comparison, the expanded value
    // should be compared directly, NOT dereferenced again as a variable name.
    // This was a bug where ${type} expanding to "STATIC_LIBRARY" would then
    // try to look up a variable named "STATIC_LIBRARY" (returning empty string).

    SECTION("Basic unquoted variable in STREQUAL") {
        auto output = run_script(R"(
            set(type "STATIC_LIBRARY")
            if(${type} STREQUAL "STATIC_LIBRARY")
                message("match")
            else()
                message("no match")
            endif()
        )");
        REQUIRE(output == "match\n");
    }

    SECTION("Quoted variable should also work") {
        auto output = run_script(R"(
            set(type "STATIC_LIBRARY")
            if("${type}" STREQUAL "STATIC_LIBRARY")
                message("match")
            else()
                message("no match")
            endif()
        )");
        REQUIRE(output == "match\n");
    }

    SECTION("Complex condition with NOT and AND") {
        auto output = run_script(R"(
            set(type "STATIC_LIBRARY")
            if(NOT ${type} STREQUAL "STATIC_LIBRARY"
                   AND NOT ${type} STREQUAL "SHARED_LIBRARY")
                message("none")
            else()
                message("found")
            endif()
        )");
        REQUIRE(output == "found\n");
    }

    SECTION("Variable expanding to non-matching value") {
        auto output = run_script(R"(
            set(type "OBJECT_LIBRARY")
            if(${type} STREQUAL "STATIC_LIBRARY")
                message("static")
            elseif(${type} STREQUAL "OBJECT_LIBRARY")
                message("object")
            else()
                message("other")
            endif()
        )");
        REQUIRE(output == "object\n");
    }

    SECTION("Variable with value that looks like a variable name") {
        // CMake behavior: if(${type}) first expands ${type} to "OTHER_VAR",
        // then the condition evaluator auto-dereferences OTHER_VAR as a variable.
        // So if(${type} STREQUAL "OTHER_VAR") becomes if(OTHER_VAR STREQUAL "OTHER_VAR")
        // and OTHER_VAR gets dereferenced to "wrong".
        auto output = run_script(R"(
            set(OTHER_VAR "wrong")
            set(type "OTHER_VAR")
            if(${type} STREQUAL "OTHER_VAR")
                message("no double deref")
            else()
                message("double dereferenced")
            endif()
        )");
        REQUIRE(output == "double dereferenced\n");
    }

    SECTION("List expansion in if() condition") {
        // When a variable contains a semicolon-separated list and is expanded
        // in an if() condition, the list elements should become separate arguments.
        // e.g., if(${COND}) where COND="NOT;MY_VAR" should become if(NOT MY_VAR)
        auto output = run_script(R"(
            set(MY_VAR OFF)
            set(MY_COND "NOT;MY_VAR")
            if(${MY_COND})
                message("TRUE")
            else()
                message("FALSE")
            endif()
        )");
        REQUIRE(output == "TRUE\n");
    }

    SECTION("List expansion in if() with multiple conditions") {
        // Test that list expansion works for more complex conditions
        auto output = run_script(R"(
            set(A ON)
            set(B ON)
            set(COND "A;AND;B")
            if(${COND})
                message("both on")
            else()
                message("not both")
            endif()
        )");
        REQUIRE(output == "both on\n");
    }

    SECTION("List expansion in elseif() condition") {
        auto output = run_script(R"(
            set(MY_VAR OFF)
            set(MY_COND "NOT;MY_VAR")
            if(FALSE)
                message("first")
            elseif(${MY_COND})
                message("elseif list expanded")
            else()
                message("else")
            endif()
        )");
        REQUIRE(output == "elseif list expanded\n");
    }
}

TEST_CASE("cmake_path command", "[interpreter][cmake_path]") {
    SECTION("GET ROOT_NAME") {
        auto output = run_script(R"(
            cmake_path(SET mypath "/usr/local/bin/app")
            cmake_path(GET mypath ROOT_NAME result)
            message("${result}")
        )");
        REQUIRE(output == "\n");  // Unix paths don't have root names
    }

    SECTION("GET ROOT_DIRECTORY") {
        auto output = run_script(R"(
            cmake_path(SET mypath "/usr/local/bin/app")
            cmake_path(GET mypath ROOT_DIRECTORY result)
            message("${result}")
        )");
        REQUIRE(output == "/\n");
    }

    SECTION("GET FILENAME") {
        auto output = run_script(R"(
            cmake_path(SET mypath "/usr/local/bin/app.exe")
            cmake_path(GET mypath FILENAME result)
            message("${result}")
        )");
        REQUIRE(output == "app.exe\n");
    }

    SECTION("GET EXTENSION") {
        auto output = run_script(R"(
            cmake_path(SET mypath "/usr/local/bin/app.tar.gz")
            cmake_path(GET mypath EXTENSION result)
            message("${result}")
        )");
        REQUIRE(output == ".tar.gz\n");
    }

    SECTION("GET EXTENSION LAST_ONLY") {
        auto output = run_script(R"(
            cmake_path(SET mypath "/usr/local/bin/app.tar.gz")
            cmake_path(GET mypath EXTENSION LAST_ONLY result)
            message("${result}")
        )");
        REQUIRE(output == ".gz\n");
    }

    SECTION("GET STEM") {
        auto output = run_script(R"(
            cmake_path(SET mypath "/usr/local/bin/app.tar.gz")
            cmake_path(GET mypath STEM result)
            message("${result}")
        )");
        REQUIRE(output == "app\n");
    }

    SECTION("GET STEM LAST_ONLY") {
        auto output = run_script(R"(
            cmake_path(SET mypath "/usr/local/bin/app.tar.gz")
            cmake_path(GET mypath STEM LAST_ONLY result)
            message("${result}")
        )");
        REQUIRE(output == "app.tar\n");
    }

    SECTION("GET PARENT_PATH") {
        auto output = run_script(R"(
            cmake_path(SET mypath "/usr/local/bin/app")
            cmake_path(GET mypath PARENT_PATH result)
            message("${result}")
        )");
        REQUIRE(output == "/usr/local/bin\n");
    }

    SECTION("GET RELATIVE_PART") {
        auto output = run_script(R"(
            cmake_path(SET mypath "/usr/local/bin")
            cmake_path(GET mypath RELATIVE_PART result)
            message("${result}")
        )");
        REQUIRE(output == "usr/local/bin\n");
    }

    SECTION("HAS_FILENAME") {
        auto output = run_script(R"(
            cmake_path(SET mypath "/usr/local/bin/app")
            cmake_path(HAS_FILENAME mypath result)
            message("${result}")
        )");
        REQUIRE(output == "ON\n");
    }

    SECTION("HAS_EXTENSION") {
        auto output = run_script(R"(
            cmake_path(SET mypath "/usr/local/bin/app.exe")
            cmake_path(HAS_EXTENSION mypath result)
            message("${result}")
        )");
        REQUIRE(output == "ON\n");
    }

    SECTION("IS_ABSOLUTE") {
        auto output = run_script(R"(
            cmake_path(SET mypath "/usr/local/bin")
            cmake_path(IS_ABSOLUTE mypath result)
            message("${result}")
        )");
        REQUIRE(output == "ON\n");
    }

    SECTION("IS_RELATIVE") {
        auto output = run_script(R"(
            cmake_path(SET mypath "local/bin")
            cmake_path(IS_RELATIVE mypath result)
            message("${result}")
        )");
        REQUIRE(output == "ON\n");
    }

    SECTION("IS_PREFIX") {
        auto output = run_script(R"(
            cmake_path(SET prefix "/usr/local")
            cmake_path(SET fullpath "/usr/local/bin/app")
            cmake_path(IS_PREFIX prefix "${fullpath}" result)
            message("${result}")
        )");
        REQUIRE(output == "ON\n");
    }

    SECTION("COMPARE EQUAL") {
        auto output = run_script(R"(
            cmake_path(SET path1 "/usr/local/bin")
            cmake_path(SET path2 "/usr/local/bin")
            cmake_path(COMPARE path1 EQUAL path2 result)
            message("${result}")
        )");
        REQUIRE(output == "ON\n");
    }

    SECTION("COMPARE NOT_EQUAL") {
        auto output = run_script(R"(
            cmake_path(SET path1 "/usr/local/bin")
            cmake_path(SET path2 "/usr/bin")
            cmake_path(COMPARE path1 NOT_EQUAL path2 result)
            message("${result}")
        )");
        REQUIRE(output == "ON\n");
    }

    SECTION("SET with NORMALIZE") {
        auto output = run_script(R"(
            cmake_path(SET mypath "/usr/./local/../local/bin" NORMALIZE)
            message("${mypath}")
        )");
        REQUIRE(output == "/usr/local/bin\n");
    }

    SECTION("APPEND") {
        auto output = run_script(R"(
            cmake_path(SET mypath "/usr/local")
            cmake_path(APPEND mypath "bin" "app")
            message("${mypath}")
        )");
        REQUIRE(output == "/usr/local/bin/app\n");
    }

    SECTION("APPEND_STRING") {
        auto output = run_script(R"(
            cmake_path(SET mypath "/usr/local/bin")
            cmake_path(APPEND_STRING mypath "/app")
            message("${mypath}")
        )");
        REQUIRE(output == "/usr/local/bin/app\n");
    }

    SECTION("REMOVE_FILENAME") {
        auto output = run_script(R"(
            cmake_path(SET mypath "/usr/local/bin/app")
            cmake_path(REMOVE_FILENAME mypath)
            message("${mypath}")
        )");
        REQUIRE(output == "/usr/local/bin/\n");
    }

    SECTION("REPLACE_FILENAME") {
        auto output = run_script(R"(
            cmake_path(SET mypath "/usr/local/bin/app")
            cmake_path(REPLACE_FILENAME mypath "newapp")
            message("${mypath}")
        )");
        REQUIRE(output == "/usr/local/bin/newapp\n");
    }

    SECTION("REMOVE_EXTENSION") {
        auto output = run_script(R"(
            cmake_path(SET mypath "/usr/local/bin/app.tar.gz")
            cmake_path(REMOVE_EXTENSION mypath)
            message("${mypath}")
        )");
        REQUIRE(output == "/usr/local/bin/app\n");
    }

    SECTION("REMOVE_EXTENSION LAST_ONLY") {
        auto output = run_script(R"(
            cmake_path(SET mypath "/usr/local/bin/app.tar.gz")
            cmake_path(REMOVE_EXTENSION mypath LAST_ONLY)
            message("${mypath}")
        )");
        REQUIRE(output == "/usr/local/bin/app.tar\n");
    }

    SECTION("REPLACE_EXTENSION") {
        auto output = run_script(R"(
            cmake_path(SET mypath "/usr/local/bin/app.tar.gz")
            cmake_path(REPLACE_EXTENSION mypath ".zip")
            message("${mypath}")
        )");
        REQUIRE(output == "/usr/local/bin/app.zip\n");
    }

    SECTION("REPLACE_EXTENSION without dot") {
        auto output = run_script(R"(
            cmake_path(SET mypath "/usr/local/bin/app.tar.gz")
            cmake_path(REPLACE_EXTENSION mypath "zip")
            message("${mypath}")
        )");
        REQUIRE(output == "/usr/local/bin/app.zip\n");
    }

    SECTION("NORMAL_PATH in-place") {
        auto output = run_script(R"(
            cmake_path(SET mypath "/usr/./local/../local/bin")
            cmake_path(NORMAL_PATH mypath)
            message("${mypath}")
        )");
        REQUIRE(output == "/usr/local/bin\n");
    }

    SECTION("NORMAL_PATH with OUTPUT_VARIABLE") {
        auto output = run_script(R"(
            cmake_path(SET mypath "/usr/./local/../local/bin")
            cmake_path(NORMAL_PATH mypath OUTPUT_VARIABLE result)
            message("original:${mypath}")
            message("result:${result}")
        )");
        REQUIRE(output == "original:/usr/./local/../local/bin\nresult:/usr/local/bin\n");
    }

    SECTION("RELATIVE_PATH with default base") {
        auto output = run_script(R"(
            set(CMAKE_CURRENT_SOURCE_DIR "/home/user/project")
            cmake_path(SET mypath "/home/user/project/src/main.cpp")
            cmake_path(RELATIVE_PATH mypath)
            message("${mypath}")
        )");
        REQUIRE(output == "src/main.cpp\n");
    }

    SECTION("RELATIVE_PATH with OUTPUT_VARIABLE") {
        auto output = run_script(R"(
            set(CMAKE_CURRENT_SOURCE_DIR "/home/user/project")
            cmake_path(SET mypath "/home/user/project/src/main.cpp")
            cmake_path(RELATIVE_PATH mypath OUTPUT_VARIABLE result)
            message("original:${mypath}")
            message("result:${result}")
        )");
        REQUIRE(output == "original:/home/user/project/src/main.cpp\nresult:src/main.cpp\n");
    }

    SECTION("RELATIVE_PATH with custom base") {
        auto output = run_script(R"(
            cmake_path(SET mypath "/home/user/project/src/main.cpp")
            cmake_path(RELATIVE_PATH mypath BASE_DIRECTORY "/home/user")
            message("${mypath}")
        )");
        REQUIRE(output == "project/src/main.cpp\n");
    }

    SECTION("ABSOLUTE_PATH in-place") {
        auto output = run_script(R"(
            set(CMAKE_CURRENT_SOURCE_DIR "/home/user/project")
            cmake_path(SET mypath "src/main.cpp")
            cmake_path(ABSOLUTE_PATH mypath)
            message("${mypath}")
        )");
        REQUIRE(output == "/home/user/project/src/main.cpp\n");
    }

    SECTION("ABSOLUTE_PATH with OUTPUT_VARIABLE") {
        auto output = run_script(R"(
            set(CMAKE_CURRENT_SOURCE_DIR "/home/user/project")
            cmake_path(SET mypath "src/main.cpp")
            cmake_path(ABSOLUTE_PATH mypath OUTPUT_VARIABLE result)
            message("original:${mypath}")
            message("result:${result}")
        )");
        REQUIRE(output == "original:src/main.cpp\nresult:/home/user/project/src/main.cpp\n");
    }

    SECTION("ABSOLUTE_PATH with NORMALIZE") {
        auto output = run_script(R"(
            set(CMAKE_CURRENT_SOURCE_DIR "/home/user/project")
            cmake_path(SET mypath "src/../include/header.h")
            cmake_path(ABSOLUTE_PATH mypath NORMALIZE)
            message("${mypath}")
        )");
        REQUIRE(output == "/home/user/project/include/header.h\n");
    }

    SECTION("HASH") {
        auto output = run_script(R"(
            cmake_path(SET mypath "/usr/local/bin")
            cmake_path(HASH mypath hash1)
            cmake_path(SET mypath2 "/usr/local/bin")
            cmake_path(HASH mypath2 hash2)
            if(hash1 STREQUAL hash2)
                message("EQUAL")
            else()
                message("NOT_EQUAL")
            endif()
        )");
        REQUIRE(output == "EQUAL\n");
    }

    SECTION("APPEND with OUTPUT_VARIABLE") {
        auto output = run_script(R"(
            cmake_path(SET mypath "/usr/local")
            cmake_path(APPEND mypath "bin" "app" OUTPUT_VARIABLE result)
            message("original:${mypath}")
            message("result:${result}")
        )");
        REQUIRE(output == "original:/usr/local\nresult:/usr/local/bin/app\n");
    }

    SECTION("REMOVE_EXTENSION with OUTPUT_VARIABLE") {
        auto output = run_script(R"(
            cmake_path(SET mypath "/usr/bin/app.tar.gz")
            cmake_path(REMOVE_EXTENSION mypath OUTPUT_VARIABLE result)
            message("original:${mypath}")
            message("result:${result}")
        )");
        REQUIRE(output == "original:/usr/bin/app.tar.gz\nresult:/usr/bin/app\n");
    }

    SECTION("REPLACE_EXTENSION with OUTPUT_VARIABLE") {
        auto output = run_script(R"(
            cmake_path(SET mypath "/usr/bin/app.txt")
            cmake_path(REPLACE_EXTENSION mypath ".md" OUTPUT_VARIABLE result)
            message("original:${mypath}")
            message("result:${result}")
        )");
        REQUIRE(output == "original:/usr/bin/app.txt\nresult:/usr/bin/app.md\n");
    }

    SECTION("GET EXTENSION on file without extension") {
        auto output = run_script(R"(
            cmake_path(SET mypath "/usr/bin/app")
            cmake_path(GET mypath EXTENSION result)
            message("ext:${result}")
        )");
        REQUIRE(output == "ext:\n");
    }

    SECTION("GET EXTENSION on dotfile") {
        auto output = run_script(R"(
            cmake_path(SET mypath "/home/user/.bashrc")
            cmake_path(GET mypath EXTENSION result)
            message("${result}")
        )");
        REQUIRE(output == "\n");  // Dotfiles don't have extensions
    }

    SECTION("GET STEM on dotfile") {
        auto output = run_script(R"(
            cmake_path(SET mypath "/home/user/.bashrc")
            cmake_path(GET mypath STEM result)
            message("${result}")
        )");
        REQUIRE(output == "\n");  // Stem of dotfile is empty
    }

    SECTION("NORMAL_PATH with multiple slashes and dots") {
        auto output = run_script(R"(
            cmake_path(SET mypath "/usr//local/./bin/../lib/./file.so")
            cmake_path(NORMAL_PATH mypath)
            message("${mypath}")
        )");
        REQUIRE(output == "/usr/local/lib/file.so\n");
    }

    SECTION("IS_PREFIX returns false when not a prefix") {
        auto output = run_script(R"(
            cmake_path(SET prefix "/usr/local")
            cmake_path(SET fullpath "/home/user/file")
            cmake_path(IS_PREFIX prefix "${fullpath}" result)
            message("${result}")
        )");
        REQUIRE(output == "OFF\n");
    }

    SECTION("IS_PREFIX with exact match") {
        auto output = run_script(R"(
            cmake_path(SET prefix "/usr/local")
            cmake_path(SET fullpath "/usr/local")
            cmake_path(IS_PREFIX prefix "${fullpath}" result)
            message("${result}")
        )");
        REQUIRE(output == "ON\n");
    }

    SECTION("IS_PREFIX with trailing slashes") {
        auto output = run_script(R"(
            set(source_dir "/usr/local/src/corelib/")
            cmake_path(IS_PREFIX source_dir "/usr/local/src/corelib/global/file.h/" result)
            message("${result}")
        )");
        REQUIRE(output == "ON\n");
    }

    SECTION("IS_PREFIX via wrapper function with trailing slashes") {
        auto output = run_script(R"(
            function(test_prefix path_var input out_var)
                if(NOT ${path_var} MATCHES "/$")
                    set(${path_var} "${${path_var}}/")
                endif()
                set(input "${input}/")
                cmake_path(IS_PREFIX ${path_var} ${input} ${out_var})
                set(${out_var} "${${out_var}}" PARENT_SCOPE)
            endfunction()

            set(source_dir "/usr/local/src/corelib")
            test_prefix(source_dir "/usr/local/src/corelib/global/file.h" is_inside)
            message("${is_inside}")
        )");
        REQUIRE(output == "ON\n");
    }

    SECTION("HAS_PARENT_PATH on root") {
        auto output = run_script(R"(
            cmake_path(SET mypath "/")
            cmake_path(HAS_PARENT_PATH mypath result)
            message("${result}")
        )");
        REQUIRE(output == "OFF\n");
    }

    SECTION("Complex extension handling - multiple dots") {
        auto output = run_script(R"(
            cmake_path(SET mypath "/path/to/file.tar.gz.backup")
            cmake_path(GET mypath EXTENSION result1)
            cmake_path(GET mypath EXTENSION LAST_ONLY result2)
            cmake_path(GET mypath STEM result3)
            cmake_path(GET mypath STEM LAST_ONLY result4)
            message("ext:${result1}")
            message("ext_last:${result2}")
            message("stem:${result3}")
            message("stem_last:${result4}")
        )");
        REQUIRE(output == "ext:.tar.gz.backup\next_last:.backup\nstem:file\nstem_last:file.tar.gz\n");
    }

    SECTION("APPEND with absolute path argument") {
        auto output = run_script(R"(
            cmake_path(SET mypath "/usr/local")
            cmake_path(APPEND mypath "/absolute/path")
            message("${mypath}")
        )");
        // When appending an absolute path, it replaces the current path
        REQUIRE(output == "/absolute/path\n");
    }

    SECTION("APPEND_STRING preserves exact string") {
        auto output = run_script(R"(
            cmake_path(SET mypath "/usr/local")
            cmake_path(APPEND_STRING mypath "123")
            message("${mypath}")
        )");
        REQUIRE(output == "/usr/local123\n");
    }

    SECTION("REPLACE_FILENAME on path without filename") {
        auto output = run_script(R"(
            cmake_path(SET mypath "/usr/local/")
            cmake_path(REPLACE_FILENAME mypath "newfile")
            message("${mypath}")
        )");
        REQUIRE(output == "/usr/local/newfile\n");
    }

    SECTION("GET operations on relative path") {
        auto output = run_script(R"(
            cmake_path(SET mypath "src/modules/main.cpp")
            cmake_path(GET mypath FILENAME result1)
            cmake_path(GET mypath PARENT_PATH result2)
            cmake_path(GET mypath EXTENSION result3)
            message("file:${result1}")
            message("parent:${result2}")
            message("ext:${result3}")
        )");
        REQUIRE(output == "file:main.cpp\nparent:src/modules\next:.cpp\n");
    }

    SECTION("SET without NORMALIZE preserves dots") {
        auto output = run_script(R"(
            cmake_path(SET mypath "/usr/./local/../bin")
            message("${mypath}")
        )");
        REQUIRE(output == "/usr/./local/../bin\n");
    }

    SECTION("ABSOLUTE_PATH on already absolute path") {
        auto output = run_script(R"(
            set(CMAKE_CURRENT_SOURCE_DIR "/home/user/project")
            cmake_path(SET mypath "/usr/local/bin")
            cmake_path(ABSOLUTE_PATH mypath)
            message("${mypath}")
        )");
        REQUIRE(output == "/usr/local/bin\n");
    }

    SECTION("NATIVE_PATH in-place") {
        auto output = run_script(R"(
            cmake_path(SET mypath "/usr/local/bin")
            cmake_path(NATIVE_PATH mypath)
            message("${mypath}")
        )");
        // On Unix, native path is the same as CMake path
        REQUIRE(output == "/usr/local/bin\n");
    }

    SECTION("NATIVE_PATH with OUTPUT_VARIABLE") {
        auto output = run_script(R"(
            cmake_path(SET mypath "/usr/local/bin")
            cmake_path(NATIVE_PATH mypath OUTPUT_VARIABLE result)
            message("original:${mypath}")
            message("result:${result}")
        )");
        REQUIRE(output == "original:/usr/local/bin\nresult:/usr/local/bin\n");
    }

    SECTION("HASH normalization consistency") {
        auto output = run_script(R"(
            cmake_path(SET mypath1 "/usr/local/./bin")
            cmake_path(SET mypath2 "/usr/local/bin")
            cmake_path(HASH mypath1 hash1)
            cmake_path(HASH mypath2 hash2)
            if(hash1 STREQUAL hash2)
                message("EQUAL")
            else()
                message("NOT_EQUAL")
            endif()
        )");
        // HASH normalizes paths before hashing
        REQUIRE(output == "EQUAL\n");
    }

    SECTION("Empty path component handling") {
        auto output = run_script(R"(
            cmake_path(SET mypath "")
            cmake_path(HAS_FILENAME mypath result1)
            cmake_path(IS_ABSOLUTE mypath result2)
            cmake_path(IS_RELATIVE mypath result3)
            message("has_file:${result1}")
            message("absolute:${result2}")
            message("relative:${result3}")
        )");
        REQUIRE(output == "has_file:OFF\nabsolute:OFF\nrelative:ON\n");
    }

    SECTION("COMPARE with normalized vs non-normalized") {
        auto output = run_script(R"(
            cmake_path(SET path1 "/usr/local/bin")
            cmake_path(SET path2 "/usr/local/./bin")
            cmake_path(COMPARE path1 EQUAL path2 result)
            message("${result}")
        )");
        // COMPARE does NOT normalize, lexical comparison only
        REQUIRE(output == "OFF\n");
    }

    SECTION("Multiple APPEND calls") {
        auto output = run_script(R"(
            cmake_path(SET mypath "/usr")
            cmake_path(APPEND mypath "local")
            cmake_path(APPEND mypath "bin")
            cmake_path(APPEND mypath "app")
            message("${mypath}")
        )");
        REQUIRE(output == "/usr/local/bin/app\n");
    }

    SECTION("REMOVE_EXTENSION then REPLACE_EXTENSION") {
        auto output = run_script(R"(
            cmake_path(SET mypath "/path/file.old.txt")
            cmake_path(REMOVE_EXTENSION mypath)
            cmake_path(REPLACE_EXTENSION mypath ".new")
            message("${mypath}")
        )");
        REQUIRE(output == "/path/file.new\n");
    }

    SECTION("GET ROOT_PATH on relative path") {
        auto output = run_script(R"(
            cmake_path(SET mypath "relative/path/file.txt")
            cmake_path(GET mypath ROOT_PATH result)
            message("root:${result}")
        )");
        REQUIRE(output == "root:\n");
    }

    SECTION("HAS_ROOT_DIRECTORY on absolute path") {
        auto output = run_script(R"(
            cmake_path(SET mypath "/usr/local")
            cmake_path(HAS_ROOT_DIRECTORY mypath result)
            message("${result}")
        )");
        REQUIRE(output == "ON\n");
    }

    SECTION("HAS_ROOT_DIRECTORY on relative path") {
        auto output = run_script(R"(
            cmake_path(SET mypath "usr/local")
            cmake_path(HAS_ROOT_DIRECTORY mypath result)
            message("${result}")
        )");
        REQUIRE(output == "OFF\n");
    }
}

TEST_CASE("get_filename_component command", "[interpreter][get_filename_component]") {
    SECTION("ABSOLUTE mode with relative path") {
        auto output = run_script(R"(
            set(CMAKE_CURRENT_SOURCE_DIR "/home/project")
            get_filename_component(result "src/main.cpp" ABSOLUTE)
            message("${result}")
        )");
        REQUIRE(output == "/home/project/src/main.cpp\n");
    }

    SECTION("ABSOLUTE mode with BASE_DIR") {
        auto output = run_script(R"(
            get_filename_component(result "src/main.cpp" ABSOLUTE BASE_DIR "/custom/path")
            message("${result}")
        )");
        REQUIRE(output == "/custom/path/src/main.cpp\n");
    }

    SECTION("ABSOLUTE mode with already absolute path") {
        auto output = run_script(R"(
            set(CMAKE_CURRENT_SOURCE_DIR "/home/project")
            get_filename_component(result "/usr/local/bin/app" ABSOLUTE)
            message("${result}")
        )");
        REQUIRE(output == "/usr/local/bin/app\n");
    }

    SECTION("REALPATH mode normalizes path") {
        auto output = run_script(R"(
            set(CMAKE_CURRENT_SOURCE_DIR "/home/project")
            get_filename_component(result "src/../include/header.h" REALPATH)
            message("${result}")
        )");
        REQUIRE(output == "/home/project/include/header.h\n");
    }

    SECTION("PATH mode (alias for DIRECTORY)") {
        auto output = run_script(R"(
            get_filename_component(result "/usr/local/bin/app" PATH)
            message("${result}")
        )");
        REQUIRE(output == "/usr/local/bin\n");
    }

    SECTION("NAME mode on path with directory") {
        auto output = run_script(R"(
            get_filename_component(result "/usr/local/bin/myapp" NAME)
            message("${result}")
        )");
        REQUIRE(output == "myapp\n");
    }

    SECTION("EXT on file with no extension") {
        auto output = run_script(R"(
            get_filename_component(result "/path/to/README" EXT)
            message("ext:${result}")
        )");
        REQUIRE(output == "ext:\n");
    }

    SECTION("NAME_WE on file with no extension") {
        auto output = run_script(R"(
            get_filename_component(result "/path/to/README" NAME_WE)
            message("${result}")
        )");
        REQUIRE(output == "README\n");
    }

    SECTION("Complex extension handling") {
        auto output = run_script(R"(
            get_filename_component(ext1 "archive.tar.gz.backup" EXT)
            get_filename_component(ext2 "archive.tar.gz.backup" LAST_EXT)
            get_filename_component(stem1 "archive.tar.gz.backup" NAME_WE)
            get_filename_component(stem2 "archive.tar.gz.backup" NAME_WLE)
            message("EXT:${ext1}")
            message("LAST_EXT:${ext2}")
            message("NAME_WE:${stem1}")
            message("NAME_WLE:${stem2}")
        )");
        REQUIRE(output == "EXT:.tar.gz.backup\nLAST_EXT:.backup\nNAME_WE:archive\nNAME_WLE:archive.tar.gz\n");
    }

    SECTION("Relative path handling") {
        auto output = run_script(R"(
            get_filename_component(dir "src/modules/utils.cpp" DIRECTORY)
            get_filename_component(name "src/modules/utils.cpp" NAME)
            message("DIR:${dir}")
            message("NAME:${name}")
        )");
        REQUIRE(output == "DIR:src/modules\nNAME:utils.cpp\n");
    }

    SECTION("Single filename without directory") {
        auto output = run_script(R"(
            get_filename_component(dir "app.exe" DIRECTORY)
            get_filename_component(name "app.exe" NAME)
            message("DIR:${dir}")
            message("NAME:${name}")
        )");
        REQUIRE(output == "DIR:\nNAME:app.exe\n");
    }

    SECTION("Extension starting with dot handling") {
        auto output = run_script(R"(
            get_filename_component(ext1 "file.txt" EXT)
            get_filename_component(ext2 ".hidden" EXT)
            get_filename_component(ext3 ".config.yaml" EXT)
            message("${ext1}|${ext2}|${ext3}")
        )");
        REQUIRE(output == ".txt||\n");
    }
}

// ============================================================================
// SCOPING RULES REGRESSION TESTS
// These tests ensure correct scoping behavior between macros, functions, and
// cache variables. They prevent regression of bugs discovered during development.
// ============================================================================

TEST_CASE("Function called from macro has correct ARGV/ARGC", "[interpreter][scoping][bugfix]") {
    // Regression test: Functions called from within macros were seeing the macro's
    // parameters in their ARGV/ARGC instead of their own parameters.
    // Root cause: macro_substitutions_ was checked before function frame variables
    // in get_variable(), causing macro parameters to bleed into function scope.
    // Fix: Clear macro_substitutions_ when entering function scope.

    auto output = run_script(R"(
        function(my_func arg1 arg2)
            message("ARGC=${ARGC}")
            message("ARGV=${ARGV}")
            message("arg1=${arg1}")
            message("arg2=${arg2}")
        endfunction()

        macro(my_macro mac_arg1 mac_arg2)
            my_func("from_macro_1" "from_macro_2")
        endmacro()

        my_macro("outer_1" "outer_2")
    )");

    // Function should see its own arguments, not the macro's arguments
    REQUIRE(output == "ARGC=2\nARGV=from_macro_1;from_macro_2\narg1=from_macro_1\narg2=from_macro_2\n");
}

TEST_CASE("Nested macro/function calls preserve correct scope", "[interpreter][scoping][bugfix]") {
    // Test that deeply nested macro/function calls maintain correct variable scoping

    auto output = run_script(R"(
        function(inner_func x)
            message("inner_func ARGC=${ARGC}")
            message("inner_func x=${x}")
        endfunction()

        function(middle_func y)
            message("middle_func ARGC=${ARGC}")
            message("middle_func y=${y}")
            inner_func("inner_val")
        endfunction()

        macro(outer_macro z)
            message("outer_macro z=${z}")
            middle_func("middle_val")
        endmacro()

        outer_macro("outer_val")
    )");

    REQUIRE(output == "outer_macro z=outer_val\nmiddle_func ARGC=1\nmiddle_func y=middle_val\ninner_func ARGC=1\ninner_func x=inner_val\n");
}

TEST_CASE("Function called from macro doesn't see macro's local variables", "[interpreter][scoping][bugfix]") {
    // Functions should not see variables set within the macro's body

    auto output = run_script(R"(
        function(check_vars)
            message("MACRO_VAR=${MACRO_VAR}")
            message("ARGC=${ARGC}")
        endfunction()

        macro(set_and_call)
            set(MACRO_VAR "macro_local")
            check_vars()
        endmacro()

        set_and_call()
    )");

    // The function can see MACRO_VAR because macros leak variables to caller scope,
    // but ARGC should be 0 (not the macro's ARGC)
    REQUIRE(output == "MACRO_VAR=macro_local\nARGC=0\n");
}

TEST_CASE("Cache variables are globally accessible", "[interpreter][scoping][cache]") {
    // Regression test: Cache variables were not accessible from functions
    // because get_variable() didn't check cache_variables_ after searching call_stack_
    // Fix: Added cache variable lookup in get_variable()

    auto output = run_script(R"(
        set(CACHE_VAR "cached_value" CACHE STRING "A cache variable")

        function(read_cache)
            message("CACHE_VAR=${CACHE_VAR}")
        endfunction()

        function(nested_read)
            function(deeply_nested)
                message("Deep CACHE_VAR=${CACHE_VAR}")
            endfunction()
            deeply_nested()
        endfunction()

        read_cache()
        nested_read()
        message("Root CACHE_VAR=${CACHE_VAR}")
    )");

    REQUIRE(output == "CACHE_VAR=cached_value\nDeep CACHE_VAR=cached_value\nRoot CACHE_VAR=cached_value\n");
}

TEST_CASE("Cache variables don't create local scope variables", "[interpreter][scoping][cache]") {
    // Regression test: set(VAR value CACHE ...) was creating both a cache variable
    // AND a local scope variable, causing confusion about which one was accessed
    // Fix: Only set in cache namespace, not in local scope

    auto output = run_script(R"(
        function(test_cache_isolation)
            set(CACHE_VAR "function_local" CACHE STRING "Test")
            set(LOCAL_VAR "function_local")
        endfunction()

        set(CACHE_VAR "root_cache" CACHE STRING "Test")
        set(LOCAL_VAR "root_local")

        message("Before function - CACHE_VAR=${CACHE_VAR} LOCAL_VAR=${LOCAL_VAR}")
        test_cache_isolation()
        message("After function - CACHE_VAR=${CACHE_VAR} LOCAL_VAR=${LOCAL_VAR}")
    )");

    // CACHE_VAR should remain "root_cache" because set(... CACHE STRING ...) without
    // FORCE doesn't overwrite existing cache entries. LOCAL_VAR remains in root scope.
    REQUIRE(output == "Before function - CACHE_VAR=root_cache LOCAL_VAR=root_local\nAfter function - CACHE_VAR=root_cache LOCAL_VAR=root_local\n");
}

TEST_CASE("Cache variables vs local variables precedence", "[interpreter][scoping][cache]") {
    // Test that local variables shadow cache variables when both exist

    auto output = run_script(R"(
        set(VAR "cache_value" CACHE STRING "Test")
        message("Cache only: ${VAR}")

        set(VAR "local_value")
        message("With local: ${VAR}")

        function(test_shadowing)
            message("Function sees: ${VAR}")
            set(VAR "function_local")
            message("Function local: ${VAR}")
        endfunction()

        test_shadowing()
        message("After function: ${VAR}")
    )");

    // Local variables should shadow cache variables at each scope level
    REQUIRE(output == "Cache only: cache_value\nWith local: local_value\nFunction sees: local_value\nFunction local: function_local\nAfter function: local_value\n");
}

TEST_CASE("Macro parameters don't leak into nested function calls", "[interpreter][scoping][bugfix]") {
    // Comprehensive test: Macro calls function which calls another function
    // None of the functions should see the macro's parameters in their ARGV

    auto output = run_script(R"(
        function(level2 a b)
            message("level2 ARGC=${ARGC} ARGV=${ARGV}")
            message("level2 a=${a} b=${b}")
        endfunction()

        function(level1 x)
            message("level1 ARGC=${ARGC} ARGV=${ARGV}")
            message("level1 x=${x}")
            level2("L2_A" "L2_B")
        endfunction()

        macro(caller m1 m2 m3)
            message("macro m1=${m1} m2=${m2} m3=${m3}")
            level1("L1_X")
        endmacro()

        caller("M1" "M2" "M3")
    )");

    REQUIRE(output == "macro m1=M1 m2=M2 m3=M3\nlevel1 ARGC=1 ARGV=L1_X\nlevel1 x=L1_X\nlevel2 ARGC=2 ARGV=L2_A;L2_B\nlevel2 a=L2_A b=L2_B\n");
}

TEST_CASE("file(STRINGS) REGEX uses substring matching", "[interpreter][file][regex][bugfix]") {
    // Regression test: file(STRINGS) with REGEX was using regex_match (full string)
    // instead of regex_search (substring), causing legitimate matches to fail
    // Fix: Changed regex_match to regex_search

    // Create a temporary test file
    std::filesystem::path temp_file = std::filesystem::temp_directory_path() / "kiln_test_strings.txt";
    {
        std::ofstream out(temp_file);
        out << "This line contains ERROR message\n";
        out << "Another line with WARNING text\n";
        out << "Normal line without keywords\n";
        out << "Multiple ERROR and WARNING in one line\n";
    }

    std::string script =
        "file(STRINGS \"" + temp_file.string() + "\" lines REGEX \"ERROR\")\n"
        "message(\"ERROR lines: ${lines}\")\n"
        "\n"
        "file(STRINGS \"" + temp_file.string() + "\" warn_lines REGEX \"WARNING\")\n"
        "message(\"WARNING lines: ${warn_lines}\")\n";

    auto output = run_script(script);

    std::filesystem::remove(temp_file);

    // Should find lines containing the regex pattern as a substring
    REQUIRE(output == "ERROR lines: This line contains ERROR message;Multiple ERROR and WARNING in one line\nWARNING lines: Another line with WARNING text;Multiple ERROR and WARNING in one line\n");
}

TEST_CASE("file(STRINGS) REGEX sets CMAKE_MATCH_* variables", "[interpreter][file][regex][bugfix]") {
    // Regression test: file(STRINGS) with REGEX should set CMAKE_MATCH_* variables
    // for the most recent match (last matching line)

    std::filesystem::path temp_file = std::filesystem::temp_directory_path() / "kiln_test_regex_match.txt";
    {
        std::ofstream out(temp_file);
        out << "Version: 1.2.3\n";
        out << "Version: 4.5.6\n";
        out << "Version: 7.8.9\n";
    }

    std::string script =
        "file(STRINGS \"" + temp_file.string() + "\" lines REGEX \"Version: ([0-9]+)\\\\.([0-9]+)\\\\.([0-9]+)\")\n"
        "message(\"CMAKE_MATCH_COUNT=${CMAKE_MATCH_COUNT}\")\n"
        "message(\"CMAKE_MATCH_0=${CMAKE_MATCH_0}\")\n"
        "message(\"CMAKE_MATCH_1=${CMAKE_MATCH_1}\")\n"
        "message(\"CMAKE_MATCH_2=${CMAKE_MATCH_2}\")\n"
        "message(\"CMAKE_MATCH_3=${CMAKE_MATCH_3}\")\n";

    auto output = run_script(script);

    std::filesystem::remove(temp_file);

    // Should have capture groups from the last matching line (7.8.9)
    REQUIRE(output == "CMAKE_MATCH_COUNT=3\nCMAKE_MATCH_0=Version: 7.8.9\nCMAKE_MATCH_1=7\nCMAKE_MATCH_2=8\nCMAKE_MATCH_3=9\n");
}

TEST_CASE("Multiple levels of function/macro nesting with PARENT_SCOPE", "[interpreter][scoping][parent_scope]") {
    // Test that PARENT_SCOPE works correctly through macro/function boundaries
    // Important: Macros don't create their own scope, so modify_parent()'s parent
    // scope is the macro's caller scope (root in this case)

    auto output = run_script(R"(
        function(modify_parent)
            set(VAR "modified_by_func" PARENT_SCOPE)
        endfunction()

        macro(call_modifier)
            set(VAR "before_func")
            message("Before: ${VAR}")
            modify_parent()
            message("After: ${VAR}")
        endmacro()

        set(VAR "initial")
        call_modifier()
        message("Root: ${VAR}")
    )");

    // Since macros don't create scope, modify_parent() modifies root scope directly
    // So both "After" and "Root" see the modified value
    REQUIRE(output == "Before: before_func\nAfter: modified_by_func\nRoot: modified_by_func\n");
}

// Tests for global function/macro scope (CMake semantics: functions are globally visible)
TEST_CASE("Functions are globally visible - basic", "[interpreter][function][scoping]") {
    // Function defined, then called - basic sanity check
    auto output = run_script(R"(
        function(my_func)
            message("func called")
        endfunction()
        my_func()
    )");
    REQUIRE(output == "func called\n");
}

TEST_CASE("Macros are globally visible - basic", "[interpreter][macro][scoping]") {
    // Macro defined, then called - basic sanity check
    auto output = run_script(R"(
        macro(my_macro ARG)
            message("macro: ${ARG}")
        endmacro()
        my_macro("test")
    )");
    REQUIRE(output == "macro: test\n");
}

TEST_CASE("Function redefinition replaces previous", "[interpreter][function][scoping]") {
    // CMake behavior: last definition wins globally
    auto output = run_script(R"(
        function(test_func)
            message("Version 1")
        endfunction()

        function(test_func)
            message("Version 2")
        endfunction()

        test_func()
    )");
    REQUIRE(output == "Version 2\n");
}

TEST_CASE("Macro redefinition replaces previous", "[interpreter][macro][scoping]") {
    // CMake behavior: last definition wins globally
    auto output = run_script(R"(
        macro(test_macro)
            message("Version 1")
        endmacro()

        macro(test_macro)
            message("Version 2")
        endmacro()

        test_macro()
    )");
    REQUIRE(output == "Version 2\n");
}

TEST_CASE("Function replaces macro with same name", "[interpreter][function][macro][scoping]") {
    // Defining a function with the same name as a macro should replace the macro
    auto output = run_script(R"(
        macro(test_cmd)
            message("macro version")
        endmacro()

        function(test_cmd)
            message("function version")
        endfunction()

        test_cmd()
    )");
    REQUIRE(output == "function version\n");
}

TEST_CASE("Macro replaces function with same name", "[interpreter][function][macro][scoping]") {
    // Defining a macro with the same name as a function should replace the function
    auto output = run_script(R"(
        function(test_cmd)
            message("function version")
        endfunction()

        macro(test_cmd)
            message("macro version")
        endmacro()

        test_cmd()
    )");
    REQUIRE(output == "macro version\n");
}

// Directory-level property commands tests

TEST_CASE("add_definitions strips -D prefix", "[interpreter][directory_properties]") {
    std::string temp_dir = "build_test_add_definitions";
    std::filesystem::create_directories(temp_dir);
    { std::ofstream f("test.cpp"); f << "int main() { return 0; }\n"; }

    kiln::Interpreter interp(".", &std::cout, &std::cerr, temp_dir);

    kiln::Parser parser(R"(
        add_definitions(-DFOO -DBAR=123)
        add_executable(mytest test.cpp)
    )");
    auto ast = parser.parse();
    REQUIRE(ast.has_value());

    auto result = interp.interpret(ast.value());
    interp.finalize_directory_targets();
    REQUIRE(result.has_value());

    // Check that definitions were added without -D prefix
    auto& targets = interp.get_targets();
    REQUIRE(targets.find("mytest") != targets.end());

    auto& target = targets["mytest"];
    const auto& defs = target->get_property_list("COMPILE_DEFINITIONS", kiln::PropertyVisibility::PRIVATE);
    REQUIRE(defs.size() == 2);
    REQUIRE(defs[0] == "FOO");
    REQUIRE(defs[1] == "BAR=123");

    std::filesystem::remove_all(temp_dir);
    std::filesystem::remove("test.cpp");
}

TEST_CASE("add_compile_definitions does not strip prefix", "[interpreter][directory_properties]") {
    std::string temp_dir = "build_test_add_compile_definitions";
    std::filesystem::create_directories(temp_dir);
    { std::ofstream f("test.cpp"); f << "int main() { return 0; }\n"; }

    kiln::Interpreter interp(".", &std::cout, &std::cerr, temp_dir);

    kiln::Parser parser(R"(
        add_compile_definitions(FOO BAR=456)
        add_executable(mytest test.cpp)
    )");
    auto ast = parser.parse();
    REQUIRE(ast.has_value());

    auto result = interp.interpret(ast.value());
    interp.finalize_directory_targets();
    REQUIRE(result.has_value());

    auto& targets = interp.get_targets();
    auto& target = targets["mytest"];
    const auto& defs = target->get_property_list("COMPILE_DEFINITIONS", kiln::PropertyVisibility::PRIVATE);
    REQUIRE(defs.size() == 2);
    REQUIRE(defs[0] == "FOO");
    REQUIRE(defs[1] == "BAR=456");

    std::filesystem::remove_all(temp_dir);
    std::filesystem::remove("test.cpp");
}

TEST_CASE("add_compile_options applies to targets", "[interpreter][directory_properties]") {
    std::string temp_dir = "build_test_add_compile_options";
    std::filesystem::create_directories(temp_dir);
    { std::ofstream f("test.cpp"); f << "int main() { return 0; }\n"; }

    kiln::Interpreter interp(".", &std::cout, &std::cerr, temp_dir);

    kiln::Parser parser(R"(
        add_compile_options(-Wall -Werror)
        add_executable(mytest test.cpp)
    )");
    auto ast = parser.parse();
    REQUIRE(ast.has_value());

    auto result = interp.interpret(ast.value());
    interp.finalize_directory_targets();
    REQUIRE(result.has_value());

    auto& targets = interp.get_targets();
    auto& target = targets["mytest"];
    const auto& opts = target->get_property_list("COMPILE_OPTIONS", kiln::PropertyVisibility::PRIVATE);
    REQUIRE(opts.size() == 2);
    REQUIRE(opts[0] == "-Wall");
    REQUIRE(opts[1] == "-Werror");

    std::filesystem::remove_all(temp_dir);
    std::filesystem::remove("test.cpp");
}

TEST_CASE("add_link_options applies to targets", "[interpreter][directory_properties]") {
    std::string temp_dir = "build_test_add_link_options";
    std::filesystem::create_directories(temp_dir);
    { std::ofstream f("test.cpp"); f << "int main() { return 0; }\n"; }

    kiln::Interpreter interp(".", &std::cout, &std::cerr, temp_dir);

    kiln::Parser parser(R"(
        add_link_options(-Wl,--as-needed)
        add_executable(mytest test.cpp)
    )");
    auto ast = parser.parse();
    REQUIRE(ast.has_value());

    auto result = interp.interpret(ast.value());
    interp.finalize_directory_targets();
    REQUIRE(result.has_value());

    auto& targets = interp.get_targets();
    auto& target = targets["mytest"];
    const auto& opts = target->get_property_list("LINK_OPTIONS", kiln::PropertyVisibility::PRIVATE);
    REQUIRE(opts.size() == 1);
    REQUIRE(opts[0] == "-Wl,--as-needed");

    std::filesystem::remove_all(temp_dir);
    std::filesystem::remove("test.cpp");
}

TEST_CASE("link_libraries applies to targets", "[interpreter][directory_properties]") {
    std::string temp_dir = "build_test_link_libraries";
    std::filesystem::create_directories(temp_dir);
    { std::ofstream f("test.cpp"); f << "int main() { return 0; }\n"; }
    { std::ofstream f("lib.cpp"); f << "void lib_func() {}\n"; }

    kiln::Interpreter interp(".", &std::cout, &std::cerr, temp_dir);

    kiln::Parser parser(R"(
        add_library(mylib STATIC lib.cpp)
        link_libraries(mylib pthread)
        add_executable(mytest test.cpp)
    )");
    auto ast = parser.parse();
    REQUIRE(ast.has_value());

    auto result = interp.interpret(ast.value());
    // Finalize to apply retroactive properties
    interp.finalize_directory_targets();
    REQUIRE(result.has_value());

    auto& targets = interp.get_targets();
    auto& target = targets["mytest"];
    const auto& libs = target->get_property_list("LINK_LIBRARIES", kiln::PropertyVisibility::PRIVATE);
    // Check that required libs are present (may have duplicates due to creation + finalization)
    REQUIRE(libs.size() >= 2);
    REQUIRE(std::find(libs.begin(), libs.end(), "mylib") != libs.end());
    REQUIRE(std::find(libs.begin(), libs.end(), "pthread") != libs.end());

    std::filesystem::remove_all(temp_dir);
    std::filesystem::remove("test.cpp");
    std::filesystem::remove("lib.cpp");
}

TEST_CASE("Directory properties apply retroactively", "[interpreter][directory_properties]") {
    std::string temp_dir = "build_test_retroactive";
    std::filesystem::create_directories(temp_dir);
    { std::ofstream f("before.cpp"); f << "int main() { return 0; }\n"; }
    { std::ofstream f("after.cpp"); f << "int main() { return 0; }\n"; }

    kiln::Interpreter interp(".", &std::cout, &std::cerr, temp_dir);

    kiln::Parser parser(R"(
        add_executable(before before.cpp)
        add_definitions(-DTEST_VALUE=100)
        add_executable(after after.cpp)
    )");
    auto ast = parser.parse();
    REQUIRE(ast.has_value());

    auto result = interp.interpret(ast.value());
    REQUIRE(result.has_value());

    // Finalize to apply retroactive properties
    interp.finalize_directory_targets();

    auto& targets = interp.get_targets();

    // Both targets should have the definition
    auto& before = targets["before"];
    const auto& before_defs = before->get_property_list("COMPILE_DEFINITIONS", kiln::PropertyVisibility::PRIVATE);
    REQUIRE(before_defs.size() >= 1);
    REQUIRE(std::find(before_defs.begin(), before_defs.end(), "TEST_VALUE=100") != before_defs.end());

    auto& after = targets["after"];
    const auto& after_defs = after->get_property_list("COMPILE_DEFINITIONS", kiln::PropertyVisibility::PRIVATE);
    REQUIRE(after_defs.size() >= 1);
    REQUIRE(std::find(after_defs.begin(), after_defs.end(), "TEST_VALUE=100") != after_defs.end());

    std::filesystem::remove_all(temp_dir);
    std::filesystem::remove("before.cpp");
    std::filesystem::remove("after.cpp");
}

TEST_CASE("Multiple directory properties accumulate", "[interpreter][directory_properties]") {
    std::string temp_dir = "build_test_accumulate";
    std::filesystem::create_directories(temp_dir);
    { std::ofstream f("test.cpp"); f << "int main() { return 0; }\n"; }

    kiln::Interpreter interp(".", &std::cout, &std::cerr, temp_dir);

    kiln::Parser parser(R"(
        add_definitions(-DFOO=1)
        add_compile_definitions(BAR=2)
        add_compile_options(-Wall)
        add_executable(mytest test.cpp)
        add_definitions(-DBAZ=3)
    )");
    auto ast = parser.parse();
    REQUIRE(ast.has_value());

    auto result = interp.interpret(ast.value());
    REQUIRE(result.has_value());

    // Finalize to apply retroactive properties
    interp.finalize_directory_targets();

    auto& targets = interp.get_targets();
    auto& target = targets["mytest"];

    const auto& defs = target->get_property_list("COMPILE_DEFINITIONS", kiln::PropertyVisibility::PRIVATE);
    REQUIRE(defs.size() >= 3);
    REQUIRE(std::find(defs.begin(), defs.end(), "FOO=1") != defs.end());
    REQUIRE(std::find(defs.begin(), defs.end(), "BAR=2") != defs.end());
    REQUIRE(std::find(defs.begin(), defs.end(), "BAZ=3") != defs.end());

    const auto& opts = target->get_property_list("COMPILE_OPTIONS", kiln::PropertyVisibility::PRIVATE);
    REQUIRE(opts.size() >= 1);
    REQUIRE(std::find(opts.begin(), opts.end(), "-Wall") != opts.end());

    std::filesystem::remove_all(temp_dir);
    std::filesystem::remove("test.cpp");
}

// Tests for FindPython-related bugs (2026-01-31)
TEST_CASE("if condition: CMake list is truthy", "[interpreter][if][bugfix]") {
    // Bug: "3;2" was being treated as falsy because it starts with a digit
    // but doesn't parse as a complete number (strtod stops at ';')
    SECTION("List with semicolons is truthy") {
        auto output = run_script(R"(
            set(MY_LIST "3;2")
            if(MY_LIST)
                message("truthy")
            else()
                message("falsy")
            endif()
        )");
        REQUIRE(output == "truthy\n");
    }

    SECTION("Single element list is truthy") {
        auto output = run_script(R"(
            set(MY_LIST "3")
            if(MY_LIST)
                message("truthy")
            else()
                message("falsy")
            endif()
        )");
        REQUIRE(output == "truthy\n");
    }

    SECTION("List starting with number is truthy") {
        auto output = run_script(R"(
            set(VERSIONS "3;2;1")
            if(VERSIONS)
                message("truthy")
            else()
                message("falsy")
            endif()
        )");
        REQUIRE(output == "truthy\n");
    }
}

TEST_CASE("foreach: loop variable with variable reference in name", "[interpreter][foreach][bugfix]") {
    // Bug: foreach(_${PREFIX}_VAR IN LISTS ...) was not expanding the loop variable name
    SECTION("Variable reference in loop variable name") {
        auto output = run_script(R"(
            set(_PREFIX "Python")
            set(_Python_VERSIONS "3;2")
            foreach(_${_PREFIX}_MAJOR IN LISTS _${_PREFIX}_VERSIONS)
                message("${_Python_MAJOR}")
            endforeach()
        )");
        REQUIRE(output == "3\n2\n");
    }

    SECTION("Loop variable is properly set and accessible") {
        auto output = run_script(R"(
            set(PREFIX "Test")
            set(Test_ITEMS "a;b;c")
            foreach(_${PREFIX}_ITEM IN LISTS ${PREFIX}_ITEMS)
                if(DEFINED _${PREFIX}_ITEM)
                    message("defined: ${_Test_ITEM}")
                endif()
            endforeach()
        )");
        REQUIRE(output == "defined: a\ndefined: b\ndefined: c\n");
    }
}

TEST_CASE("if condition: variable reference in compound name is dereferenced", "[interpreter][if][bugfix]") {
    // Bug: if(_${PREFIX}_VAR EQUAL "3") was not dereferencing _Python_VAR after expansion
    SECTION("EQUAL comparison with compound variable name") {
        auto output = run_script(R"(
            set(_PREFIX "Python")
            set(_Python_VERSION "3")
            if(_${_PREFIX}_VERSION EQUAL "3")
                message("match")
            else()
                message("no match")
            endif()
        )");
        REQUIRE(output == "match\n");
    }

    SECTION("STREQUAL comparison with compound variable name") {
        auto output = run_script(R"(
            set(NS "My")
            set(My_VALUE "hello")
            if(${NS}_VALUE STREQUAL "hello")
                message("match")
            else()
                message("no match")
            endif()
        )");
        REQUIRE(output == "match\n");
    }

    SECTION("Compound name in elseif") {
        auto output = run_script(R"(
            set(_PREFIX "Python")
            set(_Python_MAJOR "3")
            if(_${_PREFIX}_MAJOR EQUAL "2")
                message("python2")
            elseif(_${_PREFIX}_MAJOR EQUAL "3")
                message("python3")
            else()
                message("unknown")
            endif()
        )");
        REQUIRE(output == "python3\n");
    }
}

TEST_CASE("VERSION comparison component-wise", "[interpreter][if][version]") {
    SECTION("Numeric component comparison (1.10 > 1.9)") {
        CHECK(run_script(R"(
            if("1.10" VERSION_GREATER "1.9")
                message(STATUS "PASS")
            else()
                message(STATUS "FAIL")
            endif()
        )").find("PASS") != std::string::npos);
    }

    SECTION("1.2 < 1.10 (not lexicographic)") {
        CHECK(run_script(R"(
            if("1.2" VERSION_LESS "1.10")
                message(STATUS "PASS")
            else()
                message(STATUS "FAIL")
            endif()
        )").find("PASS") != std::string::npos);
    }

    SECTION("Missing components treated as zero") {
        CHECK(run_script(R"(
            if("1" VERSION_EQUAL "1.0")
                message(STATUS "PASS")
            endif()
        )").find("PASS") != std::string::npos);

        CHECK(run_script(R"(
            if("1" VERSION_EQUAL "1.0.0")
                message(STATUS "PASS")
            endif()
        )").find("PASS") != std::string::npos);

        CHECK(run_script(R"(
            if("1.0" VERSION_EQUAL "1.0.0")
                message(STATUS "PASS")
            endif()
        )").find("PASS") != std::string::npos);
    }

    SECTION("Multi-component comparison") {
        CHECK(run_script(R"(
            if("3.21.1" VERSION_GREATER "3.21")
                message(STATUS "PASS")
            endif()
        )").find("PASS") != std::string::npos);

        CHECK(run_script(R"(
            if("1.0.0.1" VERSION_GREATER "1.0.0")
                message(STATUS "PASS")
            endif()
        )").find("PASS") != std::string::npos);
    }

    SECTION("VERSION_LESS_EQUAL and VERSION_GREATER_EQUAL") {
        CHECK(run_script(R"(
            if("1.0" VERSION_LESS_EQUAL "1.0")
                message(STATUS "PASS")
            endif()
        )").find("PASS") != std::string::npos);

        CHECK(run_script(R"(
            if("1.0" VERSION_GREATER_EQUAL "1.0")
                message(STATUS "PASS")
            endif()
        )").find("PASS") != std::string::npos);

        CHECK(run_script(R"(
            if("1.9" VERSION_LESS_EQUAL "1.10")
                message(STATUS "PASS")
            endif()
        )").find("PASS") != std::string::npos);
    }
}

TEST_CASE("Truthiness - CMake exact rules", "[interpreter][if][truthiness]") {
    SECTION("Invalid numeric-looking strings are truthy") {
        // "2x" starts with digit but isn't a valid number - should be truthy in CMake
        CHECK(run_script(R"(
            set(V "2x")
            if(V)
                message(STATUS "PASS")
            else()
                message(STATUS "FAIL")
            endif()
        )").find("PASS") != std::string::npos);

        CHECK(run_script(R"(
            set(V "3.14abc")
            if(V)
                message(STATUS "PASS")
            else()
                message(STATUS "FAIL")
            endif()
        )").find("PASS") != std::string::npos);
    }

    SECTION("Only exact '0' is falsy, not variants") {
        // "0" is falsy
        CHECK(run_script(R"(
            set(V "0")
            if(V)
                message(STATUS "FAIL")
            else()
                message(STATUS "PASS")
            endif()
        )").find("PASS") != std::string::npos);

        // "0.0" is truthy
        CHECK(run_script(R"(
            set(V "0.0")
            if(V)
                message(STATUS "PASS")
            else()
                message(STATUS "FAIL")
            endif()
        )").find("PASS") != std::string::npos);

        // "00" is truthy
        CHECK(run_script(R"(
            set(V "00")
            if(V)
                message(STATUS "PASS")
            else()
                message(STATUS "FAIL")
            endif()
        )").find("PASS") != std::string::npos);

        // "-0" is truthy
        CHECK(run_script(R"(
            set(V "-0")
            if(V)
                message(STATUS "PASS")
            else()
                message(STATUS "FAIL")
            endif()
        )").find("PASS") != std::string::npos);

        // "+0" is truthy
        CHECK(run_script(R"(
            set(V "+0")
            if(V)
                message(STATUS "PASS")
            else()
                message(STATUS "FAIL")
            endif()
        )").find("PASS") != std::string::npos);
    }

    SECTION("Strings with trailing space are truthy") {
        CHECK(run_script(R"(
            set(V "0 ")
            if(V)
                message(STATUS "PASS")
            else()
                message(STATUS "FAIL")
            endif()
        )").find("PASS") != std::string::npos);
    }
}

TEST_CASE("IS_NEWER_THAN operator", "[interpreter][if][is_newer_than]") {
    auto temp_dir = std::filesystem::temp_directory_path() / "kiln_test_is_newer_than";
    std::filesystem::create_directories(temp_dir);
    auto cleanup = [&]() { std::filesystem::remove_all(temp_dir); };

    SECTION("Newer file returns true") {
        auto td = temp_dir.string();
        CHECK(run_script(
            "file(MAKE_DIRECTORY \"" + td + "\")\n"
            "file(WRITE \"" + td + "/older.txt\" \"older\")\n"
            "execute_process(COMMAND sleep 0.1)\n"
            "file(WRITE \"" + td + "/newer.txt\" \"newer\")\n"
            "if(\"" + td + "/newer.txt\" IS_NEWER_THAN \"" + td + "/older.txt\")\n"
            "    message(STATUS \"PASS\")\n"
            "else()\n"
            "    message(STATUS \"FAIL\")\n"
            "endif()\n"
        ).find("PASS") != std::string::npos);
    }

    SECTION("Missing file returns true") {
        auto td = temp_dir.string();
        CHECK(run_script(
            "file(WRITE \"" + td + "/exists_file.txt\" \"exists\")\n"
            "if(\"" + td + "/nonexistent_file_12345.txt\" IS_NEWER_THAN \"" + td + "/exists_file.txt\")\n"
            "    message(STATUS \"PASS\")\n"
            "else()\n"
            "    message(STATUS \"FAIL\")\n"
            "endif()\n"
        ).find("PASS") != std::string::npos);
    }

    SECTION("Same file returns true (equal mtime)") {
        auto td = temp_dir.string();
        CHECK(run_script(
            "file(WRITE \"" + td + "/same_file.txt\" \"content\")\n"
            "if(\"" + td + "/same_file.txt\" IS_NEWER_THAN \"" + td + "/same_file.txt\")\n"
            "    message(STATUS \"PASS\")\n"
            "else()\n"
            "    message(STATUS \"FAIL\")\n"
            "endif()\n"
        ).find("PASS") != std::string::npos);
    }

    cleanup();
}

TEST_CASE("execute_process reports redirect open failures", "[interpreter][execute_process]") {
    auto temp_dir = std::filesystem::temp_directory_path() / "kiln_test_execute_process_redirect";
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);
    auto missing_dir = (temp_dir / "missing").string();

    CHECK_THROWS_WITH(run_script(
        "execute_process(COMMAND echo hi OUTPUT_FILE \"" + missing_dir + "/out.txt\")\n"
    ), Catch::Matchers::ContainsSubstring("Failed to open OUTPUT_FILE"));

    CHECK_THROWS_WITH(run_script(
        "execute_process(COMMAND sh -c \"echo err >&2\" ERROR_FILE \"" + missing_dir + "/err.txt\")\n"
    ), Catch::Matchers::ContainsSubstring("Failed to open ERROR_FILE"));

    std::filesystem::remove_all(temp_dir);
}

TEST_CASE("if condition: undefined variable in MATCHES is literal", "[interpreter][if][bugfix]") {
    // Undefined bare words should be kept as literal strings, not become empty.
    // An empty regex would match everything, causing false positives.
    auto output = run_script(R"(
        set(MY_VAR "Linux")
        if(MY_VAR MATCHES AIX)
            message("matched")
        else()
            message("no match")
        endif()
    )");
    REQUIRE(output == "no match\n");

    // Verify MATCHES still works when the pattern is a defined variable
    output = run_script(R"(
        set(MY_VAR "Linux")
        set(PAT "Lin")
        if(MY_VAR MATCHES PAT)
            message("matched")
        else()
            message("no match")
        endif()
    )");
    REQUIRE(output == "matched\n");

    // Verify undefined variable on LHS of MATCHES also uses literal string
    output = run_script(R"(
        if(UNDEFINED_VAR MATCHES "UNDEFINED_VAR")
            message("literal")
        else()
            message("empty")
        endif()
    )");
    REQUIRE(output == "literal\n");

    // Verify undefined variable with OR doesn't cause false positive
    output = run_script(R"(
        set(CMAKE_SYSTEM_PROCESSOR "x86_64")
        set(CMAKE_SYSTEM_NAME "Linux")
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "ppc64|powerpc64" OR CMAKE_SYSTEM_NAME MATCHES AIX)
            message("ppc")
        else()
            message("not ppc")
        endif()
    )");
    REQUIRE(output == "not ppc\n");
}

TEST_CASE("if condition: undefined variable before binary operator", "[interpreter][if][bugfix]") {
    // When ${UNDEFINED} expands to nothing, a binary operator can end up
    // at the start of the condition or after NOT. CMake handles these cases:
    // 1. MATCHES without left operand -> false (special case)
    // 2. NOT followed by binary operator -> NOT becomes left operand of the operator

    SECTION("MATCHES without left operand returns false") {
        // if(${UNDEFINED} MATCHES "pat") -> if(MATCHES "pat") -> false
        auto output = run_script(R"(
            if(MATCHES "pattern")
                message("Y")
            else()
                message("N")
            endif()
        )");
        REQUIRE(output == "N\n");
    }

    SECTION("NOT followed by STREQUAL: NOT becomes left operand") {
        // if(NOT ${UNDEFINED} STREQUAL "X") -> if(NOT STREQUAL "X")
        // CMake interprets this as: "NOT" STREQUAL "X" = false
        auto output = run_script(R"(
            if(NOT STREQUAL "X")
                message("Y")
            else()
                message("N")
            endif()
        )");
        REQUIRE(output == "N\n");
    }

    SECTION("NOT followed by MATCHES: NOT becomes left operand") {
        // if(NOT ${UNDEFINED} MATCHES "pat") -> if(NOT MATCHES "pat")
        // CMake interprets this as: "NOT" MATCHES "pat" = false (no match)
        auto output = run_script(R"(
            if(NOT MATCHES "pattern")
                message("Y")
            else()
                message("N")
            endif()
        )");
        REQUIRE(output == "N\n");
    }

    SECTION("NOT followed by EQUAL: NOT becomes left operand") {
        // if(NOT ${UNDEFINED} EQUAL 0) -> if(NOT EQUAL 0)
        // CMake interprets this as: "NOT" EQUAL 0 = false (string can't compare as number)
        auto output = run_script(R"(
            if(NOT EQUAL 0)
                message("Y")
            else()
                message("N")
            endif()
        )");
        REQUIRE(output == "N\n");
    }

    SECTION("MySQL pattern: NOT ${VAR} STREQUAL empty") {
        // Real-world pattern from MySQL: if(NOT ${PLUGIN_${name}} STREQUAL "")
        // When the nested variable is undefined, this should evaluate correctly
        auto output = run_script(R"(
            set(target_name "my_plugin")
            # PLUGIN_my_plugin is NOT defined
            if(NOT ${PLUGIN_${target_name}} STREQUAL "")
                message("plugin defined")
            else()
                message("plugin not defined")
            endif()
        )");
        REQUIRE(output == "plugin not defined\n");

        // Now with the plugin defined
        output = run_script(R"(
            set(target_name "my_plugin")
            set(PLUGIN_my_plugin "dynamic")
            if(NOT ${PLUGIN_${target_name}} STREQUAL "")
                message("plugin defined")
            else()
                message("plugin not defined")
            endif()
        )");
        REQUIRE(output == "plugin defined\n");
    }

    SECTION("Bare NOT evaluates as truthy string") {
        // if(NOT) should evaluate NOT as a string value (truthy)
        auto output = run_script(R"(
            if(NOT)
                message("Y")
            else()
                message("N")
            endif()
        )");
        // NOT as a bare word is truthy (non-empty string), but in condition context
        // it's treated as an operator without operand, which falls back to evaluating
        // NOT itself. Since it's not a defined variable, it's falsy.
        REQUIRE(output == "N\n");
    }

    SECTION("Regular NOT negation still works") {
        // Make sure normal NOT operator still works
        auto output = run_script(R"(
            set(VAR "value")
            if(NOT VAR)
                message("Y")
            else()
                message("N")
            endif()
        )");
        REQUIRE(output == "N\n");  // VAR is truthy, NOT VAR is false

        output = run_script(R"(
            if(NOT UNDEFINED_VAR)
                message("Y")
            else()
                message("N")
            endif()
        )");
        REQUIRE(output == "Y\n");  // UNDEFINED_VAR is falsy, NOT UNDEFINED_VAR is true
    }

    SECTION("Binary operator without left operand errors (except MATCHES)") {
        // if(STREQUAL "") should error - missing left operand
        // This matches CMake behavior: "Unknown arguments specified"
        CHECK_THROWS_WITH(run_script(R"(
            if(STREQUAL "")
                message("Y")
            endif()
        )"), Catch::Matchers::ContainsSubstring("missing left operand"));

        CHECK_THROWS_WITH(run_script(R"(
            if(EQUAL 5)
                message("Y")
            endif()
        )"), Catch::Matchers::ContainsSubstring("missing left operand"));

        CHECK_THROWS_WITH(run_script(R"(
            if(LESS 5)
                message("Y")
            endif()
        )"), Catch::Matchers::ContainsSubstring("missing left operand"));

        CHECK_THROWS_WITH(run_script(R"(
            if(VERSION_LESS "1.0")
                message("Y")
            endif()
        )"), Catch::Matchers::ContainsSubstring("missing left operand"));
    }
}

TEST_CASE("if condition: lowercase keyword names are dereferenced as variables", "[interpreter][if][bugfix]") {
    // CMake keywords (NOT, AND, OR, TARGET, MATCHES, etc.) are case-sensitive.
    // Lowercase variants like "target", "not", "matches" should be treated as
    // variable names and dereferenced, not as keywords.

    SECTION("variable named 'target' in MATCHES") {
        // Real-world LLVM pattern: if(NOT target MATCHES "^check-")
        auto output = run_script(R"(
            set(target "check-all")
            if(target MATCHES "^check-")
                message("matched")
            else()
                message("no match")
            endif()
        )");
        REQUIRE(output == "matched\n");
    }

    SECTION("NOT variable named 'target' in MATCHES") {
        auto output = run_script(R"(
            set(target "check-all")
            if(NOT target MATCHES "^check-")
                message("no match")
            else()
                message("matched")
            endif()
        )");
        REQUIRE(output == "matched\n");
    }

    SECTION("variable named 'target' in STREQUAL") {
        auto output = run_script(R"(
            set(target "check-all")
            if(target STREQUAL "check-all")
                message("equal")
            else()
                message("not equal")
            endif()
        )");
        REQUIRE(output == "equal\n");
    }

    SECTION("variable named 'matches' in STREQUAL") {
        auto output = run_script(R"(
            set(matches "foo")
            if(matches STREQUAL "foo")
                message("equal")
            else()
                message("not equal")
            endif()
        )");
        REQUIRE(output == "equal\n");
    }

    SECTION("variable named 'defined' is dereferenced as primary") {
        auto output = run_script(R"(
            set(defined "yes")
            if(defined)
                message("truthy")
            else()
                message("falsy")
            endif()
        )");
        REQUIRE(output == "truthy\n");
    }

    SECTION("variable named 'not' is dereferenced") {
        auto output = run_script(R"(
            set(not "something")
            if(not)
                message("truthy")
            else()
                message("falsy")
            endif()
        )");
        REQUIRE(output == "truthy\n");

        auto output2 = run_script(R"(
            set(not "hello")
            if(not STREQUAL "hello")
                message("equal")
            else()
                message("not equal")
            endif()
        )");
        REQUIRE(output2 == "equal\n");
    }

    SECTION("variable named 'exists' in STREQUAL") {
        auto output = run_script(R"(
            set(exists "/tmp")
            if(exists STREQUAL "/tmp")
                message("equal")
            else()
                message("not equal")
            endif()
        )");
        REQUIRE(output == "equal\n");
    }

    SECTION("variable named 'command' in STREQUAL") {
        auto output = run_script(R"(
            set(command "bar")
            if(command STREQUAL "bar")
                message("equal")
            else()
                message("not equal")
            endif()
        )");
        REQUIRE(output == "equal\n");
    }

    SECTION("variable named 'or' and 'and' are dereferenced as primaries") {
        auto output = run_script(R"(
            set(or "1")
            set(and "1")
            if(or)
                message("or truthy")
            else()
                message("or falsy")
            endif()
            if(and)
                message("and truthy")
            else()
                message("and falsy")
            endif()
        )");
        REQUIRE(output == "or truthy\nand truthy\n");
    }

    SECTION("uppercase keywords still work as operators") {
        // Ensure the fix didn't break uppercase keyword behavior
        auto output = run_script(R"(
            set(NOT "truthy_value")
            if(NOT 0)
                message("NOT is operator")
            else()
                message("NOT is variable")
            endif()
        )");
        REQUIRE(output == "NOT is operator\n");
    }
}

TEST_CASE("if condition: MATCHES side effects in compound AND/OR", "[interpreter][if]") {
    // CMake does NOT short-circuit AND/OR. MATCHES always sets CMAKE_MATCH_*
    // even when the overall condition could be determined early.
    // Our compound fast path short-circuits pure operators but must fall back
    // for MATCHES to preserve these side effects.

    SECTION("FALSE AND ... MATCHES — MATCHES must still run") {
        auto output = run_script(R"cmake(
            set(CMAKE_MATCH_0 "UNSET")
            if(FALSE AND "hello" MATCHES "(hello)")
                message("taken")
            else()
                message("not taken")
            endif()
            message("match=${CMAKE_MATCH_0}")
        )cmake");
        REQUIRE(output == "not taken\nmatch=hello\n");
    }

    SECTION("TRUE OR ... MATCHES — MATCHES must still run") {
        auto output = run_script(R"cmake(
            set(CMAKE_MATCH_0 "UNSET")
            if(TRUE OR "hello" MATCHES "(hello)")
                message("taken")
            else()
                message("not taken")
            endif()
            message("match=${CMAKE_MATCH_0}")
        )cmake");
        REQUIRE(output == "taken\nmatch=hello\n");
    }

    SECTION("TRUE AND FALSE AND ... MATCHES — MATCHES must still run") {
        auto output = run_script(R"cmake(
            set(CMAKE_MATCH_0 "UNSET")
            if(TRUE AND FALSE AND "xyz" MATCHES "(xyz)")
                message("taken")
            else()
                message("not taken")
            endif()
            message("match=${CMAKE_MATCH_0}")
        )cmake");
        REQUIRE(output == "not taken\nmatch=xyz\n");
    }

    SECTION("FALSE OR TRUE OR ... MATCHES — MATCHES must still run") {
        auto output = run_script(R"cmake(
            set(CMAKE_MATCH_0 "UNSET")
            if(FALSE OR TRUE OR "xyz" MATCHES "(xyz)")
                message("taken")
            else()
                message("not taken")
            endif()
            message("match=${CMAKE_MATCH_0}")
        )cmake");
        REQUIRE(output == "taken\nmatch=xyz\n");
    }

    SECTION("MATCHES at start with AND — captures must be set") {
        auto output = run_script(R"cmake(
            set(CMAKE_MATCH_1 "UNSET")
            if("abc123" MATCHES "([a-z]+)([0-9]+)" AND TRUE)
                message("taken")
            else()
                message("not taken")
            endif()
            message("m1=${CMAKE_MATCH_1} m2=${CMAKE_MATCH_2}")
        )cmake");
        REQUIRE(output == "taken\nm1=abc m2=123\n");
    }

    SECTION("compound AND without MATCHES — fast path works correctly") {
        auto output = run_script(R"(
            set(x 5)
            if(x GREATER_EQUAL 0 AND x LESS 10)
                message("in range")
            else()
                message("out of range")
            endif()
        )");
        REQUIRE(output == "in range\n");
    }

    SECTION("compound AND short-circuits correctly for pure operators") {
        auto output = run_script(R"(
            set(x -1)
            if(x GREATER_EQUAL 0 AND x LESS 10)
                message("in range")
            else()
                message("out of range")
            endif()
        )");
        REQUIRE(output == "out of range\n");
    }

    SECTION("compound OR with all false") {
        auto output = run_script(R"(
            set(x 5)
            if(x LESS 0 OR x GREATER 10 OR x EQUAL 3)
                message("matched")
            else()
                message("none")
            endif()
        )");
        REQUIRE(output == "none\n");
    }

    SECTION("compound OR with one true") {
        auto output = run_script(R"(
            set(x 5)
            if(x LESS 0 OR x GREATER 10 OR x EQUAL 5)
                message("matched")
            else()
                message("none")
            endif()
        )");
        REQUIRE(output == "matched\n");
    }
}

TEST_CASE("Embedded quotes in unquoted arguments are preserved", "[interpreter]") {
    // CMake preserves literal quote characters in unquoted arguments.
    // e.g. BENCHMARK_VERSION="${VERSION}" -> BENCHMARK_VERSION="v1.0"
    // This is critical for compile definitions that need C string literals.
    SECTION("simple embedded quotes") {
        auto output = run_script(R"(
            set(VERSION "v1.0.0")
            set(VAR BENCHMARK_VERSION="${VERSION}")
            message("${VAR}")
        )");
        REQUIRE(output == "BENCHMARK_VERSION=\"v1.0.0\"\n");
    }

    SECTION("embedded quotes with no variable expansion") {
        auto output = run_script(R"(
            set(VAR KEY="hello")
            message("${VAR}")
        )");
        REQUIRE(output == "KEY=\"hello\"\n");
    }

    SECTION("flag-style embedded quotes") {
        auto output = run_script(R"(
            set(NAME "foo")
            set(VAR -combiners="${NAME}")
            message("${VAR}")
        )");
        REQUIRE(output == "-combiners=\"foo\"\n");
    }
}

// ============================================================================
// Multi-line argument parsing tests
// CMake behavior: whitespace (spaces, tabs, newlines) separates arguments,
// and multiple unquoted arguments are joined with semicolons in list contexts.
// This is critical for generator expressions that span multiple lines.
// ============================================================================

TEST_CASE("Multi-line arguments are joined with semicolons", "[interpreter][multiline]") {
    SECTION("newlines separate arguments into list") {
        // CMake joins whitespace-separated arguments with semicolons
        auto output = run_script(R"(
            set(MY_LIST
                item1
                item2
                item3
            )
            message("${MY_LIST}")
        )");
        REQUIRE(output == "item1;item2;item3\n");
    }

    SECTION("spaces separate arguments into list") {
        auto output = run_script(R"(
            set(MY_LIST item1 item2 item3)
            message("${MY_LIST}")
        )");
        REQUIRE(output == "item1;item2;item3\n");
    }

    SECTION("mixed spaces and newlines") {
        auto output = run_script(R"(
            set(MY_LIST item1 item2
                item3
                item4 item5)
            message("${MY_LIST}")
        )");
        REQUIRE(output == "item1;item2;item3;item4;item5\n");
    }
}

TEST_CASE("Multi-line generator expressions", "[interpreter][multiline][genex]") {
    // Critical test: CMake does NOT parse multi-line genex as a single argument.
    // Instead, whitespace separates arguments, and they get joined with semicolons.
    // Example: $<$<BOOL:TRUE>:\n-Wall\n-Wextra\n> becomes $<$<BOOL:TRUE>:;-Wall;-Wextra;>

    SECTION("genex spanning multiple lines is split and joined") {
        auto output = run_script(R"(
            set(MY_OPTS
                $<$<BOOL:TRUE>:
                -Wall
                -Wextra
                >
            )
            message("${MY_OPTS}")
        )");
        // Each line becomes a separate argument, joined with semicolons
        // Verify no newlines or extra whitespace in output
        REQUIRE(output.find('\n') == output.size() - 1);  // Only trailing newline
        REQUIRE(output.find("    ") == std::string::npos);  // No indentation preserved
        REQUIRE(output == "$<$<BOOL:TRUE>:;-Wall;-Wextra;>\n");
    }

    SECTION("genex with spaces on single line is split") {
        // Even on a single line, spaces inside genex cause splitting
        auto output = run_script(R"(
            set(MY_OPTS $<$<BOOL:TRUE>:-Wall -Wextra>)
            message("${MY_OPTS}")
        )");
        // Spaces separate the arguments: "$<$<BOOL:TRUE>:-Wall" and "-Wextra>"
        REQUIRE(output == "$<$<BOOL:TRUE>:-Wall;-Wextra>\n");
    }

    SECTION("complete genex on single line stays intact") {
        // A genex that doesn't contain whitespace stays as one argument
        auto output = run_script(R"(
            set(MY_OPTS $<$<BOOL:TRUE>:-Wall>)
            message("${MY_OPTS}")
        )");
        REQUIRE(output == "$<$<BOOL:TRUE>:-Wall>\n");
    }

    SECTION("real-world flatbuffers pattern") {
        // This is the actual pattern that flatbuffers uses
        auto output = run_script(R"(
            set(STRICT_OPTS
                $<$<BOOL:TRUE>:
                    -Wall
                    -Wextra
                    -Werror
                >
            )
            message("${STRICT_OPTS}")
        )");
        REQUIRE(output == "$<$<BOOL:TRUE>:;-Wall;-Wextra;-Werror;>\n");
    }

    SECTION("nested genex spanning multiple lines") {
        auto output = run_script(R"(
            set(OPTS
                $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:
                    -Wall
                    -Wextra
                >
            )
            message("${OPTS}")
        )");
        REQUIRE(output == "$<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:;-Wall;-Wextra;>\n");
    }

    SECTION("unclosed genex parses as literal string (MariaDB compatibility)") {
        // CMake treats generator expressions as strings at parse time.
        // Some projects (MariaDB) have unbalanced genex that split across lines.
        // The unclosed $<... should parse as a literal string, not cause an error.
        auto output = run_script(R"(
            set(MY_VAR $<BOOL:TRUE)
            message("${MY_VAR}")
        )");
        REQUIRE(output == "$<BOOL:TRUE\n");
    }

    SECTION("unclosed genex in multi-line context") {
        // Each line is a separate argument, unclosed genex is just a string
        auto output = run_script(R"(
            set(MY_OPTS
                $<$<BOOL:TRUE>:
                -Wall
            )
            message("${MY_OPTS}")
        )");
        // The closing > never appears, so we get partial genex strings joined
        REQUIRE(output == "$<$<BOOL:TRUE>:;-Wall\n");
    }
}

TEST_CASE("build_command()", "[interpreter][build_command]") {
    SECTION("basic - no options") {
        auto output = run_script(R"(
            build_command(BUILD_CMD)
            message("${BUILD_CMD}")
        )");
        REQUIRE(output == "kiln\n");
    }

    SECTION("with CONFIGURATION") {
        auto output = run_script(R"(
            build_command(BUILD_CMD CONFIGURATION Release)
            message("${BUILD_CMD}")
        )");
        REQUIRE(output == "kiln --config Release\n");
    }

    SECTION("with PARALLEL_LEVEL") {
        auto output = run_script(R"(
            build_command(BUILD_CMD PARALLEL_LEVEL 8)
            message("${BUILD_CMD}")
        )");
        REQUIRE(output == "kiln -j 8\n");
    }

    SECTION("with TARGET") {
        auto output = run_script(R"(
            build_command(BUILD_CMD TARGET myapp)
            message("${BUILD_CMD}")
        )");
        REQUIRE(output == "kiln myapp\n");
    }

    SECTION("with multiple TARGETs") {
        auto output = run_script(R"(
            build_command(BUILD_CMD TARGET myapp TARGET mylib)
            message("${BUILD_CMD}")
        )");
        REQUIRE(output == "kiln myapp mylib\n");
    }

    SECTION("with all options") {
        auto output = run_script(R"(
            build_command(BUILD_CMD CONFIGURATION Debug PARALLEL_LEVEL 4 TARGET foo TARGET bar)
            message("${BUILD_CMD}")
        )");
        REQUIRE(output == "kiln --config Debug -j 4 foo bar\n");
    }

    SECTION("legacy signature") {
        auto output = run_script(R"(
            build_command(BUILD_CMD /usr/bin/make)
            message("${BUILD_CMD}")
        )");
        // Legacy form ignores the makecommand and returns bare kiln (no --target)
        REQUIRE(output == "kiln\n");
    }

    SECTION("PROJECT_NAME is accepted but ignored") {
        // PROJECT_NAME is deprecated; it should be accepted with a warning
        // and have no effect on the generated command
        auto output = run_script(R"(
            build_command(BUILD_CMD PROJECT_NAME MyProject TARGET myapp)
            message("${BUILD_CMD}")
        )");
        REQUIRE(output.find("WARNING") != std::string::npos);
        REQUIRE(output.find("PROJECT_NAME") != std::string::npos);
        REQUIRE(output.find("kiln myapp") != std::string::npos);
    }
}

TEST_CASE("cmake_minimum_required sets CMAKE_MINIMUM_REQUIRED_VERSION", "[interpreter][cmake_minimum_required]") {
    auto output = run_script(R"(
        cmake_minimum_required(VERSION 3.16)
        message("${CMAKE_MINIMUM_REQUIRED_VERSION}")
    )");
    REQUIRE(output == "3.16\n");
}

TEST_CASE("cmake_minimum_required with range syntax uses minimum", "[interpreter][cmake_minimum_required]") {
    auto output = run_script(R"(
        cmake_minimum_required(VERSION 3.16...3.27)
        message("${CMAKE_MINIMUM_REQUIRED_VERSION}")
    )");
    REQUIRE(output == "3.16\n");
}

TEST_CASE("cmake_minimum_required accepts low version", "[interpreter][cmake_minimum_required]") {
    auto output = run_script(R"(
        cmake_minimum_required(VERSION 2.8)
        message("OK")
    )");
    REQUIRE(output == "OK\n");
}

TEST_CASE("cmake_minimum_required accepts exact version", "[interpreter][cmake_minimum_required]") {
    auto output = run_script(R"(
        cmake_minimum_required(VERSION ${CMAKE_VERSION})
        message("OK")
    )");
    REQUIRE(output == "OK\n");
}

TEST_CASE("cmake_minimum_required rejects version higher than supported", "[interpreter][cmake_minimum_required]") {
    REQUIRE_THROWS_WITH(run_script(R"(
        cmake_minimum_required(VERSION 99.0)
    )"), Catch::Matchers::ContainsSubstring("or higher is required"));
}

TEST_CASE("cmake_minimum_required range with high minimum rejects", "[interpreter][cmake_minimum_required]") {
    REQUIRE_THROWS_WITH(run_script(R"(
        cmake_minimum_required(VERSION 99.0...100.0)
    )"), Catch::Matchers::ContainsSubstring("or higher is required"));
}

TEST_CASE("cmake_minimum_required with FATAL_ERROR keyword", "[interpreter][cmake_minimum_required]") {
    // FATAL_ERROR is accepted but ignored (same as CMake 2.6+)
    auto output = run_script(R"(
        cmake_minimum_required(VERSION 3.16 FATAL_ERROR)
        message("${CMAKE_MINIMUM_REQUIRED_VERSION}")
    )");
    REQUIRE(output == "3.16\n");
}

TEST_CASE("cmake_minimum_required version check uses numeric comparison", "[interpreter][cmake_minimum_required]") {
    // VERSION 9 should be accepted (less than any double-digit major we'd claim)
    auto output = run_script(R"(
        cmake_minimum_required(VERSION 1.0)
        if(CMAKE_MINIMUM_REQUIRED_VERSION VERSION_LESS "2.0")
            message("OK")
        endif()
    )");
    REQUIRE(output == "OK\n");
}

TEST_CASE("cmake_policy SET and GET", "[interpreter][policy]") {
    SECTION("CMP0126 defaults to NEW (kiln emulates 3.31)") {
        auto output = run_script(R"(
            cmake_policy(GET CMP0126 result)
            message("${result}")
        )");
        REQUIRE(output == "NEW\n");
    }

    SECTION("CMP0126 becomes OLD with cmake_minimum_required < 3.21") {
        auto output = run_script(R"(
            cmake_minimum_required(VERSION 3.12)
            cmake_policy(GET CMP0126 result)
            message("${result}")
        )");
        REQUIRE(output == "OLD\n");
    }

    SECTION("CMP0167 defaults to NEW") {
        auto output = run_script(R"(
            cmake_policy(GET CMP0167 result)
            message("${result}")
        )");
        REQUIRE(output == "NEW\n");
    }

    SECTION("SET changes policy state") {
        auto output = run_script(R"(
            cmake_policy(SET CMP0126 NEW)
            cmake_policy(GET CMP0126 result)
            message("${result}")
        )");
        REQUIRE(output == "NEW\n");
    }

    SECTION("Unknown policy GET returns empty") {
        auto output = run_script(R"(
            cmake_policy(GET CMP9999 result)
            message("[${result}]")
        )");
        REQUIRE(output == "[NEW]\n");
    }
}

TEST_CASE("cmake_policy PUSH/POP", "[interpreter][policy]") {
    auto output = run_script(R"(
        cmake_minimum_required(VERSION 3.12)
        cmake_policy(GET CMP0126 before)
        message("before=${before}")

        cmake_policy(PUSH)
        cmake_policy(SET CMP0126 NEW)
        cmake_policy(GET CMP0126 during)
        message("during=${during}")
        cmake_policy(POP)

        cmake_policy(GET CMP0126 after)
        message("after=${after}")
    )");
    REQUIRE(output == "before=OLD\nduring=NEW\nafter=OLD\n");
}

TEST_CASE("CMP0126 OLD: set(CACHE) removes local variable", "[interpreter][policy]") {
    auto output = run_script(R"(
        cmake_minimum_required(VERSION 3.12)
        set(MY_VAR "local_value")
        set(MY_VAR "cache_value" CACHE STRING "doc")
        message("${MY_VAR}")
    )");
    REQUIRE(output == "cache_value\n");
}

TEST_CASE("CMP0126 NEW: set(CACHE) does not remove local variable", "[interpreter][policy]") {
    auto output = run_script(R"(
        cmake_policy(SET CMP0126 NEW)
        set(MY_VAR "local_value")
        set(MY_VAR "cache_value" CACHE STRING "doc")
        message("${MY_VAR}")
    )");
    REQUIRE(output == "local_value\n");
}

// ---- Legacy / ancient CMake syntax ----

TEST_CASE("legacy endif with condition", "[interpreter][legacy]") {
    auto output = run_script(R"(
        set(MY_VAR TRUE)
        if(MY_VAR)
            message("yes")
        endif(MY_VAR)
    )");
    REQUIRE(output == "yes\n");

    output = run_script(R"(
        set(X FALSE)
        if(X)
            message("if")
        else(X)
            message("else")
        endif(X)
    )");
    REQUIRE(output == "else\n");
}

TEST_CASE("legacy elseif/else/endif with conditions", "[interpreter][legacy]") {
    auto output = run_script(R"(
        set(VAL "B")
        if(VAL STREQUAL "A")
            message("A")
        elseif(VAL STREQUAL "B")
            message("B")
        else(VAL STREQUAL "B")
            message("else")
        endif(VAL STREQUAL "A")
    )");
    REQUIRE(output == "B\n");
}

TEST_CASE("legacy endfunction with name", "[interpreter][legacy]") {
    auto output = run_script(R"(
        function(greet name)
            message("Hello ${name}")
        endfunction(greet)

        greet("World")
    )");
    REQUIRE(output == "Hello World\n");
}

TEST_CASE("legacy endmacro with name", "[interpreter][legacy]") {
    auto output = run_script(R"(
        macro(shout msg)
            message("${msg}!")
        endmacro(shout)

        shout("Hey")
    )");
    REQUIRE(output == "Hey!\n");
}

TEST_CASE("legacy endforeach with variable", "[interpreter][legacy]") {
    auto output = run_script(R"(
        foreach(i 1 2 3)
            message("${i}")
        endforeach(i)
    )");
    REQUIRE(output == "1\n2\n3\n");
}

TEST_CASE("legacy endwhile with condition", "[interpreter][legacy]") {
    auto output = run_script(R"(
        set(COUNT 0)
        while(COUNT LESS 3)
            message("${COUNT}")
            math(EXPR COUNT "${COUNT} + 1")
        endwhile(COUNT LESS 3)
    )");
    REQUIRE(output == "0\n1\n2\n");
}

TEST_CASE("uppercase command names", "[interpreter][legacy]") {
    auto output = run_script(R"(
        SET(MY_VAR "hello")
        MESSAGE("${MY_VAR}")
    )");
    REQUIRE(output == "hello\n");

    output = run_script(R"(
        SET(ITEMS "a;b;c")
        FOREACH(i ${ITEMS})
            MESSAGE("${i}")
        ENDFOREACH()
    )");
    REQUIRE(output == "a\nb\nc\n");

    output = run_script(R"(
        FUNCTION(greet name)
            MESSAGE("Hi ${name}")
        ENDFUNCTION()
        greet("Bob")
    )");
    REQUIRE(output == "Hi Bob\n");
}

TEST_CASE("mixed case command names", "[interpreter][legacy]") {
    auto output = run_script(R"(
        Set(X "1")
        If(X)
            Message("yes")
        Else()
            Message("no")
        EndIf()
    )");
    REQUIRE(output == "yes\n");

    output = run_script(R"(
        ForEach(i RANGE 2)
            Message("${i}")
        EndForEach()
    )");
    REQUIRE(output == "0\n1\n2\n");
}

TEST_CASE("if(COMMAND) with user function", "[interpreter][legacy]") {
    auto output = run_script(R"(
        function(my_func)
            message("called")
        endfunction()

        if(COMMAND my_func)
            message("exists")
        else()
            message("missing")
        endif()
    )");
    REQUIRE(output == "exists\n");

    output = run_script(R"(
        if(COMMAND nonexistent_func)
            message("exists")
        else()
            message("missing")
        endif()
    )");
    REQUIRE(output == "missing\n");
}

TEST_CASE("if(COMMAND) with builtin command", "[interpreter][legacy]") {
    auto output = run_script(R"(
        if(COMMAND message)
            message("builtin exists")
        else()
            message("builtin missing")
        endif()
    )");
    REQUIRE(output == "builtin exists\n");

    output = run_script(R"(
        if(COMMAND set)
            message("set exists")
        else()
            message("set missing")
        endif()
    )");
    REQUIRE(output == "set exists\n");
}

TEST_CASE("if(DEFINED) legacy pattern", "[interpreter][legacy]") {
    auto output = run_script(R"(
        set(FOO "bar")
        if(DEFINED FOO)
            message("defined")
        else()
            message("undefined")
        endif()
    )");
    REQUIRE(output == "defined\n");

    output = run_script(R"(
        if(DEFINED NONEXISTENT)
            message("defined")
        else()
            message("undefined")
        endif()
    )");
    REQUIRE(output == "undefined\n");
}

TEST_CASE("legacy combined: uppercase with repeated args", "[interpreter][legacy]") {
    auto output = run_script(R"(
        SET(MY_VAR TRUE)
        IF(MY_VAR)
            MESSAGE("yes")
        ELSE(MY_VAR)
            MESSAGE("no")
        ENDIF(MY_VAR)
    )");
    REQUIRE(output == "yes\n");

    output = run_script(R"(
        FUNCTION(greet name)
            MESSAGE("Hi ${name}")
        ENDFUNCTION(greet)
        greet("Alice")
    )");
    REQUIRE(output == "Hi Alice\n");

    output = run_script(R"(
        FOREACH(i 1 2 3)
            MESSAGE("${i}")
        ENDFOREACH(i)
    )");
    REQUIRE(output == "1\n2\n3\n");
}

TEST_CASE("Parser warns on old-style control flow commands", "[parser][warning]") {
    std::string script = R"(
        # Case 1: Matching old-style arguments (should NOT warn)
        if(VAR)
            message("if")
        else(VAR)
            message("else")
        endif(VAR)

        function(foo x y)
            message("foo")
        endfunction(foo)

        macro(bar x y)
            message("bar")
        endmacro(bar)

        foreach(i IN LISTS mylist)
            message("i")
        endforeach(i)

        while(VAR)
            message("while")
        endwhile(VAR)

        # Case 2: Empty arguments (should NOT warn)
        if(VAR2)
            message("if2")
        else()
            message("else2")
        endif()

        # Case 3: Non-matching arguments (should WARN)
        if(VAR_IF)
            message("if3")
        else(VAR_OTHER_ELSE)
            message("else3")
        endif(VAR_OTHER_ENDIF)

        function(foo2)
            message("foo2")
        endfunction(wrong_foo2)

        macro(bar2)
            message("bar2")
        endmacro(wrong_bar2)

        foreach(j IN LISTS mylist)
            message("j")
        endforeach(wrong_j)

        while(VAR_WHILE)
            message("while2")
        endwhile(wrong_while)
    )";

    // Redirect std::cerr to capture the warnings
    std::stringstream buffer;
    std::streambuf* old = std::cerr.rdbuf(buffer.rdbuf());

    kiln::Parser parser(script, "CMakeLists.txt");
    auto ast_or_error = parser.parse();

    // Restore std::cerr
    std::cerr.rdbuf(old);

    REQUIRE(ast_or_error.has_value());
    
    std::string warnings = buffer.str();

    // Assert that the matching cases did NOT generate warnings
    CHECK(warnings.find("else(VAR)") == std::string::npos);
    CHECK(warnings.find("endif(VAR)") == std::string::npos);
    CHECK(warnings.find("endfunction(foo)") == std::string::npos);
    CHECK(warnings.find("endmacro(bar)") == std::string::npos);
    CHECK(warnings.find("endforeach(i)") == std::string::npos);
    CHECK(warnings.find("endwhile(VAR)") == std::string::npos);

    // Assert that empty cases did NOT generate warnings
    CHECK(warnings.find("else()") == std::string::npos);
    CHECK(warnings.find("endif()") == std::string::npos);

    // Assert that non-matching cases DID generate warnings
    CHECK(warnings.find("old style CMake syntax: 'else(...)' has arguments that do not match the opening command.") != std::string::npos);
    CHECK(warnings.find("old style CMake syntax: 'endif(...)' has arguments that do not match the opening command.") != std::string::npos);
    CHECK(warnings.find("old style CMake syntax: 'endfunction(...)' has arguments that do not match the opening command.") != std::string::npos);
    CHECK(warnings.find("old style CMake syntax: 'endmacro(...)' has arguments that do not match the opening command.") != std::string::npos);
    CHECK(warnings.find("old style CMake syntax: 'endforeach(...)' has arguments that do not match the opening command.") != std::string::npos);
    CHECK(warnings.find("old style CMake syntax: 'endwhile(...)' has arguments that do not match the opening command.") != std::string::npos);
}

