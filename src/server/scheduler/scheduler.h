#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

#include "common/config/config.h"
#include "common/db/connection_pool.h"
#include "common/kafka/kafka_producer.h"
#include "server/db/job_repository.h"
#include "server/scheduler/worker_registry.h"

namespace jq {

// ---------------------------------------------------------------------------
// CalculateRetryNotBefore — pure backoff function (exposed for unit tests).
//
// Returns Unix epoch seconds for when the retry should next be attempted:
//   min(base_delay_s * 2^retry_count, max_delay_s) + uniform_jitter
//
// now_epoch — current time (seconds since epoch)
// retry_count — the new retry_count (after increment, i.e. attempt number)
// base_delay_s, max_delay_s — backoff parameters
// jitter_s — maximum random jitter in seconds (caller supplies a [0, jitter_s) value)
// ---------------------------------------------------------------------------
int64_t CalculateRetryNotBefore(int64_t now_epoch,
                                 int     retry_count,
                                 int     base_delay_s  = 5,
                                 int     max_delay_s   = 300,
                                 int     jitter_s      = 0);

// ---------------------------------------------------------------------------
// Scheduler — runs the job scheduling loop and heartbeat monitor.
//
// Two internal threads:
//   1. Scheduling loop  — runs every cfg.interval_ms
//   2. Heartbeat monitor — runs every 10 seconds
//
// Both threads own their own ConnectionPool connections and RedisClient
// instances (RedisClient is NOT thread-safe).
// ---------------------------------------------------------------------------
class Scheduler {
public:
    Scheduler(db::ConnectionPool& pool,
              const RedisConfig&  redis_cfg,
              IKafkaProducer&     kafka,
              WorkerRegistry&     registry,
              const SchedulerConfig& cfg);

    ~Scheduler();

    // Start both background threads. Returns immediately.
    void Start();

    // Signal both threads to stop and join them. Blocks until done.
    void Stop();

private:
    // Main scheduling loop (runs on loop_thread_).
    void RunLoop();

    // Heartbeat timeout monitor (runs on heartbeat_thread_).
    void HeartbeatMonitor();

    // Apply retry logic to a FAILED job.
    // Uses job_repo and kafka directly (no state captured by value).
    void ApplyRetry(db::IJobRepository& job_repo,
                    const std::string&  job_id,
                    int                 retry_count,
                    int                 max_retries);

    db::ConnectionPool&   pool_;
    const RedisConfig&    redis_cfg_;
    IKafkaProducer&       kafka_;
    WorkerRegistry&       registry_;
    SchedulerConfig       cfg_;

    std::atomic<bool> running_{false};
    std::thread       loop_thread_;
    std::thread       heartbeat_thread_;
};

}  // namespace jq
