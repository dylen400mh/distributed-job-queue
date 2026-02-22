#pragma once

#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "common/config/config.h"
#include "common/kafka/kafka_producer.h"
#include "common/db/connection_pool.h"
#include "server/db/job_repository.h"
#include "server/grpc/job_service_impl.h"
#include "server/grpc/worker_service_impl.h"
#include "server/grpc/admin_service_impl.h"

namespace jq {

// ---------------------------------------------------------------------------
// GrpcServer — owns all gRPC service implementations and the server itself.
//
// Usage:
//   GrpcServer srv(cfg, pool, kafka);
//   srv.Start();   // blocks until Stop() is called from another thread
//   srv.Stop();    // drain in-flight RPCs, then shut down
// ---------------------------------------------------------------------------
class GrpcServer {
public:
    GrpcServer(const Config&       cfg,
               db::ConnectionPool& pool,
               IKafkaProducer&     kafka);

    // Build and start the gRPC server. Blocks until Stop() is called.
    void Start();

    // Initiate graceful shutdown. Safe to call from a signal handler or
    // another thread. Start() will return after in-flight RPCs drain.
    void Stop();

private:
    const Config&       cfg_;
    db::JobRepository   job_repo_;

    JobServiceImpl      job_svc_;
    WorkerServiceImpl   worker_svc_;
    AdminServiceImpl    admin_svc_;

    std::unique_ptr<grpc::Server> server_;
};

}  // namespace jq
