#include <catch2/catch_test_macros.hpp>
#include "kiln/module_scanner.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

using namespace kiln;

namespace {

int detect_gcc_major() {
    FILE* p = popen("g++ -dumpfullversion -dumpversion 2>/dev/null", "r");
    if (!p) return 0;
    char buf[64] = {};
    size_t n = fread(buf, 1, sizeof(buf) - 1, p);
    pclose(p);
    if (n == 0) return 0;
    return std::atoi(buf);
}

bool gcc_is_real_gcc() {
    FILE* p = popen("g++ --version 2>/dev/null", "r");
    if (!p) return false;
    char buf[256] = {};
    size_t n = fread(buf, 1, sizeof(buf) - 1, p);
    pclose(p);
    if (n == 0) return false;
    std::string out(buf, n);
    return out.find("clang") == std::string::npos && (out.find("g++") != std::string::npos || out.find("GCC") != std::string::npos);
}

} // namespace

// Canonical sample shape produced by `g++ -fdeps-format=p1689r5` on a
// module-interface unit. Verified locally against GCC 16:
//   g++ -fmodules-ts -std=c++20 -fdeps-format=p1689r5 \
//       -fdeps-file=out.json -fdeps-target=foo.o \
//       -E -MD -MF foo.d -x c++ math.cppm -o /dev/null
TEST_CASE("P1689 parse: interface unit with no requires", "[p1689]") {
    constexpr auto json = R"({
"rules": [{
  "primary-output": "/tmp/foo.o",
  "provides": [ { "logical-name": "Math", "is-interface": true } ],
  "requires": []
}],
"version": 0, "revision": 0
})";

    auto parsed = parse_p1689_string(json);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->rules.size() == 1);
    const auto& r = parsed->rules[0];
    CHECK(r.primary_output == "/tmp/foo.o");
    REQUIRE(r.provides.size() == 1);
    CHECK(r.provides[0].logical_name == "Math");
    CHECK(r.provides[0].is_interface == true);
    CHECK(r.requires_.empty());
}

TEST_CASE("P1689 parse: importer with by-name requires", "[p1689]") {
    constexpr auto json = R"({
"rules": [{
  "primary-output": "/tmp/main.o",
  "provides": [],
  "requires": [
    { "logical-name": "Math", "lookup-method": "by-name" },
    { "logical-name": "std", "lookup-method": "by-name" }
  ]
}],
"version": 0, "revision": 0
})";

    auto parsed = parse_p1689_string(json);
    REQUIRE(parsed.has_value());
    const auto& r = parsed->rules[0];
    CHECK(r.provides.empty());
    REQUIRE(r.requires_.size() == 2);
    CHECK(r.requires_[0].logical_name == "Math");
    CHECK(r.requires_[0].lookup_method == "by-name");
    CHECK(r.requires_[1].logical_name == "std");
}

TEST_CASE("P1689 parse: partition appears as 'Mod:Part'", "[p1689]") {
    constexpr auto json = R"({
"rules": [{
  "primary-output": "/tmp/p.o",
  "provides": [ { "logical-name": "Math:Helpers", "is-interface": true } ],
  "requires": []
}],
"version": 0, "revision": 0
})";

    auto parsed = parse_p1689_string(json);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->rules[0].provides.size() == 1);
    CHECK(parsed->rules[0].provides[0].logical_name == "Math:Helpers");
}

TEST_CASE("P1689 parse: header-unit lookup-method survives parse", "[p1689]") {
    // The collator (build_system.cpp) is what rejects header units; the
    // parser preserves whatever the compiler emitted so the diagnostic
    // can name the offending lookup-method.
    constexpr auto json = R"({
"rules": [{
  "primary-output": "/tmp/main.o",
  "provides": [],
  "requires": [
    { "logical-name": "vector", "source-path": "/usr/include/c++/16/vector",
      "lookup-method": "include-angle" }
  ]
}],
"version": 0, "revision": 0
})";

    auto parsed = parse_p1689_string(json);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->rules[0].requires_.size() == 1);
    CHECK(parsed->rules[0].requires_[0].lookup_method == "include-angle");
    CHECK(parsed->rules[0].requires_[0].logical_name == "vector");
}

TEST_CASE("P1689 parse: malformed JSON returns error", "[p1689]") {
    auto parsed = parse_p1689_string("{ this is not json");
    REQUIRE_FALSE(parsed.has_value());
}

