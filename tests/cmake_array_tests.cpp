#include <catch2/catch_test_macros.hpp>
#include "kiln/CMakeArray.hpp"
#include <vector>
#include <string>

using namespace kiln;

// --- CMakeArray tests ---

TEST_CASE("CMakeArray: empty string", "[cmake_array]") {
    CMakeArray arr(std::string(""));
    CHECK(arr.size() == 0);
    CHECK(arr.empty());
    CHECK(arr.to_string() == "");
    CHECK(arr.to_vector().empty());
}

TEST_CASE("CMakeArray: semicolon-separated", "[cmake_array]") {
    CMakeArray arr(std::string("a;b;c"));
    REQUIRE(arr.size() == 3);
    CHECK(arr[0] == "a");
    CHECK(arr[1] == "b");
    CHECK(arr[2] == "c");
}

TEST_CASE("CMakeArray: construct from vector", "[cmake_array]") {
    std::vector<std::string> v = {"x", "y", "z"};
    CMakeArray arr(v);
    REQUIRE(arr.size() == 3);
    CHECK(arr[0] == "x");
    CHECK(arr[1] == "y");
    CHECK(arr[2] == "z");
}

TEST_CASE("CMakeArray: to_string round-trips", "[cmake_array]") {
    CMakeArray arr(std::string("foo;bar;baz"));
    CHECK(arr.to_string() == "foo;bar;baz");

    CMakeArray arr2(arr.to_string());
    REQUIRE(arr2.size() == arr.size());
    for (size_t i = 0; i < arr.size(); ++i) { CHECK(arr2[i] == arr[i]); }
}

TEST_CASE("CMakeArray: to_vector", "[cmake_array]") {
    CMakeArray arr(std::string("1;2;3"));
    auto v = arr.to_vector();
    REQUIRE(v.size() == 3);
    CHECK(v == std::vector<std::string>{"1", "2", "3"});
}

TEST_CASE("CMakeArray: append", "[cmake_array]") {
    CMakeArray arr(std::string("a;b"));
    arr.append("c");
    REQUIRE(arr.size() == 3);
    CHECK(arr[2] == "c");

    arr.append("d;e");
    REQUIRE(arr.size() == 5);
    CHECK(arr[3] == "d");
    CHECK(arr[4] == "e");
}

TEST_CASE("CMakeArray: insert", "[cmake_array]") {
    CMakeArray arr(std::string("a;c"));
    arr.insert(1, {"b"});
    REQUIRE(arr.size() == 3);
    CHECK(arr[0] == "a");
    CHECK(arr[1] == "b");
    CHECK(arr[2] == "c");
}

TEST_CASE("CMakeArray: erase", "[cmake_array]") {
    CMakeArray arr(std::string("a;b;c"));
    arr.erase(1);
    REQUIRE(arr.size() == 2);
    CHECK(arr[0] == "a");
    CHECK(arr[1] == "c");
}

TEST_CASE("CMakeArray: reverse", "[cmake_array]") {
    CMakeArray arr(std::string("a;b;c"));
    arr.reverse();
    CHECK(arr[0] == "c");
    CHECK(arr[1] == "b");
    CHECK(arr[2] == "a");
}

TEST_CASE("CMakeArray: sort", "[cmake_array]") {
    CMakeArray arr(std::string("c;a;b"));
    arr.sort();
    CHECK(arr[0] == "a");
    CHECK(arr[1] == "b");
    CHECK(arr[2] == "c");
}

TEST_CASE("CMakeArray: remove_duplicates", "[cmake_array]") {
    CMakeArray arr(std::string("a;b;a;c;b"));
    arr.remove_duplicates();
    REQUIRE(arr.size() == 3);
    CHECK(arr[0] == "a");
    CHECK(arr[1] == "b");
    CHECK(arr[2] == "c");
}

TEST_CASE("CMakeArray: sublist", "[cmake_array]") {
    CMakeArray arr(std::string("a;b;c;d;e"));
    auto sub = arr.sublist(1, 3);
    REQUIRE(sub.size() == 3);
    CHECK(sub[0] == "b");
    CHECK(sub[1] == "c");
    CHECK(sub[2] == "d");
}

TEST_CASE("CMakeArray: contains", "[cmake_array]") {
    CMakeArray arr(std::string("a;b;c"));
    CHECK(arr.contains("b"));
    CHECK_FALSE(arr.contains("d"));
}

