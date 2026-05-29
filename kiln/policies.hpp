#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "utils.hpp"

namespace kiln {

struct PolicyInfo {
    std::string_view name;       // e.g. "CMP0126"
    std::string_view introduced; // CMake version that introduced NEW behavior
};

enum class CMakePolicy : uint8_t {
    CMP0037, // 3.0:  Target names should not be reserved and should match a validity pattern
    CMP0126, // 3.21: set(CACHE) removes local variables
    CMP0148, // 3.27: FindPythonInterp/FindPythonLibs removed
    CMP0167, // 3.30: Boost find_package config-first
    CMP0173, // 3.31: CMakeFindFrameworks.cmake is deprecated
    COUNT
};

inline constexpr PolicyInfo policy_info[] = {
    {"CMP0037", "3.0"}, {"CMP0126", "3.21"}, {"CMP0148", "3.27"}, {"CMP0167", "3.30"}, {"CMP0173", "3.31"},
};
static_assert(std::size(policy_info) == size_t(CMakePolicy::COUNT));

enum class PolicyState : uint8_t { OLD, NEW };

// Mark sites that use OLD behavior — grep KILN_POLICY_OLD to find all deviations
#define KILN_POLICY_OLD(policy) /* using OLD behavior for policy */

struct PolicyStack {
    using StateArray = std::array<PolicyState, size_t(CMakePolicy::COUNT)>;
    StateArray current_;
    std::vector<StateArray> stack_;

    PolicyState get(CMakePolicy p) const { return current_[size_t(p)]; }
    void set(CMakePolicy p, PolicyState s) { current_[size_t(p)] = s; }
    void push() { stack_.push_back(current_); }
    void pop() {
        if (!stack_.empty()) {
            current_ = stack_.back();
            stack_.pop_back();
        }
    }

    // Set all known policies based on cmake_minimum_required version.
    // Policies introduced at or before the requested version → NEW,
    // later ones → OLD.
    void set_defaults_for_version(std::string_view version) {
        for (size_t i = 0; i < size_t(CMakePolicy::COUNT); ++i) {
            current_[i] = compare_versions(version, policy_info[i].introduced) >= 0 ? PolicyState::NEW : PolicyState::OLD;
        }
    }

    // Defaults match the CMake version kiln emulates (3.31) — all
    // known policies are NEW.  cmake_minimum_required() then
    // downgrades to OLD for policies introduced after the requested
    // version.
    static PolicyStack make_defaults() {
        PolicyStack ps;
        ps.set_defaults_for_version("3.31");
        return ps;
    }
};

// Parse "CMP0126" → enum. Returns nullopt for unknown policies.
inline std::optional<CMakePolicy> parse_cmake_policy(std::string_view name) {
    for (size_t i = 0; i < size_t(CMakePolicy::COUNT); ++i) {
        if (name == policy_info[i].name) return static_cast<CMakePolicy>(i);
    }
    return std::nullopt;
}

} // namespace kiln
