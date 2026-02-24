// src/worker/main.cc
// jq-worker: job executor daemon.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "common/config/config.h"
#include "common/config/flags.h"
#include "common/logging/logger.h"
#include "common/metrics/metrics.h"
#include "worker/worker.h"

// ---------------------------------------------------------------------------
// Global shutdown flag and worker pointer — set by signal handler.
// ---------------------------------------------------------------------------
static std::atomic<bool> g_shutdown{false};
static jq::Worker*       g_worker = nullptr;

static void SignalHandler(int /*sig*/) {
    g_shutdown = true;
    if (g_worker) g_worker->Shutdown();
}

// ---------------------------------------------------------------------------
// Minimal health server for jq-worker.
// Worker has no DB/Redis/Kafka deps; always returns 200 OK.
// ---------------------------------------------------------------------------
static void RunHealthServer(int port) {
    int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) return;

    int opt = 1;
    ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(static_cast<uint16_t>(port));

    if (::bind(listen_fd, reinterpret_cast<struct sockaddr*>(&addr),
               sizeof(addr)) != 0) {
        ::close(listen_fd);
        return;
    }
    ::listen(listen_fd, 8);

    static const char kOkResp[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 2\r\n"
        "Connection: close\r\n\r\nOK";

    while (!g_shutdown) {
        struct timeval tv{1, 0};
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(listen_fd, &fds);
        if (::select(listen_fd + 1, &fds, nullptr, nullptr, &tv) <= 0) continue;
        int client_fd = ::accept(listen_fd, nullptr, nullptr);
        if (client_fd < 0) continue;
        ::send(client_fd, kOkResp, sizeof(kOkResp) - 1, 0);
        ::close(client_fd);
    }
    ::close(listen_fd);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    auto flags = jq::ParseWorkerFlags(argc, argv);

    if (flags.version) { jq::PrintVersion();           return 0; }
    if (flags.help)    { jq::PrintWorkerHelp(argv[0]); return 0; }

    if (flags.config.empty()) {
        std::cerr << "Error: --config is required\n";
        jq::PrintWorkerHelp(argv[0]);
        return 1;
    }
    if (flags.server_addr.empty()) {
        std::cerr << "Error: --server-addr is required\n";
        jq::PrintWorkerHelp(argv[0]);
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
    if (flags.metrics_port > 0) cfg.metrics.port = flags.metrics_port;
    if (flags.health_port  > 0) cfg.health.port  = flags.health_port;

    // ---- Logger ----
    jq::log::InitLogger("jq-worker", flags.log_level);
    for (const auto& w : cfg.sensitive_field_warnings) LOG_WARN(w);

    // ---- Worker ID (hostname+PID if not specified) ----
    std::string worker_id = flags.worker_id;
    if (worker_id.empty()) {
        char hostname[256] = {};
        ::gethostname(hostname, sizeof(hostname));
        worker_id = std::string(hostname) + "-" + std::to_string(::getpid());
    }

    // ---- Concurrency ----
    int concurrency = (flags.concurrency > 0) ? flags.concurrency : 4;

    // ---- Queues ----
    std::vector<std::string> queues = flags.queues;
    if (queues.empty()) queues.push_back("default");

    LOG_INFO("jq-worker starting",
             {{"server_addr", flags.server_addr},
              {"worker_id",   worker_id},
              {"concurrency", concurrency}});

    // ---- Metrics server ----
    jq::metrics::StartMetricsServer(cfg.metrics.port);

    // ---- Health server (background thread) ----
    std::thread health_thread(RunHealthServer, cfg.health.port);
    health_thread.detach();

    // ---- Signal handlers ----
    std::signal(SIGTERM, SignalHandler);
    std::signal(SIGINT,  SignalHandler);

    // ---- Worker (blocks until Shutdown()) ----
    jq::Worker worker(flags.server_addr, worker_id, concurrency, queues);
    g_worker = &worker;
    worker.Run();
    g_worker = nullptr;

    LOG_INFO("jq-worker stopped");
    return 0;
}
