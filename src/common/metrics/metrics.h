#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <prometheus/counter.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <prometheus/registry.h>

namespace jq::metrics {

// ---------------------------------------------------------------------------
// ScopedDuration — RAII timer that observes elapsed seconds into a histogram
// on destruction. Construct at the top of the scope being timed:
//   metrics::ScopedDuration timer(metrics::DbQueryDuration().Add({{"query_name", "X"}}));
// ---------------------------------------------------------------------------
class ScopedDuration {
public:
    explicit ScopedDuration(prometheus::Histogram& hist)
        : hist_(hist), start_(std::chrono::steady_clock::now()) {}

    ~ScopedDuration() {
        const double elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start_).count();
        hist_.Observe(elapsed);
    }

    ScopedDuration(const ScopedDuration&)            = delete;
    ScopedDuration& operator=(const ScopedDuration&) = delete;

private:
    prometheus::Histogram&               hist_;
    std::chrono::steady_clock::time_point start_;
};

// ---------------------------------------------------------------------------
// Global prometheus registry — shared by all metrics in this process.
// ---------------------------------------------------------------------------
prometheus::Registry& GlobalRegistry();

// ---------------------------------------------------------------------------
// FR-040 metrics — all defined here as accessors returning the named Family.
// ---------------------------------------------------------------------------

// jq_job_queue_depth{queue, status} — Gauge
prometheus::Family<prometheus::Gauge>&
JobQueueDepth();

// jq_job_processing_duration_seconds{queue} — Histogram
prometheus::Family<prometheus::Histogram>&
JobProcessingDuration();

// jq_job_total{queue, status} — Counter
prometheus::Family<prometheus::Counter>&
JobTotal();

// jq_worker_active_count — Gauge (no labels)
prometheus::Family<prometheus::Gauge>&
WorkerActiveCount();

// jq_worker_job_concurrency{worker_id} — Gauge
prometheus::Family<prometheus::Gauge>&
WorkerJobConcurrency();

// jq_grpc_request_duration_seconds{method, status_code} — Histogram
prometheus::Family<prometheus::Histogram>&
GrpcRequestDuration();

// jq_scheduler_cycle_duration_seconds — Histogram (no labels)
prometheus::Family<prometheus::Histogram>&
SchedulerCycleDuration();

// jq_scheduler_jobs_assigned_total{queue} — Counter
prometheus::Family<prometheus::Counter>&
SchedulerJobsAssignedTotal();

// jq_db_query_duration_seconds{query_name} — Histogram
prometheus::Family<prometheus::Histogram>&
DbQueryDuration();

// Returns the jq_db_query_duration_seconds child for the given query name,
// with the default duration buckets applied. Repository methods time
// themselves with:
//   metrics::ScopedDuration timer(metrics::DbQueryTimer("MethodName"));
prometheus::Histogram& DbQueryTimer(const std::string& query_name);

// jq_redis_operation_duration_seconds{operation} — Histogram
prometheus::Family<prometheus::Histogram>&
RedisOperationDuration();

// ---------------------------------------------------------------------------
// Start the HTTP /metrics exposer on the given port.
// Must be called once after the process starts. Non-blocking (runs in its own
// thread inside prometheus-cpp).
// ---------------------------------------------------------------------------
void StartMetricsServer(int port);

}  // namespace jq::metrics
