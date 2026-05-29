#include <catch2/catch_test_macros.hpp>
#include "kiln/target.hpp"
#include "kiln/interperter.hpp"
#include "kiln/cmake-language.hpp"
#include "kiln/builtins/registry.hpp"
#include <filesystem>
#include <sstream>

using namespace kiln;

// Helper: interpret a script, resolve all targets, return them + stderr output
static auto run_and_resolve(const std::string& script) {
    std::string temp_dir = "build_test_resolve";
    std::filesystem::create_directories(temp_dir);

    std::ostringstream out, err;
    Interpreter interp(".", &out, &err, temp_dir);
    register_target_builtins(interp);

    // Set variables that resolve/genex evaluation needs
    interp.set_variable("CMAKE_BUILD_TYPE", "Debug");
    interp.set_variable("CMAKE_CXX_COMPILER_ID", "GNU");
    interp.set_variable("CMAKE_C_COMPILER_ID", "GNU");

    interp.set_source_view(script);
    Parser parser(script);
    auto ast = parser.parse();
    REQUIRE(ast.has_value());
    auto result = interp.interpret(*ast);
    REQUIRE(result.has_value());

    // Resolve all targets
    auto& targets = interp.get_targets();
    for (auto& [name, target] : targets) { target->resolve(targets, interp); }

    // Deferred circular dep pass
    for (auto& [name, target] : targets) { target->resolve_deferred_circular_deps(targets); }

    std::filesystem::remove_all(temp_dir);
    return std::make_pair(std::move(targets), err.str());
}

static bool contains(const std::vector<std::string>& vec, const std::string& val) {
    return std::find(vec.begin(), vec.end(), val) != vec.end();
}

TEST_CASE("PUBLIC include propagation", "[resolve]") {
    auto [targets, err] = run_and_resolve(R"(
        add_library(mylib INTERFACE)
        target_include_directories(mylib INTERFACE /usr/include/mylib)
        add_executable(myapp main.cpp)
        target_link_libraries(myapp PUBLIC mylib)
    )");

    auto& app = targets["myapp"];
    const auto& includes = app->get_resolved_property("INCLUDE_DIRECTORIES");
    REQUIRE(contains(includes, "/usr/include/mylib"));
}

TEST_CASE("PRIVATE include does NOT propagate", "[resolve]") {
    auto [targets, err] = run_and_resolve(R"(
        add_library(mylib STATIC lib.cpp)
        target_include_directories(mylib PRIVATE /internal/include)
        add_executable(myapp main.cpp)
        target_link_libraries(myapp PUBLIC mylib)
    )");

    auto& app = targets["myapp"];
    const auto& includes = app->get_resolved_property("INCLUDE_DIRECTORIES");
    REQUIRE_FALSE(contains(includes, "/internal/include"));
}

TEST_CASE("Transitive propagation A->B->C", "[resolve]") {
    auto [targets, err] = run_and_resolve(R"(
        add_library(C INTERFACE)
        target_include_directories(C INTERFACE /c/include)
        add_library(B INTERFACE)
        target_link_libraries(B INTERFACE C)
        add_executable(A main.cpp)
        target_link_libraries(A PUBLIC B)
    )");

    auto& a = targets["A"];
    const auto& includes = a->get_resolved_property("INCLUDE_DIRECTORIES");
    REQUIRE(contains(includes, "/c/include"));
}

TEST_CASE("PRIVATE breaks transitive chain", "[resolve]") {
    auto [targets, err] = run_and_resolve(R"(
        add_library(C INTERFACE)
        target_include_directories(C INTERFACE /c/include)
        add_library(B STATIC b.cpp)
        target_link_libraries(B PRIVATE C)
        add_executable(A main.cpp)
        target_link_libraries(A PUBLIC B)
    )");

    auto& a = targets["A"];
    const auto& includes = a->get_resolved_property("INCLUDE_DIRECTORIES");
    REQUIRE_FALSE(contains(includes, "/c/include"));
}

TEST_CASE("INTERFACE library propagation", "[resolve]") {
    auto [targets, err] = run_and_resolve(R"(
        add_library(iface INTERFACE)
        target_include_directories(iface INTERFACE /iface/include)
        target_compile_definitions(iface INTERFACE USE_IFACE)
        add_executable(myapp main.cpp)
        target_link_libraries(myapp PUBLIC iface)
    )");

    auto& app = targets["myapp"];
    REQUIRE(contains(app->get_resolved_property("INCLUDE_DIRECTORIES"), "/iface/include"));
    REQUIRE(contains(app->get_resolved_property("COMPILE_DEFINITIONS"), "USE_IFACE"));
}

