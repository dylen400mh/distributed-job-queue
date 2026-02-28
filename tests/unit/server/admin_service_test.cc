#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <optional>
#include <string>
#include <vector>

#include "admin_service.pb.h"
#include "common.pb.h"

#include "common/config/config.h"
#include "common/db/connection_pool.h"
#include "common/kafka/kafka_producer.h"
#include "server/db/queue_repository.h"
#include "server/db/worker_repository.h"
#include "server/grpc/admin_service_impl.h"
#include "server/scheduler/worker_registry.h"

using ::testing::_;
using ::testing::Return;

// ---------------------------------------------------------------------------
// Mock implementations
// ---------------------------------------------------------------------------

class MockQueueRepository : public jq::db::IQueueRepository {
public:
    MOCK_METHOD(jq::db::QueueRow, CreateQueue,
                (const std::string&, int, int),
                (override));
    MOCK_METHOD(bool, DeleteQueue,
                (const std::string&, bool),
                (override));
    MOCK_METHOD(std::vector<jq::db::QueueRow>, ListQueues, (), (override));
    MOCK_METHOD(std::optional<jq::db::QueueStatsRow>, GetQueueStats,
                (const std::string&),
                (override));
};

class MockWorkerRepository : public jq::db::IWorkerRepository {
public:
    MOCK_METHOD(std::string, UpsertWorker,
                (const std::string&, const std::string&, int),
                (override));
    MOCK_METHOD(std::optional<jq::db::WorkerRow>, FindWorkerById,
                (const std::string&),
                (override));
    MOCK_METHOD(bool, UpdateWorkerHeartbeat,
                (const std::string&),
                (override));
    MOCK_METHOD(void, SetWorkerStatus,
                (const std::string&, const std::string&),
                (override));
    MOCK_METHOD(std::vector<jq::db::WorkerRow>, FetchStaleWorkers,
                (int),
                (override));
    MOCK_METHOD(std::vector<jq::db::WorkerRow>, ListWorkers, (), (override));
};

class MockKafkaProducer : public jq::IKafkaProducer {
public:
    MOCK_METHOD(void, Publish,
                (const std::string&, const std::string&, const std::vector<uint8_t>&),
                (override));
    MOCK_METHOD(void, Flush, (int), (override));
    MOCK_METHOD(bool, IsHealthy, (), (override));
};

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

class AdminServiceTest : public ::testing::Test {
protected:
    MockQueueRepository  queue_repo_;
    MockWorkerRepository worker_repo_;
    MockKafkaProducer    kafka_;
    jq::WorkerRegistry   registry_;
    // pool_size = 0: constructor skips all connection attempts — safe for unit tests.
    jq::db::ConnectionPool pool_{"", 0};
    jq::RedisConfig      redis_cfg_{};
    jq::AdminServiceImpl svc_{queue_repo_, worker_repo_, registry_, pool_, redis_cfg_, kafka_};

    grpc::ServerContext ctx_;

    static jq::db::QueueRow MakeQueueRow(const std::string& name) {
        jq::db::QueueRow r;
        r.name        = name;
        r.max_retries = 3;
        r.ttl_seconds = 0;
        r.created_at  = 1700000000;
        return r;
    }

    static jq::db::QueueStatsRow MakeStatsRow(const std::string& name) {
        jq::db::QueueStatsRow s;
        s.queue_name      = name;
        s.pending_count   = 5;
        s.running_count   = 2;
        s.done_count      = 100;
        s.total_processed = 102;
        return s;
    }

    static jq::db::WorkerRow MakeWorkerRow(const std::string& id) {
        jq::db::WorkerRow w;
        w.worker_id        = id;
        w.hostname         = "host-1";
        w.status           = "ONLINE";
        w.concurrency      = 4;
        w.active_job_count = 1;
        w.last_heartbeat   = 1700000000;
        return w;
    }
};

// ---------------------------------------------------------------------------
// CreateQueue tests
// ---------------------------------------------------------------------------

TEST_F(AdminServiceTest, CreateQueue_Success) {
    EXPECT_CALL(queue_repo_, CreateQueue("myqueue", 3, 0))
        .WillOnce(Return(MakeQueueRow("myqueue")));

    jq::CreateQueueRequest req;
    req.set_name("myqueue");
    jq::CreateQueueResponse resp;

    auto status = svc_.CreateQueue(&ctx_, &req, &resp);
    EXPECT_TRUE(status.ok());
    EXPECT_EQ(resp.queue().name(), "myqueue");
    EXPECT_EQ(resp.queue().max_retries(), 3);
}

