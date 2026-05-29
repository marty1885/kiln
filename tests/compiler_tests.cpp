#include <catch2/catch_test_macros.hpp>
#include "kiln/gnu_compiler.hpp"
#include "kiln/utils.hpp"
#include <vector>

using namespace kiln;

TEST_CASE("GnuCompiler: CXX compile command", "[compiler]") {
    GnuCompiler compiler("g++", Language::CXX);
    CompileContext ctx;
    ctx.source = "main.cpp";
    ctx.output = "main.o";
    ctx.standard = "23";
    ctx.includes = {"include", "/usr/local/include"};
    ctx.definitions = {"DEBUG", "VERSION=1"};
    ctx.is_shared = true;

    std::vector<std::string> cmd_vec = compiler.get_compile_command(ctx).argv;

    std::string cmd = kiln::join_command(cmd_vec);

    CHECK(cmd_vec[0] == "g++");
    CHECK(cmd_vec[1] == "-std=gnu++23");
    CHECK(std::find(cmd_vec.begin(), cmd_vec.end(), "-Iinclude") != cmd_vec.end());
    CHECK(std::find(cmd_vec.begin(), cmd_vec.end(), "-I/usr/local/include") != cmd_vec.end());
    CHECK(std::find(cmd_vec.begin(), cmd_vec.end(), "-DDEBUG") != cmd_vec.end());
    CHECK(std::find(cmd_vec.begin(), cmd_vec.end(), "-DVERSION=1") != cmd_vec.end());
    CHECK(std::find(cmd_vec.begin(), cmd_vec.end(), "-fPIC") != cmd_vec.end());
    CHECK(std::find(cmd_vec.begin(), cmd_vec.end(), "-c") != cmd_vec.end());
    CHECK(std::find(cmd_vec.begin(), cmd_vec.end(), "main.o") != cmd_vec.end());
    CHECK(std::find(cmd_vec.begin(), cmd_vec.end(), "main.cpp") != cmd_vec.end());
}

TEST_CASE("GnuCompiler: C compile command", "[compiler]") {
    GnuCompiler compiler("gcc", Language::C);
    CompileContext ctx;
    ctx.source = "main.c";
    ctx.output = "main.o";
    ctx.standard = "11";

    std::vector<std::string> cmd_vec = compiler.get_compile_command(ctx).argv;
    CHECK(cmd_vec[0] == "gcc");
    CHECK(std::find(cmd_vec.begin(), cmd_vec.end(), "-std=gnu11") != cmd_vec.end());
    CHECK(std::find(cmd_vec.begin(), cmd_vec.end(), "main.c") != cmd_vec.end());
}

TEST_CASE("make_compiler handles Intel ICC with GNU-style driver", "[compiler][icc]") {
    auto c = make_compiler("Intel", "icc", Language::CXX);
    REQUIRE(c != nullptr);
    CompileContext ctx;
    ctx.source = "main.cpp";
    ctx.output = "main.o";
    ctx.standard = "17";
    ctx.definitions = {"ICC_BUILD"};
    auto cmd_vec = c->get_compile_command(ctx).argv;
    CHECK(cmd_vec[0] == "icc");
    CHECK(std::find(cmd_vec.begin(), cmd_vec.end(), "-std=gnu++17") != cmd_vec.end());
    CHECK(std::find(cmd_vec.begin(), cmd_vec.end(), "-DICC_BUILD") != cmd_vec.end());
    CHECK(std::find(cmd_vec.begin(), cmd_vec.end(), "main.cpp") != cmd_vec.end());
}

TEST_CASE("GnuCompiler: Link command", "[compiler]") {
    GnuCompiler compiler("g++", Language::CXX);
    LinkContext ctx;
    ctx.output = "app";
    ctx.objects = {"main.o", "lib.o"};
    ctx.lib_dirs = {"build/lib"};
    ctx.libs = {"m", "pthread"};
    ctx.is_shared = false;

    std::vector<std::string> cmd_vec = compiler.get_link_command(ctx).argv;
    CHECK(cmd_vec[0] == "g++");
    CHECK(std::find(cmd_vec.begin(), cmd_vec.end(), "-std=c++17") == cmd_vec.end()); // No standard set in ctx, so it should not be present
    CHECK(std::find(cmd_vec.begin(), cmd_vec.end(), "main.o") != cmd_vec.end());
    CHECK(std::find(cmd_vec.begin(), cmd_vec.end(), "lib.o") != cmd_vec.end());
    CHECK(std::find(cmd_vec.begin(), cmd_vec.end(), "-Lbuild/lib") != cmd_vec.end());
    CHECK(std::find(cmd_vec.begin(), cmd_vec.end(), "-lm") != cmd_vec.end());
    CHECK(std::find(cmd_vec.begin(), cmd_vec.end(), "-lpthread") != cmd_vec.end());
}