// End-to-end: run real g++ to emit P1689, feed to our parser. Validates that
// the schema matches what the compiler produces, not just what we believe.
// Skipped silently when GCC is missing or too old.
TEST_CASE("P1689 parse: real g++ output round-trips", "[p1689][gcc]") {
    if (!gcc_is_real_gcc()) SKIP("g++ not GCC (or unavailable)");
    int major = detect_gcc_major();
    if (major < 14) SKIP("GCC " + std::to_string(major) + " predates -fdeps-format=p1689r5 (need 14+)");

    namespace fs = std::filesystem;
    auto tmpdir = fs::temp_directory_path() / ("kiln_p1689_test_" + std::to_string(::getpid()));
    fs::create_directories(tmpdir);

    auto cppm = tmpdir / "Math.cppm";
    auto importer = tmpdir / "use.cpp";
    auto ddi_iface = tmpdir / "iface.json";
    auto ddi_user = tmpdir / "user.json";

    { std::ofstream(cppm) << "export module Math;\nexport int square(int x) { return x * x; }\n"; }
    { std::ofstream(importer) << "import Math;\nint main() { return square(4) - 16; }\n"; }

    auto run_scan = [&](const fs::path& src, const fs::path& obj, const fs::path& ddi) {
        std::string cmd = "g++ -fmodules-ts -std=c++20 -fdeps-format=p1689r5"
                          " -fdeps-file="
                          + ddi.string() + " -fdeps-target=" + obj.string() + " -E -MD -MF " + (ddi.string() + ".d") + " -x c++ "
                          + src.string() + " -o /dev/null 2>/dev/null";
        return std::system(cmd.c_str());
    };

    REQUIRE(run_scan(cppm, tmpdir / "Math.o", ddi_iface) == 0);
    REQUIRE(run_scan(importer, tmpdir / "use.o", ddi_user) == 0);

    {
        auto p = parse_p1689_file(ddi_iface.string());
        REQUIRE(p.has_value());
        REQUIRE(p->rules.size() == 1);
        REQUIRE(p->rules[0].provides.size() == 1);
        CHECK(p->rules[0].provides[0].logical_name == "Math");
        CHECK(p->rules[0].requires_.empty());
    }
    {
        auto p = parse_p1689_file(ddi_user.string());
        REQUIRE(p.has_value());
        REQUIRE(p->rules.size() == 1);
        CHECK(p->rules[0].provides.empty());
        bool saw_math = false;
        for (const auto& r : p->rules[0].requires_) {
            if (r.logical_name == "Math") {
                saw_math = true;
                CHECK(r.lookup_method == "by-name");
            }
        }
        CHECK(saw_math);
    }

    std::error_code ec;
    fs::remove_all(tmpdir, ec);
}

TEST_CASE("Module manifest: round-trip", "[manifest]") {
    namespace fs = std::filesystem;
    auto tmp = fs::temp_directory_path() / ("kiln_manifest_" + std::to_string(::getpid()) + ".json");

    ModuleManifest m;
    m.entries.push_back({"Foo", "/bmi/Foo.gcm", "/obj/foo.o", "src/foo.cppm", "PUBLIC"});
    m.entries.push_back({"Foo:Helpers", "/bmi/Foo-Helpers.gcm", "/obj/helpers.o", "src/helpers.cppm", "INTERFACE"});

    auto wr = write_module_manifest(tmp.string(), m);
    REQUIRE(wr.has_value());

    auto rd = read_module_manifest(tmp.string());
    REQUIRE(rd.has_value());
    REQUIRE(rd->entries.size() == 2);
    CHECK(rd->entries[0].logical_name == "Foo");
    CHECK(rd->entries[0].bmi_path == "/bmi/Foo.gcm");
    CHECK(rd->entries[0].primary_output == "/obj/foo.o");
    CHECK(rd->entries[0].visibility == "PUBLIC");
    CHECK(rd->entries[1].logical_name == "Foo:Helpers");
    CHECK(rd->entries[1].visibility == "INTERFACE");

    std::error_code ec;
    fs::remove(tmp, ec);
}

TEST_CASE("Module manifest: missing file returns error", "[manifest]") {
    auto rd = read_module_manifest("/nonexistent/manifest.json");
    REQUIRE_FALSE(rd.has_value());
}

TEST_CASE("Module manifest: malformed JSON returns error", "[manifest]") {
    auto rd = parse_module_manifest_string("{ not json");
    REQUIRE_FALSE(rd.has_value());
}

TEST_CASE("P1689 parse: multiple rules survive parse", "[p1689]") {
    // The collator rejects multi-rule files; the parser just preserves them.
    constexpr auto json = R"({
"rules": [
  { "primary-output": "a.o", "provides": [], "requires": [] },
  { "primary-output": "b.o", "provides": [], "requires": [] }
],
"version": 0, "revision": 0
})";
    auto parsed = parse_p1689_string(json);
    REQUIRE(parsed.has_value());
    CHECK(parsed->rules.size() == 2);
}
