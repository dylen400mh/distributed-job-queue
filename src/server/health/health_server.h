#pragma once

#include <atomic>
#include <thread>

#include "common/db/connection_pool.h"
#include "common/redis/redis_client.h"
#include "common/kafka/kafka_producer.h"

namespace jq {

// ---------------------------------------------------------------------------
// HealthServer — minimal HTTP server exposing /healthz and /readyz.
//
// Runs in a background thread started by Start(); stopped by Stop().
//
// /healthz — always 200 OK (liveness probe)
// /readyz  — 200 if DB, Redis, and Kafka are reachable; 503 otherwise
// ---------------------------------------------------------------------------
class HealthServer {
public:
    HealthServer(int                 port,
                 db::ConnectionPool& pool,
                 RedisClient&        redis,
                 IKafkaProducer&     kafka);
    ~HealthServer();

    // Start the background listener thread.
    void Start();

    // Signal the background thread to stop and join it.
    void Stop();

private:
    void AcceptLoop();

    // Returns true if the dependency is reachable.
    bool CheckDb();
    bool CheckRedis();

    int                 port_;
    db::ConnectionPool& pool_;
    RedisClient&        redis_;
    IKafkaProducer&     kafka_;

    std::atomic<bool>   running_{false};
    std::thread         thread_;
    int                 listen_fd_ = -1;
};

}  // namespace jq
