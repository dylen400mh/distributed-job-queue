// tests/integration/e2e_test.cc
// End-to-end integration tests for the distributed job queue system.
//
// These tests require:
//   1. jq-server running and accessible.
//   2. jq-worker running and connected to the same server.
//   3. A "default" queue created (jq-server creates it on startup or via migration).
//
// Set JQ_TEST_SERVER_ADDR to the gRPC address (e.g. "localhost:50051").
// If the env var is unset, all tests skip gracefully.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

#include <grpcpp/grpcpp.h>

#include "admin_service.grpc.pb.h"
#include "job_service.grpc.pb.h"

using namespace jq;  // NOLINT — test-only convenience

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

const char* TestServerAddr() {
    const char* env = std::getenv("JQ_TEST_SERVER_ADDR");
    return env ? env : "";
}

// grpc::ClientContext is non-copyable; return by unique_ptr.
std::unique_ptr<grpc::ClientContext> MakeCtx(int timeout_s = 30) {
    auto ctx = std::make_unique<grpc::ClientContext>();
    ctx->set_deadline(std::chrono::system_clock::now() +
                      std::chrono::seconds(timeout_s));
    return ctx;
}

}  // namespace

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

class E2ETest : public ::testing::Test {
protected:
    void SetUp() override {
        addr_ = TestServerAddr();
        if (addr_.empty()) {
            GTEST_SKIP() << "JQ_TEST_SERVER_ADDR not set; skipping integration tests";
        }
        auto channel = grpc::CreateChannel(addr_, grpc::InsecureChannelCredentials());
        job_stub_   = JobService::NewStub(channel);
        admin_stub_ = AdminService::NewStub(channel);
    }

    std::string addr_;
    std::unique_ptr<JobService::Stub>   job_stub_;
    std::unique_ptr<AdminService::Stub> admin_stub_;
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_F(E2ETest, SubmitAndPollUntilDone) {
    // Submit a job that runs "echo hello".
    SubmitJobRequest submit_req;
    submit_req.set_queue_name("default");
    submit_req.set_payload(R"({"command":["echo","hello"]})");
    submit_req.set_max_retries(0);

    SubmitJobResponse submit_resp;
    {
        auto ctx = MakeCtx();
        auto status = job_stub_->SubmitJob(ctx.get(), submit_req, &submit_resp);
        ASSERT_TRUE(status.ok()) << status.error_message();
    }
    ASSERT_FALSE(submit_resp.job_id().empty());
    std::string job_id = submit_resp.job_id();

    // Poll GetJobStatus for up to 30 seconds (60 × 500 ms).
    GetJobStatusRequest status_req;
    status_req.set_job_id(job_id);

    JobStatus final_status = PENDING;
    for (int i = 0; i < 60; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        GetJobStatusResponse status_resp;
        auto ctx = MakeCtx();
        auto s = job_stub_->GetJobStatus(ctx.get(), status_req, &status_resp);
        ASSERT_TRUE(s.ok()) << s.error_message();

        final_status = status_resp.job().status();
        if (final_status == DONE || final_status == FAILED ||
            final_status == DEAD_LETTERED) {
            break;
        }
    }

    EXPECT_EQ(final_status, DONE)
        << "Job did not reach DONE within 30s (final: " << final_status << ")";
}

TEST_F(E2ETest, SubmitToNonExistentQueue_ReturnsNotFound) {
    SubmitJobRequest req;
    req.set_queue_name("nonexistent-queue-xyzzy-12345");
    req.set_payload(R"({"command":["echo","x"]})");

    SubmitJobResponse resp;
    auto ctx = MakeCtx();
    auto status = job_stub_->SubmitJob(ctx.get(), req, &resp);

    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.error_code(), grpc::StatusCode::NOT_FOUND);
}

TEST_F(E2ETest, GetSystemStatus_ReturnsHealthy) {
    GetSystemStatusRequest req;
    GetSystemStatusResponse resp;
    auto ctx = MakeCtx();
    auto status = admin_stub_->GetSystemStatus(ctx.get(), req, &resp);
    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_TRUE(resp.healthy());
    EXPECT_GT(resp.components_size(), 0);
}

// ---------------------------------------------------------------------------
// New integration scenarios
// ---------------------------------------------------------------------------

