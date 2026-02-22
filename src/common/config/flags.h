#pragma once

// Flag parsing for jq-server, jq-worker, and jq-ctl using getopt_long.
// Each binary calls the appropriate Parse*Flags() function in main().

#include <getopt.h>

#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace jq {

// ---------------------------------------------------------------------------
// Shared flags — all three binaries
// ---------------------------------------------------------------------------

struct CommonFlags {
    std::string config;
    std::string log_level = "info";
    bool        version   = false;
    bool        help      = false;
};

// ---------------------------------------------------------------------------
// Per-binary flag structs
// ---------------------------------------------------------------------------

struct ServerFlags : CommonFlags {
    int  grpc_port    = 0;   // 0 → use config file value
    int  metrics_port = 0;
    int  health_port  = 0;
    bool dry_run      = false;
};

struct WorkerFlags : CommonFlags {
    std::string              server_addr;
    std::string              worker_id;
    int                      concurrency  = 0;   // 0 → use config
    std::vector<std::string> queues;
    int                      metrics_port = 0;
    int                      health_port  = 0;
};

struct CtlFlags : CommonFlags {
    std::string server_addr = "localhost:50051";
    std::string tls_cert;
    std::string tls_key;
    std::string output  = "table";   // table | json | yaml
    int         timeout = 10;
};

// ---------------------------------------------------------------------------
// Help / version printers
// ---------------------------------------------------------------------------

inline void PrintVersion() {
    std::cout << "jq version 0.1.0 (build: dev)\n";
}

inline void PrintServerHelp(const char* prog) {
    std::cout
        << "Usage: " << prog << " [OPTIONS]\n\n"
        << "  --config <path>         Config file path (required)\n"
        << "  --log-level <level>     trace|debug|info|warn|error  (default: info)\n"
        << "  --grpc-port <port>      Override gRPC listen port\n"
        << "  --metrics-port <port>   Override Prometheus metrics port\n"
        << "  --health-port <port>    Override health check HTTP port\n"
        << "  --dry-run               Validate config + connectivity then exit\n"
        << "  --version               Print version and exit\n"
        << "  --help                  Show this help\n";
}

inline void PrintWorkerHelp(const char* prog) {
    std::cout
        << "Usage: " << prog << " [OPTIONS]\n\n"
        << "  --config <path>              Config file path (required)\n"
        << "  --server-addr <host:port>    jq-server gRPC address (required)\n"
        << "  --worker-id <id>             Unique worker ID (default: hostname+PID)\n"
        << "  --concurrency <n>            Max parallel jobs (default: from config)\n"
        << "  --queues <q1,q2,...>         Queue names to subscribe to\n"
        << "  --log-level <level>          trace|debug|info|warn|error\n"
        << "  --metrics-port <port>        Override Prometheus metrics port\n"
        << "  --health-port <port>         Override health check HTTP port\n"
        << "  --version                    Print version and exit\n"
        << "  --help                       Show this help\n";
}

inline void PrintCtlHelp(const char* prog) {
    std::cout
        << "Usage: " << prog << " <command> [OPTIONS]\n\n"
        << "Commands: job, queue, worker, status, version\n\n"
        << "  --server-addr <host:port>    jq-server address  (default: localhost:50051)\n"
        << "  --tls-cert <path>            Client TLS certificate\n"
        << "  --tls-key <path>             Client TLS key\n"
        << "  --output <fmt>               table|json|yaml  (default: table)\n"
        << "  --timeout <seconds>          Request timeout  (default: 10)\n"
        << "  --log-level <level>          trace|debug|info|warn|error\n"
        << "  --version                    Print version and exit\n"
        << "  --help                       Show this help\n";
}

// ---------------------------------------------------------------------------
// Internal helper — apply a common flag by short-option character
// ---------------------------------------------------------------------------

namespace detail {
inline void ApplyCommon(CommonFlags& f, int opt, const char* arg) {
    switch (opt) {
        case 'c': f.config    = arg ? arg : ""; break;
        case 'l': f.log_level = arg ? arg : "info"; break;
        case 'V': f.version   = true; break;
        case 'h': f.help      = true; break;
        default: break;
    }
}
}  // namespace detail

// ---------------------------------------------------------------------------
// Parse functions
// ---------------------------------------------------------------------------

