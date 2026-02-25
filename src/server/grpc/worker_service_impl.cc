#include "server/grpc/worker_service_impl.h"

#include <chrono>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "common.pb.h"
#include "common/logging/logger.h"
#include "server/scheduler/scheduler.h"

namespace jq {

WorkerServiceImpl::WorkerServiceImpl(db::IJobRepository&    job_repo,
                                     db::IWorkerRepository& worker_repo,
                                     IKafkaProducer&        kafka,
                                     WorkerRegistry&        registry)
    : job_repo_(job_repo),
      worker_repo_(worker_repo),
      kafka_(kafka),
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
    const int concurrency = 4;  // default; RegisterWorker sets the DB value

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
    if (job->status == "ASSIGNED") {
        try {
            job_repo_.TransitionJobStatus(
                req->job_id(), "ASSIGNED", "RUNNING", "STARTED", req->worker_id());
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

    if (req->success()) {
        // Publish job.completed event.
        JobEvent ev;
        ev.set_job_id(req->job_id());
        ev.set_from_status(RUNNING);
        ev.set_to_status(DONE);
        ev.set_reason("SUCCESS");
        ev.set_worker_id(req->worker_id());
        PublishEvent(KafkaTopics::kJobCompleted, ev);

        LOG_INFO("Job completed", {{"job_id", req->job_id()}});
    } else {
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
                JobEvent ev;
                ev.set_job_id(req->job_id());
                ev.set_from_status(FAILED);
                ev.set_to_status(DEAD_LETTERED);
                ev.set_reason("MAX_RETRIES_EXCEEDED");
                ev.set_worker_id(req->worker_id());
                PublishEvent(KafkaTopics::kJobDeadLettered, ev);
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

// ---------------------------------------------------------------------------
// PublishEvent
// ---------------------------------------------------------------------------

void WorkerServiceImpl::PublishEvent(const std::string& topic, const JobEvent& ev) {
    try {
        std::string serialized;
        ev.SerializeToString(&serialized);
        std::vector<uint8_t> payload(serialized.begin(), serialized.end());
        kafka_.Publish(topic, ev.job_id(), payload);
    } catch (const std::exception& e) {
        LOG_WARN("Failed to publish Kafka event",
                 {{"topic", topic}, {"job_id", ev.job_id()}, {"error", e.what()}});
    }
}

}  // namespace jq
