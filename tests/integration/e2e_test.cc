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
