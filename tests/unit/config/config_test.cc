#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

#include "common/config/config.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Write a temporary YAML file and return its path.
static std::filesystem::path WriteTempYaml(const std::string& content) {
    auto path = std::filesystem::temp_directory_path() /
                ("jq_config_test_" + std::to_string(::getpid()) + ".yaml");
    std::ofstream f(path);
    f << content;
    return path;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST(ConfigTest, LoadsDefaultsFromMinimalYaml) {
    auto path = WriteTempYaml(
        "db:\n"
        "  user: testuser\n");

    auto cfg = jq::LoadConfig(path.string());
    std::filesystem::remove(path);

    EXPECT_EQ(cfg.db.user, "testuser");
    EXPECT_EQ(cfg.db.host, "localhost");    // struct default
    EXPECT_EQ(cfg.grpc.port, 50051);        // struct default
    EXPECT_EQ(cfg.metrics.port, 9090);      // struct default
    EXPECT_EQ(cfg.scheduler.batch_size, 100); // struct default
}

TEST(ConfigTest, LoadsAllYamlFields) {
    auto path = WriteTempYaml(
        "grpc:\n"
        "  port: 9000\n"
        "db:\n"
        "  host: dbhost\n"
        "  port: 5433\n"
        "  name: mydb\n"
        "  user: myuser\n"
        "  pool_size: 5\n"
        "redis:\n"
        "  addr: redis:6380\n"
        "logging:\n"
        "  level: debug\n"
        "  format: json\n"
        "metrics:\n"
        "  port: 9091\n");

    auto cfg = jq::LoadConfig(path.string());
    std::filesystem::remove(path);

    EXPECT_EQ(cfg.grpc.port, 9000);
    EXPECT_EQ(cfg.db.host, "dbhost");
    EXPECT_EQ(cfg.db.port, 5433);
    EXPECT_EQ(cfg.db.name, "mydb");
    EXPECT_EQ(cfg.db.user, "myuser");
    EXPECT_EQ(cfg.db.pool_size, 5);
    EXPECT_EQ(cfg.redis.addr, "redis:6380");
    EXPECT_EQ(cfg.logging.level, "debug");
    EXPECT_EQ(cfg.metrics.port, 9091);
}

TEST(ConfigTest, EnvVarOverridesYaml) {
    auto path = WriteTempYaml(
        "db:\n"
        "  host: yaml-host\n"
        "  user: yaml-user\n");

    ::setenv("JQ_DB_HOST", "env-host", 1);
    ::setenv("JQ_DB_USER", "env-user", 1);

    auto cfg = jq::LoadConfig(path.string());
    std::filesystem::remove(path);

    ::unsetenv("JQ_DB_HOST");
    ::unsetenv("JQ_DB_USER");

    EXPECT_EQ(cfg.db.host, "env-host");
    EXPECT_EQ(cfg.db.user, "env-user");
}

TEST(ConfigTest, SensitiveFieldWarningForDbPassword) {
    auto path = WriteTempYaml(
        "db:\n"
        "  user: u\n"
        "  password: secret123\n");

    auto cfg = jq::LoadConfig(path.string());
    std::filesystem::remove(path);

    ASSERT_FALSE(cfg.sensitive_field_warnings.empty());
    EXPECT_NE(cfg.sensitive_field_warnings[0].find("db.password"),
              std::string::npos);
}

TEST(ConfigTest, NoSensitiveWarningWhenPasswordAbsent) {
    auto path = WriteTempYaml(
        "db:\n"
        "  user: u\n");

    auto cfg = jq::LoadConfig(path.string());
    std::filesystem::remove(path);

    EXPECT_TRUE(cfg.sensitive_field_warnings.empty());
}

TEST(ConfigTest, ValidateConfigDetectsMissingUser) {
    jq::Config cfg;
    cfg.db.user = "";  // required field left empty

    const auto errors = jq::ValidateConfig(cfg);
    ASSERT_FALSE(errors.empty());
    bool found = false;
    for (const auto& e : errors)
        if (e.find("db.user") != std::string::npos) found = true;
    EXPECT_TRUE(found);
}

TEST(ConfigTest, ValidateConfigPassesWithMinimalValidConfig) {
    jq::Config cfg;
    cfg.db.user = "u";
    // All other fields have valid defaults (host, redis.addr, kafka.brokers, ports)

    const auto errors = jq::ValidateConfig(cfg);
    EXPECT_TRUE(errors.empty());
}

TEST(ConfigTest, ThrowsOnMissingFile) {
    EXPECT_THROW(jq::LoadConfig("/nonexistent/path/config.yaml"),
                 std::runtime_error);
}