TEST_CASE("CMakeArray: genex semicolons are split (CMake behavior)", "[cmake_array]") {
    // CMake's list() does NOT protect semicolons inside genex -- they are still separators.
    // Genex are only evaluated later during build graph generation.
    CMakeArray arr(std::string("$<1:a;b>;c"));
    REQUIRE(arr.size() == 3);
    CHECK(arr[0] == "$<1:a");
    CHECK(arr[1] == "b>");
    CHECK(arr[2] == "c");
}

// --- CMakeArrayView tests ---

TEST_CASE("CMakeArrayView: empty string", "[cmake_array]") {
    CMakeArrayView view(std::string_view(""));
    CHECK(view.size() == 0);
    CHECK(view.empty());
    CHECK(view.to_string() == "");
}

TEST_CASE("CMakeArrayView: default constructed", "[cmake_array]") {
    CMakeArrayView view;
    CHECK(view.size() == 0);
    CHECK(view.empty());
}

TEST_CASE("CMakeArrayView: semicolon-separated", "[cmake_array]") {
    std::string src = "a;b;c";
    CMakeArrayView view(src);
    REQUIRE(view.size() == 3);
    CHECK(view[0] == "a");
    CHECK(view[1] == "b");
    CHECK(view[2] == "c");
}

TEST_CASE("CMakeArrayView: at() and bounds check", "[cmake_array]") {
    CMakeArrayView view(std::string_view("x;y"));
    CHECK(view.at(0) == "x");
    CHECK(view.at(1) == "y");
    CHECK_THROWS_AS(view.at(2), std::out_of_range);
}

TEST_CASE("CMakeArrayView: contains", "[cmake_array]") {
    CMakeArrayView view(std::string_view("foo;bar;baz"));
    CHECK(view.contains("bar"));
    CHECK_FALSE(view.contains("qux"));
}

TEST_CASE("CMakeArrayView: to_string", "[cmake_array]") {
    std::string src = "a;b;c";
    CMakeArrayView view(src);
    CHECK(view.to_string() == "a;b;c");
}

TEST_CASE("CMakeArrayView: range-for iteration", "[cmake_array]") {
    std::string src = "one;two;three";
    CMakeArrayView view(src);
    std::vector<std::string_view> items;
    for (auto sv : view) { items.push_back(sv); }
    REQUIRE(items.size() == 3);
    CHECK(items[0] == "one");
    CHECK(items[1] == "two");
    CHECK(items[2] == "three");
}

TEST_CASE("CMakeArrayView: genex semicolons are split (CMake behavior)", "[cmake_array]") {
    // CMake's list splitting does NOT protect semicolons inside genex.
    std::string src = "$<1:a;b>;c";
    CMakeArrayView view(src);
    REQUIRE(view.size() == 3);
    CHECK(view[0] == "$<1:a");
    CHECK(view[1] == "b>");
    CHECK(view[2] == "c");
}

TEST_CASE("CMakeArrayView: single element", "[cmake_array]") {
    CMakeArrayView view(std::string_view("hello"));
    REQUIRE(view.size() == 1);
    CHECK(view[0] == "hello");
    CHECK_FALSE(view.empty());
}

TEST_CASE("CMakeArrayView: trailing semicolons produce empty elements", "[cmake_array]") {
    CMakeArrayView view(std::string_view("a;b;"));
    REQUIRE(view.size() == 3);
    CHECK(view[0] == "a");
    CHECK(view[1] == "b");
    CHECK(view[2] == "");
}

TEST_CASE("CMakeArrayView: leading semicolons produce empty elements", "[cmake_array]") {
    CMakeArrayView view(std::string_view(";a;b"));
    REQUIRE(view.size() == 3);
    CHECK(view[0] == "");
    CHECK(view[1] == "a");
    CHECK(view[2] == "b");
}

TEST_CASE("CMakeArrayView: equivalence with CMakeArray", "[cmake_array]") {
    std::vector<std::string> test_strings = {
        "", "a", "a;b;c", "$<1:a;b>;c", ";a;b;", ";;", "hello", "x;y;z;w;v", "$<AND:$<1:a;b>;$<0:c;d>>;e"};
    for (const auto& s : test_strings) {
        CMakeArray arr(s);
        CMakeArrayView view(s);

        INFO("Testing string: \"" << s << "\"");
        REQUIRE(arr.size() == view.size());
        for (size_t i = 0; i < arr.size(); ++i) { CHECK(arr[i] == view[i]); }
    }
}
