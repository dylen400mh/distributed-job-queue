#include "ctl/client.h"

#include <chrono>
#include <iostream>
#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

namespace jq {

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

GrpcClient::GrpcClient(const std::string& server_addr,
                        int                timeout_s,
                        const std::string& tls_cert,
                        const std::string& tls_key)
    : timeout_s_(timeout_s) {
    std::shared_ptr<grpc::ChannelCredentials> creds;
    if (!tls_cert.empty() && !tls_key.empty()) {
        grpc::SslCredentialsOptions ssl_opts;
        ssl_opts.pem_cert_chain  = tls_cert;
        ssl_opts.pem_private_key = tls_key;
        creds = grpc::SslCredentials(ssl_opts);
    } else {
        creds = grpc::InsecureChannelCredentials();
    }
    channel_    = grpc::CreateChannel(server_addr, creds);
    job_stub_   = JobService::NewStub(channel_);
    admin_stub_ = AdminService::NewStub(channel_);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::unique_ptr<grpc::ClientContext> GrpcClient::MakeCtx() const {
    auto ctx = std::make_unique<grpc::ClientContext>();
    ctx->set_deadline(std::chrono::system_clock::now() +
                      std::chrono::seconds(timeout_s_));
    return ctx;
}

void GrpcClient::Die(const grpc::Status& s, const char* rpc_name) {
    std::cerr << "Error: " << rpc_name << " failed: "
              << s.error_message()
              << " (code " << static_cast<int>(s.error_code()) << ")\n";
    std::exit(1);
}

// ---------------------------------------------------------------------------
// JobService wrappers
// ---------------------------------------------------------------------------

SubmitJobResponse GrpcClient::SubmitJob(const SubmitJobRequest& req) {
    auto ctx = MakeCtx();
    SubmitJobResponse resp;
    auto s = job_stub_->SubmitJob(ctx.get(), req, &resp);
    if (!s.ok()) Die(s, "SubmitJob");
    return resp;
}

CancelJobResponse GrpcClient::CancelJob(const CancelJobRequest& req) {
    auto ctx = MakeCtx();
    CancelJobResponse resp;
    auto s = job_stub_->CancelJob(ctx.get(), req, &resp);
    if (!s.ok()) Die(s, "CancelJob");
    return resp;
}

GetJobStatusResponse GrpcClient::GetJobStatus(const GetJobStatusRequest& req) {
    auto ctx = MakeCtx();
    GetJobStatusResponse resp;
    auto s = job_stub_->GetJobStatus(ctx.get(), req, &resp);
    if (!s.ok()) Die(s, "GetJobStatus");
    return resp;
}

ListJobsResponse GrpcClient::ListJobs(const ListJobsRequest& req) {
    auto ctx = MakeCtx();
    ListJobsResponse resp;
    auto s = job_stub_->ListJobs(ctx.get(), req, &resp);
    if (!s.ok()) Die(s, "ListJobs");
    return resp;
}

GetJobLogsResponse GrpcClient::GetJobLogs(const GetJobLogsRequest& req) {
    auto ctx = MakeCtx();
    GetJobLogsResponse resp;
    auto s = job_stub_->GetJobLogs(ctx.get(), req, &resp);
    if (!s.ok()) Die(s, "GetJobLogs");
    return resp;
}

RetryJobResponse GrpcClient::RetryJob(const RetryJobRequest& req) {
    auto ctx = MakeCtx();
    RetryJobResponse resp;
    auto s = job_stub_->RetryJob(ctx.get(), req, &resp);
    if (!s.ok()) Die(s, "RetryJob");
    return resp;
}

// ---------------------------------------------------------------------------
// AdminService wrappers
// ---------------------------------------------------------------------------

CreateQueueResponse GrpcClient::CreateQueue(const CreateQueueRequest& req) {
    auto ctx = MakeCtx();
    CreateQueueResponse resp;
    auto s = admin_stub_->CreateQueue(ctx.get(), req, &resp);
    if (!s.ok()) Die(s, "CreateQueue");
    return resp;
}

DeleteQueueResponse GrpcClient::DeleteQueue(const DeleteQueueRequest& req) {
    auto ctx = MakeCtx();
    DeleteQueueResponse resp;
    auto s = admin_stub_->DeleteQueue(ctx.get(), req, &resp);
    if (!s.ok()) Die(s, "DeleteQueue");
    return resp;
}

ListQueuesResponse GrpcClient::ListQueues(const ListQueuesRequest& req) {
    auto ctx = MakeCtx();
    ListQueuesResponse resp;
    auto s = admin_stub_->ListQueues(ctx.get(), req, &resp);
    if (!s.ok()) Die(s, "ListQueues");
    return resp;
}

GetQueueStatsResponse GrpcClient::GetQueueStats(const GetQueueStatsRequest& req) {
    auto ctx = MakeCtx();
    GetQueueStatsResponse resp;
    auto s = admin_stub_->GetQueueStats(ctx.get(), req, &resp);
    if (!s.ok()) Die(s, "GetQueueStats");
    return resp;
}

ListWorkersResponse GrpcClient::ListWorkers(const ListWorkersRequest& req) {
    auto ctx = MakeCtx();
    ListWorkersResponse resp;
    auto s = admin_stub_->ListWorkers(ctx.get(), req, &resp);
    if (!s.ok()) Die(s, "ListWorkers");
    return resp;
}

DrainWorkerResponse GrpcClient::DrainWorker(const DrainWorkerRequest& req) {
    auto ctx = MakeCtx();
    DrainWorkerResponse resp;
    auto s = admin_stub_->DrainWorker(ctx.get(), req, &resp);
    if (!s.ok()) Die(s, "DrainWorker");
    return resp;
}

ShutdownWorkerResponse GrpcClient::ShutdownWorker(const ShutdownWorkerRequest& req) {
    auto ctx = MakeCtx();
    ShutdownWorkerResponse resp;
    auto s = admin_stub_->ShutdownWorker(ctx.get(), req, &resp);
    if (!s.ok()) Die(s, "ShutdownWorker");
    return resp;
}

GetSystemStatusResponse GrpcClient::GetSystemStatus(const GetSystemStatusRequest& req) {
    auto ctx = MakeCtx();
    GetSystemStatusResponse resp;
    auto s = admin_stub_->GetSystemStatus(ctx.get(), req, &resp);
    if (!s.ok()) Die(s, "GetSystemStatus");
    return resp;
}

}  // namespace jq
