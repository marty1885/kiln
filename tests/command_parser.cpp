#include <catch2/catch_test_macros.hpp>
#include "kiln/command_parser.hpp"
#include <vector>
#include <string>

using namespace kiln;

TEST_CASE("CommandParser: basic positional", "[command_parser]") {
    std::vector<std::string> args = {"my_target", "src1.cpp", "src2.cpp"};
    CommandParser parser("test_cmd");

    std::string target;
    std::vector<std::string> sources;

    parser.positional(target, "target");
    parser.positionals(sources, "sources");

    auto res = parser.parse(args);
    REQUIRE(res.has_value());
    CHECK(target == "my_target");
    CHECK(sources == std::vector<std::string>{"src1.cpp", "src2.cpp"});
}

TEST_CASE("CommandParser: flags and keywords", "[command_parser]") {
    std::vector<std::string> args = {"my_lib", "SHARED", "SOURCES", "s1.cpp", "s2.cpp", "VERBOSE"};
    CommandParser parser("add_library");

    std::string name;
    bool shared = false;
    bool verbose = false;
    std::vector<std::string> sources;

    parser.positional(name, "name");
    parser.flag("SHARED", shared);
    parser.flag("VERBOSE", verbose);
    parser.list("SOURCES", sources);

    auto res = parser.parse(args);
    REQUIRE(res.has_value());
    CHECK(name == "my_lib");
    CHECK(shared == true);
    CHECK(verbose == true);
    CHECK(sources == std::vector<std::string>{"s1.cpp", "s2.cpp"});
}

TEST_CASE("CommandParser: single values", "[command_parser]") {
    std::vector<std::string> args = {"DESTINATION", "bin", "COMPONENT", "runtime"};
    CommandParser parser("install");

    std::string dest;
    std::string comp;

    parser.value("DESTINATION", dest);
    parser.value("COMPONENT", comp);

    auto res = parser.parse(args);
    REQUIRE(res.has_value());
    CHECK(dest == "bin");
    CHECK(comp == "runtime");
}

TEST_CASE("CommandParser: multi-lists", "[command_parser]") {
    std::vector<std::string> args = {"COMMAND", "echo", "hello", "COMMAND", "ls", "-l", "WORKING_DIRECTORY", "/tmp"};
    CommandParser parser("execute_process");

    std::vector<std::vector<std::string>> commands;
    std::string working_dir;

    parser.multi_list("COMMAND", commands);
    parser.value("WORKING_DIRECTORY", working_dir);

    auto res = parser.parse(args);
    REQUIRE(res.has_value());
    REQUIRE(commands.size() == 2);
    CHECK(commands[0] == std::vector<std::string>{"echo", "hello"});
    CHECK(commands[1] == std::vector<std::string>{"ls", "-l"});
    CHECK(working_dir == "/tmp");
}

TEST_CASE("CommandParser: optional positional", "[command_parser]") {
    std::vector<std::string> args = {};
    CommandParser parser("test_cmd");

    std::string target = "default";
    parser.positional(target, "target", false);

    auto res = parser.parse(args);
    REQUIRE(res.has_value());
    CHECK(target == "default");
}

TEST_CASE("CommandParser: positional stops at keyword", "[command_parser]") {
    std::vector<std::string> args = {"pos1", "KEY", "val"};
    CommandParser parser("test_cmd");

    std::string p1, p2;
    std::string key_val;

    parser.positional(p1, "p1");
    parser.positional(p2, "p2", false);
    parser.value("KEY", key_val);

    auto res = parser.parse(args);
    REQUIRE(res.has_value());
    CHECK(p1 == "pos1");
    CHECK(p2 == "");
    CHECK(key_val == "val");
}

TEST_CASE("CommandParser: default list after positionals", "[command_parser]") {
    std::vector<std::string> args = {"target", "extra1", "extra2"};
    CommandParser parser("test_cmd");

    std::string target;
    std::vector<std::string> extras;

    parser.positional(target, "target");
    parser.positionals(extras, "extras");

    auto res = parser.parse(args);
    REQUIRE(res.has_value());
    CHECK(target == "target");
    CHECK(extras == std::vector<std::string>{"extra1", "extra2"});
}

TEST_CASE("CommandParser: flags do not interrupt positional parsing", "[command_parser]") {
    // Actually, according to docs/command_parser.md:
    // "If a recognized keyword is encountered during this phase, it terminates positional parsing (unless the keyword is a flag)."

    std::vector<std::string> args = {"pos1", "MY_FLAG", "pos2"};
    CommandParser parser("test_cmd");

    std::string p1, p2;
    bool flag = false;

    parser.positional(p1, "p1");
    parser.positional(p2, "p2");
    parser.flag("MY_FLAG", flag);

    auto res = parser.parse(args);
    REQUIRE(res.has_value());
    CHECK(p1 == "pos1");
    CHECK(p2 == "pos2");
    CHECK(flag == true);
}

TEST_CASE("CommandParser: unparsed collects bare args after keywords", "[command_parser]") {
    // Simulates: find_package(Qt5 REQUIRED Gui Widgets QUIET)
    std::vector<std::string> args = {"Qt5", "REQUIRED", "Gui", "Widgets", "QUIET"};
    CommandParser parser("find_package");

    std::string name;
    bool required = false;
    bool quiet = false;
    std::vector<std::string> components;
    std::vector<std::string> bare;

    parser.positional(name, "name");
    parser.flag("REQUIRED", required);
    parser.flag("QUIET", quiet);
    parser.list("COMPONENTS", components);
    parser.unparsed(bare);

    auto res = parser.parse(args);
    REQUIRE(res.has_value());
    CHECK(name == "Qt5");
    CHECK(required == true);
    CHECK(quiet == true);
    CHECK(bare == std::vector<std::string>{"Gui", "Widgets"});
}

TEST_CASE("CommandParser: without unparsed, bare args still error", "[command_parser]") {
    std::vector<std::string> args = {"Qt5", "REQUIRED", "Gui"};
    CommandParser parser("find_package");

    std::string name;
    bool required = false;

    parser.positional(name, "name");
    parser.flag("REQUIRED", required);

    auto res = parser.parse(args);
    REQUIRE_FALSE(res.has_value());
    CHECK(res.error().find("unexpected argument") != std::string::npos);
}

TEST_CASE("CommandParser: unparsed with explicit COMPONENTS", "[command_parser]") {
    // find_package(Qt5 COMPONENTS Core REQUIRED Widgets)
    // "Widgets" is bare after REQUIRED, "Core" is explicit via COMPONENTS
    std::vector<std::string> args = {"Qt5", "COMPONENTS", "Core", "REQUIRED", "Widgets"};
    CommandParser parser("find_package");

    std::string name;
    bool required = false;
    std::vector<std::string> components;
    std::vector<std::string> bare;

    parser.positional(name, "name");
    parser.flag("REQUIRED", required);
    parser.list("COMPONENTS", components);
    parser.unparsed(bare);

    auto res = parser.parse(args);
    REQUIRE(res.has_value());
    CHECK(name == "Qt5");
    CHECK(required == true);
    CHECK(components == std::vector<std::string>{"Core"});
    CHECK(bare == std::vector<std::string>{"Widgets"});
}
