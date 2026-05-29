#include <catch2/catch_test_macros.hpp>
#include "kiln/shadow_map.hpp"

using namespace kiln;

TEST_CASE("ShadowMap basic operations", "[shadow_map]") {
    ShadowMap map;

    SECTION("Initial state") {
        REQUIRE(map.depth() == 0);
        REQUIRE(map.get("VAR") == "");
        REQUIRE_FALSE(map.is_defined("VAR"));
    }

    SECTION("Set and get at root level") {
        map.set("VAR", "value1");
        REQUIRE(map.get("VAR") == "value1");
        REQUIRE(map.is_defined("VAR"));
    }

    SECTION("Modify existing variable at same depth") {
        map.set("VAR", "value1");
        map.set("VAR", "value2");
        REQUIRE(map.get("VAR") == "value2");
        REQUIRE(map.is_defined("VAR"));
    }

    SECTION("Multiple variables at root") {
        map.set("VAR1", "value1");
        map.set("VAR2", "value2");
        map.set("VAR3", "value3");

        REQUIRE(map.get("VAR1") == "value1");
        REQUIRE(map.get("VAR2") == "value2");
        REQUIRE(map.get("VAR3") == "value3");
    }
}

TEST_CASE("ShadowMap variable shadowing", "[shadow_map]") {
    ShadowMap map;

    SECTION("Shadow variable in nested scope") {
        map.set("VAR", "outer");
        REQUIRE(map.get("VAR") == "outer");

        map.push_scope();
        REQUIRE(map.depth() == 1);
        REQUIRE(map.get("VAR") == "outer"); // Parent value visible

        map.set("VAR", "inner");
        REQUIRE(map.get("VAR") == "inner"); // Shadowed

        map.pop_scope();
        REQUIRE(map.depth() == 0);
        REQUIRE(map.get("VAR") == "outer"); // Original value restored
    }

    SECTION("Multiple levels of shadowing") {
        map.set("VAR", "depth0");

        map.push_scope(); // depth 1
        map.set("VAR", "depth1");
        REQUIRE(map.get("VAR") == "depth1");

        map.push_scope(); // depth 2
        map.set("VAR", "depth2");
        REQUIRE(map.get("VAR") == "depth2");

        map.push_scope(); // depth 3
        map.set("VAR", "depth3");
        REQUIRE(map.get("VAR") == "depth3");

        map.pop_scope(); // back to depth 2
        REQUIRE(map.get("VAR") == "depth2");

        map.pop_scope(); // back to depth 1
        REQUIRE(map.get("VAR") == "depth1");

        map.pop_scope(); // back to depth 0
        REQUIRE(map.get("VAR") == "depth0");
    }

    SECTION("Shadow some variables but not others") {
        map.set("VAR1", "outer1");
        map.set("VAR2", "outer2");

        map.push_scope();
        map.set("VAR1", "inner1"); // Shadow VAR1
        // Don't shadow VAR2

        REQUIRE(map.get("VAR1") == "inner1");
        REQUIRE(map.get("VAR2") == "outer2"); // Still visible

        map.pop_scope();
        REQUIRE(map.get("VAR1") == "outer1");
        REQUIRE(map.get("VAR2") == "outer2");
    }
}

TEST_CASE("ShadowMap scope cleanup", "[shadow_map]") {
    ShadowMap map;

    SECTION("Pop scope removes all modifications") {
        map.push_scope();
        map.set("VAR1", "value1");
        map.set("VAR2", "value2");
        map.set("VAR3", "value3");

        REQUIRE(map.is_defined("VAR1"));
        REQUIRE(map.is_defined("VAR2"));
        REQUIRE(map.is_defined("VAR3"));

        map.pop_scope();

        REQUIRE_FALSE(map.is_defined("VAR1"));
        REQUIRE_FALSE(map.is_defined("VAR2"));
        REQUIRE_FALSE(map.is_defined("VAR3"));
    }

    SECTION("Pop scope only removes current depth") {
        map.set("VAR", "outer");

        map.push_scope();
        map.set("VAR", "inner");
        map.set("VAR2", "inner_only");

        map.pop_scope();

        REQUIRE(map.get("VAR") == "outer");    // Restored
        REQUIRE_FALSE(map.is_defined("VAR2")); // Gone
    }

    SECTION("Empty scopes don't cause issues") {
        map.set("VAR", "value");

        map.push_scope(); // Don't modify anything
        map.pop_scope();

        REQUIRE(map.get("VAR") == "value"); // Unchanged
    }
}

