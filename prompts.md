# Claude Code Prompts: Distributed Job Queue

These are the first 8 prompts to use with Claude Code to build this application, in order. Each prompt builds on the previous one. Before starting, make sure `tech-stack.md`, `design-notes.md`, and `requirements.md` are in the root of the repository.

---

## Prompt 1 — Project Scaffold & Build System

```
Read tech-stack.md, design-notes.md, and requirements.md before doing anything.

Scaffold the complete project structure for the distributed job queue as defined in those documents. Do the following:

1. Create the full directory layout from the "Project Directory Structure" section of tech-stack.md
2. Create the root CMakeLists.txt that:
   - Sets the C++ standard to C++17
   - Points to the Homebrew LLVM compiler via cmake/toolchain-macos.cmake
   - Defines three build targets: jq-server, jq-worker, and jq-ctl
   - Links each target against its expected dependencies (grpc, libpqxx, hiredis, librdkafka, spdlog, nlohmann-json, boost, abseil, prometheus-cpp)
   - Adds a tests/ subdirectory with gtest
3. Create cmake/toolchain-macos.cmake that sets CC and CXX to the Homebrew LLVM clang and clang++ at /opt/homebrew/opt/llvm/bin/
4. Create vcpkg.json listing all C++ dependencies as a fallback for non-macOS builds
5. Create a .gitignore appropriate for a C++/CMake project
6. Create placeholder main.cc files for each of the three binaries in their respective src/ directories so the build compiles cleanly with no logic yet
7. Verify the build works: cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-macos.cmake -B build && cmake --build build

Do not write any application logic yet. The goal is a clean, compiling skeleton.
```

---

## Prompt 2 — Protobuf & gRPC Service Definitions

```
Read tech-stack.md and design-notes.md.

Create all Protocol Buffer service and message definitions for this project in the proto/ directory. Based on the three services defined in design-notes.md, create the following .proto files:

1. proto/common.proto — Shared enums and messages:
   - JobStatus enum: PENDING, ASSIGNED, RUNNING, DONE, FAILED, DEAD_LETTERED
   - Job message: job_id, queue_name, payload (bytes), priority (int32 0-9), status, max_retries, retry_count, created_at, started_at, completed_at, worker_id, result, not_before (timestamp)
   - Queue message: name, max_retries, ttl_seconds, created_at
   - Worker message: worker_id, hostname, status (ONLINE, OFFLINE, DRAINING), concurrency, active_job_count, last_heartbeat

2. proto/job_service.proto — JobService with these RPCs:
   - SubmitJob(SubmitJobRequest) returns (SubmitJobResponse)
   - CancelJob(CancelJobRequest) returns (CancelJobResponse)
   - GetJobStatus(GetJobStatusRequest) returns (GetJobStatusResponse)
   - ListJobs(ListJobsRequest) returns (ListJobsResponse)
   - GetJobLogs(GetJobLogsRequest) returns (GetJobLogsResponse)
   - RetryJob(RetryJobRequest) returns (RetryJobResponse)

3. proto/worker_service.proto — WorkerService with these RPCs:
   - RegisterWorker(RegisterWorkerRequest) returns (RegisterWorkerResponse)
   - Heartbeat(HeartbeatRequest) returns (HeartbeatResponse)
   - StreamJobs(StreamJobsRequest) returns (stream JobAssignment) — server-streaming
   - ReportResult(ReportResultRequest) returns (ReportResultResponse)
   - Deregister(DeregisterRequest) returns (DeregisterResponse)

4. proto/admin_service.proto — AdminService with these RPCs:
   - CreateQueue(CreateQueueRequest) returns (CreateQueueResponse)
   - DeleteQueue(DeleteQueueRequest) returns (DeleteQueueResponse)
   - ListQueues(ListQueuesRequest) returns (ListQueuesResponse)
   - GetQueueStats(GetQueueStatsRequest) returns (GetQueueStatsResponse)
   - ListWorkers(ListWorkerRequest) returns (ListWorkersResponse)
   - DrainWorker(DrainWorkerRequest) returns (DrainWorkerResponse)
   - ShutdownWorker(ShutdownWorkerRequest) returns (ShutdownWorkerResponse)
   - GetSystemStatus(GetSystemStatusRequest) returns (GetSystemStatusResponse)

5. Update CMakeLists.txt to add a protobuf/gRPC code generation step that runs protoc and grpc_cpp_plugin on all .proto files and makes the generated sources available to all three build targets

Use proto3 syntax. All request/response messages should be defined in the same file as their service. Import common.proto where needed. Compile and verify there are no protoc errors.
```

