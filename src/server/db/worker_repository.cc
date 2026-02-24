#include "server/db/worker_repository.h"

#include <stdexcept>
#include <string>

#include <pqxx/pqxx>

#include "common/logging/logger.h"

namespace jq::db {

namespace {

WorkerRow RowToWorker(const pqxx::row& r) {
    WorkerRow w;
    w.worker_id        = r["worker_id"].as<std::string>();
    w.hostname         = r["hostname"].as<std::string>();
    w.status           = r["status"].as<std::string>();
    w.concurrency      = r["concurrency"].as<int>();
    w.active_job_count = r["active_job_count"].as<int>();
    w.last_heartbeat   = r["heartbeat_epoch"].is_null()
                             ? 0
                             : static_cast<int64_t>(r["heartbeat_epoch"].as<double>());
    w.registered_at    = r["registered_epoch"].is_null()
                             ? 0
                             : static_cast<int64_t>(r["registered_epoch"].as<double>());
    return w;
}

constexpr const char* kWorkerSelect =
    "SELECT worker_id::text, hostname, status::text, concurrency, active_job_count, "
    "       EXTRACT(EPOCH FROM last_heartbeat) AS heartbeat_epoch, "
    "       EXTRACT(EPOCH FROM registered_at)  AS registered_epoch "
    "FROM workers ";

}  // namespace

// ---------------------------------------------------------------------------
// UpsertWorker
// ---------------------------------------------------------------------------

std::string WorkerRepository::UpsertWorker(const std::string& worker_id,
                                            const std::string& hostname,
                                            int                concurrency) {
    try {
        auto c = Conn();
        pqxx::work txn(c.get());

        std::string sql;
        if (worker_id.empty()) {
            // Server assigns a new UUID.
            sql =
                "INSERT INTO workers (hostname, concurrency, status, last_heartbeat) "
                "VALUES (" + txn.quote(hostname) + ", " +
                std::to_string(concurrency) + ", 'ONLINE', now()) "
                "RETURNING worker_id::text";
        } else {
            // Client provided a UUID; upsert (reconnect on restart).
            sql =
                "INSERT INTO workers (worker_id, hostname, concurrency, status, last_heartbeat) "
                "VALUES (" + txn.quote(worker_id) + "::uuid, " +
                txn.quote(hostname) + ", " +
                std::to_string(concurrency) + ", 'ONLINE', now()) "
                "ON CONFLICT (worker_id) DO UPDATE "
                "SET hostname = EXCLUDED.hostname, "
                "    concurrency = EXCLUDED.concurrency, "
                "    status = 'ONLINE', "
                "    last_heartbeat = now(), "
                "    updated_at = now() "
                "RETURNING worker_id::text";
        }

        auto r = txn.exec(sql);
        txn.commit();
        return r[0][0].as<std::string>();
    } catch (const std::exception& e) {
        LOG_ERROR("UpsertWorker failed",
                  {{"worker_id", worker_id}, {"error", e.what()}});
        throw;
    }
}

// ---------------------------------------------------------------------------
// FindWorkerById
// ---------------------------------------------------------------------------

std::optional<WorkerRow> WorkerRepository::FindWorkerById(const std::string& worker_id) {
    try {
        auto c = Conn();
        pqxx::work txn(c.get());
        std::string sql = std::string(kWorkerSelect) +
            "WHERE worker_id = " + txn.quote(worker_id) + "::uuid";
        auto r = txn.exec(sql);
        txn.commit();
        if (r.empty()) return std::nullopt;
        return RowToWorker(r[0]);
    } catch (const std::exception& e) {
        LOG_ERROR("FindWorkerById failed",
                  {{"worker_id", worker_id}, {"error", e.what()}});
        throw;
    }
}

// ---------------------------------------------------------------------------
// UpdateWorkerHeartbeat
// ---------------------------------------------------------------------------

bool WorkerRepository::UpdateWorkerHeartbeat(const std::string& worker_id) {
    try {
        auto c = Conn();
        pqxx::work txn(c.get());
        auto r = txn.exec(
            "UPDATE workers SET last_heartbeat = now(), updated_at = now() "
            "WHERE worker_id = " + txn.quote(worker_id) + "::uuid "
            "RETURNING worker_id");
        txn.commit();
        return !r.empty();
    } catch (const std::exception& e) {
        LOG_ERROR("UpdateWorkerHeartbeat failed",
                  {{"worker_id", worker_id}, {"error", e.what()}});
        throw;
    }
}

// ---------------------------------------------------------------------------
// SetWorkerStatus
// ---------------------------------------------------------------------------

void WorkerRepository::SetWorkerStatus(const std::string& worker_id,
                                        const std::string& status) {
    try {
        auto c = Conn();
        pqxx::work txn(c.get());
        txn.exec(
            "UPDATE workers SET status = '" + status + "'::worker_status, "
            "updated_at = now() "
            "WHERE worker_id = " + txn.quote(worker_id) + "::uuid");
        txn.commit();
    } catch (const std::exception& e) {
        LOG_ERROR("SetWorkerStatus failed",
                  {{"worker_id", worker_id}, {"status", status}, {"error", e.what()}});
        throw;
    }
}

// ---------------------------------------------------------------------------
// ListWorkers
// ---------------------------------------------------------------------------

std::vector<WorkerRow> WorkerRepository::ListWorkers() {
    try {
        auto c = Conn();
        pqxx::work txn(c.get());
        std::string sql = std::string(kWorkerSelect) +
            "ORDER BY registered_at DESC";
        auto r = txn.exec(sql);
        txn.commit();
        std::vector<WorkerRow> workers;
        workers.reserve(r.size());
        for (const auto& row : r) workers.push_back(RowToWorker(row));
        return workers;
    } catch (const std::exception& e) {
        LOG_ERROR("ListWorkers failed", {{"error", e.what()}});
        throw;
    }
}

// ---------------------------------------------------------------------------
// FetchStaleWorkers
// ---------------------------------------------------------------------------

std::vector<WorkerRow> WorkerRepository::FetchStaleWorkers(int timeout_s) {
    try {
        auto c = Conn();
        pqxx::work txn(c.get());
        std::string sql = std::string(kWorkerSelect) +
            "WHERE status = 'ONLINE'::worker_status "
            "  AND last_heartbeat < now() - interval '" +
            std::to_string(timeout_s) + " seconds'";
        auto r = txn.exec(sql);
        txn.commit();
        std::vector<WorkerRow> workers;
        workers.reserve(r.size());
        for (const auto& row : r) workers.push_back(RowToWorker(row));
        return workers;
    } catch (const std::exception& e) {
        LOG_ERROR("FetchStaleWorkers failed", {{"error", e.what()}});
        throw;
    }
}

}  // namespace jq::db
