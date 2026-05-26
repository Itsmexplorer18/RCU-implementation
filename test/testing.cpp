
#include "rcu_gp.h"
#include "rcu_list.h"

#include <shared_mutex>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <iostream>
#include <iomanip>
#include <string>
#include <cassert>
#include <functional>
#include <optional>


static int g_pass = 0;
static int g_fail = 0;

void check(bool cond, const std::string& name) {
    if (cond) {
        std::cout << "  PASS  " << name << "\n";
        ++g_pass;
    } else {
        std::cout << "  FAIL  " << name << "\n";
        ++g_fail;
    }
}

void section(const std::string& name) {
    std::cout << "\n── " << name << " ──\n";
}


using Domain = gp_rcu::RcuDomain;
using List   = RcuList<int, Domain>;

// Spawn N threads, each calling thread_register, fn(tid), thread_unregister.
void run_threads(Domain& dom, int N, std::function<void(int)> fn) {
    std::vector<std::thread> ts;
    ts.reserve(N);
    for (int i = 0; i < N; ++i)
        ts.emplace_back([&dom, &fn, i] {
            dom.thread_register();
            fn(i);
            dom.thread_unregister();
        });
    for (auto& t : ts) t.join();
}

void t1_basic_read_lock() {
    section("T1: basic read_lock / read_unlock");

    Domain dom;
    dom.thread_register();
    auto* s = Domain::tls_state;

    check(s->nesting == 0,          "nesting starts at 0");
    check(s->gp_snap == UINT64_MAX, "gp_snap starts as UINT64_MAX (idle)");

    dom.read_lock();
    check(s->nesting == 1,          "nesting = 1 after read_lock");
    check(s->gp_snap != UINT64_MAX, "gp_snap set after read_lock");

    dom.read_unlock();
    check(s->nesting == 0,          "nesting = 0 after read_unlock");
    check(s->gp_snap == UINT64_MAX, "gp_snap reset to UINT64_MAX after unlock");

    dom.thread_unregister();
}

void t2_nesting() {
    section("T2: nested read_lock");

    Domain dom;
    dom.thread_register();
    auto* s = Domain::tls_state;

    dom.read_lock();
    uint64_t snap1 = s->gp_snap.load();

    dom.read_lock();
    check(s->nesting == 2,                  "nesting = 2 after second lock");
    check(s->gp_snap.load() == snap1,       "gp_snap unchanged on nested lock");

    dom.read_lock();
    check(s->nesting == 3,                  "nesting = 3 after third lock");
    check(s->gp_snap.load() == snap1,       "gp_snap unchanged on third lock");

    dom.read_unlock();
    check(s->nesting == 2,                  "nesting = 2 after first unlock");
    check(s->gp_snap != UINT64_MAX,         "still inside CS after first unlock");

    dom.read_unlock();
    check(s->nesting == 1,                  "nesting = 1 after second unlock");

    dom.read_unlock();
    check(s->nesting == 0,                  "nesting = 0 fully unlocked");
    check(s->gp_snap == UINT64_MAX,         "gp_snap reset after full unlock");

    dom.thread_unregister();
}

void t3_sync_waits_for_reader() {
    section("T3: synchronize_rcu waits for single active reader");

    Domain dom;
    std::atomic<bool> reader_inside{false};
    std::atomic<bool> reader_exited{false};

    std::thread reader([&] {
        dom.thread_register();
        dom.read_lock();
        reader_inside = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        dom.read_unlock();
        reader_exited = true;
        dom.thread_unregister();
    });

    // Wait until reader is holding the lock, then time synchronize_rcu.
    dom.thread_register();
    while (!reader_inside) std::this_thread::yield();
    auto t0 = std::chrono::steady_clock::now();
    dom.synchronize_rcu();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - t0).count();
    dom.thread_unregister();

    reader.join();

    check(reader_exited.load(), "reader exited before synchronize_rcu returned");
    check(elapsed >= 50,
          "synchronize_rcu blocked >= 50ms (reader held 80ms), got "
          + std::to_string(elapsed) + "ms");
}