---

## Prompt 3 — Database Schema & Migrations

```
Read tech-stack.md, design-notes.md, and requirements.md.

Create the complete PostgreSQL database schema for this project as versioned migration files in db/migrations/. Use Flyway naming convention: V{version}__{description}.sql

Create the following migration files:

V1__create_queues_table.sql
- queues: name (PK), max_retries (default 3), ttl_seconds (nullable), created_at, updated_at

V2__create_workers_table.sql
- workers: worker_id (PK, UUID), hostname, status (ONLINE/OFFLINE/DRAINING enum), concurrency, active_job_count, last_heartbeat, registered_at, updated_at

V3__create_jobs_table.sql
- jobs: job_id (PK, UUID), queue_name (FK -> queues), payload (bytea), status (JobStatus enum matching proto), priority (smallint 0-9, default 0), max_retries (smallint), retry_count (smallint default 0), worker_id (FK -> workers, nullable), result (bytea, nullable), error_message (text, nullable), not_before (timestamptz, nullable), created_at (timestamptz), started_at (timestamptz, nullable), completed_at (timestamptz, nullable), updated_at (timestamptz)
- Add indexes on: (status, priority DESC, created_at ASC) for the scheduler query, (worker_id) for heartbeat timeout recovery, (queue_name, status) for queue stats, (status, not_before) for retry scheduling

V4__create_job_events_table.sql
- job_events: id (bigserial PK), job_id (UUID, FK -> jobs), from_status (nullable), to_status (not null), worker_id (nullable), reason (text), occurred_at (timestamptz default now())
- This is append-only. Add index on job_id for log retrieval.

V5__seed_default_queue.sql
- Insert the 'default' queue with max_retries=3, ttl_seconds=NULL

Also create:
- A src/common/db/repository.h and repository.cc defining a base Repository class and a connection pool wrapper around libpqxx
- A src/common/db/migrations.cc with a function RunMigrations() that applies all pending Flyway migrations from db/migrations/ on startup using libpqxx
- Unit tests in tests/unit/db/ that verify the migration runs cleanly against a test database

Use proper PostgreSQL types. All timestamps are timestamptz. Use UUIDs for job_id and worker_id (pgcrypto gen_random_uuid()). Define the JobStatus and WorkerStatus as PostgreSQL enums to match the proto definitions.
```

---

## Prompt 4 — Configuration & Logging Infrastructure

```
Read tech-stack.md and design-notes.md, specifically the Configuration File and Development Conventions sections.

Build the shared configuration and logging infrastructure used by all three binaries. Create everything in src/common/.

1. src/common/config/config.h and config.cc:
   - Define a Config struct that mirrors the full config.yaml structure from design-notes.md: grpc, db, redis, kafka, scheduler, metrics, health, logging sections
   - Implement LoadConfig(path string) -> Config that parses a YAML file using a suitable C++ YAML library (use yaml-cpp via Homebrew: brew install yaml-cpp)
   - Implement environment variable override logic: for every config field, check for a corresponding JQ_* env var (e.g. JQ_DB_PASSWORD overrides db.password). Document the full mapping in a comment.
   - Implement ValidateConfig(config) that returns errors for missing required fields
   - If a sensitive field (db.password, redis.password, kafka credentials) is set in the config file rather than via env var, log a WARNING

2. src/common/config/flags.h:
   - Define a ParseFlags(argc, argv) function using a simple flag parsing approach (getopt_long or a lightweight library)
   - All three binaries share common flags: --config, --log-level, --version, --help
   - jq-server additional flags: --grpc-port, --metrics-port, --health-port, --dry-run
   - jq-worker additional flags: --server-addr, --worker-id, --concurrency, --queues, --metrics-port, --health-port
   - jq-ctl additional flags: --server-addr, --tls-cert, --tls-key, --output (table|json|yaml), --timeout

3. src/common/logging/logger.h and logger.cc:
   - Wrap spdlog to output structured JSON logs to stdout
   - Each log line must include: timestamp (ISO8601), level, service (set once at startup: "jq-server" | "jq-worker" | "jq-ctl"), message, and any extra context fields passed as key-value pairs
   - Provide a global InitLogger(service_name, log_level) function called at binary startup
   - Provide convenience macros: LOG_INFO, LOG_WARN, LOG_ERROR, LOG_DEBUG that accept a message and optional structured fields
   - Correlation ID support: provide SetCorrelationId(id) and GetCorrelationId() using thread-local storage so that the correlation ID is automatically included in all log lines on that thread

4. Update each binary's main.cc to:
   - Parse flags
   - Load and validate config
   - Initialize logger
   - Print version string when --version is passed
   - Print --help and exit when --help is passed

Write unit tests for config loading, env var overrides, and the sensitive field warning.
```

