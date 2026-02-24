#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "common/db/repository.h"
#include "common/db/connection_pool.h"

namespace jq::db {

// ---------------------------------------------------------------------------
// QueueRow — plain data struct mirroring the queues table columns.
// ---------------------------------------------------------------------------
struct QueueRow {
    std::string name;
    int         max_retries = 3;
    int         ttl_seconds = 0;   // 0 means no TTL
    int64_t     created_at  = 0;   // Unix epoch seconds
};

// ---------------------------------------------------------------------------
// QueueStats — per-queue job counts + throughput metrics.
// ---------------------------------------------------------------------------
struct QueueStatsRow {
    std::string queue_name;
    int64_t     pending_count    = 0;
    int64_t     running_count    = 0;
    int64_t     failed_count     = 0;
    int64_t     done_count       = 0;
    int64_t     dead_letter_count = 0;
    int64_t     total_processed  = 0;
    double      avg_duration_ms  = 0.0;  // avg of (completed_at - started_at) for DONE jobs
    double      error_rate       = 0.0;  // failed / (done + failed)
};

// ---------------------------------------------------------------------------
// IQueueRepository — interface for mock injection in unit tests.
// ---------------------------------------------------------------------------
class IQueueRepository {
public:
    virtual ~IQueueRepository() = default;

    // Create a new queue; throws on duplicate name.
    virtual QueueRow CreateQueue(const std::string& name,
                                  int                max_retries,
                                  int                ttl_seconds) = 0;

    // Delete a queue.  If force=false and the queue has non-terminal jobs,
    // returns false rather than deleting.  Returns true on success.
    virtual bool DeleteQueue(const std::string& name, bool force) = 0;

    // List all queues ordered by name.
    virtual std::vector<QueueRow> ListQueues() = 0;

    // Fetch per-status job counts and throughput for a single queue.
    // Returns nullopt if the queue does not exist.
    virtual std::optional<QueueStatsRow> GetQueueStats(const std::string& name) = 0;
};

// ---------------------------------------------------------------------------
// QueueRepository — concrete implementation backed by libpqxx.
// ---------------------------------------------------------------------------
class QueueRepository : public Repository, public IQueueRepository {
public:
    explicit QueueRepository(ConnectionPool& pool) : Repository(pool) {}

    QueueRow CreateQueue(const std::string& name,
                          int                max_retries,
                          int                ttl_seconds) override;

    bool DeleteQueue(const std::string& name, bool force) override;

    std::vector<QueueRow> ListQueues() override;

    std::optional<QueueStatsRow> GetQueueStats(const std::string& name) override;
};

}  // namespace jq::db
