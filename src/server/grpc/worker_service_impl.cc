#include "server/grpc/worker_service_impl.h"

#include <chrono>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <grpcpp/grpcpp.h>
#include <prometheus/histogram.h>

#include "common/logging/logger.h"
#include "common/metrics/metrics.h"
#include "server/scheduler/scheduler.h"

namespace jq {

namespace {

// Job durations span sub-second subprocess calls to long-running work.
const prometheus::Histogram::BucketBoundaries kJobDurationBuckets = {
    0.1, 0.5, 1, 2.5, 5, 10, 30, 60, 120, 300, 600
};

}  // namespace

WorkerServiceImpl::WorkerServiceImpl(db::IJobRepository&    job_repo,
                                     db::IWorkerRepository& worker_repo,
                                     WorkerRegistry&        registry)
    : job_repo_(job_repo),
      worker_repo_(worker_repo),
      registry_(registry) {}

// ---------------------------------------------------------------------------
// RegisterWorker
// ---------------------------------------------------------------------------

grpc::Status WorkerServiceImpl::RegisterWorker(grpc::ServerContext*         /*ctx*/,
                                               const RegisterWorkerRequest* req,
                                               RegisterWorkerResponse*      resp) {
    if (req->worker_id().empty() && req->hostname().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "worker_id or hostname is required");
    }

    std::string effective_id;
    try {
        effective_id = worker_repo_.UpsertWorker(
            req->worker_id(), req->hostname(), req->concurrency());
    } catch (const std::exception& e) {
        LOG_ERROR("RegisterWorker: UpsertWorker failed", {{"error", e.what()}});
        return grpc::Status(grpc::StatusCode::INTERNAL, "database error");
    }

    resp->set_worker_id(effective_id);
    LOG_INFO("Worker registered",
             {{"worker_id", effective_id}, {"hostname", req->hostname()}});
    return grpc::Status::OK;
}

// ---------------------------------------------------------------------------
// Heartbeat
// ---------------------------------------------------------------------------

grpc::Status WorkerServiceImpl::Heartbeat(grpc::ServerContext*    /*ctx*/,
                                          const HeartbeatRequest* req,
                                          HeartbeatResponse*      /*resp*/) {
    if (req->worker_id().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "worker_id is required");
    }

    bool found = false;
    try {
        found = worker_repo_.UpdateWorkerHeartbeat(req->worker_id());
    } catch (const std::exception& e) {
        LOG_ERROR("Heartbeat: DB error",
                  {{"worker_id", req->worker_id()}, {"error", e.what()}});
        return grpc::Status(grpc::StatusCode::INTERNAL, "database error");
    }

    if (!found) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND,
                            "worker '" + req->worker_id() + "' not found");
    }
    return grpc::Status::OK;
}

// ---------------------------------------------------------------------------
// StreamJobs
// ---------------------------------------------------------------------------

