#include "ctl/commands/commands.h"

#include <getopt.h>

#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#include "common.pb.h"
#include "job_service.pb.h"

namespace jq::ctl {

namespace {

// Format a proto Timestamp as "YYYY-MM-DD HH:MM:SS UTC" or "-".
std::string FmtTs(const google::protobuf::Timestamp& ts) {
    if (ts.seconds() == 0) return "-";
    std::time_t t = static_cast<std::time_t>(ts.seconds());
    char buf[32];
    struct tm tm_info{};
    ::gmtime_r(&t, &tm_info);
    ::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_info);
    return std::string(buf) + " UTC";
}

std::string StatusStr(jq::JobStatus s) {
    switch (s) {
        case jq::PENDING:       return "PENDING";
        case jq::ASSIGNED:      return "ASSIGNED";
        case jq::RUNNING:       return "RUNNING";
        case jq::DONE:          return "DONE";
        case jq::FAILED:        return "FAILED";
        case jq::DEAD_LETTERED: return "DEAD_LETTERED";
        default:                return "UNKNOWN";
    }
}

jq::JobStatus ParseStatus(const std::string& s) {
    if (s == "PENDING")       return jq::PENDING;
    if (s == "ASSIGNED")      return jq::ASSIGNED;
    if (s == "RUNNING")       return jq::RUNNING;
    if (s == "DONE")          return jq::DONE;
    if (s == "FAILED")        return jq::FAILED;
    if (s == "DEAD_LETTERED") return jq::DEAD_LETTERED;
    return jq::JOB_STATUS_UNSPECIFIED;
}

nlohmann::json JobToJson(const jq::Job& j) {
    nlohmann::json o;
    o["job_id"]      = j.job_id();
    o["queue"]       = j.queue_name();
    o["status"]      = StatusStr(j.status());
    o["priority"]    = j.priority();
    o["retry_count"] = j.retry_count();
    o["max_retries"] = j.max_retries();
    o["created_at"]  = FmtTs(j.created_at());
    o["started_at"]  = FmtTs(j.started_at());
    o["completed_at"]= FmtTs(j.completed_at());
    o["worker_id"]   = j.worker_id();
    if (!j.error_message().empty()) o["error"] = j.error_message();
    return o;
}

void PrintJob(const jq::Job& job, OutputFormat fmt) {
    if (fmt == OutputFormat::TABLE) {
        std::vector<std::string> headers = {
            "FIELD", "VALUE"
        };
        std::vector<std::vector<std::string>> rows = {
            {"job_id",      job.job_id()},
            {"queue",       job.queue_name()},
            {"status",      StatusStr(job.status())},
            {"priority",    std::to_string(job.priority())},
            {"retry_count", std::to_string(job.retry_count())},
            {"max_retries", std::to_string(job.max_retries())},
            {"worker_id",   job.worker_id()},
            {"created_at",  FmtTs(job.created_at())},
            {"started_at",  FmtTs(job.started_at())},
            {"completed_at",FmtTs(job.completed_at())},
            {"error",       job.error_message()},
        };
        std::cout << FormatTable(headers, rows);
    } else if (fmt == OutputFormat::JSON) {
        std::cout << FormatJson(JobToJson(job));
    } else {
        std::cout << FormatYaml(JobToJson(job));
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// job submit
// ---------------------------------------------------------------------------

int RunJobSubmit(GrpcClient& client, OutputFormat fmt, int argc, char** argv) {
    std::string queue;
    std::string payload;
    int priority    = 0;
    int max_retries = 0;
    int ttl_seconds = 0;

    static const struct option kOpts[] = {
        {"queue",       required_argument, nullptr, 'q'},
        {"payload",     required_argument, nullptr, 'p'},
        {"priority",    required_argument, nullptr, 'P'},
        {"max-retries", required_argument, nullptr, 'r'},
        {"ttl",         required_argument, nullptr, 't'},
        {nullptr, 0, nullptr, 0}
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "q:p:P:r:t:", kOpts, nullptr)) != -1) {
        switch (opt) {
            case 'q': queue       = optarg; break;
            case 'p': payload     = optarg; break;
            case 'P': priority    = std::stoi(optarg); break;
            case 'r': max_retries = std::stoi(optarg); break;
            case 't': ttl_seconds = std::stoi(optarg); break;
            default: break;
        }
    }

    if (queue.empty()) {
        std::cerr << "Usage: jq-ctl job submit --queue <name> --payload <json|@file>\n";
        return 1;
    }
    if (payload.empty()) {
        std::cerr << "Error: --payload is required\n";
        return 1;
    }

    // Support @file syntax.
    if (!payload.empty() && payload[0] == '@') {
        std::ifstream f(payload.substr(1));
        if (!f) { std::cerr << "Cannot open file: " << payload.substr(1) << '\n'; return 1; }
        std::ostringstream ss;
        ss << f.rdbuf();
        payload = ss.str();
    }

    SubmitJobRequest req;
    req.set_queue_name(queue);
    req.set_payload(payload.data(), payload.size());
    req.set_priority(priority);
    req.set_max_retries(max_retries);
    req.set_ttl_seconds(ttl_seconds);

    auto resp = client.SubmitJob(req);
    if (fmt == OutputFormat::TABLE) {
        std::cout << resp.job_id() << '\n';
    } else if (fmt == OutputFormat::JSON) {
        std::cout << FormatJson({{"job_id", resp.job_id()}});
    } else {
        std::cout << "job_id: " << resp.job_id() << '\n';
    }
    return 0;
}

// ---------------------------------------------------------------------------
// job status
// ---------------------------------------------------------------------------

int RunJobStatus(GrpcClient& client, OutputFormat fmt, int argc, char** argv) {
    if (optind >= argc) {
        std::cerr << "Usage: jq-ctl job status <job-id>\n";
        return 1;
    }
    std::string job_id = argv[optind++];

    GetJobStatusRequest req;
    req.set_job_id(job_id);
    auto resp = client.GetJobStatus(req);
    PrintJob(resp.job(), fmt);
    return 0;
}

// ---------------------------------------------------------------------------
// job cancel
// ---------------------------------------------------------------------------

int RunJobCancel(GrpcClient& client, OutputFormat fmt, int argc, char** argv) {
    if (optind >= argc) {
        std::cerr << "Usage: jq-ctl job cancel <job-id>\n";
        return 1;
    }
    std::string job_id = argv[optind++];

    CancelJobRequest req;
    req.set_job_id(job_id);
    client.CancelJob(req);
    std::cout << "Cancelled " << job_id << '\n';
    return 0;
}

// ---------------------------------------------------------------------------
// job list
// ---------------------------------------------------------------------------

int RunJobList(GrpcClient& client, OutputFormat fmt, int argc, char** argv) {
    std::string queue;
    std::string status_str;
    int         limit = 20;

    static const struct option kOpts[] = {
        {"queue",  required_argument, nullptr, 'q'},
        {"status", required_argument, nullptr, 's'},
        {"limit",  required_argument, nullptr, 'l'},
        {nullptr, 0, nullptr, 0}
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "q:s:l:", kOpts, nullptr)) != -1) {
        switch (opt) {
            case 'q': queue      = optarg; break;
            case 's': status_str = optarg; break;
            case 'l': limit      = std::stoi(optarg); break;
            default: break;
        }
    }

    ListJobsRequest req;
    if (!queue.empty())      req.set_queue_name(queue);
    if (!status_str.empty()) req.set_status(ParseStatus(status_str));
    req.set_limit(limit);

    auto resp = client.ListJobs(req);

    if (fmt == OutputFormat::TABLE) {
        std::vector<std::string> headers = {
            "JOB_ID", "QUEUE", "STATUS", "PRI", "RETRY", "CREATED_AT"
        };
        std::vector<std::vector<std::string>> rows;
        for (const auto& job : resp.jobs()) {
            rows.push_back({
                job.job_id(),
                job.queue_name(),
                StatusStr(job.status()),
                std::to_string(job.priority()),
                std::to_string(job.retry_count()) + "/" + std::to_string(job.max_retries()),
                FmtTs(job.created_at()),
            });
        }
        std::cout << FormatTable(headers, rows);
        if (!resp.next_page_token().empty()) {
            std::cout << "(next_page_token: " << resp.next_page_token() << ")\n";
        }
    } else {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& job : resp.jobs()) arr.push_back(JobToJson(job));
        nlohmann::json j = {{"jobs", arr}};
        if (!resp.next_page_token().empty())
            j["next_page_token"] = resp.next_page_token();
        if (fmt == OutputFormat::JSON) std::cout << FormatJson(j);
        else                           std::cout << FormatYaml(j);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// job logs
