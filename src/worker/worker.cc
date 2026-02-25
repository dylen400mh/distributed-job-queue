#include "worker/worker.h"

#include <chrono>
#include <thread>
#include <string>

#include <grpcpp/grpcpp.h>

#include "worker_service.grpc.pb.h"
#include "common/logging/logger.h"
#include "worker/job_executor.h"

namespace jq {

// ---------------------------------------------------------------------------
// ThreadPool
// ---------------------------------------------------------------------------

ThreadPool::ThreadPool(int n) {
    for (int i = 0; i < n; ++i) {
        threads_.emplace_back([this] { WorkerFunc(); });
    }
}

ThreadPool::~ThreadPool() {
    shutdown_ = true;
    cv_.notify_all();
    for (auto& t : threads_) {
        if (t.joinable()) t.join();
    }
}

void ThreadPool::Enqueue(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        tasks_.emplace_back(std::move(task));
    }
    cv_.notify_one();
}

int ThreadPool::ActiveCount() const {
    return active_.load();
}

void ThreadPool::WorkerFunc() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait(lock, [this] { return shutdown_ || !tasks_.empty(); });
            if (shutdown_ && tasks_.empty()) return;
            task = std::move(tasks_.front());
            tasks_.pop_front();
        }
        ++active_;
        task();
        --active_;
    }
}

// ---------------------------------------------------------------------------
// Worker
// ---------------------------------------------------------------------------

Worker::Worker(const std::string&              server_addr,
               const std::string&              worker_id,
               int                             concurrency,
               const std::vector<std::string>& queues,
               int                             heartbeat_interval_s,
               int                             grace_period_s)
    : server_addr_(server_addr)
    , registration_id_(worker_id)  // may be empty; server generates UUID if so
    , concurrency_(concurrency)
    , queues_(queues)
    , heartbeat_interval_s_(heartbeat_interval_s)
    , grace_period_s_(grace_period_s)
    , pool_(concurrency)
{
    // Display ID: use user-provided value if set, else hostname+PID for logging.
    if (!worker_id.empty()) {
        worker_id_ = worker_id;
    } else {
        char hostname[256] = {};
        ::gethostname(hostname, sizeof(hostname));
        worker_id_ = std::string(hostname) + "-" + std::to_string(::getpid());
    }
}

void Worker::Connect() {
    channel_ = grpc::CreateChannel(server_addr_, grpc::InsecureChannelCredentials());
    stub_    = WorkerService::NewStub(channel_);
}

// ---------------------------------------------------------------------------
// Shutdown (called from signal handler)
// ---------------------------------------------------------------------------

void Worker::Shutdown() {
    shutdown_ = true;
    // Cancel the streaming RPC so StreamJobs' Read() returns.
    std::lock_guard<std::mutex> lock(stream_ctx_mu_);
    if (stream_ctx_) {
        stream_ctx_->TryCancel();
    }
    // Wake the concurrency gate in case it's waiting.
    active_cv_.notify_all();
}

// ---------------------------------------------------------------------------
// Run
// ---------------------------------------------------------------------------

