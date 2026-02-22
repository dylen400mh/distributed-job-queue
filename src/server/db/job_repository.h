#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "common/db/repository.h"
#include "common/db/connection_pool.h"

namespace jq::db {

// ---------------------------------------------------------------------------
// Row types — plain data structs mirroring DB columns.
// Timestamps are stored as Unix epoch seconds (int64_t).
// ---------------------------------------------------------------------------

struct JobRow {
    std::string         job_id;
    std::string         queue_name;
    std::vector<uint8_t> payload;
    int                 priority      = 0;
    std::string         status;        // "PENDING" | "ASSIGNED" | "RUNNING" | "DONE" | "FAILED" | "DEAD_LETTERED"
    int                 max_retries   = 3;
    int                 retry_count   = 0;
    std::string         worker_id;     // empty if unset
    std::vector<uint8_t> result;
    std::string         error_message;
    int64_t             created_at    = 0;
    int64_t             started_at    = 0;   // 0 if unset
    int64_t             completed_at  = 0;   // 0 if unset
    int64_t             not_before    = 0;   // 0 if unset
};

struct JobEventRow {
    int64_t     id          = 0;
    std::string job_id;
    std::string from_status;  // empty for initial PENDING insertion
    std::string to_status;
    std::string worker_id;
    std::string reason;
    int64_t     occurred_at = 0;
};

// ---------------------------------------------------------------------------
// IJobRepository — interface for mock injection in unit tests.
// ---------------------------------------------------------------------------
class IJobRepository {
public:
    virtual ~IJobRepository() = default;

    // Returns true if a queue with the given name exists.
    virtual bool QueueExists(const std::string& queue_name) = 0;

    // Returns the max_retries configured for the queue, or 3 on error.
    virtual int GetQueueMaxRetries(const std::string& queue_name) = 0;

    // Insert a new job row (status=PENDING) and an initial job_event row
    // (from_status=NULL, to_status=PENDING) in the same transaction.
    // Returns the new job_id (UUID string).
    virtual std::string InsertJob(const std::string&         queue_name,
                                   const std::vector<uint8_t>& payload,
                                   int                         priority,
                                   int                         max_retries) = 0;

    // Fetch a single job by ID. Returns nullopt if not found.
    virtual std::optional<JobRow> FindJobById(const std::string& job_id) = 0;

    // Atomically update jobs.status and insert a job_events row in a single
    // transaction.  Returns false if the job was not found or the current
    // status does not match expected_from_status (optimistic concurrency check).
    virtual bool TransitionJobStatus(const std::string& job_id,
                                      const std::string& expected_from_status,
                                      const std::string& new_status,
                                      const std::string& reason,
                                      const std::string& worker_id = "") = 0;

    // Reset a FAILED/DEAD_LETTERED job to PENDING with retry_count=0 and
    // not_before=NOW(). Also inserts the job_event row. Returns false if
    // job not found or status mismatch.
    virtual bool ResetJobForRetry(const std::string& job_id,
                                   const std::string& expected_from_status) = 0;

    // List jobs with optional filters.  Pass empty string to skip a filter.
    virtual std::vector<JobRow> ListJobs(const std::string& queue_name,
                                          const std::string& status_filter,
                                          int                limit,
                                          int                offset) = 0;

    // Fetch all job_events for a job in chronological order.
    virtual std::vector<JobEventRow> ListJobEvents(const std::string& job_id) = 0;
};

// ---------------------------------------------------------------------------
// JobRepository — concrete implementation backed by libpqxx.
// ---------------------------------------------------------------------------
class JobRepository : public Repository, public IJobRepository {
public:
    explicit JobRepository(ConnectionPool& pool) : Repository(pool) {}

    bool QueueExists(const std::string& queue_name) override;
    int  GetQueueMaxRetries(const std::string& queue_name) override;

    std::string InsertJob(const std::string&         queue_name,
                           const std::vector<uint8_t>& payload,
                           int                         priority,
                           int                         max_retries) override;

    std::optional<JobRow> FindJobById(const std::string& job_id) override;

    bool TransitionJobStatus(const std::string& job_id,
                              const std::string& expected_from_status,
                              const std::string& new_status,
                              const std::string& reason,
                              const std::string& worker_id = "") override;

    bool ResetJobForRetry(const std::string& job_id,
                           const std::string& expected_from_status) override;

    std::vector<JobRow> ListJobs(const std::string& queue_name,
                                  const std::string& status_filter,
                                  int                limit,
                                  int                offset) override;

    std::vector<JobEventRow> ListJobEvents(const std::string& job_id) override;
};

}  // namespace jq::db
