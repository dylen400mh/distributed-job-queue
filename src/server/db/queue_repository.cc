#include "server/db/queue_repository.h"

#include <stdexcept>
#include <string>

#include <pqxx/pqxx>

#include "common/logging/logger.h"

namespace jq::db {

namespace {

QueueRow RowToQueue(const pqxx::row& r) {
    QueueRow q;
    q.name        = r["name"].as<std::string>();
    q.max_retries = r["max_retries"].as<int>();
    q.ttl_seconds = r["ttl_seconds"].is_null() ? 0 : r["ttl_seconds"].as<int>();
    q.created_at  = r["created_epoch"].is_null()
                        ? 0
                        : static_cast<int64_t>(r["created_epoch"].as<double>());
    return q;
}

constexpr const char* kQueueSelect =
    "SELECT name, max_retries, ttl_seconds, "
    "       EXTRACT(EPOCH FROM created_at) AS created_epoch "
    "FROM queues ";

}  // namespace

// ---------------------------------------------------------------------------
// CreateQueue
// ---------------------------------------------------------------------------

QueueRow QueueRepository::CreateQueue(const std::string& name,
                                       int                max_retries,
                                       int                ttl_seconds) {
    try {
        auto c = Conn();
        pqxx::work txn(c.get());

        std::string ttl_sql = (ttl_seconds > 0)
            ? std::to_string(ttl_seconds)
            : "NULL";

        std::string sql =
            "INSERT INTO queues (name, max_retries, ttl_seconds) "
            "VALUES (" + txn.quote(name) + ", " +
            std::to_string(max_retries > 0 ? max_retries : 3) + ", " +
            ttl_sql + ") "
            "RETURNING name, max_retries, ttl_seconds, "
            "          EXTRACT(EPOCH FROM created_at) AS created_epoch";

        auto r = txn.exec(sql);
        txn.commit();
        return RowToQueue(r[0]);
    } catch (const pqxx::unique_violation& e) {
        throw std::runtime_error("queue '" + name + "' already exists");
    } catch (const std::exception& e) {
        LOG_ERROR("CreateQueue failed", {{"name", name}, {"error", e.what()}});
        throw;
    }
}

// ---------------------------------------------------------------------------
// DeleteQueue
// ---------------------------------------------------------------------------

bool QueueRepository::DeleteQueue(const std::string& name, bool force) {
    try {
        auto c = Conn();
        pqxx::work txn(c.get());

        if (!force) {
            // Check for non-terminal jobs (PENDING, ASSIGNED, RUNNING).
            std::string check_sql =
                "SELECT COUNT(*) FROM jobs "
                "WHERE queue_name = " + txn.quote(name) + " "
                "  AND status NOT IN ('DONE'::job_status, 'DEAD_LETTERED'::job_status)";
            auto r = txn.exec(check_sql);
            if (!r.empty() && r[0][0].as<int64_t>() > 0) {
                txn.commit();
                return false;  // non-empty, not forced
            }
        }

        txn.exec("DELETE FROM queues WHERE name = " + txn.quote(name));
        txn.commit();
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("DeleteQueue failed", {{"name", name}, {"error", e.what()}});
        throw;
    }
}

// ---------------------------------------------------------------------------
// ListQueues
// ---------------------------------------------------------------------------

std::vector<QueueRow> QueueRepository::ListQueues() {
    try {
        auto c = Conn();
        pqxx::work txn(c.get());
        auto r = txn.exec(std::string(kQueueSelect) + "ORDER BY name");
        txn.commit();
        std::vector<QueueRow> queues;
        queues.reserve(r.size());
        for (const auto& row : r) queues.push_back(RowToQueue(row));
        return queues;
    } catch (const std::exception& e) {
        LOG_ERROR("ListQueues failed", {{"error", e.what()}});
        throw;
    }
}

// ---------------------------------------------------------------------------
// GetQueueStats
// ---------------------------------------------------------------------------

std::optional<QueueStatsRow> QueueRepository::GetQueueStats(const std::string& name) {
    try {
        auto c = Conn();
        pqxx::work txn(c.get());

        // First check queue exists.
        auto qr = txn.exec(
            "SELECT 1 FROM queues WHERE name = " + txn.quote(name));
        if (qr.empty()) {
            txn.commit();
            return std::nullopt;
        }

        // Aggregate job counts and throughput.
        std::string sql =
            "SELECT "
            "  COUNT(*) FILTER (WHERE status = 'PENDING'::job_status)       AS pending, "
            "  COUNT(*) FILTER (WHERE status = 'RUNNING'::job_status)       AS running, "
            "  COUNT(*) FILTER (WHERE status = 'FAILED'::job_status)        AS failed, "
            "  COUNT(*) FILTER (WHERE status = 'DONE'::job_status)          AS done, "
            "  COUNT(*) FILTER (WHERE status = 'DEAD_LETTERED'::job_status) AS dead_letter, "
            "  COUNT(*) FILTER (WHERE status IN ('DONE'::job_status, 'FAILED'::job_status, "
            "                                    'DEAD_LETTERED'::job_status)) AS total, "
            "  COALESCE("
            "    AVG(EXTRACT(EPOCH FROM (completed_at - started_at)) * 1000.0) "
            "    FILTER (WHERE status = 'DONE'::job_status AND started_at IS NOT NULL "
            "                                               AND completed_at IS NOT NULL), "
            "    0.0) AS avg_ms, "
            "  CASE WHEN COUNT(*) FILTER (WHERE status IN ("
            "         'DONE'::job_status, 'FAILED'::job_status)) > 0 "
            "    THEN COUNT(*) FILTER (WHERE status = 'FAILED'::job_status)::float / "
            "         COUNT(*) FILTER (WHERE status IN ('DONE'::job_status, 'FAILED'::job_status)) "
            "    ELSE 0.0 END AS error_rate "
            "FROM jobs "
            "WHERE queue_name = " + txn.quote(name);

        auto r = txn.exec(sql);
        txn.commit();

        QueueStatsRow stats;
        stats.queue_name      = name;
        if (!r.empty()) {
            stats.pending_count    = r[0]["pending"].as<int64_t>(0);
            stats.running_count    = r[0]["running"].as<int64_t>(0);
            stats.failed_count     = r[0]["failed"].as<int64_t>(0);
            stats.done_count       = r[0]["done"].as<int64_t>(0);
            stats.dead_letter_count = r[0]["dead_letter"].as<int64_t>(0);
            stats.total_processed  = r[0]["total"].as<int64_t>(0);
            stats.avg_duration_ms  = r[0]["avg_ms"].as<double>(0.0);
            stats.error_rate       = r[0]["error_rate"].as<double>(0.0);
        }
        return stats;
    } catch (const std::exception& e) {
        LOG_ERROR("GetQueueStats failed", {{"name", name}, {"error", e.what()}});
        throw;
    }
}

}  // namespace jq::db