void Worker::Run() {
    Connect();

    // Register with jq-server.
    {
        RegisterWorkerRequest req;
        req.set_worker_id(registration_id_);  // empty = let server assign a UUID
        req.set_concurrency(concurrency_);
        for (const auto& q : queues_) req.add_queues(q);

        char hostname_buf[256] = {};
        ::gethostname(hostname_buf, sizeof(hostname_buf));
        req.set_hostname(hostname_buf);

        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
        RegisterWorkerResponse resp;
        auto status = stub_->RegisterWorker(&ctx, req, &resp);
        if (!status.ok()) {
            LOG_ERROR("RegisterWorker failed",
                      {{"error", status.error_message()}});
            return;
        }
        // Server may assign a new worker_id (e.g. UUID).
        if (!resp.worker_id().empty()) {
            worker_id_ = resp.worker_id();
        }
        LOG_INFO("Worker registered",
                 {{"worker_id", worker_id_}, {"concurrency", concurrency_}});
    }

    // Start heartbeat thread.
    std::thread hb_thread([this] { HeartbeatLoop(); });

    // Open StreamJobs.
    {
        StreamJobsRequest req;
        req.set_worker_id(worker_id_);
        for (const auto& q : queues_) req.add_queues(q);

        auto ctx = std::make_unique<grpc::ClientContext>();
        {
            std::lock_guard<std::mutex> lock(stream_ctx_mu_);
            stream_ctx_ = std::move(ctx);
        }

        auto reader = stub_->StreamJobs(stream_ctx_.get(), req);

        LOG_INFO("StreamJobs open", {{"worker_id", worker_id_}});

        JobAssignment assignment;
        while (!shutdown_ && reader->Read(&assignment)) {
            // Wait for a concurrency slot before dispatching.
            {
                std::unique_lock<std::mutex> lock(active_mu_);
                active_cv_.wait(lock, [this] {
                    return shutdown_ || active_count_.load() < concurrency_;
                });
            }
            if (shutdown_) break;

            ++active_count_;

            // Copy assignment into lambda (proto messages are copyable).
            JobAssignment copy = assignment;
            pool_.Enqueue([this, copy = std::move(copy)]() mutable {
                ExecuteAndReport(copy);
                --active_count_;
                active_cv_.notify_one();
            });
        }

        reader->Finish();  // drain any trailing status
        LOG_INFO("StreamJobs closed", {{"worker_id", worker_id_}});
    }

    // Graceful shutdown: wait for active jobs to finish (up to grace_period_s).
    LOG_INFO("Waiting for active jobs to finish",
             {{"active", active_count_.load()}, {"grace_s", grace_period_s_}});
    {
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(grace_period_s_);
        std::unique_lock<std::mutex> lock(active_mu_);
        active_cv_.wait_until(lock, deadline, [this] {
            return active_count_.load() == 0;
        });
    }

    // Deregister.
    {
        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
        DeregisterRequest req;
        req.set_worker_id(worker_id_);
        DeregisterResponse resp;
        stub_->Deregister(&ctx, req, &resp);
        LOG_INFO("Worker deregistered", {{"worker_id", worker_id_}});
    }

    hb_thread.join();
}

// ---------------------------------------------------------------------------
// HeartbeatLoop
// ---------------------------------------------------------------------------

void Worker::HeartbeatLoop() {
    int consecutive_failures = 0;

    while (!shutdown_) {
        // Sleep in short increments so we notice shutdown quickly.
        for (int i = 0; i < heartbeat_interval_s_ * 10 && !shutdown_; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (shutdown_) break;

        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
        HeartbeatRequest req;
        req.set_worker_id(worker_id_);
        req.set_active_job_count(active_count_.load());
        HeartbeatResponse resp;

        auto status = stub_->Heartbeat(&ctx, req, &resp);
        if (status.ok()) {
            consecutive_failures = 0;
        } else {
            ++consecutive_failures;
            LOG_WARN("Heartbeat failed",
                     {{"worker_id", worker_id_},
                      {"error", status.error_message()},
                      {"consecutive_failures", consecutive_failures}});
            if (consecutive_failures >= 3) {
                LOG_ERROR("3 consecutive heartbeat failures; "
                          "server may have lost this worker",
                          {{"worker_id", worker_id_}});
                // Reconnect the stub (channel stays alive for RPCs in progress).
                Connect();
                consecutive_failures = 0;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// ExecuteAndReport
// ---------------------------------------------------------------------------

void Worker::ExecuteAndReport(const JobAssignment& assignment) {
    const std::string& job_id = assignment.job_id();

    LOG_INFO("Executing job",
             {{"job_id", job_id}, {"queue", assignment.queue_name()}});

    // Payload bytes → string for JSON parsing.
    std::string payload_str(assignment.payload().begin(),
                             assignment.payload().end());

    // TTL comes from the scheduler assignment; we don't have it directly,
    // so use 0 (no timeout) in this implementation.
    auto result = JobExecutor::Execute(job_id, payload_str, /*ttl_seconds=*/0);

    // Report result back to jq-server.
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));

    ReportResultRequest req;
    req.set_worker_id(worker_id_);
    req.set_job_id(job_id);
    req.set_success(result.success);
    req.set_result(result.output.data(), result.output.size());
    req.set_error_message(result.error_message);

    ReportResultResponse resp;
    auto status = stub_->ReportResult(&ctx, req, &resp);
    if (!status.ok()) {
        LOG_ERROR("ReportResult failed",
                  {{"job_id", job_id}, {"error", status.error_message()}});
    } else {
        LOG_INFO("Job result reported",
                 {{"job_id", job_id}, {"success", result.success}});
    }
}

}  // namespace jq