void t4_sync_waits_for_all_readers() {
    section("T4: synchronize_rcu waits for all N readers");

    const int N = 8;
    Domain dom;
    std::atomic<int>  readers_inside{0};
    std::atomic<int>  readers_exited{0};

    std::vector<std::thread> readers;
    for (int i = 0; i < N; ++i)
        readers.emplace_back([&] {
            dom.thread_register();
            dom.read_lock();
            ++readers_inside;
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            dom.read_unlock();
            ++readers_exited;
            dom.thread_unregister();
        });

    dom.thread_register();
    while (readers_inside.load() < N) std::this_thread::yield();
    dom.synchronize_rcu();
    dom.thread_unregister();

    for (auto& r : readers) r.join();

    check(readers_exited.load() == N,
          "all " + std::to_string(N) + " readers exited before synchronize_rcu returned");
}

void t5_new_generation_reader_does_not_block() {
    section("T5: reader starting after GP increment does not block grace period");

    Domain dom;
    std::atomic<bool> sync_started{false};
    std::atomic<bool> late_reader_inside{false};

    // Late reader: waits for the updater to signal it has called fetch_add,
    // THEN enters the read section. This reader is in the new generation.
    std::thread late_reader([&] {
        dom.thread_register();
        while (!sync_started) std::this_thread::yield();
        // Small sleep to let global_gp increment propagate.
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        dom.read_lock();
        late_reader_inside = true;
        // Hold for 100ms — should NOT block the grace period.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        dom.read_unlock();
        dom.thread_unregister();
    });

    dom.thread_register();
    // Signal the late reader just before we bump global_gp.
    // synchronize_rcu internally does fetch_add then inspects snaps,
    // so we signal here and call immediately.
    sync_started = true;
    auto t0 = std::chrono::steady_clock::now();
    dom.synchronize_rcu();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - t0).count();
    dom.thread_unregister();

    late_reader.join();

    // Grace period must complete well before the late reader exits (it holds
    // 100ms). Allow 60ms for the synchronize_rcu to finish.
    check(elapsed < 60,
          "synchronize_rcu completed quickly despite new-generation reader ("
          + std::to_string(elapsed) + "ms, reader held 100ms)");
}

void t6_consecutive_grace_periods() {
    section("T6: consecutive grace periods — reader from GP1 does not block GP2");

    Domain dom;
    std::atomic<bool> gp1_done{false};

    // Reader active only during GP1
    std::thread reader([&] {
        dom.thread_register();
        dom.read_lock();
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        dom.read_unlock();
        // Reader exits here — before GP2 starts
        dom.thread_unregister();
    });

    dom.thread_register();

    // GP1: reader is active, should take ~40ms
    dom.synchronize_rcu();
    gp1_done = true;

    // GP2: reader is gone, should be near-instant
    auto t0 = std::chrono::steady_clock::now();
    dom.synchronize_rcu();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - t0).count();

    dom.thread_unregister();
    reader.join();

    check(gp1_done.load(), "GP1 completed");
    check(elapsed < 30,
          "GP2 completed quickly with no active readers ("
          + std::to_string(elapsed) + "ms)");
}
void t7_remove_unlinks() {
    section("T7: remove unlinks node");

    Domain dom;
    dom.thread_register();

    List list;
    list.insert(10);
    list.insert(20);
    list.insert(30);

    check(list.size_unsafe() == 3,   "size = 3 after 3 inserts");
    check(list.find(20).has_value(), "find(20) before remove");

    bool removed = list.remove(20, dom);
    check(removed,                    "remove(20) returns true");
    check(!list.find(20).has_value(), "find(20) after remove returns nullopt");
    check(list.size_unsafe() == 2,    "size = 2 after remove");

    bool removed_again = list.remove(20, dom);
    check(!removed_again,             "remove(20) second time returns false");

    dom.thread_unregister();
}

void t8_find_missing() {
    section("T8: find returns nullopt for missing value");

    Domain dom;
    dom.thread_register();

    List list;
    list.insert(1);
    list.insert(2);
    list.insert(3);

    check(!list.find(99).has_value(), "find(99) = nullopt");
    check( list.find(2).has_value(),  "find(2) found");
    check(*list.find(3) == 3,         "find(3) value correct");

    dom.thread_unregister();
}

void t9_insert_find() {
    section("T9: insert and find 100 values");

    Domain dom;
    dom.thread_register();

    List list;
    for (int i = 0; i < 100; ++i) list.insert(i);

    bool all_found = true;
    for (int i = 0; i < 100; ++i)
        if (!list.find(i).has_value()) { all_found = false; break; }

    check(all_found,               "all 100 inserted values found");
    check(list.size_unsafe() == 100, "size = 100");

    dom.thread_unregister();
}

