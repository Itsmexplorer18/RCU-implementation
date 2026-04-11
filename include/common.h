#pragma once


#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>


#define BARRIER()        asm volatile("" ::: "memory")

#define smp_mb()         __atomic_thread_fence(__ATOMIC_SEQ_CST)

#define smp_rmb()        __atomic_thread_fence(__ATOMIC_ACQUIRE)

#define smp_wmb()        __atomic_thread_fence(__ATOMIC_RELEASE)

template <typename T>
inline void rcu_assign_pointer(T*& dst, T* src)
{
    __atomic_store_n(&dst, src, __ATOMIC_RELEASE);
}

template <typename T>
inline T* rcu_dereference(T* const& src)
{
    return __atomic_load_n(&src, __ATOMIC_CONSUME);
}


struct RcuCallback {
    std::function<void()> fn;
    RcuCallback*          next = nullptr;
};

