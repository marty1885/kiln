#pragma once
#include <expected>
#include <functional>
#include <string>
#include <vector>

namespace kiln {

// Fixed-capacity cache with clock (second-chance) eviction.
// Key must be equality-comparable and constructible from the lookup type.
// Value is move-only friendly. Factory returns expected<Value, std::string>.
//
// Not thread-safe — each call site spawns its own instance.
template <typename Key, typename Value> class ClockCache {
public:
    using Factory = std::function<std::expected<Value, std::string>(const Key&)>;

    explicit ClockCache(size_t capacity, Factory factory) : capacity_(capacity), factory_(std::move(factory)) {
        entries_.reserve(capacity);
    }

    ClockCache(const ClockCache&) = delete;
    ClockCache& operator=(const ClockCache&) = delete;

    // Returns a non-owning pointer into the cache.
    // Valid until the entry is evicted by a subsequent call.
    std::expected<const Value*, std::string> get(const Key& key) {
        // Linear scan — fine for small capacities (8-32)
        for (auto& e : entries_) {
            if (e.key == key) {
                e.clock_bit = true;
                return &e.value;
            }
        }

        auto result = factory_(key);
        if (!result) return std::unexpected(std::move(result.error()));

        // Room to grow
        if (entries_.size() < capacity_) {
            entries_.push_back({Key(key), std::move(*result), true});
            return &entries_.back().value;
        }

        // Clock eviction
        while (true) {
            auto& victim = entries_[clock_hand_];
            if (!victim.clock_bit) {
                victim.key = Key(key);
                victim.value = std::move(*result);
                victim.clock_bit = true;
                const Value* ptr = &victim.value;
                clock_hand_ = (clock_hand_ + 1) % capacity_;
                return ptr;
            }
            victim.clock_bit = false;
            clock_hand_ = (clock_hand_ + 1) % capacity_;
        }
    }

private:
    struct Entry {
        Key key;
        Value value;
        bool clock_bit;
    };

    std::vector<Entry> entries_;
    size_t capacity_;
    size_t clock_hand_ = 0;
    Factory factory_;
};

} // namespace kiln