void t10_concurrent_inserts() {
    section("T10: concurrent inserts from 8 threads");

    const int THREADS = 8;
    const int PER     = 1000;

    Domain dom;
    List   list;

    run_threads(dom, THREADS, [&](int tid) {
        int base = tid * PER;
        for (int i = 0; i < PER; ++i) list.insert(base + i);
    });

    dom.thread_register();
    int found = 0;
    for (int i = 0; i < THREADS * PER; ++i)
        if (list.find(i).has_value()) ++found;
    dom.thread_unregister();

    check(found == THREADS * PER,
          "all " + std::to_string(THREADS * PER) + " values found, got "
          + std::to_string(found));
}

void t11_concurrent_find_during_remove() {
    section("T11: concurrent find during remove — no use-after-free");

    Domain dom;
    List   list;

    // Pre-populate
    for (int i = 0; i < 50; ++i) list.insert(i);

    std::atomic<bool> stop{false};
    std::atomic<bool> bad{false};

    const int READERS  = 6;
    const int DURATION = 300; // ms

    std::vector<std::thread> readers;
    for (int i = 0; i < READERS; ++i) {
        readers.emplace_back([&] {
            dom.thread_register();
            while (!stop.load(std::memory_order_relaxed)) {
                dom.read_lock();
                // search for values the writer is cycling through
                for (int v = 0; v < 50; ++v) {
                    auto r = list.find(v);
                    (void)r;
                }
                dom.read_unlock();
            }
            dom.thread_unregister();
        });
    }

    // Single writer: repeatedly remove and re-insert values
    std::thread writer([&] {
        dom.thread_register();
        auto deadline = std::chrono::steady_clock::now()
                      + std::chrono::milliseconds(DURATION);
        while (std::chrono::steady_clock::now() < deadline) {
            for (int v = 0; v < 50; ++v) {
                list.remove(v, dom);
                list.insert(v);
            }
        }
        stop = true;
        dom.thread_unregister();
    });

    writer.join();
    for (auto& r : readers) r.join();

    check(!bad.load(), "no use-after-free detected during concurrent find+remove");
    // Structural check: all values should be back in the list
    dom.thread_register();
    int found = 0;
    for (int v = 0; v < 50; ++v)
        if (list.find(v).has_value()) ++found;
    dom.thread_unregister();
    check(found == 50, "all 50 values present after cycling, found " + std::to_string(found));
}
══════════════════════════════════════════════════════════════════════════

void t12_remove_positions() {
    section("T12: remove head, middle, tail nodes");

    Domain dom;
    dom.thread_register();

    // insert builds list: 5 -> 4 -> 3 -> 2 -> 1 (head is last inserted)
    List list;
    for (int i = 1; i <= 5; ++i) list.insert(i);
    // head = 5

    // remove head (5)
    bool r1 = list.remove(5, dom);
    check(r1,                           "remove head (5) returns true");
    check(!list.find(5).has_value(),    "head (5) not found after remove");
    check(list.size_unsafe() == 4,      "size = 4 after head remove");

    // remove tail (1)
    bool r2 = list.remove(1, dom);
    check(r2,                           "remove tail (1) returns true");
    check(!list.find(1).has_value(),    "tail (1) not found after remove");
    check(list.size_unsafe() == 3,      "size = 3 after tail remove");

    // remove middle (3)
    bool r3 = list.remove(3, dom);
    check(r3,                           "remove middle (3) returns true");
    check(!list.find(3).has_value(),    "middle (3) not found after remove");
    check(list.size_unsafe() == 2,      "size = 2 after middle remove");

    // remaining: 4, 2
    check(list.find(4).has_value(),     "4 still present");
    check(list.find(2).has_value(),     "2 still present");

    dom.thread_unregister();
}

struct PoisonInt {
    std::atomic<int> value;
    explicit PoisonInt(int v) : value(v) {}
};
static constexpr int POISON_VAL = 0x0BADBEEF;

