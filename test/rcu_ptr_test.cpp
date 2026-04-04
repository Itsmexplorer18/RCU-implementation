#include "rcu_ptr.h"
#include <iostream>
#include <vector>
#include <string>
#include <cassert>

// Simple test runner
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


TEST(test_read_after_construct) {
    auto vec = std::make_shared<std::vector<int>>(std::initializer_list<int>{1, 2, 3});
    rcu_ptr<std::vector<int>> p(vec);

    auto snapshot = p.read();
    ASSERT(snapshot != nullptr);
    ASSERT(snapshot->size() == 3);
    ASSERT((*snapshot)[0] == 1);
    ASSERT((*snapshot)[2] == 3);
}


TEST(test_default_null) {
    rcu_ptr<std::vector<int>> p;
    ASSERT(p.is_null());
}


TEST(test_reset) {
    rcu_ptr<std::string> p(std::make_shared<const std::string>("hello"));

    auto snap1 = p.read();
    ASSERT(*snap1 == "hello");

    p.reset(std::make_shared<const std::string>("world"));

    auto snap2 = p.read();
    ASSERT(*snap2 == "world");

    // Old snapshot still valid — shared_ptr kept it alive
    ASSERT(*snap1 == "hello");
}


TEST(test_copy_update_isolation) {
    rcu_ptr<std::vector<int>> p(
        std::make_shared<const std::vector<int>>(std::initializer_list<int>{1, 2, 3}));

    // Take a snapshot BEFORE update
    auto old_snap = p.read();

    // Update: append 4
    p.copy_update([](std::vector<int>* copy) {
        copy->push_back(4);
    });

    auto new_snap = p.read();

    // New snapshot has 4 elements
    ASSERT(new_snap->size() == 4);
    ASSERT(new_snap->back() == 4);

    // Old snapshot is UNCHANGED — this is the core RCU guarantee
    ASSERT(old_snap->size() == 3);
}


TEST(test_multiple_updates) {
    rcu_ptr<int> p(std::make_shared<const int>(0));

    for (int i = 1; i <= 5; i++) {
        p.copy_update([i](int* copy) {
            *copy = i;
        });
    }

    auto snap = p.read();
    ASSERT(*snap == 5);
}


TEST(test_update_on_null) {
    rcu_ptr<std::vector<int>> p; // null

    // copy_update on null should default-construct T then apply fun
    p.copy_update([](std::vector<int>* copy) {
        copy->push_back(42);
    });

    auto snap = p.read();
    ASSERT(snap != nullptr);
    ASSERT(snap->size() == 1);
    ASSERT((*snap)[0] == 42);
}


TEST(test_snapshot_outlives_reset) {
    rcu_ptr<std::string> p(std::make_shared<const std::string>("original"));

    auto snap = p.read(); // reader holds on to old version

    p.reset(std::make_shared<const std::string>("replaced"));
    p.reset(std::make_shared<const std::string>("replaced again"));

    // snap still points to "original" — shared_ptr kept it alive
    ASSERT(*snap == "original");
    ASSERT(*p.read() == "replaced again");
}


struct Config {
    int timeout;
    std::string host;
    bool enabled;

    Config() : timeout(0), host(""), enabled(false) {}
    Config(int t, std::string h, bool e)
        : timeout(t), host(std::move(h)), enabled(e) {}
};

TEST(test_struct_update) {
    rcu_ptr<Config> p(std::make_shared<const Config>(30, "localhost", true));

    auto old_snap = p.read();
    ASSERT(old_snap->timeout == 30);
    ASSERT(old_snap->host == "localhost");

    // Update only the timeout
    p.copy_update([](Config* copy) {
        copy->timeout = 60;
        copy->host    = "192.168.1.1";
    });

    auto new_snap = p.read();
    ASSERT(new_snap->timeout == 60);
    ASSERT(new_snap->host == "192.168.1.1");
    ASSERT(new_snap->enabled == true); // unchanged

    // Old snapshot unaffected
    ASSERT(old_snap->timeout == 30);
    ASSERT(old_snap->host == "localhost");
}


int main() {
    std::cout << "=== rcu_ptr Sequential Tests ===\n\n";

    RUN(test_read_after_construct);
    RUN(test_default_null);
    RUN(test_reset);
    RUN(test_copy_update_isolation);
    RUN(test_multiple_updates);
    RUN(test_update_on_null);
    RUN(test_snapshot_outlives_reset);
    RUN(test_struct_update);

    std::cout << "\nResults: " << tests_passed << "/" << tests_run << " passed\n";
    return (tests_passed == tests_run) ? 0 : 1;
}
