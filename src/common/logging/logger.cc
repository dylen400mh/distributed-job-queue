#include "common/logging/logger.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>

#include <spdlog/sinks/null_sink.h>
#include <spdlog/spdlog.h>

namespace jq::log {

namespace {

std::string                g_service;
thread_local std::string   t_correlation_id;
std::mutex                 g_out_mutex;

spdlog::level::level_enum ParseLevel(const std::string& s) {
    if (s == "trace") return spdlog::level::trace;
    if (s == "debug") return spdlog::level::debug;
    if (s == "warn")  return spdlog::level::warn;
    if (s == "error") return spdlog::level::err;
    return spdlog::level::info;
}

const char* LevelStr(spdlog::level::level_enum l) {
    switch (l) {
        case spdlog::level::trace:    return "trace";
        case spdlog::level::debug:    return "debug";
        case spdlog::level::warn:     return "warn";
        case spdlog::level::err:      return "error";
        case spdlog::level::critical: return "critical";
        default:                      return "info";
    }
}

// ISO-8601 UTC timestamp with millisecond precision.
std::string NowIso8601() {
    const auto now    = std::chrono::system_clock::now();
    const auto t      = std::chrono::system_clock::to_time_t(now);
    const auto ms     = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now.time_since_epoch()) % 1000;
    std::tm tm{};
    gmtime_r(&t, &tm);
    std::ostringstream ss;
    ss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S")
       << '.' << std::setfill('0') << std::setw(3) << ms.count() << 'Z';
    return ss.str();
}

}  // namespace

// ---------------------------------------------------------------------------

void InitLogger(const std::string& service_name, const std::string& level) {
    g_service = service_name;

    // Use a null spdlog sink — we write JSON output ourselves in Log().
    // spdlog is only used here for its level-filter state.
    auto null_sink = std::make_shared<spdlog::sinks::null_sink_mt>();
    auto logger    = std::make_shared<spdlog::logger>("jq", null_sink);
    logger->set_level(ParseLevel(level));
    spdlog::set_default_logger(logger);
}

void SetCorrelationId(const std::string& id) { t_correlation_id = id; }
std::string GetCorrelationId()               { return t_correlation_id; }

void Log(spdlog::level::level_enum level,
         std::string_view           msg,
         nlohmann::json             extra) {
    // Respect the configured log level.
    if (!spdlog::default_logger()->should_log(level)) return;

    nlohmann::json entry;
    entry["timestamp"] = NowIso8601();
    entry["level"]     = LevelStr(level);
    entry["service"]   = g_service;
    entry["message"]   = std::string(msg);

    if (!t_correlation_id.empty())
        entry["correlation_id"] = t_correlation_id;

    for (auto& [k, v] : extra.items())
        entry[k] = v;

    // Serialise and write atomically so concurrent threads don't interleave.
    const std::string line = entry.dump() + '\n';
    std::lock_guard<std::mutex> lk(g_out_mutex);
    std::cout << line;
}

}  // namespace jq::log
