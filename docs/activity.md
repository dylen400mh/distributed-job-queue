# Activity Log

## 2026-02-21

### Prompt
> Yeah I'd like you to start with this: [Prompt 1 — Project Scaffold & Build System]
> Scaffold the complete project structure for the distributed job queue. Create directory layout, CMakeLists.txt, toolchain file, vcpkg.json, .gitignore, placeholder main.cc files, and verify build.

### Actions
1. Read tech-stack.md, design-notes.md, requirements.md, prompts.md
2. Wrote plan to tasks/todo.md
3. Created full directory layout: proto/, src/{server,worker,ctl,common}/, db/migrations/, k8s/, docker/, cmake/, prometheus/, grafana/, docs/, tests/{unit,integration,e2e}/
4. Installed Homebrew dependencies: grpc, protobuf, libpqxx, hiredis, librdkafka, boost, abseil, spdlog, nlohmann-json, googletest, yaml-cpp, prometheus-cpp, pkg-config
5. Created cmake/toolchain-macos.cmake — points to Homebrew LLVM clang/clang++ at /opt/homebrew/opt/llvm/bin/
6. Created CMakeLists.txt — C++17, three targets (jq-server, jq-worker, jq-ctl), find_package for all deps, tests/ subdirectory
   - Fixed Boost: Boost.System is header-only in Boost 1.90; use only Boost::filesystem + Boost::headers
   - Fixed pkg-config paths: extended PKG_CONFIG_PATH to include keg-only Homebrew opt/* paths for libpq, libpqxx, hiredis, librdkafka
7. Created vcpkg.json — all C++ dependencies listed for non-macOS fallback
8. Created .gitignore — standard C++/CMake ignores
9. Created placeholder main.cc files for jq-server, jq-worker, jq-ctl (each just returns 0)
10. Created tests/CMakeLists.txt — placeholder comments for future test targets
11. Verified build: cmake configure succeeded, all three binaries compiled cleanly with Homebrew LLVM Clang 21.1.8

---

### Prompt
> [Prompt 2 — Protobuf & gRPC Service Definitions]
> Create all Protocol Buffer service and message definitions in proto/. Update CMakeLists.txt to add protobuf/gRPC code generation.

### Actions
1. Verified protoc (libprotoc 33.4) and grpc_cpp_plugin at /opt/homebrew/bin/
2. Created proto/common.proto — JobStatus enum, WorkerStatus enum, Job/Queue/Worker/JobEvent messages using google.protobuf.Timestamp
3. Created proto/job_service.proto — JobService (6 RPCs): SubmitJob, CancelJob, GetJobStatus, ListJobs, GetJobLogs, RetryJob
4. Created proto/worker_service.proto — WorkerService (5 RPCs): RegisterWorker, Heartbeat, StreamJobs (server-streaming), ReportResult, Deregister
5. Created proto/admin_service.proto — AdminService (8 RPCs): CreateQueue, DeleteQueue, ListQueues, GetQueueStats, ListWorkers, DrainWorker, ShutdownWorker, GetSystemStatus
6. Fixed: removed unused import of common.proto from worker_service.proto (all WorkerService messages use only primitive types)
7. Verified all .proto files compile cleanly with protoc (no warnings or errors)
8. Updated CMakeLists.txt: added proto codegen via add_custom_command per .proto file; created proto_gen static library; added proto_gen to COMMON_LIBS
9. Verified full cmake configure + build: proto_gen library built (8 generated .cc files), all three binaries linked successfully

---

### Prompt
> [Prompt 3 — Database Schema & Migrations]
> Create PostgreSQL migration files (Flyway naming), base Repository class with connection pool, RunMigrations() function, and unit tests.

### Actions
1. Created db/migrations/V1__create_queues_table.sql — queues table + pgcrypto extension
2. Created db/migrations/V2__create_workers_table.sql — worker_status enum + workers table + 2 indexes
3. Created db/migrations/V3__create_jobs_table.sql — job_status enum + jobs table + 4 partial indexes (scheduler, heartbeat recovery, queue stats, retry scheduling)
4. Created db/migrations/V4__create_job_events_table.sql — append-only job_events table + index on (job_id, occurred_at)
5. Created db/migrations/V5__seed_default_queue.sql — INSERT default queue with ON CONFLICT DO NOTHING
6. Created src/common/db/connection_pool.h/.cc — thread-safe pool of pqxx::connection; BorrowedConnection RAII handle auto-returns and reconnects dropped connections
7. Created src/common/db/repository.h/.cc — base Repository class exposing Conn() for subclasses
8. Created src/common/db/migrations.h/.cc — RunMigrations(): schema_migrations tracker table, reads V*.sql sorted by version, applies pending in transactions
9. Updated CMakeLists.txt: jq_db static library added to COMMON_LIBS; tests/CMakeLists.txt updated with db_unit_tests target
10. Created tests/unit/db/migration_test.cc — 3 tests (apply all 5 migrations, default queue seeded, idempotency); auto-skips if DB unavailable
11. Fixed libpqxx 7.10 deprecations: exec_params → exec with pqxx::params{}; exec1 → exec().one_row()
12. Verified: zero warnings, full clean build

---

### Prompt
> [Prompt 4 — Configuration & Logging Infrastructure]
> Build shared configuration parsing and structured logging used by all three binaries.

### Actions
1. Created src/common/config/config.h — Config struct hierarchy mirroring config.yaml layout: GrpcConfig, DbConfig, RedisConfig, KafkaConfig, SchedulerConfig, MetricsConfig, HealthConfig, LoggingConfig; sensitive_field_warnings field; LoadConfig() and ValidateConfig() declarations
2. Created src/common/config/config.cc — LoadConfig(): parses YAML via yaml-cpp, detects sensitive fields (db.password, redis.password, kafka.sasl.*) in YAML and records warnings, applies 25+ JQ_* env var overrides (env vars always win); ValidateConfig(): checks required fields (db.host, db.user, redis.addr, kafka.brokers, valid grpc.port, batch_size > 0)
3. Created src/common/config/flags.h — header-only getopt_long flag parsing; CommonFlags/ServerFlags/WorkerFlags/CtlFlags structs; ParseServerFlags/ParseWorkerFlags/ParseCtlFlags; PrintVersion/PrintServerHelp/PrintWorkerHelp/PrintCtlHelp
4. Created src/common/logging/logger.h — InitLogger(), SetCorrelationId(), GetCorrelationId(), Log(), and LOG_INFO/LOG_WARN/LOG_ERROR/LOG_DEBUG macros
5. Created src/common/logging/logger.cc — spdlog null_sink for level filtering only; JSON output built manually via nlohmann-json and written to stdout under a mutex; thread-local correlation ID; ISO-8601 UTC timestamps via gmtime_r
6. Updated src/server/main.cc — ParseServerFlags, LoadConfig, ValidateConfig, InitLogger, flag overrides (grpc-port, metrics-port, health-port), --dry-run support, structured startup log
7. Updated src/worker/main.cc — ParseWorkerFlags, LoadConfig, ValidateConfig, InitLogger, flag overrides, structured startup log
8. Updated src/ctl/main.cc — ParseCtlFlags, optional config load (ctl can run without --config), InitLogger, structured startup log
9. Added jq_config static lib to CMakeLists.txt (config.cc + yaml-cpp via pkg_check_modules)
10. Added jq_log static lib to CMakeLists.txt (logger.cc + spdlog + nlohmann_json)
11. Added both to COMMON_LIBS; added yaml-cpp pkg_check_modules call
12. Created tests/unit/config/config_test.cc — 8 tests: defaults from minimal YAML, all YAML fields loaded, env var overrides, sensitive field warning detection, no warning when password absent, ValidateConfig errors on missing user, passes with minimal valid config, throws on missing file
13. Updated tests/CMakeLists.txt: added config_unit_tests executable linking jq_config + GTest
14. Verified: cmake configure clean, full build 100% successful, all 8 config unit tests pass

---

### Prompt
> [Prompt 5 — Redis, Kafka, and Metrics Clients]
> Build the Redis and Kafka client wrapper classes in src/common/ and all FR-040 metrics.

### Actions
1. Read design-notes.md (Caching & Fast State, Event Streaming, Error Handling Conventions), tech-stack.md, requirements.md (FR-040), and existing source for context
2. Note: redis-plus-plus is not available via Homebrew; implemented using raw hiredis C API instead
3. Created src/common/metrics/metrics.h + metrics.cc — 11 FR-040 metric families (Gauge/Counter/Histogram) as static singletons; StartMetricsServer(port) starts the prometheus-cpp HTTP exposer thread
4. Created src/common/redis/redis_client.h + redis_client.cc:
   - IRedisLockable interface (SetNxPx + Del) for mock injection in tests
   - RedisClient implements IRedisLockable; wraps hiredis for SetNxPx, Del, Get, Set, Incr, Expire; auto-reconnects on connection loss; all errors caught and logged — never crashes the process
   - DistributedLock RAII: acquires lock:job:<key> on construction (throws on failure), releases via Del on destruction
   - OpTimer helper observes jq_redis_operation_duration_seconds per operation
5. Created src/common/kafka/kafka_producer.h + kafka_producer.cc:
   - KafkaTopics namespace with six topic-name constants
   - KafkaProducer wraps librdkafka; async Publish with auto-poll; Flush for graceful shutdown
   - DeliveryReportCb: DEBUG log on success; ERROR log + jq_kafka_publish_errors_total increment on failure
6. Created src/common/kafka/kafka_consumer.h + kafka_consumer.cc:
   - KafkaConsumer with Poll (returns optional<KafkaMessage>), Commit (async), Close
   - auto.commit disabled — callers control offset commits
7. Added jq_metrics, jq_redis, jq_kafka static libraries to CMakeLists.txt; all added to COMMON_LIBS
8. Created tests/unit/redis/distributed_lock_test.cc — 5 tests using gmock MockRedis: acquire+release, throw on contention, correct key prefix, Del called exactly once, default TTL 60s
9. Created tests/unit/kafka/kafka_delivery_test.cc — 4 tests: counter increments on failure, independent per-topic counters, all six topics handled, success path no phantom counters
10. Added redis_unit_tests + kafka_unit_tests targets to tests/CMakeLists.txt
11. Verified: cmake configure clean, full build 100% successful, 5/5 redis + 4/4 kafka + 8/8 config = 17/17 unit tests pass
