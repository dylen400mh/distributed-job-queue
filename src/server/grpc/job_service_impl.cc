#include "server/grpc/job_service_impl.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include <google/protobuf/timestamp.pb.h>
#include <grpcpp/grpcpp.h>

#include "common.pb.h"
#include "common/logging/logger.h"

namespace jq {

namespace {

// Populate a proto Timestamp from Unix epoch seconds (int64_t).
void SetTimestamp(google::protobuf::Timestamp* ts, int64_t epoch_secs) {
    if (epoch_secs == 0) return;
    ts->set_seconds(epoch_secs);
    ts->set_nanos(0);
}

// Clamp list limit to [1, 100] per FR-032.
int ClampLimit(int requested) {
    if (requested <= 0) return 20;
    return std::min(requested, 100);
}

}  // namespace

// ---------------------------------------------------------------------------

JobServiceImpl::JobServiceImpl(db::IJobRepository& repo, IKafkaProducer& kafka)
    : repo_(repo), kafka_(kafka) {}

// ---------------------------------------------------------------------------
// SubmitJob
// ---------------------------------------------------------------------------

grpc::Status JobServiceImpl::SubmitJob(grpc::ServerContext*    /*ctx*/,
                                        const SubmitJobRequest* req,
                                        SubmitJobResponse*      resp) {
    if (req->queue_name().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "queue_name is required");
    }

    // FR-006: reject submissions to non-existent queues.
    bool exists = false;
    try {
        exists = repo_.QueueExists(req->queue_name());
    } catch (const std::exception& e) {
        LOG_ERROR("SubmitJob: QueueExists DB error", {{"error", e.what()}});
        return grpc::Status(grpc::StatusCode::INTERNAL, "database error");
    }
    if (!exists) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND,
                            "queue '" + req->queue_name() + "' not found");
    }

    // Resolve max_retries: use queue default if caller omitted (0).
    int max_retries = req->max_retries();
    if (max_retries <= 0) {
        try {
            max_retries = repo_.GetQueueMaxRetries(req->queue_name());
        } catch (...) {
            max_retries = 3;
        }
    }

    // Convert payload bytes → uint8_t vector
    const std::string& pb = req->payload();
    std::vector<uint8_t> payload(pb.begin(), pb.end());

    std::string job_id;
    try {
        job_id = repo_.InsertJob(req->queue_name(), payload,
                                  req->priority(), max_retries);
    } catch (const std::exception& e) {
        LOG_ERROR("SubmitJob: InsertJob failed", {{"error", e.what()}});
        return grpc::Status(grpc::StatusCode::INTERNAL, "failed to persist job");
    }

    // Publish job.submitted Kafka event (non-critical).
    JobEvent ev;
    ev.set_job_id(job_id);
    ev.set_to_status(PENDING);
    ev.set_reason("SUBMITTED");
    PublishEvent(KafkaTopics::kJobSubmitted, ev);

    LOG_INFO("Job submitted", {{"job_id", job_id}, {"queue", req->queue_name()}});
    resp->set_job_id(job_id);
    return grpc::Status::OK;
}

// ---------------------------------------------------------------------------
// CancelJob
// ---------------------------------------------------------------------------