TEST_CASE("Static lib propagates PRIVATE link deps", "[resolve]") {
    auto [targets, err] = run_and_resolve(R"(
        add_library(inner STATIC inner.cpp)
        add_library(outer STATIC outer.cpp)
        target_link_libraries(outer PRIVATE inner)
        add_executable(myapp main.cpp)
        target_link_libraries(myapp PUBLIC outer)
    )");

    auto& app = targets["myapp"];
    const auto& libs = app->get_resolved_property("LINK_LIBRARIES");
    // Static libs propagate ALL link deps (inner must appear for symbol resolution)
    bool has_inner = std::any_of(libs.begin(), libs.end(), [](const std::string& s) { return s.find("libinner.a") != std::string::npos; });
    REQUIRE(has_inner);
}

TEST_CASE("Shared lib does NOT propagate PRIVATE link deps", "[resolve]") {
    auto [targets, err] = run_and_resolve(R"(
        add_library(inner STATIC inner.cpp)
        add_library(outer SHARED outer.cpp)
        target_link_libraries(outer PRIVATE inner)
        add_executable(myapp main.cpp)
        target_link_libraries(myapp PUBLIC outer)
    )");

    auto& app = targets["myapp"];
    const auto& libs = app->get_resolved_property("LINK_LIBRARIES");
    bool has_inner = std::any_of(libs.begin(), libs.end(), [](const std::string& s) { return s.find("libinner.a") != std::string::npos; });
    REQUIRE_FALSE(has_inner);
}

TEST_CASE("Order-preserving dedup", "[resolve]") {
    auto [targets, err] = run_and_resolve(R"(
        add_library(B INTERFACE)
        target_include_directories(B INTERFACE /common/include)
        add_library(C INTERFACE)
        target_include_directories(C INTERFACE /common/include)
        add_executable(A main.cpp)
        target_link_libraries(A PUBLIC B C)
    )");

    auto& a = targets["A"];
    const auto& includes = a->get_resolved_property("INCLUDE_DIRECTORIES");
    // /common/include should appear exactly once
    int count = std::count(includes.begin(), includes.end(), "/common/include");
    REQUIRE(count == 1);
}

TEST_CASE("TargetPropertyScope::BUILD returns PRIVATE + PUBLIC", "[resolve]") {
    auto [targets, err] = run_and_resolve(R"(
        add_library(mylib STATIC priv.cpp)
        target_include_directories(mylib PRIVATE /priv/inc)
        target_include_directories(mylib PUBLIC /pub/inc)
        target_include_directories(mylib INTERFACE /iface/inc)
    )");

    auto& lib = targets["mylib"];
    auto build = lib->get_property_list("INCLUDE_DIRECTORIES", TargetPropertyScope::BUILD);
    REQUIRE(contains(build, "/priv/inc"));
    REQUIRE(contains(build, "/pub/inc"));
    REQUIRE_FALSE(contains(build, "/iface/inc"));

    auto iface = lib->get_property_list("INCLUDE_DIRECTORIES", TargetPropertyScope::INTERFACE);
    REQUIRE_FALSE(contains(iface, "/priv/inc"));
    REQUIRE(contains(iface, "/pub/inc"));
    REQUIRE(contains(iface, "/iface/inc"));
}

TEST_CASE("Multiple visibility resolved correctly", "[resolve]") {
    auto [targets, err] = run_and_resolve(R"(
        add_library(mylib STATIC lib.cpp)
        target_include_directories(mylib PRIVATE /priv)
        target_include_directories(mylib PUBLIC /pub)
        target_include_directories(mylib INTERFACE /iface)
    )");

    auto& lib = targets["mylib"];
    // resolved_properties_ should have PRIVATE + PUBLIC
    const auto& resolved = lib->get_resolved_property("INCLUDE_DIRECTORIES");
    REQUIRE(contains(resolved, "/priv"));
    REQUIRE(contains(resolved, "/pub"));
    REQUIRE_FALSE(contains(resolved, "/iface"));

    // resolved_interface_properties_ should have PUBLIC + INTERFACE
    const auto& resolved_iface = lib->get_resolved_interface_property("INCLUDE_DIRECTORIES");
    REQUIRE_FALSE(contains(resolved_iface, "/priv"));
    REQUIRE(contains(resolved_iface, "/pub"));
    REQUIRE(contains(resolved_iface, "/iface"));
}

