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

---

### Prompt
> [Prompt 6 — jq-server gRPC Server & JobService]
> Implement the jq-server gRPC server and JobService. Build GrpcServer, health server, replace main.cc, write unit tests.

### Actions
1. Added `IKafkaProducer` interface to `src/common/kafka/kafka_producer.h`; `KafkaProducer` now implements it (consistent with `IRedisLockable` pattern)
2. Created `src/server/db/job_repository.h` — `JobRow`, `JobEventRow` structs + `IJobRepository` interface
3. Created `src/server/db/job_repository.cc` — concrete `JobRepository`:
   - `QueueExists`, `GetQueueMaxRetries`, `InsertJob` (job + initial job_event in one txn), `FindJobById`, `TransitionJobStatus` (update + event in one txn; sets started_at/completed_at per status), `ResetJobForRetry`, `ListJobs`, `ListJobEvents`
   - Fixed pqxx 7.10 issue: pqxx::bytes is std::basic_string<std::byte>; requires explicit cast to/from uint8_t; use txn.quote() not exec_params()
4. Created `src/server/grpc/job_service_impl.h/.cc` — all 6 RPCs with correct gRPC status codes:
   - SubmitJob: NOT_FOUND on missing queue, INTERNAL on DB error, publishes job.submitted
   - CancelJob: FAILED_PRECONDITION if not PENDING/ASSIGNED, transitions to DEAD_LETTERED, publishes job.dead-lettered
   - GetJobStatus: NOT_FOUND if missing, maps DB row → proto Job
   - ListJobs: limit+1 trick for pagination; next_page_token is base-10 offset
   - GetJobLogs: returns ordered job_events as proto JobEvent messages
   - RetryJob: FAILED_PRECONDITION if not FAILED/DEAD_LETTERED, resets to PENDING
5. Created `src/server/grpc/worker_service_impl.h` and `admin_service_impl.h` — header-only UNIMPLEMENTED stubs
6. Created `src/server/grpc/server.h/.cc` — `GrpcServer`: registers all three services; `Start()` blocks on `server->Wait()`; `Stop()` drains with 30s deadline
7. Created `src/server/health/health_server.h/.cc` — POSIX TCP accept loop in background thread; /healthz always 200; /readyz checks DB SELECT 1 + RedisClient::IsConnected()
8. Replaced `src/server/main.cc` — full startup: flags → config → logger → metrics → DB pool + Kafka + Redis → dry-run checks (FR-046) → migrations → health server → signal handlers → gRPC server (blocks) → graceful shutdown (flush Kafka)
9. Added `jq_server_db`, `jq_server_grpc`, `jq_server_health` static libs to CMakeLists.txt; added `server_unit_tests` target
10. Created `tests/unit/server/job_service_test.cc` — 15 tests across all 6 RPCs using MockJobRepository + MockKafkaProducer
11. Fixed build error: ternary with const char* cannot be + to const char[] literal; extracted to std::string locals
12. Verified: full clean build, zero errors; 15/15 server + 5/5 redis + 4/4 kafka + 8/8 config = 32/32 unit tests pass

---

### Prompt
> [Prompt 7 — Scheduler Loop, WorkerService & WorkerRegistry]
> Implement the Scheduler loop, WorkerService RPCs, and WorkerRegistry for jq-server.

