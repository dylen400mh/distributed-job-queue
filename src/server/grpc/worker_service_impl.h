#pragma once

#include <grpcpp/grpcpp.h>
#include "worker_service.grpc.pb.h"

namespace jq {

// Stub implementation — WorkerService RPCs are implemented in a later prompt.
class WorkerServiceImpl final : public WorkerService::Service {
public:
    grpc::Status RegisterWorker(grpc::ServerContext*,
                                const RegisterWorkerRequest*,
                                RegisterWorkerResponse*) override {
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "not yet implemented");
    }
    grpc::Status Heartbeat(grpc::ServerContext*,
                           const HeartbeatRequest*,
                           HeartbeatResponse*) override {
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "not yet implemented");
    }
    grpc::Status StreamJobs(grpc::ServerContext*,
                            const StreamJobsRequest*,
                            grpc::ServerWriter<JobAssignment>*) override {
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "not yet implemented");
    }
    grpc::Status ReportResult(grpc::ServerContext*,
                              const ReportResultRequest*,
                              ReportResultResponse*) override {
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "not yet implemented");
    }
    grpc::Status Deregister(grpc::ServerContext*,
                            const DeregisterRequest*,
                            DeregisterResponse*) override {
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "not yet implemented");
    }
};

}  // namespace jq