TEST_CASE("Imported target includes become system includes", "[resolve]") {
    auto [targets, err] = run_and_resolve(R"(
        add_library(ext STATIC IMPORTED)
        target_include_directories(ext INTERFACE /ext/include)
        add_executable(myapp main.cpp)
        target_link_libraries(myapp PUBLIC ext)
    )");

    auto& app = targets["myapp"];
    const auto& sys_includes = app->get_resolved_property("SYSTEM_INCLUDE_DIRECTORIES");
    REQUIRE(contains(sys_includes, "/ext/include"));
    // Should NOT be in regular includes
    const auto& includes = app->get_resolved_property("INCLUDE_DIRECTORIES");
    REQUIRE_FALSE(contains(includes, "/ext/include"));
}

TEST_CASE("COMPILE_DEFINITIONS propagation", "[resolve]") {
    auto [targets, err] = run_and_resolve(R"(
        add_library(mylib INTERFACE)
        target_compile_definitions(mylib INTERFACE FOO=1 BAR)
        add_executable(myapp main.cpp)
        target_link_libraries(myapp PUBLIC mylib)
    )");

    auto& app = targets["myapp"];
    const auto& defs = app->get_resolved_property("COMPILE_DEFINITIONS");
    REQUIRE(contains(defs, "FOO=1"));
    REQUIRE(contains(defs, "BAR"));
}

TEST_CASE("Per-artifact OUTPUT_NAME overrides OUTPUT_NAME", "[target][output]") {
    auto [targets, err] = run_and_resolve(R"(
        add_executable(myapp main.cpp)
        set_target_properties(myapp PROPERTIES
            OUTPUT_NAME generic_name
            RUNTIME_OUTPUT_NAME runtime_name)
        add_library(mystatic STATIC src.cpp)
        set_target_properties(mystatic PROPERTIES
            OUTPUT_NAME generic_static
            ARCHIVE_OUTPUT_NAME archive_name)
        add_library(myshared SHARED src.cpp)
        set_target_properties(myshared PROPERTIES
            OUTPUT_NAME generic_shared
            LIBRARY_OUTPUT_NAME library_name)
    )");

    REQUIRE(targets["myapp"]->get_output_path().find("runtime_name") != std::string::npos);
    REQUIRE(targets["mystatic"]->get_output_path().find("libarchive_name.a") != std::string::npos);
    REQUIRE(targets["myshared"]->get_output_path().find("liblibrary_name.so") != std::string::npos);
}

TEST_CASE("DEBUG_POSTFIX appended for libraries only", "[target][output]") {
    auto [targets, err] = run_and_resolve(R"(
        add_library(mylib STATIC src.cpp)
        set_target_properties(mylib PROPERTIES DEBUG_POSTFIX _d)
        add_library(myshared SHARED src.cpp)
        set_target_properties(myshared PROPERTIES DEBUG_POSTFIX _d)
        add_executable(myapp main.cpp)
        set_target_properties(myapp PROPERTIES DEBUG_POSTFIX _d)
    )");

    // Build type is Debug in the harness — postfix applies to libs.
    REQUIRE(targets["mylib"]->get_output_path().find("libmylib_d.a") != std::string::npos);
    REQUIRE(targets["myshared"]->get_output_path().find("libmyshared_d.so") != std::string::npos);
    // Executables are not postfixed.
    REQUIRE(targets["myapp"]->get_output_path().find("myapp_d") == std::string::npos);
}

TEST_CASE("Standard-required property is storable", "[target][std]") {
    auto [targets, err] = run_and_resolve(R"(
        add_executable(myapp main.cpp)
        set_target_properties(myapp PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            C_STANDARD_REQUIRED OFF)
    )");
    auto& t = targets["myapp"];
    REQUIRE(t->get_property("CXX_STANDARD_REQUIRED") == "ON");
    REQUIRE(t->get_property("C_STANDARD_REQUIRED") == "OFF");
    REQUIRE(t->get_language_standard(Language::CXX) == "20");
}

TEST_CASE("VERSION/SOVERSION/RPATH properties stored", "[target][rpath]") {
    auto [targets, err] = run_and_resolve(R"(
        add_library(mylib SHARED src.cpp)
        set_target_properties(mylib PROPERTIES
            VERSION 1.2.3
            SOVERSION 1
            BUILD_RPATH "/opt/foo/lib;$ORIGIN/../lib"
            INSTALL_RPATH "/opt/install/lib"
            BUILD_WITH_INSTALL_RPATH TRUE)
    )");
    auto& t = targets["mylib"];
    REQUIRE(t->get_property("VERSION") == "1.2.3");
    REQUIRE(t->get_property("SOVERSION") == "1");
    REQUIRE(t->get_property("BUILD_RPATH") == "/opt/foo/lib;$ORIGIN/../lib");
    REQUIRE(t->get_property("INSTALL_RPATH") == "/opt/install/lib");
    REQUIRE(t->get_property("BUILD_WITH_INSTALL_RPATH") == "TRUE");
}
