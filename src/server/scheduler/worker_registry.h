#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "worker_service.pb.h"

namespace jq {

// ---------------------------------------------------------------------------
// StreamHandle — shared ownership of an active StreamJobs write channel.
//
// Stores a write callback rather than a raw ServerWriter* so the registry
// is testable without a live gRPC channel (ServerWriter is marked final).
//
// Thread safety: `mu` serializes all write_fn calls. StreamJobs holds `mu`
// before setting `active = false` and returning, so the scheduler can
// never write to a dead stream.
// ---------------------------------------------------------------------------
struct StreamHandle {
    // Callback that writes one JobAssignment. Returns true on success.
    std::function<bool(const JobAssignment&)> write_fn;
    std::mutex                                mu;
    bool                                      active = false;

    // Non-copyable, non-movable (holds mutex).
    StreamHandle()                             = default;
    StreamHandle(const StreamHandle&)          = delete;
    StreamHandle& operator=(const StreamHandle&) = delete;
};

// ---------------------------------------------------------------------------
// WorkerInfo — in-memory record of a registered worker.
// ---------------------------------------------------------------------------
struct WorkerInfo {
    std::string              worker_id;
    std::vector<std::string> queues;            // subscribed queue names
    int                      concurrency      = 4;
    int                      active_job_count = 0;
    bool                     draining         = false; // set by DrainWorker()
    std::shared_ptr<StreamHandle> stream;       // null if not currently streaming
};

// ---------------------------------------------------------------------------
// WorkerRegistry — thread-safe in-memory registry of active workers.
//
// The scheduler calls AssignJob() to push jobs to workers.
// WorkerService RPCs call RegisterStream() / RemoveWorker().
// ---------------------------------------------------------------------------
class WorkerRegistry {
public:
    WorkerRegistry() = default;

    // Register (or update) a worker and attach a write callback.
    // Returns a shared_ptr<StreamHandle> that StreamJobs should hold for
    // its lifetime. The StreamHandle is marked active=true on registration.
    //
    // In production, pass a lambda wrapping grpc::ServerWriter<>::Write:
    //   registry.RegisterStream(id, queues, concurrency,
    //       [writer](const JobAssignment& m){ return writer->Write(m); });
    std::shared_ptr<StreamHandle> RegisterStream(
        const std::string&                        worker_id,
        const std::vector<std::string>&           queues,
        int                                       concurrency,
        std::function<bool(const JobAssignment&)> write_fn);

    // Remove a worker from the registry (called on disconnect / Deregister).
    void RemoveWorker(const std::string& worker_id);

    // Decrement the active_job_count for a worker (called on ReportResult).
    void DecrementActiveCount(const std::string& worker_id);

    // Attempt to assign a job to an available worker.
    // Selects a worker whose subscribed queues include queue_name and whose
    // active_job_count < concurrency. Calls write_fn and increments
    // active_job_count. Returns false if no suitable worker is available.
    bool AssignJob(const std::string&          job_id,
                   const std::string&          queue_name,
                   const std::vector<uint8_t>& payload,
                   int                         priority);

    // Mark a worker as draining: no new jobs are assigned, but it finishes
    // its current jobs. Called by AdminService::DrainWorker.
    void DrainWorker(const std::string& worker_id);

    // Return a snapshot of all registered workers (for AdminService).
    std::vector<WorkerInfo> GetWorkerStats();

private:
    std::mutex                                  mu_;
    std::unordered_map<std::string, WorkerInfo> workers_;  // keyed by worker_id
};

}  // namespace jq
