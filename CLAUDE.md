# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Workflow Rules

1. **Plan first:** Read the codebase for relevant files and write a plan to `tasks/todo.md` before starting work.
2. **Verify plan:** Check in with the user before beginning implementation.
3. **Mark progress:** Check off todo items as you complete them.
4. **Communicate simply:** Give a high-level explanation of changes at each step.
5. **Keep it simple:** Every change should impact as little code as possible. Avoid large or complex changes.
6. **Document activity:** Append all actions (including user prompts) to `docs/activity.md`. Read it when context is needed.
7. **Push on success:** Push changes to the git repository after every successful change.
8. **Review at end:** Add a review section to `todo.md` summarizing changes made.

## Project Overview

A distributed job queue system with three C++ binaries: `jq-server` (control plane), `jq-worker` (data plane), and `jq-ctl` (operator CLI). No UI — all interaction is via CLI and gRPC. Designed for at-least-once job execution with PostgreSQL as source of truth.

## Build System

**Language:** C++17 | **Build:** CMake ≥ 3.20 | **Compiler (macOS):** Homebrew LLVM (NOT Apple Clang)

```bash
# One-time macOS setup
brew install llvm cmake ninja grpc protobuf libpqxx hiredis librdkafka \
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
    kafka/          # KafkaProducer and KafkaConsumer wrappers (librdkafka)
    metrics/        # prometheus-cpp metric definitions (all FR-040 metrics)
db/migrations/      # Flyway SQL migrations (V1__..., V2__..., etc.)
docker/             # Dockerfiles for jq-server and jq-worker (multi-stage Linux builds)
k8s/                # Kubernetes manifests
cmake/              # toolchain-macos.cmake
prometheus/         # Prometheus config and alert rules
grafana/            # Grafana dashboard JSON
docs/               # Architecture docs, ADRs, activity.md
tasks/              # todo.md for current work
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
- `jq-server → Kafka`: publishes lifecycle events for every state transition
- `jq-server → PostgreSQL`: all durable state (source of truth)
- `jq-server → Redis`: ephemeral only — distributed locks, caches, counters (Redis loss is recoverable)

## Key Conventions

- **Config:** YAML file via `--config`; any value overridable by `JQ_<UPPERCASED_KEY>` env var. Secrets must come from env vars only.
- **Logging:** Structured JSON via spdlog. Every log line includes `timestamp`, `level`, `service`, `message`, plus context fields like `job_id`/`worker_id`.
- **DB access:** Repository pattern only — no raw SQL outside repository classes. State transitions update `jobs` table and insert into `job_events` in a single transaction.
- **gRPC errors:** Use canonical status codes (`NOT_FOUND`, `FAILED_PRECONDITION`, `INTERNAL`, etc.). Never leak stack traces to clients.
- **Redis unavailability:** Never crashes the process. Scheduler falls back to single-instance mode; all Redis errors are logged + metriced.
- **Kafka unavailability:** Job processing continues; failures are logged and recorded in `job_events`. Not on the critical path.
- **Retry backoff:** `min(base_delay * 2^attempt, max_delay) + jitter`. Defaults: `base_delay=5s`, `max_delay=300s`.
- **Metrics:** All defined in `src/common/metrics/metrics.h` as globals; see FR-040 in `requirements.md` for the full list.

## Proto Files

- `proto/common.proto` — `JobStatus` enum, `Job`, `Queue`, `Worker` messages
- `proto/job_service.proto` — `JobService` (submit, cancel, status, list, logs, retry)
- `proto/worker_service.proto` — `WorkerService` (register, heartbeat, stream jobs, report result, deregister)
- `proto/admin_service.proto` — `AdminService` (queue CRUD, worker drain/shutdown, system status)

Generated C++ sources are produced by `protoc` + `grpc_cpp_plugin` as part of the CMake build.

## Reference Documents

- `requirements.md` — Full functional and non-functional requirements (FR-001 through NFR-020)
- `design-notes.md` — Architectural decisions, config schema, graceful shutdown sequence, testing strategy
- `tech-stack.md` — Full dependency list, directory structure, development workflow
- `prompts.md` — Ordered build prompts (8 steps) for constructing the system from scratch
