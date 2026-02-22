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

---

## Prompt 9 — Docker & Docker Compose

```
Read tech-stack.md and design-notes.md, specifically the Containerization section and the Development Workflow.

Create all Docker and Docker Compose files for this project.

1. docker/Dockerfile.server — Multi-stage build for jq-server:
   - Stage 1 (builder): FROM ubuntu:22.04. Install build tools (GCC 12, CMake, Ninja, all C++ dependencies via apt and source builds where needed). Copy the full source tree. Run cmake and build only the jq-server target. Output binary at /build/jq-server.
   - Stage 2 (runtime): FROM debian:bookworm-slim. Install only runtime shared libraries (libpqxx, hiredis, librdkafka, libboost, libssl). Copy the compiled binary from builder. Set a non-root USER (uid 1001). Set ENTRYPOINT ["/usr/local/bin/jq-server"]. Final image must be under 200MB.

2. docker/Dockerfile.worker — Identical structure to Dockerfile.server but builds and packages the jq-worker binary.

3. docker/Dockerfile.ctl — Builds jq-ctl. Runtime stage is even leaner — only gRPC runtime libs needed. Used for running jq-ctl inside the cluster.

4. docker-compose.yml — Full local development environment:
   Services:
   - postgres: postgres:15, port 5432, volume for data persistence, env vars for POSTGRES_USER/PASSWORD/DB matching config.local.yaml
   - redis: redis:7-alpine, port 6379
   - kafka: confluentinc/cp-kafka:7.5.0 in KRaft mode (no Zookeeper), port 9092, single-node for local dev, KAFKA_AUTO_CREATE_TOPICS_ENABLE=true
   - prometheus: prom/prometheus:latest, port 9090, mounts prometheus/prometheus.local.yml
   - grafana: grafana/grafana:latest, port 3000, mounts grafana/provisioning/, default admin/admin credentials
   - All services on a shared bridge network named jq-network
   - Health checks on postgres, redis, and kafka so dependent services wait properly
   - Do NOT include jq-server or jq-worker in docker-compose.yml — those run natively on macOS during development. Docker Compose is for backing services only.

5. docker-compose.test.yml — For integration and e2e tests:
   - Inherits from docker-compose.yml via extends
   - Adds jq-server and jq-worker services using the Docker images
   - Used by make test-integration and make test-e2e targets
   - Overrides ports to avoid conflicts with local dev compose instance

6. config.local.yaml — A local development config file (not committed with secrets):
   - Preconfigured to point at localhost ports for all Docker Compose services
   - TLS disabled
   - Log level: debug

7. .env.example — Documents all JQ_* environment variables with placeholder values and explanations. Commit this. The real .env is gitignored.

8. Update .gitignore to exclude: .env, config.local.yaml, any *.key or *.crt files

9. Add a Makefile with targets:
   - make build — cmake build for macOS native
   - make docker-build — builds all three Docker images
   - make up — docker compose up -d
   - make down — docker compose down
   - make logs — docker compose logs -f
   - make test-unit — runs unit tests natively
   - make test-integration — spins up docker-compose.test.yml, runs integration tests, tears down
   - make test-e2e — spins up docker-compose.test.yml with all services, runs e2e tests, tears down
   - make clean — removes build artifacts

Verify: docker compose up -d starts cleanly, all services pass health checks, and the native jq-server binary can connect to Postgres/Redis/Kafka running in Docker.
```

---

## Prompt 10 — Kubernetes Manifests

