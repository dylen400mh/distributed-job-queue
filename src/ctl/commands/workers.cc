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

std::string WorkerStatusStr(jq::WorkerStatus s) {
    switch (s) {
        case jq::ONLINE:    return "ONLINE";
        case jq::OFFLINE:   return "OFFLINE";
        case jq::DRAINING:  return "DRAINING";
        default:            return "UNKNOWN";
    }
}

nlohmann::json WorkerToJson(const jq::Worker& w) {
    nlohmann::json o;
    o["worker_id"]       = w.worker_id();
    o["hostname"]        = w.hostname();
    o["status"]          = WorkerStatusStr(w.status());
    o["concurrency"]     = w.concurrency();
    o["active_job_count"]= w.active_job_count();
    o["last_heartbeat"]  = FmtTs(w.last_heartbeat());
    nlohmann::json queues = nlohmann::json::array();
    for (const auto& q : w.queues()) queues.push_back(q);
    o["queues"] = queues;
    return o;
}

}  // namespace

// ---------------------------------------------------------------------------
// worker list
// ---------------------------------------------------------------------------

int RunWorkerList(GrpcClient& client, OutputFormat fmt, int argc, char** argv) {
    ListWorkersRequest req;
    auto resp = client.ListWorkers(req);

    if (fmt == OutputFormat::TABLE) {
        std::vector<std::string> headers = {
            "WORKER_ID", "HOSTNAME", "STATUS", "ACTIVE/CONC", "LAST_HEARTBEAT"
        };
        std::vector<std::vector<std::string>> rows;
        for (const auto& w : resp.workers()) {
            rows.push_back({
                w.worker_id(),
                w.hostname(),
                WorkerStatusStr(w.status()),
                std::to_string(w.active_job_count()) + "/" + std::to_string(w.concurrency()),
                FmtTs(w.last_heartbeat()),
            });
        }
        std::cout << FormatTable(headers, rows);
    } else {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& w : resp.workers()) arr.push_back(WorkerToJson(w));
        nlohmann::json j = {{"workers", arr}};
        if (fmt == OutputFormat::JSON) std::cout << FormatJson(j);
        else                           std::cout << FormatYaml(j);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// worker drain
// ---------------------------------------------------------------------------

int RunWorkerDrain(GrpcClient& client, OutputFormat fmt, int argc, char** argv) {
    if (optind >= argc) {
        std::cerr << "Usage: jq-ctl worker drain <worker-id>\n";
        return 1;
    }
    std::string worker_id = argv[optind++];

    DrainWorkerRequest req;
    req.set_worker_id(worker_id);
    client.DrainWorker(req);
    std::cout << "Draining worker " << worker_id << '\n';
    return 0;
}

// ---------------------------------------------------------------------------
// worker shutdown
// ---------------------------------------------------------------------------

int RunWorkerShutdown(GrpcClient& client, OutputFormat fmt, int argc, char** argv) {
    if (optind >= argc) {
        std::cerr << "Usage: jq-ctl worker shutdown <worker-id>\n";
        return 1;
    }
    std::string worker_id = argv[optind++];

    ShutdownWorkerRequest req;
    req.set_worker_id(worker_id);
    client.ShutdownWorker(req);
    std::cout << "Shutdown worker " << worker_id << '\n';
    return 0;
}

}  // namespace jq::ctl