void t13_no_use_after_free() {
    section("T13: concurrent readers + updater — no use-after-free");

    Domain dom;
    std::atomic<PoisonInt*> shared_ptr{new PoisonInt(1)};
    std::atomic<bool>       stop{false};
    std::atomic<bool>       bad_read{false};

    const int READERS  = 6;
    const int DURATION = 500; // ms

    std::vector<std::thread> readers;
    for (int i = 0; i < READERS; ++i) {
        readers.emplace_back([&] {
            dom.thread_register();
            while (!stop.load(std::memory_order_relaxed)) {
                dom.read_lock();
                PoisonInt* p = shared_ptr.load(std::memory_order_acquire);
                int v = p->value.load(std::memory_order_relaxed);
                dom.read_unlock();
                if (v == POISON_VAL) bad_read = true;
            }
            dom.thread_unregister();
        });
    }

    std::thread updater([&] {
        dom.thread_register();
        int gen = 2;
        auto deadline = std::chrono::steady_clock::now()
                      + std::chrono::milliseconds(DURATION);
        while (std::chrono::steady_clock::now() < deadline) {
            PoisonInt* old_p = shared_ptr.load(std::memory_order_relaxed);
            PoisonInt* new_p = new PoisonInt(gen++);
            shared_ptr.store(new_p, std::memory_order_release);
            dom.synchronize_rcu();
            old_p->value.store(POISON_VAL, std::memory_order_relaxed);
            delete old_p;
        }
        stop = true;
        dom.thread_unregister();
    });

    for (auto& r : readers) r.join();
    updater.join();

    dom.thread_register();
    delete shared_ptr.load();
    dom.thread_unregister();

    check(!bad_read.load(), "no reader saw POISON_VAL (no use-after-free)");
}


void t14_reader_sees_consistent_node() {
    section("T14: reader always sees fully initialised node");

    Domain dom;

    const int MAGIC    = 0x1234ABCD;
    const int READERS  = 6;
    const int DURATION = 300; // ms

    // list contains only MAGIC values
    List list;

    std::atomic<bool> stop{false};
    std::atomic<bool> bad{false};

    std::vector<std::thread> readers;
    for (int i = 0; i < READERS; ++i) {
        readers.emplace_back([&] {
            dom.thread_register();
            while (!stop.load(std::memory_order_relaxed)) {
                dom.read_lock();
                auto result = list.find(MAGIC);
                // If find returns a value it must be exactly MAGIC.
                // A partially-written node could expose 0 or garbage.
                if (result.has_value() && *result != MAGIC) bad = true;
                dom.read_unlock();
            }
            dom.thread_unregister();
        });
    }

    std::thread writer([&] {
        dom.thread_register();
        auto deadline = std::chrono::steady_clock::now()
                      + std::chrono::milliseconds(DURATION);
        while (std::chrono::steady_clock::now() < deadline) {
            list.insert(MAGIC);
            list.remove(MAGIC, dom);
        }
        stop = true;
        dom.thread_unregister();
    });

    writer.join();
    for (auto& r : readers) r.join();

    check(!bad.load(), "no reader observed a partially-initialised node");
}
void t15_slow_reader_unblocks_sync() {
    section("T15: synchronize_rcu unblocks after slow reader exits");

    Domain dom;
    std::atomic<bool> reader_inside{false};

    std::thread reader([&] {
        dom.thread_register();
        dom.read_lock();
        reader_inside = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        dom.read_unlock();
        // thread_unregister must not be blocked by registry_lock
        dom.thread_unregister();
    });

    dom.thread_register();
    while (!reader_inside) std::this_thread::yield();

    std::atomic<bool> sync_done{false};
    std::thread syncer([&] {
        dom.synchronize_rcu();
        sync_done = true;
    });

    // Give up to 3 seconds
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::seconds(3);
    while (!sync_done.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    dom.thread_unregister();
    syncer.join();
    reader.join();

    check(sync_done.load(),
          "synchronize_rcu returned after slow reader exited (no deadlock)");
}

void t16_register_unregister() {
    section("T16: thread register / unregister");

    Domain dom;

    run_threads(dom, 4, [&](int) {
        dom.read_lock();
        dom.read_lock();   // nested
        dom.read_unlock();
        dom.read_unlock();
    });

    // After all threads unregistered, synchronize_rcu should be fast.
    dom.thread_register();
    auto t0 = std::chrono::steady_clock::now();
    dom.synchronize_rcu();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0).count();
    dom.thread_unregister();

    check(ms < 100,
          "synchronize_rcu fast with no active readers (" + std::to_string(ms) + "ms)");

    bool all_offline = true;
    for (auto* ts : dom.threads)
        if (ts->online) { all_offline = false; break; }
    check(all_offline, "all unregistered threads marked offline");
}

