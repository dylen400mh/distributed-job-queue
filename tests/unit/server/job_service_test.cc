#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <optional>
#include <string>
#include <vector>

// Pull in the generated proto headers (build/proto_gen is on the include path).
#include "common.pb.h"
#include "job_service.pb.h"

#include "common/kafka/kafka_producer.h"
#include "server/db/job_repository.h"
#include "server/grpc/job_service_impl.h"

using ::testing::_;
using ::testing::Return;
using ::testing::DoAll;
using ::testing::SetArgPointee;

// ---------------------------------------------------------------------------
// Mock implementations
// ---------------------------------------------------------------------------

class MockJobRepository : public jq::db::IJobRepository {
public:
    MOCK_METHOD(bool, QueueExists,       (const std::string&), (override));
    MOCK_METHOD(int,  GetQueueMaxRetries,(const std::string&), (override));
    MOCK_METHOD(std::string, InsertJob,
                (const std::string&, const std::vector<uint8_t>&, int, int),
                (override));
    MOCK_METHOD(std::optional<jq::db::JobRow>, FindJobById,
                (const std::string&), (override));
    MOCK_METHOD(bool, TransitionJobStatus,
                (const std::string&, const std::string&,
                 const std::string&, const std::string&, const std::string&),
                (override));
    MOCK_METHOD(bool, ResetJobForRetry,
                (const std::string&, const std::string&), (override));
    MOCK_METHOD(std::vector<jq::db::JobRow>, ListJobs,
                (const std::string&, const std::string&, int, int), (override));
    MOCK_METHOD(std::vector<jq::db::JobEventRow>, ListJobEvents,
                (const std::string&), (override));
};

class MockKafkaProducer : public jq::IKafkaProducer {
public:
    MOCK_METHOD(void, Publish,
                (const std::string&, const std::string&, const std::vector<uint8_t>&),
                (override));
    MOCK_METHOD(void, Flush, (int), (override));
};

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

class JobServiceTest : public ::testing::Test {
protected:
    MockJobRepository  repo;
    MockKafkaProducer  kafka;
    jq::JobServiceImpl svc{repo, kafka};

    grpc::ServerContext ctx;

    // Build a minimal JobRow for tests that need one.
    static jq::db::JobRow MakeRow(const std::string& id,
                                   const std::string& status = "PENDING") {
        jq::db::JobRow r;
        r.job_id     = id;
        r.queue_name = "default";
        r.priority   = 0;
        r.status     = status;
        r.max_retries = 3;
        r.retry_count = 0;
        r.created_at  = 1700000000;
        return r;
    }
};

// ---------------------------------------------------------------------------
// SubmitJob tests
// ---------------------------------------------------------------------------

TEST_F(JobServiceTest, SubmitJob_QueueNotFound_ReturnsNotFound) {
    EXPECT_CALL(repo, QueueExists("no-such-queue")).WillOnce(Return(false));
    EXPECT_CALL(kafka, Publish(_, _, _)).Times(0);

    jq::SubmitJobRequest req;
    req.set_queue_name("no-such-queue");
    jq::SubmitJobResponse resp;

    auto status = svc.SubmitJob(&ctx, &req, &resp);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::NOT_FOUND);
}

TEST_F(JobServiceTest, SubmitJob_Success_ReturnsJobId) {
    EXPECT_CALL(repo, QueueExists("default")).WillOnce(Return(true));
    EXPECT_CALL(repo, GetQueueMaxRetries("default")).WillOnce(Return(3));
    EXPECT_CALL(repo, InsertJob("default", _, 5, 3))
        .WillOnce(Return("job-uuid-1234"));
    EXPECT_CALL(kafka, Publish(jq::KafkaTopics::kJobSubmitted, "job-uuid-1234", _))
        .Times(1);

    jq::SubmitJobRequest req;
    req.set_queue_name("default");
    req.set_priority(5);
    // max_retries == 0 → use queue default
    jq::SubmitJobResponse resp;

    auto status = svc.SubmitJob(&ctx, &req, &resp);
    EXPECT_TRUE(status.ok());
    EXPECT_EQ(resp.job_id(), "job-uuid-1234");
}

