// src/server/main.cc
// jq-server: gRPC server + scheduler daemon.

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include "common/config/config.h"
#include "common/config/flags.h"
#include "common/db/connection_pool.h"
#include "common/db/migrations.h"
#include "common/logging/logger.h"
#include "common/metrics/metrics.h"
#include "common/redis/redis_client.h"
#include "server/grpc/server.h"
#include "server/health/health_server.h"

// ---------------------------------------------------------------------------
// Global shutdown flag — set by signal handler, polled by main thread.
// ---------------------------------------------------------------------------
static std::atomic<bool> g_shutdown{false};
static jq::GrpcServer*   g_grpc_server = nullptr;

// Only touches an atomic — logging and gRPC's Shutdown() aren't
// async-signal-safe, so the actual Stop() call happens on the
// shutdown-watcher thread instead of here.
static void SignalHandler(int /*sig*/) {
    g_shutdown = true;
}

// ---------------------------------------------------------------------------
// Connectivity test helpers (for --dry-run)
// ---------------------------------------------------------------------------

static bool TestDb(const jq::DbConfig& cfg) {
    try {
        const std::string cs =
            "host=" + cfg.host +
            " port=" + std::to_string(cfg.port) +
            " dbname=" + cfg.name +
            " user=" + cfg.user +
            (cfg.password.empty() ? "" : " password=" + cfg.password) +
            " connect_timeout=" + std::to_string(cfg.connect_timeout_ms / 1000);
        jq::db::ConnectionPool pool(cs, 1);
        auto c = pool.Borrow();
        pqxx::work txn(c.get());
        txn.exec("SELECT 1");
        txn.commit();
        return true;
    } catch (...) { return false; }
}

static bool TestRedis(const jq::RedisConfig& cfg) {
    try {
        jq::RedisClient rc(cfg);
        return rc.IsConnected();
    } catch (...) { return false; }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    auto flags = jq::ParseServerFlags(argc, argv);

    if (flags.version) { jq::PrintVersion();          return 0; }
    if (flags.help)    { jq::PrintServerHelp(argv[0]); return 0; }

    if (flags.config.empty()) {
        std::cerr << "Error: --config is required\n";
        jq::PrintServerHelp(argv[0]);
        return 1;
    }

    // ---- Config ----
    jq::Config cfg;
    try {
        cfg = jq::LoadConfig(flags.config);
    } catch (const std::exception& e) {
        std::cerr << "Config error: " << e.what() << '\n';
        return 1;
    }

    const auto errors = jq::ValidateConfig(cfg);
    if (!errors.empty()) {
        for (const auto& err : errors) std::cerr << "Config error: " << err << '\n';
        return 1;
    }

    if (flags.grpc_port    > 0) cfg.grpc.port    = flags.grpc_port;
    if (flags.metrics_port > 0) cfg.metrics.port = flags.metrics_port;
    if (flags.health_port  > 0) cfg.health.port  = flags.health_port;

    // ---- Logger ----
    jq::log::InitLogger("jq-server", flags.log_level);
    for (const auto& w : cfg.sensitive_field_warnings) LOG_WARN(w);

    LOG_INFO("jq-server starting",
             {{"grpc_port",    cfg.grpc.port},
              {"metrics_port", cfg.metrics.port},
              {"health_port",  cfg.health.port}});

    // ---- Metrics server ----
    jq::metrics::StartMetricsServer(cfg.metrics.port);

    // ---- DB connection pool ----
    const std::string connstr =
        "host=" + cfg.db.host +
        " port=" + std::to_string(cfg.db.port) +
        " dbname=" + cfg.db.name +
        " user=" + cfg.db.user +
        (cfg.db.password.empty() ? "" : " password=" + cfg.db.password) +
        " connect_timeout=" + std::to_string(cfg.db.connect_timeout_ms / 1000);

    auto pool = std::make_unique<jq::db::ConnectionPool>(
        connstr, static_cast<std::size_t>(cfg.db.pool_size));

    // ---- Redis client ----
    auto redis = std::make_unique<jq::RedisClient>(cfg.redis);

    // ---- Dry-run: connectivity checks, then exit ----
    if (flags.dry_run) {
        bool db_ok    = TestDb(cfg.db);
        bool redis_ok = TestRedis(cfg.redis);

        std::cout << "DB:    " << (db_ok    ? "OK" : "FAIL") << '\n';
        std::cout << "Redis: " << (redis_ok ? "OK" : "FAIL") << '\n';

        return (db_ok && redis_ok) ? 0 : 1;
    }

    // ---- DB migrations ----
    try {
        jq::db::RunMigrations(*pool, "db/migrations");
        LOG_INFO("DB migrations applied successfully");
    } catch (const std::exception& e) {
        LOG_ERROR("DB migrations failed", {{"error", e.what()}});
        return 1;
    }

    // ---- Health server ----
    jq::HealthServer health(cfg.health.port, *pool, *redis);
    try {
        health.Start();
    } catch (const std::exception& e) {
        LOG_ERROR("Health server failed to start", {{"error", e.what()}});
        return 1;
    }

    // ---- Signal handlers ----
    std::signal(SIGTERM, SignalHandler);
    std::signal(SIGINT,  SignalHandler);

    // Polls the shutdown flag from ordinary thread context and calls
    // GrpcServer::Stop() there, since the signal handler itself can't
    // safely do that work.
    std::thread shutdown_watcher([] {
        while (!g_shutdown.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (g_grpc_server) g_grpc_server->Stop();
    });

    // ---- gRPC server (blocks until shutdown) ----
    try {
        jq::GrpcServer grpc_server(cfg, *pool);
        g_grpc_server = &grpc_server;
        grpc_server.Start();  // blocks here
        g_grpc_server = nullptr;
    } catch (const std::exception& e) {
        LOG_ERROR("gRPC server error", {{"error", e.what()}});
        g_shutdown = true;
        shutdown_watcher.join();
        health.Stop();
        return 1;
    }

    g_shutdown = true;
    shutdown_watcher.join();

    // ---- Graceful shutdown ----
    LOG_INFO("jq-server shutting down");
    health.Stop();
    LOG_INFO("jq-server stopped");
    return 0;
}