// FR-030/031: Queue lifecycle — create, list, delete.
TEST_F(E2ETest, QueueLifecycle_CreateListDeleteQueue) {
    const std::string name = "e2e-lifecycle-queue";

    // Create.
    {
        CreateQueueRequest req;
        req.set_name(name);
        req.set_max_retries(2);
        CreateQueueResponse resp;
        auto ctx = MakeCtx();
        auto s = admin_stub_->CreateQueue(ctx.get(), req, &resp);
        // OK or ALREADY_EXISTS (left over from a prior run).
        ASSERT_TRUE(s.ok() || s.error_code() == grpc::StatusCode::ALREADY_EXISTS)
            << s.error_message();
    }

    // List — verify the queue is present.
    {
        ListQueuesRequest req;
        ListQueuesResponse resp;
        auto ctx = MakeCtx();
        auto s = admin_stub_->ListQueues(ctx.get(), req, &resp);
        ASSERT_TRUE(s.ok()) << s.error_message();
        bool found = false;
        for (int i = 0; i < resp.queues_size(); ++i) {
            if (resp.queues(i).name() == name) { found = true; break; }
        }
        EXPECT_TRUE(found) << "Created queue not found in ListQueues";
    }

    // Delete (queue is empty so force=false is fine).
    {
        DeleteQueueRequest req;
        req.set_name(name);
        DeleteQueueResponse resp;
        auto ctx = MakeCtx();
        auto s = admin_stub_->DeleteQueue(ctx.get(), req, &resp);
        EXPECT_TRUE(s.ok()) << s.error_message();
    }
}

// FR-033/034: Failing job retries and eventually dead-letters.
TEST_F(E2ETest, RetryOnFailure_EventuallyDeadLetters) {
    // Submit a job whose command always exits non-zero.  max_retries=1 means
    // the scheduler will retry once and then dead-letter on the second failure.
    SubmitJobRequest sub_req;
    sub_req.set_queue_name("default");
    sub_req.set_payload(R"({"command":["sh","-c","exit 1"]})");
    sub_req.set_max_retries(1);

    SubmitJobResponse sub_resp;
    {
        auto ctx = MakeCtx();
        auto s = job_stub_->SubmitJob(ctx.get(), sub_req, &sub_resp);
        ASSERT_TRUE(s.ok()) << s.error_message();
    }
    const std::string job_id = sub_resp.job_id();

    // Poll for DEAD_LETTERED (up to 90 s: first run + 10-15 s backoff + second run).
    GetJobStatusRequest status_req;
    status_req.set_job_id(job_id);
    JobStatus final_status = PENDING;
    for (int i = 0; i < 180; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        GetJobStatusResponse status_resp;
        auto ctx = MakeCtx();
        auto s = job_stub_->GetJobStatus(ctx.get(), status_req, &status_resp);
        ASSERT_TRUE(s.ok()) << s.error_message();
        final_status = status_resp.job().status();
        if (final_status == DEAD_LETTERED) break;
    }
    EXPECT_EQ(final_status, DEAD_LETTERED)
        << "Job did not reach DEAD_LETTERED within 90s (final: " << final_status << ")";
}

// FR-015: Cancelling a pending job transitions it to DEAD_LETTERED.
TEST_F(E2ETest, CancelPendingJob_BecomesDeadLettered) {
    // Use a queue the worker is NOT subscribed to (worker subscribes to "default" only).
    const std::string queue_name = "cancel-test-queue";
    {
        CreateQueueRequest req;
        req.set_name(queue_name);
        CreateQueueResponse resp;
        auto ctx = MakeCtx();
        // Ignore ALREADY_EXISTS from a prior run.
        admin_stub_->CreateQueue(ctx.get(), req, &resp);
    }

    // Submit — job stays PENDING because no worker drains this queue.
    SubmitJobRequest sub_req;
    sub_req.set_queue_name(queue_name);
    sub_req.set_payload(R"({"command":["echo","cancel-me"]})");
    sub_req.set_max_retries(0);
    SubmitJobResponse sub_resp;
    {
        auto ctx = MakeCtx();
        auto s = job_stub_->SubmitJob(ctx.get(), sub_req, &sub_resp);
        ASSERT_TRUE(s.ok()) << s.error_message();
    }
    const std::string job_id = sub_resp.job_id();

    // Cancel.
    {
        CancelJobRequest req;
        req.set_job_id(job_id);
        CancelJobResponse resp;
        auto ctx = MakeCtx();
        auto s = job_stub_->CancelJob(ctx.get(), req, &resp);
        EXPECT_TRUE(s.ok()) << s.error_message();
    }

    // Assert DEAD_LETTERED.
    {
        GetJobStatusRequest req;
        req.set_job_id(job_id);
        GetJobStatusResponse resp;
        auto ctx = MakeCtx();
        auto s = job_stub_->GetJobStatus(ctx.get(), req, &resp);
        ASSERT_TRUE(s.ok()) << s.error_message();
        EXPECT_EQ(resp.job().status(), DEAD_LETTERED);
    }

    // Cleanup.
    {
        DeleteQueueRequest req;
        req.set_name(queue_name);
        req.set_force(true);
        DeleteQueueResponse resp;
        auto ctx = MakeCtx();
        admin_stub_->DeleteQueue(ctx.get(), req, &resp);
    }
}