TEST_CASE("ShadowMap unset behavior", "[shadow_map]") {
    ShadowMap map;

    SECTION("Unset at root removes variable") {
        map.set("VAR", "value");
        REQUIRE(map.is_defined("VAR"));

        map.unset("VAR");
        REQUIRE_FALSE(map.is_defined("VAR"));
        REQUIRE(map.get("VAR") == "");
    }

    SECTION("Unset in nested scope masks parent value") {
        map.set("VAR", "outer");

        map.push_scope();
        map.set("VAR", "inner");
        REQUIRE(map.get("VAR") == "inner");

        map.unset("VAR");
        REQUIRE(map.get("VAR") == ""); // Parent masked by tombstone
        REQUIRE_FALSE(map.is_defined("VAR"));
    }

    SECTION("Unset non-existent variable is safe") {
        map.unset("NONEXISTENT"); // Should not crash
        REQUIRE_FALSE(map.is_defined("NONEXISTENT"));
    }

    SECTION("Unset only affects current depth") {
        map.set("VAR", "depth0");

        map.push_scope();
        map.set("VAR", "depth1");

        map.push_scope();
        map.set("VAR", "depth2");
        map.unset("VAR"); // Remove depth2 version, tombstone masks depth1

        REQUIRE(map.get("VAR") == ""); // Masked by tombstone
        REQUIRE_FALSE(map.is_defined("VAR"));

        map.pop_scope();
        REQUIRE(map.get("VAR") == "depth1"); // Tombstone removed, depth1 visible

        map.pop_scope();
        REQUIRE(map.get("VAR") == "depth0"); // Back to root
    }
}

TEST_CASE("ShadowMap PARENT_SCOPE", "[shadow_map]") {
    ShadowMap map;

    SECTION("Set parent scope from nested scope") {
        map.set("VAR", "original");

        map.push_scope();
        auto result = map.set_parent_scope("VAR", "modified_parent");
        REQUIRE(result.has_value());
        REQUIRE(result.value() == true); // Replaced existing

        // CMake semantics: PARENT_SCOPE does NOT affect current scope's view
        // The current scope sees the snapshotted "original" value
        REQUIRE(map.get("VAR") == "original");

        map.pop_scope();
        REQUIRE(map.get("VAR") == "modified_parent"); // Parent was modified
    }

    SECTION("Set parent scope creates new variable in parent") {
        map.push_scope();
        auto result = map.set_parent_scope("NEW_VAR", "parent_value");
        REQUIRE(result.has_value());
        REQUIRE(result.value() == false); // Created new

        // CMake semantics: a new variable created via PARENT_SCOPE is NOT
        // visible in the current scope - only after exiting
        REQUIRE(map.get("NEW_VAR") == "");
        REQUIRE_FALSE(map.is_defined("NEW_VAR"));

        map.pop_scope();
        REQUIRE(map.get("NEW_VAR") == "parent_value"); // Now visible in parent
    }

    SECTION("Set parent scope with local shadow") {
        map.set("VAR", "original");

        map.push_scope();
        map.set("VAR", "local"); // Shadow it locally
        auto result = map.set_parent_scope("VAR", "modified_parent");
        REQUIRE(result.has_value());
        REQUIRE(result.value() == true); // Replaced existing

        // Local value takes precedence
        REQUIRE(map.get("VAR") == "local");

        map.pop_scope();
        REQUIRE(map.get("VAR") == "modified_parent"); // Parent was modified
    }

    SECTION("Set parent scope at root returns error") {
        auto result = map.set_parent_scope("VAR", "value");
        REQUIRE_FALSE(result.has_value()); // Error - no parent scope
        REQUIRE_FALSE(map.is_defined("VAR"));
    }

    SECTION("Set parent scope multiple levels deep") {
        map.set("VAR", "depth0");

        map.push_scope(); // depth 1
        map.set("VAR", "depth1");

        map.push_scope(); // depth 2
        auto result = map.set_parent_scope("VAR", "modified_depth1");
        REQUIRE(result.has_value());
        REQUIRE(result.value() == true); // Replaced existing

        // CMake semantics: depth2 sees the snapshotted value ("depth1") from
        // before PARENT_SCOPE was called, NOT the modified parent value
        REQUIRE(map.get("VAR") == "depth1");

        map.pop_scope();                              // back to depth 1
        REQUIRE(map.get("VAR") == "modified_depth1"); // depth1 has new value

        map.pop_scope();                     // back to depth 0
        REQUIRE(map.get("VAR") == "depth0"); // depth0 unchanged
    }
}

