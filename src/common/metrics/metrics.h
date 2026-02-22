#pragma once

#include <memory>
#include <prometheus/counter.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <prometheus/registry.h>

namespace jq::metrics {

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

// jq_kafka_publish_errors_total{topic} — Counter
prometheus::Family<prometheus::Counter>&
KafkaPublishErrorsTotal();

// jq_db_query_duration_seconds{query_name} — Histogram
prometheus::Family<prometheus::Histogram>&
DbQueryDuration();

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
