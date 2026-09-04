#include "server/scheduler/scheduler.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/logging/logger.h"
#include "common/metrics/metrics.h"
#include <prometheus/histogram.h>
#include "common/redis/redis_client.h"
#include "server/db/job_repository.h"
#include "server/db/queue_repository.h"
#include "server/db/worker_repository.h"

namespace jq {

// ---------------------------------------------------------------------------
// CalculateRetryNotBefore (pure; exposed for unit tests)
// ---------------------------------------------------------------------------

int64_t CalculateRetryNotBefore(int64_t now_epoch,
                                 int     retry_count,
                                 int     base_delay_s,
                                 int     max_delay_s,
                                 int     jitter_s) {
    // delay = min(base_delay * 2^retry_count, max_delay)
    double delay = static_cast<double>(base_delay_s);
    for (int i = 0; i < retry_count; ++i) {
        delay *= 2.0;
        if (delay >= static_cast<double>(max_delay_s)) {
            delay = static_cast<double>(max_delay_s);
            break;
        }
    }
    if (delay > static_cast<double>(max_delay_s)) delay = static_cast<double>(max_delay_s);
    return now_epoch + static_cast<int64_t>(delay) + jitter_s;
}

// ---------------------------------------------------------------------------
// Scheduler constructor / destructor
// ---------------------------------------------------------------------------

Scheduler::Scheduler(db::ConnectionPool&    pool,
                     const RedisConfig&     redis_cfg,
                     WorkerRegistry&        registry,
                     const SchedulerConfig& cfg)
    : pool_(pool),
      redis_cfg_(redis_cfg),
      registry_(registry),
      cfg_(cfg) {}

Scheduler::~Scheduler() {
    Stop();
}

// ---------------------------------------------------------------------------
// Start / Stop
// ---------------------------------------------------------------------------

void Scheduler::Start() {
    running_ = true;
    loop_thread_      = std::thread([this] { RunLoop(); });
    heartbeat_thread_ = std::thread([this] { HeartbeatMonitor(); });
    LOG_INFO("Scheduler started",
             {{"interval_ms", cfg_.interval_ms},
              {"batch_size",  cfg_.batch_size}});
}

void Scheduler::Stop() {
    if (!running_.exchange(false)) return;  // already stopped
    if (loop_thread_.joinable())      loop_thread_.join();
    if (heartbeat_thread_.joinable()) heartbeat_thread_.join();
    LOG_INFO("Scheduler stopped");
}

// ---------------------------------------------------------------------------
// RunLoop — scheduling thread
// ---------------------------------------------------------------------------

void Scheduler::RunLoop() {
    // Each thread owns its own RedisClient (not thread-safe).
    RedisClient redis(redis_cfg_);
    db::JobRepository job_repo(pool_);

    while (running_) {
        auto cycle_start = std::chrono::steady_clock::now();

        try {
            // 1. Fetch PENDING jobs.
            auto jobs = job_repo.FetchPendingBatch(cfg_.batch_size);

            if (!jobs.empty() && running_) {
                std::unordered_map<std::string, const db::JobRow*> by_id;
                by_id.reserve(jobs.size());
                std::vector<std::string> lock_keys;
                lock_keys.reserve(jobs.size());
                for (const auto& job : jobs) {
                    by_id[job.job_id] = &job;
                    lock_keys.push_back("job:" + job.job_id);
                }

                // 2. Acquire Redis distributed locks for the whole batch in
                // one pipelined round trip instead of one call per job.
                // SetNxPxBatch never throws -- like every other RedisClient
                // method it swallows hiredis errors internally -- so detect
                // a genuine outage via IsConnected() rather than a catch
                // block that could never fire, and skip locking (not skip
                // assignment) for the documented single-instance fallback.
                const int64_t lock_ttl =
                    static_cast<int64_t>(cfg_.assignment_timeout_s) * 1000;
                std::vector<std::string> locked_ids;
                locked_ids.reserve(jobs.size());
                if (redis.IsConnected()) {
                    std::unordered_set<std::string> acquired_keys;
                    for (auto& key : redis.SetNxPxBatch(lock_keys, "1", lock_ttl)) {
                        acquired_keys.insert(std::move(key));
                    }
                    for (const auto& job : jobs) {
                        if (acquired_keys.count("job:" + job.job_id)) {
                            locked_ids.push_back(job.job_id);
                        }
                    }
                } else {
                    LOG_WARN("Redis unavailable; proceeding without locks", {});
                    for (const auto& job : jobs) locked_ids.push_back(job.job_id);
                }

                if (!locked_ids.empty()) {
                    // 3. Transition PENDING → ASSIGNED for the whole batch in
                    // one UPDATE + one job_events INSERT.
                    std::vector<std::string> transitioned;
                    try {
                        transitioned = job_repo.TransitionJobsBatch(
                            locked_ids, "PENDING", "ASSIGNED", "ASSIGNED");
                    } catch (const std::exception& e) {
                        LOG_ERROR("TransitionJobsBatch failed", {{"error", e.what()}});
                    }

                    // Release locks for jobs that didn't actually transition
                    // (lost a race to a concurrent server replica, or the
                    // batch transition failed outright).
                    if (transitioned.size() != locked_ids.size()) {
                        std::unordered_set<std::string> ok(transitioned.begin(),
                                                             transitioned.end());
                        for (const auto& id : locked_ids) {
                            if (!ok.count(id)) redis.Del("job:" + id);
                        }
                    }

                    // 4. Stream each transitioned job to a worker via
                    // WorkerRegistry — in-process, no network round trip, so
                    // this stays a per-job loop.
                    std::vector<std::string> not_streamed;
                    for (const auto& id : transitioned) {
                        const db::JobRow* job = by_id[id];
                        bool streamed = registry_.AssignJob(
                            job->job_id, job->queue_name, job->payload, job->priority);

                        if (streamed) {
                            metrics::SchedulerJobsAssignedTotal()
                                .Add({{"queue", job->queue_name}})
                                .Increment();
                            LOG_INFO("Job assigned",
                                     {{"job_id", job->job_id}, {"queue", job->queue_name}});
                        } else {
                            not_streamed.push_back(id);
                        }
                    }

                    // No worker had a free concurrency slot for these —
                    // revert to PENDING and release their locks so the next
                    // cycle retries immediately, rather than leaving them
                    // stuck in ASSIGNED with no worker_id (nothing else ever
                    // revisits an ASSIGNED job that isn't tied to a worker).
                    if (!not_streamed.empty()) {
                        LOG_DEBUG("No worker available; reverting to PENDING",
                                  {{"count", static_cast<int>(not_streamed.size())}});
                        try {
                            job_repo.TransitionJobsBatch(
                                not_streamed, "ASSIGNED", "PENDING", "NO_WORKER_AVAILABLE");
                        } catch (const std::exception& e) {
                            LOG_ERROR("Reverting unassigned jobs to PENDING failed",
                                      {{"error", e.what()}});
                        }
                        for (const auto& id : not_streamed) redis.Del("job:" + id);
                    }
                }
            }

            // 5. Expire TTL-exceeded PENDING jobs.
            try {
                auto expired = job_repo.FetchExpiredTtlJobs();
                for (const auto& job : expired) {
                    bool dead = job_repo.TransitionJobStatus(
                        job.job_id, "PENDING", "DEAD_LETTERED", "TTL_EXPIRED");
                    if (dead) {
                        metrics::JobTotal()
                            .Add({{"queue", job.queue_name}, {"status", "DEAD_LETTERED"}})
                            .Increment();
                        LOG_INFO("Job TTL expired",
                                 {{"job_id", job.job_id}, {"queue", job.queue_name}});
                    }
                }
            } catch (const std::exception& e) {
                LOG_WARN("TTL expiry check failed", {{"error", e.what()}});
            }

        } catch (const std::exception& e) {
            LOG_ERROR("Scheduler cycle error", {{"error", e.what()}});
        }

        // Record cycle duration.
        auto elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - cycle_start).count();
        static prometheus::Histogram& cycle_hist =
            metrics::SchedulerCycleDuration().Add(
                {}, prometheus::Histogram::BucketBoundaries{
                        0.001, 0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 5.0});
        cycle_hist.Observe(elapsed);

        // Sleep for the remainder of the interval.
        auto sleep_ms = std::chrono::milliseconds(cfg_.interval_ms) -
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - cycle_start);
        if (sleep_ms > std::chrono::milliseconds(0)) {
            std::this_thread::sleep_for(sleep_ms);
        }
    }
}