TEST_F(AdminServiceTest, CreateQueue_EmptyName_ReturnsInvalidArgument) {
    EXPECT_CALL(queue_repo_, CreateQueue(_, _, _)).Times(0);

    jq::CreateQueueRequest req;  // name is empty
    jq::CreateQueueResponse resp;

    auto status = svc_.CreateQueue(&ctx_, &req, &resp);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// ---------------------------------------------------------------------------
// DeleteQueue tests
// ---------------------------------------------------------------------------

TEST_F(AdminServiceTest, DeleteQueue_NonEmpty_ReturnsFailedPrecondition) {
    // force = false (default), repo signals the queue is non-empty
    EXPECT_CALL(queue_repo_, DeleteQueue("busy-queue", false))
        .WillOnce(Return(false));

    jq::DeleteQueueRequest req;
    req.set_name("busy-queue");
    jq::DeleteQueueResponse resp;

    auto status = svc_.DeleteQueue(&ctx_, &req, &resp);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
}

TEST_F(AdminServiceTest, DeleteQueue_Force_Succeeds) {
    EXPECT_CALL(queue_repo_, DeleteQueue("myqueue", true))
        .WillOnce(Return(true));

    jq::DeleteQueueRequest req;
    req.set_name("myqueue");
    req.set_force(true);
    jq::DeleteQueueResponse resp;

    auto status = svc_.DeleteQueue(&ctx_, &req, &resp);
    EXPECT_TRUE(status.ok());
}

// ---------------------------------------------------------------------------
// GetQueueStats tests
// ---------------------------------------------------------------------------

TEST_F(AdminServiceTest, GetQueueStats_NotFound_ReturnsNotFound) {
    EXPECT_CALL(queue_repo_, GetQueueStats("no-such-queue"))
        .WillOnce(Return(std::nullopt));

    jq::GetQueueStatsRequest req;
    req.set_name("no-such-queue");
    jq::GetQueueStatsResponse resp;

    auto status = svc_.GetQueueStats(&ctx_, &req, &resp);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::NOT_FOUND);
}

TEST_F(AdminServiceTest, GetQueueStats_Found_ReturnsStats) {
    EXPECT_CALL(queue_repo_, GetQueueStats("myqueue"))
        .WillOnce(Return(MakeStatsRow("myqueue")));

    jq::GetQueueStatsRequest req;
    req.set_name("myqueue");
    jq::GetQueueStatsResponse resp;

    auto status = svc_.GetQueueStats(&ctx_, &req, &resp);
    EXPECT_TRUE(status.ok());
    EXPECT_EQ(resp.stats().queue_name(), "myqueue");
    EXPECT_EQ(resp.stats().pending_count(), 5);
    EXPECT_EQ(resp.stats().running_count(), 2);
    EXPECT_EQ(resp.stats().done_count(), 100);
}

// ---------------------------------------------------------------------------
// ListWorkers tests
// ---------------------------------------------------------------------------

TEST_F(AdminServiceTest, ListWorkers_ReturnsWorkers) {
    EXPECT_CALL(worker_repo_, ListWorkers())
        .WillOnce(Return(std::vector<jq::db::WorkerRow>{
            MakeWorkerRow("w1"),
            MakeWorkerRow("w2"),
        }));

    jq::ListWorkersRequest req;
    jq::ListWorkersResponse resp;

    auto status = svc_.ListWorkers(&ctx_, &req, &resp);
    EXPECT_TRUE(status.ok());
    EXPECT_EQ(resp.workers_size(), 2);
    EXPECT_EQ(resp.workers(0).worker_id(), "w1");
    EXPECT_EQ(resp.workers(0).status(), jq::ONLINE);
}

// ---------------------------------------------------------------------------
// DrainWorker tests
// ---------------------------------------------------------------------------

TEST_F(AdminServiceTest, DrainWorker_EmptyId_ReturnsInvalidArgument) {
    EXPECT_CALL(worker_repo_, SetWorkerStatus(_, _)).Times(0);

    jq::DrainWorkerRequest req;  // worker_id is empty
    jq::DrainWorkerResponse resp;

    auto status = svc_.DrainWorker(&ctx_, &req, &resp);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

TEST_F(AdminServiceTest, DrainWorker_Succeeds) {
    EXPECT_CALL(worker_repo_, SetWorkerStatus("w1", "DRAINING")).Times(1);

    jq::DrainWorkerRequest req;
    req.set_worker_id("w1");
    jq::DrainWorkerResponse resp;

    auto status = svc_.DrainWorker(&ctx_, &req, &resp);
    EXPECT_TRUE(status.ok());
}

// ---------------------------------------------------------------------------
// ShutdownWorker tests
// ---------------------------------------------------------------------------

TEST_F(AdminServiceTest, ShutdownWorker_Succeeds) {
    EXPECT_CALL(worker_repo_, SetWorkerStatus("w2", "OFFLINE")).Times(1);

    jq::ShutdownWorkerRequest req;
    req.set_worker_id("w2");
    jq::ShutdownWorkerResponse resp;

    auto status = svc_.ShutdownWorker(&ctx_, &req, &resp);
    EXPECT_TRUE(status.ok());
}
