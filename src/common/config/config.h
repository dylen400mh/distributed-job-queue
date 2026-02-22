#pragma once

#include <string>
#include <vector>

namespace jq {

// ---------------------------------------------------------------------------
// Config sub-structs — mirror the config.yaml layout from design-notes.md
// ---------------------------------------------------------------------------

struct GrpcTlsConfig {
    bool        enabled   = false;
    std::string cert_path;
    std::string key_path;
};

struct GrpcConfig {
    int          port = 50051;
    GrpcTlsConfig tls;
};

struct DbConfig {
    std::string host               = "localhost";
    int         port               = 5432;
    std::string name               = "jobqueue";
    std::string user;
    std::string password;           // Set via JQ_DB_PASSWORD — never in config file
    int         pool_size          = 10;
    int         connect_timeout_ms = 3000;
};

struct RedisConfig {
    std::string addr               = "localhost:6379";
    std::string password;           // Set via JQ_REDIS_PASSWORD
    int         db                 = 0;
    int         connect_timeout_ms = 1000;
};

struct KafkaProducerConfig {
    std::string acks        = "all";
    std::string compression = "snappy";
};

struct KafkaConsumerConfig {
    std::string group_id          = "jq-server";
    std::string auto_offset_reset = "earliest";
};

struct KafkaSaslConfig {
    std::string username;   // Set via JQ_KAFKA_SASL_USERNAME
    std::string password;   // Set via JQ_KAFKA_SASL_PASSWORD
};

struct KafkaConfig {
    std::vector<std::string> brokers = {"localhost:9092"};
    KafkaProducerConfig      producer;
    KafkaConsumerConfig      consumer;
    KafkaSaslConfig          sasl;
};

struct SchedulerConfig {
    int interval_ms                = 500;
    int batch_size                 = 100;
    int assignment_timeout_s       = 60;
    int worker_heartbeat_timeout_s = 30;
};

struct MetricsConfig {
    int port = 9090;
};

struct HealthConfig {
    int port = 8080;
};

struct LoggingConfig {
    std::string level  = "info";
    std::string format = "json";
};

// ---------------------------------------------------------------------------
// Top-level Config
// ---------------------------------------------------------------------------

struct Config {
    GrpcConfig      grpc;
    DbConfig        db;
    RedisConfig     redis;
    KafkaConfig     kafka;
    SchedulerConfig scheduler;
    MetricsConfig   metrics;
    HealthConfig    health;
    LoggingConfig   logging;

    // Populated by LoadConfig when sensitive fields are found in the YAML file.
    // Callers should log each entry as a WARN after InitLogger().
    std::vector<std::string> sensitive_field_warnings;
};

// Load config from a YAML file at path, then apply JQ_* environment variable
// overrides.  Throws std::runtime_error on parse failure.
Config LoadConfig(const std::string& path);

// Validate required fields.  Returns a list of error strings; empty = valid.
std::vector<std::string> ValidateConfig(const Config& config);

}  // namespace jq
