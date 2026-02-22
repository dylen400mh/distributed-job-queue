#pragma once

#include <grpcpp/grpcpp.h>
#include "admin_service.grpc.pb.h"

namespace jq {

// Stub implementation — AdminService RPCs are implemented in a later prompt.
class AdminServiceImpl final : public AdminService::Service {
public:
    grpc::Status CreateQueue(grpc::ServerContext*,
                             const CreateQueueRequest*,
                             CreateQueueResponse*) override {
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "not yet implemented");
    }
    grpc::Status DeleteQueue(grpc::ServerContext*,
                             const DeleteQueueRequest*,
                             DeleteQueueResponse*) override {
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "not yet implemented");
    }
    grpc::Status ListQueues(grpc::ServerContext*,
                            const ListQueuesRequest*,
                            ListQueuesResponse*) override {
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "not yet implemented");
    }
    grpc::Status GetQueueStats(grpc::ServerContext*,
                               const GetQueueStatsRequest*,
                               GetQueueStatsResponse*) override {
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "not yet implemented");
    }
    grpc::Status ListWorkers(grpc::ServerContext*,
                             const ListWorkersRequest*,
                             ListWorkersResponse*) override {
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "not yet implemented");
    }
    grpc::Status DrainWorker(grpc::ServerContext*,
                             const DrainWorkerRequest*,
                             DrainWorkerResponse*) override {
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "not yet implemented");
    }
    grpc::Status ShutdownWorker(grpc::ServerContext*,
                                const ShutdownWorkerRequest*,
                                ShutdownWorkerResponse*) override {
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "not yet implemented");
    }
    grpc::Status GetSystemStatus(grpc::ServerContext*,
                                 const GetSystemStatusRequest*,
                                 GetSystemStatusResponse*) override {
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "not yet implemented");
    }
};

}  // namespace jq
