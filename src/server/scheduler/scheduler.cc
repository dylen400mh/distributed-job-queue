#include "server/scheduler/scheduler.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "common.pb.h"
#include "common/kafka/kafka_producer.h"
#include "common/logging/logger.h"
#include "common/metrics/metrics.h"
#include <prometheus/histogram.h>
#include "common/redis/redis_client.h"
#include "server/db/job_repository.h"
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
                     IKafkaProducer&        kafka,
                     WorkerRegistry&        registry,
                     const SchedulerConfig& cfg)
    : pool_(pool),
      redis_cfg_(redis_cfg),
      kafka_(kafka),
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

            for (const auto& job : jobs) {
                if (!running_) break;

                // 2. Try to acquire Redis distributed lock.
                const std::string lock_key = "job:" + job.job_id;
                const int64_t     lock_ttl = static_cast<int64_t>(cfg_.assignment_timeout_s) * 1000;
                bool acquired = false;
                try {
                    acquired = redis.SetNxPx(lock_key, "1", lock_ttl);
                } catch (const std::exception& e) {
                    // Redis unavailable — fall back to single-instance mode (skip lock).
                    LOG_WARN("Redis unavailable; proceeding without lock",
                             {{"job_id", job.job_id}, {"error", e.what()}});
                    acquired = true;
                }

                if (!acquired) continue;  // another instance claimed it

                // 3. Transition PENDING → ASSIGNED in DB.
                bool ok = false;
                try {
                    ok = job_repo.TransitionJobStatus(
                        job.job_id, "PENDING", "ASSIGNED", "ASSIGNED");
                } catch (const std::exception& e) {
                    LOG_ERROR("TransitionJobStatus failed",
                              {{"job_id", job.job_id}, {"error", e.what()}});
                    redis.Del(lock_key);
                    continue;
                }
                if (!ok) {
                    // Concurrent claim — release lock and skip.
                    redis.Del(lock_key);
                    continue;
                }

                // 4. Publish job.submitted Kafka event.
                try {
                    JobEvent ev;
                    ev.set_job_id(job.job_id);
                    ev.set_from_status(PENDING);
                    ev.set_to_status(ASSIGNED);
                    ev.set_reason("ASSIGNED");
                    std::string serialized;
                    ev.SerializeToString(&serialized);
                    std::vector<uint8_t> payload(serialized.begin(), serialized.end());
                    kafka_.Publish(KafkaTopics::kJobSubmitted, job.job_id, payload);
                } catch (...) { /* Kafka not on critical path */ }

                // 5. Try to stream to a worker via WorkerRegistry.
                bool streamed = registry_.AssignJob(
                    job.job_id, job.queue_name, job.payload, job.priority);

                if (streamed) {
                    metrics::SchedulerJobsAssignedTotal()
                        .Add({{"queue", job.queue_name}})
                        .Increment();
                    LOG_INFO("Job assigned",
                             {{"job_id", job.job_id}, {"queue", job.queue_name}});
                } else {
                    // No worker available — job stays ASSIGNED; the assignment
                    // timeout (lock TTL) will expire and the scheduler retries.
                    LOG_DEBUG("No worker available for job",
                              {{"job_id", job.job_id}, {"queue", job.queue_name}});
                }
            }

            // 6. Expire TTL-exceeded PENDING jobs.
            try {
                auto expired = job_repo.FetchExpiredTtlJobs();
                for (const auto& job : expired) {
                    bool dead = job_repo.TransitionJobStatus(
                        job.job_id, "PENDING", "DEAD_LETTERED", "TTL_EXPIRED");
                    if (dead) {
                        LOG_INFO("Job TTL expired",
                                 {{"job_id", job.job_id}, {"queue", job.queue_name}});
                        try {
                            JobEvent ev;
                            ev.set_job_id(job.job_id);
                            ev.set_from_status(PENDING);
                            ev.set_to_status(DEAD_LETTERED);
                            ev.set_reason("TTL_EXPIRED");
                            std::string serialized;
                            ev.SerializeToString(&serialized);
                            std::vector<uint8_t> payload(serialized.begin(), serialized.end());
                            kafka_.Publish(KafkaTopics::kJobDeadLettered, job.job_id, payload);
                        } catch (...) {}
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

    while (running_) {
        // Run every 10 seconds.
        for (int i = 0; i < 100 && running_; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!running_) break;

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
                    ApplyRetry(job_repo, job.job_id, job.retry_count, job.max_retries);
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
                LOG_INFO("Job dead-lettered",
                         {{"job_id", job_id}, {"retry_count", retry_count}});
                try {
                    JobEvent ev;
                    ev.set_job_id(job_id);
                    ev.set_from_status(FAILED);
                    ev.set_to_status(DEAD_LETTERED);
                    ev.set_reason("MAX_RETRIES_EXCEEDED");
                    std::string serialized;
                    ev.SerializeToString(&serialized);
                    std::vector<uint8_t> payload(serialized.begin(), serialized.end());
                    kafka_.Publish(KafkaTopics::kJobDeadLettered, job_id, payload);
                } catch (...) {}
            }
        } catch (const std::exception& e) {
            LOG_ERROR("Dead-letter transition failed",
                      {{"job_id", job_id}, {"error", e.what()}});
        }
    }
}

}  // namespace jq
