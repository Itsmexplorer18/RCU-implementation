//general purpose rcu implementation
#pragma once

#include "rcu_common.h"
#include <pthread.h>
#include <sched.h>
#include <cassert>
#include <vector>
#include <mutex>
#include <climits>

namespace gp_rcu {

struct ThreadState {
    alignas(64) std::atomic<uint64_t> gp_snap{UINT64_MAX};
    int      nesting{0};
    uint64_t snap_value{0};
    bool     online{true};
};

class RcuDomain {
public:
    alignas(64) std::atomic<uint64_t> global_gp{1};
    std::mutex               registry_lock;
    std::vector<ThreadState*> threads;

    thread_local static ThreadState* tls_state;

    void thread_register() {
        ThreadState* s = new ThreadState();
        std::lock_guard<std::mutex> lg(registry_lock);
        threads.push_back(s);
        tls_state = s;
    }

    void thread_unregister() {
        assert(tls_state != nullptr);
        std::lock_guard<std::mutex> lg(registry_lock);
        tls_state->online = false;
        tls_state->gp_snap.store(UINT64_MAX, std::memory_order_release);
        tls_state = nullptr;
    }

    void read_lock() {
        assert(tls_state != nullptr);
        ThreadState* s = tls_state;
        if (s->nesting++ == 0) {
            s->snap_value = global_gp.load(std::memory_order_acquire);
            s->gp_snap.store(s->snap_value, std::memory_order_release);
        }
    }

    void read_unlock() {
        assert(tls_state != nullptr);
        ThreadState* s = tls_state;
        if (--s->nesting == 0)
            s->gp_snap.store(UINT64_MAX, std::memory_order_release);
    }

    void synchronize_rcu() {
        uint64_t target = global_gp.fetch_add(1, std::memory_order_seq_cst);
        smp_mb();

        std::lock_guard<std::mutex> lg(registry_lock);
        for (ThreadState* s : threads) {
            if (!s->online) continue;
            while (true) {
                uint64_t snap = s->gp_snap.load(std::memory_order_acquire);
                if (snap == UINT64_MAX || snap > target) break;
                sched_yield();
            }
        }
        smp_mb();
    }

    template<typename T>
    void defer_free(T* ptr) {
        synchronize_rcu();
        delete ptr;
    }
};

inline thread_local ThreadState* RcuDomain::tls_state = nullptr;

} // namespace gp_rcu