void t17_unregister_during_sync() {
    section("T17: thread can unregister while synchronize_rcu is spinning");

    Domain dom;
    std::atomic<bool> reader_inside{false};
    std::atomic<bool> unregister_done{false};

    // Slow reader — holds lock for 120ms
    std::thread reader([&] {
        dom.thread_register();
        dom.read_lock();
        reader_inside = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        dom.read_unlock();
        dom.thread_unregister();
    });

    // Third thread: registers, does nothing, then unregisters while updater spins
    std::thread bystander([&] {
        dom.thread_register();
        while (!reader_inside) std::this_thread::yield();
        // Updater has started synchronize_rcu by now (see below); try to unregister
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        dom.thread_unregister();
        unregister_done = true;
    });

    dom.thread_register();
    while (!reader_inside) std::this_thread::yield();
    // Start synchronize_rcu in background so we can check unregister_done
    std::atomic<bool> sync_done{false};
    std::thread updater([&] {
        dom.synchronize_rcu();
        sync_done = true;
    });

    // Give bystander 3 seconds to complete unregister
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!unregister_done.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

    dom.thread_unregister();
    reader.join();
    bystander.join();
    updater.join();

    check(unregister_done.load(),
          "bystander thread unregistered without deadlock while sync was spinning");
    check(sync_done.load(), "synchronize_rcu completed");
}

struct PerfResult {
    std::string name;
    double      ops_per_sec;
};

void print_perf(const PerfResult& r) {
    std::cout << "  " << std::left  << std::setw(52) << r.name
              << std::right << std::setw(14)
              << std::fixed << std::setprecision(0) << r.ops_per_sec
              << " ops/sec\n";
}

// ── P1: read-only throughput — RCU vs std::shared_mutex (pthread rwlock) ────

PerfResult p1_rcu_readers(int n_threads, int dur_ms) {
    Domain dom;
    std::atomic<uint64_t> total{0};
    std::atomic<bool>     stop{false};
    std::atomic<int>      data{42};

    std::vector<std::thread> ts;
    for (int i = 0; i < n_threads; ++i) {
        ts.emplace_back([&] {
            dom.thread_register();
            uint64_t count = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                dom.read_lock();
                int v = data.load(std::memory_order_relaxed);
                (void)v;
                dom.read_unlock();
                ++count;
            }
            total.fetch_add(count, std::memory_order_relaxed);
            dom.thread_unregister();
        });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(dur_ms));
    stop = true;
    for (auto& t : ts) t.join();
    return {"RCU read (" + std::to_string(n_threads) + " threads)",
            (double)total.load() / (dur_ms / 1000.0)};
}

// std::shared_mutex is backed by pthread_rwlock_t on Linux (glibc).
// This benchmark is therefore a direct RCU vs pthread rwlock comparison.
PerfResult p1_rwlock_readers(int n_threads, int dur_ms) {
    std::shared_mutex     rwlock;
    std::atomic<uint64_t> total{0};
    std::atomic<bool>     stop{false};
    std::atomic<int>      data{42};

    std::vector<std::thread> ts;
    for (int i = 0; i < n_threads; ++i) {
        ts.emplace_back([&] {
            uint64_t count = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                std::shared_lock<std::shared_mutex> lk(rwlock);
                int v = data.load(std::memory_order_relaxed);
                (void)v;
                ++count;
            }
            total.fetch_add(count, std::memory_order_relaxed);
        });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(dur_ms));
    stop = true;
    for (auto& t : ts) t.join();
    return {"std::shared_mutex / pthread_rwlock read (" + std::to_string(n_threads) + " threads)",
            (double)total.load() / (dur_ms / 1000.0)};
}

// ── P2: mixed read+write ─────────────────────────────────────────────────────

