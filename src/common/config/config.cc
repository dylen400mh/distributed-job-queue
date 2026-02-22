#include "common/config/config.h"

#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <string>

#include <yaml-cpp/yaml.h>

namespace jq {

namespace {

// ---------------------------------------------------------------------------
// Env var helpers
// ---------------------------------------------------------------------------

// Return the value of env var `name`, or `def` if unset/empty.
std::string Env(const char* name, const std::string& def = "") {
    const char* v = std::getenv(name);
    return (v && *v) ? std::string(v) : def;
}

int EnvInt(const char* name, int def) {
    const char* v = std::getenv(name);
    if (v && *v) {
        try { return std::stoi(v); } catch (...) {}
    }
    return def;
}

bool EnvBool(const char* name, bool def) {
    const char* v = std::getenv(name);
    if (!v || !*v) return def;
    std::string s(v);
    return s == "1" || s == "true" || s == "yes";
}

std::vector<std::string> SplitComma(const std::string& s) {
    std::vector<std::string> parts;
    std::istringstream ss(s);
    std::string part;
    while (std::getline(ss, part, ',')) {
        if (!part.empty()) parts.push_back(part);
    }
    return parts;
}

}  // namespace

// ---------------------------------------------------------------------------
// LoadConfig
// ---------------------------------------------------------------------------

Config LoadConfig(const std::string& path) {
    Config cfg;

    YAML::Node y;
    try {
        y = YAML::LoadFile(path);
    } catch (const YAML::Exception& e) {
        throw std::runtime_error("Failed to load config file '" + path +
                                 "': " + e.what());
    }

    // -- grpc ----------------------------------------------------------------
    if (auto g = y["grpc"]) {
        if (g["port"]) cfg.grpc.port = g["port"].as<int>();
        if (auto t = g["tls"]) {
            if (t["enabled"])   cfg.grpc.tls.enabled   = t["enabled"].as<bool>();
            if (t["cert_path"]) cfg.grpc.tls.cert_path = t["cert_path"].as<std::string>();
            if (t["key_path"])  cfg.grpc.tls.key_path  = t["key_path"].as<std::string>();
        }
    }

    // -- db ------------------------------------------------------------------
    if (auto d = y["db"]) {
        if (d["host"])               cfg.db.host               = d["host"].as<std::string>();
        if (d["port"])               cfg.db.port               = d["port"].as<int>();
        if (d["name"])               cfg.db.name               = d["name"].as<std::string>();
        if (d["user"])               cfg.db.user               = d["user"].as<std::string>();
        if (d["password"])           cfg.db.password           = d["password"].as<std::string>();
        if (d["pool_size"])          cfg.db.pool_size          = d["pool_size"].as<int>();
        if (d["connect_timeout_ms"]) cfg.db.connect_timeout_ms = d["connect_timeout_ms"].as<int>();
    }

    // -- redis ---------------------------------------------------------------
    if (auto r = y["redis"]) {
        if (r["addr"])               cfg.redis.addr               = r["addr"].as<std::string>();
        if (r["password"])           cfg.redis.password           = r["password"].as<std::string>();
        if (r["db"])                 cfg.redis.db                 = r["db"].as<int>();
        if (r["connect_timeout_ms"]) cfg.redis.connect_timeout_ms = r["connect_timeout_ms"].as<int>();
    }

    // -- kafka ---------------------------------------------------------------
    if (auto k = y["kafka"]) {
        if (k["brokers"]) {
            cfg.kafka.brokers.clear();
            for (const auto& b : k["brokers"])
                cfg.kafka.brokers.push_back(b.as<std::string>());
        }
        if (auto p = k["producer"]) {
            if (p["acks"])        cfg.kafka.producer.acks        = p["acks"].as<std::string>();
            if (p["compression"]) cfg.kafka.producer.compression = p["compression"].as<std::string>();
        }
        if (auto c = k["consumer"]) {
            if (c["group_id"])          cfg.kafka.consumer.group_id          = c["group_id"].as<std::string>();
            if (c["auto_offset_reset"]) cfg.kafka.consumer.auto_offset_reset = c["auto_offset_reset"].as<std::string>();
        }
        if (auto s = k["sasl"]) {
            if (s["username"]) cfg.kafka.sasl.username = s["username"].as<std::string>();
            if (s["password"]) cfg.kafka.sasl.password = s["password"].as<std::string>();
        }
    }

    // -- scheduler -----------------------------------------------------------
    if (auto s = y["scheduler"]) {
        if (s["interval_ms"])                cfg.scheduler.interval_ms               = s["interval_ms"].as<int>();
        if (s["batch_size"])                 cfg.scheduler.batch_size                = s["batch_size"].as<int>();
        if (s["assignment_timeout_s"])       cfg.scheduler.assignment_timeout_s      = s["assignment_timeout_s"].as<int>();
        if (s["worker_heartbeat_timeout_s"]) cfg.scheduler.worker_heartbeat_timeout_s= s["worker_heartbeat_timeout_s"].as<int>();
    }

    // -- metrics / health / logging ------------------------------------------
    if (auto m = y["metrics"]) { if (m["port"]) cfg.metrics.port = m["port"].as<int>(); }
    if (auto h = y["health"])  { if (h["port"]) cfg.health.port  = h["port"].as<int>(); }
    if (auto l = y["logging"]) {
        if (l["level"])  cfg.logging.level  = l["level"].as<std::string>();
        if (l["format"]) cfg.logging.format = l["format"].as<std::string>();
    }

    // -------------------------------------------------------------------------
    // Sensitive field check — warn if credentials appear in the YAML file.
    // Env vars should be used exclusively for secrets.
    // -------------------------------------------------------------------------
    auto warn_if_in_yaml = [&](const std::string& yaml_value,
                               const char* env_name,
                               const std::string& field_name) {
        if (!yaml_value.empty()) {
            cfg.sensitive_field_warnings.push_back(
                "Sensitive field '" + field_name +
                "' is set in config file. Use env var " +
                std::string(env_name) + " instead.");
        }
    };
    warn_if_in_yaml(cfg.db.password,        "JQ_DB_PASSWORD",        "db.password");
    warn_if_in_yaml(cfg.redis.password,     "JQ_REDIS_PASSWORD",     "redis.password");
    warn_if_in_yaml(cfg.kafka.sasl.username,"JQ_KAFKA_SASL_USERNAME","kafka.sasl.username");
    warn_if_in_yaml(cfg.kafka.sasl.password,"JQ_KAFKA_SASL_PASSWORD","kafka.sasl.password");

    // -------------------------------------------------------------------------
    // Environment variable overrides — JQ_* env vars take precedence over YAML.
    //
    // Full mapping:
    //   JQ_GRPC_PORT                        -> grpc.port
    //   JQ_GRPC_TLS_ENABLED                 -> grpc.tls.enabled
    //   JQ_GRPC_TLS_CERT_PATH               -> grpc.tls.cert_path
    //   JQ_GRPC_TLS_KEY_PATH                -> grpc.tls.key_path
    //   JQ_DB_HOST                          -> db.host
    //   JQ_DB_PORT                          -> db.port
    //   JQ_DB_NAME                          -> db.name
    //   JQ_DB_USER                          -> db.user
    //   JQ_DB_PASSWORD                      -> db.password
    //   JQ_DB_POOL_SIZE                     -> db.pool_size
    //   JQ_DB_CONNECT_TIMEOUT_MS            -> db.connect_timeout_ms
    //   JQ_REDIS_ADDR                       -> redis.addr
    //   JQ_REDIS_PASSWORD                   -> redis.password
    //   JQ_REDIS_DB                         -> redis.db
    //   JQ_REDIS_CONNECT_TIMEOUT_MS         -> redis.connect_timeout_ms
    //   JQ_KAFKA_BROKERS                    -> kafka.brokers (comma-separated)
    //   JQ_KAFKA_PRODUCER_ACKS              -> kafka.producer.acks
    //   JQ_KAFKA_PRODUCER_COMPRESSION       -> kafka.producer.compression
    //   JQ_KAFKA_CONSUMER_GROUP_ID          -> kafka.consumer.group_id
    //   JQ_KAFKA_CONSUMER_AUTO_OFFSET_RESET -> kafka.consumer.auto_offset_reset
    //   JQ_KAFKA_SASL_USERNAME              -> kafka.sasl.username
    //   JQ_KAFKA_SASL_PASSWORD              -> kafka.sasl.password
    //   JQ_SCHEDULER_INTERVAL_MS            -> scheduler.interval_ms
    //   JQ_SCHEDULER_BATCH_SIZE             -> scheduler.batch_size
    //   JQ_SCHEDULER_ASSIGNMENT_TIMEOUT_S   -> scheduler.assignment_timeout_s
    //   JQ_SCHEDULER_WORKER_HB_TIMEOUT_S    -> scheduler.worker_heartbeat_timeout_s
    //   JQ_METRICS_PORT                     -> metrics.port
    //   JQ_HEALTH_PORT                      -> health.port
    //   JQ_LOG_LEVEL                        -> logging.level
    //   JQ_LOG_FORMAT                       -> logging.format
    // -------------------------------------------------------------------------

    cfg.grpc.port          = EnvInt("JQ_GRPC_PORT",         cfg.grpc.port);
    cfg.grpc.tls.enabled   = EnvBool("JQ_GRPC_TLS_ENABLED", cfg.grpc.tls.enabled);
    cfg.grpc.tls.cert_path = Env("JQ_GRPC_TLS_CERT_PATH",   cfg.grpc.tls.cert_path);
    cfg.grpc.tls.key_path  = Env("JQ_GRPC_TLS_KEY_PATH",    cfg.grpc.tls.key_path);

    cfg.db.host               = Env("JQ_DB_HOST",               cfg.db.host);
    cfg.db.port               = EnvInt("JQ_DB_PORT",            cfg.db.port);
    cfg.db.name               = Env("JQ_DB_NAME",               cfg.db.name);
    cfg.db.user               = Env("JQ_DB_USER",               cfg.db.user);
    cfg.db.password           = Env("JQ_DB_PASSWORD",           cfg.db.password);
    cfg.db.pool_size          = EnvInt("JQ_DB_POOL_SIZE",        cfg.db.pool_size);
    cfg.db.connect_timeout_ms = EnvInt("JQ_DB_CONNECT_TIMEOUT_MS", cfg.db.connect_timeout_ms);

    cfg.redis.addr               = Env("JQ_REDIS_ADDR",               cfg.redis.addr);
    cfg.redis.password           = Env("JQ_REDIS_PASSWORD",           cfg.redis.password);
    cfg.redis.db                 = EnvInt("JQ_REDIS_DB",              cfg.redis.db);
    cfg.redis.connect_timeout_ms = EnvInt("JQ_REDIS_CONNECT_TIMEOUT_MS", cfg.redis.connect_timeout_ms);

    {
        const std::string brokers = Env("JQ_KAFKA_BROKERS", "");
        if (!brokers.empty()) cfg.kafka.brokers = SplitComma(brokers);
    }
    cfg.kafka.producer.acks              = Env("JQ_KAFKA_PRODUCER_ACKS",              cfg.kafka.producer.acks);
    cfg.kafka.producer.compression       = Env("JQ_KAFKA_PRODUCER_COMPRESSION",       cfg.kafka.producer.compression);
    cfg.kafka.consumer.group_id          = Env("JQ_KAFKA_CONSUMER_GROUP_ID",          cfg.kafka.consumer.group_id);
    cfg.kafka.consumer.auto_offset_reset = Env("JQ_KAFKA_CONSUMER_AUTO_OFFSET_RESET", cfg.kafka.consumer.auto_offset_reset);
    cfg.kafka.sasl.username              = Env("JQ_KAFKA_SASL_USERNAME",              cfg.kafka.sasl.username);
    cfg.kafka.sasl.password              = Env("JQ_KAFKA_SASL_PASSWORD",              cfg.kafka.sasl.password);

    cfg.scheduler.interval_ms                = EnvInt("JQ_SCHEDULER_INTERVAL_MS",          cfg.scheduler.interval_ms);
    cfg.scheduler.batch_size                 = EnvInt("JQ_SCHEDULER_BATCH_SIZE",            cfg.scheduler.batch_size);
    cfg.scheduler.assignment_timeout_s       = EnvInt("JQ_SCHEDULER_ASSIGNMENT_TIMEOUT_S",  cfg.scheduler.assignment_timeout_s);
    cfg.scheduler.worker_heartbeat_timeout_s = EnvInt("JQ_SCHEDULER_WORKER_HB_TIMEOUT_S",   cfg.scheduler.worker_heartbeat_timeout_s);

    cfg.metrics.port = EnvInt("JQ_METRICS_PORT", cfg.metrics.port);
    cfg.health.port  = EnvInt("JQ_HEALTH_PORT",  cfg.health.port);

    cfg.logging.level  = Env("JQ_LOG_LEVEL",  cfg.logging.level);
    cfg.logging.format = Env("JQ_LOG_FORMAT", cfg.logging.format);

    return cfg;
}

// ---------------------------------------------------------------------------
// ValidateConfig
// ---------------------------------------------------------------------------

std::vector<std::string> ValidateConfig(const Config& cfg) {
    std::vector<std::string> errors;

    if (cfg.db.host.empty())
        errors.push_back("db.host is required (or JQ_DB_HOST)");
    if (cfg.db.user.empty())
        errors.push_back("db.user is required (or JQ_DB_USER)");
    if (cfg.redis.addr.empty())
        errors.push_back("redis.addr is required (or JQ_REDIS_ADDR)");
    if (cfg.kafka.brokers.empty())
        errors.push_back("kafka.brokers is required (or JQ_KAFKA_BROKERS)");
    if (cfg.grpc.port <= 0 || cfg.grpc.port > 65535)
        errors.push_back("grpc.port must be in range 1–65535");
    if (cfg.scheduler.batch_size <= 0)
        errors.push_back("scheduler.batch_size must be > 0");

    return errors;
}

}  // namespace jq