TEST_F(JobServiceTest, SubmitJob_EmptyQueueName_ReturnsInvalidArgument) {
    jq::SubmitJobRequest req;  // queue_name is empty
    jq::SubmitJobResponse resp;
    auto status = svc.SubmitJob(&ctx, &req, &resp);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// ---------------------------------------------------------------------------
// CancelJob tests
// ---------------------------------------------------------------------------

TEST_F(JobServiceTest, CancelJob_JobNotFound_ReturnsNotFound) {
    EXPECT_CALL(repo, FindJobById("bad-id"))
        .WillOnce(Return(std::nullopt));

    jq::CancelJobRequest req;
    req.set_job_id("bad-id");
    jq::CancelJobResponse resp;

    auto status = svc.CancelJob(&ctx, &req, &resp);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::NOT_FOUND);
}

TEST_F(JobServiceTest, CancelJob_RunningJob_ReturnsFailedPrecondition) {
    EXPECT_CALL(repo, FindJobById("j1"))
        .WillOnce(Return(MakeRow("j1", "RUNNING")));

    jq::CancelJobRequest req;
    req.set_job_id("j1");
    jq::CancelJobResponse resp;

    auto status = svc.CancelJob(&ctx, &req, &resp);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
}

TEST_F(JobServiceTest, CancelJob_PendingJob_Succeeds) {
    EXPECT_CALL(repo, FindJobById("j2"))
        .WillOnce(Return(MakeRow("j2", "PENDING")));
    EXPECT_CALL(repo, TransitionJobStatus("j2", "PENDING", "DEAD_LETTERED", "CANCELLED", ""))
        .WillOnce(Return(true));
    EXPECT_CALL(kafka, Publish(jq::KafkaTopics::kJobDeadLettered, _, _)).Times(1);

    jq::CancelJobRequest req;
    req.set_job_id("j2");
    jq::CancelJobResponse resp;

    auto status = svc.CancelJob(&ctx, &req, &resp);
    EXPECT_TRUE(status.ok());
}

// ---------------------------------------------------------------------------
// GetJobStatus tests
// ---------------------------------------------------------------------------

TEST_F(JobServiceTest, GetJobStatus_NotFound_ReturnsNotFound) {
    EXPECT_CALL(repo, FindJobById("x")).WillOnce(Return(std::nullopt));

    jq::GetJobStatusRequest req;
    req.set_job_id("x");
    jq::GetJobStatusResponse resp;

    auto status = svc.GetJobStatus(&ctx, &req, &resp);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::NOT_FOUND);
}

TEST_F(JobServiceTest, GetJobStatus_Found_ReturnsJob) {
    auto row = MakeRow("j3", "RUNNING");
    EXPECT_CALL(repo, FindJobById("j3")).WillOnce(Return(row));

    jq::GetJobStatusRequest req;
    req.set_job_id("j3");
    jq::GetJobStatusResponse resp;

    auto status = svc.GetJobStatus(&ctx, &req, &resp);
    EXPECT_TRUE(status.ok());
    EXPECT_EQ(resp.job().job_id(), "j3");
    EXPECT_EQ(resp.job().status(), jq::RUNNING);
}

// ---------------------------------------------------------------------------
// ListJobs tests
// ---------------------------------------------------------------------------

TEST_F(JobServiceTest, ListJobs_NoFilter_ReturnsRows) {
    std::vector<jq::db::JobRow> rows = {MakeRow("a"), MakeRow("b")};
    // limit defaults to 20; we fetch limit+1 = 21 to detect next page.
    EXPECT_CALL(repo, ListJobs("", "", 21, 0)).WillOnce(Return(rows));

    jq::ListJobsRequest req;
    jq::ListJobsResponse resp;

    auto status = svc.ListJobs(&ctx, &req, &resp);
    EXPECT_TRUE(status.ok());
    EXPECT_EQ(resp.jobs_size(), 2);
    EXPECT_TRUE(resp.next_page_token().empty());  // fewer than limit rows
}

