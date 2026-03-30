#pragma once

#include "rcu_ptr.h"
#include <vector>
#include <optional>
#include <functional>

template <typename Key, typename Value>
class rcu_list {
    using Pair     = std::pair<Key, Value>;
    using ListType = std::vector<Pair>;

    rcu_ptr<ListType> data;

public:
    rcu_list() {
        // Initialize with an empty list
        data.reset(std::make_shared<const ListType>());
    }


    std::optional<Value> lookup(const Key& key) const {
        auto snapshot = data.read();

        if (!snapshot) return std::nullopt;

        for (const auto& [k, v] : *snapshot) {
            if (k == key) {
                return v;
            }
        }
        return std::nullopt;
    }

    void insert(const Key& key, const Value& value) {
        data.copy_update([&key, &value](ListType* copy) {
            // Remove existing entry with same key first (upsert semantics)
            auto it = std::find_if(copy->begin(), copy->end(),
                [&key](const Pair& p) { return p.first == key; });
            if (it != copy->end()) {
                copy->erase(it);
            }
            copy->emplace_back(key, value);
        });
    }

    bool remove(const Key& key) {
        bool found = false;
        data.copy_update([&key, &found](ListType* copy) {
            auto it = std::find_if(copy->begin(), copy->end(),
                [&key](const Pair& p) { return p.first == key; });
            if (it != copy->end()) {
                copy->erase(it);
                found = true;
            }
        });
        return found;
    }

    size_t size() const {
        auto snapshot = data.read();
        return snapshot ? snapshot->size() : 0;
    }


    void for_each(std::function<void(const Key&, const Value&)> fn) const {
        auto snapshot = data.read();
        if (!snapshot) return;
        for (const auto& [k, v] : *snapshot) {
            fn(k, v);
        }
    }
};