### Actions
1. Extended `IJobRepository` + `JobRepository` with 5 new methods: `FetchPendingBatch`, `SetJobRetry`, `FetchExpiredTtlJobs`, `FetchJobsForWorker`, `StoreJobResult`
2. Created `src/server/db/worker_repository.h/.cc` — `WorkerRow`, `IWorkerRepository`, `WorkerRepository` (UpsertWorker via INSERT ... ON CONFLICT, FetchStaleWorkers for heartbeat timeout detection)
3. Created `src/server/scheduler/worker_registry.h/.cc` — `StreamHandle` with `std::function<bool(const JobAssignment&)>` write callback (instead of raw `grpc::ServerWriter*` which is `final`), mutex, active flag; `WorkerRegistry` with thread-safe `AssignJob` (per-worker concurrency limit, rollback on write failure), `RegisterStream`, `RemoveWorker`, `DecrementActiveCount`, `GetWorkerStats`
4. Created `src/server/scheduler/scheduler.h/.cc` — two threads: `RunLoop()` (500ms cycle: FetchPendingBatch → Redis SetNxPx lock → PENDING→ASSIGNED transition → Kafka event → WorkerRegistry::AssignJob; plus TTL expiry via FetchExpiredTtlJobs) and `HeartbeatMonitor()` (10s cycle: FetchStaleWorkers → mark OFFLINE → apply retry/dead-letter). `CalculateRetryNotBefore()` exposed as a pure free function for testability
5. Replaced `src/server/grpc/worker_service_impl.h` stub with full class header; created `worker_service_impl.cc` — all 5 RPCs: RegisterWorker, Heartbeat, StreamJobs (lambda wraps `ServerWriter::Write`; polls `handle->active`), ReportResult (validate RUNNING state, StoreJobResult, retry logic), Deregister
6. Updated `src/server/grpc/server.h/.cc` — added `WorkerRepository`, `WorkerRegistry`, `Scheduler` members; `Start()` calls `scheduler_.Start()`; `Stop()` calls `scheduler_.Stop()` first
7. Updated `CMakeLists.txt` — new `jq_server_scheduler` static lib; `worker_repository.cc` added to `jq_server_db`; `worker_service_impl.cc` added to `jq_server_grpc`; `jq_server_grpc` deps include `jq_server_scheduler`
8. Updated `tests/CMakeLists.txt` — added `scheduler_unit_tests` executable; added `absl::base/strings/synchronization` to link set
9. Updated `tests/unit/server/job_service_test.cc` — added 5 `MOCK_METHOD` entries to `MockJobRepository` for the new pure virtuals
10. Created `tests/unit/server/scheduler_test.cc` — 12 tests: 5 `BackoffTest` (formula verification) + 7 `WorkerRegistryTest` (no workers, wrong queue, matching worker, concurrency limit, decrement allows next, remove prevents assign, write failure count rollback)
11. Fixed toolchain: added `CMAKE_EXE_LINKER_FLAGS` in `cmake/toolchain-macos.cmake` pointing to `/opt/homebrew/opt/llvm/lib/c++` so `std::__1::__hash_memory` (Homebrew LLVM 21 libc++ ABI symbol required by `std::unordered_map<std::string, ...>`) resolves at link time
12. Verified: full clean build, zero errors; 12/12 scheduler + 15/15 server + 5/5 redis + 4/4 kafka + 8/8 config = 44/44 unit tests pass (db_unit_tests skipped — requires running PostgreSQL)

---

### Prompt
> [Prompt 8 — jq-worker Binary & jq-ctl Operator CLI]
> Implement the jq-worker binary and the jq-ctl operator CLI.

### Actions
1. Created `src/worker/worker.h/.cc` — `ThreadPool` (fixed-size thread pool with task queue and active count) + `Worker` class:
   - `Run()`: calls `RegisterWorker` (server may assign a UUID back), starts heartbeat thread, opens `StreamJobs` server-streaming RPC; waits on concurrency gate before dispatching each `JobAssignment` to thread pool
   - `HeartbeatLoop()`: sends `Heartbeat` every `heartbeat_interval_s`; reconnects stub after 3 consecutive failures
   - `ExecuteAndReport()`: calls `JobExecutor::Execute`, calls `ReportResult`; runs on thread pool
   - `Shutdown()`: sets `shutdown_` flag, cancels `stream_ctx_` via `TryCancel()`, notifies concurrency CV; waits up to `grace_period_s` for active jobs, then calls `Deregister`
