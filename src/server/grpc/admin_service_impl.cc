#include "server/grpc/admin_service_impl.h"

#include <string>

#include <grpcpp/grpcpp.h>
#include <pqxx/pqxx>

#include "common/kafka/kafka_producer.h"
#include "common/logging/logger.h"
#include "common/redis/redis_client.h"
#include "common.pb.h"

namespace jq {

namespace {

// Map a QueueRow to a proto Queue message.
Queue QueueRowToProto(const db::QueueRow& row) {
    Queue q;
    q.set_name(row.name);
    q.set_max_retries(row.max_retries);
    q.set_ttl_seconds(row.ttl_seconds);
    if (row.created_at > 0) {
        auto* ts = q.mutable_created_at();
        ts->set_seconds(row.created_at);
        ts->set_nanos(0);
    }
    return q;
}

// Map a QueueStatsRow to a proto QueueStats message.
QueueStats StatsRowToProto(const db::QueueStatsRow& row) {
    QueueStats s;
    s.set_queue_name(row.queue_name);
    s.set_pending_count(row.pending_count);
    s.set_running_count(row.running_count);
    s.set_failed_count(row.failed_count);
    s.set_done_count(row.done_count);
    s.set_dead_letter_count(row.dead_letter_count);
    s.set_total_processed(row.total_processed);
    s.set_avg_duration_ms(row.avg_duration_ms);
    s.set_error_rate(row.error_rate);
    return s;
}

// Map a WorkerRow to a proto Worker message.
Worker WorkerRowToProto(const db::WorkerRow& row) {
    Worker w;
    w.set_worker_id(row.worker_id);
    w.set_hostname(row.hostname);
    w.set_concurrency(row.concurrency);
    w.set_active_job_count(row.active_job_count);
    if (row.status == "ONLINE")         w.set_status(ONLINE);
    else if (row.status == "DRAINING")  w.set_status(DRAINING);
    else                                w.set_status(OFFLINE);
    if (row.last_heartbeat > 0) {
        auto* ts = w.mutable_last_heartbeat();
        ts->set_seconds(row.last_heartbeat);
        ts->set_nanos(0);
    }
    return w;
}

}  // namespace

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

AdminServiceImpl::AdminServiceImpl(db::IQueueRepository&  queue_repo,
                                    db::IWorkerRepository& worker_repo,
                                    WorkerRegistry&        registry,
                                    db::ConnectionPool&    pool,
                                    const RedisConfig&     redis_cfg,
                                    IKafkaProducer&        kafka)
    : queue_repo_(queue_repo)
    , worker_repo_(worker_repo)
    , registry_(registry)
    , pool_(pool)
    , redis_cfg_(redis_cfg)
    , kafka_(kafka)
{}

// ---------------------------------------------------------------------------
// CreateQueue
// ---------------------------------------------------------------------------

grpc::Status AdminServiceImpl::CreateQueue(grpc::ServerContext*       /*ctx*/,
                                            const CreateQueueRequest* req,
                                            CreateQueueResponse*      resp) {
    if (req->name().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "queue name is required");
    }
    try {
        auto row = queue_repo_.CreateQueue(
            req->name(),
            req->max_retries() > 0 ? req->max_retries() : 3,
            req->ttl_seconds());
        *resp->mutable_queue() = QueueRowToProto(row);
        LOG_INFO("Queue created", {{"name", req->name()}});
        return grpc::Status::OK;
    } catch (const std::runtime_error& e) {
        // Duplicate queue name
        return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, e.what());
    } catch (const std::exception& e) {
        LOG_ERROR("CreateQueue failed", {{"name", req->name()}, {"error", e.what()}});
        return grpc::Status(grpc::StatusCode::INTERNAL, "database error");
    }
}

// ---------------------------------------------------------------------------
// DeleteQueue
// ---------------------------------------------------------------------------

grpc::Status AdminServiceImpl::DeleteQueue(grpc::ServerContext*       /*ctx*/,
                                            const DeleteQueueRequest* req,
                                            DeleteQueueResponse*      /*resp*/) {
    if (req->name().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "queue name is required");
    }
    try {
        bool ok = queue_repo_.DeleteQueue(req->name(), req->force());
        if (!ok) {
            return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                                "queue '" + req->name() + "' is non-empty; use force=true to delete");
        }
        LOG_INFO("Queue deleted", {{"name", req->name()}});
        return grpc::Status::OK;
    } catch (const std::exception& e) {
        LOG_ERROR("DeleteQueue failed", {{"name", req->name()}, {"error", e.what()}});
        return grpc::Status(grpc::StatusCode::INTERNAL, "database error");
    }
}

// ---------------------------------------------------------------------------
// ListQueues
// ---------------------------------------------------------------------------

grpc::Status AdminServiceImpl::ListQueues(grpc::ServerContext*     /*ctx*/,
                                           const ListQueuesRequest* /*req*/,
                                           ListQueuesResponse*      resp) {
    try {
        auto queues = queue_repo_.ListQueues();
        for (const auto& row : queues) {
            *resp->add_queues() = QueueRowToProto(row);
        }
        return grpc::Status::OK;
    } catch (const std::exception& e) {
        LOG_ERROR("ListQueues failed", {{"error", e.what()}});
        return grpc::Status(grpc::StatusCode::INTERNAL, "database error");
    }
}

