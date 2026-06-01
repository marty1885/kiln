#pragma once
#include "gnu_compiler.hpp"
#include "path.hpp"
#include <sstream>

#ifdef __unix__
#include <sys/utsname.h>
#endif

namespace kiln {

namespace {

// NVCC does not accept -Wl,... directly; forward each piece to the host linker.
void nvccify_host_linker_args(std::vector<std::string>& args) {
    std::vector<std::string> out;
    out.reserve(args.size() + 8);

    auto push_xlinker = [&](std::string piece) {
        if (piece.size() >= 2 && piece.front() == '\'' && piece.back() == '\'') piece = piece.substr(1, piece.size() - 2);
        if (piece.empty()) return;
        out.push_back("-Xlinker");
        out.push_back(std::move(piece));
    };

    for (const auto& arg : args) {
        if (arg.starts_with("-Wl,")) {
            std::string_view payload(arg);
            payload.remove_prefix(4);
            size_t pos = 0;
            while (pos < payload.size()) {
                auto comma = payload.find(',', pos);
                if (comma == std::string_view::npos) comma = payload.size();
                push_xlinker(std::string(payload.substr(pos, comma - pos)));
                pos = comma + 1;
            }
            continue;
        }
        if (arg == "-pie" || arg == "-shared") {
            push_xlinker(arg);
            continue;
        }
        out.push_back(arg);
    }
    args = std::move(out);
}

} // namespace

class NvccCompiler : public GnuCompiler {
public:
    using GnuCompiler::GnuCompiler;

    PlatformInfo detect_platform() const override {
        PlatformInfo info;
        info.sizeof_void_p = std::to_string(sizeof(void*));

        std::string version_output = detail::run_command(binary_ + " --version 2>&1");
        if (!version_output.empty() && version_output.find("Cuda compilation tools") != std::string::npos) {
            info.compiler_id = "NVCC";
            auto vpos = version_output.rfind('V');
            if (vpos != std::string::npos) {
                auto end = version_output.find_first_not_of("0123456789.", vpos + 1);
                info.compiler_version = version_output.substr(vpos + 1, end - vpos - 1);
            }
        } else {
            info.compiler_id = "NVCC";
        }

#ifdef __unix__
        struct utsname un;
        if (uname(&un) == 0) {
            info.system_name = un.sysname;
            info.system_processor = un.machine;
        } else
#endif
        {
            info.system_name = "Linux";
            info.system_processor = "x86_64";
        }

        return info;
    }

    std::string std_compile_option(Language lang, int standard) const override {
        if (lang == Language::CXX || lang == Language::CUDA) { return "-std=c++" + std::to_string(standard); }
        if (lang == Language::C) { return "-std=c" + std::to_string(standard); }
        return {};
    }

    CompilerCommand get_compile_command(const CompileContext& ctx) const override {
        std::vector<std::string> cmd;
        std::vector<size_t> cosmetic;
        cmd.push_back(binary_);
        inject_target_flags(cmd);

        if (!ctx.standard.empty() && lang_ != Language::ASM) {
            if (lang_ == Language::C) {
                std::string std_prefix = "c";
                if (ctx.extensions_enabled) std_prefix = "gnu" + std_prefix.substr(1);
                cmd.push_back("-std=" + std_prefix + ctx.standard);
            } else {
                // NVCC only accepts -std=c++NN, not gnu++NN.
                cmd.push_back("-std=c++" + ctx.standard);
            }
        }

        if (ctx.color_diagnostics) emit_color_flag(cmd, cosmetic);

        for (const auto& opt : ctx.options) {
            if (opt.empty()) continue;
            if (opt.starts_with("SHELL:")) {
                std::string rest = opt.substr(6);
                std::stringstream ss(rest);
                std::string arg;
                while (ss >> arg) { cmd.push_back(arg); }
            } else {
                cmd.push_back(opt);
            }
        }
        for (const auto& def : ctx.definitions) {
            std::string clean_def = def;
            if (clean_def.starts_with("-D")) { clean_def = clean_def.substr(2); }
            if (clean_def.empty()) continue;
            cmd.push_back("-D" + clean_def);
        }

        emit_dependency_flags(cmd, ctx.output);

        if (ctx.is_shared)
            cmd.push_back("-fPIC");
        else if (ctx.is_pie)
            cmd.push_back("-fPIE");
        if (!ctx.visibility_preset.empty()) cmd.push_back("-fvisibility=" + ctx.visibility_preset);
        if (ctx.visibility_inlines_hidden) cmd.push_back("-fvisibility-inlines-hidden");

        cmd.push_back("-c");
        cmd.push_back("-o");
        cmd.push_back(ctx.output);

        for (const auto& dir : ctx.includes) { cmd.push_back("-I" + dir); }
        for (const auto& dir : ctx.system_includes) { cmd.push_back("-isystem" + dir); }

        if (!ctx.pch_include.empty()) {
            cmd.push_back("-Winvalid-pch");
            std::stringstream ss(ctx.pch_include);
            std::string arg;
            while (ss >> arg) cmd.push_back(arg);
        }

        cmd.push_back(ctx.source);

        for (const auto& arg : cmd) assert_no_genex(arg, "compile command");
        return finalize(std::move(cmd), cosmetic);
    }

    CompilerCommand get_link_command(const LinkContext& ctx) const override {
        auto cc = GnuCompiler::get_link_command(ctx);
        for (auto& arg : cc.argv) {
            if (arg.starts_with("-std=gnu++")) arg.replace(0, 10, "-std=c++");
        }
        for (auto& arg : cc.signature_argv) {
            if (arg.starts_with("-std=gnu++")) arg.replace(0, 10, "-std=c++");
        }
        nvccify_host_linker_args(cc.argv);
        nvccify_host_linker_args(cc.signature_argv);
        return cc;
    }
};

} // namespace kiln
