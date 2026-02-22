#include "server/db/job_repository.h"

#include <cstddef>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <pqxx/pqxx>

#include "common/logging/logger.h"

namespace jq::db {

namespace {

// Convert pqxx::bytes (std::basic_string<std::byte>) → vector<uint8_t>.
std::vector<uint8_t> BinaryField(const pqxx::field& f) {
    if (f.is_null()) return {};
    auto bv = f.as<pqxx::bytes>();
    std::vector<uint8_t> out;
    out.reserve(bv.size());
    for (std::byte b : bv) out.push_back(static_cast<uint8_t>(b));
    return out;
}

// Convert vector<uint8_t> → pqxx::bytes.
pqxx::bytes ToBytes(const std::vector<uint8_t>& v) {
    pqxx::bytes out;
    out.reserve(v.size());
    for (uint8_t u : v) out.push_back(static_cast<std::byte>(u));
    return out;
}

// Parse a PostgreSQL timestamptz column (returned as epoch seconds via EXTRACT).
int64_t TimestampField(const pqxx::field& f) {
    if (f.is_null()) return 0;
    return static_cast<int64_t>(f.as<double>());
}

// Build a JobRow from a query result row.
JobRow RowToJob(const pqxx::row& r) {
    JobRow j;
    j.job_id        = r["job_id"].as<std::string>();
    j.queue_name    = r["queue_name"].as<std::string>();
    j.payload       = BinaryField(r["payload"]);
    j.priority      = r["priority"].as<int>();
    j.status        = r["status"].as<std::string>();
    j.max_retries   = r["max_retries"].as<int>();
    j.retry_count   = r["retry_count"].as<int>();
    j.worker_id     = r["worker_id"].is_null() ? "" : r["worker_id"].as<std::string>();
    j.result        = BinaryField(r["result"]);
    j.error_message = r["error_message"].is_null() ? "" : r["error_message"].as<std::string>();
    j.created_at    = TimestampField(r["created_epoch"]);
    j.started_at    = TimestampField(r["started_epoch"]);
    j.completed_at  = TimestampField(r["completed_epoch"]);
    j.not_before    = TimestampField(r["not_before_epoch"]);
    return j;
}

// Jobs SELECT projection — aliases timestamp columns to epoch seconds.
constexpr const char* kJobSelect =
    "SELECT job_id::text, queue_name, payload, priority, status::text, "
    "       max_retries, retry_count, "
    "       worker_id::text, result, error_message, "
    "       EXTRACT(EPOCH FROM created_at)   AS created_epoch, "
    "       EXTRACT(EPOCH FROM started_at)   AS started_epoch, "
    "       EXTRACT(EPOCH FROM completed_at) AS completed_epoch, "
    "       EXTRACT(EPOCH FROM not_before)   AS not_before_epoch "
    "FROM jobs ";

}  // namespace

// ---------------------------------------------------------------------------
// QueueExists
// ---------------------------------------------------------------------------

bool JobRepository::QueueExists(const std::string& queue_name) {
    try {
        auto c = Conn();
        pqxx::work txn(c.get());
        auto r = txn.exec(
            "SELECT 1 FROM queues WHERE name = " + txn.quote(queue_name));
        txn.commit();
        return !r.empty();
    } catch (const std::exception& e) {
        LOG_ERROR("QueueExists query failed",
                  {{"queue", queue_name}, {"error", e.what()}});
        throw;
    }
}

// ---------------------------------------------------------------------------
// GetQueueMaxRetries
// ---------------------------------------------------------------------------

int JobRepository::GetQueueMaxRetries(const std::string& queue_name) {
    try {
        auto c = Conn();
        pqxx::work txn(c.get());
        auto r = txn.exec(
            "SELECT max_retries FROM queues WHERE name = " + txn.quote(queue_name));
        txn.commit();
        if (r.empty()) return 3;
        return r[0][0].as<int>();
    } catch (const std::exception& e) {
        LOG_ERROR("GetQueueMaxRetries query failed",
                  {{"queue", queue_name}, {"error", e.what()}});
        return 3;
    }
}

// ---------------------------------------------------------------------------
// InsertJob
// ---------------------------------------------------------------------------

std::string JobRepository::InsertJob(const std::string&         queue_name,
                                      const std::vector<uint8_t>& payload,
                                      int                         priority,
                                      int                         max_retries) {
    auto c = Conn();
    pqxx::work txn(c.get());

    pqxx::bytes payload_bytes = ToBytes(payload);

    auto r = txn.exec(
        "INSERT INTO jobs (queue_name, payload, priority, max_retries, status) "
        "VALUES (" +
        txn.quote(queue_name) + ", " +
        txn.quote(payload_bytes) + ", " +
        txn.quote(priority) + ", " +
        txn.quote(max_retries) + ", 'PENDING') "
        "RETURNING job_id::text");

    const std::string job_id = r[0][0].as<std::string>();

    // Insert initial job_event (from_status = NULL → PENDING)
    txn.exec(
        "INSERT INTO job_events (job_id, from_status, to_status, reason) "
        "VALUES (" + txn.quote(job_id) + "::uuid, NULL, 'PENDING', 'SUBMITTED')");

    txn.commit();
    return job_id;
}

// ---------------------------------------------------------------------------
// FindJobById
// ---------------------------------------------------------------------------

std::optional<JobRow> JobRepository::FindJobById(const std::string& job_id) {
    try {
        auto c = Conn();
        pqxx::work txn(c.get());
        std::string sql = std::string(kJobSelect) +
            "WHERE job_id = " + txn.quote(job_id) + "::uuid";
        auto r = txn.exec(sql);
        txn.commit();
        if (r.empty()) return std::nullopt;
        return RowToJob(r[0]);
    } catch (const std::exception& e) {
        LOG_ERROR("FindJobById failed",
                  {{"job_id", job_id}, {"error", e.what()}});
        throw;
    }
}

// ---------------------------------------------------------------------------
// TransitionJobStatus
// ---------------------------------------------------------------------------

bool JobRepository::TransitionJobStatus(const std::string& job_id,
                                         const std::string& expected_from_status,
                                         const std::string& new_status,
                                         const std::string& reason,
                                         const std::string& worker_id) {
    try {
        auto c = Conn();
        pqxx::work txn(c.get());

        // Timestamp column to set depends on new_status.
        std::string ts_col;
        if (new_status == "RUNNING") {
            ts_col = ", started_at = now()";
        } else if (new_status == "DONE" || new_status == "FAILED" || new_status == "DEAD_LETTERED") {
            ts_col = ", completed_at = now()";
        }

        auto r = txn.exec(
            "UPDATE jobs SET status = '" + new_status + "'::job_status" +
            ts_col + ", updated_at = now() "
            "WHERE job_id = " + txn.quote(job_id) + "::uuid "
            "  AND status = '" + expected_from_status + "'::job_status "
            "RETURNING job_id");

        if (r.empty()) {
            txn.abort();
            return false;
        }

        // Insert job_event row
        std::string extra_col = worker_id.empty() ? std::string{} : std::string{", worker_id"};
        std::string extra_val = worker_id.empty() ? std::string{} : (", " + txn.quote(worker_id) + "::uuid");
        std::string event_sql =
            "INSERT INTO job_events (job_id, from_status, to_status, reason" + extra_col + ") "
            "VALUES (" + txn.quote(job_id) + "::uuid, "
            "'" + expected_from_status + "'::job_status, "
            "'" + new_status + "'::job_status, "
            + txn.quote(reason) + extra_val + ")";
        txn.exec(event_sql);

        txn.commit();
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("TransitionJobStatus failed",
                  {{"job_id", job_id}, {"to", new_status}, {"error", e.what()}});
        throw;
    }
}

// ---------------------------------------------------------------------------
// ResetJobForRetry
// ---------------------------------------------------------------------------

bool JobRepository::ResetJobForRetry(const std::string& job_id,
                                      const std::string& expected_from_status) {
    try {
        auto c = Conn();
        pqxx::work txn(c.get());

        auto r = txn.exec(
            "UPDATE jobs "
            "SET status = 'PENDING', retry_count = 0, not_before = now(), "
            "    completed_at = NULL, updated_at = now() "
            "WHERE job_id = " + txn.quote(job_id) + "::uuid "
            "  AND status = '" + expected_from_status + "'::job_status "
            "RETURNING job_id");

        if (r.empty()) {
            txn.abort();
            return false;
        }

        txn.exec(
            "INSERT INTO job_events (job_id, from_status, to_status, reason) "
            "VALUES (" + txn.quote(job_id) + "::uuid, "
            "'" + expected_from_status + "'::job_status, "
            "'PENDING', 'MANUAL_RETRY')");

        txn.commit();
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("ResetJobForRetry failed",
                  {{"job_id", job_id}, {"error", e.what()}});
        throw;
    }
}

// ---------------------------------------------------------------------------
// ListJobs
// ---------------------------------------------------------------------------

std::vector<JobRow> JobRepository::ListJobs(const std::string& queue_name,
                                             const std::string& status_filter,
                                             int                limit,
                                             int                offset) {
    try {
        auto c = Conn();
        pqxx::work txn(c.get());

        std::string sql = std::string(kJobSelect);
        std::string where;
        if (!queue_name.empty()) {
            where += "queue_name = " + txn.quote(queue_name);
        }
        if (!status_filter.empty()) {
            if (!where.empty()) where += " AND ";
            where += "status = '" + status_filter + "'::job_status";
        }
        if (!where.empty()) sql += "WHERE " + where + " ";
        sql += "ORDER BY created_at DESC LIMIT " + std::to_string(limit) +
               " OFFSET " + std::to_string(offset);

        auto r = txn.exec(sql);
        txn.commit();

        std::vector<JobRow> jobs;
        jobs.reserve(r.size());
        for (const auto& row : r) jobs.push_back(RowToJob(row));
        return jobs;
    } catch (const std::exception& e) {
        LOG_ERROR("ListJobs failed", {{"error", e.what()}});
        throw;
    }
}

// ---------------------------------------------------------------------------
// ListJobEvents
// ---------------------------------------------------------------------------

std::vector<JobEventRow> JobRepository::ListJobEvents(const std::string& job_id) {
    try {
        auto c = Conn();
        pqxx::work txn(c.get());

        auto r = txn.exec(
            "SELECT id, job_id::text, "
            "       COALESCE(from_status::text, '') AS from_status, "
            "       to_status::text, "
            "       COALESCE(worker_id::text, '') AS worker_id, "
            "       COALESCE(reason, '') AS reason, "
            "       EXTRACT(EPOCH FROM occurred_at) AS occurred_epoch "
            "FROM job_events "
            "WHERE job_id = " + txn.quote(job_id) + "::uuid "
            "ORDER BY occurred_at ASC");

        txn.commit();

        std::vector<JobEventRow> events;
        events.reserve(r.size());
        for (const auto& row : r) {
            JobEventRow e;
            e.id          = row["id"].as<int64_t>();
            e.job_id      = row["job_id"].as<std::string>();
            e.from_status = row["from_status"].as<std::string>();
            e.to_status   = row["to_status"].as<std::string>();
            e.worker_id   = row["worker_id"].as<std::string>();
            e.reason      = row["reason"].as<std::string>();
            e.occurred_at = TimestampField(row["occurred_epoch"]);
            events.push_back(std::move(e));
        }
        return events;
    } catch (const std::exception& e) {
        LOG_ERROR("ListJobEvents failed",
                  {{"job_id", job_id}, {"error", e.what()}});
        throw;
    }
}

}  // namespace jq::db