// ---------------------------------------------------------------------------
// GetQueueStats
// ---------------------------------------------------------------------------

grpc::Status AdminServiceImpl::GetQueueStats(grpc::ServerContext*,
                                              const GetQueueStatsRequest* req,
                                              GetQueueStatsResponse*      resp) {
    if (req->name().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "queue name is required");
    }
    try {
        auto stats = queue_repo_.GetQueueStats(req->name());
        if (!stats) {
            return grpc::Status(grpc::StatusCode::NOT_FOUND,
                                "queue '" + req->name() + "' not found");
        }
        *resp->mutable_stats() = StatsRowToProto(*stats);
        return grpc::Status::OK;
    } catch (const std::exception& e) {
        LOG_ERROR("GetQueueStats failed", {{"name", req->name()}, {"error", e.what()}});
        return grpc::Status(grpc::StatusCode::INTERNAL, "database error");
    }
}

// ---------------------------------------------------------------------------
// ListWorkers
// ---------------------------------------------------------------------------

grpc::Status AdminServiceImpl::ListWorkers(grpc::ServerContext*     /*ctx*/,
                                            const ListWorkersRequest* /*req*/,
                                            ListWorkersResponse*     resp) {
    try {
        auto workers = worker_repo_.ListWorkers();
        for (const auto& row : workers) {
            *resp->add_workers() = WorkerRowToProto(row);
        }
        return grpc::Status::OK;
    } catch (const std::exception& e) {
        LOG_ERROR("ListWorkers failed", {{"error", e.what()}});
        return grpc::Status(grpc::StatusCode::INTERNAL, "database error");
    }
}

// ---------------------------------------------------------------------------
// DrainWorker
// ---------------------------------------------------------------------------

grpc::Status AdminServiceImpl::DrainWorker(grpc::ServerContext*     /*ctx*/,
                                            const DrainWorkerRequest* req,
                                            DrainWorkerResponse*     /*resp*/) {
    if (req->worker_id().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "worker_id is required");
    }
    try {
        worker_repo_.SetWorkerStatus(req->worker_id(), "DRAINING");
        registry_.DrainWorker(req->worker_id());
        LOG_INFO("Worker draining", {{"worker_id", req->worker_id()}});
        return grpc::Status::OK;
    } catch (const std::exception& e) {
        LOG_ERROR("DrainWorker failed",
                  {{"worker_id", req->worker_id()}, {"error", e.what()}});
        return grpc::Status(grpc::StatusCode::INTERNAL, "database error");
    }
}

// ---------------------------------------------------------------------------
// ShutdownWorker
// ---------------------------------------------------------------------------

grpc::Status AdminServiceImpl::ShutdownWorker(grpc::ServerContext*       /*ctx*/,
                                               const ShutdownWorkerRequest* req,
                                               ShutdownWorkerResponse*    /*resp*/) {
    if (req->worker_id().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "worker_id is required");
    }
    try {
        worker_repo_.SetWorkerStatus(req->worker_id(), "OFFLINE");
        // Close the streaming channel; StreamJobs will exit and the worker
        // will detect the closed stream and begin its graceful shutdown.
        registry_.RemoveWorker(req->worker_id());
        LOG_INFO("Worker shutdown signalled", {{"worker_id", req->worker_id()}});
        return grpc::Status::OK;
    } catch (const std::exception& e) {
        LOG_ERROR("ShutdownWorker failed",
                  {{"worker_id", req->worker_id()}, {"error", e.what()}});
        return grpc::Status(grpc::StatusCode::INTERNAL, "database error");
    }
}

// ---------------------------------------------------------------------------
// GetSystemStatus
// ---------------------------------------------------------------------------

grpc::Status AdminServiceImpl::GetSystemStatus(grpc::ServerContext*,
                                                const GetSystemStatusRequest*,
                                                GetSystemStatusResponse* resp) {
    bool all_healthy = true;

    // DB check.
    {
        auto* comp = resp->add_components();
        comp->set_name("database");
        bool ok = false;
        try {
            auto conn = pool_.Borrow();
            pqxx::work txn(conn.get());
            txn.exec("SELECT 1");
            txn.commit();
            ok = true;
        } catch (...) {}
        comp->set_healthy(ok);
        comp->set_message(ok ? "reachable" : "unreachable");
        if (!ok) all_healthy = false;
    }

    // Redis check (temporary client, cheap).
    {
        auto* comp = resp->add_components();
        comp->set_name("redis");
        bool ok = false;
        try {
            RedisClient rc(redis_cfg_);
            ok = rc.IsConnected();
        } catch (...) {}
        comp->set_healthy(ok);
        comp->set_message(ok ? "reachable" : "unreachable");
        if (!ok) all_healthy = false;
    }

    // Kafka — probe broker reachability via metadata fetch.
    {
        auto* comp = resp->add_components();
        comp->set_name("kafka");
        bool ok = kafka_.IsHealthy();
        comp->set_healthy(ok);
        comp->set_message(ok ? "reachable" : "unreachable");
        if (!ok) all_healthy = false;
    }

    // Active worker count from in-memory registry.
    auto worker_stats = registry_.GetWorkerStats();
    resp->set_active_workers(static_cast<int32_t>(worker_stats.size()));
    resp->set_healthy(all_healthy);

    return grpc::Status::OK;
}

}  // namespace jq
