#pragma once

#include <string>
#include <vector>

namespace jq {

// ---------------------------------------------------------------------------
// ExecutionResult — outcome of running a single job payload.
// ---------------------------------------------------------------------------
struct ExecutionResult {
    bool        success       = false;
    std::string output;         // combined stdout + stderr (truncated at 1 MiB)
    std::string error_message;  // human-readable failure description
};

// ---------------------------------------------------------------------------
// JobExecutor — runs job payloads as subprocesses via fork/exec.
//
// The payload JSON must contain a "command" field: an array of strings
// (argv[0] is the executable, remaining entries are arguments).
//
// Example payload:
//   {"command": ["echo", "hello world"]}
//
// Per-job timeout: if ttl_seconds > 0 the subprocess is killed with SIGKILL
// after that many seconds have elapsed.
// ---------------------------------------------------------------------------
class JobExecutor {
public:
    // Execute a job; blocks until the subprocess exits or times out.
    // job_id is logged for observability but not passed to the subprocess.
    static ExecutionResult Execute(const std::string& job_id,
                                   const std::string& payload_json,
                                   int                ttl_seconds = 0);
};

}  // namespace jq