// ---------------------------------------------------------------------------

int RunJobLogs(GrpcClient& client, OutputFormat fmt, int argc, char** argv) {
    if (optind >= argc) {
        std::cerr << "Usage: jq-ctl job logs <job-id>\n";
        return 1;
    }
    std::string job_id = argv[optind++];

    GetJobLogsRequest req;
    req.set_job_id(job_id);
    auto resp = client.GetJobLogs(req);

    if (fmt == OutputFormat::TABLE) {
        std::vector<std::string> headers = {"TIMESTAMP", "FROM", "TO", "REASON", "WORKER"};
        std::vector<std::vector<std::string>> rows;
        for (const auto& ev : resp.events()) {
            rows.push_back({
                FmtTs(ev.occurred_at()),
                StatusStr(ev.from_status()),
                StatusStr(ev.to_status()),
                ev.reason(),
                ev.worker_id(),
            });
        }
        std::cout << FormatTable(headers, rows);
    } else {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& ev : resp.events()) {
            arr.push_back({
                {"occurred_at",  FmtTs(ev.occurred_at())},
                {"from_status",  StatusStr(ev.from_status())},
                {"to_status",    StatusStr(ev.to_status())},
                {"reason",       ev.reason()},
                {"worker_id",    ev.worker_id()},
            });
        }
        nlohmann::json j = {{"events", arr}};
        if (fmt == OutputFormat::JSON) std::cout << FormatJson(j);
        else                           std::cout << FormatYaml(j);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// job retry
// ---------------------------------------------------------------------------

int RunJobRetry(GrpcClient& client, OutputFormat fmt, int argc, char** argv) {
    if (optind >= argc) {
        std::cerr << "Usage: jq-ctl job retry <job-id>\n";
        return 1;
    }
    std::string job_id = argv[optind++];

    RetryJobRequest req;
    req.set_job_id(job_id);
    auto resp = client.RetryJob(req);
    std::cout << "Retrying job " << resp.job_id() << '\n';
    return 0;
}

}  // namespace jq::ctl
