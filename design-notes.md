# Design Notes: Distributed Job Queue

This document captures architectural decisions, design rationale, and behavioral contracts for the distributed job queue system. It is intended as a reference for Claude Code and contributors when making implementation choices. There is no UI — all interaction is via command-line binaries, gRPC clients, and configuration files.

---

## Goals

- Provide a general-purpose background job queue that any application can submit work to via gRPC
- Guarantee at-least-once job execution with idempotency support at the application layer
- Support priority queues, retries with backoff, and dead-lettering out of the box
- Be horizontally scalable: adding more worker processes increases throughput linearly
- Be operationally observable from day one (metrics, structured logs, traces)

## Non-Goals

- No built-in job business logic — workers load job handlers as registered functions or subprocess calls
- No web UI or REST API — gRPC only for programmatic access; CLI for human operators
- No workflow/DAG support in v1 — jobs are independent units; dependencies are the caller's responsibility
- No multi-tenancy in v1 — single organization deployment

---

## Binaries

The project compiles three binaries. Each has a single, well-scoped responsibility.

### `jq-server`

The control plane. Runs as a long-lived daemon process on a dedicated host or Kubernetes Deployment.

**Responsibilities:**
- Hosts the gRPC `JobService` and `AdminService` endpoints
- Accepts job submissions from external clients and writes them to PostgreSQL
- Runs the scheduler loop: queries for pending jobs, assigns them to available workers using Redis-based distributed locks, and publishes `job.submitted` events to Kafka
- Handles job timeout detection and retry scheduling
- Exposes a Prometheus `/metrics` endpoint on a configurable secondary port (default: `9090`)
- Exposes `/healthz` and `/readyz` HTTP endpoints (default: `8080`)

**Does NOT execute job payloads.** It only manages job lifecycle state.

**CLI flags:**
```
--config <path>          Path to config file (YAML). Required.
--grpc-port <port>       gRPC listen port (default: 50051)
--metrics-port <port>    Prometheus metrics port (default: 9090)
--health-port <port>     Health check HTTP port (default: 8080)
--log-level <level>      Log level: trace|debug|info|warn|error (default: info)
--dry-run                Validate config and connectivity, then exit
```

---

### `jq-worker`

The data plane. Runs as a long-lived daemon process. Many instances run concurrently — this is the unit of horizontal scale.

**Responsibilities:**
- Registers itself with `jq-server` via the gRPC `WorkerService` on startup
- Sends periodic heartbeats to `jq-server` (default every 5 seconds)
- Polls for assigned jobs via gRPC server-streaming RPC (`WorkerService.StreamJobs`)
- Executes job payloads by dispatching to registered handler functions (linked at compile time) or by invoking a subprocess defined in the job payload
- Reports job results (success, failure, partial output) back to `jq-server`
- Respects a configurable concurrency limit: number of jobs executed in parallel per worker instance (default: `4`)
- Exposes its own Prometheus `/metrics` endpoint and health endpoints

**Worker model — long-running processes (not on-demand):** Workers stay alive between jobs. This avoids cold-start latency, maintains persistent Kafka consumer group membership, and allows connection pool reuse for Redis and Postgres. Workers are scaled horizontally via Kubernetes HPA triggered by the `job_queue_depth` metric.

**CLI flags:**
```
--config <path>           Path to config file (YAML). Required.
--server-addr <host:port> Address of jq-server gRPC endpoint. Required.
--worker-id <id>          Unique worker ID (default: hostname + PID)
--concurrency <n>         Max parallel jobs (default: 4)
--queues <q1,q2,...>      Comma-separated list of queue names to subscribe to (default: default)
--metrics-port <port>     Prometheus metrics port (default: 9091)
--health-port <port>      Health check HTTP port (default: 8081)
--log-level <level>       Log level (default: info)
```

---

### `jq-ctl`

The operator CLI. A short-lived command tool for humans to inspect and manage the system. Communicates with `jq-server` exclusively over gRPC.

**This binary is not a daemon.** It runs, executes a command, prints output to stdout, and exits.

**Commands:**

```
# Job management
jq-ctl job submit --queue <name> --payload <json|@file> [--priority <0-9>] [--max-retries <n>]
jq-ctl job status <job-id>
jq-ctl job cancel <job-id>
jq-ctl job list [--queue <name>] [--status <pending|running|done|failed>] [--limit <n>]
jq-ctl job logs <job-id>
jq-ctl job retry <job-id>

# Queue management
jq-ctl queue list
jq-ctl queue create <name> [--max-retries <n>] [--ttl <seconds>] [--priority-weights]
jq-ctl queue delete <name>
jq-ctl queue stats <name>

# Worker management
jq-ctl worker list
jq-ctl worker drain <worker-id>      # Stop sending new jobs; finish current
jq-ctl worker shutdown <worker-id>   # Graceful shutdown signal

# System
jq-ctl status                        # Overall system health summary
jq-ctl version                       # Print server and client version
```

