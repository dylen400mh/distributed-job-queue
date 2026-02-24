#include "ctl/commands/commands.h"

#include <getopt.h>

#include <ctime>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

#include "admin_service.pb.h"
#include "common.pb.h"

namespace jq::ctl {

namespace {

std::string FmtTs(const google::protobuf::Timestamp& ts) {
    if (ts.seconds() == 0) return "-";
    std::time_t t = static_cast<std::time_t>(ts.seconds());
    char buf[32];
    struct tm tm_info{};
    ::gmtime_r(&t, &tm_info);
    ::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_info);
    return std::string(buf) + " UTC";
}

nlohmann::json QueueToJson(const jq::Queue& q) {
    nlohmann::json o;
    o["name"]        = q.name();
    o["max_retries"] = q.max_retries();
    o["ttl_seconds"] = q.ttl_seconds();
    o["created_at"]  = FmtTs(q.created_at());
    return o;
}

nlohmann::json StatsToJson(const jq::QueueStats& s) {
    nlohmann::json o;
    o["queue_name"]      = s.queue_name();
    o["pending"]         = s.pending_count();
    o["running"]         = s.running_count();
    o["failed"]          = s.failed_count();
    o["done"]            = s.done_count();
    o["dead_letter"]     = s.dead_letter_count();
    o["total_processed"] = s.total_processed();
    o["avg_duration_ms"] = s.avg_duration_ms();
    o["error_rate"]      = s.error_rate();
    return o;
}

}  // namespace

// ---------------------------------------------------------------------------
// queue list
// ---------------------------------------------------------------------------

int RunQueueList(GrpcClient& client, OutputFormat fmt, int argc, char** argv) {
    ListQueuesRequest req;
    auto resp = client.ListQueues(req);

    if (fmt == OutputFormat::TABLE) {
        std::vector<std::string> headers = {
            "NAME", "MAX_RETRIES", "TTL_SECONDS", "CREATED_AT"
        };
        std::vector<std::vector<std::string>> rows;
        for (const auto& q : resp.queues()) {
            rows.push_back({
                q.name(),
                std::to_string(q.max_retries()),
                q.ttl_seconds() == 0 ? "-" : std::to_string(q.ttl_seconds()),
                FmtTs(q.created_at()),
            });
        }
        std::cout << FormatTable(headers, rows);
    } else {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& q : resp.queues()) arr.push_back(QueueToJson(q));
        nlohmann::json j = {{"queues", arr}};
        if (fmt == OutputFormat::JSON) std::cout << FormatJson(j);
        else                           std::cout << FormatYaml(j);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// queue create
// ---------------------------------------------------------------------------

int RunQueueCreate(GrpcClient& client, OutputFormat fmt, int argc, char** argv) {
    std::string name;
    int max_retries = 0;
    int ttl_seconds = 0;

    static const struct option kOpts[] = {
        {"name",        required_argument, nullptr, 'n'},
        {"max-retries", required_argument, nullptr, 'r'},
        {"ttl",         required_argument, nullptr, 't'},
        {nullptr, 0, nullptr, 0}
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "n:r:t:", kOpts, nullptr)) != -1) {
        switch (opt) {
            case 'n': name        = optarg; break;
            case 'r': max_retries = std::stoi(optarg); break;
            case 't': ttl_seconds = std::stoi(optarg); break;
            default: break;
        }
    }

    if (name.empty()) {
        std::cerr << "Usage: jq-ctl queue create --name <name> [--max-retries N] [--ttl N]\n";
        return 1;
    }

    CreateQueueRequest req;
    req.set_name(name);
    req.set_max_retries(max_retries);
    req.set_ttl_seconds(ttl_seconds);
    auto resp = client.CreateQueue(req);

    if (fmt == OutputFormat::TABLE) {
        std::cout << "Created queue: " << resp.queue().name() << '\n';
    } else if (fmt == OutputFormat::JSON) {
        std::cout << FormatJson(QueueToJson(resp.queue()));
    } else {
        std::cout << FormatYaml(QueueToJson(resp.queue()));
    }
    return 0;
}

// ---------------------------------------------------------------------------
// queue delete
// ---------------------------------------------------------------------------

int RunQueueDelete(GrpcClient& client, OutputFormat fmt, int argc, char** argv) {
    bool force = false;

    static const struct option kOpts[] = {
        {"force", no_argument, nullptr, 'f'},
        {nullptr, 0, nullptr, 0}
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "f", kOpts, nullptr)) != -1) {
        if (opt == 'f') force = true;
    }

    if (optind >= argc) {
        std::cerr << "Usage: jq-ctl queue delete [--force] <name>\n";
        return 1;
    }
    std::string name = argv[optind++];

    DeleteQueueRequest req;
    req.set_name(name);
    req.set_force(force);
    client.DeleteQueue(req);
    std::cout << "Deleted queue: " << name << '\n';
    return 0;
}

// ---------------------------------------------------------------------------
// queue stats
// ---------------------------------------------------------------------------

int RunQueueStats(GrpcClient& client, OutputFormat fmt, int argc, char** argv) {
    if (optind >= argc) {
        std::cerr << "Usage: jq-ctl queue stats <name>\n";
        return 1;
    }
    std::string name = argv[optind++];

    GetQueueStatsRequest req;
    req.set_name(name);
    auto resp = client.GetQueueStats(req);
    const auto& s = resp.stats();

    if (fmt == OutputFormat::TABLE) {
        std::vector<std::string> headers = {"FIELD", "VALUE"};
        std::vector<std::vector<std::string>> rows = {
            {"queue_name",      s.queue_name()},
            {"pending",         std::to_string(s.pending_count())},
            {"running",         std::to_string(s.running_count())},
            {"failed",          std::to_string(s.failed_count())},
            {"done",            std::to_string(s.done_count())},
            {"dead_letter",     std::to_string(s.dead_letter_count())},
            {"total_processed", std::to_string(s.total_processed())},
            {"avg_duration_ms", std::to_string(s.avg_duration_ms())},
            {"error_rate",      std::to_string(s.error_rate())},
        };
        std::cout << FormatTable(headers, rows);
    } else if (fmt == OutputFormat::JSON) {
        std::cout << FormatJson(StatsToJson(s));
    } else {
        std::cout << FormatYaml(StatsToJson(s));
    }
    return 0;
}

}  // namespace jq::ctl