TEST_CASE("ShadowMap deep nesting", "[shadow_map]") {
    ShadowMap map;

    SECTION("10 levels of nesting") {
        const int DEPTH = 10;

        // Set at each depth
        for (int i = 0; i < DEPTH; ++i) {
            map.push_scope();
            map.set("VAR", "value" + std::to_string(i));
            REQUIRE(map.depth() == i + 1);
        }

        REQUIRE(map.get("VAR") == "value9");

        // Pop back to root
        for (int i = DEPTH - 1; i >= 0; --i) {
            map.pop_scope();
            if (i > 0) { REQUIRE(map.get("VAR") == "value" + std::to_string(i - 1)); }
        }

        REQUIRE(map.depth() == 0);
        REQUIRE_FALSE(map.is_defined("VAR"));
    }

    SECTION("Many variables at many depths") {
        for (int depth = 0; depth < 5; ++depth) {
            map.push_scope();
            for (int var = 0; var < 10; ++var) {
                std::string name = "VAR" + std::to_string(var);
                std::string value = "d" + std::to_string(depth) + "_v" + std::to_string(var);
                map.set(name, value);
            }
        }

        // Verify all variables at depth 5
        for (int var = 0; var < 10; ++var) {
            std::string name = "VAR" + std::to_string(var);
            std::string expected = "d4_v" + std::to_string(var);
            REQUIRE(map.get(name) == expected);
        }

        // Pop all scopes
        for (int depth = 0; depth < 5; ++depth) { map.pop_scope(); }

        // All variables should be gone
        for (int var = 0; var < 10; ++var) {
            std::string name = "VAR" + std::to_string(var);
            REQUIRE_FALSE(map.is_defined(name));
        }
    }
}

TEST_CASE("ShadowMap mixed operations", "[shadow_map]") {
    ShadowMap map;

    SECTION("Complex scenario with shadowing, unset, and parent scope") {
        // Root level
        map.set("VAR1", "root1");
        map.set("VAR2", "root2");

        // Level 1
        map.push_scope();
        map.set("VAR1", "level1_1"); // Shadow VAR1
        map.set("VAR3", "level1_3"); // New variable

        // Level 2
        map.push_scope();
        map.set("VAR2", "level2_2");                                     // Shadow VAR2
        auto result = map.set_parent_scope("VAR1", "modified_level1_1"); // Modify parent's VAR1
        REQUIRE(result.has_value());
        REQUIRE(result.value() == true); // Replaced existing

        // CMake semantics: PARENT_SCOPE does NOT affect current scope's view
        // Level 2 sees snapshotted value from before the PARENT_SCOPE call
        REQUIRE(map.get("VAR1") == "level1_1"); // Snapshotted value at level 2
        REQUIRE(map.get("VAR2") == "level2_2"); // Shadowed
        REQUIRE(map.get("VAR3") == "level1_3"); // From parent

        map.unset("VAR2");              // Remove shadow, tombstone masks root
        REQUIRE(map.get("VAR2") == ""); // Masked by tombstone
        REQUIRE_FALSE(map.is_defined("VAR2"));

        map.pop_scope(); // Back to level 1

        REQUIRE(map.get("VAR1") == "modified_level1_1"); // Was modified by child
        REQUIRE(map.get("VAR2") == "root2");             // Tombstone gone, root visible
        REQUIRE(map.get("VAR3") == "level1_3");          // Local

        map.pop_scope(); // Back to root

        REQUIRE(map.get("VAR1") == "root1");   // Original root value
        REQUIRE(map.get("VAR2") == "root2");   // Original root value
        REQUIRE_FALSE(map.is_defined("VAR3")); // Was only at level 1
    }
}