**Global flags:**
```
--server-addr <host:port>   jq-server gRPC address (default: localhost:50051)
--tls-cert <path>           Client TLS cert (for mTLS)
--tls-key <path>            Client TLS key
--output <format>           Output format: table|json|yaml (default: table)
--timeout <seconds>         Request timeout (default: 10)
```

---

## Job Lifecycle

```
PENDING → ASSIGNED → RUNNING → DONE
                              ↘ FAILED → (retry?) → PENDING
                                                   ↘ DEAD_LETTERED
```

**State descriptions:**
- `PENDING` — Job submitted, waiting to be assigned to a worker
- `ASSIGNED` — Scheduler has claimed the job (via Redis lock) and dispatched to a worker; not yet acknowledged by worker
- `RUNNING` — Worker has acknowledged and is actively executing the job
- `DONE` — Worker reported successful completion
- `FAILED` — Worker reported failure; retry logic evaluates whether to re-enqueue
- `DEAD_LETTERED` — Exceeded `max_retries`; moved to dead-letter queue, no further automatic processing

**Heartbeat timeout:** If a worker fails to heartbeat within `worker_heartbeat_timeout` (default: 30s), the server marks its in-progress jobs as `FAILED` and re-enqueues them. This handles worker crashes without manual intervention.

**Duplicate execution:** The system guarantees **at-least-once** execution. Job handlers should be idempotent. A `job_id` is always passed to the handler for deduplication if needed.

---

## Scheduling Design

The scheduler runs as a loop inside `jq-server` on a configurable interval (default: 500ms).

1. Query PostgreSQL for `PENDING` jobs ordered by `(priority DESC, created_at ASC)` up to a configurable batch size
2. For each job, attempt to acquire a Redis distributed lock keyed on `lock:job:<job_id>` (Redlock with TTL = assignment timeout, default: 60s)
3. If lock acquired: update job status to `ASSIGNED`, publish `job.submitted` to Kafka, stream the job assignment to the target worker via gRPC
4. If lock not acquired: skip (another scheduler instance already claimed it — safe for multi-instance `jq-server` deployments)

This design means `jq-server` can be scaled to multiple replicas without a leader-election mechanism — Redis locks provide the coordination.

---

## Retry & Backoff Policy

Retries use **exponential backoff with jitter:**

```
retry_delay = min(base_delay * 2^attempt, max_delay) + random_jitter
```

Defaults: `base_delay = 5s`, `max_delay = 300s`, `max_retries = 3` (all configurable per queue).

On each retry the job is re-inserted into the queue as `PENDING` with a `not_before` timestamp respecting the backoff delay. The scheduler ignores jobs where `now < not_before`.

---

## Configuration File (YAML)

Both `jq-server` and `jq-worker` load a YAML config file. Environment variables override any config file value using the prefix `JQ_` (e.g., `JQ_DB_PASSWORD` overrides `db.password`). Secrets should always be provided via environment variables, never in the config file.

**Example `config.yaml`:**
```yaml
grpc:
  port: 50051
  tls:
    enabled: true
    cert_path: /etc/jq/tls/server.crt
    key_path: /etc/jq/tls/server.key

db:
  host: localhost
  port: 5432
  name: jobqueue
  user: jq_user
  password: ""           # Set via JQ_DB_PASSWORD env var
  pool_size: 10
  connect_timeout_ms: 3000

redis:
  addr: localhost:6379
  password: ""           # Set via JQ_REDIS_PASSWORD env var
  db: 0
  connect_timeout_ms: 1000

kafka:
  brokers:
    - localhost:9092
  producer:
    acks: all            # Wait for all in-sync replicas
    compression: snappy
  consumer:
    group_id: jq-server
    auto_offset_reset: earliest

scheduler:
  interval_ms: 500
  batch_size: 100
  assignment_timeout_s: 60
  worker_heartbeat_timeout_s: 30

metrics:
  port: 9090

health:
  port: 8080

logging:
  level: info
  format: json           # json | text
```

---

## Inter-Service Communication Patterns

- **`jq-ctl` → `jq-server`:** gRPC unary calls (request/response). Used for all operator commands.
- **`jq-worker` → `jq-server`:** gRPC unary for registration, heartbeat, and result reporting. gRPC server-streaming for receiving job assignments (`jq-server` streams jobs to the worker as they are assigned).
- **`jq-server` → Kafka:** Publishes lifecycle events for every job state transition. Downstream consumers (e.g., audit log writer, metrics aggregator) subscribe independently without coupling to `jq-server`.
- **`jq-server` → PostgreSQL:** All durable state. Single source of truth for job and worker records.
- **`jq-server` → Redis:** Ephemeral state only. Locks, caches, and counters. Redis data loss is recoverable — the scheduler will re-process jobs from PostgreSQL.

