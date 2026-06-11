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
    explicit RcuList(RcuImpl& rcu) : head_(nullptr), rcu_(rcu) {}
    using Node = RcuNode<T>;

    //RcuList() : head_(nullptr) {}

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

   
    // bool remove(const T& value, RcuImpl& rcu) {
    //     std::unique_lock<std::mutex> ul(update_lock_);

    //     std::atomic<Node*>* indirect_atomic = &head_;
    //     Node* cur = head_.load(std::memory_order_relaxed);

    //     while (cur != nullptr) {
    //         if (cur->value == value) {
    //             indirect_atomic->store(cur->next, std::memory_order_release);
    //             ul.unlock();

    //             rcu.synchronize_rcu();
    //             delete cur;
    //             return true;
    //         }
            
    //         indirect_atomic = reinterpret_cast<std::atomic<Node*>*>(&cur->next);
    //         cur = cur->next;
    //     }
    //     return false;
    // }
bool remove(const T& value) {
    std::unique_lock<std::mutex> ul(update_lock_);
    Node* prev = nullptr;
    Node* cur  = head_.load(std::memory_order_relaxed);
    while (cur != nullptr) {
        if (cur->value == value) {
            if (prev == nullptr)
                head_.store(cur->next, std::memory_order_release);
            else
                prev->next = cur->next;  // writers are serialized by update_lock_
            ul.unlock();
            rcu_.synchronize_rcu();
            delete cur;
            return true;
        }
        prev = cur;
        cur  = cur->next;
    }
    return false;
}
bool update(const T& old_value, T new_value) {
    std::unique_lock<std::mutex> ul(update_lock_);
    Node* prev = nullptr;
    Node* cur  = head_.load(std::memory_order_relaxed);
    while (cur != nullptr) {
        if (cur->value == old_value) {
            Node* new_node = new Node(std::move(new_value));
            new_node->next = cur->next;
            if (prev == nullptr)
                head_.store(new_node, std::memory_order_release);
            else
                prev->next = new_node;
            ul.unlock();
            rcu_.synchronize_rcu();
            delete cur;
            return true;
        }
        prev = cur;
        cur  = cur->next;
    }
    return false;
}

    std::optional<T> find(const T& value) const {
    rcu_.read_lock();
        Node* cur = head_.load(std::memory_order_acquire);
        while (cur) {
            if (cur->value == value) {
                rcu_.read_unlock();
                return cur->value;
            }
            cur = __atomic_load_n(&cur->next, __ATOMIC_ACQUIRE);
        }
       rcu_.read_unlock();
        return std::nullopt;
    }

    void for_each(std::function<void(const T&)> fn) const {
       rcu_.read_lock();
    Node* cur = head_.load(std::memory_order_acquire);
    while (cur) {
        fn(cur->value);
        cur = __atomic_load_n(&cur->next, __ATOMIC_ACQUIRE);
    }
    rcu_.read_unlock();
    }

    size_t size_unsafe() const {
        size_t n = 0;
        Node* cur = head_.load(std::memory_order_relaxed);
        while (cur) { ++n; cur = cur->next; }
        return n;
    }

private:
    RcuImpl& rcu_;
    alignas(64) std::atomic<Node*> head_;
    std::mutex                     update_lock_;
};