```
Read tech-stack.md (Orchestration section) and design-notes.md.

Create all Kubernetes manifests in the k8s/ directory. Organize by component. These manifests target production deployment on AWS EKS.

Directory structure:
k8s/
  base/
    namespace.yaml
    jq-server/
    jq-worker/
    monitoring/
  overlays/
    staging/
    production/

1. k8s/base/namespace.yaml:
   - Define namespaces: job-queue-prod, job-queue-staging

2. k8s/base/jq-server/:
   - deployment.yaml: 2 replicas, image: ECR_REGISTRY/jq-server:IMAGE_TAG (use a placeholder), resources requests/limits (500m CPU, 512Mi memory), env vars sourced from ConfigMap and Secrets, readinessProbe on /readyz (HTTP, port 8080, initialDelaySeconds: 10), livenessProbe on /healthz (HTTP, port 8080), graceful termination with terminationGracePeriodSeconds: 40 (slightly above the 30s grace period in config)
   - service.yaml: ClusterIP service exposing port 50051 (gRPC) and port 9090 (metrics)
   - configmap.yaml: non-secret config values (grpc port, scheduler settings, log level, kafka brokers, redis host)
   - hpa.yaml: HorizontalPodAutoscaler, min 1 max 5 replicas, target CPU 70%
   - pdb.yaml: PodDisruptionBudget, minAvailable: 1

3. k8s/base/jq-worker/:
   - deployment.yaml: 3 replicas initially, same structure as server, terminationGracePeriodSeconds: 70 (above worker's 60s grace period), env var WORKER_ID sourced from pod metadata using downwardAPI (fieldRef: metadata.name)
   - service.yaml: headless service (ClusterIP: None) — workers don't need load balancing, just DNS
   - configmap.yaml: worker-specific config (concurrency, queues, server address pointing to jq-server Service)
   - hpa.yaml: HorizontalPodAutoscaler, min 2 max 50 replicas. Use custom metric jq_job_queue_depth (from Prometheus via prometheus-adapter) with target value 10 (scale up when queue depth per worker exceeds 10 jobs)
   - pdb.yaml: minAvailable: 1

4. k8s/base/monitoring/:
   - servicemonitor.yaml: Prometheus ServiceMonitor resource (for prometheus-operator) scraping jq-server and jq-worker /metrics endpoints every 15s
   - prometheusrule.yaml: placeholder file — alerts will be added in Prompt 14

5. k8s/overlays/staging/ and k8s/overlays/production/:
   - kustomization.yaml using Kustomize
   - staging: 1 jq-server replica, 1 jq-worker replica, image tag: staging
   - production: 2 jq-server replicas, 3 jq-worker replicas, image tag: latest, adds resource limit increases

6. k8s/base/jq-server/ingress.yaml:
   - AWS ALB Ingress (using AWS Load Balancer Controller annotations)
   - Expose the gRPC port (50051) over HTTPS/443 with TLS termination at the ALB
   - Annotation: alb.ingress.kubernetes.io/backend-protocol-version: GRPC
   - Annotation: alb.ingress.kubernetes.io/scheme: internet-facing
   - Placeholder for certificate ARN: alb.ingress.kubernetes.io/certificate-arn: ACM_CERT_ARN

7. k8s/README.md:
   - Deployment instructions: how to apply manifests with kubectl and kustomize
   - How to update IMAGE_TAG for a new release
   - How to scale workers manually: kubectl scale deployment jq-worker --replicas=N
   - How to drain a worker before a node termination

Use imagePullPolicy: Always in production overlays. All Secrets reference external-secrets.io ExternalSecret resources pointing at AWS Secrets Manager — do not embed actual secret values in any manifest. Include comments explaining non-obvious configuration choices.
```

---

## Prompt 11 — AWS Infrastructure (Terraform)

