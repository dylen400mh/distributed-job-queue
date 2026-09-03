# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Workflow Rules

1. **Plan first:** Read the codebase for relevant files and state a plan before starting work.
2. **Verify plan:** Check in with the user before beginning implementation.
3. **Communicate simply:** Give a high-level explanation of changes at each step, and a summary of what changed at the end.
4. **Keep it simple:** Every change should impact as little code as possible. Avoid large or complex changes.
5. **Push on success:** Push changes to the git repository after every successful change.

## Project Overview

A distributed job queue system with three C++ binaries: `jq-server` (control plane), `jq-worker` (data plane), and `jq-ctl` (operator CLI). No UI — all interaction is via CLI and gRPC. Designed for at-least-once job execution with PostgreSQL as source of truth.

**Non-goals (v1):** no built-in job business logic (workers invoke subprocesses), no web UI/REST API, no workflow/DAG support (jobs are independent units), no multi-tenancy.

**Why three binaries instead of one?** Separation of concerns maps to operational roles — `jq-server` and `jq-worker` have very different scaling profiles (e.g. 1 server, 50 workers) and keeping them separate avoids coupling their deployments. `jq-ctl` is a human tool with no business being in the same process as a daemon.

**Why long-running workers instead of on-demand?** On-demand workers (spinning up a container per job) add 1–30s of cold-start latency and complicate connection pool management. Long-running workers keep warm DB/Redis connections at the cost of slightly higher idle resource usage.

**Why gRPC instead of REST?** Primary consumers are other services, not browsers — gRPC gives strongly-typed contracts via protobuf, bidirectional streaming (used for job assignment push from server to worker), and better performance than HTTP/JSON.

**Why Redis for locks instead of PostgreSQL advisory locks?** PostgreSQL advisory locks are connection-scoped and don't survive connection drops cleanly in a pooled environment (PgBouncer in transaction mode drops session state). Redis `SET NX PX` gives a clean, TTL-based distributed lock safe with connection pooling and across multiple `jq-server` replicas.

## Build System

**Language:** C++17 | **Build:** CMake ≥ 3.20 | **Compiler (macOS):** Homebrew LLVM (NOT Apple Clang)

```bash
# One-time macOS setup
brew install llvm cmake ninja grpc protobuf libpqxx hiredis \
             boost abseil spdlog nlohmann-json googletest yaml-cpp

# Configure (use Homebrew LLVM toolchain)
cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-macos.cmake -B build

# Build all targets
cmake --build build --parallel

# Start backing services
docker compose up -d
```

**Toolchain file** (`cmake/toolchain-macos.cmake`) must point CC/CXX to `/opt/homebrew/opt/llvm/bin/clang` and `clang++`.

## Running the System

```bash
./build/jq-server --config config.local.yaml
./build/jq-worker --config config.local.yaml --server-addr localhost:50051
./build/jq-ctl --server-addr localhost:50051 <command>
```

## Testing

```bash
# Unit tests
cmake --build build --target tests && ./build/tests/unit_tests

# Integration tests (requires Docker Compose services running)
make test-integration

# End-to-end tests
make test-e2e
```

## Directory Structure

```
proto/              # .proto definitions for JobService, WorkerService, AdminService
src/
  server/           # jq-server: gRPC API, scheduler loop, health endpoints
    grpc/           # Service implementations (job_service_impl, worker_service_impl, admin_service_impl)
    scheduler/      # Scheduler loop + WorkerRegistry
    health/         # /healthz and /readyz HTTP endpoints
  worker/           # jq-worker: job executor, heartbeat loop
  ctl/              # jq-ctl: command handlers per subcommand, output formatter
  common/           # Shared across all binaries
    config/         # Config struct, YAML loader, env var overrides (JQ_ prefix)
    logging/        # spdlog wrapper for structured JSON logs
    db/             # Repository base class, connection pool (libpqxx), RunMigrations()
    redis/          # RedisClient wrapper + DistributedLock RAII class
    metrics/        # prometheus-cpp metric definitions (all FR-040 metrics)
db/migrations/      # Flyway SQL migrations (V1__..., V2__..., etc.)
docker/             # Dockerfiles for jq-server and jq-worker (multi-stage Linux builds)
terraform/          # AWS infra (VPC, EC2, RDS, ElastiCache, ECR) + app host user-data
cmake/              # toolchain-macos.cmake
prometheus/         # Prometheus config and alert rules
grafana/            # Grafana dashboard JSON
docs/               # Architecture docs
```