grpc::Status WorkerServiceImpl::StreamJobs(grpc::ServerContext*               ctx,
                                           const StreamJobsRequest*           req,
                                           grpc::ServerWriter<JobAssignment>* writer) {
    if (req->worker_id().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "worker_id is required");
    }

    const std::vector<std::string> queues(req->queues().begin(), req->queues().end());

    // RegisterWorker (called before the worker opens this stream, see
    // Worker::Run()) already persisted the worker's real concurrency to the
    // DB -- look it up rather than hardcoding a default, since this value is
    // what actually gates AssignJob's per-worker capacity check.
    int concurrency = 4;
    if (auto worker = worker_repo_.FindWorkerById(req->worker_id())) {
        concurrency = worker->concurrency;
    }

    // Register this stream in the WorkerRegistry using a lambda write callback.
    // This avoids subclassing the final grpc::ServerWriter while keeping the
    // registry testable without a real gRPC channel.
    auto handle = registry_.RegisterStream(
        req->worker_id(), queues, concurrency,
        [writer](const JobAssignment& msg) { return writer->Write(msg); });

    LOG_INFO("Worker StreamJobs open", {{"worker_id", req->worker_id()}});

    // Block until client disconnects or Deregister marks the handle inactive.
    // Poll every 100ms — low CPU cost since workers are long-lived.
    while (!ctx->IsCancelled()) {
        {
            std::lock_guard<std::mutex> lock(handle->mu);
            if (!handle->active) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Mark handle inactive *under the stream mutex* so the scheduler cannot
    // write to a dead stream after this RPC returns.
    {
        std::lock_guard<std::mutex> lock(handle->mu);
        handle->active = false;
        handle->write_fn = nullptr;
    }

    registry_.RemoveWorker(req->worker_id());

    LOG_INFO("Worker StreamJobs closed", {{"worker_id", req->worker_id()}});
    return grpc::Status::OK;
}

// ---------------------------------------------------------------------------
// ReportResult
// ---------------------------------------------------------------------------

grpc::Status WorkerServiceImpl::ReportResult(grpc::ServerContext*       /*ctx*/,
                                             const ReportResultRequest* req,
                                             ReportResultResponse*      /*resp*/) {
    if (req->job_id().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "job_id is required");
    }
    if (req->worker_id().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "worker_id is required");
    }

    // Verify the job exists and is in RUNNING state.
    std::optional<db::JobRow> job;
    try {
        job = job_repo_.FindJobById(req->job_id());
    } catch (const std::exception& e) {
        LOG_ERROR("ReportResult: FindJobById failed",
                  {{"job_id", req->job_id()}, {"error", e.what()}});
        return grpc::Status(grpc::StatusCode::INTERNAL, "database error");
    }
    if (!job) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND,
                            "job '" + req->job_id() + "' not found");
    }
    // If job is still ASSIGNED (worker received it but hasn't transitioned yet),
    // auto-advance to RUNNING now so StoreJobResult can accept the final status.
    // There's no separate "job started" RPC, so this is the only place a job
    // is ever transitioned to RUNNING — job->started_at (fetched above, before
    // this call) is therefore always 0 here and can't be used directly; track
    // the moment we set it instead.
    int64_t started_at = job->started_at;
    if (job->status == "ASSIGNED") {
        try {
            job_repo_.TransitionJobStatus(
                req->job_id(), "ASSIGNED", "RUNNING", "STARTED", req->worker_id());
            started_at = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        } catch (const std::exception& e) {
            LOG_ERROR("ReportResult: ASSIGNED→RUNNING failed",
                      {{"job_id", req->job_id()}, {"error", e.what()}});
            return grpc::Status(grpc::StatusCode::INTERNAL, "database error");
        }
    } else if (job->status != "RUNNING") {
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                            "job is not RUNNING (status=" + job->status + ")");
    }

    // Store result: RUNNING → DONE or FAILED.
    const std::string& rb = req->result();
    std::vector<uint8_t> result_bytes(rb.begin(), rb.end());

    bool ok = false;
    try {
        ok = job_repo_.StoreJobResult(req->job_id(), req->success(),
                                       result_bytes, req->error_message(),
                                       req->worker_id());
    } catch (const std::exception& e) {
        LOG_ERROR("ReportResult: StoreJobResult failed",
                  {{"job_id", req->job_id()}, {"error", e.what()}});
        return grpc::Status(grpc::StatusCode::INTERNAL, "database error");
    }
    if (!ok) {
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                            "job state changed concurrently");
    }

    // Decrement the worker's active count in the registry.
    registry_.DecrementActiveCount(req->worker_id());

    // started_at has whole-second granularity (stored/derived as epoch
    // seconds), so sub-second jobs observe as ~0 — the best available
    // precision without a schema change.
    const int64_t completion_epoch = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (started_at > 0) {
        metrics::JobProcessingDuration()
            .Add({{"queue", job->queue_name}}, kJobDurationBuckets)
            .Observe(static_cast<double>(completion_epoch - started_at));
    }

    if (req->success()) {
        metrics::JobTotal().Add({{"queue", job->queue_name}, {"status", "DONE"}}).Increment();
        LOG_INFO("Job completed", {{"job_id", req->job_id()}});
    } else {
        metrics::JobTotal().Add({{"queue", job->queue_name}, {"status", "FAILED"}}).Increment();
        // Failure — apply retry logic.
        // job->retry_count is the count *before* this attempt.
        if (job->retry_count < job->max_retries) {
            const int new_retry_count = job->retry_count + 1;

            thread_local std::mt19937 rng{std::random_device{}()};
            std::uniform_int_distribution<int> jitter_dist(0, 4);
            const int jitter_s = jitter_dist(rng);

            const int64_t now_epoch =
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();

            const int64_t not_before = CalculateRetryNotBefore(
                now_epoch, new_retry_count, 5, 300, jitter_s);

            try {
                job_repo_.SetJobRetry(req->job_id(), new_retry_count, not_before);
                LOG_INFO("Job failure; retry scheduled",
                         {{"job_id", req->job_id()}, {"retry", new_retry_count}});
            } catch (const std::exception& e) {
                LOG_ERROR("SetJobRetry failed",
                          {{"job_id", req->job_id()}, {"error", e.what()}});
            }
        } else {
            // Dead-letter.
            try {
                job_repo_.TransitionJobStatus(
                    req->job_id(), "FAILED", "DEAD_LETTERED", "MAX_RETRIES_EXCEEDED",
                    req->worker_id());
                metrics::JobTotal()
                    .Add({{"queue", job->queue_name}, {"status", "DEAD_LETTERED"}})
                    .Increment();
                LOG_INFO("Job dead-lettered", {{"job_id", req->job_id()}});
            } catch (const std::exception& e) {
                LOG_ERROR("Dead-letter transition failed",
                          {{"job_id", req->job_id()}, {"error", e.what()}});
            }
        }
    }

    return grpc::Status::OK;
}

// ---------------------------------------------------------------------------
// Deregister
// ---------------------------------------------------------------------------

grpc::Status WorkerServiceImpl::Deregister(grpc::ServerContext*     /*ctx*/,
                                           const DeregisterRequest* req,
                                           DeregisterResponse*      /*resp*/) {
    if (req->worker_id().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "worker_id is required");
    }

    try {
        worker_repo_.SetWorkerStatus(req->worker_id(), "OFFLINE");
    } catch (const std::exception& e) {
        LOG_ERROR("Deregister: SetWorkerStatus failed",
                  {{"worker_id", req->worker_id()}, {"error", e.what()}});
        return grpc::Status(grpc::StatusCode::INTERNAL, "database error");
    }

    // Signal StreamJobs to exit by marking the handle inactive.
    // RemoveWorker also removes the entry from the registry.
    // StreamJobs polls handle->active and will exit shortly after.
    registry_.RemoveWorker(req->worker_id());

    LOG_INFO("Worker deregistered", {{"worker_id", req->worker_id()}});
    return grpc::Status::OK;
}

}  // namespace jq
