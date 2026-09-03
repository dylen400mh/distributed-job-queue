#include "server/scheduler/worker_registry.h"

#include <algorithm>
#include <functional>

#include "common/logging/logger.h"
#include "common/metrics/metrics.h"

namespace jq {

// ---------------------------------------------------------------------------
// PublishMetrics — caller must hold mu_.
// ---------------------------------------------------------------------------

void WorkerRegistry::PublishMetrics() {
    metrics::WorkerActiveCount().Add({}).Set(static_cast<double>(workers_.size()));
    for (const auto& [wid, info] : workers_) {
        metrics::WorkerJobConcurrency()
            .Add({{"worker_id", wid}})
            .Set(static_cast<double>(info.active_job_count));
    }
}

// ---------------------------------------------------------------------------
// RegisterStream
// ---------------------------------------------------------------------------

std::shared_ptr<StreamHandle> WorkerRegistry::RegisterStream(
        const std::string&                        worker_id,
        const std::vector<std::string>&           queues,
        int                                       concurrency,
        std::function<bool(const JobAssignment&)> write_fn) {

    auto handle = std::make_shared<StreamHandle>();
    handle->write_fn = std::move(write_fn);
    handle->active   = true;

    std::lock_guard<std::mutex> lock(mu_);

    auto& entry       = workers_[worker_id];
    entry.worker_id   = worker_id;
    entry.queues      = queues;
    entry.concurrency = concurrency;
    // Preserve active_job_count across stream reconnects.
    entry.stream      = handle;

    PublishMetrics();

    LOG_INFO("Worker stream registered",
             {{"worker_id", worker_id}, {"concurrency", concurrency}});

    return handle;
}

// ---------------------------------------------------------------------------
// RemoveWorker
// ---------------------------------------------------------------------------

void WorkerRegistry::RemoveWorker(const std::string& worker_id) {
    std::lock_guard<std::mutex> lock(mu_);
    workers_.erase(worker_id);
    // Drop the per-worker concurrency series so it doesn't linger as a stale
    // gauge after the worker is gone.
    metrics::WorkerJobConcurrency().Remove(
        &metrics::WorkerJobConcurrency().Add({{"worker_id", worker_id}}));
    PublishMetrics();
    LOG_INFO("Worker removed from registry", {{"worker_id", worker_id}});
}

// ---------------------------------------------------------------------------
// DecrementActiveCount
// ---------------------------------------------------------------------------

void WorkerRegistry::DecrementActiveCount(const std::string& worker_id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = workers_.find(worker_id);
    if (it != workers_.end() && it->second.active_job_count > 0) {
        --it->second.active_job_count;
    }
    PublishMetrics();
}

// ---------------------------------------------------------------------------
// AssignJob
// ---------------------------------------------------------------------------

bool WorkerRegistry::AssignJob(const std::string&          job_id,
                                const std::string&          queue_name,
                                const std::vector<uint8_t>& payload,
                                int                         priority) {
    std::shared_ptr<StreamHandle> chosen_handle;
    std::string                   chosen_id;

    {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& [wid, info] : workers_) {
            if (!info.stream) continue;
            if (info.draining) continue;
            if (info.active_job_count >= info.concurrency) continue;
            bool subscribed = std::find(info.queues.begin(), info.queues.end(), queue_name)
                              != info.queues.end();
            if (!subscribed) continue;

            chosen_handle = info.stream;
            chosen_id     = wid;
            ++info.active_job_count;
            break;
        }
        if (chosen_handle) PublishMetrics();
    }

    if (!chosen_handle) {
        return false;
    }

    // Write under the per-worker stream mutex.
    std::lock_guard<std::mutex> stream_lock(chosen_handle->mu);
    if (!chosen_handle->active) {
        // Worker disconnected between selection and write — roll back.
        std::lock_guard<std::mutex> lock(mu_);
        auto it = workers_.find(chosen_id);
        if (it != workers_.end() && it->second.active_job_count > 0) {
            --it->second.active_job_count;
        }
        PublishMetrics();
        return false;
    }

    JobAssignment msg;
    msg.set_job_id(job_id);
    msg.set_queue_name(queue_name);
    msg.set_payload(payload.data(), payload.size());
    msg.set_priority(priority);

    bool ok = chosen_handle->write_fn(msg);
    if (!ok) {
        LOG_WARN("Stream write failed; worker likely disconnected",
                 {{"worker_id", chosen_id}, {"job_id", job_id}});
        std::lock_guard<std::mutex> lock(mu_);
        auto it = workers_.find(chosen_id);
        if (it != workers_.end() && it->second.active_job_count > 0) {
            --it->second.active_job_count;
        }
        PublishMetrics();
        return false;
    }

    LOG_INFO("Job assigned to worker",
             {{"job_id", job_id}, {"worker_id", chosen_id}, {"queue", queue_name}});
    return true;
}

// ---------------------------------------------------------------------------
// DrainWorker
// ---------------------------------------------------------------------------

void WorkerRegistry::DrainWorker(const std::string& worker_id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = workers_.find(worker_id);
    if (it != workers_.end()) {
        it->second.draining = true;
        LOG_INFO("Worker set to draining", {{"worker_id", worker_id}});
    }
}

// ---------------------------------------------------------------------------
// GetWorkerStats
// ---------------------------------------------------------------------------

std::vector<WorkerInfo> WorkerRegistry::GetWorkerStats() {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<WorkerInfo> out;
    out.reserve(workers_.size());
    for (const auto& [wid, info] : workers_) {
        out.push_back(info);
    }
    return out;
}

}  // namespace jq
