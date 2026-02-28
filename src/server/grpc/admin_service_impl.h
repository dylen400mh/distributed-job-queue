#pragma once

#include <grpcpp/grpcpp.h>

#include "admin_service.grpc.pb.h"
#include "common/config/config.h"
#include "common/db/connection_pool.h"
#include "server/db/queue_repository.h"
#include "server/db/worker_repository.h"
#include "server/scheduler/worker_registry.h"

namespace jq {

class IKafkaProducer;  // forward declaration — full type only needed in .cc

// ---------------------------------------------------------------------------
// AdminServiceImpl — implements all eight AdminService RPCs.
//
// Dependencies injected for testability and correctness:
//   IQueueRepository  — queue CRUD + stats
//   IWorkerRepository — worker status changes
//   WorkerRegistry    — in-memory drain/shutdown signalling
//   ConnectionPool    — DB connectivity check for GetSystemStatus
//   RedisConfig       — Redis connectivity check for GetSystemStatus
// ---------------------------------------------------------------------------
class AdminServiceImpl final : public AdminService::Service {
public:
    AdminServiceImpl(db::IQueueRepository&  queue_repo,
                     db::IWorkerRepository& worker_repo,
                     WorkerRegistry&        registry,
                     db::ConnectionPool&    pool,
                     const RedisConfig&     redis_cfg,
                     IKafkaProducer&        kafka);

    grpc::Status CreateQueue(grpc::ServerContext*,
                             const CreateQueueRequest*,
                             CreateQueueResponse*) override;

    grpc::Status DeleteQueue(grpc::ServerContext*,
                             const DeleteQueueRequest*,
                             DeleteQueueResponse*) override;

    grpc::Status ListQueues(grpc::ServerContext*,
                            const ListQueuesRequest*,
                            ListQueuesResponse*) override;

    grpc::Status GetQueueStats(grpc::ServerContext*,
                               const GetQueueStatsRequest*,
                               GetQueueStatsResponse*) override;

    grpc::Status ListWorkers(grpc::ServerContext*,
                             const ListWorkersRequest*,
                             ListWorkersResponse*) override;

    grpc::Status DrainWorker(grpc::ServerContext*,
                             const DrainWorkerRequest*,
                             DrainWorkerResponse*) override;

    grpc::Status ShutdownWorker(grpc::ServerContext*,
                                const ShutdownWorkerRequest*,
                                ShutdownWorkerResponse*) override;

    grpc::Status GetSystemStatus(grpc::ServerContext*,
                                 const GetSystemStatusRequest*,
                                 GetSystemStatusResponse*) override;

private:
    db::IQueueRepository&  queue_repo_;
    db::IWorkerRepository& worker_repo_;
    WorkerRegistry&        registry_;
    db::ConnectionPool&    pool_;
    const RedisConfig&     redis_cfg_;
    IKafkaProducer&        kafka_;
};

}  // namespace jq
