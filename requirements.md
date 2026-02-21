# Requirements: Distributed Job Queue

This document defines the functional and non-functional requirements for the distributed job queue system. It is scoped to v1 — a general-purpose, CLI-operated job queue with no UI. All requirements are written to be verifiable by a test.

---

## 1. System Overview

The system is a distributed job queue that accepts units of work ("jobs") submitted by client applications, queues them durably, dispatches them to worker processes for execution, and reports results. All operator interaction occurs through the `jq-ctl` command-line tool. The system runs as a set of coordinated daemon processes managed by Docker/Kubernetes.

---

## 2. Functional Requirements

### 2.1 Job Submission

**FR-001** — A client shall be able to submit a job by providing a target queue name and a JSON payload over gRPC.

**FR-002** — A client may optionally specify a priority (integer 0–9, where 9 is highest) at submission time. If omitted, priority defaults to 0.

**FR-003** — A client may optionally specify a `max_retries` value (integer >= 0) per job at submission time. If omitted, the queue's default `max_retries` is used.

**FR-004** — A client may optionally specify a `ttl` (time-to-live, in seconds) per job. If a job has not started execution within its TTL window, it shall be expired and moved to `DEAD_LETTERED` status automatically.

**FR-005** — The server shall assign a globally unique `job_id` (UUID v4) to each submitted job and return it to the client in the submission response.

**FR-006** — The server shall reject job submissions to queues that do not exist, returning a gRPC `NOT_FOUND` error.

**FR-007** — Job payloads shall be treated as opaque bytes by the queue infrastructure. The server shall not parse or validate payload contents.

**FR-008** — `jq-ctl job submit` shall print the assigned `job_id` to stdout on successful submission.

---

### 2.2 Job Lifecycle & State Management

**FR-009** — Every job shall exist in exactly one of the following states at any point in time: `PENDING`, `ASSIGNED`, `RUNNING`, `DONE`, `FAILED`, `DEAD_LETTERED`.

**FR-010** — Valid state transitions are:
- `PENDING` -> `ASSIGNED` (scheduler claims the job)
- `ASSIGNED` -> `RUNNING` (worker acknowledges the job)
- `RUNNING` -> `DONE` (worker reports success)
- `RUNNING` -> `FAILED` (worker reports failure)
- `FAILED` -> `PENDING` (retry re-enqueue, if retries remain)
- `FAILED` -> `DEAD_LETTERED` (no retries remaining)
- `ASSIGNED` -> `FAILED` (assignment timeout exceeded)
- `PENDING` -> `DEAD_LETTERED` (TTL expired)

No other state transitions are permitted.

**FR-011** — All job state transitions shall be recorded as immutable rows in the `job_events` table with a timestamp and reason string.

**FR-012** — A job in `PENDING` or `ASSIGNED` state may be cancelled by an operator via `jq-ctl job cancel <job_id>`. Cancellation moves the job to `DEAD_LETTERED` with reason `CANCELLED`.

**FR-013** — A job in `DEAD_LETTERED` or `FAILED` state may be manually retried by an operator via `jq-ctl job retry <job_id>`, which re-inserts it as `PENDING` with a reset retry counter.

---

### 2.3 Job Scheduling & Dispatch

**FR-014** — The scheduler shall dispatch jobs in priority order (highest first). Within the same priority level, jobs shall be dispatched in FIFO order (earliest `created_at` first).

**FR-015** — The scheduler shall use a distributed Redis lock per job to ensure a job is dispatched to exactly one worker even when multiple `jq-server` instances are running simultaneously.

**FR-016** — If a job remains in `ASSIGNED` state beyond the configurable `assignment_timeout` (default: 60s) without the worker transitioning it to `RUNNING`, the server shall mark it `FAILED` and apply retry logic.

**FR-017** — The scheduler shall process jobs in configurable batches (default batch size: 100) per scheduling interval (default: 500ms).

---

### 2.4 Worker Management