---

## Prompt 5 — Redis & Kafka Client Wrappers

```
Read tech-stack.md and design-notes.md, specifically the Caching & Fast State, Event Streaming, and Error Handling Conventions sections.

Build the Redis and Kafka client wrapper classes in src/common/. These are shared by jq-server and jq-worker.

1. src/common/redis/redis_client.h and redis_client.cc:
   Wrap redis-plus-plus to provide:
   - RedisClient(config) constructor that connects using config from the Config struct
   - SetNxPx(key, value, ttl_ms) -> bool  — atomic SET if not exists with TTL; returns true if lock acquired
   - Del(key) -> void
   - Get(key) -> optional<string>
   - Set(key, value, ttl_ms) -> void
   - Incr(key) -> int64
   - Expire(key, ttl_ms) -> void
   - A higher-level DistributedLock class using RAII: acquires the lock on construction (SetNxPx), releases on destruction (Del). Constructor throws if lock cannot be acquired. Used by the scheduler to lock individual jobs.
   - If Redis is unavailable, methods should catch exceptions, log a warning with LOG_WARN, and return sensible defaults (SetNxPx returns false, Get returns nullopt). Redis unavailability must never crash the process.
   - Instrument all operations with a jq_redis_operation_duration_seconds histogram using prometheus-cpp

2. src/common/kafka/kafka_producer.h and kafka_producer.cc:
   Wrap librdkafka C++ API to provide:
   - KafkaProducer(config) constructor
   - Publish(topic, key, payload_bytes) -> void — async publish with delivery report callback
   - Flush(timeout_ms) -> void — used during graceful shutdown to drain the producer queue
   - The delivery report callback must: on success, log at DEBUG level; on failure, log at ERROR level with topic/key/error and record the failure in a jq_kafka_publish_errors_total counter
   - Handle all six Kafka topics defined in design-notes.md: job.submitted, job.started, job.completed, job.failed, job.dead-lettered, worker.heartbeat
   - Provide topic name constants in a KafkaTopics namespace

3. src/common/kafka/kafka_consumer.h and kafka_consumer.cc:
   - KafkaConsumer(config, group_id, topics) constructor
   - Poll(timeout_ms) -> optional<Message> — non-blocking poll
   - Commit(message) -> void
   - Close() -> void

4. src/common/metrics/metrics.h and metrics.cc:
   - Initialize the prometheus-cpp exposer on a given port
   - Define and register all metrics from FR-040 in requirements.md as global singletons
   - Provide a Metrics struct with named fields for each metric so other code can reference them as Metrics::job_queue_depth.Set(...) etc.
   - StartMetricsServer(port) -> void — starts the HTTP /metrics endpoint

Write unit tests for the DistributedLock RAII behavior using a mock Redis client, and for the Kafka delivery report failure path.
```

---

## Prompt 6 — jq-server: Core gRPC Server & JobService