TEST_CASE("ShadowMap modification at same depth", "[shadow_map]") {
    ShadowMap map;

    SECTION("Multiple modifications at same depth don't create duplicates") {
        map.set("VAR", "value1");
        map.set("VAR", "value2");
        map.set("VAR", "value3");

        REQUIRE(map.get("VAR") == "value3");

        // Should only have one version at depth 0
        map.push_scope();
        REQUIRE(map.get("VAR") == "value3");

        map.set("VAR", "nested");
        map.pop_scope();

        REQUIRE(map.get("VAR") == "value3"); // Only one version at root
    }
}

TEST_CASE("ShadowMap edge cases", "[shadow_map]") {
    ShadowMap map;

    SECTION("Empty variable values") {
        map.set("VAR", "");
        REQUIRE(map.is_defined("VAR"));
        REQUIRE(map.get("VAR") == "");
    }

    SECTION("Variable names with special characters") {
        map.set("CMAKE_CXX_FLAGS", "-std=c++23");
        map.set("MY_VAR_123", "value");
        map.set("_UNDERSCORE", "test");

        REQUIRE(map.get("CMAKE_CXX_FLAGS") == "-std=c++23");
        REQUIRE(map.get("MY_VAR_123") == "value");
        REQUIRE(map.get("_UNDERSCORE") == "test");
    }

    SECTION("Pop scope at root is safe") {
        map.pop_scope(); // Should not crash
        REQUIRE(map.depth() == 0);
    }

    SECTION("Multiple push/pop cycles") {
        for (int i = 0; i < 5; ++i) {
            map.push_scope();
            map.set("VAR", "cycle" + std::to_string(i));
            REQUIRE(map.get("VAR") == "cycle" + std::to_string(i));
            map.pop_scope();
            REQUIRE_FALSE(map.is_defined("VAR"));
        }
    }
}

TEST_CASE("ShadowMap try_get", "[shadow_map]") {
    ShadowMap map;

    SECTION("Returns nullptr for missing variable") {
        REQUIRE(map.try_get("MISSING") == nullptr);
    }

    SECTION("Returns pointer to existing variable") {
        map.set("VAR", "hello");
        auto* val = map.try_get("VAR");
        REQUIRE(val != nullptr);
        REQUIRE(*val == "hello");
    }

    SECTION("Returns nullptr for tombstoned variable") {
        map.set("VAR", "outer");
        map.push_scope();
        map.set("VAR", "inner");
        map.unset("VAR");
        REQUIRE(map.try_get("VAR") == nullptr);
        map.pop_scope();
        REQUIRE(*map.try_get("VAR") == "outer");
    }

    SECTION("Returns pointer to empty string value") {
        map.set("VAR", "");
        auto* val = map.try_get("VAR");
        REQUIRE(val != nullptr);
        REQUIRE(*val == "");
    }
}

