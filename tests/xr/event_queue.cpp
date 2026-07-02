#include <openxr/XRQueue.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

using namespace OpenXR;

// tests/xr/event_queue.cpp — the lock-free SPSC ring behind the OpenXR frame->main event queue
// (docs/openxr/04-input.md §7.2). Pure, header-only, no OpenXR runtime/headers needed.

// A ring of capacity CAP holds CAP-1 live items; the CAP-th push fails (full/empty
// disambiguation reserves one slot). FIFO order is preserved.
TEST(XRQueue, FillsToCapacityMinusOneAndPreservesOrder) {
    CXRSPSCRing<int, 4> ring;
    EXPECT_TRUE(ring.empty());

    EXPECT_TRUE(ring.push(10));
    EXPECT_TRUE(ring.push(20));
    EXPECT_TRUE(ring.push(30));
    EXPECT_FALSE(ring.push(40)); // full: only CAP-1 = 3 usable slots
    EXPECT_FALSE(ring.empty());

    int v = 0;
    ASSERT_TRUE(ring.pop(v));
    EXPECT_EQ(v, 10);
    ASSERT_TRUE(ring.pop(v));
    EXPECT_EQ(v, 20);

    // A freed slot lets a new push succeed (wrap-around).
    EXPECT_TRUE(ring.push(40));

    ASSERT_TRUE(ring.pop(v));
    EXPECT_EQ(v, 30);
    ASSERT_TRUE(ring.pop(v));
    EXPECT_EQ(v, 40);
    EXPECT_FALSE(ring.pop(v));
    EXPECT_TRUE(ring.empty());
}

TEST(XRQueue, PopOnEmptyReturnsFalse) {
    CXRSPSCRing<int, 8> ring;
    int                 v = -1;
    EXPECT_FALSE(ring.pop(v));
    EXPECT_EQ(v, -1); // out untouched
}

TEST(XRQueue, ResetClearsContents) {
    CXRSPSCRing<int, 8> ring;
    ring.push(1);
    ring.push(2);
    ring.reset();
    EXPECT_TRUE(ring.empty());
    int v = 0;
    EXPECT_FALSE(ring.pop(v));
}

// Lossless under burst: a producer thread enqueues N items (retrying on a full ring, since the
// consumer drains concurrently); the consumer pops until it has seen all N. Every value must be
// received exactly once and in FIFO order. A small ring forces many full/empty transitions,
// exercising the acquire/release handoff.
TEST(XRQueue, ConcurrentBurstIsLosslessAndOrdered) {
    constexpr int        N = 10000;
    CXRSPSCRing<int, 16> ring;

    std::atomic<bool>    producerDone{false};

    std::thread          producer([&] {
        for (int i = 0; i < N; ++i)
            while (!ring.push(i))
                std::this_thread::yield(); // full: wait for the consumer
        producerDone.store(true, std::memory_order_release);
    });

    int                  received = 0;
    int                  expected = 0;
    int                  v        = 0;
    while (received < N) {
        if (ring.pop(v)) {
            EXPECT_EQ(v, expected); // strict FIFO, no loss, no duplication
            ++expected;
            ++received;
        } else if (producerDone.load(std::memory_order_acquire) && ring.empty())
            break;
        else
            std::this_thread::yield();
    }

    producer.join();
    EXPECT_EQ(received, N);
}