```
Read tech-stack.md, design-notes.md, and requirements.md sections 2.1, 2.2, and 2.6.

Implement the jq-server binary's gRPC server and the JobService implementation. Build on the generated proto stubs, config, logging, db repository, and metrics infrastructure from previous steps.

1. src/server/grpc/job_service_impl.h and job_service_impl.cc:
   Implement all JobService RPCs from proto/job_service.proto:

   - SubmitJob: validate queue exists (return NOT_FOUND if not), generate UUID v4 job_id, insert into jobs table with status=PENDING, insert a job_event row (NULL -> PENDING), publish job.submitted Kafka event, return job_id. Return INTERNAL on DB failure.
   - CancelJob: look up job, return NOT_FOUND if missing. If status is not PENDING or ASSIGNED, return FAILED_PRECONDITION. Transition to DEAD_LETTERED with reason CANCELLED, insert job_event row, publish job.dead-lettered Kafka event.
   - GetJobStatus: query jobs table by job_id, map to proto Job message, return. Return NOT_FOUND if missing.
   - ListJobs: query with optional filters (queue_name, status), ORDER BY created_at DESC, apply limit (default 20, max 100). Return paginated results.
   - GetJobLogs: query job_events by job_id ORDER BY occurred_at ASC, return list of events.
   - RetryJob: look up job, return NOT_FOUND if missing. If status is not FAILED or DEAD_LETTERED, return FAILED_PRECONDITION. Reset retry_count to 0, set status to PENDING, set not_before to now, insert job_event row (old_status -> PENDING, reason MANUAL_RETRY).

2. src/server/grpc/server.h and server.cc:
   - GrpcServer class that owns the gRPC ServerBuilder, registers all service implementations, and starts listening on the configured port
   - StartServer(config) -> void
   - StopServer() -> void — drains in-flight RPCs, then shuts down. Called on SIGTERM.
   - Register all three services: JobServiceImpl, WorkerServiceImpl (stub for now), AdminServiceImpl (stub for now)

3. src/server/health/health_server.h and health_server.cc:
   - Simple HTTP server (use Boost.Beast or cpp-httplib) on the health port
   - GET /healthz — always returns 200 OK with body "ok"
   - GET /readyz — returns 200 if DB, Redis, and Kafka connections are healthy; 503 otherwise. Checks connectivity by doing a lightweight ping to each.

4. src/server/main.cc (replace placeholder):
   - Parse flags and load config
   - Initialize logger, metrics server, health server
   - Run DB migrations via RunMigrations()
   - If --dry-run: test connectivity to DB, Redis, Kafka, print results, exit
   - Register SIGTERM/SIGINT handlers that call StopServer() and begin graceful shutdown sequence from design-notes.md
   - Start gRPC server (blocks until shutdown)

Use the repository pattern for all DB access — no raw SQL in service implementations. All state transitions must write a row to job_events atomically with the jobs table update (use a DB transaction). Use proper gRPC status codes per design-notes.md error handling conventions.

Write unit tests for each JobService RPC using a mock repository and mock Kafka producer.
```

---

## Prompt 7 — jq-server: Scheduler & WorkerService

```
Read design-notes.md sections: Scheduling Design, Retry & Backoff Policy, Job Lifecycle, and the WorkerService definition.

Implement the scheduler loop and WorkerService in jq-server.

1. src/server/scheduler/scheduler.h and scheduler.cc:
   Implement the scheduling loop exactly as described in design-notes.md:

   - Scheduler class with Start() and Stop() methods. Runs on a dedicated thread.
   - The loop runs every scheduler.interval_ms (default 500ms)
   - Each cycle:
     a. Query PostgreSQL for up to batch_size PENDING jobs WHERE not_before <= now() ORDER BY priority DESC, created_at ASC
     b. For each job: attempt to acquire Redis distributed lock keyed "lock:job:{job_id}" with TTL = assignment_timeout_s * 1000
     c. If lock acquired: update job status to ASSIGNED in a DB transaction, insert job_event row (PENDING -> ASSIGNED), publish job.submitted Kafka event, call WorkerRegistry::AssignJob(job) to stream the job to an available worker
     d. If lock not acquired: skip (another jq-server instance claimed it)
   - Record jq_scheduler_cycle_duration_seconds histogram on each cycle
   - Record jq_scheduler_jobs_assigned_total counter per successful assignment

   - Heartbeat timeout monitor: a second goroutine/thread runs every 10s. Query workers WHERE status=ONLINE AND last_heartbeat < now() - worker_heartbeat_timeout_s. For each stale worker: set status=OFFLINE, find all ASSIGNED or RUNNING jobs for that worker, set each to FAILED, apply retry logic (see below), insert job_event rows.

   - Retry logic (call this from both the heartbeat monitor and WorkerService.ReportResult):
     - If job.retry_count < job.max_retries: increment retry_count, calculate not_before using exponential backoff with jitter (min(base_delay * 2^retry_count, max_delay) + random jitter), set status=PENDING, insert job_event (FAILED -> PENDING, reason RETRY)
     - If job.retry_count >= job.max_retries: set status=DEAD_LETTERED, insert job_event (FAILED -> DEAD_LETTERED), publish job.dead-lettered Kafka event

   - TTL expiry: query PENDING jobs WHERE ttl_seconds IS NOT NULL AND created_at + ttl_seconds * interval '1 second' < now(). Set to DEAD_LETTERED with reason TTL_EXPIRED.

2. src/server/grpc/worker_service_impl.h and worker_service_impl.cc:
   Implement all WorkerService RPCs:

   - RegisterWorker: insert or upsert worker row (status=ONLINE, last_heartbeat=now()), store the active gRPC stream context in WorkerRegistry for job assignment streaming
   - Heartbeat: update workers.last_heartbeat=now() for the given worker_id. Return NOT_FOUND if worker unknown.
   - StreamJobs: keep the server-streaming RPC open. Register the stream in WorkerRegistry so the scheduler can push JobAssignment messages to this worker. Block until the worker disconnects or is shut down.
   - ReportResult: look up job (NOT_FOUND if missing), validate status is RUNNING (FAILED_PRECONDITION otherwise). If success: set status=DONE, store result, insert job_event (RUNNING -> DONE), publish job.completed Kafka event. If failure: set status=FAILED, store error_message, apply retry logic.
   - Deregister: set worker status=OFFLINE, close the StreamJobs stream for this worker.

3. src/server/scheduler/worker_registry.h and worker_registry.cc:
   - Thread-safe registry of active workers and their StreamJobs gRPC streams
   - AssignJob(job) -> bool: select an available worker (one whose active_job_count < concurrency and whose queues include the job's queue), write a JobAssignment message to its stream, increment active_job_count. Return false if no worker available (job stays ASSIGNED, will be retried next scheduler cycle via assignment timeout).
   - RemoveWorker(worker_id): called on disconnect
   - GetWorkerStats(): returns list of WorkerInfo for AdminService

Write unit tests for: the scheduler batch query, retry backoff calculation (verify the formula), heartbeat timeout detection, and WorkerRegistry.AssignJob worker selection logic.
```

