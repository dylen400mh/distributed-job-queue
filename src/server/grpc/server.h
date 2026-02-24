#pragma once

#include <memory>

#include <grpcpp/grpcpp.h>

#include "common/config/config.h"
#include "common/kafka/kafka_producer.h"
#include "common/db/connection_pool.h"
#include "server/db/job_repository.h"
#include "server/db/queue_repository.h"
#include "server/db/worker_repository.h"
#include "server/grpc/job_service_impl.h"
#include "server/grpc/worker_service_impl.h"
#include "server/grpc/admin_service_impl.h"
#include "server/scheduler/worker_registry.h"
#include "server/scheduler/scheduler.h"

namespace jq {

// ---------------------------------------------------------------------------
// GrpcServer — owns all gRPC service implementations, WorkerRegistry, and
//              the Scheduler. Starts the scheduler alongside the gRPC server.
//
// Usage:
//   GrpcServer srv(cfg, pool, kafka);
//   srv.Start();   // blocks until Stop() is called from another thread
//   srv.Stop();    // stop scheduler, drain in-flight RPCs, shut down
// ---------------------------------------------------------------------------
class GrpcServer {
public:
    GrpcServer(const Config&       cfg,
               db::ConnectionPool& pool,
               IKafkaProducer&     kafka);

    // Build and start the gRPC server (and scheduler). Blocks until Stop().
    void Start();

    // Initiate graceful shutdown. Safe to call from a signal handler or
    // another thread. Start() will return after in-flight RPCs drain.
    void Stop();

private:
    const Config&        cfg_;
    db::JobRepository    job_repo_;
    db::QueueRepository  queue_repo_;
    db::WorkerRepository worker_repo_;

    WorkerRegistry       registry_;

    JobServiceImpl       job_svc_;
    WorkerServiceImpl    worker_svc_;
    AdminServiceImpl     admin_svc_;

    Scheduler           scheduler_;

    std::unique_ptr<grpc::Server> server_;
};

}  // namespace jq
