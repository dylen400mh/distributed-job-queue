# Prompt 4 — Configuration & Logging Infrastructure

## Todo

- [x] 1. Create src/common/config/config.h — Config struct (GrpcConfig, DbConfig, RedisConfig, KafkaConfig, SchedulerConfig, MetricsConfig, HealthConfig, LoggingConfig)
- [x] 2. Create src/common/config/config.cc — LoadConfig(path) via yaml-cpp, env var overrides (JQ_* prefix), ValidateConfig(), sensitive field warning
- [x] 3. Create src/common/config/flags.h — ParseFlags with getopt_long; ServerFlags, WorkerFlags, CtlFlags structs
- [x] 4. Create src/common/logging/logger.h + logger.cc — spdlog JSON sink, InitLogger(), SetCorrelationId(), GetCorrelationId(), LOG_INFO/WARN/ERROR/DEBUG macros
- [x] 5. Update src/server/main.cc — parse flags, load+validate config, init logger, --version, --help
- [x] 6. Update src/worker/main.cc — same as server
- [x] 7. Update src/ctl/main.cc — same as server
- [x] 8. Create tests/unit/config/config_test.cc — test LoadConfig, env var overrides, sensitive field warning
- [x] 9. Update CMakeLists.txt — jq_config + jq_log static libs; config test target
- [x] 10. Verify full build compiles cleanly
- [x] 11. Append to docs/activity.md and push to git

## Review

### Summary of Changes

**New libraries:**
- `jq_config` — `config.cc` parses YAML with yaml-cpp, applies `JQ_*` env var overrides, detects credentials in YAML and produces `sensitive_field_warnings`
- `jq_log` — `logger.cc` uses an spdlog null sink for level-filtering and writes structured JSON directly to stdout under a mutex; thread-local correlation ID is included automatically

**New headers:**
- `config.h` — Config struct hierarchy with sane defaults; `LoadConfig()` + `ValidateConfig()` declarations
- `flags.h` — header-only `getopt_long` parsing for all three binaries with help/version printers
- `logger.h` — `InitLogger`, `SetCorrelationId`, `GetCorrelationId`, `Log`, and `LOG_*` macros

**Updated binaries:**
- All three `main.cc` files now parse flags, load/validate config (config optional for `jq-ctl`), init logger, emit sensitive-field warnings, log a startup message, and support `--version`/`--help`/`--dry-run` (server only)

**Tests:**
- 8 unit tests covering YAML parsing, env var overrides, sensitive-field detection, validation errors — all pass without any external services

**Build:** Full clean build, zero warnings; 8/8 config unit tests pass.