// ---------------------------------------------------------------------------
// HeartbeatMonitor — heartbeat timeout thread
// ---------------------------------------------------------------------------

void Scheduler::HeartbeatMonitor() {
    db::JobRepository    job_repo(pool_);
    db::WorkerRepository worker_repo(pool_);
    db::QueueRepository  queue_repo(pool_);

    while (running_) {
        // Run every 10 seconds.
        for (int i = 0; i < 100 && running_; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!running_) break;

        // Snapshot jq_job_queue_depth from the same repository query jq-ctl
        // queue stats already uses. Piggybacks on this 10s loop rather than a
        // dedicated thread; a deleted queue's series simply goes stale rather
        // than being cleaned up (rare, explicit operator action).
        try {
            for (const auto& q : queue_repo.ListQueues()) {
                auto stats = queue_repo.GetQueueStats(q.name);
                if (!stats) continue;
                metrics::JobQueueDepth().Add({{"queue", q.name}, {"status", "PENDING"}})
                    .Set(static_cast<double>(stats->pending_count));
                metrics::JobQueueDepth().Add({{"queue", q.name}, {"status", "RUNNING"}})
                    .Set(static_cast<double>(stats->running_count));
                metrics::JobQueueDepth().Add({{"queue", q.name}, {"status", "FAILED"}})
                    .Set(static_cast<double>(stats->failed_count));
                metrics::JobQueueDepth().Add({{"queue", q.name}, {"status", "DONE"}})
                    .Set(static_cast<double>(stats->done_count));
                metrics::JobQueueDepth().Add({{"queue", q.name}, {"status", "DEAD_LETTERED"}})
                    .Set(static_cast<double>(stats->dead_letter_count));
            }
        } catch (const std::exception& e) {
            LOG_WARN("Queue depth snapshot failed", {{"error", e.what()}});
        }

        try {
            auto stale_workers = worker_repo.FetchStaleWorkers(
                cfg_.worker_heartbeat_timeout_s);

            for (const auto& worker : stale_workers) {
                LOG_WARN("Stale worker detected; marking OFFLINE",
                         {{"worker_id", worker.worker_id}});

                // Mark worker offline.
                worker_repo.SetWorkerStatus(worker.worker_id, "OFFLINE");

                // Remove from in-memory registry.
                registry_.RemoveWorker(worker.worker_id);

                // Find all ASSIGNED/RUNNING jobs and apply retry/dead-letter.
                auto jobs = job_repo.FetchJobsForWorker(worker.worker_id);
                for (const auto& job : jobs) {
                    // First transition to FAILED.
                    job_repo.TransitionJobStatus(
                        job.job_id, job.status, "FAILED", "WORKER_TIMEOUT",
                        worker.worker_id);
                    // Then apply retry logic.
                    ApplyRetry(job_repo, job.job_id, job.queue_name,
                               job.retry_count, job.max_retries);
                }
            }
        } catch (const std::exception& e) {
            LOG_ERROR("HeartbeatMonitor error", {{"error", e.what()}});
        }
    }
}

