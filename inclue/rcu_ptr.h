#pragma once

#include <memory>
#include <atomic>

template <typename T>
class rcu_ptr {
    std::shared_ptr<const T> sp;

public:

    rcu_ptr() = default;
    ~rcu_ptr() = default;

    rcu_ptr(const rcu_ptr&)            = delete;
    rcu_ptr& operator=(const rcu_ptr&) = delete;
    rcu_ptr(rcu_ptr&&)                 = delete;
    rcu_ptr& operator=(rcu_ptr&&)      = delete;
    explicit rcu_ptr(const std::shared_ptr<const T>& sp_) : sp(sp_) {}
    explicit rcu_ptr(std::shared_ptr<const T>&& sp_)      : sp(std::move(sp_)) {}

    explicit rcu_ptr(const std::shared_ptr<T>& sp_)
        : sp(std::const_pointer_cast<const T>(sp_)) {}

    std::shared_ptr<const T> read() const {
        return std::atomic_load_explicit(&sp, std::memory_order_acquire);
    }

 
    void reset(const std::shared_ptr<const T>& r) {
        std::atomic_store_explicit(&sp, r, std::memory_order_release);
    }
    void reset(std::shared_ptr<const T>&& r) {
        std::atomic_store_explicit(&sp, std::move(r), std::memory_order_release);
    }

    template <typename Func>
    void copy_update(Func&& fun) {
        // Step 1: load current value
        std::shared_ptr<const T> sp_l =
            std::atomic_load_explicit(&sp, std::memory_order_acquire);

        std::shared_ptr<T> r;
        do {
            if (sp_l) {
                // Step 2: deep copy (copy constructor of T)
                r = std::make_shared<T>(*sp_l);
            } else {
                r = std::make_shared<T>();
            }

            std::forward<Func>(fun)(r.get());
        } while (!std::atomic_compare_exchange_strong_explicit(
            &sp,
            &sp_l,
            std::shared_ptr<const T>(std::move(r)),
            std::memory_order_release,
            std::memory_order_acquire));
    }

    bool is_null() const {
        return read() == nullptr;
    }
};