```
Read tech-stack.md (Cloud Infrastructure section) and design-notes.md.

Create Terraform infrastructure-as-code in an infra/ directory to provision all AWS resources for this project.

Directory structure:
infra/
  modules/
    vpc/
    eks/
    rds/
    elasticache/
    msk/
    ecr/
    secrets/
    iam/
  environments/
    staging/
    production/
  variables.tf
  outputs.tf

1. infra/modules/vpc/:
   - VPC with CIDR 10.0.0.0/16
   - 3 public subnets and 3 private subnets across 3 AZs
   - NAT Gateway for private subnet egress
   - Internet Gateway for public subnets
   - Route tables for public and private subnets
   - Security groups: one for EKS nodes, one for RDS (allow only from EKS node SG), one for ElastiCache (allow only from EKS node SG), one for MSK (allow only from EKS node SG)

2. infra/modules/eks/:
   - EKS cluster (Kubernetes 1.29+) in private subnets
   - Managed node group: c5.xlarge instances, min 2 max 10 nodes, on-demand
   - Enable IRSA (IAM Roles for Service Accounts)
   - Install aws-load-balancer-controller via Helm
   - Install external-secrets-operator via Helm
   - Install prometheus-operator via Helm (kube-prometheus-stack)
   - Install prometheus-adapter for custom metrics (needed for HPA on jq_job_queue_depth)
   - Enable CloudWatch Container Insights

3. infra/modules/rds/:
   - RDS PostgreSQL 15, db.t3.medium for staging / db.r6g.large for production
   - Multi-AZ enabled for production
   - Private subnets only
   - Automated backups: 7 day retention
   - Encryption at rest with AWS KMS
   - Parameter group: max_connections=200, shared_preload_libraries=pg_stat_statements
   - Output: endpoint, port, db_name

4. infra/modules/elasticache/:
   - ElastiCache Redis 7, cache.r6g.large for production / cache.t3.micro for staging
   - Cluster mode disabled (single shard) for v1 simplicity
   - Multi-AZ with automatic failover for production
   - Encryption in transit and at rest
   - Private subnets only
   - Output: primary_endpoint

5. infra/modules/msk/:
   - Amazon MSK, kafka.m5.large brokers, 3 brokers for production / 1 for staging
   - Kafka version 3.5.x
   - Private subnets only
   - EBS storage: 100GB per broker
   - TLS encryption in transit
   - Output: bootstrap_brokers_tls

6. infra/modules/ecr/:
   - ECR repositories for: jq-server, jq-worker, jq-ctl
   - Image scanning on push enabled
   - Lifecycle policy: keep last 20 images, expire untagged images after 7 days

7. infra/modules/secrets/:
   - AWS Secrets Manager secrets for: jq/db-password, jq/redis-password, jq/kafka-credentials
   - Placeholder values — actual values set manually (see manual steps doc)
   - IAM policy that allows EKS service accounts to read these specific secrets (for external-secrets-operator)

8. infra/modules/iam/:
   - IRSA role for jq-server pods: read Secrets Manager, write CloudWatch logs
   - IRSA role for jq-worker pods: read Secrets Manager, write CloudWatch logs

9. infra/environments/staging/ and infra/environments/production/:
   - main.tf calling all modules with environment-appropriate variable values
   - terraform.tfvars with non-secret variable values
   - backend.tf configuring S3 remote state with DynamoDB locking (bucket and table names as variables)

10. infra/README.md:
    - Prerequisites: Terraform >= 1.6, AWS CLI configured, kubectl
    - How to initialize: terraform init
    - How to plan and apply per environment
    - What to do after apply (manual steps — seed secrets, run DB migrations, push first image)

Use data sources to look up the latest EKS-optimized AMI rather than hardcoding. Tag all resources with: Project=jq, Environment=<env>, ManagedBy=terraform. Do not hardcode AWS account IDs or region — use variables. Do not store any credentials or secrets in Terraform files.
```

---

## Prompt 12 — CI/CD Pipeline

```
Read tech-stack.md, design-notes.md, and the Makefile from Prompt 9.

Create a complete GitHub Actions CI/CD pipeline in .github/workflows/.

1. .github/workflows/ci.yml — Runs on every push and pull request to main:

   Jobs (run in parallel where possible):

   a. lint:
      - Checkout code
      - Install clang-format and clang-tidy via apt
      - Run clang-format --dry-run --Werror on all .cc and .h files
      - Run cmake with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON then clang-tidy on all source files

   b. unit-tests:
      - Runs on ubuntu-22.04
      - Install all C++ dependencies via apt
      - cmake build with GCC
      - Run unit tests: ctest --output-on-failure -L unit
      - Upload test results as artifact

   c. integration-tests:
      - Runs on ubuntu-22.04
      - Start Docker Compose services (postgres, redis, kafka) using docker compose -f docker-compose.test.yml up -d
      - Wait for health checks to pass (poll /healthz for up to 60s)
      - cmake build
      - Run integration tests: ctest --output-on-failure -L integration
      - docker compose down on success or failure (always)

   d. build-images:
      - Runs only on push to main (not PRs)
      - Depends on unit-tests and integration-tests passing
      - Configure AWS credentials using aws-actions/configure-aws-credentials with OIDC (no long-lived keys)
      - Login to ECR
      - Build and push jq-server, jq-worker, and jq-ctl images
      - Tag with both the git SHA and latest
      - Use docker buildx with cache-from/cache-to pointing at ECR cache for faster builds

2. .github/workflows/deploy-staging.yml — Runs automatically after ci.yml succeeds on main:
   - Configure AWS credentials (OIDC)
   - Update kubeconfig for EKS staging cluster
   - Run kubectl set image to update jq-server and jq-worker deployments to the new SHA tag
   - Wait for rollout: kubectl rollout status deployment/jq-server -n job-queue-staging --timeout=120s
   - Run smoke test: submit a test job via jq-ctl and verify it completes within 30s
   - On failure: kubectl rollout undo and post a Slack notification (webhook URL from GitHub secret)

3. .github/workflows/deploy-production.yml — Manual trigger only (workflow_dispatch):
   - Requires input: image_tag (defaults to latest)
   - Requires manual approval via GitHub Environments (configure a production environment with required reviewers)
   - Same steps as staging deploy but targets job-queue-prod namespace
   - Post success/failure notification to Slack

4. .github/workflows/pr-checks.yml — Runs on pull requests only:
   - Check that CLAUDE.md, tech-stack.md, design-notes.md, and requirements.md have not been modified without also updating docs/activity.md
   - Verify db/migrations/ files have not been modified (only new files allowed — no editing existing migrations)
   - Post a PR comment with test coverage summary if unit test results artifact is available

5. .github/dependabot.yml:
   - Enable Dependabot for GitHub Actions workflows (weekly)
   - Enable Dependabot for Docker base images (weekly)

6. .github/CODEOWNERS:
   - Require review for changes to infra/, k8s/, db/migrations/, and .github/workflows/

Store all secrets (AWS_ROLE_ARN, SLACK_WEBHOOK_URL, etc.) as GitHub repository secrets. Document their names and purpose in a comment at the top of each workflow file. Use OIDC for AWS authentication — never store AWS access keys as GitHub secrets.
```