2. Created `src/worker/job_executor.h/.cc` — `JobExecutor::Execute(job_id, payload_json, ttl_seconds)`:
   - Parses `"command"` array from payload JSON via nlohmann-json
   - `fork()`/`exec()` with `pipe()` for combined stdout+stderr capture (truncated at 1 MiB)
   - Per-job `ttl_seconds` timeout enforced with `SIGKILL`
   - Returns `ExecutionResult{success, output, error_message}`
3. Replaced `src/worker/main.cc` — flags → config → logger → metrics → health server → signal handlers → `Worker::Run()` (blocking) → `Worker::Shutdown()` on SIGTERM/SIGINT
4. Created `src/ctl/client.h/.cc` — `GrpcClient`: wraps `JobService`, `WorkerService`, `AdminService` stubs on a single channel; configurable `--timeout` deadline per call; TLS credentials support
5. Created `src/ctl/commands/commands.h` — declarations of all 13 command handler functions
6. Created `src/ctl/commands/jobs.cc` — 6 job subcommands: `submit` (--queue, --payload, --priority, --max-retries), `status`, `cancel`, `list` (--queue, --status, --limit), `logs`, `retry`
7. Created `src/ctl/commands/queues.cc` — 4 queue subcommands: `list`, `create` (--max-retries, --ttl), `delete` (--force), `stats`
8. Created `src/ctl/commands/workers.cc` — 3 worker subcommands: `list`, `drain`, `shutdown`
9. Created `src/ctl/commands/system.cc` — `system status` (calls `GetSystemStatus`, prints component health) and `version`
10. Created `src/ctl/output/formatter.h/.cc` — `Format(data, OutputFormat)`: TABLE (column-width pass + aligned output), JSON (pretty-printed via nlohmann-json), YAML (via yaml-cpp)
11. Replaced `src/ctl/main.cc` — dispatches `job|queue|worker|system|version` groups to handlers; `--help` at any level; exits non-zero on error
12. Created `src/server/grpc/admin_service_impl.cc` — all 8 AdminService RPCs: `CreateQueue`, `DeleteQueue`, `ListQueues`, `GetQueueStats`, `ListWorkers`, `DrainWorker`, `ShutdownWorker`, `GetSystemStatus` (SELECT 1 + hiredis ping for readiness)
13. Created `src/server/db/queue_repository.h/.cc` — `QueueRow`, `QueueStatsRow`, `IQueueRepository`, `QueueRepository`: `InsertQueue`, `DeleteQueue` (rejects non-empty unless force), `ListQueues`, `GetQueueStats` (aggregate counts + avg duration + error rate)
14. Updated `CMakeLists.txt` — added `jq_worker_lib` (worker.cc + job_executor.cc) and `jq_ctl_lib` (client.cc + commands + formatter); extended `jq_server_db` with queue_repository.cc
15. Created `tests/integration/e2e_test.cc` — 3 integration tests (skip automatically if `JQ_TEST_SERVER_ADDR` unset): `SubmitAndPollUntilDone`, `SubmitToNonExistentQueue_ReturnsNotFound`, `GetSystemStatus_ReturnsHealthy`
16. Updated `tests/CMakeLists.txt` — added `integration_tests` executable
17. Verified: full clean build, zero errors; 44/44 non-DB unit tests pass; integration tests skip gracefully without server

---

### Prompt
> Docker & local dev infrastructure — create Docker Compose, config.local.yaml, Makefile, Dockerfiles, and monitoring config.

