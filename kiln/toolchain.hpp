#pragma once
#include "compiler.hpp"
#include "gnu_compiler.hpp"
#include "language.hpp"
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>

namespace kiln {

// Registry of detected compilers, keyed by (language, binary, sysroot, target).
// Holds:
//   - "default" compiler per language: the one set up by the most recent
//     enable_language() / project() call, used by code that doesn't have
//     a per-target capture available.
//   - "additional" compilers: any compiler resolved at target-definition
//     time when a target's scope had a different CMAKE_<LANG>_COMPILER /
//     CMAKE_SYSROOT / CMAKE_<LANG>_COMPILER_TARGET than the current default.
//     This is what makes per-subdirectory `set(CMAKE_<LANG>_COMPILER ...)`
//     work — each target captures a const Compiler* into its own state and
//     uses it at generate_tasks time, regardless of what the global default
//     has rotated to since.
class Toolchain {
public:
    // Set the default compiler for a language. Called by enable_language.
    // Also inserts the compiler into the registry under its identity tuple
    // so a per-target capture with the same (binary, sysroot, target) hits
    // the same instance instead of creating a duplicate.
    void set_compiler(Language lang, std::unique_ptr<Compiler> compiler) {
        std::lock_guard<std::mutex> lock(mutex_);
        Compiler* raw = compiler.get();
        defaults_[lang] = raw;
        Key key{lang, raw->binary(), raw->sysroot(), raw->compiler_target()};
        // If a registry entry already exists under this key (e.g. an earlier
        // get_or_register from try_compile resolved the same tuple), keep
        // the existing one and discard the new compiler — pointer stability
        // matters for Targets that already captured a const Compiler*.
        auto [it, inserted] = registry_.try_emplace(std::move(key), std::move(compiler));
        if (!inserted) {
            // Repoint the default to the existing instance so callers that
            // hit defaults_ also see the canonical pointer.
            defaults_[lang] = it->second.get();
        }
    }

    const Compiler* get_compiler(Language lang) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = defaults_.find(lang);
        return it == defaults_.end() ? nullptr : it->second;
    }

    const Compiler* get_compiler_ptr(Language lang) const { return get_compiler(lang); }

    // Look up or register a compiler for this exact (lang, binary, sysroot,
    // target) combination. Returns a stable pointer owned by the registry.
    // Used by per-target capture in add_executable / add_library, and by
    // try_compile when scope vars differ from the default.
    //
    // The Key is (lang, binary, sysroot, target) — `compiler_id` is not
    // part of identity (a binary's id is determined by the binary). It only
    // feeds the factory on insert.
    //
    // const because target generation is invoked through const Toolchain&
    // and may need to lazily resolve a captured (binary, sysroot, target)
    // tuple. Internal state is mutex-protected.
    const Compiler* get_or_register(Language lang, const std::string& compiler_id, const std::string& binary, const std::string& sysroot,
                                    const std::string& compiler_target) const {
        Key key{lang, binary, sysroot, compiler_target};
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = registry_.find(key);
            if (it != registry_.end()) return it->second.get();
            auto compiler = make_compiler(compiler_id, binary, lang, sysroot, compiler_target);
            // Propagate version from any sibling entry sharing this binary
            // (e.g. the default compiler registered by enable_language). The
            // detected version lives on whichever Compiler ran detect_platform;
            // a per-subdir `set(CMAKE_CXX_COMPILER ...)` that captures the same
            // binary under a different sysroot/target should still mix the
            // version into task signatures.
            for (const auto& [_, sibling] : registry_) {
                if (sibling->binary() == binary && !sibling->version().empty()) {
                    compiler->set_version(sibling->version());
                    break;
                }
            }
            const Compiler* raw = compiler.get();
            registry_.emplace(std::move(key), std::move(compiler));
            return raw;
        }
    }

    // Look up a registered compiler's detected version by binary path.
    // Returns the version string (e.g. "13.2.0") or empty if no compiler
    // with this binary is registered. Used by BuildGraph::calculate_signature
    // to mix the compiler version into task signatures without re-probing
    // arbitrary executables (which would be unsafe — custom_command first
    // tokens are arbitrary user scripts).
    std::string version_for(const std::string& binary) const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [_, uniq] : registry_) {
            if (uniq->binary() == binary) {
                const std::string& v = uniq->version();
                if (!v.empty()) return v;
            }
        }
        return {};
    }

private:
    using Key = std::tuple<Language, std::string, std::string, std::string>;
    mutable std::mutex mutex_;
    std::map<Language, const Compiler*> defaults_;
    mutable std::map<Key, std::unique_ptr<Compiler>> registry_;
};

} // namespace kiln