grpc::Status JobServiceImpl::CancelJob(grpc::ServerContext*    /*ctx*/,
                                        const CancelJobRequest* req,
                                        CancelJobResponse*      /*resp*/) {
    if (req->job_id().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "job_id is required");
    }

    std::optional<db::JobRow> job;
    try {
        job = repo_.FindJobById(req->job_id());
    } catch (const std::exception& e) {
        LOG_ERROR("CancelJob: FindJobById failed",
                  {{"job_id", req->job_id()}, {"error", e.what()}});
        return grpc::Status(grpc::StatusCode::INTERNAL, "database error");
    }
    if (!job) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND,
                            "job '" + req->job_id() + "' not found");
    }

    // FR-012: only PENDING or ASSIGNED jobs may be cancelled.
    if (job->status != "PENDING" && job->status != "ASSIGNED") {
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                            "job is not in a cancellable state (status=" + job->status + ")");
    }

    bool ok = false;
    try {
        ok = repo_.TransitionJobStatus(req->job_id(), job->status,
                                        "DEAD_LETTERED", "CANCELLED");
    } catch (const std::exception& e) {
        LOG_ERROR("CancelJob: TransitionJobStatus failed",
                  {{"job_id", req->job_id()}, {"error", e.what()}});
        return grpc::Status(grpc::StatusCode::INTERNAL, "database error");
    }
    if (!ok) {
        // Concurrent state change — treat as precondition failure.
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                            "job state changed concurrently; cannot cancel");
    }

    // Publish job.dead-lettered event.
    JobEvent ev;
    ev.set_job_id(req->job_id());
    ev.set_from_status(StatusFromString(job->status));
    ev.set_to_status(DEAD_LETTERED);
    ev.set_reason("CANCELLED");
    PublishEvent(KafkaTopics::kJobDeadLettered, ev);

    LOG_INFO("Job cancelled", {{"job_id", req->job_id()}});
    return grpc::Status::OK;
}

// ---------------------------------------------------------------------------
// GetJobStatus
// ---------------------------------------------------------------------------

grpc::Status JobServiceImpl::GetJobStatus(grpc::ServerContext*       /*ctx*/,
                                           const GetJobStatusRequest* req,
                                           GetJobStatusResponse*      resp) {
    if (req->job_id().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "job_id is required");
    }

    std::optional<db::JobRow> job;
    try {
        job = repo_.FindJobById(req->job_id());
    } catch (const std::exception& e) {
        LOG_ERROR("GetJobStatus failed",
                  {{"job_id", req->job_id()}, {"error", e.what()}});
        return grpc::Status(grpc::StatusCode::INTERNAL, "database error");
    }
    if (!job) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND,
                            "job '" + req->job_id() + "' not found");
    }

    *resp->mutable_job() = JobRowToProto(*job);
    return grpc::Status::OK;
}

// ---------------------------------------------------------------------------
// ListJobs
// ---------------------------------------------------------------------------

grpc::Status JobServiceImpl::ListJobs(grpc::ServerContext*  /*ctx*/,
                                       const ListJobsRequest* req,
                                       ListJobsResponse*      resp) {
    const int limit  = ClampLimit(req->limit());
    int       offset = 0;

    // Decode page token (simple base-10 offset).
    if (!req->page_token().empty()) {
        try { offset = std::stoi(req->page_token()); } catch (...) { offset = 0; }
    }

    // Map proto JobStatus → string; UNSPECIFIED means "no filter".
    std::string status_filter;
    if (req->status() != JOB_STATUS_UNSPECIFIED) {
        status_filter = JobStatus_Name(req->status());
    }

    std::vector<db::JobRow> jobs;
    try {
        jobs = repo_.ListJobs(req->queue_name(), status_filter, limit + 1, offset);
    } catch (const std::exception& e) {
        LOG_ERROR("ListJobs failed", {{"error", e.what()}});
        return grpc::Status(grpc::StatusCode::INTERNAL, "database error");
    }

    // If we got limit+1 rows, there are more pages.
    bool has_more = (static_cast<int>(jobs.size()) > limit);
    if (has_more) jobs.resize(limit);

    for (const auto& row : jobs) {
        *resp->add_jobs() = JobRowToProto(row);
    }
    if (has_more) {
        resp->set_next_page_token(std::to_string(offset + limit));
    }

    return grpc::Status::OK;
}

// ---------------------------------------------------------------------------
// GetJobLogs
// ---------------------------------------------------------------------------

grpc::Status JobServiceImpl::GetJobLogs(grpc::ServerContext*      /*ctx*/,
                                         const GetJobLogsRequest*  req,
                                         GetJobLogsResponse*       resp) {
    if (req->job_id().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "job_id is required");
    }

    std::vector<db::JobEventRow> events;
    try {
        events = repo_.ListJobEvents(req->job_id());
    } catch (const std::exception& e) {
        LOG_ERROR("GetJobLogs failed",
                  {{"job_id", req->job_id()}, {"error", e.what()}});
        return grpc::Status(grpc::StatusCode::INTERNAL, "database error");
    }

    for (const auto& row : events) {
        *resp->add_events() = EventRowToProto(row);
    }
    return grpc::Status::OK;
}