---

## Error Handling Conventions

- **gRPC errors:** Use canonical gRPC status codes (`NOT_FOUND`, `ALREADY_EXISTS`, `RESOURCE_EXHAUSTED`, `INTERNAL`, etc.). Include a human-readable message in the status detail. Never leak internal stack traces to the client.
- **Database errors:** Transient errors (connection loss, deadlock) are retried with backoff up to 3 times before propagating. Non-transient errors (constraint violation, schema mismatch) propagate immediately.
- **Kafka producer errors:** Use `librdkafka` delivery reports. On permanent failure, log the event and record it in PostgreSQL (`job_events` table) so no state transition is silently lost.
- **Redis errors:** Redis is not on the critical path for correctness. If Redis is unavailable, the scheduler falls back to a single-instance mode (skips distributed lock, still safe if only one `jq-server` is running). Log a warning and emit a metric.
- **Worker panics / unexpected exits:** Detected via heartbeat timeout. Jobs are automatically re-enqueued. Worker process should be managed by Kubernetes (restartPolicy: Always) or systemd.

---

## Graceful Shutdown

All three binaries handle `SIGTERM` and `SIGINT`:

**`jq-server` on SIGTERM:**
1. Stop accepting new gRPC connections
2. Wait for in-flight gRPC handlers to complete (up to `shutdown_grace_period`, default: 30s)
3. Stop the scheduler loop; do not assign new jobs
4. Flush the Kafka producer
5. Close database connection pool
6. Exit 0

**`jq-worker` on SIGTERM:**
1. Stop accepting new job assignments from the stream
2. Allow currently running jobs to finish (up to `shutdown_grace_period`, default: 60s). Jobs that exceed this are marked `FAILED` and re-enqueued by the server via heartbeat timeout
3. Send a final `WorkerService.Deregister` RPC to `jq-server`
4. Exit 0

**`jq-ctl`:** No special shutdown handling needed; it is a short-lived process.

---

## Testing Strategy

- **Unit tests:** Pure logic (scheduler algorithm, retry backoff, proto serialization) using `gtest`/`gmock`. No external dependencies.
- **Integration tests:** Each service tested against real PostgreSQL, Redis, and Kafka instances spun up via Docker Compose. Located in `/tests/integration`.
- **End-to-end tests:** All three binaries started together via Docker Compose; `jq-ctl` used to submit jobs and assert outcomes. Located in `/tests/e2e`.
- **Test helpers:** A `FakeWorker` gRPC stub for testing `jq-server` scheduler logic in isolation without a real `jq-worker`.

---

## Security Notes

- All gRPC communication uses mTLS in production; plaintext only in local dev
- Database credentials, Redis passwords, and Kafka SASL credentials are injected via environment variables only — never in config files committed to source control
- `jq-ctl` requires a valid client TLS certificate to connect in production environments
- Job payloads are treated as opaque bytes by the queue infrastructure — validation is the handler's responsibility
- Workers should run as non-root inside containers; use `securityContext.runAsNonRoot: true` in Kubernetes pod specs

---

## Key Design Decisions & Rationale

**Why long-running workers instead of on-demand?** On-demand workers (e.g., spinning up a container per job) add 1–30 seconds of cold-start latency per job and complicate connection pool management. Long-running workers maintain warm database and Redis connections, immediately reduce latency, and are simpler to operate at the cost of slightly higher idle resource usage. Kubernetes HPA handles scaling automatically.

**Why three binaries instead of one?** Separation of concerns maps directly to operational roles. `jq-server` and `jq-worker` have very different scaling profiles — you may run 1 server and 50 workers. Keeping them separate avoids coupling their deployments. `jq-ctl` is a human tool and has no business being in the same process as a daemon.

**Why gRPC instead of REST?** The primary consumers of this system are other services, not browsers. gRPC gives us strongly-typed contracts via protobuf, bidirectional streaming (used for job assignment push from server to worker), and better performance than HTTP/JSON. `jq-ctl` also benefits from generated client stubs rather than hand-rolled HTTP calls.

**Why Kafka for events if we already have PostgreSQL?** PostgreSQL is the source of truth for current state. Kafka is for event fans-out — multiple downstream consumers (metrics aggregators, audit log writers, notification services) can subscribe to job lifecycle events without polling the database or coupling to `jq-server` internals. If Kafka is unavailable, job processing continues; events are buffered and replayed when Kafka recovers.

**Why Redis for locks instead of PostgreSQL advisory locks?** PostgreSQL advisory locks are connection-scoped and don't survive connection drops cleanly in a pooled environment (PgBouncer in transaction mode drops session state). Redis `SET NX PX` (via Redlock) gives us a clean, TTL-based distributed lock that is safe with connection pooling and across multiple `jq-server` replicas.