---

## Prompt 13 — Grafana Dashboards

```
Read requirements.md section FR-040 for the full list of metrics, and tech-stack.md for the Prometheus/Grafana setup.

Create Grafana dashboard JSON definitions in grafana/dashboards/ and the Grafana provisioning config so dashboards load automatically when Grafana starts via Docker Compose or the Helm chart.

1. grafana/provisioning/datasources/prometheus.yaml:
   - Configure Prometheus as the default datasource pointing at http://prometheus:9090 for local Docker Compose
   - Use a variable for the URL so it can be overridden in production

2. grafana/provisioning/dashboards/dashboard.yaml:
   - Configure Grafana to auto-load all JSON files from grafana/dashboards/

3. grafana/dashboards/job-queue-overview.json — Main operations dashboard:
   Panels:
   - Stat: Total jobs in PENDING state (jq_job_queue_depth{status="PENDING"} summed across all queues)
   - Stat: Total jobs in RUNNING state
   - Stat: Active worker count (jq_worker_active_count)
   - Stat: Dead-lettered jobs in last 1h (increase(jq_job_total{status="DEAD_LETTERED"}[1h]))
   - Time series: Job throughput — rate(jq_job_total{status="DONE"}[5m]) and rate(jq_job_total{status="FAILED"}[5m]) on same graph
   - Time series: Queue depth over time per queue (jq_job_queue_depth, one line per queue label)
   - Time series: Job processing latency percentiles — p50, p95, p99 from jq_job_processing_duration_seconds
   - Time series: Scheduler cycle duration p95 (jq_scheduler_cycle_duration_seconds)
   - Bar gauge: Jobs assigned per queue in last 5m (rate(jq_scheduler_jobs_assigned_total[5m]))
   - Table: Per-queue breakdown — depth, throughput, error rate, avg latency
   - Time series: Kafka publish error rate (rate(jq_kafka_publish_errors_total[5m]))

4. grafana/dashboards/worker-health.json — Worker operations dashboard:
   Panels:
   - Stat: Total registered workers
   - Table: Per-worker breakdown — worker_id, active jobs, concurrency, status
   - Time series: Worker concurrency utilization per worker (jq_worker_job_concurrency / configured_concurrency)
   - Time series: gRPC request rate by method (rate(jq_grpc_request_duration_seconds_count[5m]))
   - Heatmap: gRPC request latency distribution (jq_grpc_request_duration_seconds_bucket)
   - Time series: DB query duration p95 by query name (jq_db_query_duration_seconds)
   - Time series: Redis operation duration p95 by operation (jq_redis_operation_duration_seconds)

5. grafana/dashboards/infrastructure.json — System health dashboard:
   Panels:
   - Stat panels for: DB connection pool usage, Redis memory usage, Kafka consumer lag
   - Time series: CPU and memory usage per pod (from kube-state-metrics / container metrics)
   - Time series: HPA replica count over time for jq-worker
   - Alerts panel showing any currently firing Prometheus alerts

All dashboards must:
- Have a time range selector defaulting to "Last 1 hour"
- Have a refresh interval of 30s
- Use template variables for: datasource, namespace (job-queue-prod / job-queue-staging), queue name (multi-select, all by default)
- Be tagged with "job-queue"
- Use consistent color scheme: green for DONE/healthy, yellow for warnings, red for FAILED/errors

Write valid Grafana 10.x JSON. Test that all dashboards load without errors in the local Docker Compose Grafana instance.
```