**FR-018** — A `jq-worker` process shall register itself with `jq-server` via gRPC on startup, providing its worker ID, hostname, and configured concurrency limit.

**FR-019** — A registered worker shall send a heartbeat to `jq-server` at a configurable interval (default: 5s).

**FR-020** — If `jq-server` does not receive a heartbeat from a worker within the configurable `worker_heartbeat_timeout` (default: 30s), it shall mark the worker as `OFFLINE` and move all of its `RUNNING` or `ASSIGNED` jobs to `FAILED`, triggering retry logic.

**FR-021** — A worker shall respect its configured `concurrency` limit and shall not accept more simultaneous jobs than this value (default: 4).

**FR-022** — A worker shall subscribe only to the queues specified in its `--queues` flag. It shall not receive jobs from other queues.

**FR-023** — An operator may drain a worker via `jq-ctl worker drain <worker_id>`. A drained worker finishes its currently running jobs but accepts no new assignments. Its status shall be shown as `DRAINING`.

**FR-024** — An operator may trigger graceful shutdown of a worker via `jq-ctl worker shutdown <worker_id>`. This sends a shutdown signal; the worker finishes current jobs and then exits.

---

### 2.5 Queue Management

**FR-025** — An operator shall be able to create a named queue via `jq-ctl queue create <name>` with optional parameters: `--max-retries`, `--ttl`, `--priority-weights`.

**FR-026** — An operator shall be able to delete an empty queue via `jq-ctl queue delete <name>`. Deleting a non-empty queue shall return an error unless `--force` is passed.

**FR-027** — A `default` queue shall be created automatically on first server startup if it does not exist.

**FR-028** — `jq-ctl queue stats <name>` shall display: current depth by status (`PENDING`, `RUNNING`, `FAILED`), total jobs processed, average processing time, and error rate.

---

### 2.6 Job Result & Log Retrieval

**FR-029** — Workers shall report a result payload (JSON, may be empty) and an exit status (success or failure) to `jq-server` upon job completion. Both shall be stored in PostgreSQL.

**FR-030** — `jq-ctl job status <job_id>` shall display: job ID, queue, status, priority, payload, result, retry count, created time, started time, and completed time.

**FR-031** — `jq-ctl job logs <job_id>` shall display the ordered list of state transitions from `job_events` with timestamps and reason strings.

**FR-032** — `jq-ctl job list` shall support filtering by `--queue`, `--status`, and `--limit` (default: 20). Output shall be paginated when results exceed the limit.

---

### 2.7 Retry & Dead-Letter Behavior

**FR-033** — When a job transitions to `FAILED` and its `retry_count` is less than `max_retries`, the server shall re-enqueue it as `PENDING` after a delay calculated by exponential backoff with jitter: `min(base_delay * 2^attempt, max_delay) + jitter`. Defaults: `base_delay = 5s`, `max_delay = 300s`.

**FR-034** — When a job transitions to `FAILED` and its `retry_count` equals `max_retries`, the server shall move it to `DEAD_LETTERED` and publish a `job.dead-lettered` Kafka event.

**FR-035** — Dead-lettered jobs shall be queryable via `jq-ctl job list --status DEAD_LETTERED` and shall remain in the database indefinitely until explicitly deleted.

---

### 2.8 Event Streaming

**FR-036** — `jq-server` shall publish a Kafka event for every job state transition. Each event shall include: `job_id`, `queue`, `from_status`, `to_status`, `timestamp`, `worker_id` (if applicable), and `reason`.

**FR-037** — Kafka events shall be serialized using Protocol Buffers (reusing the shared `.proto` definitions).

**FR-038** — If Kafka is unavailable at the time of a state transition, the server shall log the failure, record the event in PostgreSQL, and continue processing. Job execution shall not be blocked by Kafka unavailability.

---

### 2.9 Observability

**FR-039** — `jq-server` and `jq-worker` shall each expose a Prometheus-compatible `/metrics` endpoint.

**FR-040** — The following metrics shall be instrumented:

