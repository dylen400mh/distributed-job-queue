#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "common/db/repository.h"
#include "common/db/connection_pool.h"

namespace jq::db {

// ---------------------------------------------------------------------------
// WorkerRow — plain data struct mirroring the workers table columns.
// ---------------------------------------------------------------------------
struct WorkerRow {
    std::string worker_id;      // UUID as string
    std::string hostname;
    std::string status;         // "ONLINE" | "OFFLINE" | "DRAINING"
    int         concurrency     = 4;
    int         active_job_count = 0;
    int64_t     last_heartbeat  = 0;   // Unix epoch seconds
    int64_t     registered_at  = 0;
};

// ---------------------------------------------------------------------------
// IWorkerRepository — interface for mock injection in unit tests.
// ---------------------------------------------------------------------------
class IWorkerRepository {
public:
    virtual ~IWorkerRepository() = default;

    // Insert a new worker row or update hostname/concurrency/status/last_heartbeat
    // for an existing one (upsert on worker_id).
    // If worker_id is empty, the DB generates a UUID via gen_random_uuid().
    // Returns the effective worker_id (DB-assigned or provided).
    virtual std::string UpsertWorker(const std::string& worker_id,
                                      const std::string& hostname,
                                      int                concurrency) = 0;

    // Fetch a worker by ID. Returns nullopt if not found.
    virtual std::optional<WorkerRow> FindWorkerById(const std::string& worker_id) = 0;

    // Update workers.last_heartbeat to now().
    // Returns false if the worker is not found.
    virtual bool UpdateWorkerHeartbeat(const std::string& worker_id) = 0;

    // Set worker status ("ONLINE" | "OFFLINE" | "DRAINING").
    virtual void SetWorkerStatus(const std::string& worker_id,
                                  const std::string& status) = 0;

    // Fetch workers WHERE status = 'ONLINE' AND last_heartbeat < now() - timeout_s.
    virtual std::vector<WorkerRow> FetchStaleWorkers(int timeout_s) = 0;
};

// ---------------------------------------------------------------------------
// WorkerRepository — concrete implementation backed by libpqxx.
// ---------------------------------------------------------------------------
class WorkerRepository : public Repository, public IWorkerRepository {
public:
    explicit WorkerRepository(ConnectionPool& pool) : Repository(pool) {}

    std::string UpsertWorker(const std::string& worker_id,
                              const std::string& hostname,
                              int                concurrency) override;

    std::optional<WorkerRow> FindWorkerById(const std::string& worker_id) override;

    bool UpdateWorkerHeartbeat(const std::string& worker_id) override;

    void SetWorkerStatus(const std::string& worker_id,
                          const std::string& status) override;

    std::vector<WorkerRow> FetchStaleWorkers(int timeout_s) override;
};

}  // namespace jq::db
