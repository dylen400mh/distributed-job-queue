#pragma once

#include <memory>

#include <grpcpp/grpcpp.h>

#include "job_service.grpc.pb.h"
#include "common/kafka/kafka_producer.h"
#include "server/db/job_repository.h"

namespace jq {

// ---------------------------------------------------------------------------
// JobServiceImpl — implements all six JobService RPCs.
//
// Depends on IJobRepository and IKafkaProducer so it can be unit-tested
// with mocks without a real database or Kafka broker.
// ---------------------------------------------------------------------------
class JobServiceImpl final : public JobService::Service {
public:
    JobServiceImpl(db::IJobRepository& repo, IKafkaProducer& kafka);

    grpc::Status SubmitJob(grpc::ServerContext*        ctx,
                           const SubmitJobRequest*     req,
                           SubmitJobResponse*          resp) override;

    grpc::Status CancelJob(grpc::ServerContext*        ctx,
                           const CancelJobRequest*     req,
                           CancelJobResponse*          resp) override;

    grpc::Status GetJobStatus(grpc::ServerContext*          ctx,
                              const GetJobStatusRequest*    req,
                              GetJobStatusResponse*         resp) override;

    grpc::Status ListJobs(grpc::ServerContext*      ctx,
                          const ListJobsRequest*    req,
                          ListJobsResponse*         resp) override;

    grpc::Status GetJobLogs(grpc::ServerContext*        ctx,
                            const GetJobLogsRequest*    req,
                            GetJobLogsResponse*         resp) override;

    grpc::Status RetryJob(grpc::ServerContext*      ctx,
                          const RetryJobRequest*    req,
                          RetryJobResponse*         resp) override;

private:
    db::IJobRepository& repo_;
    IKafkaProducer&     kafka_;

    // Publish a JobEvent proto message to Kafka.
    // Swallows exceptions — Kafka is not on the critical path (FR-038).
    void PublishEvent(const std::string& topic, const JobEvent& event);

    // Build a proto Job message from a DB row.
    static Job JobRowToProto(const db::JobRow& row);

    // Build a proto JobEvent message from a DB row.
    static JobEvent EventRowToProto(const db::JobEventRow& row);

    // Map a DB status string to the proto JobStatus enum.
    static JobStatus StatusFromString(const std::string& s);
};

}  // namespace jq
