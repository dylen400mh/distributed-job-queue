#include "server/grpc/metrics_interceptor.h"

#include <prometheus/histogram.h>

#include "common/metrics/metrics.h"

namespace jq {

namespace {

const prometheus::Histogram::BucketBoundaries kGrpcDurationBuckets = {
    0.001, 0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0
};

}  // namespace

MetricsInterceptor::MetricsInterceptor(grpc::experimental::ServerRpcInfo* info)
    : method_(info->method()) {}

void MetricsInterceptor::Intercept(grpc::experimental::InterceptorBatchMethods* methods) {
    if (methods->QueryInterceptionHookPoint(
            grpc::experimental::InterceptionHookPoints::POST_RECV_INITIAL_METADATA)) {
        start_   = std::chrono::steady_clock::now();
        started_ = true;
    }

    if (started_ &&
        methods->QueryInterceptionHookPoint(
            grpc::experimental::InterceptionHookPoints::PRE_SEND_STATUS)) {
        const double elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start_).count();
        const std::string status_code =
            std::to_string(static_cast<int>(methods->GetSendStatus().error_code()));
        metrics::GrpcRequestDuration()
            .Add({{"method", method_}, {"status_code", status_code}}, kGrpcDurationBuckets)
            .Observe(elapsed);
    }

    methods->Proceed();
}

grpc::experimental::Interceptor* MetricsInterceptorFactory::CreateServerInterceptor(
        grpc::experimental::ServerRpcInfo* info) {
    return new MetricsInterceptor(info);
}

}  // namespace jq