inline ServerFlags ParseServerFlags(int argc, char** argv) {
    ServerFlags f;
    // clang-format off
    static const struct option kOpts[] = {
        {"config",       required_argument, nullptr, 'c'},
        {"log-level",    required_argument, nullptr, 'l'},
        {"grpc-port",    required_argument, nullptr, 'g'},
        {"metrics-port", required_argument, nullptr, 'm'},
        {"health-port",  required_argument, nullptr, 'H'},
        {"dry-run",      no_argument,       nullptr, 'd'},
        {"version",      no_argument,       nullptr, 'V'},
        {"help",         no_argument,       nullptr, 'h'},
        {nullptr, 0, nullptr, 0}
    };
    // clang-format on
    int opt;
    while ((opt = getopt_long(argc, argv, "c:l:g:m:H:dVh", kOpts, nullptr)) != -1) {
        switch (opt) {
            case 'g': f.grpc_port    = std::stoi(optarg); break;
            case 'm': f.metrics_port = std::stoi(optarg); break;
            case 'H': f.health_port  = std::stoi(optarg); break;
            case 'd': f.dry_run      = true;              break;
            default:  detail::ApplyCommon(f, opt, optarg); break;
        }
    }
    return f;
}

inline WorkerFlags ParseWorkerFlags(int argc, char** argv) {
    WorkerFlags f;
    // clang-format off
    static const struct option kOpts[] = {
        {"config",       required_argument, nullptr, 'c'},
        {"log-level",    required_argument, nullptr, 'l'},
        {"server-addr",  required_argument, nullptr, 's'},
        {"worker-id",    required_argument, nullptr, 'w'},
        {"concurrency",  required_argument, nullptr, 'n'},
        {"queues",       required_argument, nullptr, 'q'},
        {"metrics-port", required_argument, nullptr, 'm'},
        {"health-port",  required_argument, nullptr, 'H'},
        {"version",      no_argument,       nullptr, 'V'},
        {"help",         no_argument,       nullptr, 'h'},
        {nullptr, 0, nullptr, 0}
    };
    // clang-format on
    int opt;
    while ((opt = getopt_long(argc, argv, "c:l:s:w:n:q:m:H:Vh", kOpts, nullptr)) != -1) {
        switch (opt) {
            case 's': f.server_addr  = optarg; break;
            case 'w': f.worker_id    = optarg; break;
            case 'n': f.concurrency  = std::stoi(optarg); break;
            case 'q': {
                std::istringstream ss(optarg);
                std::string part;
                while (std::getline(ss, part, ','))
                    if (!part.empty()) f.queues.push_back(part);
                break;
            }
            case 'm': f.metrics_port = std::stoi(optarg); break;
            case 'H': f.health_port  = std::stoi(optarg); break;
            default:  detail::ApplyCommon(f, opt, optarg); break;
        }
    }
    return f;
}

inline CtlFlags ParseCtlFlags(int argc, char** argv) {
    CtlFlags f;
    // clang-format off
    static const struct option kOpts[] = {
        {"config",      required_argument, nullptr, 'c'},
        {"log-level",   required_argument, nullptr, 'l'},
        {"server-addr", required_argument, nullptr, 's'},
        {"tls-cert",    required_argument, nullptr, 'C'},
        {"tls-key",     required_argument, nullptr, 'k'},
        {"output",      required_argument, nullptr, 'o'},
        {"timeout",     required_argument, nullptr, 't'},
        {"version",     no_argument,       nullptr, 'V'},
        {"help",        no_argument,       nullptr, 'h'},
        {nullptr, 0, nullptr, 0}
    };
    // clang-format on
    int opt;
    while ((opt = getopt_long(argc, argv, "c:l:s:C:k:o:t:Vh", kOpts, nullptr)) != -1) {
        switch (opt) {
            case 's': f.server_addr = optarg; break;
            case 'C': f.tls_cert    = optarg; break;
            case 'k': f.tls_key     = optarg; break;
            case 'o': f.output      = optarg; break;
            case 't': f.timeout     = std::stoi(optarg); break;
            default:  detail::ApplyCommon(f, opt, optarg); break;
        }
    }
    return f;
}

}  // namespace jq