## Architecture

**Job lifecycle:** `PENDING → ASSIGNED → RUNNING → DONE` (or `FAILED → PENDING` for retries, `FAILED → DEAD_LETTERED` when exhausted)

**Scheduler loop** (inside `jq-server`, runs every 500ms):
1. Query PostgreSQL for `PENDING` jobs ordered by `(priority DESC, created_at ASC)`
2. For each job, acquire Redis lock `lock:job:<job_id>` (Redlock, TTL = assignment_timeout)
3. If acquired: transition to `ASSIGNED`, push via `WorkerService.StreamJobs` gRPC stream
4. If not acquired: skip (another `jq-server` replica claimed it — no leader election needed)

**Worker model:** Long-running processes (not on-demand). Workers register via gRPC, receive jobs over a persistent server-streaming RPC, execute via fork/exec, and report results back.

**Communication patterns:**
- `jq-ctl → jq-server`: gRPC unary
- `jq-worker → jq-server`: gRPC unary (register/heartbeat/result) + server-streaming (receive assignments)
- `jq-server → PostgreSQL`: all durable state (source of truth), including the `job_events` audit trail
- `jq-server → Redis`: ephemeral only — distributed locks, caches, counters (Redis loss is recoverable)

**Graceful shutdown** (`SIGTERM`/`SIGINT`):
- `jq-server`: stop accepting new gRPC connections → drain in-flight RPCs (up to 30s) → stop the scheduler loop → close the DB pool → exit 0
- `jq-worker`: stop accepting new job assignments from the stream → let running jobs finish (up to 60s; jobs still running past this are left for heartbeat-timeout reclaim) → send `WorkerService.Deregister` → exit 0
- `jq-ctl`: no special handling — short-lived process

**Testing strategy:**
- Unit tests (`gtest`/`gmock`): pure logic — scheduler algorithm, retry backoff formula, proto mapping, config parsing. No external dependencies.
- Integration tests (`tests/integration`): against real PostgreSQL and Redis via Docker Compose.
- End-to-end tests (`tests/e2e`): all binaries started via Docker Compose; `jq-ctl` submits jobs and asserts outcomes.

## Key Conventions

- **Config:** YAML file via `--config`; any value overridable by `JQ_<UPPERCASED_KEY>` env var. Secrets must come from env vars only.
- **Logging:** Structured JSON via spdlog. Every log line includes `timestamp`, `level`, `service`, `message`, plus context fields like `job_id`/`worker_id`.
- **DB access:** Repository pattern only — no raw SQL outside repository classes. State transitions update `jobs` table and insert into `job_events` in a single transaction.
- **gRPC errors:** Use canonical status codes (`NOT_FOUND`, `FAILED_PRECONDITION`, `INTERNAL`, etc.). Never leak stack traces to clients.
- **Redis unavailability:** Never crashes the process. Scheduler falls back to single-instance mode; all Redis errors are logged + metriced.
- **Retry backoff:** `min(base_delay * 2^attempt, max_delay) + jitter`. Defaults: `base_delay=5s`, `max_delay=300s`.
- **Metrics:** All defined in `src/common/metrics/metrics.h` as globals; see FR-040 in `README.md`'s Requirements section for the full list.

## Proto Files

- `proto/common.proto` — `JobStatus` enum, `Job`, `Queue`, `Worker` messages
- `proto/job_service.proto` — `JobService` (submit, cancel, status, list, logs, retry)
- `proto/worker_service.proto` — `WorkerService` (register, heartbeat, stream jobs, report result, deregister)
- `proto/admin_service.proto` — `AdminService` (queue CRUD, worker drain/shutdown, system status)

Generated C++ sources are produced by `protoc` + `grpc_cpp_plugin` as part of the CMake build.

## Reference Documents

- `README.md` — Requirements summary (condensed FR/NFR), architecture diagram, quick start, deployment
- `docs/performance.md`, `docs/test-results.md` — Historical measurement records from the prior Kafka/EKS architecture
