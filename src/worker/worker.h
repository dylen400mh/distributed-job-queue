#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "worker_service.grpc.pb.h"

namespace jq {

// ---------------------------------------------------------------------------
// ThreadPool — fixed-size pool of worker threads with a task queue.
// ---------------------------------------------------------------------------
class ThreadPool {
public:
    explicit ThreadPool(int n);
    ~ThreadPool();

    // Enqueue a task. Thread-safe. Returns immediately.
    void Enqueue(std::function<void()> task);

    // Number of currently queued + running tasks.
    int ActiveCount() const;

private:
    void WorkerFunc();

    std::vector<std::thread>         threads_;
    std::deque<std::function<void()>> tasks_;
    mutable std::mutex               mu_;
    std::condition_variable          cv_;
    std::atomic<bool>                shutdown_{false};
    std::atomic<int>                 active_{0};
};

// ---------------------------------------------------------------------------
// Worker — long-running jq-worker daemon.
//
// On Run():
//   1. Registers with jq-server via WorkerService::RegisterWorker
//   2. Starts a heartbeat thread (sends Heartbeat every heartbeat_interval_s)
//   3. Opens WorkerService::StreamJobs and blocks reading assignments
//   4. For each assignment: waits for a concurrency slot, then dispatches to
//      a ThreadPool; each thread calls JobExecutor::Execute then ReportResult
//   5. On Shutdown(): stops accepting new jobs, waits for active jobs
//      (up to grace_period_s), calls Deregister, exits
// ---------------------------------------------------------------------------
class Worker {
public:
    Worker(const std::string&              server_addr,
           const std::string&              worker_id,
           int                             concurrency,
           const std::vector<std::string>& queues,
           int                             heartbeat_interval_s = 5,
           int                             grace_period_s       = 60);

    // Blocks until Shutdown() is called (from signal handler).
    void Run();

    // Called from SIGTERM/SIGINT handler. Thread-safe.
    void Shutdown();

private:
    // Send periodic Heartbeat RPCs; retries on failure up to 3 consecutive.
    void HeartbeatLoop();

    // Execute one job assignment on the calling thread and report the result.
    void ExecuteAndReport(const JobAssignment& assignment);

    // Build a fresh channel + stub (used on startup and reconnect).
    void Connect();

    std::string              server_addr_;
    std::string              worker_id_;        // effective ID (updated to server-assigned UUID on Register)
    std::string              registration_id_;  // user-provided UUID from --worker-id, or empty
    int                      concurrency_;
    std::vector<std::string> queues_;
    int                      heartbeat_interval_s_;
    int                      grace_period_s_;

    std::shared_ptr<grpc::Channel>           channel_;
    std::unique_ptr<WorkerService::Stub>     stub_;

    ThreadPool pool_;

    // Concurrency gate: StreamJobs loop waits here when active == concurrency.
    std::atomic<int>        active_count_{0};
    std::mutex              active_mu_;
    std::condition_variable active_cv_;

    std::atomic<bool> shutdown_{false};

    // ClientContext for the StreamJobs RPC — kept alive until Shutdown().
    std::unique_ptr<grpc::ClientContext> stream_ctx_;
    std::mutex                           stream_ctx_mu_;
};

}  // namespace jq
