#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "server/scheduler/scheduler.h"
#include "server/scheduler/worker_registry.h"

// ---------------------------------------------------------------------------
// CalculateRetryNotBefore — verify the backoff formula exactly.
// ---------------------------------------------------------------------------

TEST(BackoffTest, FirstRetry_BaseDelay) {
    // retry_count=1 → delay = base * 2^1 = 5 * 2 = 10s (no jitter)
    int64_t now    = 1700000000;
    int64_t result = jq::CalculateRetryNotBefore(now, /*retry_count=*/1,
                                                  /*base=*/5, /*max=*/300, /*jitter=*/0);
    EXPECT_EQ(result, now + 10);
}

TEST(BackoffTest, SecondRetry_Doubles) {
    // retry_count=2 → delay = 5 * 2^2 = 20s
    int64_t now    = 1700000000;
    int64_t result = jq::CalculateRetryNotBefore(now, 2, 5, 300, 0);
    EXPECT_EQ(result, now + 20);
}

TEST(BackoffTest, CapsAtMaxDelay) {
    // retry_count=10 → delay = min(5 * 2^10, 300) = 300s
    int64_t now    = 1700000000;
    int64_t result = jq::CalculateRetryNotBefore(now, 10, 5, 300, 0);
    EXPECT_EQ(result, now + 300);
}

TEST(BackoffTest, JitterAdded) {
    int64_t now    = 1700000000;
    int64_t result = jq::CalculateRetryNotBefore(now, 1, 5, 300, /*jitter=*/7);
    // delay = 10 + 7 = 17
    EXPECT_EQ(result, now + 17);
}

TEST(BackoffTest, RetryCountZero_BaseDelay) {
    // retry_count=0 → delay = base * 2^0 = 5s
    int64_t now    = 1700000000;
    int64_t result = jq::CalculateRetryNotBefore(now, 0, 5, 300, 0);
    EXPECT_EQ(result, now + 5);
}

// ---------------------------------------------------------------------------
// WorkerRegistry — AssignJob selection logic.
//
// Uses lambda write callbacks instead of real gRPC streams (ServerWriter
// is marked final and cannot be subclassed in tests).
// ---------------------------------------------------------------------------

namespace {

// Create a write callback that records all written messages.
struct WriteCapture {
    std::vector<std::string> job_ids;
    bool                     return_value = true;

    std::function<bool(const jq::JobAssignment&)> Fn() {
        return [this](const jq::JobAssignment& msg) {
            job_ids.push_back(msg.job_id());
            return return_value;
        };
    }
};

}  // namespace

TEST(WorkerRegistryTest, AssignJob_NoWorkers_ReturnsFalse) {
    jq::WorkerRegistry registry;
    std::vector<uint8_t> payload;
    EXPECT_FALSE(registry.AssignJob("job1", "default", payload, 0));
}

TEST(WorkerRegistryTest, AssignJob_WrongQueue_ReturnsFalse) {
    jq::WorkerRegistry registry;
    WriteCapture cap;

    auto handle = registry.RegisterStream("w1", {"other-queue"}, 4, cap.Fn());

    std::vector<uint8_t> payload;
    EXPECT_FALSE(registry.AssignJob("job1", "default", payload, 0));
    EXPECT_TRUE(cap.job_ids.empty());

    { std::lock_guard<std::mutex> lock(handle->mu); handle->active = false; }
    registry.RemoveWorker("w1");
}

TEST(WorkerRegistryTest, AssignJob_MatchingWorker_WritesAndReturnsTrue) {
    jq::WorkerRegistry registry;
    WriteCapture cap;

    auto handle = registry.RegisterStream("w1", {"default"}, 4, cap.Fn());

    std::vector<uint8_t> payload = {1, 2, 3};
    bool ok = registry.AssignJob("job-abc", "default", payload, 5);

    EXPECT_TRUE(ok);
    ASSERT_EQ(cap.job_ids.size(), 1u);
    EXPECT_EQ(cap.job_ids[0], "job-abc");

    { std::lock_guard<std::mutex> lock(handle->mu); handle->active = false; }
    registry.RemoveWorker("w1");
}

TEST(WorkerRegistryTest, AssignJob_ConcurrencyLimitReached_ReturnsFalse) {
    jq::WorkerRegistry registry;
    WriteCapture cap;

    // concurrency = 1
    auto handle = registry.RegisterStream("w1", {"default"}, 1, cap.Fn());

    std::vector<uint8_t> payload;
    // First assign succeeds (active_job_count 0 → 1).
    EXPECT_TRUE(registry.AssignJob("job1", "default", payload, 0));
    // Second assign fails (active_job_count 1 >= concurrency 1).
    EXPECT_FALSE(registry.AssignJob("job2", "default", payload, 0));
    EXPECT_EQ(cap.job_ids.size(), 1u);

    { std::lock_guard<std::mutex> lock(handle->mu); handle->active = false; }
    registry.RemoveWorker("w1");
}

TEST(WorkerRegistryTest, DecrementActiveCount_AllowsNextAssign) {
    jq::WorkerRegistry registry;
    WriteCapture cap;

    auto handle = registry.RegisterStream("w1", {"default"}, 1, cap.Fn());

    std::vector<uint8_t> payload;
    EXPECT_TRUE(registry.AssignJob("job1", "default", payload, 0));
    EXPECT_FALSE(registry.AssignJob("job2", "default", payload, 0));

    // Simulate job completion — decrement count.
    registry.DecrementActiveCount("w1");

    // Now a third assign should succeed.
    EXPECT_TRUE(registry.AssignJob("job3", "default", payload, 0));
    EXPECT_EQ(cap.job_ids.size(), 2u);  // job1 + job3

    { std::lock_guard<std::mutex> lock(handle->mu); handle->active = false; }
    registry.RemoveWorker("w1");
}

TEST(WorkerRegistryTest, RemoveWorker_PreventsAssign) {
    jq::WorkerRegistry registry;
    WriteCapture cap;

    auto handle = registry.RegisterStream("w1", {"default"}, 4, cap.Fn());
    { std::lock_guard<std::mutex> lock(handle->mu); handle->active = false; }
    registry.RemoveWorker("w1");

    std::vector<uint8_t> payload;
    EXPECT_FALSE(registry.AssignJob("job1", "default", payload, 0));
}

TEST(WorkerRegistryTest, AssignJob_WriteFails_RollsBackCount) {
    jq::WorkerRegistry registry;
    WriteCapture cap;
    cap.return_value = false;  // simulate stream write failure

    auto handle = registry.RegisterStream("w1", {"default"}, 4, cap.Fn());

    std::vector<uint8_t> payload;
    bool ok = registry.AssignJob("job1", "default", payload, 0);
    EXPECT_FALSE(ok);

    // active_job_count should be back to 0; next assign from a fresh worker
    // would succeed again (we verify by checking stats).
    auto stats = registry.GetWorkerStats();
    ASSERT_EQ(stats.size(), 1u);
    EXPECT_EQ(stats[0].active_job_count, 0);

    { std::lock_guard<std::mutex> lock(handle->mu); handle->active = false; }
    registry.RemoveWorker("w1");
}
