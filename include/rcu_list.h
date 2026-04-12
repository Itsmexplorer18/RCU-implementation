#pragma once

#include "rcu_common.h"
#include <functional>
#include <optional>
#include <mutex>

template<typename T>
struct RcuNode {
    T        value;
    RcuNode* next{nullptr};
    explicit RcuNode(T v) : value(std::move(v)) {}
};

template<typename T, typename RcuImpl>
class RcuList {
public:
    using Node = RcuNode<T>;

    RcuList() : head_(nullptr) {}

    ~RcuList() {
        Node* n = head_.load(std::memory_order_relaxed);
        while (n) { Node* nx = n->next; delete n; n = nx; }
    }

    void insert(T value) {
        Node* node = new Node(std::move(value));
        Node* old_head;
        do {
            old_head   = head_.load(std::memory_order_relaxed);
            node->next = old_head;
        } while (!head_.compare_exchange_weak(old_head, node,
                                              std::memory_order_release,
                                              std::memory_order_relaxed));
    }

   
    bool remove(const T& value, RcuImpl& rcu) {
        std::unique_lock<std::mutex> ul(update_lock_);

        std::atomic<Node*>* indirect_atomic = &head_;
        Node* cur = head_.load(std::memory_order_relaxed);

        while (cur != nullptr) {
            if (cur->value == value) {
                indirect_atomic->store(cur->next, std::memory_order_release);
                ul.unlock();

                rcu.synchronize_rcu();
                delete cur;
                return true;
            }
            
            indirect_atomic = reinterpret_cast<std::atomic<Node*>*>(&cur->next);
            cur = cur->next;
        }
        return false;
    }

    std::optional<T> find(const T& value) const {
        Node* cur = head_.load(std::memory_order_acquire);
        while (cur) {
            if (cur->value == value) return cur->value;
            cur = __atomic_load_n(&cur->next, __ATOMIC_CONSUME);
        }
        return std::nullopt;
    }

    void for_each(std::function<void(const T&)> fn) const {
        Node* cur = head_.load(std::memory_order_acquire);
        while (cur) {
            fn(cur->value);
            cur = __atomic_load_n(&cur->next, __ATOMIC_CONSUME);
        }
    }

    size_t size_unsafe() const {
        size_t n = 0;
        Node* cur = head_.load(std::memory_order_relaxed);
        while (cur) { ++n; cur = cur->next; }
        return n;
    }

private:
    alignas(64) std::atomic<Node*> head_;
    std::mutex                     update_lock_;
};