// FR-013: GetJobLogs returns events covering the full job lifecycle.
TEST_F(E2ETest, GetJobLogs_ShowsStateTransitions) {
    // Submit a fast job and wait for it to complete.
    SubmitJobRequest sub_req;
    sub_req.set_queue_name("default");
    sub_req.set_payload(R"({"command":["echo","log-test"]})");
    sub_req.set_max_retries(0);
    SubmitJobResponse sub_resp;
    {
        auto ctx = MakeCtx();
        auto s = job_stub_->SubmitJob(ctx.get(), sub_req, &sub_resp);
        ASSERT_TRUE(s.ok()) << s.error_message();
    }
    const std::string job_id = sub_resp.job_id();

    // Wait for DONE.
    GetJobStatusRequest status_req;
    status_req.set_job_id(job_id);
    for (int i = 0; i < 60; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        GetJobStatusResponse status_resp;
        auto ctx = MakeCtx();
        job_stub_->GetJobStatus(ctx.get(), status_req, &status_resp);
        if (status_resp.job().status() == DONE) break;
    }

    // Fetch logs and verify state transitions.
    GetJobLogsRequest logs_req;
    logs_req.set_job_id(job_id);
    GetJobLogsResponse logs_resp;
    {
        auto ctx = MakeCtx();
        auto s = job_stub_->GetJobLogs(ctx.get(), logs_req, &logs_resp);
        ASSERT_TRUE(s.ok()) << s.error_message();
    }
    EXPECT_GE(logs_resp.events_size(), 2)
        << "Expected at least 2 job events (submission + at least one transition)";

    bool has_done_event = false;
    for (int i = 0; i < logs_resp.events_size(); ++i) {
        if (logs_resp.events(i).to_status() == DONE) { has_done_event = true; break; }
    }
    EXPECT_TRUE(has_done_event) << "No DONE event found in job logs";
}

// FR-012: ListJobs supports filtering by queue name and returns paginated results.
TEST_F(E2ETest, ListJobs_WithQueueFilter_ReturnsDoneJobs) {
    // Submit a job, wait for it to complete, then verify ListJobs returns it.
    SubmitJobRequest sub_req;
    sub_req.set_queue_name("default");
    sub_req.set_payload(R"({"command":["echo","list-filter-test"]})");
    sub_req.set_max_retries(0);
    SubmitJobResponse sub_resp;
    {
        auto ctx = MakeCtx();
        auto s = job_stub_->SubmitJob(ctx.get(), sub_req, &sub_resp);
        ASSERT_TRUE(s.ok()) << s.error_message();
    }
    const std::string job_id = sub_resp.job_id();

    // Wait for DONE.
    GetJobStatusRequest status_req;
    status_req.set_job_id(job_id);
    for (int i = 0; i < 60; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        GetJobStatusResponse status_resp;
        auto ctx = MakeCtx();
        job_stub_->GetJobStatus(ctx.get(), status_req, &status_resp);
        if (status_resp.job().status() == DONE) break;
    }

    // ListJobs filtered by queue_name="default" — must return a non-empty list.
    {
        ListJobsRequest req;
        req.set_queue_name("default");
        ListJobsResponse resp;
        auto ctx = MakeCtx();
        auto s = job_stub_->ListJobs(ctx.get(), req, &resp);
        ASSERT_TRUE(s.ok()) << s.error_message();
        EXPECT_GT(resp.jobs_size(), 0)
            << "Expected at least one job in 'default' queue after submission";
    }

    // ListJobs filtered by queue_name that doesn't exist — must return empty.
    {
        ListJobsRequest req;
        req.set_queue_name("nonexistent-queue-xyz-99999");
        ListJobsResponse resp;
        auto ctx = MakeCtx();
        auto s = job_stub_->ListJobs(ctx.get(), req, &resp);
        ASSERT_TRUE(s.ok()) << s.error_message();
        EXPECT_EQ(resp.jobs_size(), 0)
            << "Expected no jobs for a queue that doesn't exist";
    }
}
