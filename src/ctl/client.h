#pragma once

#include <chrono>
#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "job_service.grpc.pb.h"
#include "worker_service.grpc.pb.h"
#include "admin_service.grpc.pb.h"

namespace jq {

// ---------------------------------------------------------------------------
// GrpcClient — wraps all three service stubs with a configurable deadline.
//
// On any gRPC error, the helper methods print a human-readable message to
// stderr and call std::exit(1), so callers need not check return codes for
// the command-handler use-case.
// ---------------------------------------------------------------------------
class GrpcClient {
public:
    GrpcClient(const std::string& server_addr,
               int                timeout_s,
               const std::string& tls_cert = "",
               const std::string& tls_key  = "");

    // --- JobService ---
    SubmitJobResponse    SubmitJob   (const SubmitJobRequest&    req);
    CancelJobResponse    CancelJob   (const CancelJobRequest&    req);
    GetJobStatusResponse GetJobStatus(const GetJobStatusRequest& req);
    ListJobsResponse     ListJobs    (const ListJobsRequest&     req);
    GetJobLogsResponse   GetJobLogs  (const GetJobLogsRequest&   req);
    RetryJobResponse     RetryJob    (const RetryJobRequest&     req);

    // --- AdminService ---
    CreateQueueResponse    CreateQueue   (const CreateQueueRequest&    req);
    DeleteQueueResponse    DeleteQueue   (const DeleteQueueRequest&    req);
    ListQueuesResponse     ListQueues    (const ListQueuesRequest&      req);
    GetQueueStatsResponse  GetQueueStats (const GetQueueStatsRequest&  req);
    ListWorkersResponse    ListWorkers   (const ListWorkersRequest&     req);
    DrainWorkerResponse    DrainWorker   (const DrainWorkerRequest&     req);
    ShutdownWorkerResponse ShutdownWorker(const ShutdownWorkerRequest&  req);
    GetSystemStatusResponse GetSystemStatus(const GetSystemStatusRequest& req);

private:
    // Create a context with the configured deadline.
    std::unique_ptr<grpc::ClientContext> MakeCtx() const;

    // Print gRPC error to stderr and exit(1).
    [[noreturn]] static void Die(const grpc::Status& s, const char* rpc_name);

    int timeout_s_;
    std::shared_ptr<grpc::Channel>          channel_;
    std::unique_ptr<JobService::Stub>       job_stub_;
    std::unique_ptr<AdminService::Stub>     admin_stub_;
};

}  // namespace jq