| Metric | Type | Labels |
|---|---|---|
| `jq_job_queue_depth` | Gauge | `queue`, `status` |
| `jq_job_processing_duration_seconds` | Histogram | `queue` |
| `jq_job_total` | Counter | `queue`, `status` |
| `jq_worker_active_count` | Gauge | — |
| `jq_worker_job_concurrency` | Gauge | `worker_id` |
| `jq_grpc_request_duration_seconds` | Histogram | `method`, `status_code` |
| `jq_scheduler_cycle_duration_seconds` | Histogram | — |
| `jq_scheduler_jobs_assigned_total` | Counter | `queue` |
| `jq_kafka_publish_errors_total` | Counter | `topic` |
| `jq_db_query_duration_seconds` | Histogram | `query_name` |
| `jq_redis_operation_duration_seconds` | Histogram | `operation` |

**FR-041** — All log output shall be structured JSON written to stdout. Each log line shall include: `timestamp`, `level`, `service`, `message`, and any relevant context fields (e.g., `job_id`, `worker_id`, `queue`).

**FR-042** — `jq-ctl status` shall print a human-readable system health summary including: server reachability, active worker count, per-queue job depth, and Kafka/Redis/DB connectivity status.

---

### 2.10 Configuration

**FR-043** — `jq-server` and `jq-worker` shall load configuration from a YAML file specified by `--config <path>`.

**FR-044** — Any configuration value shall be overridable by an environment variable with the prefix `JQ_` followed by the uppercased key path using underscores (e.g., `db.password` becomes `JQ_DB_PASSWORD`).

**FR-045** — Sensitive values (database password, Redis password, Kafka credentials) shall never be read from the config file in production. They shall be injected via environment variables only. The server shall log a warning if sensitive values are detected in the config file.

**FR-046** — `jq-server --dry-run` shall validate all configuration values and test connectivity to PostgreSQL, Redis, and Kafka, then print a result summary and exit without starting any services.

---

### 2.11 Security

**FR-047** — All gRPC communication shall support mTLS. TLS shall be configurable (enabled/disabled) to allow plaintext in local development.

**FR-048** — `jq-ctl` shall support providing a client TLS certificate and key via `--tls-cert` and `--tls-key` flags for mTLS connections.

**FR-049** — Worker processes shall run as non-root users inside containers.

---

## 3. Non-Functional Requirements

### 3.1 Performance

**NFR-001** — `jq-server` shall sustain a job submission throughput of at least 1,000 jobs per second on a single instance under normal operating conditions.

**NFR-002** — The p99 end-to-end latency from job submission to job execution start shall be under 2 seconds under normal load (queue depth < 10,000, workers available).

**NFR-003** — The scheduler loop shall complete a full cycle (query + lock + dispatch for a batch of 100) in under 200ms at p95.

**NFR-004** — `jq-ctl` commands shall return a response within 5 seconds under normal conditions. Commands shall time out and return an error after a configurable timeout (default: 10s).

---

### 3.2 Reliability & Durability

**NFR-005** — No submitted job shall be silently lost. If a job cannot be enqueued (e.g., database write failure), the server shall return a gRPC error to the submitter so the caller can retry.

**NFR-006** — The system shall guarantee at-least-once execution. Jobs may be executed more than once in the event of a worker crash mid-execution; job handlers are expected to be idempotent.

**NFR-007** — The system shall automatically recover from worker crashes without operator intervention, via heartbeat timeout detection and job re-enqueue.

**NFR-008** — `jq-server` shall be deployable as multiple replicas simultaneously without job duplication (achieved via Redis distributed locks per FR-015).

**NFR-009** — PostgreSQL is the system of record. Redis data loss (e.g., cache flush) shall not result in permanent job loss. The scheduler shall re-discover and re-process pending jobs from PostgreSQL.

---

### 3.3 Scalability

**NFR-010** — Throughput shall scale linearly with the number of `jq-worker` instances. Doubling the worker count shall approximately double job processing throughput.