TEST_CASE("ShadowMap ConstEntry", "[shadow_map]") {
    ShadowMap map;

    SECTION("const_entry returns nullopt for missing variable") {
        auto entry = map.const_entry("MISSING");
        REQUIRE_FALSE(entry.has_value());
    }

    SECTION("const_entry reads existing variable") {
        map.set("VAR", "hello");
        auto entry = map.const_entry("VAR");
        REQUIRE(entry.has_value());
        REQUIRE(entry->is_defined());
        REQUIRE(entry->get() == "hello");
    }

    SECTION("ConstEntry sees mutations from set()") {
        map.set("VAR", "before");
        auto entry = map.const_entry("VAR");
        REQUIRE(entry->get() == "before");

        map.set("VAR", "after");
        REQUIRE(entry->get() == "after");
    }

    SECTION("ConstEntry survives scope changes") {
        map.set("VAR", "outer");
        auto entry = map.const_entry("VAR");

        map.push_scope();
        map.set("VAR", "inner");
        REQUIRE(entry->get() == "inner");

        map.pop_scope();
        REQUIRE(entry->get() == "outer");
    }

    SECTION("ConstEntry sees tombstone from unset") {
        map.set("VAR", "value");
        auto entry = map.const_entry("VAR");
        REQUIRE(entry->is_defined());

        map.push_scope();
        map.unset("VAR");
        REQUIRE_FALSE(entry->is_defined());
        REQUIRE(entry->get() == "");

        map.pop_scope();
        REQUIRE(entry->is_defined());
        REQUIRE(entry->get() == "value");
    }
}

TEST_CASE("ShadowMap Entry", "[shadow_map]") {
    ShadowMap map;

    SECTION("Entry set and get") {
        auto e = map.entry("VAR");
        REQUIRE_FALSE(e.is_defined());

        e.set("hello");
        REQUIRE(e.is_defined());
        REQUIRE(e.get() == "hello");
        REQUIRE(map.get("VAR") == "hello");
    }

    SECTION("Entry set at same depth modifies in place") {
        auto e = map.entry("VAR");
        e.set("first");
        e.set("second");
        e.set("third");
        REQUIRE(e.get() == "third");
        REQUIRE(map.get("VAR") == "third");
    }

    SECTION("Entry set at different depth pushes version") {
        map.set("VAR", "depth0");
        map.push_scope();
        auto e = map.entry("VAR");
        REQUIRE(e.get() == "depth0");

        e.set("depth1");
        REQUIRE(e.get() == "depth1");

        map.pop_scope();
        REQUIRE(map.get("VAR") == "depth0");
    }

    SECTION("Entry unset with tombstone") {
        map.set("VAR", "outer");
        map.push_scope();
        auto e = map.entry("VAR");
        e.set("inner");
        e.unset();
        REQUIRE_FALSE(e.is_defined());
        REQUIRE(e.get() == "");

        map.pop_scope();
        REQUIRE(map.get("VAR") == "outer");
    }

    SECTION("Entry loop pattern — amortized single lookup") {
        auto e = map.entry("LOOP_VAR");
        for (int i = 0; i < 100; ++i) {
            e.set(std::to_string(i));
            REQUIRE(e.get() == std::to_string(i));
        }
        REQUIRE(map.get("LOOP_VAR") == "99");
    }

    SECTION("Entry implicit conversion to ConstEntry") {
        map.set("VAR", "value");
        auto e = map.entry("VAR");
        ShadowMap::ConstEntry ce = e;
        REQUIRE(ce.is_defined());
        REQUIRE(ce.get() == "value");

        // ConstEntry still sees mutations via Entry
        e.set("new_value");
        REQUIRE(ce.get() == "new_value");
    }

    SECTION("Entry and ConstEntry survive insertion of other variables") {
        map.set("VAR1", "val1");
        auto e = map.entry("VAR1");
        auto ce = map.const_entry("VAR1");

        // Insert many other variables to potentially trigger rehash
        for (int i = 0; i < 1000; ++i) { map.set("OTHER_" + std::to_string(i), "x"); }

        // Original handles still valid (reference stability guarantee)
        REQUIRE(e.get() == "val1");
        REQUIRE(ce->get() == "val1");

        e.set("modified");
        REQUIRE(e.get() == "modified");
        REQUIRE(ce->get() == "modified");
        REQUIRE(map.get("VAR1") == "modified");
    }

    SECTION("Entry visible through ShadowMap::get and is_defined") {
        auto e = map.entry("VAR");
        e.set("test");
        REQUIRE(map.is_defined("VAR"));
        REQUIRE(map.get("VAR") == "test");
    }
}
