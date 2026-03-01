#include "common/metrics/metrics.h"

#include <memory>
#include <string>

#include <prometheus/exposer.h>
#include <prometheus/registry.h>

namespace jq::metrics {

namespace {

// Singleton registry — created once on first access.
prometheus::Registry& RegistryInstance() {
    static prometheus::Registry registry;
    return registry;
}

// Default histogram buckets for duration metrics (seconds).
const prometheus::Histogram::BucketBoundaries kDurationBuckets = {
    0.001, 0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0, 10.0
};

}  // namespace

prometheus::Registry& GlobalRegistry() {
    return RegistryInstance();
}

// ---------------------------------------------------------------------------
// FR-040 metric families
// ---------------------------------------------------------------------------

prometheus::Family<prometheus::Gauge>& JobQueueDepth() {
    static auto& family = prometheus::BuildGauge()
        .Name("jq_job_queue_depth")
        .Help("Number of jobs in the queue by status")
        .Register(RegistryInstance());
    return family;
}

prometheus::Family<prometheus::Histogram>& JobProcessingDuration() {
    static auto& family = prometheus::BuildHistogram()
        .Name("jq_job_processing_duration_seconds")
        .Help("Time taken to process a job from start to completion")
        .Register(RegistryInstance());
    return family;
}

prometheus::Family<prometheus::Counter>& JobTotal() {
    static auto& family = prometheus::BuildCounter()
        .Name("jq_job_total")
        .Help("Total number of jobs by queue and final status")
        .Register(RegistryInstance());
    return family;
}

prometheus::Family<prometheus::Gauge>& WorkerActiveCount() {
    static auto& family = prometheus::BuildGauge()
        .Name("jq_worker_active_count")
        .Help("Number of currently active workers")
        .Register(RegistryInstance());
    return family;
}

prometheus::Family<prometheus::Gauge>& WorkerJobConcurrency() {
    static auto& family = prometheus::BuildGauge()
        .Name("jq_worker_job_concurrency")
        .Help("Number of jobs currently executing on each worker")
        .Register(RegistryInstance());
    return family;
}

prometheus::Family<prometheus::Histogram>& GrpcRequestDuration() {
    static auto& family = prometheus::BuildHistogram()
        .Name("jq_grpc_request_duration_seconds")
        .Help("Duration of gRPC requests by method and status code")
        .Register(RegistryInstance());
    return family;
}

prometheus::Family<prometheus::Histogram>& SchedulerCycleDuration() {
    static auto& family = prometheus::BuildHistogram()
        .Name("jq_scheduler_cycle_duration_seconds")
        .Help("Duration of each scheduler loop cycle")
        .Register(RegistryInstance());
    return family;
}

prometheus::Family<prometheus::Counter>& SchedulerJobsAssignedTotal() {
    static auto& family = prometheus::BuildCounter()
        .Name("jq_scheduler_jobs_assigned_total")
        .Help("Total number of jobs assigned to workers by queue")
        .Register(RegistryInstance());
    return family;
}

prometheus::Family<prometheus::Counter>& KafkaPublishErrorsTotal() {
    static auto& family = prometheus::BuildCounter()
        .Name("jq_kafka_publish_errors_total")
        .Help("Total number of Kafka publish errors by topic")
        .Register(RegistryInstance());
    return family;
}

prometheus::Family<prometheus::Histogram>& DbQueryDuration() {
    static auto& family = prometheus::BuildHistogram()
        .Name("jq_db_query_duration_seconds")
        .Help("Duration of database queries by query name")
        .Register(RegistryInstance());
    return family;
}

prometheus::Family<prometheus::Histogram>& RedisOperationDuration() {
    static auto& family = prometheus::BuildHistogram()
        .Name("jq_redis_operation_duration_seconds")
        .Help("Duration of Redis operations by operation name")
        .Register(RegistryInstance());
    return family;
}

// ---------------------------------------------------------------------------
// Metrics HTTP server
// ---------------------------------------------------------------------------

void StartMetricsServer(int port) {
    // prometheus::Exposer runs its own background thread; keep it alive via a
    // static so it outlives this call.
    static prometheus::Exposer exposer{"0.0.0.0:" + std::to_string(port)};
    // RegisterCollectable stores a weak_ptr internally, so the shared_ptr
    // must outlive this call. Keep it as a static so the weak_ptr never expires.
    static auto registry_ptr = std::shared_ptr<prometheus::Registry>(
        &RegistryInstance(), [](prometheus::Registry*) {});  // no-op deleter
    exposer.RegisterCollectable(registry_ptr);
}

}  // namespace jq::metrics