TEST_F(JobServiceTest, ListJobs_HasNextPage_ReturnsToken) {
    // Return limit+1 rows to indicate there are more.
    std::vector<jq::db::JobRow> rows;
    for (int i = 0; i <= 20; ++i) rows.push_back(MakeRow("j" + std::to_string(i)));

    EXPECT_CALL(repo, ListJobs("", "", 21, 0)).WillOnce(Return(rows));

    jq::ListJobsRequest req;
    jq::ListJobsResponse resp;

    auto status = svc.ListJobs(&ctx, &req, &resp);
    EXPECT_TRUE(status.ok());
    EXPECT_EQ(resp.jobs_size(), 20);
    EXPECT_EQ(resp.next_page_token(), "20");
}

// ---------------------------------------------------------------------------
// GetJobLogs tests
// ---------------------------------------------------------------------------

TEST_F(JobServiceTest, GetJobLogs_ReturnsEvents) {
    jq::db::JobEventRow e1;
    e1.id = 1; e1.job_id = "j4"; e1.to_status = "PENDING"; e1.occurred_at = 1700000001;
    jq::db::JobEventRow e2;
    e2.id = 2; e2.job_id = "j4"; e2.from_status = "PENDING"; e2.to_status = "RUNNING";
    e2.occurred_at = 1700000002;

    EXPECT_CALL(repo, ListJobEvents("j4"))
        .WillOnce(Return(std::vector<jq::db::JobEventRow>{e1, e2}));

    jq::GetJobLogsRequest req;
    req.set_job_id("j4");
    jq::GetJobLogsResponse resp;

    auto status = svc.GetJobLogs(&ctx, &req, &resp);
    EXPECT_TRUE(status.ok());
    EXPECT_EQ(resp.events_size(), 2);
    EXPECT_EQ(resp.events(0).to_status(), jq::PENDING);
    EXPECT_EQ(resp.events(1).to_status(), jq::RUNNING);
}

// ---------------------------------------------------------------------------
// RetryJob tests
// ---------------------------------------------------------------------------

TEST_F(JobServiceTest, RetryJob_NotFound_ReturnsNotFound) {
    EXPECT_CALL(repo, FindJobById("z")).WillOnce(Return(std::nullopt));

    jq::RetryJobRequest req;
    req.set_job_id("z");
    jq::RetryJobResponse resp;

    auto status = svc.RetryJob(&ctx, &req, &resp);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::NOT_FOUND);
}

TEST_F(JobServiceTest, RetryJob_PendingJob_ReturnsFailedPrecondition) {
    EXPECT_CALL(repo, FindJobById("j5"))
        .WillOnce(Return(MakeRow("j5", "PENDING")));

    jq::RetryJobRequest req;
    req.set_job_id("j5");
    jq::RetryJobResponse resp;

    auto status = svc.RetryJob(&ctx, &req, &resp);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
}

TEST_F(JobServiceTest, RetryJob_FailedJob_Succeeds) {
    EXPECT_CALL(repo, FindJobById("j6"))
        .WillOnce(Return(MakeRow("j6", "FAILED")));
    EXPECT_CALL(repo, ResetJobForRetry("j6", "FAILED")).WillOnce(Return(true));

    jq::RetryJobRequest req;
    req.set_job_id("j6");
    jq::RetryJobResponse resp;

    auto status = svc.RetryJob(&ctx, &req, &resp);
    EXPECT_TRUE(status.ok());
    EXPECT_EQ(resp.job_id(), "j6");
}

TEST_F(JobServiceTest, RetryJob_DeadLettered_Succeeds) {
    EXPECT_CALL(repo, FindJobById("j7"))
        .WillOnce(Return(MakeRow("j7", "DEAD_LETTERED")));
    EXPECT_CALL(repo, ResetJobForRetry("j7", "DEAD_LETTERED")).WillOnce(Return(true));

    jq::RetryJobRequest req;
    req.set_job_id("j7");
    jq::RetryJobResponse resp;

    auto status = svc.RetryJob(&ctx, &req, &resp);
    EXPECT_TRUE(status.ok());
    EXPECT_EQ(resp.job_id(), "j7");
}
