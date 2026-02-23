#pragma once

#include <grpcpp/grpcpp.h>

#include "worker_service.grpc.pb.h"
#include "common.pb.h"
#include "common/kafka/kafka_producer.h"
#include "server/db/job_repository.h"
#include "server/db/worker_repository.h"
#include "server/scheduler/worker_registry.h"

namespace jq {

// ---------------------------------------------------------------------------
// WorkerServiceImpl — implements all five WorkerService RPCs.
//
// Dependencies injected so they can be mocked in unit tests:
//   IJobRepository    — job state transitions
//   IWorkerRepository — worker CRUD (heartbeat, status)
//   IKafkaProducer    — lifecycle events
//   WorkerRegistry    — in-memory stream registry for job push
// ---------------------------------------------------------------------------
class WorkerServiceImpl final : public WorkerService::Service {
public:
    WorkerServiceImpl(db::IJobRepository&    job_repo,
                      db::IWorkerRepository& worker_repo,
                      IKafkaProducer&        kafka,
                      WorkerRegistry&        registry);

    // Insert or upsert worker row (ONLINE, last_heartbeat=now()).
    grpc::Status RegisterWorker(grpc::ServerContext*          ctx,
                                const RegisterWorkerRequest*  req,
                                RegisterWorkerResponse*       resp) override;

    // Update last_heartbeat for the given worker_id.
    grpc::Status Heartbeat(grpc::ServerContext*    ctx,
                           const HeartbeatRequest* req,
                           HeartbeatResponse*      resp) override;

    // Keep the server-streaming RPC open; register the stream in WorkerRegistry
    // so the scheduler can push JobAssignment messages. Blocks until disconnect.
    grpc::Status StreamJobs(grpc::ServerContext*               ctx,
                            const StreamJobsRequest*           req,
                            grpc::ServerWriter<JobAssignment>* writer) override;

    // Validate status == RUNNING, store result (DONE or FAILED), apply retry.
    grpc::Status ReportResult(grpc::ServerContext*       ctx,
                              const ReportResultRequest* req,
                              ReportResultResponse*      resp) override;

    // Set worker status OFFLINE and remove from WorkerRegistry.
    grpc::Status Deregister(grpc::ServerContext*     ctx,
                            const DeregisterRequest* req,
                            DeregisterResponse*      resp) override;

private:
    // Publish a serialized JobEvent proto to Kafka. Non-throwing.
    void PublishEvent(const std::string& topic, const JobEvent& ev);

    db::IJobRepository&    job_repo_;
    db::IWorkerRepository& worker_repo_;
    IKafkaProducer&        kafka_;
    WorkerRegistry&        registry_;
};

}  // namespace jq