TEST_CASE("TccCompiler: C compile command", "[compiler][tcc]") {
    TccCompiler compiler("tcc", Language::C);
    CompileContext ctx;
    ctx.source = "main.c";
    ctx.output = "main.o";
    ctx.standard = "11";
    ctx.includes = {"include"};
    ctx.definitions = {"DEBUG"};
    ctx.color_diagnostics = true; // user-facing toggle, but TCC ignores

    auto cmd_vec = compiler.get_compile_command(ctx).argv;
    auto has = [&](const std::string& s) { return std::find(cmd_vec.begin(), cmd_vec.end(), s) != cmd_vec.end(); };

    CHECK(cmd_vec[0] == "tcc");
    // Driver-internal divergences from GCC: TCC has no color, uses -MD not
    // -MMD, and no link-group flags (only relevant on link, but ensure
    // -MMD is absent here).
    CHECK_FALSE(has("-fdiagnostics-color=always"));
    CHECK_FALSE(has("-MMD"));
    CHECK(has("-MD"));
    CHECK(has("-MF"));
    // Pass-through user flags still emitted faithfully.
    CHECK(has("-std=gnu11"));
    CHECK(has("-Iinclude"));
    CHECK(has("-DDEBUG"));
    CHECK(has("-c"));
    CHECK(has("main.c"));
}

TEST_CASE("TccCompiler: link command has no --start-group", "[compiler][tcc]") {
    TccCompiler compiler("tcc", Language::C);
    LinkContext ctx;
    ctx.output = "app";
    // Mix of .o and .a triggers GnuCompiler's group-wrapping branch — TCC
    // must elide the flags entirely.
    ctx.objects = {"main.o", "libfoo.a"};
    ctx.libs = {"m"};

    auto cmd_vec = compiler.get_link_command(ctx).argv;
    auto has = [&](const std::string& s) { return std::find(cmd_vec.begin(), cmd_vec.end(), s) != cmd_vec.end(); };

    CHECK_FALSE(has("-Wl,--start-group"));
    CHECK_FALSE(has("-Wl,--end-group"));
    CHECK(has("main.o"));
    CHECK(has("libfoo.a"));
    CHECK(has("-lm"));
}

TEST_CASE("TccCompiler: archive command uses tcc -ar", "[compiler][tcc]") {
    TccCompiler compiler("tcc", Language::C);
    auto cmd = compiler.get_archive_command("libtest.a", {"a.o", "b.o"});
    REQUIRE(cmd.size() == 6);
    CHECK(cmd[0] == "tcc");
    CHECK(cmd[1] == "-ar");
    CHECK(cmd[2] == "rcs");
    CHECK(cmd[3] == "libtest.a");
    CHECK(cmd[4] == "a.o");
    CHECK(cmd[5] == "b.o");
}

TEST_CASE("TccCompiler: capability flags decline modules", "[compiler][tcc]") {
    TccCompiler compiler("tcc", Language::C);
    CHECK_FALSE(compiler.supports_p1689());
    CHECK_FALSE(compiler.uses_per_task_module_rsp());
    CHECK_FALSE(compiler.supports_import_std());
    CHECK(compiler.libstdcxx_modules_json_path().empty());
}

TEST_CASE("TccCompiler: pass-through (translator-not-validator)", "[compiler][tcc]") {
    // User-facing CMake properties translate faithfully even when TCC
    // would reject them at compile time. The driver is not a validator —
    // CMake doesn't validate -std=c100000 either, and TCC's own error
    // message is more accurate than anything kiln could synthesize.
    TccCompiler compiler("tcc", Language::C);
    CompileContext ctx;
    ctx.source = "x.c";
    ctx.output = "x.o";
    ctx.standard = "23";              // TCC accepts the syntax but ~implements C99
    ctx.visibility_preset = "hidden"; // TCC errors on -fvisibility=
    ctx.is_shared = true;             // -fPIC works on TCC
    auto cmd_vec = compiler.get_compile_command(ctx).argv;
    auto has = [&](const std::string& s) { return std::find(cmd_vec.begin(), cmd_vec.end(), s) != cmd_vec.end(); };
    CHECK(has("-std=gnu23"));
    CHECK(has("-fvisibility=hidden"));
    CHECK(has("-fPIC"));
}

TEST_CASE("make_compiler dispatches TCC to TccCompiler", "[compiler][tcc]") {
    auto c = make_compiler("TCC", "tcc", Language::C);
    REQUIRE(c != nullptr);
    // Any TCC-specific behavior confirms the right subclass; archive
    // command is a clean discriminator (GnuCompiler emits "ar", Tcc uses
    // its own binary).
    auto cmd = c->get_archive_command("out.a", {"a.o"});
    REQUIRE(!cmd.empty());
    CHECK(cmd[0] == "tcc");
}
