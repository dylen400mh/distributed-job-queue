#pragma once

#include <memory>

#include <grpcpp/grpcpp.h>

#include "job_service.grpc.pb.h"
#include "server/db/job_repository.h"

namespace jq {

// ---------------------------------------------------------------------------
// JobServiceImpl — implements all six JobService RPCs.
//
// Depends on IJobRepository so it can be unit-tested with a mock without a
// real database.
// ---------------------------------------------------------------------------
class JobServiceImpl final : public JobService::Service {
public:
    explicit JobServiceImpl(db::IJobRepository& repo);

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

    // Build a proto Job message from a DB row.
    static Job JobRowToProto(const db::JobRow& row);

    // Build a proto JobEvent message from a DB row.
    static JobEvent EventRowToProto(const db::JobEventRow& row);

    // Map a DB status string to the proto JobStatus enum.
    static JobStatus StatusFromString(const std::string& s);
};

}  // namespace jq