PerfResult p2_rcu_mixed(int n_readers, int dur_ms) {
    Domain dom;
    std::atomic<uint64_t> read_total{0};
    std::atomic<bool>     stop{false};
    std::atomic<int*>     data{new int(0)};

    std::vector<std::thread> ts;
    for (int i = 0; i < n_readers; ++i) {
        ts.emplace_back([&] {
            dom.thread_register();
            uint64_t count = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                dom.read_lock();
                int v = *data.load(std::memory_order_acquire);
                (void)v;
                dom.read_unlock();
                ++count;
            }
            read_total.fetch_add(count, std::memory_order_relaxed);
            dom.thread_unregister();
        });
    }
    ts.emplace_back([&] {
        dom.thread_register();
        while (!stop.load(std::memory_order_relaxed)) {
            int* old = data.load(std::memory_order_relaxed);
            int* nw  = new int(*old + 1);
            data.store(nw, std::memory_order_release);
            dom.synchronize_rcu();
            delete old;
        }
        dom.thread_unregister();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(dur_ms));
    stop = true;
    for (auto& t : ts) t.join();
    delete data.load();
    return {"RCU mixed (" + std::to_string(n_readers) + "R+1W) reads/sec",
            (double)read_total.load() / (dur_ms / 1000.0)};
}

PerfResult p2_rwlock_mixed(int n_readers, int dur_ms) {
    std::shared_mutex     rwlock;
    std::atomic<uint64_t> read_total{0};
    std::atomic<bool>     stop{false};
    int                   data{0};

    std::vector<std::thread> ts;
    for (int i = 0; i < n_readers; ++i) {
        ts.emplace_back([&] {
            uint64_t count = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                std::shared_lock<std::shared_mutex> lk(rwlock);
                int v = data;
                (void)v;
                ++count;
            }
            read_total.fetch_add(count, std::memory_order_relaxed);
        });
    }
    ts.emplace_back([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            std::unique_lock<std::shared_mutex> lk(rwlock);
            ++data;
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(dur_ms));
    stop = true;
    for (auto& t : ts) t.join();
    return {"std::shared_mutex / pthread_rwlock mixed (" + std::to_string(n_readers) + "R+1W) reads/sec",
            (double)read_total.load() / (dur_ms / 1000.0)};
}

// ── P3: grace-period throughput ───────────────────────────────────────────────

PerfResult p3_grace_period_cost(int n_readers, int dur_ms) {
    Domain dom;
    std::atomic<bool>     stop{false};
    std::atomic<uint64_t> gp_count{0};

    std::vector<std::thread> readers;
    for (int i = 0; i < n_readers; ++i) {
        readers.emplace_back([&] {
            dom.thread_register();
            while (!stop.load(std::memory_order_relaxed)) {
                dom.read_lock();
                std::this_thread::yield();
                dom.read_unlock();
            }
            dom.thread_unregister();
        });
    }

    std::thread updater([&] {
        dom.thread_register();
        uint64_t count = 0;
        auto deadline = std::chrono::steady_clock::now()
                      + std::chrono::milliseconds(dur_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            dom.synchronize_rcu();
            ++count;
        }
        gp_count.store(count);
        dom.thread_unregister();
    });

    updater.join();
    stop = true;
    for (auto& r : readers) r.join();

    return {"grace periods/sec (" + std::to_string(n_readers) + " readers)",
            (double)gp_count.load() / (dur_ms / 1000.0)};
}

