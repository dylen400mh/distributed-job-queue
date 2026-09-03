#pragma once

#include <chrono>
#include <string>

#include <grpcpp/support/interceptor.h>
#include <grpcpp/support/server_interceptor.h>

namespace jq {

// ---------------------------------------------------------------------------
// MetricsInterceptor — records jq_grpc_request_duration_seconds{method,
// status_code} for every RPC. Times from the request being received
// (POST_RECV_INITIAL_METADATA) to the final status being sent
// (PRE_SEND_STATUS).
// ---------------------------------------------------------------------------
class MetricsInterceptor final : public grpc::experimental::Interceptor {
public:
    explicit MetricsInterceptor(grpc::experimental::ServerRpcInfo* info);

    void Intercept(grpc::experimental::InterceptorBatchMethods* methods) override;

private:
    std::string                           method_;
    std::chrono::steady_clock::time_point start_;
    bool                                  started_ = false;
};

class MetricsInterceptorFactory final
    : public grpc::experimental::ServerInterceptorFactoryInterface {
public:
    grpc::experimental::Interceptor* CreateServerInterceptor(
        grpc::experimental::ServerRpcInfo* info) override;
};

}  // namespace jq