---

## Prompt 8 — jq-worker Binary & jq-ctl Operator CLI

```
Read tech-stack.md, design-notes.md (Binaries section for jq-worker and jq-ctl), and requirements.md sections 2.4 and the jq-ctl command list.

Implement the jq-worker binary and the jq-ctl operator CLI.

--- jq-worker ---

1. src/worker/worker.h and worker.cc:
   - Worker class that on startup:
     a. Connects to jq-server via gRPC (address from --server-addr)
     b. Calls WorkerService.RegisterWorker with its worker_id, hostname, concurrency, and subscribed queues
     c. Opens the WorkerService.StreamJobs server-streaming RPC and begins receiving JobAssignment messages
     d. For each received job: if active_job_count < concurrency, dispatch to a thread pool; otherwise send back a temporary NACK (hold the assignment until a slot opens)
   - A thread pool with --concurrency threads for parallel job execution
   - Heartbeat loop: every heartbeat_interval_s seconds, call WorkerService.Heartbeat. If the call fails 3 consecutive times, log ERROR and attempt reconnect.
   - On SIGTERM: stop accepting new jobs from the stream, wait for active jobs to finish (up to shutdown_grace_period), call WorkerService.Deregister, exit 0.

2. src/worker/job_executor.h and job_executor.cc:
   - JobExecutor::Execute(job) -> ExecutionResult
   - For v1, execute job payloads as subprocesses: parse the payload JSON for a "command" field (array of strings), run it via fork/exec, capture stdout/stderr, wait for exit.
   - Set a per-job execution timeout from the job's TTL (if present)
   - Return ExecutionResult{success, output_bytes, error_message}
   - After execution: call WorkerService.ReportResult with the outcome

3. src/worker/main.cc (replace placeholder):
   - Parse flags, load config, init logger, init metrics
   - Start health server on --health-port
   - Instantiate Worker and call worker.Run() (blocks)
   - Handle SIGTERM for graceful shutdown

--- jq-ctl ---

4. src/ctl/client.h and client.cc:
   - GrpcClient class that connects to jq-server (from --server-addr)
   - Wraps all three generated stubs (JobService, WorkerService, AdminService) with a configurable deadline (--timeout flag)
   - Handles gRPC errors: print a human-readable error to stderr and exit non-zero

5. src/ctl/commands/ — one file per command group:
   - jobs.cc: submit, status, cancel, list, logs, retry — each maps flags to a gRPC call and formats the response
   - queues.cc: list, create, delete, stats
   - workers.cc: list, drain, shutdown
   - system.cc: status (calls GetSystemStatus, prints health of all components), version

6. src/ctl/output/formatter.h and formatter.cc:
   - Format(data, OutputFormat) -> string
   - OutputFormat::TABLE: tabular output with aligned columns using a simple table renderer
   - OutputFormat::JSON: pretty-printed JSON via nlohmann-json
   - OutputFormat::YAML: YAML output

7. src/ctl/main.cc (replace placeholder):
   - Parse the command tree (e.g. "job submit", "queue list", "worker drain") and dispatch to the correct command handler
   - --help at any level prints usage for that subcommand
   - All commands exit 0 on success, non-zero on error

Write integration tests in tests/integration/ that:
- Start jq-server against a real test DB/Redis/Kafka (via Docker Compose)
- Use jq-ctl to submit a job, verify it appears as PENDING
- Start jq-worker, verify the job transitions to RUNNING then DONE
- Verify jq-ctl job status returns DONE with a result
```