// ---------------------------------------------------------------------------
// ApplyRetry — shared retry logic for heartbeat monitor and WorkerService
// ---------------------------------------------------------------------------

void Scheduler::ApplyRetry(db::IJobRepository& job_repo,
                             const std::string&  job_id,
                             const std::string&  queue_name,
                             int                 retry_count,
                             int                 max_retries) {
    if (retry_count < max_retries) {
        // Schedule retry with exponential backoff + jitter.
        const int new_retry_count = retry_count + 1;

        // Generate jitter: [0, 5) seconds.
        thread_local std::mt19937 rng{std::random_device{}()};
        std::uniform_int_distribution<int> jitter_dist(0, 4);
        const int jitter_s = jitter_dist(rng);

        const int64_t now_epoch =
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();

        const int64_t not_before = CalculateRetryNotBefore(
            now_epoch, new_retry_count, 5, 300, jitter_s);

        try {
            job_repo.SetJobRetry(job_id, new_retry_count, not_before);
            LOG_INFO("Job scheduled for retry",
                     {{"job_id", job_id}, {"retry_count", new_retry_count}});
        } catch (const std::exception& e) {
            LOG_ERROR("SetJobRetry failed",
                      {{"job_id", job_id}, {"error", e.what()}});
        }
    } else {
        // Exhausted retries — dead-letter.
        try {
            bool ok = job_repo.TransitionJobStatus(
                job_id, "FAILED", "DEAD_LETTERED", "MAX_RETRIES_EXCEEDED");
            if (ok) {
                metrics::JobTotal()
                    .Add({{"queue", queue_name}, {"status", "DEAD_LETTERED"}})
                    .Increment();
                LOG_INFO("Job dead-lettered",
                         {{"job_id", job_id}, {"retry_count", retry_count}});
            }
        } catch (const std::exception& e) {
            LOG_ERROR("Dead-letter transition failed",
                      {{"job_id", job_id}, {"error", e.what()}});
        }
    }
}

}  // namespace jq