### Actions
1. Created `docker-compose.yml` — five services: PostgreSQL 15 (alpine), Redis 7 (alpine), Redpanda (Kafka-compatible, single container, no Zookeeper), Prometheus (UI on host:9095), Grafana (UI on host:3000); named volumes for Postgres and Grafana data; healthchecks on all backing services
2. Created `config.local.yaml` — full config pointing at Docker Compose localhost ports: gRPC 50051, DB localhost:5432 (user=jq, db=jobqueue), Redis localhost:6379, Kafka broker localhost:19092 (Redpanda external listener); debug log level; TLS disabled
3. Created `Makefile` — five targets: `build` (cmake --build), `test-unit` (runs all five non-DB unit test binaries), `test-integration` (sets JQ_TEST_SERVER_ADDR and runs integration_tests), `test-e2e` (starts backing services, launches jq-server + jq-worker as background processes, runs integration tests, kills processes on completion), `services` / `services-down` (docker compose helpers)
4. Created `docker/Dockerfile.server` — two-stage Linux build: builder (Ubuntu 22.04 + full apt toolchain + prometheus-cpp from source) compiles jq-server; runtime ships binary + db/migrations; exposes 50051/9090/8080
5. Created `docker/Dockerfile.worker` — same builder pattern; runtime adds coreutils+bash for fork/exec payloads; runs as non-root `jq` user; exposes 9091/8081
6. Created `prometheus/prometheus.yml` — scrape_configs for jq-server (host.docker.internal:9090) and jq-worker (host.docker.internal:9091); 15s scrape interval
7. Created `grafana/provisioning/datasources/prometheus.yml` — auto-provisions Prometheus datasource so Grafana starts ready
8. Verified: `docker compose config --quiet` passes; `./build/jq-server --config config.local.yaml --dry-run` parses and validates config correctly

---

### Prompt
> Debug and fix runtime issues discovered during end-to-end testing.

### Actions
1. Fixed `jq-worker` CivetException on startup — both server and worker share the same config file and would bind the same metrics/health ports; run worker with `--metrics-port 9091 --health-port 8081` flags to avoid the conflict
2. Fixed `jq-worker` `RegisterWorker` RPC failure ("Unexpected error in RPC handling") — worker sent hostname+PID string (e.g. `MacBook-Pro.local-13380`) as `worker_id`, which server passed to PostgreSQL as `::uuid` and threw an unhandled exception. Fix: added `registration_id_` field (user-supplied UUID or empty) separate from `worker_id_` (display name); `Run()` sends `registration_id_` so an empty value lets the server generate a valid UUID
3. Fixed `jq-ctl` flag parsing failure (unrecognized option `--queue`, `--payload`) — macOS `getopt_long` permutes argv by default, moving sub-command flags (`--queue`, `--payload`) before the subcommand words (`job`, `submit`) and then rejecting them as unrecognized. Fix: added `'+'` prefix to `ParseCtlFlags` option string in `src/common/config/flags.h` (POSIX stop-at-first-non-option mode); also removed four incorrect `#ifdef __APPLE__ optreset = 1 #endif` blocks from `src/ctl/commands/jobs.cc` and `src/ctl/commands/queues.cc` that were added during an earlier (incorrect) diagnosis
4. Committed and pushed all fixes in two commits: `5f215a3` (worker UUID fix) and `c5116a4` (getopt fix)

---

### Prompt
> Job stuck in ASSIGNED state — fix so jobs complete end-to-end.

### Actions
1. Diagnosed two bugs causing jobs to permanently stick in `ASSIGNED`:
   - **Missing ASSIGNED→RUNNING transition**: `ReportResult` rejected the worker's result with `FAILED_PRECONDITION` because it checked `status != "RUNNING"` — but no code ever advanced the job to `RUNNING`; the worker goes directly from receiving the assignment to reporting the final result
   - **`worker_id` not written to the `jobs` table**: `TransitionJobStatus` only updated the `status` column, leaving `worker_id` NULL even after assignment
2. Fixed `src/server/grpc/worker_service_impl.cc` `ReportResult`: if `job->status == "ASSIGNED"`, auto-transition to `RUNNING` (sets `started_at = now()` and `worker_id`) before calling `StoreJobResult`
3. Fixed `src/server/db/job_repository.cc` `TransitionJobStatus`: when `worker_id` is non-empty, append `worker_id = <uuid>` to the UPDATE SET clause so the column is populated on the ASSIGNED→RUNNING transition
4. Rebuilt jq-server; verified job reaches `DONE` with `worker_id`, `started_at`, and `completed_at` all correctly populated