void p4_scaling_sweep(int dur_ms) {
    int hw = (int)std::thread::hardware_concurrency();
    if (hw <= 0) hw = 4;

    // Build thread counts: 1..8 by 1, then steps up to 4x hw
    std::vector<int> counts;
    for (int n = 1; n <= std::min(8, hw * 4); ++n) counts.push_back(n);
    for (int n = 12; n <= hw * 4; n += std::max(1, hw / 2)) {
        if (n > counts.back()) counts.push_back(n);
    }

    std::cout << "\n  P4: Read scalability sweep  (hw threads = " << hw << ")\n";
    std::cout << "  " << std::left  << std::setw(10) << "threads"
              << std::right << std::setw(16) << "RCU (Mops/s)"
              << std::setw(22) << "rwlock (Mops/s)"
              << std::setw(10) << "speedup"
              << "\n";
    std::cout << "  " << std::string(58, '-') << "\n";

    for (int n : counts) {
        // RCU
        double rcu_ops = 0;
        {
            Domain dom;
            std::atomic<uint64_t> total{0};
            std::atomic<bool>     stop{false};
            std::atomic<int>      data{42};
            std::vector<std::thread> ts;
            for (int i = 0; i < n; ++i)
                ts.emplace_back([&]{
                    dom.thread_register();
                    uint64_t c = 0;
                    while (!stop.load(std::memory_order_relaxed)) {
                        dom.read_lock();
                        int v = data.load(std::memory_order_relaxed); (void)v;
                        dom.read_unlock();
                        ++c;
                    }
                    total.fetch_add(c, std::memory_order_relaxed);
                    dom.thread_unregister();
                });
            std::this_thread::sleep_for(std::chrono::milliseconds(dur_ms));
            stop = true;
            for (auto& t : ts) t.join();
            rcu_ops = (double)total.load() / (dur_ms / 1000.0);
        }

        // rwlock
        double rwlock_ops = 0;
        {
            std::shared_mutex     rwlock;
            std::atomic<uint64_t> total{0};
            std::atomic<bool>     stop{false};
            std::atomic<int>      data{42};
            std::vector<std::thread> ts;
            for (int i = 0; i < n; ++i)
                ts.emplace_back([&]{
                    uint64_t c = 0;
                    while (!stop.load(std::memory_order_relaxed)) {
                        std::shared_lock<std::shared_mutex> lk(rwlock);
                        int v = data.load(std::memory_order_relaxed); (void)v;
                        ++c;
                    }
                    total.fetch_add(c, std::memory_order_relaxed);
                });
            std::this_thread::sleep_for(std::chrono::milliseconds(dur_ms));
            stop = true;
            for (auto& t : ts) t.join();
            rwlock_ops = (double)total.load() / (dur_ms / 1000.0);
        }

        std::string marker = (n == hw)   ? " <- hw threads" :
                             (n == hw*2) ? " <- 2x hw"      : "";
        std::cout << "  " << std::left  << std::setw(10) << n
                  << std::right << std::fixed << std::setprecision(1)
                  << std::setw(16) << rcu_ops / 1e6
                  << std::setw(22) << rwlock_ops / 1e6
                  << std::setw(10) << rcu_ops / rwlock_ops
                  << marker << "\n";
    }
}


int main() {
    std::cout << "═══════════════════════════════════════════════════\n";
    std::cout << "  GP-RCU  —  Single Writer / Multiple Reader\n";
    std::cout << "  Correctness Tests\n";
    std::cout << "═══════════════════════════════════════════════════\n";

    // Read-side mechanics
    t1_basic_read_lock();
    t2_nesting();

    // Grace-period guarantee
    t3_sync_waits_for_reader();
    t4_sync_waits_for_all_readers();
    t5_new_generation_reader_does_not_block();
    t6_consecutive_grace_periods();

    // List operations
    t7_remove_unlinks();
    t8_find_missing();
    t9_insert_find();
    t10_concurrent_inserts();
    t11_concurrent_find_during_remove();
    t12_remove_positions();

    // Safety
    t13_no_use_after_free();
    t14_reader_sees_consistent_node();
    t15_slow_reader_unblocks_sync();

    // Thread lifecycle
    t16_register_unregister();
    t17_unregister_during_sync();

    std::cout << "\n───────────────────────────────────────────────────\n";
    std::cout << "  Results: " << g_pass << " passed, " << g_fail << " failed\n";
    std::cout << "───────────────────────────────────────────────────\n";

    // ── Performance ─────────────────────────────────────────────────────────
    // std::shared_mutex is backed by pthread_rwlock_t on Linux (glibc),
    // so this is a direct comparison between GP-RCU and pthread rwlock.
    std::cout << "\n═══════════════════════════════════════════════════\n";
    std::cout << "  Performance  (1000ms each)\n";
    std::cout << "  Note: std::shared_mutex = pthread_rwlock_t on Linux\n";
    std::cout << "═══════════════════════════════════════════════════\n";

    const int DUR = 1000;

    std::cout << "\n  P1: Read-only scalability\n";
    for (int n : {1, 2, 4, 8}) {
        print_perf(p1_rcu_readers(n, DUR));
        print_perf(p1_rwlock_readers(n, DUR));
        std::cout << "\n";
    }

    std::cout << "  P2: Mixed read+write (1 writer, varying readers)\n";
    for (int n : {1, 2, 4, 8}) {
        print_perf(p2_rcu_mixed(n, DUR));
        print_perf(p2_rwlock_mixed(n, DUR));
        std::cout << "\n";
    }

    std::cout << "  P3: Grace period throughput\n";
    for (int n : {0, 2, 4, 8}) {
        print_perf(p3_grace_period_cost(n, DUR));
    }

    p4_scaling_sweep(DUR);

    std::cout << "\n";
    return g_fail > 0 ? 1 : 0;
}
