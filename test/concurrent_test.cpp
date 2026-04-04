#include "rcu_ptr.h"
#include "rcu_list.h"
#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <cassert>
#include <chrono>
#include <string>

static int tests_run    = 0;
static int tests_passed = 0;

#define TEST(name) void name()
#define RUN(name)                                          \
    do {                                                   \
        tests_run++;                                       \
        try {                                              \
            name();                                        \
            tests_passed++;                                \
            std::cout << "  [PASS] " << #name << "\n";    \
        } catch (const std::exception& e) {                \
            std::cout << "  [FAIL] " << #name             \
                      << " — " << e.what() << "\n";        \
        }                                                  \
    } while (0)

#define ASSERT(cond)                                                \
    do {                                                            \
        if (!(cond)) throw std::runtime_error("Assert failed: "    \
            #cond " at line " + std::to_string(__LINE__));         \
    } while (0)


TEST(test_concurrent_readers_one_writer) {
    constexpr int NUM_READERS   = 8;
    constexpr int ITERATIONS    = 10000;

    // The vector always contains either all 1s or all 2s — never mixed
    rcu_ptr<std::vector<int>> p(
        std::make_shared<const std::vector<int>>(100, 1)); // 100 x 1

    std::atomic<bool> stop{false};
    std::atomic<int>  torn_reads{0};

    // Writer: alternate between all-1s and all-2s
    std::thread writer([&]() {
        int val = 2;
        for (int i = 0; i < ITERATIONS; i++) {
            p.copy_update([val](std::vector<int>* copy) {
                std::fill(copy->begin(), copy->end(), val);
            });
            val = (val == 2) ? 1 : 2;
        }
        stop = true;
    });

    // Readers: every element in a snapshot must be the same value
    std::vector<std::thread> readers;
    for (int r = 0; r < NUM_READERS; r++) {
        readers.emplace_back([&]() {
            while (!stop.load(std::memory_order_relaxed)) {
                auto snap = p.read();
                if (!snap || snap->empty()) continue;

                int first = (*snap)[0];
                for (int v : *snap) {
                    if (v != first) {
                        torn_reads.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        });
    }

    writer.join();
    for (auto& t : readers) t.join();

    ASSERT(torn_reads.load() == 0);
    std::cout << "      (0 torn reads across " << NUM_READERS << " reader threads)\n";
}


TEST(test_concurrent_writers) {
    constexpr int NUM_WRITERS = 4;
    constexpr int OPS_EACH    = 1000;

    rcu_ptr<std::vector<int>> p(
        std::make_shared<const std::vector<int>>(NUM_WRITERS, 0));

    std::vector<std::thread> writers;
    for (int w = 0; w < NUM_WRITERS; w++) {
        writers.emplace_back([&p, w]() {
            for (int i = 0; i < OPS_EACH; i++) {
                p.copy_update([w](std::vector<int>* copy) {
                    (*copy)[w]++;
                });
            }
        });
    }
    for (auto& t : writers) t.join();

    auto snap = p.read();
    for (int w = 0; w < NUM_WRITERS; w++) {
        ASSERT((*snap)[w] == OPS_EACH);
    }
    std::cout << "      (each of " << NUM_WRITERS << " writers did "
              << OPS_EACH << " updates, all correct)\n";
}


TEST(test_snapshot_stability) {
    constexpr int ITERATIONS = 5000;

    rcu_ptr<int> p(std::make_shared<const int>(0));
    std::atomic<bool> stop{false};
    std::atomic<int>  violations{0};

    std::thread writer([&]() {
        for (int i = 1; i <= ITERATIONS; i++) {
            p.copy_update([i](int* copy) { *copy = i; });
        }
        stop = true;
    });

    std::thread reader([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            auto snap = p.read();
            int val_at_read = *snap;
            // Simulate doing some work while holding the snapshot
            std::this_thread::yield();
            // The snapshot must still have the same value
            if (*snap != val_at_read) {
                violations.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    writer.join();
    reader.join();

    ASSERT(violations.load() == 0);
    std::cout << "      (0 snapshot mutations observed)\n";
}

TEST(test_rcu_list_concurrent) {
    constexpr int NUM_THREADS = 4;
    constexpr int OPS_EACH    = 500;

    rcu_list<int, std::string> list;
    std::atomic<int> successful_lookups{0};

    // Pre-insert some known keys
    for (int i = 0; i < 10; i++) {
        list.insert(i, "value_" + std::to_string(i));
    }

    std::vector<std::thread> threads;

    // Half threads insert, half threads lookup
    for (int t = 0; t < NUM_THREADS; t++) {
        if (t % 2 == 0) {
            // Writer
            threads.emplace_back([&list, t]() {
                for (int i = 0; i < OPS_EACH; i++) {
                    int key = 100 + t * OPS_EACH + i;
                    list.insert(key, "thread_" + std::to_string(t));
                }
            });
        } else {
            // Reader — lookup known pre-inserted keys
            threads.emplace_back([&list, &successful_lookups]() {
                for (int i = 0; i < OPS_EACH; i++) {
                    auto val = list.lookup(i % 10); // keys 0-9 always exist
                    if (val.has_value()) {
                        successful_lookups.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }
    }

    for (auto& t : threads) t.join();

    // All reader lookups on pre-inserted keys must have succeeded
    int expected_reader_threads = NUM_THREADS / 2;
    ASSERT(successful_lookups.load() == expected_reader_threads * OPS_EACH);
    std::cout << "      (" << successful_lookups.load()
              << " successful lookups, no misses on stable keys)\n";
}


TEST(test_rcu_list_remove) {
    rcu_list<std::string, int> list;
    list.insert("a", 1);
    list.insert("b", 2);
    list.insert("c", 3);

    ASSERT(list.size() == 3);
    ASSERT(list.lookup("b").value() == 2);

    bool removed = list.remove("b");
    ASSERT(removed == true);
    ASSERT(list.size() == 2);
    ASSERT(!list.lookup("b").has_value());

    // Non-existent key
    bool not_found = list.remove("z");
    ASSERT(not_found == false);
}


int main() {
    std::cout << "=== rcu_ptr Concurrent / Stress Tests ===\n\n";
    std::cout << "Hardware threads: "
              << std::thread::hardware_concurrency() << "\n\n";

    RUN(test_concurrent_readers_one_writer);
    RUN(test_concurrent_writers);
    RUN(test_snapshot_stability);
    RUN(test_rcu_list_concurrent);
    RUN(test_rcu_list_remove);

    std::cout << "\nResults: " << tests_passed << "/" << tests_run << " passed\n";
    return (tests_passed == tests_run) ? 0 : 1;
}
