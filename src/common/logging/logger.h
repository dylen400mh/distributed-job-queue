#pragma once

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace jq::log {

// Initialize the global JSON logger.  Must be called once at startup.
//   service_name : "jq-server" | "jq-worker" | "jq-ctl"
//   level        : "trace" | "debug" | "info" | "warn" | "error"
void InitLogger(const std::string& service_name,
                const std::string& level = "info");

// Thread-local correlation ID — automatically included in every log line
// emitted on the calling thread.
void        SetCorrelationId(const std::string& id);
std::string GetCorrelationId();

// Emit one structured JSON log line.
// extra: optional additional fields merged into the JSON object.
void Log(spdlog::level::level_enum level,
         std::string_view           msg,
         nlohmann::json             extra = {});

}  // namespace jq::log

// ---------------------------------------------------------------------------
// Convenience macros
//
// Usage:
//   LOG_INFO("server started")
//   LOG_INFO("listening", {{"port", 50051}, {"tls", false}})
// ---------------------------------------------------------------------------
#define LOG_INFO(msg, ...)  ::jq::log::Log(spdlog::level::info,  (msg), ##__VA_ARGS__)
#define LOG_WARN(msg, ...)  ::jq::log::Log(spdlog::level::warn,  (msg), ##__VA_ARGS__)
#define LOG_ERROR(msg, ...) ::jq::log::Log(spdlog::level::err,   (msg), ##__VA_ARGS__)
#define LOG_DEBUG(msg, ...) ::jq::log::Log(spdlog::level::debug, (msg), ##__VA_ARGS__)