// ---------------------------------------------------------------------------
// RetryJob
// ---------------------------------------------------------------------------

grpc::Status JobServiceImpl::RetryJob(grpc::ServerContext*    /*ctx*/,
                                       const RetryJobRequest*  req,
                                       RetryJobResponse*       resp) {
    if (req->job_id().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "job_id is required");
    }

    std::optional<db::JobRow> job;
    try {
        job = repo_.FindJobById(req->job_id());
    } catch (const std::exception& e) {
        LOG_ERROR("RetryJob: FindJobById failed",
                  {{"job_id", req->job_id()}, {"error", e.what()}});
        return grpc::Status(grpc::StatusCode::INTERNAL, "database error");
    }
    if (!job) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND,
                            "job '" + req->job_id() + "' not found");
    }

    // FR-013: only FAILED or DEAD_LETTERED jobs may be manually retried.
    if (job->status != "FAILED" && job->status != "DEAD_LETTERED") {
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                            "job is not in a retriable state (status=" + job->status + ")");
    }

    bool ok = false;
    try {
        ok = repo_.ResetJobForRetry(req->job_id(), job->status);
    } catch (const std::exception& e) {
        LOG_ERROR("RetryJob: ResetJobForRetry failed",
                  {{"job_id", req->job_id()}, {"error", e.what()}});
        return grpc::Status(grpc::StatusCode::INTERNAL, "database error");
    }
    if (!ok) {
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                            "job state changed concurrently; cannot retry");
    }

    LOG_INFO("Job retried", {{"job_id", req->job_id()}});
    resp->set_job_id(req->job_id());
    return grpc::Status::OK;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void JobServiceImpl::PublishEvent(const std::string& topic, const JobEvent& event) {
    try {
        std::string serialized;
        event.SerializeToString(&serialized);
        std::vector<uint8_t> payload(serialized.begin(), serialized.end());
        kafka_.Publish(topic, event.job_id(), payload);
    } catch (const std::exception& e) {
        // FR-038: Kafka unavailability must not block job processing.
        LOG_WARN("Failed to publish Kafka event",
                 {{"topic", topic}, {"job_id", event.job_id()}, {"error", e.what()}});
    }
}

Job JobServiceImpl::JobRowToProto(const db::JobRow& row) {
    Job j;
    j.set_job_id(row.job_id);
    j.set_queue_name(row.queue_name);
    j.set_payload(row.payload.data(), row.payload.size());
    j.set_priority(row.priority);
    j.set_status(StatusFromString(row.status));
    j.set_max_retries(row.max_retries);
    j.set_retry_count(row.retry_count);
    j.set_worker_id(row.worker_id);
    j.set_result(row.result.data(), row.result.size());
    j.set_error_message(row.error_message);
    if (row.created_at)   SetTimestamp(j.mutable_created_at(),   row.created_at);
    if (row.started_at)   SetTimestamp(j.mutable_started_at(),   row.started_at);
    if (row.completed_at) SetTimestamp(j.mutable_completed_at(), row.completed_at);
    if (row.not_before)   SetTimestamp(j.mutable_not_before(),   row.not_before);
    return j;
}

JobEvent JobServiceImpl::EventRowToProto(const db::JobEventRow& row) {
    JobEvent e;
    e.set_id(row.id);
    e.set_job_id(row.job_id);
    if (!row.from_status.empty()) e.set_from_status(StatusFromString(row.from_status));
    e.set_to_status(StatusFromString(row.to_status));
    e.set_worker_id(row.worker_id);
    e.set_reason(row.reason);
    if (row.occurred_at) SetTimestamp(e.mutable_occurred_at(), row.occurred_at);
    return e;
}

JobStatus JobServiceImpl::StatusFromString(const std::string& s) {
    JobStatus out;
    if (!JobStatus_Parse(s, &out)) return JOB_STATUS_UNSPECIFIED;
    return out;
}

}  // namespace jq
