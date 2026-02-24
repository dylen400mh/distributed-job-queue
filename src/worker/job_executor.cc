#include "worker/job_executor.h"

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <errno.h>

#include <chrono>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "common/logging/logger.h"

namespace jq {

namespace {

constexpr std::size_t kMaxOutputBytes = 1024 * 1024;  // 1 MiB cap

// Read all available bytes from fd until EOF or cap reached, with a
// per-read timeout of poll_ms milliseconds (0 = non-blocking).
std::string DrainFd(int fd, int poll_ms) {
    std::string buf;
    char chunk[4096];
    while (buf.size() < kMaxOutputBytes) {
        struct pollfd pfd{fd, POLLIN, 0};
        int rc = ::poll(&pfd, 1, poll_ms);
        if (rc <= 0) break;  // timeout or error — return what we have
        if (!(pfd.revents & POLLIN)) break;
        ssize_t n = ::read(fd, chunk, sizeof(chunk));
        if (n <= 0) break;  // EOF or error
        buf.append(chunk, static_cast<std::size_t>(n));
    }
    return buf;
}

}  // namespace

// ---------------------------------------------------------------------------
// Execute
// ---------------------------------------------------------------------------

ExecutionResult JobExecutor::Execute(const std::string& job_id,
                                      const std::string& payload_json,
                                      int                ttl_seconds) {
    ExecutionResult result;

    // --- Parse payload ---
    std::vector<std::string> cmd_args;
    try {
        auto j = nlohmann::json::parse(payload_json);
        if (!j.contains("command") || !j["command"].is_array()) {
            result.error_message = "payload missing 'command' array";
            return result;
        }
        for (const auto& arg : j["command"]) {
            if (!arg.is_string()) {
                result.error_message = "'command' array must contain only strings";
                return result;
            }
            cmd_args.push_back(arg.get<std::string>());
        }
    } catch (const std::exception& e) {
        result.error_message = std::string("payload parse error: ") + e.what();
        return result;
    }

    if (cmd_args.empty()) {
        result.error_message = "'command' array is empty";
        return result;
    }

    // --- Build argv ---
    std::vector<char*> argv;
    argv.reserve(cmd_args.size() + 1);
    for (auto& s : cmd_args) argv.push_back(s.data());
    argv.push_back(nullptr);

    // --- Create pipe for stdout+stderr ---
    int pipefd[2];
    if (::pipe(pipefd) != 0) {
        result.error_message = std::string("pipe() failed: ") + std::strerror(errno);
        return result;
    }

    LOG_DEBUG("Executing job", {{"job_id", job_id}, {"cmd", cmd_args[0]}});

    // --- Fork ---
    pid_t child_pid = ::fork();
    if (child_pid < 0) {
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        result.error_message = std::string("fork() failed: ") + std::strerror(errno);
        return result;
    }

    if (child_pid == 0) {
        // --- Child ---
        ::close(pipefd[0]);                      // close read end
        ::dup2(pipefd[1], STDOUT_FILENO);
        ::dup2(pipefd[1], STDERR_FILENO);
        ::close(pipefd[1]);

        ::execvp(argv[0], argv.data());
        // execvp failed — write error and exit.
        const char* err = std::strerror(errno);
        ::write(STDERR_FILENO, err, std::strlen(err));
        ::_exit(127);
    }

    // --- Parent ---
    ::close(pipefd[1]);  // close write end; only child writes

    const auto start = std::chrono::steady_clock::now();
    const int timeout_ms = (ttl_seconds > 0) ? ttl_seconds * 1000 : -1;

    std::string output;
    bool timed_out = false;

    while (output.size() < kMaxOutputBytes) {
        // Remaining time until timeout.
        int remaining_ms = timeout_ms;
        if (timeout_ms > 0) {
            auto elapsed_ms = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start).count());
            remaining_ms = timeout_ms - elapsed_ms;
            if (remaining_ms <= 0) {
                timed_out = true;
                break;
            }
        }

        struct pollfd pfd{pipefd[0], POLLIN, 0};
        int rc = ::poll(&pfd, 1, remaining_ms > 0 ? remaining_ms : -1);
        if (rc < 0) break;   // interrupted
        if (rc == 0) { timed_out = true; break; }
        if (!(pfd.revents & POLLIN)) break;

        char chunk[4096];
        ssize_t n = ::read(pipefd[0], chunk, sizeof(chunk));
        if (n <= 0) break;   // EOF
        output.append(chunk, static_cast<std::size_t>(n));
    }

    // Drain any remaining bytes if we hit the cap (non-blocking).
    if (!timed_out && output.size() < kMaxOutputBytes) {
        output += DrainFd(pipefd[0], 0);
    }

    ::close(pipefd[0]);

    if (timed_out) {
        ::kill(child_pid, SIGKILL);
        LOG_WARN("Job execution timed out; process killed",
                 {{"job_id", job_id}, {"ttl_s", ttl_seconds}});
    }

    // Wait for child.
    int wstatus = 0;
    ::waitpid(child_pid, &wstatus, 0);

    result.output = std::move(output);

    if (timed_out) {
        result.success       = false;
        result.error_message = "job exceeded TTL (" + std::to_string(ttl_seconds) + "s)";
        return result;
    }

    if (WIFEXITED(wstatus)) {
        int code = WEXITSTATUS(wstatus);
        result.success = (code == 0);
        if (code != 0) {
            result.error_message = "process exited with code " + std::to_string(code);
        }
    } else if (WIFSIGNALED(wstatus)) {
        result.success       = false;
        result.error_message = "process killed by signal " + std::to_string(WTERMSIG(wstatus));
    } else {
        result.success       = false;
        result.error_message = "process exited abnormally";
    }

    LOG_DEBUG("Job execution complete",
              {{"job_id", job_id}, {"success", result.success}});
    return result;
}

}  // namespace jq