**NFR-011** — The system shall support at least 100 concurrent worker processes connected to a single `jq-server` instance.

**NFR-012** — The PostgreSQL `jobs` table shall support at least 10 million rows without degraded scheduler query performance, given proper indexing on `status`, `priority`, and `created_at`.

---

### 3.4 Availability

**NFR-013** — `jq-server` shall expose `/healthz` (liveness) and `/readyz` (readiness) HTTP endpoints for use by Kubernetes probes.

**NFR-014** — `jq-server` shall complete graceful shutdown within 30 seconds of receiving `SIGTERM`.

**NFR-015** — `jq-worker` shall complete graceful shutdown (finish running jobs, deregister) within 60 seconds of receiving `SIGTERM`. Jobs still running at the deadline shall be left to be reclaimed by heartbeat timeout.

---

### 3.5 Operability

**NFR-016** — All three binaries shall print a `--help` message describing all flags and commands.

**NFR-017** — All three binaries shall support a `--version` flag that prints the semantic version and build commit hash.

**NFR-018** — Docker images for `jq-server` and `jq-worker` shall be produced via multi-stage builds and shall be under 200MB uncompressed.

**NFR-019** — A `docker-compose.yml` shall be provided that starts the full system locally (PostgreSQL, Redis, Kafka, `jq-server`, one `jq-worker`, Prometheus, Grafana) with a single `docker compose up` command.

**NFR-020** — Database schema changes shall be managed via versioned migration files (Flyway or Liquibase) stored in `/db/migrations`. Migrations shall be applied automatically on `jq-server` startup.

---

### 3.6 Testability

**NFR-021** — Unit tests shall cover all scheduling logic, retry/backoff calculations, state transition validation, and configuration parsing. Target: >= 80% line coverage on business logic packages.

**NFR-022** — Integration tests shall verify correct behavior against real PostgreSQL, Redis, and Kafka instances via Docker Compose. They shall be runnable with a single `make test-integration` command.

**NFR-023** — End-to-end tests shall submit jobs via `jq-ctl`, allow them to execute, and assert correct final state using `jq-ctl job status`. They shall be runnable with `make test-e2e`.

---

## 4. Constraints

**C-001** — Implementation language is C++17 or C++20.

**C-002** — The production runtime target is Linux (Ubuntu 22.04 or Debian Bookworm) via Docker containers deployed on AWS. Development is performed on macOS using the native toolchain (Homebrew-managed LLVM/Clang, CMake, and dependencies). The `docker-compose.yml` provides all backing services (PostgreSQL, Redis, Kafka, Prometheus, Grafana) locally. Final production images are built via multi-stage Docker builds targeting Linux, so the macOS build is for development iteration only. Windows is not supported.

**C-003** — All infrastructure shall be deployable on AWS using the services defined in `tech-stack.md` (EC2/EKS, RDS, ElastiCache, MSK/Kafka, ECR, CloudWatch).

**C-004** — There is no UI. All human interaction is via `jq-ctl` or direct inspection of logs and metrics.

**C-005** — v1 does not support multi-tenancy, job dependencies/DAGs, or workflow orchestration.

---

## 5. Acceptance Criteria Summary

The system is considered complete for v1 when all of the following are true:

1. A client can submit a job via `jq-ctl` and observe it transition through `PENDING -> RUNNING -> DONE`
2. A worker crash is automatically detected and affected jobs are re-enqueued without operator action
3. A job that fails `max_retries` times is moved to `DEAD_LETTERED` and is visible via `jq-ctl job list --status DEAD_LETTERED`
4. Two `jq-server` instances can run simultaneously without duplicate job execution
5. `jq-ctl queue stats` returns accurate depth and throughput figures
6. Prometheus metrics are scrapeable and all metrics in FR-040 are present
7. `docker compose up` starts the full system and `jq-ctl status` reports all components healthy
8. All unit, integration, and end-to-end tests pass