---

## Prompt 14 — Prometheus Alert Rules

```
Read requirements.md (NFR section for performance and reliability thresholds) and the metrics defined in FR-040.

Create Prometheus alerting rules in prometheus/ and wire them into both the local Docker Compose setup and the Kubernetes deployment.

1. prometheus/prometheus.local.yml — Local Docker Compose Prometheus config:
   - Scrape configs for jq-server (localhost:9090/metrics) and jq-worker (localhost:9091/metrics)
   - Load rules from prometheus/rules/*.yml
   - Alertmanager not configured locally — alerts just appear in Prometheus UI

2. prometheus/rules/job-queue.yml — Core alerting rules:

   Critical alerts (page immediately):
   - JobQueueDepthCritical: jq_job_queue_depth{status="PENDING"} > 10000 for 5m. Means workers can't keep up.
   - NoActiveWorkers: jq_worker_active_count == 0 for 2m. Complete processing outage.
   - HighDeadLetterRate: rate(jq_job_total{status="DEAD_LETTERED"}[5m]) > 10. More than 10 dead-letters/sec.
   - ServerDown: up{job="jq-server"} == 0 for 1m.

   Warning alerts (alert but don't page):
   - JobProcessingLatencyHigh: histogram_quantile(0.99, rate(jq_job_processing_duration_seconds_bucket[5m])) > 2. p99 exceeds the 2s SLA from NFR-002.
   - SchedulerCycleSlow: histogram_quantile(0.95, rate(jq_scheduler_cycle_duration_seconds_bucket[5m])) > 0.2. Scheduler exceeding 200ms p95 from NFR-003.
   - KafkaPublishErrors: rate(jq_kafka_publish_errors_total[5m]) > 0 for 5m. Any sustained Kafka errors.
   - WorkerConcurrencyHigh: jq_worker_job_concurrency / on(worker_id) jq_worker_configured_concurrency > 0.9 for 10m. Workers consistently above 90% capacity.
   - DBQuerySlow: histogram_quantile(0.95, rate(jq_db_query_duration_seconds_bucket[5m])) > 0.1 for 5m. DB queries consistently over 100ms.
   - HighRetryRate: rate(jq_job_total{status="PENDING", reason="RETRY"}[5m]) > 50. Unusually high retry churn.
   - WorkerDown: up{job="jq-worker"} == 0 for 2m. A worker instance is unreachable.

3. prometheus/rules/infrastructure.yml — Infrastructure alerts:
   - PodCrashLooping: rate(kube_pod_container_status_restarts_total{namespace=~"job-queue-.*"}[15m]) > 0 for 5m
   - PodNotReady: kube_pod_status_ready{namespace=~"job-queue-.*", condition="true"} == 0 for 5m
   - HPAMaxedOut: kube_horizontalpodautoscaler_status_current_replicas == kube_horizontalpodautoscaler_spec_max_replicas for 15m. Workers are at max scale and queue is still growing.
   - PVCAlmostFull (if any PVCs used): kubelet_volume_stats_used_bytes / kubelet_volume_stats_capacity_bytes > 0.85

4. prometheus/alertmanager.yml — Alertmanager config template (values filled from env vars or Secrets Manager in production):
   - Route critical alerts to PagerDuty (integration key from secret)
   - Route warning alerts to Slack #job-queue-alerts channel (webhook from secret)
   - Group alerts by alertname and namespace
   - Inhibit warning alerts when the corresponding critical alert is already firing (e.g. suppress HighDeadLetterRate warning if NoActiveWorkers critical is firing)
   - Repeat interval: 4h for critical, 24h for warnings

5. Update k8s/base/monitoring/prometheusrule.yaml (created as placeholder in Prompt 10):
   - Fill in with the actual PrometheusRule CRD resource containing all the rules from prometheus/rules/*.yml
   - This is picked up automatically by prometheus-operator in the EKS cluster

Each alert must have:
- summary annotation: one-line human readable description
- description annotation: what it means, likely cause, and first steps to investigate
- runbook_url annotation: placeholder URL (https://github.com/your-org/jq/wiki/runbooks/<AlertName>)
- severity label: critical or warning

After creating the rules, verify they load without syntax errors: promtool check rules prometheus/rules/*.yml
```

---